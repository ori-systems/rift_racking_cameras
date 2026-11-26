/*
 * @file camera.c
 * @brief Capture live JPEG frames and send them through the selected transport.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

/* Includes */

#include "camera.h"

#include <stdbool.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "driver/jpeg_encode.h"
#include "esp_http_server.h"
#include "esp_ipa.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

/* Stream settings */

enum
{
    CAPTURE_BUFFER_COUNT = 2,
    JPEG_BUFFER_COUNT = 2,
    JPEG_QUALITY = 70,
    TIMING_FRAME_COUNT = 30,
    MAX_FRAME_LONG_SIDE = 1920,
    MAX_FRAME_SHORT_SIDE = 1080,
};

static const char *TAG = "camera";
static const char *STREAM_TYPE = "multipart/x-mixed-replace; boundary=frame";
static const uint32_t VIDEO_DEVICES = ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP
        | ESP_VIDEO_INIT_FLAGS_MOTOR;
static httpd_handle_t stream_server;
static bool video_ready;
static camera_output_t publisher_output;
static esp_cam_sensor_format_t *sensor_modes;
static size_t sensor_mode_count;
static portMUX_TYPE mode_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_mode_t requested_mode;
static camera_controls_t requested_controls;
static uint32_t capture_generation;

typedef struct
{
    uint8_t *data;
    size_t capacity;
    uint32_t size;
    uint64_t timestamp_ns;
} jpeg_frame_t;

typedef struct
{
    int fd;
    void *frames[CAPTURE_BUFFER_COUNT];
    size_t frame_size;
    bool streaming;
    jpeg_encoder_handle_t encoder;
    jpeg_encode_cfg_t jpeg_format;
    jpeg_frame_t jpeg_frames[JPEG_BUFFER_COUNT];
    QueueHandle_t available;
    QueueHandle_t ready;
    SemaphoreHandle_t encoder_done;
    bool encoder_running;
} capture_t;

/* Picture controls */

/*
 * @brief Run Espressif's image processing algorithms before applying console settings.
 * @param pipeline Active image pipeline.
 * @param stats Measurements from the current picture.
 * @param sensor Current sensor and lens settings.
 * @param metadata Changes to apply to the camera hardware.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t __real_esp_ipa_pipeline_process(esp_ipa_pipeline_handle_t pipeline,
        const esp_ipa_stats_t *stats, const esp_ipa_sensor_t *sensor, esp_ipa_metadata_t *metadata);

/*
 * @brief Apply picture controls on the ISP task with the normal sensor updates.
 * @param pipeline Active image pipeline.
 * @param stats Measurements from the current picture.
 * @param sensor Current sensor and lens settings.
 * @param metadata Changes to apply to the camera hardware.
 * @return ESP_OK on success or the error from the image pipeline.
*/
esp_err_t __wrap_esp_ipa_pipeline_process(esp_ipa_pipeline_handle_t pipeline,
        const esp_ipa_stats_t *stats, const esp_ipa_sensor_t *sensor, esp_ipa_metadata_t *metadata)
{
    // Let the SDK calculate the automatic settings first.
    const esp_err_t err = __real_esp_ipa_pipeline_process(pipeline, stats, sensor, metadata);
    if (err != ESP_OK)
    {
        return err;
    }

    // Copy the console settings without holding the lock during camera work.
    portENTER_CRITICAL(&mode_lock);
    const camera_controls_t controls = requested_controls;
    const uint32_t generation = capture_generation;
    if (sensor->focus_info != NULL)
    {
        // Report the last lens position the SDK successfully set.
        requested_controls.focus_position = sensor->focus_info->cur_pos;
    }
    portEXIT_CRITICAL(&mode_lock);

    // Hold the physical lens in place when autofocus is disabled.
    if (!controls.autofocus)
    {
        metadata->flags &= ~IPA_METADATA_FLAGS_FP;
    }

    // Hold gain and choose the requested exposure in the driver's normal units.
    if (controls.exposure_us != 0)
    {
        metadata->flags &= ~(IPA_METADATA_FLAGS_ET | IPA_METADATA_FLAGS_GN);
        // Allow the rounding needed to use whole rows of the sensor.
        const uint32_t difference = sensor->cur_exposure > controls.exposure_us
                ? sensor->cur_exposure - controls.exposure_us
                : controls.exposure_us - sensor->cur_exposure;
        if (difference > sensor->step_exposure + 1)
        {
            metadata->exposure = controls.exposure_us;
            metadata->flags |= IPA_METADATA_FLAGS_ET;
        }
    }

    // Remember the latest automatic gains so auto can resume immediately.
    static float auto_red = 1.0f;
    static float auto_blue = 1.0f;
    if (metadata->flags & IPA_METADATA_FLAGS_RG)
    {
        auto_red = metadata->red_gain;
    }
    if (metadata->flags & IPA_METADATA_FLAGS_BG)
    {
        auto_blue = metadata->blue_gain;
    }

    // Apply manual gains only when they change or a capture session is reopened.
    static float previous_red;
    static float previous_blue;
    static uint32_t previous_generation;
    const bool changed = previous_red != controls.red_gain || previous_blue != controls.blue_gain;
    if (controls.red_gain != 0.0f)
    {
        // Keep this session's fixed gains when AWB suggests a different value.
        metadata->flags &= ~(IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG);
        if (changed || generation != previous_generation)
        {
            metadata->red_gain = controls.red_gain;
            metadata->blue_gain = controls.blue_gain;
            metadata->flags |= IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG;
        }
    }
    else if (changed)
    {
        // Restore the current auto gains without waiting for a lighting change.
        metadata->red_gain = auto_red;
        metadata->blue_gain = auto_blue;
        metadata->flags |= IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG;
    }
    // Remember the settings the ISP saw on this frame.
    previous_red = controls.red_gain;
    previous_blue = controls.blue_gain;
    previous_generation = generation;
    return ESP_OK;
}

