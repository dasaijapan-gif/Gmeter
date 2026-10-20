#include "storage.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_partition.h"
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
GM_STATIC_ASSERT(sizeof(settings_t) == 16, "settings_t layout changed");
GM_STATIC_ASSERT(sizeof(blob_t) == 68 && offsetof(blob_t, crc) == 64, "stored blob layout changed");

static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

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
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (partition != NULL && esp_partition_erase_range(partition, 0, partition->size) == ESP_OK) nvs_flash_init();
  }
}

bool storage_load(settings_t* settings, gm_calibration_t* calibration, bool* migrated) {
  if (migrated) *migrated = false;
  nvs_handle_t h;
  if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  blob_t b;
  memset(&b, 0, sizeof(b));
  size_t n = 0;
  bool ok = nvs_get_blob(h, STORAGE_KEY, NULL, &n) == ESP_OK && n == sizeof(b);
  if (ok) {
    n = sizeof(b);
    ok = nvs_get_blob(h, STORAGE_KEY, &b, &n) == ESP_OK && n == sizeof(b);
  }
  nvs_close(h);
  if (!ok || b.magic != STORAGE_MAGIC || b.version != STORAGE_VERSION) return false;
  if (crc32((const uint8_t*)&b, offsetof(blob_t, crc)) != b.crc) return false;
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
  if (calibration->valid) {
    // The car frame (x right, y up, z forward) is left-handed, so a correct
    // sensor-to-car matrix has determinant -1. Firmware before the fix built
    // right = up x forward (determinant +1), which mirrors left and right;
    // negating that row converts it exactly.
    const gm_vec3_t r0 = gm_mat3_row(&calibration->sensor_to_car, 0);
    const gm_vec3_t r1 = gm_mat3_row(&calibration->sensor_to_car, 1);
    const gm_vec3_t r2 = gm_mat3_row(&calibration->sensor_to_car, 2);
    if (gm_vec3_dot(r0, gm_vec3_cross(r1, r2)) > 0.0f) {
      for (int c = 0; c < 3; ++c) calibration->sensor_to_car.m[0][c] = -calibration->sensor_to_car.m[0][c];
      if (migrated) *migrated = true;
    }
  }
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
  b.crc = crc32((const uint8_t*)&b, offsetof(blob_t, crc));
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
