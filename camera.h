/*
 * @file camera.h
 * @brief Start a live JPEG stream from a MIPI CSI camera.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

#ifndef RACKING_CAMERAS_CAMERA_H
#define RACKING_CAMERAS_CAMERA_H

/* Includes */

#include <stdint.h>

#include "esp_err.h"

/* Camera wiring and stream setup */

typedef struct
{
    uint16_t width;
    uint16_t height;
} camera_mode_t;

typedef struct
{
    int sda_gpio;
    int scl_gpio;
    int reset_gpio;       /* -1 if reset is handled on the camera. */
    int power_down_gpio;  /* -1 if power is handled on the camera. */
    uint16_t port;
    camera_mode_t mode;   /* Zero dimensions use the sensor default. */
} camera_config_t;

/*
 * @brief Make live JPEG frames available at /stream on the given port.
 * @param config Camera wiring and HTTP port for this board.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_stream_start(const camera_config_t *config);

/*
 * @brief Read the requested camera resolution.
 * @return The requested mode, or all zeros if the stream has not started.
*/
camera_mode_t camera_stream_get_mode(void);

/*
 * @brief Select a capture mode for the current and future viewers.
 * @param mode Requested dimensions.
 * @return ESP_OK if accepted or an ESP-IDF error code.
*/
esp_err_t camera_stream_set_mode(camera_mode_t mode);

#endif
