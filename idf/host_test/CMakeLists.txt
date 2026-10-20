# Host tests for the C core: compiles the ESP-IDF C sources and the original
# C++ core (lib/gmeter_core, still used by Gmeter Studio and the simulator)
# with identical flags, and checks they produce bit-identical frames and
# numbers for the same input.
#
#   cmake -S idf/host_test -B build56 -G Ninja -DGM_PANEL_H=56 -DGM_SMOKE_NX=16 -DGM_SMOKE_NY=8
#   cmake --build build56 && ./build56/gm_c_tests
cmake_minimum_required(VERSION 3.16)
project(gmeter_c_host_test C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GM_PANEL_H 64 CACHE STRING "panel height in pixels (56 or 64)")
set(GM_SMOKE_NX "" CACHE STRING "smoke cells across (empty = header default)")
set(GM_SMOKE_NY "" CACHE STRING "smoke cells down (empty = header default)")
option(GM_SANITIZE "build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# While effects are being ported in parallel: build only some of them and some
# test files, e.g. -DGM_EFFECTS="hud;sync" -DGM_TEST_FILES="test_hud.cpp;test_sync.cpp".
# Empty = everything.
set(GM_EFFECTS "" CACHE STRING "effects to compile (names of src/effects/gm_<name>.c), empty = all")
set(GM_TEST_FILES "" CACHE STRING "test files to compile (test_*.cpp), empty = all")

set(GM_IDF ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GM_ROOT ${GM_IDF}/..)

# Same flags for both cores so floating point matches bit for bit.
set(GM_FLAGS -O2 -ffp-contract=off -fno-math-errno -Wall -Wextra -Wdouble-promotion -Werror)
set(GM_DEFS GM_PANEL_W=128 GM_PANEL_H=${GM_PANEL_H} GM_LAYERS=8 GM_LAYER_PITCH_PX=16.0f)
if(GM_SMOKE_NX)
  list(APPEND GM_DEFS GM_SMOKE_NX=${GM_SMOKE_NX})
endif()
if(GM_SMOKE_NY)
  list(APPEND GM_DEFS GM_SMOKE_NY=${GM_SMOKE_NY})
endif()
if(GM_SANITIZE)
  list(APPEND GM_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

file(GLOB GM_C_SOURCES CONFIGURE_DEPENDS ${GM_IDF}/components/gmeter_core/src/*.c)
if(GM_EFFECTS)
  foreach(fx ${GM_EFFECTS})
    list(APPEND GM_C_SOURCES ${GM_IDF}/components/gmeter_core/src/effects/gm_${fx}.c)
  endforeach()
else()
  file(GLOB GM_FX_SOURCES CONFIGURE_DEPENDS ${GM_IDF}/components/gmeter_core/src/effects/*.c)
  list(APPEND GM_C_SOURCES ${GM_FX_SOURCES})
endif()
add_library(gm_c STATIC ${GM_C_SOURCES})
target_include_directories(gm_c PUBLIC ${GM_IDF}/components/gmeter_core/include)
target_compile_definitions(gm_c PUBLIC ${GM_DEFS})
target_compile_options(gm_c PRIVATE ${GM_FLAGS})
target_link_libraries(gm_c PUBLIC m)

file(GLOB GM_CPP_SOURCES CONFIGURE_DEPENDS ${GM_ROOT}/lib/gmeter_core/src/gmeter/*.cpp
     ${GM_ROOT}/lib/gmeter_core/src/gmeter/effects/*.cpp)
add_library(gm_cpp STATIC ${GM_CPP_SOURCES})
target_include_directories(gm_cpp PUBLIC ${GM_ROOT}/lib/gmeter_core/src)
target_compile_definitions(gm_cpp PUBLIC ${GM_DEFS})
target_compile_options(gm_cpp PRIVATE ${GM_FLAGS} -Wno-error)

if(GM_TEST_FILES)
  set(GM_TESTS "")
  foreach(t ${GM_TEST_FILES})
    list(APPEND GM_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/${t})
  endforeach()
else()
  file(GLOB GM_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
endif()
add_executable(gm_c_tests main.cpp ${GM_TESTS})
target_include_directories(gm_c_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${GM_ROOT}/tests)
target_link_libraries(gm_c_tests PRIVATE gm_c gm_cpp)
target_compile_options(gm_c_tests PRIVATE ${GM_FLAGS})

enable_testing()
add_test(NAME c_core COMMAND gm_c_tests)
