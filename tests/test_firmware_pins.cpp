// Pin set-up check for the firmware, built against the Arduino stubs in
// host/arduino_stubs, whose SPI.begin mirrors how Arduino-ESP32 2.0.x claims
// pins (a -1 MISO still takes the bus's default MISO pin as an INPUT). Runs
// PanelStack::begin() and checks that every panel control line ends up an
// output and that no two signals share a GPIO. Built for the classic ESP32 and,
// with GM_BOARD_ESP32_S3, for the ESP32-S3 pin map.
#include <Arduino.h>

#include <cstdio>

#include "board_config.hpp"
#include "display_ssd1309.hpp"

int main() {
#if defined(GM_BOARD_ESP32_S3)
  const char* chip = "ESP32-S3";
#else
  const char* chip = "ESP32";
#endif
  int fails = 0;
  PanelStack display;
  display.begin();

  auto expectOutput = [&](int pin, const char* what) {
    if (stub::pinModeOf[pin] != OUTPUT + 1) {
      std::printf("FAIL %s: %s (GPIO %d) is not an output after display.begin()\n", chip, what, pin);
      ++fails;
    }
  };
  expectOutput(board::kOledDc, "D/C");
  expectOutput(board::kOledReset, "RES");
  expectOutput(board::kSpiSck, "SCK");
  expectOutput(board::kSpiMosi, "MOSI");
  for (int i = 0; i < PanelStack::kPanels; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "CS panel %d", i);
    expectOutput(board::kOledCs[i], name);
  }

  const int pins[] = {board::kSpiSck, board::kSpiMosi, board::kOledDc, board::kOledReset, board::kOledCs[0],
                      board::kOledCs[1], board::kOledCs[2], board::kOledCs[3], board::kOledCs[4], board::kOledCs[5],
                      board::kOledCs[6], board::kOledCs[7], board::kI2cSda, board::kI2cScl, board::kButton};
  const int n = static_cast<int>(sizeof(pins) / sizeof(pins[0]));
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j)
      if (pins[i] == pins[j]) {
        std::printf("FAIL %s: GPIO %d is assigned to two signals\n", chip, pins[i]);
        ++fails;
      }
#if !defined(GM_BOARD_ESP32_S3)
  // Classic map: no flash pins, input-only pins, UART0 or WROVER PSRAM pins.
  for (int i = 0; i < n; ++i) {
    const int p = pins[i];
    if ((p >= 6 && p <= 11) || p >= 34 || p == 1 || p == 3 || p == 16 || p == 17 || p == 12) {
      std::printf("FAIL %s: GPIO %d is reserved (flash, input-only, UART0, strapping or WROVER PSRAM)\n", chip, p);
      ++fails;
    }
  }
#else
  // S3 map: no flash/PSRAM (26-37), USB (19/20), strapping (3, 45, 46) or UART0 (43/44).
  for (int i = 0; i < n; ++i) {
    const int p = pins[i];
    if ((p >= 26 && p <= 37) || p == 19 || p == 20 || p == 3 || p == 45 || p == 46 || p == 43 || p == 44) {
      std::printf("FAIL %s: GPIO %d is reserved (flash/PSRAM, USB, strapping or UART0)\n", chip, p);
      ++fails;
    }
  }
#endif
  std::printf("%s pin set-up: %s\n", chip, fails ? "FAILED" : "ok");
  return fails ? 1 : 0;
}