/*
 * @brief Place three focus measurement windows across the centre of the picture.
 * @param mode Dimensions of the active capture format.
 * @return ESP_OK when the focus scan was reset or an ESP-IDF error code.
*/
static esp_err_t set_focus_windows(camera_mode_t mode)
{
    // Keep the windows inside the picture even after a resolution change.
    isp_window_t windows[ISP_AF_WINDOW_NUM] = {0};
    for (unsigned i = 0; i < ISP_AF_WINDOW_NUM; ++i)
    {
        // Match the IMX708 tuning's centre band and scale it to the active size.
        windows[i].top_left.x = mode.width * (1 + 2 * i) / 8;
        windows[i].top_left.y = mode.height * 340u / 1080;
        windows[i].btm_right.x = mode.width * (3 + 2 * i) / 8 - 1;
        windows[i].btm_right.y = mode.height * 740u / 1080 - 1;
    }
    // Updating the windows also starts a fresh focus scan.
    return esp_video_isp_pipeline_set_af_windows(windows);
}

/*
 * @brief Read the requested controls and the current exposure reported by the driver.
 * @param controls Space for the settings and exposure range.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_get_controls(camera_controls_t *controls)
{
    // Check that there is room for the current settings and a camera to read.
    if (controls == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!video_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // Copy the settings while the ISP task may also be using them.
    portENTER_CRITICAL(&mode_lock);
    *controls = requested_controls;
    portEXIT_CRITICAL(&mode_lock);
    // Read the exposure that the SDK last applied and the sensor's limits.
    esp_video_isp_pipeline_sensor_ae_t ae;
    const esp_err_t err = esp_video_isp_pipeline_get_sensor_ae(&ae);
    if (err == ESP_OK)
    {
        controls->applied_exposure_us = ae.exposure_us;
        controls->min_exposure_us = ae.min_exposure_us;
        controls->max_exposure_us = ae.max_exposure_us;
    }
    return err;
}

/*
 * @brief Enable automatic focus or hold the lens in its current position.
 * @param enabled True to start searching for focus.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_set_autofocus(bool enabled)
{
    // Require a working camera and its actual focus motor.
    camera_controls_t controls;
    esp_err_t err = camera_get_controls(&controls);
    if (err != ESP_OK)
    {
        return err;
    }
    if (!controls.focus_available)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Reset the scan when turning focus on so it starts with the current picture.
    if (enabled)
    {
        err = set_focus_windows(camera_stream_get_mode());
        if (err != ESP_OK)
        {
            return err;
        }
    }
    // Let the ISP task know whether it should move the lens.
    portENTER_CRITICAL(&mode_lock);
    requested_controls.autofocus = enabled;
    portEXIT_CRITICAL(&mode_lock);
    return ESP_OK;
}

/*
 * @brief Select fixed color gains or automatic white balance.
 * @param red Red gain from 0.5 to 4.0, or zero for automatic white balance.
 * @param blue Blue gain from 0.5 to 4.0, or zero for automatic white balance.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_set_white_balance(float red, float blue)
{
    // A pair of zeros selects auto. All manual values must fit the gain range.
    if (!(red == 0.0f && blue == 0.0f)
            && !(red >= 0.5f && red <= 4.0f && blue >= 0.5f && blue <= 4.0f))
    {
        return ESP_ERR_INVALID_ARG;
    }
    camera_controls_t controls;
    const esp_err_t err = camera_get_controls(&controls);
    if (err != ESP_OK)
    {
        return err;
    }
    // Give both values to the ISP together so it cannot see half an update.
    portENTER_CRITICAL(&mode_lock);
    requested_controls.red_gain = red;
    requested_controls.blue_gain = blue;
    portEXIT_CRITICAL(&mode_lock);
    return ESP_OK;
}

/*
 * @brief Set the exposure time or return to automatic exposure.
 * @param exposure_us Exposure in microseconds, or zero for automatic control.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_set_exposure(uint32_t exposure_us)
{
    // Check the current sensor range before accepting a fixed exposure.
    camera_controls_t controls;
    const esp_err_t err = camera_get_controls(&controls);
    if (err != ESP_OK)
    {
        return err;
    }
    if (exposure_us != 0 && (exposure_us < controls.min_exposure_us
            || exposure_us > controls.max_exposure_us))
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Apply this request on the ISP task before the next sensor update.
    portENTER_CRITICAL(&mode_lock);
    requested_controls.exposure_us = exposure_us;
    portEXIT_CRITICAL(&mode_lock);
    return ESP_OK;
}

/* Camera and encoder setup */

/*
 * @brief Find the fastest sensor format that can supply the requested size.
 * @param mode Requested dimensions.
 * @return A format from the driver, or NULL if it cannot supply the mode.
*/
static esp_cam_sensor_format_t *find_sensor_mode(camera_mode_t mode)
{
    // Keep the fastest matching sensor format when there is more than one.
    esp_cam_sensor_format_t *best = NULL;
    // Look through the formats that work with the JPEG stream.
    for (size_t i = 0; i < sensor_mode_count; ++i)
    {
        // Read this size and the rate the sensor can run at.
        esp_cam_sensor_format_t *sensor = &sensor_modes[i];
        // Choose the fastest format that provides this size.
        if (sensor->width == mode.width && sensor->height == mode.height
                && (best == NULL || sensor->fps > best->fps))
        {
            // Remember this format for the next comparison.
            best = sensor;
        }
    }
    // Give back the best match, if there was one.
    return best;
}

