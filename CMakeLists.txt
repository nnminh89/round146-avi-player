# Round AVI player for the Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round).
# Build with ESP-IDF v6.0.x:  idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(round146_avi_player)
