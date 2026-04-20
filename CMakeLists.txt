cmake_minimum_required(VERSION 3.16)
set(PROJECT_NAME "esp32-firmware")


# Include the ESP-IDF project cmake file
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH environment variable is not set. "
            "Please initialize the ESP-IDF environment (e.g., run 'export.bat' or 'export.ps1') "
            "or set the IDF_PATH environment variable to your ESP-IDF directory.")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Add compile options
add_compile_options("-Wno-format")

# Set extra component directories before the project() command
set(EXTRA_COMPONENT_DIRS 
    "${CMAKE_CURRENT_SOURCE_DIR}/components/espressif__esp_io_expander-v1.0.1"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/sd_card"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/lvgl__lvgl"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/espressif__esp_lcd_touch"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/espressif__esp_lcd_touch_gt911"
)

# Project declaration
project(${PROJECT_NAME})
