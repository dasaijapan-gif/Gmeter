// Low-resolution 3D "stable fluids" (Jos Stam) smoke. A plume rises from an
// emitter at the centre of the volume and is blown around by the inertial
// force, so it leans back under acceleration, surges toward the windshield
// under braking and smears sideways in corners.
//
// Grid resolution is a compile-time choice: GM_SMOKE_NX x GM_SMOKE_NY cells
// per layer and one cell per layer in depth. NX must divide the panel width
// and NY the panel height. The default is 4x4-pixel cells (32x14 at 128x56,
// 32x16 at 128x64); the ESP32 build uses 16x8 (components/gmeter_core/
// CMakeLists.txt) to stay within the time and memory budget. wind/buoyancy/
// emit_r are in cell units, so tune on the same grid the device uses (Gmeter
// Studio builds 16x8). Buffers are heap-allocated only while active.
#include "gm_smoke.h"

#include <stdlib.h>
#include <string.h>

#define NX GM_SMOKE_NX
#define NY GM_SMOKE_NY
#define NZ GM_SMOKE_NZ
#define SX GM_SMOKE_SX
#define SY GM_SMOKE_SY
#define SZ GM_SMOKE_SZ
#define N GM_SMOKE_N
#define CX GM_SMOKE_CX  // pixels per cell
#define CY GM_SMOKE_CY

static inline int idx(int i, int j, int k) { return gm_smoke_idx(i, j, k); }

static inline void swap_ptr(float** a, float** b) {
  float* t = *a;
  *a = *b;
  *b = t;
}

static void smoke_release(gm_smoke_t* s) {
  free(s->u);
  free(s->v);
  free(s->w);
  free(s->u0);
  free(s->v0);
  free(s->w0);
  free(s->d);
  free(s->d0);
  s->u = s->v = s->w = s->u0 = s->v0 = s->w0 = s->d = s->d0 = NULL;
}

static void smoke_allocate(gm_smoke_t* s) {
  if (s->d) return;
  const size_t bytes = sizeof(float) * N;
  s->u = (float*)malloc(bytes);
  s->v = (float*)malloc(bytes);
  s->w = (float*)malloc(bytes);
  s->u0 = (float*)malloc(bytes);
  s->v0 = (float*)malloc(bytes);
  s->w0 = (float*)malloc(bytes);
  s->d = (float*)malloc(bytes);
  s->d0 = (float*)malloc(bytes);
  // Out of memory: free what was obtained and stay unallocated (the effect
  // then draws nothing). The C++ new[] would throw here instead.
  if (!s->u || !s->v || !s->w || !s->u0 || !s->v0 || !s->w0 || !s->d || !s->d0) smoke_release(s);
}

static void smoke_reset(gm_effect_t* self) {
  gm_smoke_t* s = (gm_smoke_t*)self;
  if (!s->d) return;
  const size_t bytes = sizeof(float) * N;
  memset(s->u, 0, bytes);
  memset(s->v, 0, bytes);
  memset(s->w, 0, bytes);
  memset(s->u0, 0, bytes);
  memset(s->v0, 0, bytes);
  memset(s->w0, 0, bytes);
  memset(s->d, 0, bytes);
  memset(s->d0, 0, bytes);
  s->accum = 0.0f;
  s->since_step = 0.0f;
}

static void smoke_activate(gm_effect_t* self) {
  smoke_allocate((gm_smoke_t*)self);
  smoke_reset(self);
}

static void smoke_deactivate(gm_effect_t* self) { smoke_release((gm_smoke_t*)self); }

float gm_smoke_total_density(const gm_smoke_t* s) {
  if (!s->d) return 0.0f;
  float sum = 0.0f;
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) sum += s->d[idx(i, j, k)];
  return sum;
}

