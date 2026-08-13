# Rainlog Wireless Bridge - top-level ESP-IDF project file.
cmake_minimum_required(VERSION 3.16)

# Firmware version (shown on the LCD and in esp_app_desc). Bump manually.
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rainlog-wireless-bridge)