/*
 * @brief Read the sensor's compatible modes before the HTTP server starts.
 * @param mode Startup settings, filled with the sensor's own default if requested.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t load_sensor_modes(camera_mode_t *mode)
{
    // Open the camera to ask its driver which picture sizes it supports.
    const int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    // Check that the camera is available.
    if (fd < 0)
    {
        // Stop if there is no driver to ask.
        return ESP_ERR_NOT_FOUND;
    }

    // Check for a real focus motor rather than assuming every sensor has one.
    struct v4l2_query_ext_ctrl focus = { .id = V4L2_CID_FOCUS_ABSOLUTE };
    requested_controls.focus_available = ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &focus) == 0;

    // Start with the driver's first format.
    struct v4l2_sensor_format_enum entry = {0};
    // Keep the reason if any part of setup fails.
    esp_err_t err = ESP_OK;
    // Read formats until the driver runs out.
    for (; ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &entry) == 0; ++entry.index)
    {
        // Read the size once for the 1080p and JPEG checks.
        const uint16_t width = entry.format.width;
        const uint16_t height = entry.format.height;
        // Allow up to 1080p in either landscape or portrait.
        if ((width > MAX_FRAME_LONG_SIDE || height > MAX_FRAME_SHORT_SIDE)
                && (width > MAX_FRAME_SHORT_SIDE || height > MAX_FRAME_LONG_SIDE))
        {
            // Skip larger pictures.
            continue;
        }
        // Keep only sizes and pixel data the current JPEG setup can use.
        if ((entry.format.format != ESP_CAM_SENSOR_PIXFORMAT_RAW8
             && entry.format.format != ESP_CAM_SENSOR_PIXFORMAT_RAW10)
                || entry.format.fps == 0
                || width % 16 != 0 || height % 8 != 0 || width == 0 || height == 0)
        {
            // Move on to the next format.
            continue;
        }
        // Make room for this format in the list used by the UART and HTTP tasks.
        esp_cam_sensor_format_t *modes = realloc(sensor_modes, (sensor_mode_count + 1) * sizeof(*modes));
        // Check that there is enough memory for the list.
        if (modes == NULL)
        {
            // Keep the old pointer so startup can free it.
            err = ESP_ERR_NO_MEM;
            // Stop reading formats after the failure.
            break;
        }
        // Keep the new address and copy in the format from the driver.
        sensor_modes = modes;
        // Count this format now that it has an entry.
        sensor_modes[sensor_mode_count++] = entry.format;
    }

    // Use the camera's own size when the caller leaves both dimensions blank.
    if (err == ESP_OK && mode->width == 0 && mode->height == 0)
    {
        // Make room for the settings the camera started with.
        esp_cam_sensor_format_t native;
        // Check that the camera can tell us its default size.
        if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &native) == 0)
        {
            // Fill in the dimensions from the sensor's default format.
            mode->width = native.width;
            mode->height = native.height;
        }
        else
        {
            // Stop startup if there is no default size to use.
            err = ESP_FAIL;
        }
    }
    // Close the camera until the first viewer connects.
    close(fd);
    // Reject a startup mode that the driver cannot supply.
    return err == ESP_OK && find_sensor_mode(*mode) == NULL ? ESP_ERR_NOT_SUPPORTED : err;
}

/*
 * @brief Restore the manual exposure after the sensor reloads its format.
 * @param fd Open camera device.
 * @param format Format containing the duration of one row of exposure.
 * @param exposure_us Time requested in microseconds.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t restore_exposure(int fd, const esp_cam_sensor_format_t *format, uint32_t exposure_us)
{
    // Read the sensor timing and check which exposure values the driver accepts.
    struct v4l2_query_ext_ctrl range = { .id = V4L2_CID_EXPOSURE };
    if (format->isp_info == NULL || format->isp_info->isp_v1_info.tline_ns == 0
            || ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &range) != 0 || range.step == 0)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Round the shutter time to the same row increments used by the ISP.
    const uint32_t row_ns = format->isp_info->isp_v1_info.tline_ns;
    uint64_t rows = ((uint64_t)exposure_us * 1000 + row_ns / 2) / row_ns;
    rows = rows / range.step * range.step;
    // Keep the request inside the limits of the newly selected format.
    if (rows < (uint64_t)range.minimum)
    {
        rows = (uint64_t)range.minimum;
    }
    if (rows > (uint64_t)range.maximum)
    {
        rows = (uint64_t)range.maximum;
    }
    // Set the shutter before the new capture begins delivering frames.
    struct v4l2_ext_control exposure = { .id = V4L2_CID_EXPOSURE, .value = (int32_t)rows };
    struct v4l2_ext_controls request =
    {
        .ctrl_class = V4L2_CID_CAMERA_CLASS,
        .count = 1,
        .controls = &exposure,
    };
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &request) == 0 ? ESP_OK : ESP_FAIL;
}

/*
 * @brief Program the sensor size before allocating buffers.
 * @param fd Open camera device.
 * @param mode Requested picture size.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t configure_capture_mode(int fd, camera_mode_t mode)
{
    // Look up the sensor settings for this picture size.
    esp_cam_sensor_format_t *sensor = find_sensor_mode(mode);
    // Stop if the requested settings are unavailable.
    if (sensor == NULL)
    {
        // Let the caller report that this combination cannot be used.
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Read the current sensor format to avoid rewriting it when it already matches.
    esp_cam_sensor_format_t current;
    // Change the sensor format only before the driver starts using frame buffers.
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &current) < 0
            || (current.regs != sensor->regs && ioctl(fd, VIDIOC_S_SENSOR_FMT, sensor) < 0))
    {
        // Report that the sensor could not be set up.
        return ESP_FAIL;
    }

    // A different sensor format writes its own exposure defaults.
    if (current.regs != sensor->regs)
    {
        portENTER_CRITICAL(&mode_lock);
        const uint32_t exposure_us = requested_controls.exposure_us;
        portEXIT_CRITICAL(&mode_lock);
        if (exposure_us != 0)
        {
            // Reapply the shutter even if the ISP still remembers the old value.
            const esp_err_t err = restore_exposure(fd, sensor, exposure_us);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Could not restore exposure after mode change (%s)", esp_err_to_name(err));
            }
        }
    }

    // Tell the caller the sensor size is ready.
    return ESP_OK;
}

/*
 * @brief Return the buffers and close the camera when a viewer leaves.
 * @param capture Camera and encoder parts created for this viewer.
*/
static void close_capture(const capture_t *capture)
{
    // Wait for the encoding task before releasing any memory it could still use.
    if (capture->encoder_running)
    {
        // Put a stop message ahead of any buffers waiting to be encoded.
        jpeg_frame_t *stop = NULL;
        // There is room for this message in addition to all the buffers.
        xQueueSendToFront(capture->available, &stop, 0);
        // The task signals when it has finished reading the camera and JPEG buffers.
        xSemaphoreTake(capture->encoder_done, portMAX_DELAY);
    }

    // Stop the camera before releasing its buffers.
    if (capture->streaming)
    {
        // Tell the camera which kind of stream to stop.
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        // Stop filling buffers once the viewer is gone.
        ioctl(capture->fd, VIDIOC_STREAMOFF, &type);
    }

    // Close the camera so the driver can free its raw frame buffers.
    if (capture->fd >= 0)
    {
        // Release the open camera before opening it for another viewer.
        close(capture->fd);
    }

    // Give back the hardware encoder if it was created.
    if (capture->encoder != NULL)
    {
        // Release the encoder for the next stream.
        jpeg_del_encoder_engine(capture->encoder);
    }

    // Return both reusable JPEG buffers, even if setup only made one.
    for (unsigned i = 0; i < JPEG_BUFFER_COUNT; ++i)
    {
        // Free the buffer or leave the empty entry alone.
        free(capture->jpeg_frames[i].data);
    }
    // Free the list of buffers the worker can use.
    if (capture->available != NULL)
    {
        // No task is waiting on this list anymore.
        vQueueDelete(capture->available);
    }
    // Free the list of pictures waiting to be sent.
    if (capture->ready != NULL)
    {
        // No task is waiting on this list anymore.
        vQueueDelete(capture->ready);
    }
    // Give back the signal used to tell us the encoding task finished.
    if (capture->encoder_done != NULL)
    {
        // No task will use this signal again.
        vSemaphoreDelete(capture->encoder_done);
    }
}

