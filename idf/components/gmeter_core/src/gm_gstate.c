// Discrete driving state from g-force, with hysteresis, a minimum hold time
// and bump detection on vertical spikes.
#include "gm_gstate.h"

#include <math.h>

void gm_gstate_init(gm_gstate_t* s) {
  s->enter_g = 0.25f;
  s->hold_s = 0.4f;
  s->bump_g = 0.3f;
  s->state = GM_DRIVE_IDLE;
  s->held = 0.0f;
  s->prev_vert = 0.0f;
}

void gm_gstate_reset(gm_gstate_t* s) {
  s->state = GM_DRIVE_IDLE;
  s->held = 0.0f;
  s->prev_vert = 0.0f;
}

static float support(gm_drive_state_t s, gm_vec3_t d) {
  switch (s) {
    case GM_DRIVE_ACCELERATE: return d.z;
    case GM_DRIVE_BRAKE: return -d.z;
    case GM_DRIVE_RIGHT: return d.x;
    case GM_DRIVE_LEFT: return -d.x;
    default: return 0.0f;
  }
}

static gm_drive_state_t choose(const gm_gstate_t* s, gm_vec3_t d) {
  gm_drive_state_t best = GM_DRIVE_IDLE;
  float best_v = 0.0f;
  static const gm_drive_state_t k_active[4] = {GM_DRIVE_ACCELERATE, GM_DRIVE_BRAKE, GM_DRIVE_LEFT, GM_DRIVE_RIGHT};
  for (int i = 0; i < 4; ++i) {
    const gm_drive_state_t st = k_active[i];
    if (support(st, d) > best_v) {
      best_v = support(st, d);
      best = st;
    }
  }
  if (s->state != GM_DRIVE_IDLE) {
    const float mine = support(s->state, d);
    if (mine >= 0.6f * s->enter_g) {
      // Hand over only to a clearly stronger state that could itself be entered.
      const bool hand_over = best != s->state && best_v >= mine + 0.1f && best_v >= s->enter_g;
      return hand_over ? best : s->state;
    }
  }
  return best_v >= s->enter_g ? best : GM_DRIVE_IDLE;
}

bool gm_gstate_update(gm_gstate_t* s, gm_gforce_t g, float dt, bool* bumped) {
  const gm_vec3_t d = gm_gforce_dynamic(g);
  const bool spike = fabsf(d.y) >= s->bump_g && fabsf(s->prev_vert) < s->bump_g;
  s->prev_vert = d.y;
  if (bumped) *bumped = spike;
  s->held += dt;
  if (s->held < s->hold_s) return false;
  const gm_drive_state_t next = choose(s, d);
  if (next == s->state) return false;
  s->state = next;
  s->held = 0.0f;
  return true;
}
