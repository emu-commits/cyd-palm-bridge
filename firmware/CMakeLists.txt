# CYD Palm bridge firmware (ESP32-2432S028R, no PSRAM).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyd_palm_bridge)

# LVGL draws every frame of every screen, and the size build slows it. Flash isn't the
# constraint (the app partition has room): measured in BUILD_PROGRESS.md.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -O2)
