cmake_minimum_required(VERSION 3.16)
project(gmeter_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GM_CORE ${GM_ROOT}/lib/gmeter_core/src)

# Geometry overrides, e.g. -DGM_PANEL_H=56 for 128x56 transparent panels.
set(GM_PANEL_W 128 CACHE STRING "panel width in pixels")
set(GM_PANEL_H 64 CACHE STRING "panel height in pixels")
set(GM_LAYERS 8 CACHE STRING "number of stacked panels")
set(GM_LAYER_PITCH_PX 16.0f CACHE STRING "panel spacing in pixel pitches")
# Smoke grid (cells across / down). Empty = header default (4x4-pixel cells).
# The ESP32 builds use 16 x 8; NX must divide GM_PANEL_W and NY GM_PANEL_H.
set(GM_SMOKE_NX "" CACHE STRING "smoke cells across")
set(GM_SMOKE_NY "" CACHE STRING "smoke cells down")

file(GLOB GM_CORE_SOURCES ${GM_CORE}/gmeter/*.cpp ${GM_CORE}/gmeter/effects/*.cpp)
add_library(gmeter_core STATIC ${GM_CORE_SOURCES})
target_include_directories(gmeter_core PUBLIC ${GM_CORE})
target_compile_definitions(gmeter_core PUBLIC
  GM_PANEL_W=${GM_PANEL_W} GM_PANEL_H=${GM_PANEL_H} GM_LAYERS=${GM_LAYERS} GM_LAYER_PITCH_PX=${GM_LAYER_PITCH_PX})
if(GM_SMOKE_NX)
  target_compile_definitions(gmeter_core PUBLIC GM_SMOKE_NX=${GM_SMOKE_NX})
endif()
if(GM_SMOKE_NY)
  target_compile_definitions(gmeter_core PUBLIC GM_SMOKE_NY=${GM_SMOKE_NY})
endif()
target_compile_options(gmeter_core PRIVATE -Wall -Wextra -Wshadow -Wdouble-promotion -Werror=double-promotion -fno-math-errno)

# Scenario-driven simulator: records voxel frames for the web viewer and
# prints ASCII previews / timing benchmarks.
add_executable(gmeter_sim sim_main.cpp)
target_link_libraries(gmeter_sim PRIVATE gmeter_core)
# Picks up idf/main/user_animation.h (exported from Gmeter Studio) when present.
target_include_directories(gmeter_sim PRIVATE ${GM_ROOT}/idf/main)
target_compile_options(gmeter_sim PRIVATE -Wall -Wextra)

# Unit tests (self-contained harness, no external framework).
add_executable(gmeter_tests ${GM_ROOT}/tests/test_core.cpp)
target_link_libraries(gmeter_tests PRIVATE gmeter_core)
target_include_directories(gmeter_tests PRIVATE ${GM_ROOT}/tests)
target_compile_options(gmeter_tests PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME core COMMAND gmeter_tests)

# Firmware compile check: builds the ESP32 sources against stub Arduino
# headers so API mistakes surface without a cross toolchain. Not runnable.
file(GLOB GM_FW_SOURCES ${GM_ROOT}/src/*.cpp)
if(GM_FW_SOURCES)
  add_library(firmware_hostcheck OBJECT ${GM_FW_SOURCES})
  target_include_directories(firmware_hostcheck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/arduino_stubs ${GM_ROOT}/src)
  target_link_libraries(firmware_hostcheck PRIVATE gmeter_core)
  target_compile_definitions(firmware_hostcheck PRIVATE GM_HOST_STUB=1 ARDUINO=10819 ESP32=1)
  target_compile_options(firmware_hostcheck PRIVATE -Wall -Wextra)
  set_target_properties(firmware_hostcheck PROPERTIES EXCLUDE_FROM_ALL ON)

  # Firmware pin test: runs PanelStack::begin() against the stubs, whose
  # SPI.begin mirrors the real core's pin handling, for both pin maps.
  foreach(chip classic s3)
    add_executable(firmware_pins_${chip} ${GM_ROOT}/tests/test_firmware_pins.cpp
                   ${GM_ROOT}/src/display_ssd1309.cpp ${CMAKE_CURRENT_SOURCE_DIR}/arduino_stubs/stubs.cpp)
    target_include_directories(firmware_pins_${chip} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/arduino_stubs ${GM_ROOT}/src)
    target_link_libraries(firmware_pins_${chip} PRIVATE gmeter_core)
    target_compile_definitions(firmware_pins_${chip} PRIVATE GM_HOST_STUB=1 ARDUINO=10819 ESP32=1)
    if(chip STREQUAL "s3")
      target_compile_definitions(firmware_pins_${chip} PRIVATE GM_BOARD_ESP32_S3=1)
    endif()
    target_compile_options(firmware_pins_${chip} PRIVATE -Wall -Wextra)
    add_test(NAME firmware_pins_${chip} COMMAND firmware_pins_${chip})
  endforeach()
endif()
