// Effect "particles": a pile of grains that slides and splashes.
#pragma once

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_PARTICLES_MAX 400
#define GM_PARTICLES_CELL 8  // px per density cell on a layer
#define GM_PARTICLES_GX (GM_W / GM_PARTICLES_CELL + 2)  // padded grid
#define GM_PARTICLES_GY (GM_H / GM_PARTICLES_CELL + 2)
#define GM_PARTICLES_GZ (GM_D + 2)

typedef struct {
  gm_effect_t base;

  float count;
  float tank_width_m;
  float drag;          // 1/s
  float restitution;
  float pressure;      // stiffness: acceleration = -(pressure / tank_m) * grad(max(rho - rho0, 0))
  float rest_density;  // particles per cell below which there is no repulsion
  float jitter;        // random px/s^2 to keep the pile alive
  float dot_size;
  float substeps;

  gm_vec3_t pos[GM_PARTICLES_MAX];
  gm_vec3_t vel[GM_PARTICLES_MAX];
  float density[GM_PARTICLES_GZ][GM_PARTICLES_GY][GM_PARTICLES_GX];
  float pressure_field[GM_PARTICLES_GZ][GM_PARTICLES_GY][GM_PARTICLES_GX];
  gm_rng_t rng;
} gm_particles_t;

void gm_particles_init(gm_particles_t* p);

static inline int gm_particles_active_count(const gm_particles_t* p) { return (int)p->count; }
static inline gm_vec3_t gm_particles_position(const gm_particles_t* p, int i) { return p->pos[i]; }
float gm_particles_gravity_px(const gm_particles_t* p);

#ifdef __cplusplus
}
#endif
