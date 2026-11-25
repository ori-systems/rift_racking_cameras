/*
 * @file time_sync.c
 * @brief Set the board's clock from an NTP server after Ethernet is ready.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

/* Includes */

#include "time_sync.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Service state */

static const char *TAG = "time_sync";
static const char *SETTINGS_NAMESPACE = "time_sync";
static const char *SERVER_KEY = "ntp_server";
static const char *DISABLED_SETTING = "off";
static char server_address[16];
static bool service_ready;
static bool use_dhcp;
static atomic_bool clock_ready;

/* Saved server settings */

/*
 * @brief Check that the supplied text is a usable IPv4 server address.
 * @param address Text containing the address to check.
 * @return True for an address in the usual four-number form.
*/
static bool server_address_is_valid(const char *address)
{
    // Make room for the address in the board's network format.
    esp_ip4_addr_t ip;
    // Check that the text can be read as an IP address.
    if (address == NULL || esp_netif_str_to_ip4(address, &ip) != ESP_OK)
    {
        // Stop before trying to save an unreadable address.
        return false;
    }

    // Use the first number to rule out groups meant for special uses.
    const uint32_t first = esp_netif_htonl(ip.addr) >> 24;
    // Make room to write the address back as four numbers.
    char canonical[16];
    // Require a normal device address and the usual spelling.
    return first != 0 && first != 127 && first < 224
           && esp_ip4addr_ntoa(&ip, canonical, sizeof(canonical)) != NULL
           && strcmp(address, canonical) == 0;
}

/*
 * @brief Open the flash storage area for the time server setting.
 * @param mode Access to request for the settings area.
 * @param handle Output for the opened settings area.
 * @return ESP_OK on success or an NVS error code.
*/
static esp_err_t open_settings(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    // Get flash storage ready before opening a record.
    esp_err_t err = nvs_flash_init();
    // Check that storage is ready.
    if (err != ESP_OK)
    {
        // Pass the storage error back so it can be shown to the user.
        return err;
    }

    // Open the area reserved for the time server address.
    return nvs_open(SETTINGS_NAMESPACE, mode, handle);
}

/*
 * @brief Read the saved server into the buffer used by the sync service.
 * @return ESP_OK for a saved setting, ESP_ERR_NVS_NOT_FOUND for automatic mode, or an error.
*/
static esp_err_t load_server(void)
{
    // Make a place to keep track of the open settings area.
    nvs_handle_t handle;
    // Open the saved settings for reading.
    esp_err_t err = open_settings(NVS_READONLY, &handle);
    // Check that the area opened before trying to read from it.
    if (err != ESP_OK)
    {
        // Pass back the reason the settings could not be opened.
        return err;
    }

    // Tell the reader how much room is available for the address.
    size_t length = sizeof(server_address);
    // Read the server address saved at the console.
    err = nvs_get_str(handle, SERVER_KEY, server_address, &length);
    // Close the storage area after reading.
    nvs_close(handle);
    // Accept the saved off setting or a usable address.
    if (err == ESP_OK && strcmp(server_address, DISABLED_SETTING) != 0
            && !server_address_is_valid(server_address))
    {
        // Reject damaged or invalid saved settings.
        return ESP_ERR_INVALID_ARG;
    }

    // Tell startup whether it has a server address.
    return err;
}

/*
 * @brief Store the clock setting for the next time the board starts.
 * @param setting Server address, off, or NULL to use DHCP.
 * @return ESP_OK on success or an NVS error code.
*/
static esp_err_t store_server_setting(const char *setting)
{
    // Make a place to keep track of the open settings area.
    nvs_handle_t handle;
    // Open the settings with permission to write.
    esp_err_t err = open_settings(NVS_READWRITE, &handle);
    // Check that the storage is open before changing it.
    if (err != ESP_OK)
    {
        // Tell the caller why saving could not start.
        return err;
    }

    // Choose whether to save a fixed setting or use DHCP.
    if (setting != NULL)
    {
        // Keep the chosen server address or the off setting.
        err = nvs_set_str(handle, SERVER_KEY, setting);
    }
    else
    {
        // Clear the saved setting so the board can ask DHCP for a server.
        err = nvs_erase_key(handle, SERVER_KEY);
        // Check if the setting was already cleared.
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            // Count it as success because there is nothing left to remove.
            err = ESP_OK;
        }
    }

    // Finish saving only if the change was accepted.
    if (err == ESP_OK)
    {
        // Make the setting survive a restart or power loss.
        err = nvs_commit(handle);
    }

    // Close the storage area after the change.
    nvs_close(handle);
    // Tell the caller whether the setting was saved.
    return err;
}

