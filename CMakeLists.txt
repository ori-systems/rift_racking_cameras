cmake_minimum_required(VERSION 3.22)

# Project configuration
set(IDF_TARGET esp32p4 CACHE STRING "ESP-IDF target")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_BINARY_DIR}/sdkconfig.defaults")

# Use the ROM bootloader and enable IPv4 ping and DHCP time servers.
file(CONFIGURE OUTPUT "${SDKCONFIG_DEFAULTS}" CONTENT [=[
CONFIG_ESPTOOLPY_NO_STUB=y
CONFIG_LWIP_IPV4=y
CONFIG_LWIP_ICMP=y
CONFIG_LWIP_DHCP_GET_NTP_SRV=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_CAMERA_OV5647=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X640_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y
CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE=y
CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE=y
CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y
]=])

# Register the camera dependency in the build directory to keep sources flat.
set(camera_dependencies "${CMAKE_BINARY_DIR}/camera_dependencies")
file(CONFIGURE OUTPUT "${camera_dependencies}/CMakeLists.txt"
     CONTENT "idf_component_register()\n")
file(CONFIGURE OUTPUT "${camera_dependencies}/idf_component.yml" CONTENT [=[
dependencies:
  espressif/esp_video: "2.6.0"
]=])
list(APPEND EXTRA_COMPONENT_DIRS "${camera_dependencies}")

# Components and application
set(COMPONENTS esptool_py esp_eth esp_event esp_netif nvs_flash console
    esp_psram esp_driver_jpeg esp_http_server camera_dependencies)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(racking_cameras)

idf_build_get_property(app EXECUTABLE)
target_sources(${app} PRIVATE main.c ethernet.c time_sync.c camera.c)
