// Register-level MPU-6050 driver (no library dependency). Configured for
// +/-4 g (vertical bumps exceed 2 g), 44 Hz hardware low-pass, 200 Hz output.
// Clone dies that answer WHO_AM_I 0x98 (ICM-20689), or 6500-family parts
// (0x70 MPU-6500, 0x71 MPU-9250, 0x72, 0x73 MPU-9255), are accepted; for
// those the accelerometer low-pass lives in ACCEL_CONFIG2 and is set too.
#pragma once

#include <Arduino.h>

#include "gmeter/math.hpp"

class Mpu6050 {
 public:
  static constexpr float kLsbPerG = 8192.0f;  // AFS_SEL = 1 (+/-4 g)
  static constexpr uint32_t kMaxConsecutiveErrors = 10;

  // Starts I2C on the given pins and configures the sensor. Returns false if
  // the device does not answer or WHO_AM_I is not an MPU-6050 family die.
  bool begin(int sda, int scl, uint8_t address = 0x68);
  // Full recovery: release the bus, clock out a stuck slave, restart I2C and
  // reconfigure. Safe to call repeatedly (e.g. once a second while lost).
  bool recover();

  // Reads one sample. accelG is in the sensor's own frame, in g. An all-zero
  // sample (a sensor that reset back to sleep) counts as an error; after
  // kMaxConsecutiveErrors the sensor is marked lost (present() == false).
  bool read(gm::Vec3& accelG, float* temperatureC = nullptr);

  bool present() const { return present_; }    // configured and answering
  bool detected() const { return detected_; }  // seen at least once since boot
  bool clone() const { return clone_; }
  uint8_t whoAmI() const { return who_; }
  uint32_t errorCount() const { return errors_; }
  uint32_t consecutiveErrors() const { return consecutive_; }
  uint32_t recoveries() const { return recoveries_; }

 private:
  uint8_t addr_ = 0x68;
  int sda_ = -1, scl_ = -1;
  uint8_t who_ = 0;
  bool present_ = false, detected_ = false, clone_ = false;
  uint32_t errors_ = 0, consecutive_ = 0, recoveries_ = 0;
  float tempScale_ = 1.0f / 340.0f, tempOffset_ = 36.53f;

  bool writeReg(uint8_t reg, uint8_t value);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t n);
  bool probeAndConfigure();
  bool configure();
  void unstickBus();
  void fail();
};
