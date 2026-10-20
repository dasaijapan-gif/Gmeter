// Persistent settings and IMU calibration in the ESP32's NVS flash: namespace
// "gmeter", one blob "state", in the same layout as the Arduino firmware's
// Preferences, so settings saved by either firmware load in the other.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gm_estimator.h"

#ifdef __cplusplus
extern "C" {
#endif

// Field order and types are part of the stored blob: do not change them.
typedef struct {
  uint8_t mode;            // active effect index
  uint8_t contrast_front;  // front layer segment current
  uint8_t contrast_back;   // rear layer (seen through 7 panels)
  uint8_t readout;         // numeric g overlay on every effect
  uint8_t invert;
  uint8_t forward_axis;    // sensor axis toward the car's front: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z
  uint8_t auto_zero;
  uint8_t reinit_minutes;  // periodic panel re-init (write-only bus, EMI)
  float filter_hz;
  float auto_cycle_s;
} settings_t;

// The defaults: mode 0, contrast 0x50..0xFF, readout/invert/auto-zero off,
// forward axis +z, refresh every 10 minutes, 4 Hz filter, no auto-cycle.
void settings_defaults(settings_t* s);

// Initialises the NVS flash (erasing it if it is full or from a newer
// format, as the Arduino core does at start-up). Call once before load/save.
void storage_init(void);
// `migrated` (may be NULL) reports a calibration saved by an older firmware
// whose left/right axis was mirrored and has been corrected on load. Leaves
// both outputs untouched when nothing valid is stored.
bool storage_load(settings_t* settings, gm_calibration_t* calibration, bool* migrated);
bool storage_save(const settings_t* settings, const gm_calibration_t* calibration);
bool storage_clear(void);
gm_vec3_t storage_forward_axis_vector(uint8_t index);
const char* storage_forward_axis_name(uint8_t index);
int storage_parse_forward_axis(const char* text);  // "+x", "-z", ... -> index or -1

#ifdef __cplusplus
}
#endif
