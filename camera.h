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

/* Picture controls */

typedef struct
{
    bool focus_available;
    bool autofocus;
    uint32_t focus_position;
    float red_gain;          /* Zero selects automatic white balance. */
    float blue_gain;
    uint32_t exposure_us;    /* Zero selects automatic exposure. */
    uint32_t applied_exposure_us;
    uint32_t min_exposure_us;
    uint32_t max_exposure_us;
} camera_controls_t;

/*
 * @brief Read the requested controls and the current exposure reported by the driver.
 * @param controls Space for the settings and exposure range.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_get_controls(camera_controls_t *controls);

/*
 * @brief Enable automatic focus or hold the lens in its current position.
 * @param enabled True to start searching for focus.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_set_autofocus(bool enabled);

/*
 * @brief Select fixed color gains or automatic white balance.
 * @param red Red gain from 0.5 to 4.0, or zero for automatic white balance.
 * @param blue Blue gain from 0.5 to 4.0, or zero for automatic white balance.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_set_white_balance(float red, float blue);

/*
 * @brief Set the exposure time or return to automatic exposure.
 * @param exposure_us Exposure in microseconds, or zero for automatic control.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_set_exposure(uint32_t exposure_us);

#endif
