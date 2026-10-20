# Gmeter firmware: ESP-IDF project (v5.2 or newer).
#
#   idf.py set-target esp32        (or esp32s3)
#   idf.py menuconfig              (Gmeter: edition, panel type, SPI speed, ...)
#   idf.py build flash monitor
#
# Variants are sdkconfig overlays in configs/; see README.md.
cmake_minimum_required(VERSION 3.16)

# Build only `main` and the components it needs (gmeter_core, the drivers,
# NVS): faster builds and no unused features such as a core dump partition.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The application binary is build/gmeter.bin.
project(gmeter)
