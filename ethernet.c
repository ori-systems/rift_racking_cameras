/*
 * @file ethernet.h
 * @brief API for ESP32-P4 RMII Ethernet.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

/* Includes */

#include "ethernet.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Component state */

static const char *TAG = "ethernet";
static const char *SETTINGS_NAMESPACE = "eth_config";
static const char *STATIC_IPV4_KEY = "ipv4_v1";
static esp_eth_handle_t running_driver;

/* IPv4 validation */

/*
 * @brief Parse a canonical dotted-decimal IPv4 address.
 * @param text Address string to validate.
 * @param parsed Output for the address in network byte order.
 * @return True when the string contains a valid address.
*/
static bool parse_ipv4(const char *text, esp_ip4_addr_t *parsed)
{
    // Check that an address was entered and can be read as an IP address.
    if (text == NULL || esp_netif_str_to_ip4(text, parsed) != ESP_OK)
    {
        // Reject missing or unreadable text.
        return false;
    }

    // Make room for the longest IPv4 address and its end marker.
    char canonical[16];
    // Turn the address back into text and accept only the usual four-number form.
    return esp_ip4addr_ntoa(parsed, canonical, sizeof(canonical)) != NULL
           && strcmp(text, canonical) == 0;
}

/*
 * @brief Check that an IPv4 address can identify a host on a LAN subnet.
 * @param address IPv4 address in host byte order.
 * @param host_mask Mask of the host bits, in host byte order.
 * @return True for a usable unicast host address.
*/
static bool is_host_address(uint32_t address, uint32_t host_mask)
{
    // Take the first number so we can rule out groups of special addresses.
    const uint32_t first_octet = address >> 24;
    // Keep the part that names a device within its network.
    const uint32_t host = address & host_mask;
    // Accept addresses for one device and skip special addresses.
    return first_octet != 0 && first_octet != 127 && first_octet < 224
           && host != 0 && host != host_mask;
}

/*
 * @brief Validate the static IPv4 address, LAN netmask and optional gateway.
 * @param info Parsed IPv4 settings, in network byte order.
 * @return True when the settings describe a host and a reachable gateway.
*/
static bool is_valid_static_ipv4(const esp_netif_ip_info_t *info)
{
    // Put the address numbers in the order the chip uses to compare them.
    const uint32_t address = esp_netif_htonl(info->ip.addr);
    // Put the mask in the same order so it lines up with the address.
    const uint32_t netmask = esp_netif_htonl(info->netmask.addr);
    // Put the router address in that order too so we can check it.
    const uint32_t gateway = esp_netif_htonl(info->gw.addr);
    // Flip the mask to find which part of the address can change for each device.
    const uint32_t host_mask = ~netmask;

    // Check that the mask has one group of network bits and leaves room for devices.
    if (host_mask < 3 || netmask == 0 || (host_mask & (host_mask + 1)) != 0
            || !is_host_address(address, host_mask))
    {
        // Reject the settings if the mask or the board address cannot work on this network.
        return false;
    }

    // Allow no router, or require a different, usable router address on the same network.
    return gateway == 0 || (gateway != address && is_host_address(gateway, host_mask)
                           && (gateway & netmask) == (address & netmask));
}

/* Non-volatile IPv4 settings */

/*
 * @brief Initialize NVS and open this component's settings namespace.
 * @param mode Read-only or read-write NVS access.
 * @param handle Output for the open handle, which the caller closes.
 * @return ESP_OK on success or an NVS error code.
*/
static esp_err_t open_settings(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    // Get the board's saved storage ready to use.
    esp_err_t err = nvs_flash_init();
    // Check that the storage is ready before trying to open a record.
    if (err != ESP_OK)
    {
        // Pass the storage problem back to the caller.
        return err;
    }
    // Open the area that holds the Ethernet settings.
    return nvs_open(SETTINGS_NAMESPACE, mode, handle);
}

