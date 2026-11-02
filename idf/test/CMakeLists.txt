# Host tests for the firmware's core (components/gmeter_core), in C. Not part
# of the firmware build; build it in a directory outside the repository:
#
#   cmake -S idf/test -B /tmp/gm56 -G Ninja -DGM_PANEL_H=56
#   cmake --build /tmp/gm56 && /tmp/gm56/gm_tests
#
# See README.md for the reference digests (golden_56.txt, golden_64.txt).
cmake_minimum_required(VERSION 3.16)
project(gmeter_core_tests C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(GM_PANEL_H 64 CACHE STRING "panel height in pixels (56 or 64)")
option(GM_SANITIZE "build with AddressSanitizer and UndefinedBehaviorSanitizer (float-to-int overflow included)" OFF)
# The core to test; another copy of the component can be pointed at here.
set(GM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/gmeter_core CACHE PATH "gmeter_core component to test")

# Fixed flags so floating point is reproducible: no FMA contraction, the
# float functions without errno. The definitions match the firmware's
# geometry.
set(GM_FLAGS -O2 -ffp-contract=off -fno-math-errno -Wall -Wextra -Wdouble-promotion -Werror)
set(GM_DEFS GM_PANEL_W=128 GM_PANEL_H=${GM_PANEL_H} GM_LAYERS=8 GM_LAYER_PITCH_PX=16.0f)
if(GM_SANITIZE)
  # float-cast-overflow is not part of "undefined": it checks that every float
  # to int conversion in the core is in range.
  list(APPEND GM_FLAGS -fsanitize=address,undefined,float-cast-overflow
       -fno-sanitize-recover=undefined,float-cast-overflow -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined,float-cast-overflow)
endif()

file(GLOB GM_CORE_SOURCES CONFIGURE_DEPENDS ${GM_CORE_DIR}/src/*.c ${GM_CORE_DIR}/src/effects/*.c)
add_library(gm_core STATIC ${GM_CORE_SOURCES})
target_include_directories(gm_core PUBLIC ${GM_CORE_DIR}/include)
target_compile_definitions(gm_core PUBLIC ${GM_DEFS})
target_compile_options(gm_core PRIVATE ${GM_FLAGS})
target_link_libraries(gm_core PUBLIC m)

file(GLOB GM_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_executable(gm_tests ${GM_TEST_SOURCES})
target_include_directories(gm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gm_tests PRIVATE GM_GOLDEN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/golden_${GM_PANEL_H}.txt")
target_compile_options(gm_tests PRIVATE ${GM_FLAGS})
target_link_libraries(gm_tests PRIVATE gm_core)

enable_testing()
add_test(NAME gm_tests COMMAND gm_tests)
