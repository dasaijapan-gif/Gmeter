// Pin map and panel options. Edit the pin numbers below to match your wiring;
// the panel type, SPI speed, frame rate and panel orientation are set with
// `idf.py menuconfig` > Gmeter.
//
// Wiring summary (classic ESP32: WROOM-32 or WROVER DevKit):
//   SPI bus (shared by all 8 panels): SCK GPIO18, MOSI GPIO23, D/C GPIO19, RES GPIO13
//   Chip selects, layer 0 (nearest the driver) .. layer 7 (nearest the windshield):
//     GPIO 5, 32, 14, 4, 27, 26, 25, 33
//   IMU (MPU-6050 / GY-521): SDA GPIO21, SCL GPIO22, VCC 5 V (module LDO + pull-ups to 3.3 V)
//   Mode button: GPIO0 (the BOOT button on most dev boards), active low
// Avoid GPIO6-11 (flash), 34-39 (input only), 12 (MTDI strapping), 1/3 (UART0),
// and 16/17 (PSRAM on WROVER modules, so one image runs on WROOM and WROVER).
// GPIO5 and GPIO14 may toggle during boot; the panels are hardware-reset after.
// D/C sits on the SPI bus's default MISO pin (19 / S3: 13). The bus is set up
// without MISO, so the pin stays free; display_ssd1309.c configures it as an
// output after the bus anyway.
#pragma once

#include <stdint.h>

#include "gm_config.h"
#include "sdkconfig.h"

// 0 = WiseChip 128x56 SSD1309, 1 = generic SSD1309 128x64, 2 = SSD1306 128x64
#define GM_PANEL_MODEL CONFIG_GMETER_PANEL_MODEL
#define GM_SPI_HZ CONFIG_GMETER_SPI_HZ
#define GM_TARGET_FPS CONFIG_GMETER_TARGET_FPS
// Bit i set: panel i is mounted rotated 180 degrees (segment/COM remap inverted).
#define GM_PANEL_FLIP_MASK CONFIG_GMETER_PANEL_FLIP_MASK
// 1 if the chip-select list below is wired windshield-first.
#ifdef CONFIG_GMETER_PANELS_REVERSED
#define GM_PANELS_REVERSED 1
#else
#define GM_PANELS_REVERSED 0
#endif

GM_STATIC_ASSERT(GM_LAYERS <= 8, "board_config has one chip-select pin per layer for at most 8 layers");
GM_STATIC_ASSERT(GM_PANEL_W == 128, "the SSD130x driver addresses columns 0..127");

#if CONFIG_IDF_TARGET_ESP32S3
#define BOARD_SPI_HOST SPI2_HOST  // FSPI
#define BOARD_SPI_SCK 12          // SPI2 IO_MUX pins on the S3
#define BOARD_SPI_MOSI 11
#define BOARD_OLED_DC 13
#define BOARD_OLED_RESET 14
#define BOARD_OLED_CS_PINS {10, 4, 5, 6, 7, 15, 16, 17}
#define BOARD_I2C_SDA 8
#define BOARD_I2C_SCL 9
#define BOARD_BUTTON 0
#elif CONFIG_IDF_TARGET_ESP32
#define BOARD_SPI_HOST SPI3_HOST  // VSPI
#define BOARD_SPI_SCK 18          // VSPI IO_MUX pins
#define BOARD_SPI_MOSI 23
#define BOARD_OLED_DC 19
#define BOARD_OLED_RESET 13
#define BOARD_OLED_CS_PINS {5, 32, 14, 4, 27, 26, 25, 33}
#define BOARD_I2C_SDA 21
#define BOARD_I2C_SCL 22
#define BOARD_BUTTON 0
#else
#error "Gmeter supports the ESP32 and the ESP32-S3 (idf.py set-target esp32 or esp32s3)"
#endif

#define BOARD_IMU_ADDRESS 0x68  // 0x69 if AD0 is tied high
#define BOARD_IMU_SAMPLE_HZ 200