/*
 * @brief Save a specific NTP server or turn clock sync off.
 * @param address IPv4 address of the server, or NULL to turn sync off.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid address, or an NVS error.
*/
esp_err_t time_sync_save_server(const char *address)
{
    // Check the new address before replacing the saved setting.
    if (address != NULL && !server_address_is_valid(address))
    {
        // Keep the old setting if the address is not usable.
        return ESP_ERR_INVALID_ARG;
    }

    // Save the address, or remember that sync was turned off.
    return store_server_setting(address != NULL ? address : DISABLED_SETTING);
}

/*
 * @brief Save automatic NTP discovery as the setting for the next boot.
 * @return ESP_OK on success or an NVS error code.
*/
esp_err_t time_sync_save_dhcp(void)
{
    // Clear the fixed setting so DHCP can supply the server.
    return store_server_setting(NULL);
}

/* Ethernet events and time sync */

/*
 * @brief Mark the board clock as ready after the server sets it.
 * @param received Time supplied by the server.
*/
static void on_clock_synced(struct timeval *received)
{
    // Check that the notice includes the time received from the server.
    if (received == NULL)
    {
        // Keep waiting if there was no time in the notice.
        return;
    }

    // Let other parts of the program know they can use the clock.
    atomic_store(&clock_ready, true);
    // Check which source supplied the time.
    if (use_dhcp)
    {
        // Report that the server supplied by DHCP set the clock.
        ESP_LOGI(TAG, "Clock synced using the NTP server from DHCP.");
    }
    else
    {
        // Show the saved address that set the clock.
        ESP_LOGI(TAG, "Clock synced to %s", server_address);
    }
}

/*
 * @brief Copy the first NTP server supplied by DHCP.
 * @param output Space to hold the IPv4 address as text.
 * @return ESP_OK when a server is available or ESP_ERR_NOT_FOUND.
*/
static esp_err_t read_dhcp_server(void *output)
{
    // Read the server while running inside the network task.
    const ip_addr_t *server = esp_sntp_getserver(0);
    // Check that DHCP supplied an IPv4 address.
    if (server == NULL || !IP_IS_V4(server) || ip_addr_isany(server))
    {
        // Tell the caller that the network did not supply a server.
        return ESP_ERR_NOT_FOUND;
    }

    // Copy the address as text so the console can show it.
    return ipaddr_ntoa_r(server, output, sizeof(server_address)) != NULL
           ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/*
 * @brief Drop the learned server when the Ethernet cable link goes down.
 * @param arg Extra data supplied when the notice was registered.
 * @param base Group the notice belongs to.
 * @param id Kind of notice that arrived.
 * @param data Details about the lost connection.
*/
static void on_ethernet_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Stop asking for the time while the cable link is down.
    esp_sntp_stop();
    // Clear the server so it is not reused on a different network.
    esp_sntp_setserver(0, NULL);
}

/*
 * @brief Start clock sync when Ethernet has an address.
 * @param arg Extra data supplied when the notice was registered.
 * @param base Group the notice belongs to.
 * @param id Kind of notice that arrived.
 * @param data Details of the new Ethernet address.
*/
static void on_ethernet_address(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Check whether the board is using a server supplied by DHCP.
    if (use_dhcp)
    {
        // Read which Ethernet connection got an address.
        const ip_event_got_ip_t *event = data;
        // Make room to find out whether DHCP is in use.
        esp_netif_dhcp_status_t dhcp_status;
        // Check whether this connection gets its address through DHCP.
        if (esp_netif_dhcpc_get_status(event->esp_netif, &dhcp_status) != ESP_OK
                || dhcp_status != ESP_NETIF_DHCP_STARTED)
        {
            // Explain why a fixed network address needs a saved time server.
            ESP_LOGI(TAG, "Automatic NTP needs DHCP. Use a fixed NTP server with a static IP.");
            // Wait for a server setting before asking for the time.
            return;
        }

        // Make room for the time server address sent by the network.
        char learned[sizeof(server_address)];
        // Look up the address inside the network task so it can be read safely.
        if (esp_netif_tcpip_exec(read_dhcp_server, learned) != ESP_OK)
        {
            // Wait for a server address if DHCP did not supply one this time.
            ESP_LOGI(TAG, "Waiting for DHCP to advertise an NTP server (option 42).");
        }
        else
        {
            // Show which address the network supplied for time sync.
            ESP_LOGI(TAG, "NTP server from DHCP: %s", learned);
        }
    }
    else
    {
        // Show which saved address the board will ask for the time.
        ESP_LOGI(TAG, "Waiting for time from %s", server_address);
    }

    // Start the service. It waits to send a request until it has a server address.
    esp_err_t err = esp_netif_sntp_start();
    // Check whether the clock service started.
    if (err != ESP_OK)
    {
        // Show the reason the board could not start asking for the time.
        ESP_LOGE(TAG, "Clock sync could not start (%s)", esp_err_to_name(err));
    }
}

