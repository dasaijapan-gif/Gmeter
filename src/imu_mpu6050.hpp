// Register-level MPU-6050 driver (no library dependency). Configured for
// +/-4 g (vertical bumps exceed 2 g), 44 Hz hardware low-pass, 200 Hz output.
#pragma once

#include <Arduino.h>

#include "gmeter/math.hpp"

class Mpu6050 {
 public:
  static constexpr float kLsbPerG = 8192.0f;  // AFS_SEL = 1 (+/-4 g)

  // Starts I2C on the given pins and configures the sensor. Returns false if
  // the device does not answer or WHO_AM_I is not an MPU-6050 (clones that
  // report 0x98/0x72 are accepted with a warning).
  bool begin(int sda, int scl, uint8_t address = 0x68);
  bool reinit();

  // Reads one sample. accelG is in the sensor's own frame, in g.
  bool read(gm::Vec3& accelG, float* temperatureC = nullptr);

  bool present() const { return present_; }
  bool clone() const { return clone_; }
  uint8_t whoAmI() const { return who_; }
  uint32_t errorCount() const { return errors_; }
  uint32_t consecutiveErrors() const { return consecutive_; }

 private:
  uint8_t addr_ = 0x68;
  uint8_t who_ = 0;
  bool present_ = false, clone_ = false;
  uint32_t errors_ = 0, consecutive_ = 0;

  bool writeReg(uint8_t reg, uint8_t value);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t n);
  bool configure();
};