/*
 * @brief Ask the camera for reusable buffers and queue them for capture.
 * @param capture Open camera and places to store the buffer addresses.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t queue_capture_buffers(capture_t *capture)
{
    // Give the camera a small set of buffers to fill in turn.
    struct v4l2_requestbuffers request =
    {
        // Use two buffers so the camera can fill one while we use the other.
        .count = CAPTURE_BUFFER_COUNT,
        // Ask for buffers that hold pictures from the camera.
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        // Let the driver allocate camera memory so it is safe for the hardware.
        .memory = V4L2_MEMORY_MMAP,
    };

    // Check that the driver accepted our buffer request.
    if (ioctl(capture->fd, VIDIOC_REQBUFS, &request) < 0)
    {
        // Report that the camera could not make its buffers.
        return ESP_ERR_NO_MEM;
    }

    // Set up each of the available capture buffers.
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i)
    {
        // Describe which buffer we want to read from the driver.
        struct v4l2_buffer buffer =
        {
            // Pick a buffer that holds a captured frame.
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            // Use memory already owned by the camera driver.
            .memory = V4L2_MEMORY_MMAP,
            // Find the next buffer in the camera's list.
            .index = i,
        };

        // Check the size and location before using this buffer.
        if (ioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) < 0
                || buffer.length < capture->frame_size)
        {
            // Stop if the driver cannot give us room for a whole frame.
            return ESP_FAIL;
        }

        // Use the camera's own memory so the raw picture doesn't have to be copied.
        capture->frames[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, capture->fd, buffer.m.offset);
        // Check that the buffer is available before asking the camera to fill it.
        if (capture->frames[i] == NULL || capture->frames[i] == MAP_FAILED
                || ioctl(capture->fd, VIDIOC_QBUF, &buffer) < 0)
        {
            // Let the caller close the parts that were already set up.
            return ESP_FAIL;
        }
    }

    // Tell the caller the buffers are ready.
    return ESP_OK;
}

/*
 * @brief Open the camera and start filling reusable raw frame buffers.
 * @param capture Empty set of parts for the new stream.
 * @param mode Picture size requested by the application.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t open_capture(capture_t *capture, camera_mode_t mode)
{
    // Open the camera connected to the two-lane picture port.
    capture->fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    // Check that a camera was found before using it.
    if (capture->fd < 0)
    {
        // Tell the caller there is no capture device to open.
        return ESP_ERR_NOT_FOUND;
    }

    // Set the camera's picture size before asking for frame buffers.
    esp_err_t err = configure_capture_mode(capture->fd, mode);
    // Stop if the camera could not apply those settings.
    if (err != ESP_OK)
    {
        // Let the caller report the problem and release the camera.
        return err;
    }

    // Ask the camera what picture size it will provide.
    struct v4l2_format format =
    {
        // Read settings for incoming pictures.
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    // Check that the current settings were read.
    if (ioctl(capture->fd, VIDIOC_G_FMT, &format) < 0)
    {
        // Stop if the size cannot be read.
        return ESP_FAIL;
    }

    // Use YUV422 when JPEG needs 8-row blocks. YUV420 uses 16-row blocks.
    const bool use_yuv422 = mode.height % 16 != 0;
    // Ask the camera hardware to produce the chosen color layout directly.
    const uint32_t pixel_format = use_yuv422 ? V4L2_PIX_FMT_UYVY : V4L2_PIX_FMT_YUV420;
    // Pass the color layout along to the capture driver.
    format.fmt.pix.pixelformat = pixel_format;
    // Make sure the camera accepted the format and dimensions.
    if (ioctl(capture->fd, VIDIOC_S_FMT, &format) < 0
            || ioctl(capture->fd, VIDIOC_G_FMT, &format) < 0
            || format.fmt.pix.pixelformat != pixel_format
            || format.fmt.pix.width != mode.width || format.fmt.pix.height != mode.height)
    {
        // Report that the camera cannot provide a picture the encoder can use directly.
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Count the pixels so we can work out how much memory holds one picture.
    const size_t pixels = (size_t)mode.width * mode.height;
    // YUV422 uses four bytes for two pixels. YUV420 uses three.
    capture->frame_size = use_yuv422 ? pixels * 2 : pixels * 3 / 2;
    // Prepare buffers that the camera can reuse throughout this stream.
    err = queue_capture_buffers(capture);
    // Check that the buffers are ready before starting capture.
    if (err != ESP_OK)
    {
        // Pass back the reason the buffers could not be set up.
        return err;
    }

    // Match the pixel layout, size, and amount of color detail from the camera.
    capture->jpeg_format = (jpeg_encode_cfg_t)
    {
        .src_type = use_yuv422 ? JPEG_ENCODE_IN_FORMAT_YUV422 : JPEG_ENCODE_IN_FORMAT_YUV420,
        .sub_sample = use_yuv422 ? JPEG_DOWN_SAMPLING_YUV422 : JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = JPEG_QUALITY,
        .width = mode.width,
        .height = mode.height,
    };

    // Give a disconnected camera a short time to recover before closing the stream.
    struct timeval timeout = { .tv_sec = 2 };
    // Set the time limit for waiting on the next raw picture.
    if (ioctl(capture->fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) < 0)
    {
        // Stop if the camera cannot set a limit on the wait.
        return ESP_FAIL;
    }

    // Tell the ISP task to reapply manual gains for the new capture session.
    portENTER_CRITICAL(&mode_lock);
    ++capture_generation;
    const bool focus_available = requested_controls.focus_available;
    portEXIT_CRITICAL(&mode_lock);
    if (focus_available)
    {
        // Refit the focus measurement windows when the picture size changes.
        const esp_err_t focus_err = set_focus_windows(mode);
        if (focus_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Focus window setup failed (%s)", esp_err_to_name(focus_err));
        }
    }

    // Tell the driver to start filling the frame buffers.
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // Check that the camera started before sending any pictures.
    if (ioctl(capture->fd, VIDIOC_STREAMON, &type) < 0)
    {
        // Return the camera error so the stream can be closed cleanly.
        return ESP_FAIL;
    }

    // Remember to stop capture again when the viewer leaves.
    capture->streaming = true;
    // Show the picture size in the UART log.
    ESP_LOGI(TAG, "Capture started: %ux%u JPEG", mode.width, mode.height);
    // Tell the caller the camera is ready.
    return ESP_OK;
}

/* Frame capture and JPEG worker */

