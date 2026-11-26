/*
 * @file camera_zenoh.h
 * @brief Publish ROS 2 compressed images over Zenoh with settings saved in flash.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

#ifndef RACKING_CAMERAS_CAMERA_ZENOH_H
#define RACKING_CAMERAS_CAMERA_ZENOH_H

/* Includes */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Settings */

typedef struct
{
    char id[32];
    char endpoint[32];
    char topic[64];
} camera_zenoh_settings_t;

typedef enum
{
    CAMERA_ZENOH_ID,
    CAMERA_ZENOH_ENDPOINT,
    CAMERA_ZENOH_TOPIC,
} camera_zenoh_setting_t;

/* Public API */

/*
 * @brief Load saved settings and prepare a publisher to be opened on demand.
 * @param defaults Settings to use when none have been saved.
 * @param domain_id ROS 2 discovery domain to join.
 * @return ESP_OK if the configuration is ready.
*/
esp_err_t camera_zenoh_configure(const camera_zenoh_settings_t *defaults, uint32_t domain_id);

/*
 * @brief Read the settings active on this boot.
 * @return The active settings, with an automatic ID already filled in.
*/
const camera_zenoh_settings_t *camera_zenoh_settings(void);

/*
 * @brief Save one validated setting for the next boot.
 * @param field Which setting to update.
 * @param value New setting text.
 * @return ESP_OK after the new configuration is written to flash.
*/
esp_err_t camera_zenoh_save(camera_zenoh_setting_t field, const char *value);

/*
 * @brief Open the session if necessary and check for a frame subscriber.
 * @param context Unused. There is one publisher on this board.
 * @return True if a subscriber is ready to receive frames.
*/
bool camera_zenoh_ready(void *context);

/*
 * @brief Send one JPEG and its capture time to the matching subscriber.
 * @param context Unused. There is one publisher on this board.
 * @param data JPEG bytes, valid until this call returns.
 * @param size Number of bytes in the JPEG.
 * @param timestamp_ns Board UTC time in nanoseconds, or zero before sync.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_zenoh_send(void *context, const uint8_t *data, uint32_t size, uint64_t timestamp_ns);

#endif