/*
 * @brief Prepare the clock service and wait for Ethernet to get an address.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t time_sync_start(void)
{
    // Check that the clock service is not already set up.
    if (service_ready)
    {
        // Keep the existing service instead of creating a second one.
        return ESP_ERR_INVALID_STATE;
    }

    // Read the saved choice for clock sync.
    esp_err_t err = load_server();
    // Treat a missing setting as automatic discovery.
    use_dhcp = err == ESP_ERR_NVS_NOT_FOUND;
    // Check for a real storage error before using the setting.
    if (err != ESP_OK && !use_dhcp)
    {
        // Show the storage problem without choosing a server.
        return err;
    }

    // Respect a saved choice to turn clock sync off.
    if (!use_dhcp && strcmp(server_address, DISABLED_SETTING) == 0)
    {
        // Tell the user that the board will keep its clock sync off.
        ESP_LOGI(TAG, "Clock sync is off.");
        // Let the rest of the board start normally.
        return ESP_OK;
    }

    // Get the board's network software ready for the time service.
    err = esp_netif_init();
    // Check that the network software is ready before using it.
    if (err != ESP_OK)
    {
        // Pass back the network setup error.
        return err;
    }

    // Get the system that passes notices about address changes ready.
    err = esp_event_loop_create_default();
    // Keep going if another part of the program already made it.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        // Stop if the notice system could not be made.
        return err;
    }

    // Start with the settings for one saved server.
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server_address);
    // Wait until Ethernet has an IP address before sending anything.
    config.start = false;
    // Let DHCP supply a server only when automatic mode was chosen.
    config.server_from_dhcp = use_dhcp;
    // Leave the server list empty when the network supplies the address.
    config.num_of_servers = use_dhcp ? 0 : 1;
    // Use a notice when the time arrives so startup can keep going.
    config.wait_for_sync = false;
    // Mark the board clock as ready once the server sets it.
    config.sync_cb = on_clock_synced;
    // Set up the service using these choices.
    err = esp_netif_sntp_init(&config);
    // Check that the time service was created.
    if (err != ESP_OK)
    {
        // Tell the caller why setup could not finish.
        return err;
    }

    // Ask to be told whenever Ethernet gets an address.
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                     on_ethernet_address, NULL);
    // Check that the notice was registered.
    if (err != ESP_OK)
    {
        // Free the time service if there is no way to start it.
        esp_netif_sntp_deinit();
        // Report the error so startup can print it.
        return err;
    }

    // Watch for a lost cable link only when using a server from DHCP.
    if (use_dhcp)
    {
        // Ask to be told when Ethernet loses its connection.
        err = esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                         on_ethernet_disconnected, NULL);
        // Check that the cable notice was registered.
        if (err != ESP_OK)
        {
            // Remove the address notice before freeing the time service.
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                         on_ethernet_address);
            // Free the service because it cannot switch networks safely.
            esp_netif_sntp_deinit();
            // Pass back the setup error.
            return err;
        }

        // Show that the board will use the time server sent with its network settings.
        ESP_LOGI(TAG, "NTP mode: auto. Waiting for DHCP option 42.");
    }
    else
    {
        // Show the saved server while waiting for Ethernet.
        ESP_LOGI(TAG, "NTP server: %s. Waiting for Ethernet.", server_address);
    }

    // Remember that the time service is ready and waiting.
    service_ready = true;
    // Tell the caller that the time service was set up.
    return ESP_OK;
}

/*
 * @brief Read the board's current UTC time after it has synced.
 * @param now Output for the current UTC time since the Unix epoch.
 * @return True when a synced time was read.
*/
bool time_sync_get_utc(struct timeval *now)
{
    // Only return a time if the caller gave us a place to put it and the clock is ready.
    return now != NULL && atomic_load(&clock_ready) && gettimeofday(now, NULL) == 0;
}
