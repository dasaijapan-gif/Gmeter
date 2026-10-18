#pragma once
#include "Arduino.h"
#define VSPI 3
#define HSPI 2
#define FSPI 1
class SPISettings {
 public:
  SPISettings() {}
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};
class SPIClass {
 public:
  explicit SPIClass(uint8_t = VSPI) {}
  void begin(int = -1, int = -1, int = -1, int = -1) {}
  void beginTransaction(const SPISettings&) {}
  void endTransaction() {}
  void writeBytes(const uint8_t*, uint32_t) {}
  void write(uint8_t) {}
  uint8_t transfer(uint8_t v) { return v; }
  void setFrequency(uint32_t) {}
};
extern SPIClass SPI;
