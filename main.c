/*
 * @file main.c
 * @brief 
 * @author Ajay Varghese
 * @date 2026-11-25
*/

/* Includes */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "camera.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_sync.h"

/* Board configuration */

static const char *TAG = "app";

/* Waveshare ESP32-P4-ETH U10 IP101GRI, with a PHY-supplied reference clock. */
static const ethernet_config_t ethernet_pins =
{
    .mdc_gpio = 31,
    .mdio_gpio = 52,
    .ref_clk_gpio = 50,
    .phy_reset_gpio = 51,
    .tx_en_gpio = 49,
    .txd0_gpio = 34,
    .txd1_gpio = 35,
    .crs_dv_gpio = 28,
    .rxd0_gpio = 29,
    .rxd1_gpio = 30,
};

/* Waveshare ESP32-P4-ETH camera wiring and startup settings. */
static const camera_config_t camera_config =
{
    .sda_gpio = 7,
    .scl_gpio = 8,
    .reset_gpio = -1,
    .power_down_gpio = -1,
    .port = 8080,
    .mode =
    {
        .width = 1920,
        .height = 1080,
    },
};

/* UART commands */

/*
 * @brief Report the save result and restart the board if it succeeded.
 * @param err Result of writing the Ethernet settings.
 * @param mode Human-readable name of the saved IPv4 mode.
 * @return 1 if the settings could not be saved.
*/
static int apply_saved_mode(esp_err_t err, const char *mode)
{
    // Check that saving worked before restarting the board.
    if (err != ESP_OK)
    {
        // Tell the person at the console why the save failed.
        printf("IPv4 settings could not be saved (%s)\n", esp_err_to_name(err));
        // Tell the console the command failed.
        return 1;
    }

    // Let the person know what was saved and why the console will pause.
    printf("IPv4 mode saved: %s. Restarting.\n", mode);
    // Send the message now so it is visible before the restart.
    fflush(stdout);
    // Restart so Ethernet reads the new settings.
    esp_restart();
}

/*
 * @brief Save a static IPv4 address supplied through the UART console.
 * @param argc Number of arguments, including the command name.
 * @param argv Command name followed by address, netmask and optional gateway.
 * @return 1 for invalid arguments or failure to save.
*/
static int command_net_static(int argc, char **argv)
{
    // Ask for an address and a mask. The router address is optional.
    if (argc != 3 && argc != 4)
    {
        // Show how to type the command when the number of inputs is wrong.
        printf("Usage: net-static <address> <netmask> [gateway]\n");
        // Stop the command before reading inputs that may be missing.
        return 1;
    }

    // Use the router address if one was entered. A zero address means there is no router.
    const char *gateway = argc == 4 ? argv[3] : "0.0.0.0";
    // Save the fixed address and restart if the save worked.
    return apply_saved_mode(ethernet_save_static_ipv4(argv[1], argv[2], gateway), "static");
}

/*
 * @brief Restore DHCP for Ethernet through the UART console.
 * @param argc Number of arguments, including the command name.
 * @param argv Console arguments. This command accepts only its name.
 * @return 1 for invalid arguments or failure to save.
*/
static int command_net_dhcp(int argc, char **argv)
{
    // Check that nothing extra was typed after the command name.
    if (argc != 1)
    {
        // Show that this command takes no extra inputs.
        printf("Usage: %s\n", argv[0]);
        // Stop here so a mistyped command does not change the board settings.
        return 1;
    }
    // Save the choice to get an address automatically, then restart if it worked.
    return apply_saved_mode(ethernet_save_dhcp(), "DHCP");
}

/*
 * @brief Save the NTP server for the next time the board starts.
 * @param argc Number of arguments, including the command name.
 * @param argv Command name followed by an IPv4 address, auto, or off.
 * @return 1 for invalid input or failure to save.
*/
static int command_ntp_server(int argc, char **argv)
{
    // Check that the user gave us one setting to save.
    if (argc != 2)
    {
        // Show the ways the user can choose a time server.
        printf("Usage: ntp-server <pc-ip|auto|off>\n");
        // Stop before trying to read a missing value.
        return 1;
    }

    // Read the server setting the user typed.
    const char *setting = argv[1];
    // Leave room for the result of saving the setting.
    esp_err_t err;
    // Check whether the network should supply the NTP server.
    if (strcmp(setting, "auto") == 0)
    {
        // Save automatic discovery so it is used after restart.
        err = time_sync_save_dhcp();
    }
    else
    {
        // Leave out an address when the user wants to turn sync off.
        const char *address = strcmp(setting, "off") == 0 ? NULL : setting;
        // Save the address or off setting so it survives a restart.
        err = time_sync_save_server(address);
    }
    // Check that saving worked before restarting.
    if (err != ESP_OK)
    {
        // Show why the setting was not saved.
        printf("NTP server could not be saved (%s)\n", esp_err_to_name(err));
        // Tell the console that the command failed.
        return 1;
    }

    // Tell the user which setting will be used after restarting.
    printf("NTP server saved: %s. Restarting.\n", argv[1]);
    // Send the message before the board restarts.
    fflush(stdout);
    // Restart so the time service loads the saved setting.
    esp_restart();
}

