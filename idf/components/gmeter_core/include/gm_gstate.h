// Turns g-force into a discrete driving state (idle, accelerate, brake, left,
// right) with hysteresis and a minimum hold time, plus bump detection on
// vertical spikes. Shared by effects that react to "what the car is doing".
#pragma once

#include <stdbool.h>

#include "gm_gforce.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GM_DRIVE_IDLE = 0,
  GM_DRIVE_ACCELERATE,
  GM_DRIVE_BRAKE,
  GM_DRIVE_LEFT,
  GM_DRIVE_RIGHT,
} gm_drive_state_t;

typedef struct {
  float enter_g;  // horizontal g that leaves idle (default 0.25)
  float hold_s;   // minimum seconds in a state (default 0.4)
  float bump_g;   // vertical g spike that counts as a bump (default 0.3)
  gm_drive_state_t state;
  float held;
  float prev_vert;
} gm_gstate_t;

// Defaults and idle state.
void gm_gstate_init(gm_gstate_t* s);
// Returns true when the state changed this update. `bumped` (may be NULL) is
// set on the update where a vertical spike starts.
bool gm_gstate_update(gm_gstate_t* s, gm_gforce_t g, float dt, bool* bumped);
static inline gm_drive_state_t gm_gstate_state(const gm_gstate_t* s) { return s->state; }
static inline float gm_gstate_seconds_in_state(const gm_gstate_t* s) { return s->held; }
// Back to idle (keeps the thresholds).
void gm_gstate_reset(gm_gstate_t* s);

#ifdef __cplusplus
}
#endif
