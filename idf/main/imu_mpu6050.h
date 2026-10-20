// Register-level MPU-6050 driver on ESP-IDF's I2C master driver. Configured
// for +/-4 g (vertical bumps exceed 2 g), 44 Hz hardware low-pass, 200 Hz
// output. Clone dies that answer WHO_AM_I 0x98 (ICM-20689), or 6500-family
// parts (0x70 MPU-6500, 0x71 MPU-9250, 0x72, 0x73 MPU-9255), are accepted; for
// those the accelerometer low-pass lives in ACCEL_CONFIG2 and is set too.
//
// Use from one task at a time (app_main.c: set-up, then the sensor task only).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "gm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPU6050_LSB_PER_G 8192.0f  // AFS_SEL = 1 (+/-4 g)
#define MPU6050_MAX_CONSECUTIVE_ERRORS 10u

typedef struct {
  uint8_t addr;
  int sda, scl;
  uint8_t who;
  bool present, detected, clone;
  uint32_t errors, consecutive, recoveries;
  float temp_scale, temp_offset;
  i2c_master_bus_handle_t bus;
  i2c_master_dev_handle_t dev;
} mpu6050_t;

// Not started, nothing detected.
void mpu6050_init(mpu6050_t* m);
// Starts I2C on the given pins and configures the sensor. Returns false if
// the device does not answer or WHO_AM_I is not an MPU-6050 family die.
// (The usual address is 0x68.)
bool mpu6050_begin(mpu6050_t* m, int sda, int scl, uint8_t address);
// Full recovery: release the bus, clock out a stuck slave, restart I2C and
// reconfigure. Safe to call repeatedly (e.g. once a second while lost).
bool mpu6050_recover(mpu6050_t* m);

// Reads one sample. accel_g is in the sensor's own frame, in g. An all-zero
// sample (a sensor that reset back to sleep) counts as an error; after
// MPU6050_MAX_CONSECUTIVE_ERRORS the sensor is marked lost (present false).
// temperature_c may be NULL.
bool mpu6050_read(mpu6050_t* m, gm_vec3_t* accel_g, float* temperature_c);

static inline bool mpu6050_present(const mpu6050_t* m) { return m->present; }    // configured and answering
static inline bool mpu6050_detected(const mpu6050_t* m) { return m->detected; }  // seen at least once since boot
static inline bool mpu6050_clone(const mpu6050_t* m) { return m->clone; }
static inline uint8_t mpu6050_who_am_i(const mpu6050_t* m) { return m->who; }
static inline uint32_t mpu6050_error_count(const mpu6050_t* m) { return m->errors; }
static inline uint32_t mpu6050_consecutive_errors(const mpu6050_t* m) { return m->consecutive; }
static inline uint32_t mpu6050_recoveries(const mpu6050_t* m) { return m->recoveries; }

#ifdef __cplusplus
}
#endif