// Boundary conditions. b = 1/2/3 flips the normal velocity component at the
// x/y/z walls (free-slip, no penetration); b = 0 copies (Neumann) for scalars.
static void set_bnd(int b, float* x) {
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j) {
      x[idx(0, j, k)] = b == 1 ? -x[idx(1, j, k)] : x[idx(1, j, k)];
      x[idx(NX + 1, j, k)] = b == 1 ? -x[idx(NX, j, k)] : x[idx(NX, j, k)];
    }
  for (int k = 1; k <= NZ; ++k)
    for (int i = 1; i <= NX; ++i) {
      x[idx(i, 0, k)] = b == 2 ? -x[idx(i, 1, k)] : x[idx(i, 1, k)];
      x[idx(i, NY + 1, k)] = b == 2 ? -x[idx(i, NY, k)] : x[idx(i, NY, k)];
    }
  for (int j = 1; j <= NY; ++j)
    for (int i = 1; i <= NX; ++i) {
      x[idx(i, j, 0)] = b == 3 ? -x[idx(i, j, 1)] : x[idx(i, j, 1)];
      x[idx(i, j, NZ + 1)] = b == 3 ? -x[idx(i, j, NZ)] : x[idx(i, j, NZ)];
    }
  // Edges and corners: average of the inward neighbours along each boundary axis.
  for (int k = 0; k < SZ; ++k) {
    const bool bk = (k == 0 || k == SZ - 1);
    for (int j = 0; j < SY; ++j) {
      const bool bj = (j == 0 || j == SY - 1);
      for (int i = 0; i < SX; ++i) {
        const bool bi = (i == 0 || i == SX - 1);
        const int nb = (bi ? 1 : 0) + (bj ? 1 : 0) + (bk ? 1 : 0);
        if (nb < 2) continue;
        float s = 0.0f;
        if (bi) s += x[idx(i == 0 ? 1 : SX - 2, j, k)];
        if (bj) s += x[idx(i, j == 0 ? 1 : SY - 2, k)];
        if (bk) s += x[idx(i, j, k == 0 ? 1 : SZ - 2)];
        x[idx(i, j, k)] = s / (float)nb;
      }
    }
  }
}

static void lin_solve(int b, float* x, const float* x0, float a, float c, int iters) {
  const float inv_c = 1.0f / c;
  for (int it = 0; it < iters; ++it) {
    for (int k = 1; k <= NZ; ++k)
      for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
          const int id = idx(i, j, k);
          x[id] = (x0[id] + a * (x[id - 1] + x[id + 1] + x[id - SX] + x[id + SX] + x[id - SX * SY] + x[id + SX * SY])) *
                  inv_c;
        }
    set_bnd(b, x);
  }
}

static void advect(int b, float* d, const float* d0, const float* u, const float* v, const float* w, float dt) {
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        float x = (float)i - dt * u[id];
        float y = (float)j - dt * v[id];
        float z = (float)k - dt * w[id];
        x = gm_clampf(x, 0.5f, NX + 0.5f);
        y = gm_clampf(y, 0.5f, NY + 0.5f);
        z = gm_clampf(z, 0.5f, NZ + 0.5f);
        const int i0 = (int)x, j0 = (int)y, k0 = (int)z;
        const float s1 = x - (float)i0, s0 = 1.0f - s1;
        const float t1 = y - (float)j0, t0 = 1.0f - t1;
        const float r1 = z - (float)k0, r0 = 1.0f - r1;
        const int i1 = i0 + 1, j1 = j0 + 1, k1 = k0 + 1;
        d[id] = r0 * (s0 * (t0 * d0[idx(i0, j0, k0)] + t1 * d0[idx(i0, j1, k0)]) +
                      s1 * (t0 * d0[idx(i1, j0, k0)] + t1 * d0[idx(i1, j1, k0)])) +
                r1 * (s0 * (t0 * d0[idx(i0, j0, k1)] + t1 * d0[idx(i0, j1, k1)]) +
                      s1 * (t0 * d0[idx(i1, j0, k1)] + t1 * d0[idx(i1, j1, k1)]));
      }
  set_bnd(b, d);
}

