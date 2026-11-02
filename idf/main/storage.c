#include "storage.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "gm_config.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STORAGE_NAMESPACE "gmeter"
#define STORAGE_KEY "state"
#define STORAGE_MAGIC 0x474D5452u  // "GMTR"
#define STORAGE_VERSION 1

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  settings_t settings;
  float rotation[9];
  uint8_t cal_valid;
  uint8_t pad[3];
  uint32_t crc;
} blob_t;

// The Arduino firmware's blob was 68 bytes with this layout.
static_assert(sizeof(settings_t) == 16, "settings_t layout changed");
static_assert(sizeof(blob_t) == 68 && offsetof(blob_t, crc) == 64, "stored blob layout changed");

// CRC-32 (ISO-HDLC, the zlib one) of the blob up to its crc field.
static uint32_t blob_crc(const blob_t* b) { return esp_rom_crc32_le(0, (const uint8_t*)b, offsetof(blob_t, crc)); }

void settings_defaults(settings_t* s) {
  s->mode = 0;
  s->contrast_front = 0x50;
  s->contrast_back = 0xFF;
  s->readout = 0;
  s->invert = 0;
  s->forward_axis = 4;
  s->auto_zero = 0;
  s->reinit_minutes = 10;
  s->filter_hz = 4.0f;
  s->auto_cycle_s = 0.0f;
}

void storage_init(void) {
  const esp_err_t err = nvs_flash_init();
  if ((err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) && nvs_flash_erase() == ESP_OK)
    nvs_flash_init();
}

bool storage_load(settings_t* settings, gm_calibration_t* calibration) {
  nvs_handle_t h;
  if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  blob_t b;
  // A larger stored blob fails (ESP_ERR_NVS_INVALID_LENGTH); a smaller one changes n.
  size_t n = sizeof(b);
  const bool ok = nvs_get_blob(h, STORAGE_KEY, &b, &n) == ESP_OK && n == sizeof(b);
  nvs_close(h);
  if (!ok || b.magic != STORAGE_MAGIC || b.version != STORAGE_VERSION) return false;
  if (blob_crc(&b) != b.crc) return false;
  *settings = b.settings;
  // Reject values a corrupted or hand-edited blob could carry.
  settings_t defaults;
  settings_defaults(&defaults);
  if (!isfinite(settings->filter_hz) || settings->filter_hz < 0.5f || settings->filter_hz > 30.0f)
    settings->filter_hz = defaults.filter_hz;
  if (!isfinite(settings->auto_cycle_s) || settings->auto_cycle_s < 0.0f || settings->auto_cycle_s > 3600.0f)
    settings->auto_cycle_s = defaults.auto_cycle_s;
  if (settings->forward_axis > 5) settings->forward_axis = defaults.forward_axis;
  bool finite = true;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      calibration->sensor_to_car.m[r][c] = b.rotation[r * 3 + c];
      finite = finite && isfinite(b.rotation[r * 3 + c]);
    }
  calibration->valid = b.cal_valid != 0 && finite;
  return true;
}

bool storage_save(const settings_t* settings, const gm_calibration_t* calibration) {
  blob_t b;
  memset(&b, 0, sizeof(b));
  b.magic = STORAGE_MAGIC;
  b.version = STORAGE_VERSION;
  b.settings = *settings;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) b.rotation[r * 3 + c] = calibration->sensor_to_car.m[r][c];
  b.cal_valid = calibration->valid ? 1 : 0;
  b.crc = blob_crc(&b);
  nvs_handle_t h;
  if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_blob(h, STORAGE_KEY, &b, sizeof(b)) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

bool storage_clear(void) {
  nvs_handle_t h;
  if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

gm_vec3_t storage_forward_axis_vector(uint8_t index) {
  switch (index) {
    case 0: return gm_vec3(1, 0, 0);
    case 1: return gm_vec3(-1, 0, 0);
    case 2: return gm_vec3(0, 1, 0);
    case 3: return gm_vec3(0, -1, 0);
    case 5: return gm_vec3(0, 0, -1);
    default: return gm_vec3(0, 0, 1);
  }
}

const char* storage_forward_axis_name(uint8_t index) {
  static const char* names[] = {"+x", "-x", "+y", "-y", "+z", "-z"};
  return index < 6 ? names[index] : "?";
}

int storage_parse_forward_axis(const char* text) {
  if (!text) return -1;
  int sign = 1;
  if (*text == '+') {
    ++text;
  } else if (*text == '-') {
    sign = -1;
    ++text;
  }
  switch (*text) {
    case 'x': case 'X': return sign > 0 ? 0 : 1;
    case 'y': case 'Y': return sign > 0 ? 2 : 3;
    case 'z': case 'Z': return sign > 0 ? 4 : 5;
    default: return -1;
  }
}