/*
 * @brief Read and validate a previously saved static IPv4 setting.
 * @param info Output for the saved address, netmask and gateway.
 * @return ESP_OK for valid settings, ESP_ERR_NVS_NOT_FOUND for DHCP, or an error.
*/
static esp_err_t load_static_ipv4(esp_netif_ip_info_t *info)
{
    // Make a place to keep track of the open settings area.
    nvs_handle_t handle;
    // Open the settings for reading because this function only needs to look.
    esp_err_t err = open_settings(NVS_READONLY, &handle);
    // Check that there is an open record before trying to read it.
    if (err != ESP_OK)
    {
        // Pass back the reason the settings could not be opened.
        return err;
    }

    // Tell the reader how much space is available for the saved values.
    size_t length = sizeof(*info);
    // Read the saved address, mask and router address together.
    err = nvs_get_blob(handle, STATIC_IPV4_KEY, info, &length);
    // Close the settings area once the read is finished.
    nvs_close(handle);
    // Check that the saved data is the right size and still describes a valid address.
    if (err == ESP_OK && (length != sizeof(*info) || !is_valid_static_ipv4(info)))
    {
        // Reject the saved values so the board does not use bad settings.
        return ESP_ERR_INVALID_ARG;
    }
    // Tell the caller if it can use what was read.
    return err;
}

/*
 * @brief Commit a static IPv4 setting or clear it to restore DHCP.
 * @param info Settings to store, or NULL to clear the saved settings.
 * @return ESP_OK on success or an NVS error code.
*/
static esp_err_t write_ipv4_settings(const esp_netif_ip_info_t *info)
{
    // Make a place to keep track of the open settings area.
    nvs_handle_t handle;
    // Open the settings with permission to change them.
    esp_err_t err = open_settings(NVS_READWRITE, &handle);
    // Check that we can write before making any changes.
    if (err != ESP_OK)
    {
        // Tell the caller why the settings are unavailable.
        return err;
    }

    // Choose whether to save a fixed address or clear it.
    if (info != NULL)
    {
        // Store the address, mask and router address as one record.
        err = nvs_set_blob(handle, STATIC_IPV4_KEY, info, sizeof(*info));
    }
    else
    {
        // Remove the fixed address so the next boot asks the network for one.
        err = nvs_erase_key(handle, STATIC_IPV4_KEY);
        // Check whether the record was already missing.
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            // Count that as success because there is nothing left to clear.
            err = ESP_OK;
        }
    }

    // Finish saving only if the change was accepted.
    if (err == ESP_OK)
    {
        // Make the change stick through a restart or power loss.
        err = nvs_commit(handle);
    }
    // Close the settings area once the work is finished.
    nvs_close(handle);
    // Tell the caller whether the change was saved.
    return err;
}

/*
 * @brief Save validated static IPv4 settings for the next boot.
 * @param address Interface IPv4 address in dotted-decimal notation.
 * @param netmask Contiguous LAN netmask in dotted-decimal notation.
 * @param gateway Gateway on the same subnet, or 0.0.0.0 for no gateway.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid values, or an NVS error.
*/
esp_err_t ethernet_save_static_ipv4(const char *address, const char *netmask,
                                    const char *gateway)
{
    // Start with a blank set of settings to fill with the new values.
    esp_netif_ip_info_t info = {0};
    // Read all three values and make sure they work together.
    if (!parse_ipv4(address, &info.ip) || !parse_ipv4(netmask, &info.netmask)
            || !parse_ipv4(gateway, &info.gw) || !is_valid_static_ipv4(&info))
    {
        // Keep the old settings if any of the new values are wrong.
        return ESP_ERR_INVALID_ARG;
    }
    // Save the checked values for the next time the board starts.
    return write_ipv4_settings(&info);
}

/*
 * @brief Set DHCP as the IPv4 mode for the next boot.
 * @return ESP_OK on success or an NVS error code.
*/
esp_err_t ethernet_save_dhcp(void)
{
    // Clear the fixed address so Ethernet gets an address automatically next time.
    return write_ipv4_settings(NULL);
}

/* Ethernet events and startup */