static void project(const gm_smoke_t* s, float* u, float* v, float* w, float* p, float* div) {
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        div[id] = -0.5f * (u[id + 1] - u[id - 1] + v[id + SX] - v[id - SX] + w[id + SX * SY] - w[id - SX * SY]);
        p[id] = 0.0f;
      }
  set_bnd(0, div);
  set_bnd(0, p);
  lin_solve(0, p, div, 1.0f, 6.0f, gm_maxi(1, gm_iround(s->iters)));
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        u[id] -= 0.5f * (p[id + 1] - p[id - 1]);
        v[id] -= 0.5f * (p[id + SX] - p[id - SX]);
        w[id] -= 0.5f * (p[id + SX * SY] - p[id - SX * SY]);
      }
  set_bnd(1, u);
  set_bnd(2, v);
  set_bnd(3, w);
}

static void smoke_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_smoke_t* s = (gm_smoke_t*)self;
  if (!s->d) {
    smoke_allocate(s);
    smoke_reset(self);
  }
  if (!s->d) return;  // out of memory
  // Run the solver at rate_hz regardless of the render rate (the dithered
  // rasterisation hides the lower physics rate). The schedule keeps its
  // remainder so the step rate is exact; each step advances by the real time
  // since the previous one, capped at 1/20 s for stability.
  s->accum += ctx->dt;
  s->since_step += ctx->dt;
  const float period = 1.0f / s->rate_hz;
  if (s->accum < period) return;
  s->accum -= period;
  if (s->accum > period) s->accum = 0.0f;  // drop backlog after a stall
  const float dt = gm_minf(s->since_step, 1.0f / 20.0f);
  s->since_step = 0.0f;
  const gm_vec3_t inertial = gm_gforce_inertial(ctx->g);

  // Forces. A uniform body force on a closed box is a pure pressure gradient
  // and the projection step would cancel it, so the inertial force is
  // weighted by density: the smoke behaves like a heavy gas that is thrown
  // around while the clear air stays put. Buoyancy lifts dense cells.
  // Grid j grows downward, so "up" is -v.
  const float fx = s->wind * inertial.x * dt;
  const float fy = -s->wind * inertial.y * dt;
  const float fz = s->wind * inertial.z * dt;
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        const float dens = s->d[id];
        s->u[id] += fx * dens;
        s->v[id] += (fy - s->buoyancy * dt) * dens;
        s->w[id] += fz * dens;
      }
  {
    // Emitter: a short cylinder centred in the volume, emit_radius cells wide
    // on a layer and emit_depth layers deep, so the plume starts compact and
    // longitudinal g visibly moves it through the stack.
    const float ci = (NX + 1) * 0.5f, cj = NY * 0.62f + 0.5f, ck = (NZ + 1) * 0.5f;
    const float r2 = s->emit_radius * s->emit_radius;
    for (int k = 1; k <= NZ; ++k)
      for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
          const float dx = (float)i - ci, dy = (float)j - cj;
          const float dz = ((float)k - ck) / s->emit_depth;
          if (dx * dx + dy * dy > r2 || dz * dz > 1.0f) continue;
          const int id = idx(i, j, k);
          s->d[id] += s->emit * dt;
          s->u[id] += s->swirl * gm_rng_symmetric(&s->rng) * dt * 10.0f;
          s->w[id] += s->swirl * gm_rng_symmetric(&s->rng) * dt * 10.0f;
        }
  }

  // Velocity step: project, advect, project.
  project(s, s->u, s->v, s->w, s->u0, s->v0);
  swap_ptr(&s->u, &s->u0);
  swap_ptr(&s->v, &s->v0);
  swap_ptr(&s->w, &s->w0);
  advect(1, s->u, s->u0, s->u0, s->v0, s->w0, dt);
  advect(2, s->v, s->v0, s->u0, s->v0, s->w0, dt);
  advect(3, s->w, s->w0, s->u0, s->v0, s->w0, dt);
  project(s, s->u, s->v, s->w, s->u0, s->v0);

  // Density step.
  swap_ptr(&s->d, &s->d0);
  advect(0, s->d, s->d0, s->u, s->v, s->w, dt);
  const float keep = gm_maxf(0.0f, 1.0f - s->dissipation * dt);
  for (int n = 0; n < N; ++n) s->d[n] *= keep;
}

