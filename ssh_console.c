/*
 * @file ssh_console.c
 * @brief Run application commands over an encrypted Ethernet connection.
 * @author Ajay Varghese
 * @date 2026-11-26
*/

/* Includes */

#include "ssh_console.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "wolfssh/ssh.h"
#include "wolfssl/wolfcrypt/asn_public.h"
#include "wolfssl/wolfcrypt/ecc.h"

/* Console settings and saved key */

enum
{
    HOST_KEY_CAPACITY = 192,
    COMMAND_CAPACITY = 256,
    REPLY_CAPACITY = 1536,
    LOGIN_TIMEOUT_SECONDS = 15,
    IDLE_TIMEOUT_SECONDS = 120,
};

typedef struct
{
    char text[REPLY_CAPACITY];
    size_t used;
    bool terminal;
} ssh_reply_t;

static const char *TAG = "ssh_console";
static ssh_console_config_t console_config;
static WOLFSSH_CTX *server;
static int listener;
static bool start_requested;

/* Identity and authentication */

/*
 * @brief Fill a buffer with hardware random bytes for wolfCrypt.
 * @param output Buffer to fill.
 * @param size Number of bytes to write.
 * @return 0 on success.
*/
int ssh_random_block(unsigned char *output, unsigned int size)
{
    // Use the board's random-number generator.
    esp_fill_random(output, size);
    // Tell wolfCrypt that the whole buffer was filled.
    return 0;
}

/*
 * @brief Generate a private SSH host key for this board.
 * @param output Buffer to receive the encoded key.
 * @param capacity Number of bytes available in the buffer.
 * @param length Receives the number of bytes used by the key.
 * @return ESP_OK on success or ESP_FAIL if the key could not be made.
*/
static esp_err_t generate_host_key(uint8_t *output, size_t capacity, size_t *length)
{
    // Set up the random generator wolfCrypt uses to make the key.
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0)
    {
        // A host key cannot be made without random data.
        return ESP_FAIL;
    }
    // Prepare a key that OpenSSH can use to identify the board.
    ecc_key key;
    int size = wc_ecc_init(&key);
    if (size == 0)
    {
        // Make a 256-bit key, then write it in the format wolfSSH reads.
        size = wc_ecc_make_key_ex(&rng, 32, &key, ECC_SECP256R1);
        if (size == 0)
        {
            // Keep just the bytes needed to load the key on the next boot.
            size = wc_EccKeyToDer(&key, output, (word32)capacity);
        }
        // Release the working copy of the key.
        wc_ecc_free(&key);
    }
    // Release the random generator after making the key.
    wc_FreeRng(&rng);
    // Reject an empty or failed key.
    if (size <= 0)
    {
        return ESP_FAIL;
    }
    // Remember how many bytes of the saved key to load.
    *length = (size_t)size;
    return ESP_OK;
}

