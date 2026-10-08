# Rainlog Wireless Bridge - top-level ESP-IDF project file.
cmake_minimum_required(VERSION 3.16)

# Firmware version (shown on the LCD and in esp_app_desc). Bump manually.
set(PROJECT_VER "1.0.2")
option(RAINLOG_DEBUG_CONSOLE "Enable MeshCore-style serial debug console" OFF)

if(IDF_TARGET STREQUAL "esp32" OR "$ENV{IDF_TARGET}" STREQUAL "esp32")
    set(RADIO_DEFAULT ON)
    set(RADIO_MHZ 433)
else()
    set(RADIO_DEFAULT OFF)
    set(RADIO_MHZ 0)
endif()
option(RAINLOG_RADIO "Enable SX1278 weather radio reception" ${RADIO_DEFAULT})
if(RAINLOG_RADIO AND NOT RADIO_DEFAULT)
    message(FATAL_ERROR "RAINLOG_RADIO requires the LILYGO SX1278 board")
endif()
add_compile_definitions(RAINLOG_RADIO=$<BOOL:${RAINLOG_RADIO}>
                        RAINLOG_RADIO_MHZ=${RADIO_MHZ})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# ESP-IDF dependency locks include the target. Keep ESP32 builds from
# rewriting the default C6 lock, while sharing the vendored components.
if(IDF_TARGET STREQUAL "esp32" OR "$ENV{IDF_TARGET}" STREQUAL "esp32")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.esp32.lock")
endif()
project(rainlog-wireless-bridge)
