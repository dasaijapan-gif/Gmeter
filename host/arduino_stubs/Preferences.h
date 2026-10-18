#pragma once
#include "Arduino.h"
class Preferences {
 public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  bool clear() { return true; }
  size_t putBytes(const char*, const void*, size_t n) { return n; }
  size_t getBytes(const char*, void*, size_t) { return 0; }
  size_t getBytesLength(const char*) { return 0; }
  size_t putFloat(const char*, float) { return 4; }
  float getFloat(const char*, float d = 0.0f) { return d; }
  size_t putUChar(const char*, uint8_t) { return 1; }
  uint8_t getUChar(const char*, uint8_t d = 0) { return d; }
  bool isKey(const char*) { return false; }
};
