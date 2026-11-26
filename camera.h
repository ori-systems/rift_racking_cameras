/*
 * @file camera.h
 * @brief Capture and encode live JPEG frames from a MIPI CSI camera.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

#ifndef RACKING_CAMERAS_CAMERA_H
#define RACKING_CAMERAS_CAMERA_H

/* Includes */

#include <stdint.h>
#include <stdbool.h>

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

/* Frame output callbacks run on the camera sender task. */
typedef struct
{
    bool (*ready)(void *context);
    esp_err_t (*send)(void *context, const uint8_t *data, uint32_t size, uint64_t timestamp_ns);
    void *context;
} camera_output_t;

/*
 * @brief Make live JPEG frames available at /stream on the given port.
 * @param config Camera wiring and HTTP port for this board.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_stream_start(const camera_config_t *config);

/*
 * @brief Send live JPEGs to a caller-supplied transport.
 * @param config Camera wiring and startup resolution.
 * @param output Transport callbacks. The context must live for the lifetime of the camera.
 * @return ESP_OK if setup worked, or an ESP-IDF error code.
 * @note The send callback must finish reading the JPEG before it returns.
*/
esp_err_t camera_publish_start(const camera_config_t *config, const camera_output_t *output);

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
