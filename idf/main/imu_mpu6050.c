#include "imu_mpu6050.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_SMPLRT_DIV 0x19
#define REG_CONFIG 0x1A
#define REG_GYRO_CONFIG 0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_CONFIG2 0x1D  // 6500-family dies only (clones)
#define REG_ACCEL_XOUT_H 0x3B
#define REG_SIGNAL_PATH_RESET 0x68
#define REG_PWR_MGMT_1 0x6B
#define REG_PWR_MGMT_2 0x6C
#define REG_WHO_AM_I 0x75

#define I2C_HZ 400000
#define I2C_TIMEOUT_MS 5  // a long timeout would stall a frame on a glitch

void mpu6050_init(mpu6050_t* m) {
  memset(m, 0, sizeof(*m));
  m->addr = 0x68;
  m->sda = -1;
  m->scl = -1;
  m->temp_scale = 1.0f / 340.0f;
  m->temp_offset = 36.53f;
}

static bool write_reg(mpu6050_t* m, uint8_t reg, uint8_t value) {
  if (!m->dev) return false;
  const uint8_t b[2] = {reg, value};
  return i2c_master_transmit(m->dev, b, sizeof(b), I2C_TIMEOUT_MS) == ESP_OK;
}

static bool read_regs(mpu6050_t* m, uint8_t reg, uint8_t* buf, size_t n) {
  if (!m->dev || n > 8) return false;
  uint8_t tmp[8];
  if (i2c_master_transmit_receive(m->dev, &reg, 1, tmp, n, I2C_TIMEOUT_MS) != ESP_OK) return false;
  memcpy(buf, tmp, n);  // the caller's buffer is only written by a complete read
  return true;
}

// Starts the I2C peripheral on the pins (Arduino: Wire.begin(sda, scl, 400000)).
// An existing bus (one that could not be deleted) is reset and reused.
static bool bus_start(mpu6050_t* m) {
  if (m->bus) {
    i2c_master_bus_reset(m->bus);
  } else {
    i2c_master_bus_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)m->sda;
    cfg.scl_io_num = (gpio_num_t)m->scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&cfg, &m->bus) != ESP_OK) {
      m->bus = NULL;
      return false;
    }
  }
  if (!m->dev) {
    i2c_device_config_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = m->addr;
    dev.scl_speed_hz = I2C_HZ;
    if (i2c_master_bus_add_device(m->bus, &dev, &m->dev) != ESP_OK) {
      m->dev = NULL;
      return false;
    }
  }
  return true;
}

// Detaches the I2C peripheral so the pins can be driven by hand (Arduino: Wire.end()).
static void bus_end(mpu6050_t* m) {
  if (m->dev) {
    if (i2c_master_bus_rm_device(m->dev) != ESP_OK && m->bus) {
      i2c_master_bus_reset(m->bus);  // leaves the driver's state machine idle
      i2c_master_bus_rm_device(m->dev);
    }
    m->dev = NULL;
  }
  if (m->bus && i2c_del_master_bus(m->bus) == ESP_OK) m->bus = NULL;
}

static void unstick_bus(const mpu6050_t* m) {
  // A slave reset mid-transfer can hold SDA low: clock it out with up to 9 pulses.
  const gpio_num_t sda = (gpio_num_t)m->sda, scl = (gpio_num_t)m->scl;
  gpio_config_t io;
  memset(&io, 0, sizeof(io));
  io.pin_bit_mask = 1ull << m->sda;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  gpio_set_level(scl, 1);
  io.pin_bit_mask = 1ull << m->scl;
  io.mode = GPIO_MODE_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io);
  for (int i = 0; i < 9 && gpio_get_level(sda) == 0; ++i) {
    gpio_set_level(scl, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
  }
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
}