/*
 * @brief Print the board's current UTC time once NTP has set it.
 * @param argc Number of arguments, including the command name.
 * @param argv Console arguments. This command accepts only its name.
 * @return 0 on success or 1 for invalid input or a time formatting error.
*/
static int command_time(int argc, char **argv)
{
    // Check that nothing extra was typed.
    if (argc != 1)
    {
        // Show that this command takes no extra inputs.
        printf("Usage: %s\n", argv[0]);
        // Stop before trying to read the clock.
        return 1;
    }

    // Make room for the current time.
    struct timeval now;
    // Check that a server has set the clock before showing it.
    if (!time_sync_get_utc(&now))
    {
        // Show why there is no trusted time to print yet.
        printf("Clock has not synced with NTP yet.\n");
        // Finish after reporting the current status.
        return 0;
    }

    // Make room to break the time into date and clock parts.
    struct tm utc;
    // Make room for the date and time text.
    char formatted[20];
    // Turn the clock reading into a UTC date and time.
    if (gmtime_r(&now.tv_sec, &utc) == NULL
            || strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
    {
        // Tell the user if the time could not be displayed.
        printf("Clock time could not be displayed.\n");
        // Report the error to the console.
        return 1;
    }

    // Include the small part of the second so timestamps are easy to compare.
    printf("%s.%06ldZ\n", formatted, (long)now.tv_usec);
    // Tell the console the time was printed.
    return 0;
}

/*
 * @brief Read or change the live camera resolution over UART.
 * @param argc Number of arguments, including the command name.
 * @param argv Command name followed by optional width and height.
 * @return 0 if the request succeeded or 1 if it could not be used.
*/
static int command_camera_settings(int argc, char **argv)
{
    // Check whether the user asked to see the current settings.
    if (argc == 1)
    {
        // Read the picture size the stream is set to use.
        const camera_mode_t mode = camera_stream_get_mode();
        // Check whether the camera was able to start.
        if (mode.width == 0)
        {
            // Point the user to the camera startup messages on this console.
            printf("Camera unavailable. Check the startup log.\n");
            // Report that there are no settings to show.
            return 1;
        }
        // Print the values in the same order as the command accepts them.
        printf("Camera resolution: %u x %u\n", mode.width, mode.height);
        // Finish after showing the settings.
        return 0;
    }

    // Require both dimensions when changing the resolution.
    if (argc != 3)
    {
        // Show the right order for the two numbers.
        printf("Usage: camera-settings [<width> <height>]\n");
        // Stop before reading any missing arguments.
        return 1;
    }

    // Leave room for both numbers after the command name.
    uint16_t values[2];
    // Read the width and height in order.
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    {
        // Read one argument as a decimal number.
        const char *number = argv[i + 1];
        // Reject signs, spaces and other characters that are not digits.
        if (number[0] == '\0' || number[strspn(number, "0123456789")] != '\0')
        {
            // Show why the setting could not be read.
            printf("Camera settings need whole numbers.\n");
            // Keep the current settings.
            return 1;
        }
        // Convert the digits to a number.
        const unsigned long value = strtoul(number, NULL, 10);
        // Prevent a large input from wrapping to a smaller number.
        if (value > UINT16_MAX)
        {
            // Show why the number was rejected.
            printf("Camera setting is out of range.\n");
            // Keep the current settings.
            return 1;
        }
        // Keep this value to pass to the camera.
        values[i] = (uint16_t)value;
    }

    // Ask the camera to apply both dimensions together.
    const camera_mode_t mode = { .width = values[0], .height = values[1] };
    // Let the camera check the mode before the stream uses it.
    const esp_err_t err = camera_stream_set_mode(mode);
    // Check whether the new settings were accepted.
    if (err != ESP_OK)
    {
        // Give the reason so the user can correct the setting.
        printf("Camera settings rejected (%s).\n", esp_err_to_name(err));
        // Let the console know the command failed.
        return 1;
    }
    // Confirm the settings the streaming task will use.
    printf("Camera resolution requested: %u x %u\n", mode.width, mode.height);
    // Finish without restarting the board.
    return 0;
}

/*
 * @brief Register network and camera commands and start the UART console.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t start_uart_console(void)
{
    // List the commands people can type at the console.
    const esp_console_cmd_t commands[] =
    {
        {
            // Use this name for the fixed address command.
            .command = "net-static",
            // Describe the command in the help list.
            .help = "Save a static IPv4 configuration and restart",
            // Show which values to type after the command name.
            .hint = "<address> <netmask> [gateway]",
            // Run this function when someone types the command.
            .func = command_net_static,
        },
        {
            // Use this name to switch back to an automatic address.
            .command = "net-dhcp",
            // Describe what happens when this command runs.
            .help = "Restore DHCP and restart",
            // Run this function to save the automatic setting.
            .func = command_net_dhcp,
        },
        {
            // Use this name to choose which computer sets the clock.
            .command = "ntp-server",
            // Describe the command in the help list.
            .help = "Choose an NTP server, automatic discovery, or off, then restart",
            // Show which values can be entered.
            .hint = "<pc-ip|auto|off>",
            // Run this function to save the choice.
            .func = command_ntp_server,
        },
        {
            // Use this name to check the current time.
            .command = "time",
            // Describe what the command shows.
            .help = "Show the synced UTC time",
            // Run this function to read the clock.
            .func = command_time,
        },
        {
            // Use this name to show or change the stream settings.
            .command = "camera-settings",
            // Describe both ways to use the command.
            .help = "Show or change the camera resolution",
            // Show which numbers can follow the command.
            .hint = "[<width> <height>]",
            // Run this function to handle the user's request.
            .func = command_camera_settings,
        },
    };

    // Add a help command so people can find the available commands.
    esp_err_t err = esp_console_register_help_command();
    // Check whether the help command was added.
    if (err != ESP_OK)
    {
        // Report the problem to the startup code.
        return err;
    }
    // Go through the list one command at a time.
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
    {
        // Make this command available at the console.
        err = esp_console_cmd_register(&commands[i]);
        // Check that the console accepted this command.
        if (err != ESP_OK)
        {
            // Stop setup if a command could not be added.
            return err;
        }
    }

    // Use the board's usual settings for its serial port.
    esp_console_dev_uart_config_t uart = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    // Start with the console's usual settings.
    esp_console_repl_config_t console = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    // Show this text when the console is ready for a command.
    console.prompt = "rift>";
    // Leave a place for the console to give back its handle.
    esp_console_repl_t *repl = NULL;
    // Create the console and connect it to the serial port.
    err = esp_console_new_repl_uart(&uart, &console, &repl);
    // Check that the console was created before trying to start it.
    if (err != ESP_OK)
    {
        // Report why the console could not be created.
        return err;
    }

    // Start listening for commands.
    err = esp_console_start_repl(repl);
    // Check whether the console actually started.
    if (err != ESP_OK)
    {
        // Free the console if it was created but could not start.
        repl->del(repl);
    }
    // Tell the startup code whether the console is ready.
    return err;
}

/* Application entry point */

/*
 * @brief Start Ethernet, the camera stream, and the UART console.
*/
void app_main(void)
{
    // Set up clock sync first so it sees the first Ethernet address.
    esp_err_t err = time_sync_start();
    // Check whether the clock service was able to prepare.
    if (err != ESP_OK)
    {
        // Report the problem and let the board continue starting.
        ESP_LOGE(TAG, "Clock sync setup failed (%s)", esp_err_to_name(err));
    }

    // Start Ethernet using the pins wired on this board.
    err = ethernet_start(&ethernet_pins);
    // Check whether the network started properly.
    if (err != ESP_OK)
    {
        // Show the network error so it can be fixed from the console.
        ESP_LOGE(TAG, "Ethernet startup failed (%s)", esp_err_to_name(err));
    }

    // Make the live camera stream available over Ethernet.
    err = camera_stream_start(&camera_config);
    // Check whether the camera and its server could start.
    if (err != ESP_OK)
    {
        // Show the problem on UART so a cable or sensor issue can be found.
        ESP_LOGE(TAG, "Camera startup failed (%s)", esp_err_to_name(err));
    }

    // Start the serial console even if Ethernet is down.
    err = start_uart_console();
    // Check whether the console was able to start.
    if (err != ESP_OK)
    {
        // Print the reason the console could not start.
        ESP_LOGE(TAG, "UART console startup failed (%s)", esp_err_to_name(err));
    }
}
