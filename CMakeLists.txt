cmake_minimum_required(VERSION 3.22)

# Project configuration
set(IDF_TARGET esp32p4 CACHE STRING "ESP-IDF target")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_BINARY_DIR}/sdkconfig.defaults")

# Build only the driver selected in main.c and keep each sensor's settings apart.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main.c")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/main.c" camera_sensor
     REGEX "^#define[ \t]+CAMERA_SENSOR[ \t]+")
if(camera_sensor MATCHES "^#define[ \t]+CAMERA_SENSOR[ \t]+CAMERA_SENSOR_OV5647[ \t]*$")
    set(camera_sensor_defaults [=[
CONFIG_CAMERA_OV5647=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X640_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y
]=])
elseif(camera_sensor MATCHES "^#define[ \t]+CAMERA_SENSOR[ \t]+CAMERA_SENSOR_IMX708[ \t]*$")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig.imx708")
    set(camera_sensor_defaults [=[
CONFIG_CAMERA_OV5647=n
CONFIG_CAMERA_IMX708=y
CONFIG_CAMERA_IMX708_AUTO_DETECT_MIPI_INTERFACE_SENSOR=y
CONFIG_CAMERA_IMX708_MIPI_IF_FORMAT_INDEX_DEFAULT=0
]=])
    set(imx_dependency "  mushbraindave/esp_cam_sensor_imx: \"0.4.0\"")
    set(imx_tuning "managed_components/mushbraindave__esp_cam_sensor_imx/sensors/imx708/cfg/imx708_default.json")
else()
    message(FATAL_ERROR "Set CAMERA_SENSOR in main.c to CAMERA_SENSOR_OV5647 or CAMERA_SENSOR_IMX708")
endif()

# Use the ROM bootloader and enable IPv4 ping and DHCP time servers.
file(CONFIGURE OUTPUT "${SDKCONFIG_DEFAULTS}" CONTENT [=[
CONFIG_ESPTOOLPY_NO_STUB=y
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_LWIP_IPV4=y
CONFIG_LWIP_ICMP=y
CONFIG_LWIP_DHCP_GET_NTP_SRV=y
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=23040
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE=y
CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE=y
CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y
@camera_sensor_defaults@
]=] @ONLY)

# Register the camera dependency in the build directory to keep sources flat.
set(camera_dependencies "${CMAKE_BINARY_DIR}/camera_dependencies")
file(CONFIGURE OUTPUT "${camera_dependencies}/CMakeLists.txt"
     CONTENT "idf_component_register()\n")
file(CONFIGURE OUTPUT "${camera_dependencies}/idf_component.yml" CONTENT [=[
dependencies:
  espressif/esp_video: "2.6.0"
@imx_dependency@
]=] @ONLY)
list(APPEND EXTRA_COMPONENT_DIRS "${camera_dependencies}")

# Components and application
set(COMPONENTS esptool_py esp_eth esp_event esp_netif nvs_flash console
    esp_psram esp_driver_jpeg esp_http_server camera_dependencies)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(imx_dependency)
    # Load the sensor's tuning for exposure and white balance.
    idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${imx_tuning}" APPEND)
endif()
project(racking_cameras)
if(imx_dependency AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${imx_tuning}")
    message(FATAL_ERROR "Missing IMX708 ISP tuning file: ${imx_tuning}")
endif()

idf_build_get_property(app EXECUTABLE)
target_sources(${app} PRIVATE main.c ethernet.c time_sync.c camera.c camera_zenoh.c ssh_console.c)
target_compile_options(${app} PRIVATE -ffunction-sections -fdata-sections)

# Build the pinned Zenoh-Pico ESP-IDF backend with TCP and large-frame fragmentation.
include(FetchContent)
set(ZP_PLATFORM espidf CACHE STRING "Zenoh-Pico platform" FORCE)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_TESTING OFF)
set(BATCH_UNICAST_SIZE 32768 CACHE STRING "Zenoh-Pico TCP batch size" FORCE)
set(Z_FEATURE_LIVELINESS 1 CACHE STRING "ROS 2 graph discovery" FORCE)
foreach(feature LINK_UDP_MULTICAST MULTICAST_TRANSPORT LINK_UDP_UNICAST SCOUTING
                QUERY QUERYABLE BATCHING)
    set(Z_FEATURE_${feature} 0 CACHE STRING "Unused Zenoh-Pico feature" FORCE)
endforeach()
FetchContent_Declare(zenoh_pico
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-pico.git
    GIT_TAG 1.10.2
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(zenoh_pico)
target_link_libraries(zenohpico_static PUBLIC
    idf::esp_driver_uart idf::esp_hw_support idf::freertos idf::heap idf::lwip)
target_link_libraries(${app} PRIVATE zenohpico::lib)

# Match rmw_zenoh's XXH3-128 publisher identifier without hashing per frame.
FetchContent_Declare(xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.3
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(xxhash)
target_include_directories(${app} PRIVATE "${xxhash_SOURCE_DIR}")

# Build the embeddable SSH server and the cryptography used by its connection.
FetchContent_Declare(wolfssh
    GIT_REPOSITORY https://github.com/wolfSSL/wolfssh.git
    GIT_TAG v1.6.0-stable
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE)
FetchContent_Declare(wolfssl
    GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
    GIT_TAG v5.9.4-stable
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
    SOURCE_SUBDIR wolfcrypt/src)
FetchContent_MakeAvailable(wolfssh wolfssl)

set(wolfssh_sources ssh internal io port log ossh)
set(wolfcrypt_sources aes asn coding ecc hash hmac logging memory random sha256 signature
    sp_c32 sp_int wc_port wolfmath)
list(TRANSFORM wolfssh_sources PREPEND "${wolfssh_SOURCE_DIR}/src/")
list(TRANSFORM wolfcrypt_sources PREPEND "${wolfssl_SOURCE_DIR}/wolfcrypt/src/")
list(TRANSFORM wolfssh_sources APPEND ".c")
list(TRANSFORM wolfcrypt_sources APPEND ".c")
add_library(camera_wolfssh STATIC ${wolfssh_sources} ${wolfcrypt_sources})
target_include_directories(camera_wolfssh SYSTEM PUBLIC
    "${wolfssh_SOURCE_DIR}" "${wolfssl_SOURCE_DIR}")
target_include_directories(camera_wolfssh PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(camera_wolfssh PUBLIC WOLFSSL_USER_SETTINGS)
target_compile_definitions(camera_wolfssh PRIVATE BUILDING_WOLFSSH)
target_compile_options(camera_wolfssh PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_libraries(camera_wolfssh PRIVATE idf::lwip)
target_link_libraries(${app} PRIVATE camera_wolfssh)
