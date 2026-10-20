// Effect "smoke": a smoke plume blown by g-force (a small stable-fluids
// solver). The grids live on the heap only while the effect is active.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GM_SMOKE_NX
#define GM_SMOKE_NX 32
#endif
#ifndef GM_SMOKE_NY
#define GM_SMOKE_NY (GM_PANEL_H / 4)
#endif

#define GM_SMOKE_NZ GM_D
#define GM_SMOKE_SX (GM_SMOKE_NX + 2)
#define GM_SMOKE_SY (GM_SMOKE_NY + 2)
#define GM_SMOKE_SZ (GM_SMOKE_NZ + 2)
#define GM_SMOKE_N (GM_SMOKE_SX * GM_SMOKE_SY * GM_SMOKE_SZ)
#define GM_SMOKE_CX (GM_W / GM_SMOKE_NX)  // pixels per cell
#define GM_SMOKE_CY (GM_H / GM_SMOKE_NY)
GM_STATIC_ASSERT(GM_W % GM_SMOKE_NX == 0 && GM_H % GM_SMOKE_NY == 0, "smoke grid must tile the panel");

typedef struct {
  gm_effect_t base;

  float wind;          // cells/s^2 per g of inertial force
  float buoyancy;      // cells/s^2 per unit density
  float emit;          // density per second at the emitter
  float emit_radius;   // cells
  float emit_depth;    // layers (half extent)
  float dissipation;   // 1/s
  float iters;         // Gauss-Seidel iterations per projection
  float rate_hz;       // physics steps per second (render keeps the frame rate)
  float accum;         // schedule phase (keeps its remainder)
  float since_step;    // real time since the last solver step
  float gain;          // density -> pixel intensity
  float swirl;         // random velocity injected at the emitter

  float *u, *v, *w;
  float *u0, *v0, *w0;
  float *d, *d0;
  gm_rng_t rng;
} gm_smoke_t;

void gm_smoke_init(gm_smoke_t* s);
// Frees the grids (the effect's destructor; deactivate also frees them).
void gm_smoke_deinit(gm_smoke_t* s);

static inline bool gm_smoke_allocated(const gm_smoke_t* s) { return s->d != NULL; }
static inline int gm_smoke_idx(int i, int j, int k) { return (k * GM_SMOKE_SY + j) * GM_SMOKE_SX + i; }
static inline float gm_smoke_density(const gm_smoke_t* s, int i, int j, int k) {
  return s->d ? s->d[gm_smoke_idx(i, j, k)] : 0.0f;
}
float gm_smoke_total_density(const gm_smoke_t* s);
static inline size_t gm_smoke_memory_bytes(void) { return sizeof(float) * GM_SMOKE_N * 8; }

#ifdef __cplusplus
}
#endif
