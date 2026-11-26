/*
 * @file camera_zenoh.c
 * @brief Publish ROS 2 compressed images from the camera over Zenoh.
 * @author Ajay Varghese
 * @date 2026-11-25
*/

/* Includes */

#include "camera_zenoh.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "zenoh-pico.h"

/* ROS 2 uses xxHash3 once when it creates a publisher's ID. */
#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#include "xxhash.h"

/* Publisher state */

/* Jazzy rmw_zenoh type, interface hash, and reliable keep-last-one QoS. */
#define ROS_IMAGE_TYPE "sensor_msgs::msg::dds_::CompressedImage_"
#define ROS_IMAGE_HASH "RIHS01_15640771531571185e2efc8a100baf923961a4d15d5569652e6cb6691e8e371a"
#define ROS_IMAGE_QOS "::,1:,:,:,,"

static const char *TAG = "camera_zenoh";
static const char *SETTINGS_NAMESPACE = "camera_zenoh";
static const char *SETTINGS_KEY = "settings_v1";
static camera_zenoh_settings_t saved;
static camera_zenoh_settings_t active;
static bool configured;
static bool session_open;
static int64_t next_open_us;
static z_owned_session_t session;
static z_owned_publisher_t publisher;
static z_owned_liveliness_token_t node_token;
static z_owned_liveliness_token_t publisher_token;
static uint32_t ros_domain;
static uint64_t next_sequence;
static uint8_t publisher_gid[16];

/* CDR header before the JPEG, padded to 4-byte boundaries. */
static uint8_t image_header[sizeof(active.id) + 32] = {0, 1};
static size_t image_header_size;

/* ROS 2 message format */

/*
 * @brief Write a 32-bit number in little-endian byte order.
 * @param output Where to write four bytes.
 * @param value Number to write.
*/
static void write_le32(uint8_t *output, uint32_t value)
{
    // Store the least significant byte first as required by the CDR header.
    for (unsigned i = 0; i < 4; ++i)
    {
        output[i] = (uint8_t)(value >> (8 * i));
    }
}

/*
 * @brief Write a 64-bit number in little-endian byte order.
 * @param output Where to write eight bytes.
 * @param value Number to write.
*/
static void write_le64(uint8_t *output, uint64_t value)
{
    // Write the lower half before the upper half.
    write_le32(output, (uint32_t)value);
    write_le32(output + 4, (uint32_t)(value >> 32));
}

/*
 * @brief Fill the fixed part of the CompressedImage CDR header once at startup.
*/
static void prepare_image_header(void)
{
    // The first four bytes already mark little-endian CDR. Leave space for the timestamp.
    size_t next = 12;
    // CDR strings include their terminating zero in the length.
    const size_t id_size = strlen(active.id) + 1;
    write_le32(image_header + next, id_size);
    next += 4;
    // Use the camera ID as the ROS 2 header frame_id.
    memcpy(image_header + next, active.id, id_size);
    next += id_size;
    // Pad before writing the next four-byte string length.
    next = (next + 3) & ~(size_t)3;
    static const char format[] = "jpeg";
    write_le32(image_header + next, sizeof(format));
    next += 4;
    memcpy(image_header + next, format, sizeof(format));
    next += sizeof(format);
    // Leave space for the four-byte JPEG length before the frame bytes.
    image_header_size = ((next + 3) & ~(size_t)3) + 4;
}

/* Settings validation */

/*
 * @brief Check a ROS 2 relative name before using it in a Zenoh key.
 * @param name Camera ID or topic suffix.
 * @param allow_slashes True when more than one name segment is allowed.
 * @return True if all segments contain valid ROS 2 name characters.
*/
static bool valid_name(const char *name, bool allow_slashes)
{
    // The first character of each segment has to be a letter or underscore.
    bool first = true;
    // Check the name one character at a time.
    for (const unsigned char *ch = (const unsigned char *)name; *ch != '\0'; ++ch)
    {
        // A slash can separate two complete topic segments.
        if (*ch == '/' && allow_slashes && !first)
        {
            first = true;
            continue;
        }
        // Check for letters and underscores, and allow digits after the first character.
        if (!isalpha(*ch) && *ch != '_' && (first || !isdigit(*ch)))
        {
            return false;
        }
        // The current segment now has at least one valid character.
        first = false;
    }
    // Reject an empty name or a slash at the end.
    return !first;
}