/*
 * @brief Apply the saved IPv4 configuration before Ethernet link starts.
 * @param netif Ethernet network interface.
 * @return ESP_OK on success or an ESP-NETIF error code.
*/
static esp_err_t configure_ipv4(esp_netif_t *netif)
{
    // Make room for an address, mask and router address.
    esp_netif_ip_info_t info = {0};
    // Look for a fixed address saved by the user.
    esp_err_t err = load_static_ipv4(&info);
    // Use automatic addressing if there is no usable fixed address.
    if (err != ESP_OK)
    {
        // Only warn if something went wrong while reading the settings.
        if (err != ESP_ERR_NVS_NOT_FOUND)
        {
            // Show the storage error on the console.
            ESP_LOGW(TAG, "Could not load IPv4 settings (%s)", esp_err_to_name(err));
        }
        // Show that Ethernet will ask the network for its address.
        ESP_LOGI(TAG, "IPv4 mode: DHCP");
        // Let startup continue with the usual automatic setting.
        return ESP_OK;
    }

    // Stop automatic addressing before giving the board a fixed address.
    err = esp_netif_dhcpc_stop(netif);
    // Treat an already stopped address service as ready.
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        // Stop setup if the service could not be turned off.
        return err;
    }

    // Give Ethernet the address, mask and router address from storage.
    err = esp_netif_set_ip_info(netif, &info);
    // Show the fixed settings only after Ethernet accepted them.
    if (err == ESP_OK)
    {
        // Print the fixed address so someone can find the board on the network.
        ESP_LOGI(TAG, "IPv4 mode: static, address " IPSTR ", mask " IPSTR ", gateway " IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
    }
    // Tell the startup code whether applying the fixed settings worked.
    return err;
}

/*
 * @brief Log Ethernet link state changes for this component's driver.
 * @param arg Driver handle registered as the callback context.
 * @param base ESP-IDF event base.
 * @param id Ethernet event identifier.
 * @param data Event payload containing the driver handle.
*/
static void on_link_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Check that this message came from our Ethernet connection.
    if (*(esp_eth_handle_t *)data != arg)
    {
        // Ignore messages from other connections.
        return;
    }

    // Pick a message based on what happened to the connection.
    switch (id)
    {
    // The board has started checking for a connection.
    case ETHERNET_EVENT_START:
        // Tell the person at the console that the board is waiting for Ethernet.
        ESP_LOGI(TAG, "Waiting for Ethernet link");
        // End this case so it prints only the matching message.
        break;
    // The Ethernet cable now has a working connection.
    case ETHERNET_EVENT_CONNECTED:
        // Report that the cable link is ready.
        ESP_LOGI(TAG, "Link up");
        // End this case so it does not also report a disconnect.
        break;
    // The board lost its Ethernet connection.
    case ETHERNET_EVENT_DISCONNECTED:
        // Report the lost link so it is clear why the board may be unreachable.
        ESP_LOGI(TAG, "Link down");
        // End this case after reporting the lost link.
        break;
    // Ignore other messages because they do not describe a link change.
    default:
        break;
    }
}

/*
 * @brief Log the active IPv4 settings when the Ethernet interface is ready.
 * @param arg Network interface registered as the callback context.
 * @param base ESP-IDF event base.
 * @param id IPv4 event identifier.
 * @param data Event payload containing the active IPv4 settings.
*/
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Read the message that carries the new IP settings.
    const ip_event_got_ip_t *event = data;
    // Check that the new address belongs to our Ethernet connection.
    if (event->esp_netif != arg)
    {
        // Skip the message if a different connection got the address.
        return;
    }

    // Take the address details out of the message.
    const esp_netif_ip_info_t *ip = &event->ip_info;
    // Print the active address so it can be used from a PC.
    ESP_LOGI(TAG, "IPv4 " IPSTR ", mask " IPSTR ", gateway " IPSTR,
             IP2STR(&ip->ip), IP2STR(&ip->netmask), IP2STR(&ip->gw));
}

typedef struct
{
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    esp_eth_handle_t driver;
    esp_netif_t *netif;
    esp_eth_netif_glue_handle_t glue;
    esp_event_handler_instance_t link_handler;
    esp_event_handler_instance_t ip_handler;
    bool start_attempted;
} ethernet_startup_t;

