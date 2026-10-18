// Persistent settings and IMU calibration in ESP32 NVS (Preferences).
#pragma once

#include <cstdint>

#include "gmeter/gforce_estimator.hpp"

struct Settings {
  uint8_t mode = 0;             // active effect index
  uint8_t contrastFront = 0x50; // front layer segment current
  uint8_t contrastBack = 0xFF;  // rear layer (seen through 7 panels)
  uint8_t readout = 0;          // numeric g overlay on every effect
  uint8_t invert = 0;
  uint8_t forwardAxis = 4;      // sensor axis toward the car's front: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z
  uint8_t autoZero = 0;
  uint8_t reinitMinutes = 10;   // periodic panel re-init (write-only bus, EMI)
  float filterHz = 4.0f;
  float autoCycleS = 0.0f;
};

namespace storage {

bool load(Settings& settings, gm::Calibration& calibration);
bool save(const Settings& settings, const gm::Calibration& calibration);
bool clear();
gm::Vec3 forwardAxisVector(uint8_t index);
const char* forwardAxisName(uint8_t index);
int parseForwardAxis(const char* text);  // "+x", "-z", ... -> index or -1

}  // namespace storage
