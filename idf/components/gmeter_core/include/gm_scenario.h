// Scripted g-force profiles: keyframes of dynamic g (car frame: x lateral,
// y vertical, z longitudinal) smoothly interpolated. The firmware plays one as
// a demo when no IMU is connected.
#pragma once

#include "gm_gforce.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float t;            // seconds
  gm_vec3_t dynamic;  // g, car frame
} gm_gkey_t;

typedef struct {
  const char* name;
  const gm_gkey_t* keys;
  int count;
} gm_scenario_t;

static inline const char* gm_scenario_name(const gm_scenario_t* s) { return s->name; }
static inline float gm_scenario_duration(const gm_scenario_t* s) { return s->count ? s->keys[s->count - 1].t : 0.0f; }
// Smoothly interpolated dynamic g at time t (holds the last key).
gm_vec3_t gm_scenario_dynamic_at(const gm_scenario_t* s, float t);
// Full proper acceleration (rest + dynamic) at time t.
gm_gforce_t gm_scenario_at(const gm_scenario_t* s, float t);

int gm_scenario_preset_count(void);
// Index is clamped to the valid range; the last preset is the combined "drive".
const gm_scenario_t* gm_scenario_preset(int i);
// NULL when no preset has that name.
const gm_scenario_t* gm_scenario_find(const char* name);

#ifdef __cplusplus
}
#endif
