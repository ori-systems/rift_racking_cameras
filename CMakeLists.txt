cmake_minimum_required(VERSION 3.22)

# Project configuration
set(IDF_TARGET esp32p4 CACHE STRING "ESP-IDF target")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_BINARY_DIR}/sdkconfig.defaults")

# Use the ROM bootloader for flashing and enable IPv4 ping replies.
file(CONFIGURE OUTPUT "${SDKCONFIG_DEFAULTS}" CONTENT [=[
CONFIG_ESPTOOLPY_NO_STUB=y
CONFIG_LWIP_IPV4=y
CONFIG_LWIP_ICMP=y
]=])

# Components and application
set(COMPONENTS esptool_py esp_eth esp_event esp_netif nvs_flash console)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(racking_cameras)

idf_build_get_property(app EXECUTABLE)
target_sources(${app} PRIVATE main.c ethernet.c time_sync.c)