/*
 * @brief Load the SSH host key or create it the first time the board starts.
 * @param key Buffer to receive the saved key.
 * @param size Number of bytes available, updated to the size of the saved key.
 * @return ESP_OK on success or a flash or key generation error.
*/
static esp_err_t load_host_key(uint8_t *key, size_t *size)
{
    // Keep the SSH host key in its own part of the saved settings.
    nvs_handle_t store;
    esp_err_t err = nvs_open("ssh_console", NVS_READWRITE, &store);
    if (err != ESP_OK)
    {
        // Keep SSH closed if the stored identity cannot be read safely.
        return err;
    }
    // Read the key that clients remember between reboots.
    err = nvs_get_blob(store, "host_key", key, size);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // Create the record once on a board with no SSH settings.
        err = generate_host_key(key, *size, size);
        if (err == ESP_OK)
        {
            // Store the same key for every later boot.
            err = nvs_set_blob(store, "host_key", key, *size);
        }
        if (err == ESP_OK)
        {
            // Finish saving before accepting a connection.
            err = nvs_commit(store);
        }
    }
    // Close the storage connection after reading or writing.
    nvs_close(store);
    // Reject an empty key before loading it into the server.
    if (err == ESP_OK && *size == 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

/*
 * @brief Tell the client that the board uses password authentication.
 * @param ssh Connection being set up.
 * @param context Unused callback data.
 * @return The password authentication flag.
*/
static int auth_types(WOLFSSH *ssh, void *context)
{
    // This option is the same for every connection.
    (void)ssh;
    (void)context;
    return WOLFSSH_USERAUTH_PASSWORD;
}

/*
 * @brief Compare a login attempt with the configured account.
 * @param type Login method requested by the client.
 * @param auth Login name and password sent by the client.
 * @param context Unused callback data.
 * @return The wolfSSH result for this login attempt.
*/
static int check_login(byte type, WS_UserAuthData *auth, void *context)
{
    // This console only accepts the board's password and login name.
    (void)context;
    const size_t username_size = strlen(console_config.username);
    const size_t password_size = strlen(console_config.password);
    if (type != WOLFSSH_USERAUTH_PASSWORD || auth->usernameSz != username_size
            || memcmp(auth->username, console_config.username, username_size) != 0
            || auth->sf.password.passwordSz != password_size)
    {
        return WOLFSSH_USERAUTH_FAILURE;
    }
    // Read every password character before deciding whether they match.
    unsigned int difference = 0;
    for (size_t i = 0; i < password_size; ++i)
    {
        difference |= (byte)console_config.password[i] ^ auth->sf.password.password[i];
    }
    return difference == 0 ? WOLFSSH_USERAUTH_SUCCESS : WOLFSSH_USERAUTH_FAILURE;
}

/* Command replies and terminal */

/*
 * @brief Send a complete reply even if the connection accepts it in parts.
 * @param ssh Client connection.
 * @param text Text to send.
 * @param size Number of bytes to send.
 * @return true if the whole reply was sent.
*/
static bool send_bytes(WOLFSSH *ssh, const char *text, size_t size)
{
    // Use the session channel that OpenSSH opened for this command.
    WOLFSSH_CHANNEL *channel = wolfSSH_ChannelNext(ssh, NULL);
    if (channel == NULL)
    {
        return false;
    }
    // Keep sending until the client accepts the whole message.
    while (size > 0)
    {
        // Ask the connection how much of the message it could send.
        const int sent = wolfSSH_ChannelSend(channel, (const byte *)text, (word32)size);
        if (sent <= 0)
        {
            // Stop if the client cannot receive any more.
            return false;
        }
        // Continue where the last write ended.
        text += sent;
        size -= (size_t)sent;
    }
    return true;
}

/*
 * @brief Collect the text printed by a command before sending it to the client.
 * @param cookie Reply buffer passed to the stream.
 * @param text Characters printed by the command.
 * @param length Number of characters to write.
 * @return Number of characters accepted by the stream.
*/
static int collect_reply(void *cookie, const char *text, int length)
{
    // Use the reply buffer for this command only.
    ssh_reply_t *reply = cookie;
    for (int i = 0; i < length; ++i)
    {
        // Leave space for the extra character that a terminal newline needs.
        const bool newline = reply->terminal && text[i] == '\n';
        if (reply->used + 1 + newline > sizeof(reply->text))
        {
            // Return a short write if a command produces too much text.
            return i;
        }
        if (newline)
        {
            // Take the terminal cursor back to the start of the line.
            reply->text[reply->used++] = '\r';
        }
        // Store this character so the whole reply can be sent together.
        reply->text[reply->used++] = text[i];
    }
    return length;
}

/*
 * @brief Run an application command and send its reply.
 * @param ssh Client connection.
 * @param line Command to run.
 * @param terminal Whether the reply is going to an interactive terminal.
 * @param restart Set by the command if the board should restart after replying.
 * @param status Receives the command's exit status.
 * @return true if the reply was sent.
*/
static bool run_command(WOLFSSH *ssh, const char *line, bool terminal, bool *restart, int *status)
{
    // Give the command its own output stream so UART output stays separate.
    ssh_reply_t reply = { .terminal = terminal };
    FILE *output = funopen(&reply, NULL, collect_reply, NULL, NULL);
    if (output == NULL)
    {
        // End this session if there is no memory to collect the reply.
        return false;
    }
    // Run the same command the person could type on the serial console.
    *status = console_config.command(line, output, restart);
    // Finish writing to the buffer before sending it.
    fclose(output);
    return send_bytes(ssh, reply.text, reply.used);
}

/*
 * @brief Read lines from an interactive SSH session and run each command.
 * @param ssh Client connection.
 * @return true if a command requested a restart.
*/
static bool run_terminal(WOLFSSH *ssh)
{
    // Keep room for one line of input and its ending zero byte.
    char line[COMMAND_CAPACITY];
    size_t length = 0;
    // Remember when a long line has to be discarded.
    bool too_long = false;
    // Ignore a line feed immediately after a carriage return.
    bool after_cr = false;
    // Keep the prompt length so the same prompt can be sent again.
    const size_t prompt_size = strlen(console_config.prompt);
    if (!send_bytes(ssh, console_config.prompt, prompt_size))
    {
        return false;
    }
    // Read small chunks so a pasted command can be handled in one read.
    byte input[128];
    int received;
    while ((received = wolfSSH_stream_read(ssh, input, sizeof(input))) > 0)
    {
        // Handle control keys and printable characters in the order they arrived.
        for (int i = 0; i < received; ++i)
        {
            const byte key = input[i];
            // Treat CR and LF as a single Enter key when both arrive.
            if (key == '\n' && after_cr)
            {
                after_cr = false;
                continue;
            }
            after_cr = key == '\r';
            // Finish the current line when Enter is pressed.
            if (key == '\r' || key == '\n')
            {
                if (!send_bytes(ssh, "\r\n", 2))
                {
                    return false;
                }
                // Keep an oversized line from running as a partial command.
                if (too_long)
                {
                    const char warning[] = "Command is too long.\r\n";
                    if (!send_bytes(ssh, warning, sizeof(warning) - 1))
                    {
                        return false;
                    }
                }
                else if (length != 0)
                {
                    // End the line and let 'exit' close the session.
                    line[length] = '\0';
                    if (strcmp(line, "exit") == 0)
                    {
                        return false;
                    }
                    // Finish sending the reply before applying a restart.
                    bool restart = false;
                    int status = 0;
                    if (!run_command(ssh, line, true, &restart, &status) || restart)
                    {
                        return restart;
                    }
                }
                // Clear the line and get ready for the next command.
                length = 0;
                too_long = false;
                if (!send_bytes(ssh, console_config.prompt, prompt_size))
                {
                    return false;
                }
            }
            else if (key == 4 || key == 3)
            {
                // Close cleanly when the client sends Ctrl+D or Ctrl+C.
                return false;
            }
            else if ((key == 8 || key == 127) && length > 0 && !too_long)
            {
                // Remove the last character from the line and the screen.
                --length;
                if (!send_bytes(ssh, "\b \b", 3))
                {
                    return false;
                }
            }
            else if (isprint(key))
            {
                // Drop the whole line if it cannot fit.
                if (length == sizeof(line) - 1)
                {
                    too_long = true;
                    continue;
                }
                // Keep the character and show it in the terminal.
                line[length++] = (char)key;
                if (!send_bytes(ssh, &line[length - 1], 1))
                {
                    return false;
                }
            }
        }
    }
    // Leave the board running when a client disconnects.
    return false;
}

/* Listener and public API */

/*
 * @brief Serve one SSH client and close its session before returning.
 * @param fd Accepted Ethernet socket.
 * @return true if the client ran a command that requires a restart.
*/
static bool serve_client(int fd)
{
    // Limit the time a stalled login or write can hold the only session.
    const struct timeval login_timeout = { .tv_sec = LOGIN_TIMEOUT_SECONDS };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &login_timeout, sizeof(login_timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &login_timeout, sizeof(login_timeout));
    // Prepare one SSH connection using the board's saved host key.
    WOLFSSH *ssh = wolfSSH_new(server);
    if (ssh == NULL)
    {
        return false;
    }
    // Connect wolfSSH to the accepted socket and limit login retries.
    wolfSSH_set_fd(ssh, fd);
    wolfSSH_SetMaxAuthAttempts(ssh, 3);
    bool restart = false;
    if (wolfSSH_accept(ssh) == WS_SUCCESS)
    {
        // Give an authenticated user time to type the next command.
        const struct timeval idle_timeout = { .tv_sec = IDLE_TIMEOUT_SECONDS };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle_timeout, sizeof(idle_timeout));
        // Handle either a one-off configuration command or a line-by-line prompt.
        int status = 0;
        const WS_SessionType type = wolfSSH_GetSessionType(ssh);
        if (type == WOLFSSH_SESSION_EXEC)
        {
            // Run only complete command lines that fit the console's input limit.
            const char *line = wolfSSH_GetSessionCommand(ssh);
            const word32 size = wolfSSH_GetSessionCommandSz(ssh);
            if (line != NULL && size < COMMAND_CAPACITY && strlen(line) == size)
            {
                if (!run_command(ssh, line, false, &restart, &status))
                {
                    status = 1;
                }
            }
            else
            {
                const char warning[] = "Invalid command line.\n";
                send_bytes(ssh, warning, sizeof(warning) - 1);
                status = 1;
            }
        }
        else if (type == WOLFSSH_SESSION_SHELL || type == WOLFSSH_SESSION_TERMINAL)
        {
            // Enter the same prompt that someone sees on UART.
            restart = run_terminal(ssh);
        }
        else
        {
            // Reject other SSH services with a failing exit status.
            status = 1;
        }
        // Tell the client how the command finished before closing.
        wolfSSH_stream_exit(ssh, status);
    }
    // Give back all memory used by this client.
    wolfSSH_free(ssh);
    return restart;
}