/*
 * @brief Release the Ethernet resources acquired during a failed start.
 * @param startup Resources created before the failure. Unused handles are NULL.
*/
static void cleanup_failed_start(const ethernet_startup_t *startup)
{
    // Only try to stop Ethernet if we reached the step that starts it.
    if (startup->start_attempted)
    {
        // Stop checking the cable before taking the connection apart.
        esp_eth_stop(startup->driver);
    }
    // Check whether the address watcher was added.
    if (startup->ip_handler != NULL)
    {
        // Remove the address watcher before freeing what it uses.
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              startup->ip_handler);
    }
    // Check whether the cable watcher was added.
    if (startup->link_handler != NULL)
    {
        // Remove the cable watcher so it does not use a freed driver.
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID,
                                              startup->link_handler);
    }
    // Check whether Ethernet was given a link to the network software.
    if (startup->glue != NULL)
    {
        // Free the piece that joins the driver to the network software.
        esp_eth_del_netif_glue(startup->glue);
    }
    // Check whether the software side of the connection was made.
    if (startup->netif != NULL)
    {
        // Free that connection and its settings.
        esp_netif_destroy(startup->netif);
    }
    // Check whether the Ethernet driver was installed.
    if (startup->driver != NULL)
    {
        // Remove the driver before freeing the pieces it controls.
        esp_err_t err = esp_eth_driver_uninstall(startup->driver);
        // Check whether the driver was actually removed.
        if (err != ESP_OK)
        {
            // Show why the cleanup could not finish.
            ESP_LOGE(TAG, "Driver cleanup failed (%s)", esp_err_to_name(err));
            // Keep the hardware pieces in place if the driver may still be using them.
            return;
        }
    }
    // Check whether we created controls for the chip connected to the cable.
    if (startup->phy != NULL)
    {
        // Free those controls now that the driver is gone.
        startup->phy->del(startup->phy);
    }
    // Check whether we created the ESP32's part of the Ethernet driver.
    if (startup->mac != NULL)
    {
        // Free the ESP32's part of the driver too.
        startup->mac->del(startup->mac);
    }
}

