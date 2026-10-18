// Pin map and panel model. Edit this file (or override the GM_* macros from
// platformio.ini) to match your wiring.
//
// Wiring summary (classic ESP32-WROOM):
//   SPI bus (shared by all 8 panels): SCK GPIO18, MOSI GPIO23, D/C GPIO19, RES GPIO13
//   Chip selects, layer 0 (nearest the driver) .. layer 7 (nearest the windshield):
//     GPIO 5, 17, 16, 4, 27, 26, 25, 33
//   IMU (MPU-6050 / GY-521): SDA GPIO21, SCL GPIO22, VCC 5 V (module LDO + pull-ups to 3.3 V)
//   Mode button: GPIO0 (the BOOT button on most dev boards), active low
// Avoid GPIO6-11 (flash), 34-39 (input only), 12 (MTDI strapping), 1/3 (UART0).
// On WROVER modules GPIO16/17 belong to the PSRAM: use 32 and 14 instead.
#pragma once

#include <cstdint>

#ifndef GM_PANEL_MODEL
#define GM_PANEL_MODEL 0   // 0 = WiseChip 128x56 SSD1309, 1 = generic SSD1309 128x64, 2 = SSD1306 128x64
#endif
#ifndef GM_SPI_HZ
#define GM_SPI_HZ 10000000
#endif
#ifndef GM_TARGET_FPS
#define GM_TARGET_FPS 60
#endif
// Bit i set: panel i is mounted rotated 180 degrees (segment/COM remap inverted).
#ifndef GM_PANEL_FLIP_MASK
#define GM_PANEL_FLIP_MASK 0x00
#endif
// Set to 1 if the chip-select list below is wired windshield-first.
#ifndef GM_PANELS_REVERSED
#define GM_PANELS_REVERSED 0
#endif

namespace board {

#if defined(GM_BOARD_ESP32_S3)
constexpr int kSpiSck = 12;   // SPI2 IO_MUX pins on the S3
constexpr int kSpiMosi = 11;
constexpr int kOledDc = 13;
constexpr int kOledReset = 14;
constexpr int kOledCs[8] = {10, 4, 5, 6, 7, 15, 16, 17};
constexpr int kI2cSda = 8;
constexpr int kI2cScl = 9;
constexpr int kButton = 0;
#else  // classic ESP32
constexpr int kSpiSck = 18;   // VSPI IO_MUX pins
constexpr int kSpiMosi = 23;
constexpr int kOledDc = 19;
constexpr int kOledReset = 13;
constexpr int kOledCs[8] = {5, 17, 16, 4, 27, 26, 25, 33};
constexpr int kI2cSda = 21;
constexpr int kI2cScl = 22;
constexpr int kButton = 0;
#endif

constexpr uint8_t kImuAddress = 0x68;  // 0x69 if AD0 is tied high
constexpr int kImuSampleHz = 200;

}  // namespace board
