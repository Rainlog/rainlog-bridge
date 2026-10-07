# Dependencies stay visible during ESP-IDF's early component scan.
set(console_dependencies esp_driver_uart esp_driver_usb_serial_jtag
    esp_http_server esp_driver_gpio esp_netif esp_wifi esp_timer esp_app_format)
if(RAINLOG_DEBUG_CONSOLE)
    idf_component_register(SRCS "console.c" "vendor/duktape.c" INCLUDE_DIRS "include" "vendor"
        REQUIRES ${console_dependencies})
    target_compile_options(${COMPONENT_LIB} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/include/duktape_platform.h")
    set_source_files_properties("vendor/duktape.c" PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC RAINLOG_DEBUG_CONSOLE=1)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/main"
        "${PROJECT_DIR}/main/net" "${PROJECT_DIR}/main/forward" "${PROJECT_DIR}/main/stats" "${PROJECT_DIR}/main/ota" "${PROJECT_DIR}/main/ui")
else()
    idf_component_register(INCLUDE_DIRS "include" REQUIRES ${console_dependencies})
endif()