/*
 * @brief Create and start the RMII driver and its IPv4 network interface.
 * @param config Board GPIO wiring for RMII and PHY management.
 * @param startup Tracks each acquired resource so startup can be rolled back.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t start_rmii(const ethernet_config_t *config, ethernet_startup_t *startup)
{
    // Start with the usual settings for the ESP32's Ethernet hardware.
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    // Choose the wire that keeps control messages in time.
    emac.smi_gpio.mdc_num = config->mdc_gpio;
    // Choose the wire that carries control messages to and from the Ethernet chip.
    emac.smi_gpio.mdio_num = config->mdio_gpio;
    // Use the timing signal supplied by the chip connected to the cable.
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    // Choose the pin where that timing signal comes in.
    emac.clock_config.rmii.clock_gpio = config->ref_clk_gpio;
    // Tell the ESP32 which pins carry data to and from the Ethernet chip.
    emac.emac_dataif_gpio.rmii = (eth_mac_rmii_gpio_config_t)
    {
        // Choose the wire that tells the chip when the ESP32 is sending data.
        .tx_en_num = config->tx_en_gpio,
        // Choose the wire that carries the lower bit of each outgoing pair.
        .txd0_num = config->txd0_gpio,
        // Choose the wire that carries the upper bit of each outgoing pair.
        .txd1_num = config->txd1_gpio,
        // Choose the wire that tells the ESP32 there is incoming data to read.
        .crs_dv_num = config->crs_dv_gpio,
        // Choose the wire that carries the lower bit of each incoming pair.
        .rxd0_num = config->rxd0_gpio,
        // Choose the wire that carries the upper bit of each incoming pair.
        .rxd1_num = config->rxd1_gpio,
    };

    // Use the usual settings for the part inside the ESP32 that moves data.
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    // Create that part of the driver and remember it for cleanup.
    startup->mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    // Check that the ESP32 side of the connection was created.
    if (startup->mac == NULL)
    {
        // Stop setup if that part could not be made.
        return ESP_FAIL;
    }

    // Start with the usual settings for the chip that connects to the cable.
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    // Choose the pin used to reset that chip.
    phy_config.reset_gpio_num = config->phy_reset_gpio;
    // Create the controls for the chip and remember them for cleanup.
    startup->phy = esp_eth_phy_new_generic(&phy_config);
    // Check that the controls were created.
    if (startup->phy == NULL)
    {
        // Stop setup and report that the controls could not be set up in memory.
        return ESP_ERR_NO_MEM;
    }

    // Pair the ESP32's Ethernet hardware with the chip wired to the cable.
    esp_eth_config_t driver_config = ETH_DEFAULT_CONFIG(startup->mac, startup->phy);
    // Install the driver that controls both pieces.
    esp_err_t err = esp_eth_driver_install(&driver_config, &startup->driver);
    // Check that the driver was installed before creating a network connection.
    if (err != ESP_OK)
    {
        // Pass back the driver setup error.
        return err;
    }

    // Use the usual settings for a wired network connection.
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    // Create the software side of the network connection.
    startup->netif = esp_netif_new(&netif_config);
    // Check that the software side was created.
    if (startup->netif == NULL)
    {
        // Stop and report that the network connection could not be set up in memory.
        return ESP_ERR_NO_MEM;
    }

    // Make a piece that lets the driver talk to the network software.
    startup->glue = esp_eth_new_netif_glue(startup->driver);
    // Check that the piece was made before trying to attach it.
    if (startup->glue == NULL)
    {
        // Stop and report that there was no memory to make the connection.
        return ESP_ERR_NO_MEM;
    }

    // Join the hardware driver to the network software.
    err = esp_netif_attach(startup->netif, startup->glue);
    // Check that the two sides are joined.
    if (err != ESP_OK)
    {
        // Pass back the connection error if they could not be joined.
        return err;
    }

    // Apply the saved choice of fixed or automatic address.
    err = configure_ipv4(startup->netif);
    // Check that the address settings were applied.
    if (err != ESP_OK)
    {
        // Stop setup if Ethernet could not use the address settings.
        return err;
    }

    // Ask to be told when the cable connection comes up or goes down.
    err = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_link_event,
                                              startup->driver, &startup->link_handler);
    // Check that the cable watcher was added.
    if (err != ESP_OK)
    {
        // Pass back the reason the cable watcher could not be added.
        return err;
    }

    // Ask to be told when Ethernet has an IP address.
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip,
                                              startup->netif, &startup->ip_handler);
    // Check that the address watcher was added.
    if (err != ESP_OK)
    {
        // Pass back the reason the address watcher could not be added.
        return err;
    }

    // Remember that a failed start may still need to be stopped.
    startup->start_attempted = true;
    // Start the driver so it can detect the cable and bring up the connection.
    return esp_eth_start(startup->driver);
}

/*
 * @brief Install the RMII driver, configure IPv4 and start link monitoring.
 * @param config Board GPIO wiring for RMII and PHY management.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t ethernet_start(const ethernet_config_t *config)
{
    // Check that we were told how the board's Ethernet pins are wired.
    if (config == NULL)
    {
        // Stop before using a missing set of pin settings.
        return ESP_ERR_INVALID_ARG;
    }
    // Check whether this Ethernet connection is already running.
    if (running_driver != NULL)
    {
        // Refuse to start a second copy of the same connection.
        return ESP_ERR_INVALID_STATE;
    }

    // Get the board's network software ready.
    esp_err_t err = esp_netif_init();
    // Check that the network software is ready before using it.
    if (err != ESP_OK)
    {
        // Pass back the network setup error.
        return err;
    }
    // Get the system that passes messages about link and address changes ready.
    err = esp_event_loop_create_default();
    // Keep going if that message system already exists.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        // Stop if the message system could not be made.
        return err;
    }

    // Start with an empty list of parts that may need to be freed.
    ethernet_startup_t startup = {0};
    // Set up the Ethernet connection and keep track of each part it creates.
    err = start_rmii(config, &startup);
    // Check whether the connection started.
    if (err != ESP_OK)
    {
        // Free the parts that were created before the failure.
        cleanup_failed_start(&startup);
        // Tell the caller which setup step failed.
        return err;
    }

    // Remember the driver so later calls know Ethernet is running.
    running_driver = startup.driver;
    // Tell the caller that Ethernet started.
    return ESP_OK;
}
