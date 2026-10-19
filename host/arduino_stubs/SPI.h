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
  explicit SPIClass(uint8_t bus = VSPI) : bus_(bus) {}
  // Mirrors Arduino-ESP32 2.0.x SPIClass::begin with spiAttachSCK/MISO/MOSI:
  // a negative pin is replaced by the bus default, SCK and MOSI become outputs,
  // and MISO becomes an INPUT even when the sketch passed -1 for it.
  void begin(int sck = -1, int miso = -1, int mosi = -1, int ss = -1) {
    (void)ss;
#if defined(GM_BOARD_ESP32_S3)
    const int dsck = 12, dmiso = 13, dmosi = 11;  // FSPI
#else
    const int dsck = bus_ == VSPI ? 18 : 14, dmiso = bus_ == VSPI ? 19 : 12, dmosi = bus_ == VSPI ? 23 : 13;
#endif
    pinMode(sck < 0 ? dsck : sck, OUTPUT);
    pinMode(miso < 0 ? dmiso : miso, INPUT);
    pinMode(mosi < 0 ? dmosi : mosi, OUTPUT);
  }
  void beginTransaction(const SPISettings&) {}
  void endTransaction() {}
  void writeBytes(const uint8_t*, uint32_t) {}
  void write(uint8_t) {}
  uint8_t transfer(uint8_t v) { return v; }
  void setFrequency(uint32_t) {}

 private:
  uint8_t bus_;
};
extern SPIClass SPI;
