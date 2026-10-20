// Effect "ball": a ball thrown around the volume by g-force, with rings and a
// trail.
#pragma once

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_GBALL_TRAIL_MAX 48

typedef struct {
  gm_effect_t base;

  // tunables
  float full_scale_g;         // g that moves the ball to the lateral edge
  float vertical_scale_g;     // g that moves the ball to the top/bottom edge
  float depth_scale_g;        // g that moves the ball to the front/back layer
  float natural_hz;           // spring natural frequency
  float damping;              // 1 = critically damped
  float radius;               // px
  float depth_radius_layers;  // ellipsoid half-depth in layers
  float trail_seconds;        // how long the trail persists
  float trail_intensity;
  float follow_inertia;  // 1: ball moves like a loose object; 0: like an acceleration arrow
  float hud;
  float rings;

  gm_spring3_t spring;
  gm_vec3_t trail[GM_GBALL_TRAIL_MAX];
  float trail_age[GM_GBALL_TRAIL_MAX];
  int trail_head, trail_count;
  float trail_timer;
  float peak_g, peak_age;
  gm_gforce_t last_g;
} gm_gball_t;

void gm_gball_init(gm_gball_t* b);
static inline gm_vec3_t gm_gball_position(const gm_gball_t* b) { return gm_spring3_pos(&b->spring); }

#ifdef __cplusplus
}
#endif