/*
 * @brief Load the host key and open the listening socket.
 * @return ESP_OK when the listener is ready or an ESP-IDF error code.
*/
static esp_err_t open_listener(void)
{
    // Keep the hardware noise source on for SSH keys and handshakes.
    bootloader_random_enable();
    // Set up wolfSSH before using any of its connection objects.
    if (wolfSSH_Init() != WS_SUCCESS)
    {
        return ESP_FAIL;
    }
    // Load the saved host key before opening the server to the network.
    uint8_t key[HOST_KEY_CAPACITY];
    size_t key_size = sizeof(key);
    const esp_err_t err = load_host_key(key, &key_size);
    if (err != ESP_OK)
    {
        wolfSSH_Cleanup();
        return err;
    }
    // Create the server once and share it across successive connections.
    server = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (server == NULL)
    {
        wolfSSH_Cleanup();
        return ESP_ERR_NO_MEM;
    }
    // Give the server the key that clients will remember after first login.
    if (wolfSSH_CTX_UsePrivateKey_buffer(server, key, (word32)key_size,
            WOLFSSH_FORMAT_ASN1) != WS_SUCCESS)
    {
        wolfSSH_CTX_free(server);
        server = NULL;
        wolfSSH_Cleanup();
        return ESP_FAIL;
    }
    // Require the configured password and listen for Ethernet clients.
    wolfSSH_SetUserAuthTypes(server, auth_types);
    wolfSSH_SetUserAuth(server, check_login);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const struct sockaddr_in address =
    {
        .sin_family = AF_INET,
        .sin_port = htons(console_config.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    // Allow one connection to wait for the current session to finish.
    if (listener < 0 || bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0
            || listen(listener, 1) != 0)
    {
        // Give back any resources that were opened before the failure.
        if (listener >= 0)
        {
            close(listener);
        }
        wolfSSH_CTX_free(server);
        server = NULL;
        wolfSSH_Cleanup();
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * @brief Open SSH and accept one client at a time without holding up the camera.
 * @param argument Unused task argument.
*/
static void ssh_task(void *argument)
{
    (void)argument;
    // Make the host key on the SSH task's own stack.
    const esp_err_t err = open_listener();
    if (err != ESP_OK)
    {
        // Leave the serial console running if SSH fails to start.
        ESP_LOGE(TAG, "SSH listener failed (%s)", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    // Show that the server is ready without logging its password.
    ESP_LOGI(TAG, "SSH listening on port %u", console_config.port);
    while (true)
    {
        // Sleep in accept until a connection is available.
        const int fd = accept(listener, NULL, NULL);
        if (fd < 0)
        {
            // Yield if the network stack reports a temporary error.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Run the session and close its socket even when login fails.
        const bool restart = serve_client(fd);
        close(fd);
        if (restart)
        {
            // Give Ethernet time to deliver the reply before rebooting.
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }
    }
}

/*
 * @brief Start a single-session SSH console with a saved host key.
 * @param config Port, login, prompt, and command handler.
 * @return ESP_OK when the task starts or an ESP-IDF error code.
*/
esp_err_t ssh_console_start(const ssh_console_config_t *config)
{
    // Check that the caller supplied a port and a way to run commands.
    if (config == NULL || config->port == 0 || config->prompt == NULL || config->command == NULL
            || config->username == NULL || config->username[0] == '\0'
            || config->password == NULL || config->password[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Create only one task for this console.
    if (start_requested)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // Copy the settings before creating the task that will use them.
    console_config = *config;
    // Give SSH enough stack to set up encryption, at a lower priority than capture.
    if (xTaskCreate(ssh_task, "ssh_console", 16384, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    start_requested = true;
    return ESP_OK;
}

