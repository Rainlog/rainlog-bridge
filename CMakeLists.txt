# Rainlog Wireless Bridge - top-level ESP-IDF project file.
cmake_minimum_required(VERSION 3.16)

# Firmware version (shown on the LCD and in esp_app_desc). Bump manually.
set(PROJECT_VER "1.0.0")
option(RAINLOG_DEBUG_CONSOLE "Enable MeshCore-style serial debug console" OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# ESP-IDF dependency locks include the target. Keep ESP32 builds from
# rewriting the default C6 lock, while sharing the vendored components.
if(IDF_TARGET STREQUAL "esp32" OR "$ENV{IDF_TARGET}" STREQUAL "esp32")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.esp32.lock")
endif()
project(rainlog-wireless-bridge)