/*
 * @brief Check a router locator before saving it to flash.
 * @param endpoint Locator in the form tcp/IPv4:port.
 * @return True if the address and port can be used for a connection.
*/
static bool valid_endpoint(const char *endpoint)
{
    // The board connects to the router over TCP.
    if (strncmp(endpoint, "tcp/", 4) != 0)
    {
        return false;
    }
    // Find the separator between the IP address and port.
    const char *separator = strchr(endpoint + 4, ':');
    // Make room for one dotted IPv4 address including its end marker.
    char address[INET_ADDRSTRLEN];
    // Reject a missing or overlong address.
    if (separator == NULL || (size_t)(separator - (endpoint + 4)) >= sizeof(address))
    {
        return false;
    }
    // Copy just the address before checking its parts.
    const size_t address_length = separator - (endpoint + 4);
    memcpy(address, endpoint + 4, address_length);
    address[address_length] = '\0';
    // Require an IPv4 address that can identify a router.
    struct in_addr ip;
    // Require a numeric port after the colon.
    const char *port = separator + 1;
    if (inet_pton(AF_INET, address, &ip) != 1 || ip.s_addr == 0
            || *port == '\0' || strspn(port, "0123456789") != strlen(port))
    {
        return false;
    }
    // Check that the port number fits TCP's 16-bit port field.
    const unsigned long number = strtoul(port, NULL, 10);
    return number > 0 && number <= UINT16_MAX;
}

/*
 * @brief Validate all settings including strings loaded from flash.
 * @param settings Values to check.
 * @return True if every field is safe to use.
*/
static bool valid_settings(const camera_zenoh_settings_t *settings)
{
    // Make sure every field has a string end before calling the text checks.
    return memchr(settings->id, '\0', sizeof(settings->id)) != NULL
        && memchr(settings->endpoint, '\0', sizeof(settings->endpoint)) != NULL
        && memchr(settings->topic, '\0', sizeof(settings->topic)) != NULL
        && valid_name(settings->id, false) && valid_name(settings->topic, true)
        && valid_endpoint(settings->endpoint);
}

/* Saved camera identity and endpoint */