static bool configure(mpu6050_t* m) {
  bool ok = write_reg(m, REG_PWR_MGMT_1, 0x80);  // device reset
  vTaskDelay(pdMS_TO_TICKS(100));
  ok &= write_reg(m, REG_SIGNAL_PATH_RESET, 0x07);
  vTaskDelay(pdMS_TO_TICKS(100));
  ok &= write_reg(m, REG_PWR_MGMT_1, 0x01);    // wake, clock = PLL X gyro
  ok &= write_reg(m, REG_PWR_MGMT_2, 0x00);    // all axes on (gyro is the clock reference)
  ok &= write_reg(m, REG_CONFIG, 0x03);        // DLPF 44 Hz (accel on MPU-6050; gyro only on 6500-family)
  ok &= write_reg(m, REG_SMPLRT_DIV, 0x04);    // 1 kHz / (1 + 4) = 200 Hz
  ok &= write_reg(m, REG_GYRO_CONFIG, 0x00);   // +/-250 dps
  ok &= write_reg(m, REG_ACCEL_CONFIG, 0x08);  // +/-4 g, 8192 LSB/g
  // 6500-family clones filter the accelerometer separately: A_DLPF_CFG = 3 (~45 Hz).
  if (m->clone) ok &= write_reg(m, REG_ACCEL_CONFIG2, 0x03);
  return ok;
}

static bool probe_and_configure(mpu6050_t* m) {
  m->present = false;
  if (!read_regs(m, REG_WHO_AM_I, &m->who, 1)) return false;
  if (m->who == 0x68) {
    m->clone = false;
    m->temp_scale = 1.0f / 340.0f;  // MPU-6050 datasheet
    m->temp_offset = 36.53f;
  } else if (m->who == 0x98) {
    m->clone = true;
    m->temp_scale = 1.0f / 326.8f;  // ICM-20689
    m->temp_offset = 25.0f;
  } else if (m->who == 0x70 || m->who == 0x71 || m->who == 0x72 || m->who == 0x73) {
    // MPU-6500 (0x70), MPU-9250 (0x71, a 6500 plus magnetometer), MPU-9255
    // (0x73) and the 0x72 clone: same accelerometer registers, ACCEL_CONFIG2
    // filter and temperature formula.
    m->clone = true;
    m->temp_scale = 1.0f / 333.87f;  // MPU-6500 datasheet
    m->temp_offset = 21.0f;
  } else {
    return false;
  }
  if (!configure(m)) return false;
  m->present = m->detected = true;
  m->consecutive = 0;
  return true;
}

bool mpu6050_begin(mpu6050_t* m, int sda, int scl, uint8_t address) {
  m->addr = address;
  m->sda = sda;
  m->scl = scl;
  // The firmware reports the sensor's state itself; without this, ESP-IDF's
  // I2C driver would print an error line for every failed read while no
  // sensor is wired (the Arduino build ran with the core's log level at 0).
  esp_log_level_set("i2c.master", ESP_LOG_NONE);
  esp_log_level_set("i2c.common", ESP_LOG_NONE);
  unstick_bus(m);
  bus_start(m);
  return probe_and_configure(m);
}

bool mpu6050_recover(mpu6050_t* m) {
  if (m->sda < 0) return false;
  ++m->recoveries;
  bus_end(m);
  if (!m->bus) unstick_bus(m);
  bus_start(m);
  return probe_and_configure(m);
}

static void fail(mpu6050_t* m) {
  ++m->errors;
  if (++m->consecutive >= MPU6050_MAX_CONSECUTIVE_ERRORS) m->present = false;
}

static inline int16_t be16(const uint8_t* b, int i) { return (int16_t)(((uint16_t)b[i] << 8) | b[i + 1]); }

bool mpu6050_read(mpu6050_t* m, gm_vec3_t* accel_g, float* temperature_c) {
  uint8_t b[8];
  if (!m->present || !read_regs(m, REG_ACCEL_XOUT_H, b, sizeof(b))) {
    fail(m);
    return false;
  }
  // An awake sensor never returns exactly zero on all three axes; zeros mean it
  // power-cycled into its sleep state.
  if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) {
    fail(m);
    return false;
  }
  m->consecutive = 0;
  *accel_g = gm_vec3(be16(b, 0) / MPU6050_LSB_PER_G, be16(b, 2) / MPU6050_LSB_PER_G, be16(b, 4) / MPU6050_LSB_PER_G);
  if (temperature_c) *temperature_c = (float)be16(b, 6) * m->temp_scale + m->temp_offset;
  return true;
}