/*
 * @brief Wait for the camera to fill a frame buffer.
 * @param capture Running camera.
 * @param frame Output for the buffer number and the bytes it holds.
 * @return ESP_OK when a buffer is ready.
*/
static esp_err_t dequeue_frame(const capture_t *capture, struct v4l2_buffer *frame)
{
    // Use the same kind of buffer the driver already filled.
    *frame = (struct v4l2_buffer)
    {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    // Wait for the next picture and make sure its buffer number exists.
    if (ioctl(capture->fd, VIDIOC_DQBUF, frame) < 0 || frame->index >= CAPTURE_BUFFER_COUNT)
    {
        // Stop streaming if the camera cannot give us a frame.
        return ESP_FAIL;
    }
    // The caller owns this frame until it gives the buffer back to the camera.
    return ESP_OK;
}

/*
 * @brief Capture one frame, encode it into the assigned slot, and return the raw buffer.
 * @param capture Running camera and JPEG encoder.
 * @param output JPEG buffer owned by the encoding task.
 * @param capture_wait_us Optional place to add the time spent waiting for a frame.
 * @param jpeg_us Optional place to add the time spent encoding the frame.
 * @return ESP_OK when a JPEG is ready to send.
*/
static esp_err_t encode_frame(const capture_t *capture, jpeg_frame_t *output,
                              int64_t *capture_wait_us, int64_t *jpeg_us)
{
    // Time this frame if it is part of the startup sample.
    const int64_t start = capture_wait_us != NULL ? esp_timer_get_time() : 0;
    // Leave room for the details of the captured picture.
    struct v4l2_buffer frame;
    // Take a buffer while the other task can still be sending a JPEG.
    esp_err_t err = dequeue_frame(capture, &frame);
    // Stop if the camera did not return a usable buffer.
    if (err != ESP_OK)
    {
        // Pass back the capture error.
        return err;
    }

    // Read the board's UTC clock as close as possible to when the frame arrived.
    struct timeval capture_time;
    // Use zero until NTP has set a plausible Unix time.
    output->timestamp_ns = 0;
    // A clock from before 2021 indicates that it has not been set.
    if (gettimeofday(&capture_time, NULL) == 0 && capture_time.tv_sec >= 1609459200)
    {
        // Convert seconds and microseconds to nanoseconds for the ROS 2 header.
        output->timestamp_ns = (uint64_t)capture_time.tv_sec * 1000000000ULL
                               + (uint64_t)capture_time.tv_usec * 1000;
    }

    // Mark the end of the wait and the start of the encoding work.
    const int64_t jpeg_start = capture_wait_us != NULL ? esp_timer_get_time() : 0;
    // Check that the camera wrote enough data for the whole picture.
    if (frame.bytesused >= capture->frame_size)
    {
        // Let the hardware read directly from the camera buffer and write to the JPEG slot.
        err = jpeg_encoder_process(capture->encoder, &capture->jpeg_format,
                                   capture->frames[frame.index], capture->frame_size,
                                   output->data, output->capacity, &output->size);
    }
    else
    {
        // A short frame cannot be passed to the encoder.
        err = ESP_FAIL;
    }

    // Account for each stage while taking the startup sample.
    if (capture_wait_us != NULL)
    {
        // Count the wait for a complete picture.
        *capture_wait_us += jpeg_start - start;
        // Count the JPEG encoder time.
        *jpeg_us += esp_timer_get_time() - jpeg_start;
    }

    // Let the camera fill this raw buffer again as soon as the JPEG is ready.
    if (ioctl(capture->fd, VIDIOC_QBUF, &frame) < 0)
    {
        // Stop if the driver could not take back its buffer.
        return ESP_FAIL;
    }
    // Pass back the result of making the JPEG.
    return err;
}

/*
 * @brief Encode JPEGs while the HTTP task sends previously finished frames.
 * @param arg Camera, encoder, and shared buffer queues for this viewer.
*/
static void jpeg_worker(void *arg)
{
    // Use the capture state until the viewer asks this task to stop.
    const capture_t *capture = arg;
    // Keep separate totals for waiting and encoding.
    int64_t capture_wait_us = 0;
    int64_t jpeg_us = 0;
    // Stop taking measurements after the startup sample.
    unsigned sample_count = 0;

    for (;;)
    {
        // Wait for a JPEG slot the sender no longer needs.
        jpeg_frame_t *output = NULL;
        // The queue also carries a null pointer when the viewer asks us to stop.
        xQueueReceive(capture->available, &output, portMAX_DELAY);
        // Stop without touching further frames if asked to close.
        if (output == NULL)
        {
            break;
        }

        // Only time the first few successful frames.
        const bool measure = sample_count < TIMING_FRAME_COUNT;
        // Encode into this slot while the other slot can be sent.
        const esp_err_t err = encode_frame(capture, output,
                                           measure ? &capture_wait_us : NULL,
                                           measure ? &jpeg_us : NULL);
        // Stop the worker if capture or encoding failed.
        if (err != ESP_OK)
        {
            // Put the reason on UART before closing the stream.
            ESP_LOGE(TAG, "Could not encode frame (%s)", esp_err_to_name(err));
            break;
        }
        // Hand the ready JPEG to the sender. The queue can hold every slot.
        xQueueSend(capture->ready, &output, 0);
        // Report the average once the startup sample is complete.
        if (measure && ++sample_count == TIMING_FRAME_COUNT)
        {
            // Keep this sample on UART so throughput can be checked on the board.
            ESP_LOGI(TAG, "Average over %u frames: capture wait %" PRIi64 " us, JPEG %" PRIi64 " us",
                     sample_count, capture_wait_us / TIMING_FRAME_COUNT, jpeg_us / TIMING_FRAME_COUNT);
        }
    }

    // Wake the sender if the worker stopped because of an error.
    jpeg_frame_t *stop = NULL;
    // There is room for a stop message even if every finished buffer is waiting.
    xQueueSend(capture->ready, &stop, 0);
    // Tell the owner it can now free the buffers, queues, and camera.
    xSemaphoreGive(capture->encoder_done);
    // End this task without touching the camera state again.
    vTaskDelete(NULL);
}

/*
 * @brief Give the encoder and sender two slots to use independently.
 * @param capture Running camera with its JPEG settings prepared.
 * @return ESP_OK when the worker is running.
*/
static esp_err_t start_jpeg_worker(capture_t *capture)
{
    // Limit the time spent encoding a bad frame.
    const jpeg_encode_engine_cfg_t engine = { .timeout_ms = 250 };
    // Start the chip's built-in encoder.
    esp_err_t err = jpeg_new_encoder_engine(&engine, &capture->encoder);
    // Leave the camera ready for cleanup if the encoder could not start.
    if (err != ESP_OK)
    {
        return err;
    }

    // The extra queue entry lets a stop message through without waiting for a frame.
    capture->available = xQueueCreate(JPEG_BUFFER_COUNT + 1, sizeof(jpeg_frame_t *));
    // Keep completed pictures in the order they were captured.
    capture->ready = xQueueCreate(JPEG_BUFFER_COUNT + 1, sizeof(jpeg_frame_t *));
    // Let the task say when it has finished with the buffers.
    capture->encoder_done = xSemaphoreCreateBinary();
    // Check that all the parts for handing buffers between the tasks are ready.
    if (capture->available == NULL || capture->ready == NULL || capture->encoder_done == NULL)
    {
        // The caller can free whichever parts were created.
        return ESP_ERR_NO_MEM;
    }

    // Use memory the JPEG hardware can write to directly.
    const jpeg_encode_memory_alloc_cfg_t memory = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    // Make one reusable slot for each possible frame in flight.
    for (unsigned i = 0; i < JPEG_BUFFER_COUNT; ++i)
    {
        // Fill in this slot before giving it to the worker.
        jpeg_frame_t *slot = &capture->jpeg_frames[i];
        // Leave enough room even when the scene produces a large JPEG.
        slot->data = jpeg_alloc_encoder_mem(capture->frame_size, &memory, &slot->capacity);
        // Stop if there is not enough memory for both slots.
        if (slot->data == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        // This slot is empty, so the worker can take it when it starts.
        xQueueSend(capture->available, &slot, 0);
    }

    // Let FreeRTOS schedule encoding independently of the HTTP sender.
    if (xTaskCreate(jpeg_worker, "camera_jpeg", 4096, capture, tskIDLE_PRIORITY + 5, NULL) != pdPASS)
    {
        // The caller still owns every buffer and can free them.
        return ESP_ERR_NO_MEM;
    }
    // Remember to wait for the worker during cleanup.
    capture->encoder_running = true;
    // Tell the caller the worker can now supply JPEGs.
    return ESP_OK;
}

/* HTTP output */

/*
 * @brief Write the header and payload of a JPEG frame.
 * @param context HTTP connection to the viewer.
 * @param data Bytes of the picture.
 * @param length Number of bytes in the picture.
 * @param timestamp_ns Capture time, unused by the HTTP preview.
 * @return ESP_OK if both parts were sent.
*/
static esp_err_t send_frame(void *context, const uint8_t *data, uint32_t length, uint64_t timestamp_ns)
{
    // Use the HTTP connection passed in by the stream handler.
    httpd_req_t *request = context;
    // The multipart JPEG format carries only the picture.
    (void)timestamp_ns;
    // Make room for the boundary and frame length.
    char header[96];
    // Tell the viewer how many bytes to read for this JPEG.
    const int count = snprintf(header, sizeof(header),
                               "\r\n--frame\r\nContent-Type: image/jpeg\r\n"
                               "Content-Length: %" PRIu32 "\r\n\r\n", length);
    // Stop if the whole header did not fit.
    if (count < 0 || (size_t)count >= sizeof(header))
    {
        return ESP_FAIL;
    }

    // Give the viewer the size and format before sending the picture.
    const esp_err_t err = httpd_resp_send_chunk(request, header, count);
    // Stop if the viewer has already gone away.
    if (err != ESP_OK)
    {
        return err;
    }
    // Send straight from the buffer while it belongs to the sender.
    return httpd_resp_send_chunk(request, (const char *)data, length);
}

/*
 * @brief Send frames until the viewer leaves or UART requests a different size.
 * @param output Transport callbacks for this stream.
 * @param capture Camera and JPEG worker for the active size.
 * @param mode Current picture size.
 * @return ESP_OK when capture should restart at a new size, or a stream error.
*/
static esp_err_t stream_frames(const camera_output_t *output, const capture_t *capture, camera_mode_t mode)
{
    // Time the sender separately because its work overlaps the JPEG worker.
    int64_t send_us = 0;
    // Keep just one sample after each change of resolution.
    unsigned sample_count = 0;

    for (;;)
    {
        // Stop capture when the transport has nobody to receive pictures.
        if (output->ready != NULL && !output->ready(output->context))
        {
            return ESP_ERR_INVALID_STATE;
        }
        // Check for a size change between complete pictures.
        const camera_mode_t next = camera_stream_get_mode();
        // Give the caller a chance to stop the old camera before it opens the new size.
        if (mode.width != next.width || mode.height != next.height)
        {
            return ESP_OK;
        }
        // Wait for a finished JPEG without getting in the way of the encoder.
        jpeg_frame_t *slot = NULL;
        // The worker sends a null pointer if it could not keep running.
        xQueueReceive(capture->ready, &slot, portMAX_DELAY);
        // Stop if the worker sent its end message.
        if (slot == NULL)
        {
            return ESP_FAIL;
        }
        // Mark the start of the write while there are still samples to take.
        const bool measure = sample_count < TIMING_FRAME_COUNT;
        // Start timing after receiving a finished JPEG to exclude time spent waiting.
        const int64_t send_start = measure ? esp_timer_get_time() : 0;
        // Send this JPEG while the worker can write to the other slot.
        const esp_err_t err = output->send(output->context, slot->data, slot->size, slot->timestamp_ns);
        // Add the time spent on this write.
        if (measure)
        {
            send_us += esp_timer_get_time() - send_start;
        }
        // The send has finished reading the slot, so the encoder can reuse it.
        xQueueSend(capture->available, &slot, 0);
        // Stop if the viewer disconnected.
        if (err != ESP_OK)
        {
            return err;
        }
        // Print the average TCP time after gathering the full sample.
        if (measure && ++sample_count == TIMING_FRAME_COUNT)
        {
            ESP_LOGI(TAG, "Average over %u frames: transport send %" PRIi64 " us",
                     sample_count, send_us / TIMING_FRAME_COUNT);
        }
    }
}

/*
 * @brief Serve one viewer and reopen capture if UART selects a new size.
 * @param request HTTP connection to the viewer.
 * @return An HTTP or camera error when the connection ends.
*/
static esp_err_t stream_handler(httpd_req_t *request)
{
    // Use the shared JPEG sender with an HTTP connection as its output.
    const camera_output_t output = { .send = send_frame, .context = request };
    // Tell the viewer to expect a series of separately framed pictures.
    esp_err_t err = httpd_resp_set_type(request, STREAM_TYPE);
    // Remember whether capture was able to start on this connection.
    bool started = false;

    while (err == ESP_OK)
    {
        // Get the latest settings each time a stream opens or changes size.
        const camera_mode_t mode = camera_stream_get_mode();
        // Use an empty state so cleanup can also run after partial setup.
        capture_t capture = { .fd = -1 };
        // Open the camera at the requested size.
        err = open_capture(&capture, mode);
        // Start the encoder after the camera is ready.
        if (err == ESP_OK)
        {
            err = start_jpeg_worker(&capture);
        }
        // Stream at this size until the viewer leaves or UART changes it.
        if (err == ESP_OK)
        {
            started = true;
            err = stream_frames(&output, &capture, mode);
        }
        else
        {
            // Explain a setup failure on UART.
            ESP_LOGE(TAG, "Could not start capture (%s)", esp_err_to_name(err));
        }
        // Stop the worker and return all buffers before reopening or leaving.
        close_capture(&capture);
        // Send a readable error if capture never began on this connection.
        if (!started)
        {
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Camera could not start. Check the UART log.");
        }
    }
    // Let the HTTP server close the connection.
    return err;
}

/* Public API */

/*
 * @brief Read the requested mode without mixing values from two commands.
 * @return A copy of the current request, or all zeros before startup.
*/
camera_mode_t camera_stream_get_mode(void)
{
    // Wait if the other task is changing the settings.
    portENTER_CRITICAL(&mode_lock);
    // Copy all three values together.
    const camera_mode_t mode = requested_mode;
    // Allow the other task to use the settings again.
    portEXIT_CRITICAL(&mode_lock);
    // Give back a copy the caller can use on its own.
    return mode;
}

/*
 * @brief Give the camera new settings to use between frames.
 * @param mode New picture size.
 * @return ESP_OK if accepted or an ESP-IDF error code.
*/
esp_err_t camera_stream_set_mode(camera_mode_t mode)
{
    // Make sure the camera has finished starting.
    if (!video_ready)
    {
        // There are no camera settings to change before startup.
        return ESP_ERR_INVALID_STATE;
    }
    // Check that the sensor can give us this size.
    if (find_sensor_mode(mode) == NULL)
    {
        // Keep the current mode if the request cannot be used.
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Keep the viewer from reading halfway through the change.
    portENTER_CRITICAL(&mode_lock);
    // Store the whole request for the streaming task.
    requested_mode = mode;
    // Let the viewer read the new settings.
    portEXIT_CRITICAL(&mode_lock);
    // Tell the caller that the new settings were accepted.
    return ESP_OK;
}

/*
 * @brief Free the resources used by startup if the server cannot be started.
*/
static void release_video(void)
{
    // Keep other tasks from reading partly cleared settings.
    portENTER_CRITICAL(&mode_lock);
    // Show that there is no camera mode in use.
    requested_mode = (camera_mode_t){0};
    // Let the other tasks read the cleared settings.
    portEXIT_CRITICAL(&mode_lock);
    // Give back the memory used by the available mode list.
    free(sensor_modes);
    // Clear the address so startup can try again.
    sensor_modes = NULL;
    // Forget how many modes were in the list.
    sensor_mode_count = 0;
    // Allow startup to try again after giving back the hardware.
    video_ready = false;
    // Give back the hardware when there is no server to use it.
    esp_video_deinit_with_flags(VIDEO_DEVICES);
}

/*
 * @brief Detect the camera and load its supported modes.
 * @param config Camera wiring and initial resolution.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
static esp_err_t init_camera(const camera_config_t *config)
{
    // Check that the caller supplied the camera wiring.
    if (config == NULL)
    {
        // Stop before trying to use missing settings.
        return ESP_ERR_INVALID_ARG;
    }

    // Check that there is only one owner of the camera.
    if (video_ready)
    {
        // Keep the running server if setup is called again.
        return ESP_ERR_INVALID_STATE;
    }

    // Describe the small control connection that sets up the camera.
    const esp_video_init_csi_config_t csi =
    {
        // Set up the camera's control wires before opening the picture port.
        .sccb_config =
        {
            // Have the video driver set up the control bus.
            .init_sccb = true,
            // Use the pins wired to the camera connector.
            .i2c_config =
            {
                // Use the first controller for the two control wires.
                .port = 0,
                // Set the pin that times the control messages.
                .scl_pin = config->scl_gpio,
                // Set the pin that carries the control messages.
                .sda_pin = config->sda_gpio,
            },
            // Use the standard speed for compatible camera modules.
            .freq = 100000,
        },
        // Pass along the reset pin if the board has one.
        .reset_pin = config->reset_gpio,
        // Pass along the power control pin if the board has one.
        .pwdn_pin = config->power_down_gpio,
    };
    // Use the board's camera connection for this video device.
    const esp_video_init_config_t video =
    {
        .csi = &csi,
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
        // The IMX708 lens motor shares the camera's control wires.
        .cam_motor = &(const esp_video_init_cam_motor_config_t)
        {
            .sccb_config = csi.sccb_config,
            .reset_pin = -1,
            .pwdn_pin = -1,
            .signal_pin = -1,
        },
#endif
    };
    // Let the camera driver find the sensor and set up the color processing hardware.
    esp_err_t err = esp_video_init_with_flags(&video, VIDEO_DEVICES);
    // Check whether the video hardware started before opening a server.
    if (err != ESP_OK)
    {
        // Report the camera startup failure.
        return err;
    }

    // Read the compatible formats and check the startup settings.
    camera_mode_t mode = config->mode;
    // Keep the list so UART can check settings without interrupting the camera.
    err = load_sensor_modes(&mode);
    // Stop startup if the camera cannot provide the requested mode.
    if (err != ESP_OK)
    {
        // Give back the camera and any memory used to read its modes.
        release_video();
        // Report why the camera setup could not continue.
        return err;
    }
    // Keep the mode protected when it is shared with other tasks.
    portENTER_CRITICAL(&mode_lock);
    // Set the starting mode before the HTTP server can read it.
    requested_mode = mode;
    // Let other tasks read the ready settings.
    portEXIT_CRITICAL(&mode_lock);
    // Show that the camera is ready for a transport to use.
    video_ready = true;
    // Tell the caller the sensor was set up.
    return ESP_OK;
}

/*
 * @brief Set up the camera and start its JPEG stream server.
 * @param config Camera wiring and HTTP port.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_stream_start(const camera_config_t *config)
{
    // Make sure there is an HTTP port to listen on.
    if (config == NULL || config->port == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Set up the same camera and encoder used by other transports.
    esp_err_t err = init_camera(config);
    // Stop if the camera is not ready.
    if (err != ESP_OK)
    {
        return err;
    }
    // Start from the SDK's usual HTTP server settings.
    httpd_config_t server = HTTPD_DEFAULT_CONFIG();
    // Listen on the port the viewer will connect to.
    server.server_port = config->port;
    // Give the camera and network calls working room in the server task.
    server.stack_size = 6144;
    // Drop a stuck connection promptly so the camera becomes available again.
    server.send_wait_timeout = 3;
    // Start listening for the viewer over the network.
    err = httpd_start(&stream_server, &server);
    // Check whether the server started before adding the stream path.
    if (err != ESP_OK)
    {
        // Release the camera hardware when there is no server to use it.
        release_video();
        // Pass back the server setup error.
        return err;
    }

    // Connect the stream path to the function that sends live JPEGs.
    const httpd_uri_t stream =
    {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
    };
    // Let the viewer ask for live JPEGs.
    err = httpd_register_uri_handler(stream_server, &stream);
    // Stop both services if the viewer cannot reach the stream path.
    if (err != ESP_OK)
    {
        // Close the server before freeing the camera it uses.
        httpd_stop(stream_server);
        // Allow a later call to try starting the server again.
        stream_server = NULL;
        // Give back the camera hardware.
        release_video();
        // Report why the stream could not be made available.
        return err;
    }

    // Print the path to use once Ethernet has an address.
    ESP_LOGI(TAG, "MJPEG ready on port %u at /stream", config->port);
    // Tell the caller that the server is ready.
    return ESP_OK;
}

/*
 * @brief Wait for the transport, then run the shared JPEG sender in the background.
 * @param arg Output callbacks and their context.
*/
static void publisher_task(void *arg)
{
    // Use the transport selected by the application.
    const camera_output_t *output = arg;
    for (;;)
    {
        // Leave the camera idle until someone is ready to receive frames.
        if (output->ready == NULL || output->ready(output->context))
        {
            // Read the current resolution before opening the sensor.
            const camera_mode_t mode = camera_stream_get_mode();
            // Use an empty state so cleanup works even if setup fails.
            capture_t capture = { .fd = -1 };
            // Prepare the capture buffers for the current resolution.
            esp_err_t err = open_capture(&capture, mode);
            // Start the parallel encoder only when capture is ready.
            if (err == ESP_OK)
            {
                err = start_jpeg_worker(&capture);
            }
            // Send pictures until the transport or resolution changes.
            if (err == ESP_OK)
            {
                err = stream_frames(output, &capture, mode);
            }
            // Wait for the encoder to finish before giving its memory back.
            close_capture(&capture);
            // Keep normal disconnects quiet and log other stream errors.
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(TAG, "Camera publisher paused (%s)", esp_err_to_name(err));
            }
            // Reopen straight away if UART changed only the resolution.
            if (err == ESP_OK)
            {
                continue;
            }
        }
        // Leave time for other work while the transport or camera recovers.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/*
 * @brief Start a background JPEG stream using the supplied transport callbacks.
 * @param config Camera wiring and default resolution.
 * @param output Callbacks and context used by the sender.
 * @return ESP_OK on success or an ESP-IDF error code.
*/
esp_err_t camera_publish_start(const camera_config_t *config, const camera_output_t *output)
{
    // Make sure the sender has somewhere to deliver the JPEGs.
    if (output == NULL || output->send == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Detect the camera and prepare the list of resolutions.
    esp_err_t err = init_camera(config);
    // Stop if the camera could not start.
    if (err != ESP_OK)
    {
        return err;
    }
    // Keep a copy so the sender task can use it after startup returns.
    publisher_output = *output;
    // Run the sender alongside the encoder and the UART console.
    if (xTaskCreate(publisher_task, "camera_send", 6144, &publisher_output,
                    tskIDLE_PRIORITY + 5, NULL) != pdPASS)
    {
        // Give back the camera if its sender could not be started.
        release_video();
        return ESP_ERR_NO_MEM;
    }
    // The sender will begin capturing when its transport is ready.
    return ESP_OK;
}