/*
 * @brief Load saved settings and use the Ethernet MAC for automatic IDs.
 * @param defaults Settings for a board without a saved configuration.
 * @param domain_id ROS 2 domain to use for discovery and image messages.
 * @return ESP_OK when settings are ready.
*/
esp_err_t camera_zenoh_configure(const camera_zenoh_settings_t *defaults, uint32_t domain_id)
{
    // Check the default values before working with flash.
    if (defaults == NULL || !valid_settings(defaults) || configured)
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Start with the values from the application.
    saved = *defaults;
    // Try reading an existing settings record.
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        // Read into a separate record so bad flash data cannot replace the defaults.
        camera_zenoh_settings_t stored;
        size_t size = sizeof(stored);
        err = nvs_get_blob(handle, SETTINGS_KEY, &stored, &size);
        // The record is now in memory and the flash handle can be closed.
        nvs_close(handle);
        // Accept only a complete, valid record.
        if (err == ESP_OK && size == sizeof(stored) && valid_settings(&stored))
        {
            saved = stored;
        }
    }
    // Use the saved settings for this boot.
    active = saved;
    // Derive a stable ID when the board is set to automatic naming.
    if (strcmp(saved.id, "auto") == 0)
    {
        // The same Ethernet MAC is used regardless of the current IP address.
        uint8_t mac[6];
        err = esp_read_mac(mac, ESP_MAC_ETH);
        if (err != ESP_OK)
        {
            return err;
        }
        // Use the device-specific part of the MAC in a valid ROS 2 name.
        snprintf(active.id, sizeof(active.id), "camera_%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    // Use the same domain for the graph entries and the image data.
    ros_domain = domain_id;
    // Prepare the parts of the image message that are the same for every frame.
    prepare_image_header();
    // Let the application show the ID and endpoint even while the network is down.
    ESP_LOGI(TAG, "ID %s, router %s, ROS domain %" PRIu32 ", topic /%s/%s",
             active.id, active.endpoint, ros_domain, active.id, active.topic);
    // The sender may now open the Zenoh session.
    configured = true;
    return ESP_OK;
}

/*
 * @brief Get the settings in use for this boot.
 * @return Current settings, or NULL before configuration.
*/
const camera_zenoh_settings_t *camera_zenoh_settings(void)
{
    // The active record stays unchanged until restart.
    return configured ? &active : NULL;
}

/*
 * @brief Validate and save one changed setting for the next boot.
 * @param field Setting being changed.
 * @param value New value from the caller.
 * @return ESP_OK after the record is committed to flash.
*/
esp_err_t camera_zenoh_save(camera_zenoh_setting_t field, const char *value)
{
    // Require the defaults and current settings to be loaded first.
    if (!configured || value == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // Keep the values from the other fields when one setting changes.
    camera_zenoh_settings_t next = saved;
    // Find the field and its available space.
    char *destination;
    size_t capacity;
    switch (field)
    {
        case CAMERA_ZENOH_ID:
            destination = next.id;
            capacity = sizeof(next.id);
            break;
        case CAMERA_ZENOH_ENDPOINT:
            destination = next.endpoint;
            capacity = sizeof(next.endpoint);
            break;
        case CAMERA_ZENOH_TOPIC:
            destination = next.topic;
            capacity = sizeof(next.topic);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    // Reject long text instead of saving a shortened version.
    if (strlen(value) >= capacity)
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Clear the old field and copy the new text.
    memset(destination, 0, capacity);
    memcpy(destination, value, strlen(value));
    // Make sure the combined settings still make sense.
    if (!valid_settings(&next))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Open the settings area for writing.
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    // Store all three fields in one record.
    err = nvs_set_blob(handle, SETTINGS_KEY, &next, sizeof(next));
    // Commit the record before telling the console it can restart.
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    // Return the flash handle after the write attempt.
    nvs_close(handle);
    // Keep the newly saved record for later writes without changing the live session.
    if (err == ESP_OK)
    {
        saved = next;
    }
    return err;
}

/* ROS 2 discovery and Zenoh publisher */

/*
 * @brief Advertise the camera node and publisher to rmw_zenoh's ROS graph.
 * @return True if both discovery tokens were declared.
*/
static bool announce_ros_graph(void)
{
    // ROS discovery includes the ID of the session that owns these entries.
    const z_id_t session_id = z_info_zid(z_loan(session));
    z_owned_string_t id;
    if (z_id_to_string(&session_id, &id) < 0)
    {
        return false;
    }
    // Copy the session ID so the Zenoh string can be returned.
    char zid[33];
    const int id_length = snprintf(zid, sizeof(zid), "%.*s", (int)z_string_len(z_loan(id)),
                                   z_string_data(z_loan(id)));
    z_drop(z_move(id));
    if (id_length <= 0 || id_length >= sizeof(zid))
    {
        return false;
    }

    // ROS graph keys use percent signs where the ROS name contains a slash.
    char mangled_topic[sizeof(active.id) + sizeof(active.topic) + 1];
    snprintf(mangled_topic, sizeof(mangled_topic), "%%%s%%%s", active.id, active.topic);
    for (char *ch = mangled_topic; *ch != '\0'; ++ch)
    {
        // Replace separators within a custom topic suffix as well.
        if (*ch == '/')
        {
            *ch = '%';
        }
    }

    // Advertise node 0 in the root namespace, with the camera ID as its name.
    char key[352];
    snprintf(key, sizeof(key), "@ros2_lv/%" PRIu32 "/%s/0/0/NN/%%/%%/%s", ros_domain, zid, active.id);
    z_view_keyexpr_t view;
    z_view_keyexpr_from_str_unchecked(&view, key);
    if (z_liveliness_declare_token(z_loan(session), &node_token, z_loan(view), NULL) < 0)
    {
        return false;
    }

    // Advertise the compressed image publisher as entity 1 on that node.
    snprintf(key, sizeof(key), "@ros2_lv/%" PRIu32 "/%s/0/1/MP/%%/%%/%s/%s/%s/%s/%s",
             ros_domain, zid, active.id, mangled_topic, ROS_IMAGE_TYPE, ROS_IMAGE_HASH, ROS_IMAGE_QOS);
    z_view_keyexpr_from_str_unchecked(&view, key);
    if (z_liveliness_declare_token(z_loan(session), &publisher_token, z_loan(view), NULL) < 0)
    {
        // Remove the node if its publisher could not be announced.
        z_drop(z_move(node_token));
        return false;
    }
    // Use the same hash as the ROS graph so received images can identify their publisher.
    const XXH128_hash_t hash = XXH3_128bits(key, strlen(key));
    write_le64(publisher_gid, hash.low64);
    write_le64(publisher_gid + 8, hash.high64);
    next_sequence = 1;
    return true;
}

/*
 * @brief Connect to the TCP router and declare the camera's Zenoh key.
 * @return True if the session and publisher are open.
*/
static bool open_session(void)
{
    // Prepare an explicit connection so the camera works on a direct Ethernet cable.
    z_owned_config_t config;
    if (z_config_default(&config) < 0)
    {
        return false;
    }
    // Connect as a client to the router's configured address and port.
    if (zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, Z_CONFIG_MODE_CLIENT) < 0
            || zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, active.endpoint) < 0)
    {
        z_drop(z_move(config));
        return false;
    }
    // Let the Zenoh session handle the connection in the background.
    if (z_open(&session, z_move(config), NULL) < 0)
    {
        return false;
    }

    // Use the ROS data key for this domain, topic, and message type.
    char key[sizeof(active.id) + sizeof(active.topic) + sizeof(ROS_IMAGE_TYPE) + sizeof(ROS_IMAGE_HASH) + 16];
    snprintf(key, sizeof(key), "%" PRIu32 "/%s/%s/%s/%s", ros_domain, active.id, active.topic,
             ROS_IMAGE_TYPE, ROS_IMAGE_HASH);
    // The name checks guarantee each piece is a valid Zenoh key segment.
    z_view_keyexpr_t expression;
    z_view_keyexpr_from_str_unchecked(&expression, key);
    // Label the payload as CDR because it carries a ROS message around the JPEG.
    z_owned_encoding_t encoding;
    z_encoding_clone(&encoding, z_encoding_application_cdr());
    // Back up the camera sender when the link is busy so frames stay whole.
    z_publisher_options_t options;
    z_publisher_options_default(&options);
    options.encoding = z_move(encoding);
    options.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    // Set up the publisher before handing the camera over to the sender.
    if (z_declare_publisher(z_loan(session), &publisher, z_loan(expression), &options) < 0)
    {
        z_drop(z_move(session));
        return false;
    }
    // Let ROS 2 tools discover the node and its image publisher.
    if (!announce_ros_graph())
    {
        z_drop(z_move(publisher));
        z_drop(z_move(session));
        return false;
    }
    // Show the ROS topic the board is now ready to publish.
    ESP_LOGI(TAG, "Connected to %s, ROS publisher /%s/%s", active.endpoint, active.id, active.topic);
    return true;
}

/*
 * @brief Wait for a subscriber and retry if the router could not be reached.
 * @param context Unused sender callback context.
 * @return True when a matching subscriber is present.
*/
bool camera_zenoh_ready(void *context)
{
    // There is one Zenoh session for this board.
    (void)context;
    // Refuse to open a session until the settings have been loaded.
    if (!configured)
    {
        return false;
    }
    // Retry periodically if the router was not ready at boot.
    if (!session_open && esp_timer_get_time() >= next_open_us)
    {
        session_open = open_session();
        next_open_us = esp_timer_get_time() + 2000000;
        if (!session_open)
        {
            ESP_LOGW(TAG, "Could not connect to %s. Retrying.", active.endpoint);
        }
    }
    // Read the current subscriber count before starting or continuing capture.
    z_matching_status_t status;
    return session_open && z_publisher_get_matching_status(z_loan(publisher), &status) == 0
           && status.matching;
}

/*
 * @brief Send a CompressedImage directly to ROS 2 subscribers over Zenoh.
 * @param context Unused sender callback context.
 * @param data JPEG bytes.
 * @param size Number of bytes in the JPEG.
 * @param timestamp_ns Capture time in Unix nanoseconds, or zero before NTP sync.
 * @return ESP_OK if the frame was sent.
*/
esp_err_t camera_zenoh_send(void *context, const uint8_t *data, uint32_t size, uint64_t timestamp_ns)
{
    // The publisher is shared by all viewers of this board.
    (void)context;
    // Update the timestamp and JPEG length in the prebuilt message header.
    write_le32(image_header + 4, (uint32_t)(timestamp_ns / 1000000000ULL));
    write_le32(image_header + 8, (uint32_t)(timestamp_ns % 1000000000ULL));
    write_le32(image_header + image_header_size - 4, size);

    // Join the small CDR header and the camera's existing JPEG buffer.
    z_owned_bytes_writer_t writer;
    if (z_bytes_writer_empty(&writer) < 0)
    {
        return ESP_ERR_NO_MEM;
    }
    // Borrow the JPEG until the blocking put returns its slot to the encoder.
    z_owned_bytes_t jpeg;
    if (z_bytes_writer_write_all(z_loan_mut(writer), image_header, image_header_size) < 0
            || z_bytes_from_static_buf(&jpeg, data, size) < 0)
    {
        z_drop(z_move(writer));
        return ESP_ERR_NO_MEM;
    }
    // Append the JPEG bytes without copying its pixels into a new buffer.
    if (z_bytes_writer_append(z_loan_mut(writer), z_move(jpeg)) < 0)
    {
        z_drop(z_move(writer));
        return ESP_ERR_NO_MEM;
    }
    z_owned_bytes_t payload;
    z_bytes_writer_finish(z_move(writer), &payload);

    // Include the sequence, source time, and publisher ID used by rmw_zenoh.
    uint8_t metadata[33];
    write_le64(metadata, next_sequence++);
    write_le64(metadata + 8, timestamp_ns);
    metadata[16] = sizeof(publisher_gid);
    memcpy(metadata + 17, publisher_gid, sizeof(publisher_gid));
    z_owned_bytes_t attachment;
    if (z_bytes_copy_from_buf(&attachment, metadata, sizeof(metadata)) < 0)
    {
        z_drop(z_move(payload));
        return ESP_ERR_NO_MEM;
    }
    // Send the complete ROS image and its Zenoh attachment.
    z_publisher_put_options_t options;
    z_publisher_put_options_default(&options);
    options.attachment = z_move(attachment);
    return z_publisher_put(z_loan(publisher), z_move(payload), &options) == 0 ? ESP_OK : ESP_FAIL;
}
