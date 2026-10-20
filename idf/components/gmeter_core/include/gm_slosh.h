// Effect "slosh": liquid in a tank, with real waves (shallow-water model on a
// GM_SLOSH_NX x GM_SLOSH_NZ grid).
#pragma once

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_SLOSH_NX (GM_W / 2)
#define GM_SLOSH_NZ GM_D
#define GM_SLOSH_DX ((float)GM_W / GM_SLOSH_NX)
#define GM_SLOSH_DZ GM_LAYER_PITCH_PX

typedef struct {
  gm_effect_t base;

  // tunables
  float fill_fraction;   // rest depth as fraction of panel height
  float tank_width_m;    // virtual physical width of the tank in metres
  float slope_gain;      // 1.0 = physically exact tilt (1 g lateral dries out the tank)
  float damping;         // 1/s velocity damping
  float viscosity;       // surface diffusion px^2/s (kills checkerboarding)
  float ripple;          // idle ripple injection (px/s per step)
  float bob_px_per_g;    // vertical bounce of the whole body per g of vertical jolt
  float bob_hz;
  float splash_gain;     // random velocity injected per g/s of vertical jerk
  float style;           // 0 surface only, 1 surface + dithered body, 2 solid
  float body_intensity;  // dither density of the body in style 1
  float surface_px;      // surface line thickness
  float max_substeps;

  float h[GM_SLOSH_NZ][GM_SLOSH_NX];
  float u[GM_SLOSH_NZ][GM_SLOSH_NX + 1];
  float w[GM_SLOSH_NZ + 1][GM_SLOSH_NX];
  float flux_x[GM_SLOSH_NZ][GM_SLOSH_NX + 1];
  float flux_z[GM_SLOSH_NZ + 1][GM_SLOSH_NX];
  float target_volume;
  gm_spring_t bob;
  float last_vert_g;
  float gv_filtered;  // ~1 Hz low-passed vertical g for the pressure term
  int last_substeps;
  gm_rng_t rng;
} gm_slosh_t;

void gm_slosh_init(gm_slosh_t* s);

// Accessors for tests.
static inline float gm_slosh_height(const gm_slosh_t* s, int x, int z) { return s->h[z][x]; }
float gm_slosh_total_volume(const gm_slosh_t* s);
float gm_slosh_gravity_px(const gm_slosh_t* s);
static inline int gm_slosh_last_substeps(const gm_slosh_t* s) { return s->last_substeps; }

#ifdef __cplusplus
}
#endif
