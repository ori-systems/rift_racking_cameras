/*
 * @file time_sync.h
 * @brief Save an NTP server and read the board's synced UTC time.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

#ifndef RACKING_CAMERAS_TIME_SYNC_H
#define RACKING_CAMERAS_TIME_SYNC_H

/* Includes */

#include <stdbool.h>
#include <sys/time.h>

#include "esp_err.h"

/* Public API */

/*
 * @brief Prepare clock sync before the Ethernet connection starts.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t time_sync_start(void);

/*
 * @brief Save the NTP server address for the next boot.
 * @param address IPv4 address of the server, or NULL to turn sync off.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid address, or an NVS error.
*/
esp_err_t time_sync_save_server(const char *address);

/*
 * @brief Read UTC after the board has received the time from its NTP server.
 * @param now Output for the current UTC time since the Unix epoch.
 * @return True when a synced time was read.
*/
bool time_sync_get_utc(struct timeval *now);

#endif