static void smoke_render(gm_effect_t* self, gm_frame_t* f) {
  gm_smoke_t* s = (gm_smoke_t*)self;
  if (!s->d) return;
  const float threshold = 1.0f / 64.0f;
  for (int k = 1; k <= NZ; ++k) {
    const int layer = k - 1;
    for (int j = 1; j <= NY; ++j) {
      for (int i = 1; i <= NX; ++i) {
        const float val = s->d[idx(i, j, k)] * s->gain;
        if (val < threshold) continue;
        const int x0 = (i - 1) * CX, y0 = (j - 1) * CY;
        if (val >= 1.0f) {
          gm_frame_fill_rect(f, layer, x0, y0, x0 + CX - 1, y0 + CY - 1, 1.0f);
          continue;
        }
        for (int y = y0; y < y0 + CY; ++y)
          for (int x = x0; x < x0 + CX; ++x) gm_frame_plot(f, x, y, layer, val);
      }
    }
  }
}

static const gm_effect_vtbl_t k_smoke_vtbl = {
    .name = "smoke",
    .activate = smoke_activate,
    .deactivate = smoke_deactivate,
    .reset = smoke_reset,
    .update = smoke_update,
    .render = smoke_render,
    .burn_in_offset = NULL,
};

void gm_smoke_init(gm_smoke_t* s) {
  s->base.vt = &k_smoke_vtbl;
  s->wind = 30.0f;
  s->buoyancy = 3.0f;
  s->emit = 4.0f;
  s->emit_radius = 1.5f;
  s->emit_depth = 1.0f;
  s->dissipation = 1.2f;
  s->iters = 6.0f;
  s->rate_hz = 30.0f;
  s->accum = 0.0f;
  s->since_step = 0.0f;
  s->gain = 1.5f;
  s->swirl = 2.0f;
  s->u = s->v = s->w = s->u0 = s->v0 = s->w0 = s->d = s->d0 = NULL;
  gm_rng_seed(&s->rng, 4242u);

  gm_param_table_t* p = &s->base.params;
  gm_params_init(p);
  gm_params_add(p, "wind", &s->wind, 0.0f, 200.0f, "cells/s^2 per g");
  gm_params_add(p, "buoyancy", &s->buoyancy, 0.0f, 50.0f, "rise per unit density");
  gm_params_add(p, "emit", &s->emit, 0.0f, 200.0f, "emitter density/s");
  gm_params_add(p, "emit_r", &s->emit_radius, 0.75f, 4.0f, "emitter radius cells");
  gm_params_add(p, "emit_d", &s->emit_depth, 0.5f, 4.0f, "emitter half-depth layers");
  gm_params_add(p, "dissipate", &s->dissipation, 0.0f, 5.0f, "density decay 1/s");
  gm_params_add(p, "iters", &s->iters, 1.0f, 30.0f, "pressure solver iterations");
  gm_params_add(p, "rate", &s->rate_hz, 20.0f, 120.0f, "physics steps per second (20 minimum)");
  gm_params_add(p, "gain", &s->gain, 0.1f, 10.0f, "density to brightness");
  gm_params_add(p, "swirl", &s->swirl, 0.0f, 20.0f, "emitter turbulence");
}

void gm_smoke_deinit(gm_smoke_t* s) { smoke_release(s); }
