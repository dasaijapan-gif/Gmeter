#include "imu_mpu6050.hpp"

#include <Wire.h>

namespace {
constexpr uint8_t REG_SMPLRT_DIV = 0x19;
constexpr uint8_t REG_CONFIG = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
constexpr uint8_t REG_ACCEL_CONFIG2 = 0x1D;  // 6500-family dies only (clones)
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
  if (Wire.requestFrom(addr_, static_cast<uint8_t>(n)) != static_cast<uint8_t>(n)) return false;
  for (size_t i = 0; i < n; ++i) {
    const int v = Wire.read();
    if (v < 0) return false;
    buf[i] = static_cast<uint8_t>(v);
  }
  return true;
}

void Mpu6050::unstickBus() {
  // A slave reset mid-transfer can hold SDA low: clock it out with up to 9 pulses.
  pinMode(sda_, INPUT_PULLUP);
  pinMode(scl_, OUTPUT);
  for (int i = 0; i < 9 && digitalRead(sda_) == LOW; ++i) {
    digitalWrite(scl_, LOW); delayMicroseconds(5);
    digitalWrite(scl_, HIGH); delayMicroseconds(5);
  }
  pinMode(scl_, INPUT_PULLUP);
}

bool Mpu6050::begin(int sda, int scl, uint8_t address) {
  addr_ = address;
  sda_ = sda;
  scl_ = scl;
  unstickBus();
  Wire.begin(sda_, scl_, 400000);
  Wire.setTimeOut(5);  // default 50 ms would stall a frame on a glitch
  return probeAndConfigure();
}

bool Mpu6050::recover() {
  if (sda_ < 0) return false;
  ++recoveries_;
  Wire.end();  // detach the peripheral so the pins can be driven by hand
  unstickBus();
  Wire.begin(sda_, scl_, 400000);
  Wire.setTimeOut(5);
  return probeAndConfigure();
}

bool Mpu6050::probeAndConfigure() {
  present_ = false;
  if (!readRegs(REG_WHO_AM_I, &who_, 1)) return false;
  if (who_ == 0x68) {
    clone_ = false;
    tempScale_ = 1.0f / 340.0f; tempOffset_ = 36.53f;      // MPU-6050 datasheet
  } else if (who_ == 0x98) {
    clone_ = true;
    tempScale_ = 1.0f / 326.8f; tempOffset_ = 25.0f;       // ICM-20689
  } else if (who_ == 0x70 || who_ == 0x71 || who_ == 0x72 || who_ == 0x73) {
    // MPU-6500 (0x70), MPU-9250 (0x71, a 6500 plus magnetometer), MPU-9255
    // (0x73) and the 0x72 clone: same accelerometer registers, ACCEL_CONFIG2
    // filter and temperature formula.
    clone_ = true;
    tempScale_ = 1.0f / 333.87f; tempOffset_ = 21.0f;      // MPU-6500 datasheet
  } else {
    return false;
  }
  if (!configure()) return false;
  present_ = detected_ = true;
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
  ok &= writeReg(REG_CONFIG, 0x03);          // DLPF 44 Hz (accel on MPU-6050; gyro only on 6500-family)
  ok &= writeReg(REG_SMPLRT_DIV, 0x04);      // 1 kHz / (1 + 4) = 200 Hz
  ok &= writeReg(REG_GYRO_CONFIG, 0x00);     // +/-250 dps
  ok &= writeReg(REG_ACCEL_CONFIG, 0x08);    // +/-4 g, 8192 LSB/g
  // 6500-family clones filter the accelerometer separately: A_DLPF_CFG = 3 (~45 Hz).
  if (clone_) ok &= writeReg(REG_ACCEL_CONFIG2, 0x03);
  return ok;
}

void Mpu6050::fail() {
  ++errors_;
  if (++consecutive_ >= kMaxConsecutiveErrors) present_ = false;
}

bool Mpu6050::read(gm::Vec3& accelG, float* temperatureC) {
  uint8_t b[8];
  if (!present_ || !readRegs(REG_ACCEL_XOUT_H, b, sizeof(b))) { fail(); return false; }
  // An awake sensor never returns exactly zero on all three axes; zeros mean it
  // power-cycled into its sleep state.
  if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) { fail(); return false; }
  consecutive_ = 0;
  auto be16 = [&](int i) { return static_cast<int16_t>((static_cast<uint16_t>(b[i]) << 8) | b[i + 1]); };
  accelG = gm::Vec3{be16(0) / kLsbPerG, be16(2) / kLsbPerG, be16(4) / kLsbPerG};
  if (temperatureC) *temperatureC = static_cast<float>(be16(6)) * tempScale_ + tempOffset_;
  return true;
}
