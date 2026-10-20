# The Gmeter application: drivers, serial command line, tasks.
set(requires gmeter_core nvs_flash esp_timer esp_partition vfs)
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
  list(APPEND requires esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart esp_driver_usb_serial_jtag
       esp_vfs_console)
else()
  # ESP-IDF 5.2: all drivers live in the `driver` component.
  list(APPEND requires driver)
endif()

idf_component_register(SRCS "app_main.c" "console_io.c" "display_ssd1309.c" "imu_mpu6050.c" "serial_cli.c"
                            "storage.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${requires})

# The ESP32 FPU is single precision only; promotion to double is a silent 10x
# slowdown, so make it an error (as in the core component).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wdouble-promotion -Werror=double-promotion -fno-math-errno)

# Artwork exported from Gmeter Studio (main/user_animation.h). Its presence is
# checked when CMake configures, so adding or deleting the file re-runs CMake
# and rebuilds app_main.c (a compile-time __has_include would miss a file that
# appears after the first build).
file(GLOB gm_user_animation CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/user_animation.h")
if(gm_user_animation)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE GM_HAVE_USER_ANIMATION=1)
endif()
