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

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Service state */

static const char *TAG = "time_sync";
static const char *SETTINGS_NAMESPACE = "time_sync";
static const char *SERVER_KEY = "ntp_server";
static char server_address[16];
static bool service_ready;
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
 * @return ESP_OK for a valid server, ESP_ERR_NVS_NOT_FOUND when unset, or an error.
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
    // Check the saved address before the board tries to contact it.
    if (err == ESP_OK && !server_address_is_valid(server_address))
    {
        // Reject damaged or invalid saved settings.
        return ESP_ERR_INVALID_ARG;
    }

    // Tell startup whether it has a server address.
    return err;
}

/*
 * @brief Store a new server address or clear the saved setting.
 * @param address IPv4 address of the server, or NULL to turn sync off.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid address, or an NVS error.
*/
esp_err_t time_sync_save_server(const char *address)
{
    // Check a new address before making any changes to storage.
    if (address != NULL && !server_address_is_valid(address))
    {
        // Keep the old setting if the new address is invalid.
        return ESP_ERR_INVALID_ARG;
    }

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

    // Choose whether to keep a new address or clear the saved one.
    if (address != NULL)
    {
        // Put the new server address in storage.
        err = nvs_set_str(handle, SERVER_KEY, address);
    }
    else
    {
        // Remove the saved address so the board won't look for a server.
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
    // Show which server set the clock.
    ESP_LOGI(TAG, "Clock synced to %s", server_address);
}

/*
 * @brief Ask the saved server for the time when Ethernet has an address.
 * @param arg Extra data supplied when the notice was registered.
 * @param base Group the notice belongs to.
 * @param id Kind of notice that arrived.
 * @param data Details of the new Ethernet address.
*/
static void on_ethernet_address(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Start asking for the time now that Ethernet has an address.
    esp_err_t err = esp_netif_sntp_start();
    // Check whether the clock service started.
    if (err != ESP_OK)
    {
        // Show the reason the board could not start asking for the time.
        ESP_LOGE(TAG, "Clock sync could not start (%s)", esp_err_to_name(err));
        // Stop here so the console does not report a successful start.
        return;
    }

    // Show which server the board is trying to reach.
    ESP_LOGI(TAG, "Waiting for time from %s", server_address);
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

    // Find the server address saved at the console.
    esp_err_t err = load_server();
    // Check whether someone has chosen a time server yet.
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // Let the user know why the board is not asking for the time.
        ESP_LOGI(TAG, "No NTP server configured.");
        // Let the rest of the board start even without a time server.
        return ESP_OK;
    }

    // Check that a saved server address could be read.
    if (err != ESP_OK)
    {
        // Pass back the error so the console can show it.
        return err;
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

    // Prepare to ask the saved server for the time.
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server_address);
    // Wait until Ethernet has an IP address before sending anything.
    config.start = false;
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

    // Remember that the time service is ready and waiting.
    service_ready = true;
    // Show the saved server while waiting for Ethernet.
    ESP_LOGI(TAG, "NTP server: %s. Waiting for Ethernet.", server_address);
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
