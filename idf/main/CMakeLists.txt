# The Gmeter application: drivers, serial command line, tasks.
set(requires gmeter_core nvs_flash esp_timer esp_partition vfs)
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
  list(APPEND requires esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart esp_driver_usb_serial_jtag)
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
