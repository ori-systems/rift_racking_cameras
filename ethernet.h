/*
 * @file ethernet.h
 * @brief API for ESP32-P4 RMII Ethernet.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

#ifndef RACKING_CAMERAS_ETHERNET_H
#define RACKING_CAMERAS_ETHERNET_H

/* Includes */

#include "esp_err.h"

/* Hardware configuration */

typedef struct
{
    int mdc_gpio;
    int mdio_gpio;
    int ref_clk_gpio;    /* Input for the 50 MHz reference clock. */
    int phy_reset_gpio;  /* -1 when reset is handled by hardware. */
    int tx_en_gpio;
    int txd0_gpio;
    int txd1_gpio;
    int crs_dv_gpio;
    int rxd0_gpio;
    int rxd1_gpio;
} ethernet_config_t;

/* API */

/*
 * @brief Start one RMII Ethernet interface using the saved IPv4 settings.
 * @param config Board wiring for a PHY supplying the 50 MHz reference clock.
 * @return ESP_OK on success or an ESP-IDF error code.
 * @note Call from one startup task. The component keeps the interface alive.
 *       Listen for IP_EVENT_ETH_GOT_IP before opening network sessions.
*/
esp_err_t ethernet_start(const ethernet_config_t *config);

/*
 * @brief Save a static IPv4 configuration for the next restart.
 * @param address Interface IPv4 address in dotted-decimal notation.
 * @param netmask Contiguous IPv4 netmask in dotted-decimal notation.
 * @param gateway IPv4 gateway on the same subnet, or 0.0.0.0 for no gateway.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid values, or an NVS error.
*/
esp_err_t ethernet_save_static_ipv4(const char *address, const char *netmask,
                                    const char *gateway);

/*
 * @brief Clear the saved static address and use DHCP on the next restart.
 * @return ESP_OK on success or an NVS error code.
*/
esp_err_t ethernet_save_dhcp(void);

#endif
