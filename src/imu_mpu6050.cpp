#include "imu_mpu6050.hpp"

#include <Wire.h>

namespace {
constexpr uint8_t REG_SMPLRT_DIV = 0x19;
constexpr uint8_t REG_CONFIG = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;
constexpr uint8_t REG_SIGNAL_PATH_RESET = 0x68;
constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
constexpr uint8_t REG_PWR_MGMT_2 = 0x6C;
constexpr uint8_t REG_WHO_AM_I = 0x75;
}  // namespace

bool Mpu6050::writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool Mpu6050::readRegs(uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr_, static_cast<uint8_t>(n)) != n) return false;
  for (size_t i = 0; i < n; ++i) {
    const int v = Wire.read();
    if (v < 0) return false;
    buf[i] = static_cast<uint8_t>(v);
  }
  return true;
}

bool Mpu6050::begin(int sda, int scl, uint8_t address) {
  addr_ = address;
  // Recover a slave left holding SDA low by a reset mid-transaction.
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT);
  if (digitalRead(sda) == LOW) {
    for (int i = 0; i < 9; ++i) {
      digitalWrite(scl, LOW); delayMicroseconds(5);
      digitalWrite(scl, HIGH); delayMicroseconds(5);
    }
  }
  pinMode(scl, INPUT_PULLUP);
  Wire.begin(sda, scl, 400000);
  Wire.setTimeOut(5);  // default 50 ms would stall a frame on a glitch
  return reinit();
}

bool Mpu6050::reinit() {
  present_ = clone_ = false;
  if (!readRegs(REG_WHO_AM_I, &who_, 1)) return false;
  if (who_ == 0x68) {
    present_ = true;
  } else if (who_ == 0x98 || who_ == 0x72) {
    present_ = clone_ = true;  // ICM-2068x-class dies answering the MPU-6050 map
  } else {
    return false;
  }
  if (!configure()) { present_ = false; return false; }
  consecutive_ = 0;
  return true;
}

bool Mpu6050::configure() {
  bool ok = writeReg(REG_PWR_MGMT_1, 0x80);  // device reset
  delay(100);
  ok &= writeReg(REG_SIGNAL_PATH_RESET, 0x07);
  delay(100);
  ok &= writeReg(REG_PWR_MGMT_1, 0x01);      // wake, clock = PLL X gyro
  ok &= writeReg(REG_PWR_MGMT_2, 0x00);      // all axes on (gyro is the clock reference)
  ok &= writeReg(REG_CONFIG, 0x03);          // DLPF 44 Hz accel, 1 kHz internal rate
  ok &= writeReg(REG_SMPLRT_DIV, 0x04);      // 1 kHz / (1 + 4) = 200 Hz
  ok &= writeReg(REG_GYRO_CONFIG, 0x00);     // +/-250 dps
  ok &= writeReg(REG_ACCEL_CONFIG, 0x08);    // +/-4 g, 8192 LSB/g
  return ok;
}

bool Mpu6050::read(gm::Vec3& accelG, float* temperatureC) {
  uint8_t b[8];
  if (!readRegs(REG_ACCEL_XOUT_H, b, sizeof(b))) {
    ++errors_;
    ++consecutive_;
    return false;
  }
  consecutive_ = 0;
  auto be16 = [&](int i) { return static_cast<int16_t>((static_cast<uint16_t>(b[i]) << 8) | b[i + 1]); };
  accelG = gm::Vec3{be16(0) / kLsbPerG, be16(2) / kLsbPerG, be16(4) / kLsbPerG};
  if (temperatureC) *temperatureC = be16(6) / 340.0f + 36.53f;
  return true;
}
