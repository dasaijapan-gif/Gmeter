// Sloshing liquid: a height field on the horizontal (lateral x depth) plane
// integrated with the nonlinear shallow-water equations in flux form, driven
// by the car's effective gravity. The free surface tilts so its slope equals
// a_horizontal / g_vertical, exactly like coffee in a cup holder, and waves
// travel at sqrt(g * depth).
//
// Grid: NX cells across the panel (dx = GM_W/NX pixels), NZ = GM_D cells
// through the stack (dz = layer pitch). Staggered (Arakawa C) layout: h at cell
// centres, u on x-faces, w on z-faces. Units are pixels and seconds; gravity
// is scaled by a "virtual tank width" so a 35 mm display sloshes like a
// 1 m tank instead of buzzing at 5 Hz.
#include "gm_slosh.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define NX GM_SLOSH_NX
#define NZ GM_SLOSH_NZ
#define DX GM_SLOSH_DX
#define DZ GM_SLOSH_DZ

// (int)v as an x86 build of the C++ computes it: INT_MIN for NaN, infinite and
// out-of-range values (the conversion is undefined there), without the
// undefined behaviour. Only absurd g input (NaN, inf, millions of g) gets such
// values here: in the substep count and in the surface row.
static int to_int(float v) { return v >= -2147483648.0f && v < 2147483648.0f ? (int)v : INT_MIN; }

float gm_slosh_gravity_px(const gm_slosh_t* s) {
  // 1 g in pixels/s^2 given the virtual tank width.
  const float px_per_metre = (float)GM_W / s->tank_width_m;
  return GM_STANDARD_GRAVITY * px_per_metre;
}

static void slosh_reset(gm_effect_t* self) {
  gm_slosh_t* s = (gm_slosh_t*)self;
  const float h0 = s->fill_fraction * (float)GM_H;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) s->h[z][x] = h0;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x <= NX; ++x) s->u[z][x] = 0.0f;
  for (int z = 0; z <= NZ; ++z)
    for (int x = 0; x < NX; ++x) s->w[z][x] = 0.0f;
  s->target_volume = h0 * NX * NZ;
  gm_spring_snap(&s->bob, 0.0f);
  s->last_vert_g = 0.0f;
  s->gv_filtered = 1.0f;
}

float gm_slosh_total_volume(const gm_slosh_t* s) {
  float v = 0.0f;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) v += s->h[z][x];
  return v;
}

static void substep(gm_slosh_t* s, float dt, float gv, gm_vec3_t g_eff) {
  const float gpx = gm_slosh_gravity_px(s);
  const float ax = s->slope_gain * g_eff.x * gpx;  // lateral body force (px/s^2)
  const float az = s->slope_gain * g_eff.z * gpx;  // longitudinal body force
  const float damp = gm_maxf(0.0f, 1.0f - s->damping * dt);
  const float k_dry = 0.05f;
  const float inv_dx = 1.0f / DX, inv_dz = 1.0f / DZ;

  // Momentum on interior faces (forward step). Walls stay at zero velocity
  // (reflecting); faces between two dry cells carry nothing.
  for (int z = 0; z < NZ; ++z) {
    s->u[z][0] = 0.0f;
    s->u[z][NX] = 0.0f;
    for (int x = 1; x < NX; ++x) {
      if (s->h[z][x] < k_dry && s->h[z][x - 1] < k_dry) {
        s->u[z][x] = 0.0f;
        continue;
      }
      const float dhdx = (s->h[z][x] - s->h[z][x - 1]) * inv_dx;
      s->u[z][x] = (s->u[z][x] + dt * (-gv * dhdx + ax)) * damp;
    }
  }
  for (int x = 0; x < NX; ++x) {
    s->w[0][x] = 0.0f;
    s->w[NZ][x] = 0.0f;
  }
  for (int z = 1; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      if (s->h[z][x] < k_dry && s->h[z - 1][x] < k_dry) {
        s->w[z][x] = 0.0f;
        continue;
      }
      const float dhdz = (s->h[z][x] - s->h[z - 1][x]) * inv_dz;
      s->w[z][x] = (s->w[z][x] + dt * (-gv * dhdz + az)) * damp;
    }
  }

  // Mass fluxes with upwind depth.
  for (int z = 0; z < NZ; ++z) {
    s->flux_x[z][0] = s->flux_x[z][NX] = 0.0f;
    for (int x = 1; x < NX; ++x) {
      const float u = s->u[z][x];
      s->flux_x[z][x] = u * (u > 0.0f ? s->h[z][x - 1] : s->h[z][x]);
    }
  }
  for (int x = 0; x < NX; ++x) s->flux_z[0][x] = s->flux_z[NZ][x] = 0.0f;
  for (int z = 1; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      const float w = s->w[z][x];
      s->flux_z[z][x] = w * (w > 0.0f ? s->h[z - 1][x] : s->h[z][x]);
    }
  }
  // Outflow limiter: a cell may not export more volume in one step than it
  // holds, so heights never go negative and mass is conserved exactly.
  for (int z = 0; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      float out = 0.0f;
      if (s->flux_x[z][x + 1] > 0.0f) out += s->flux_x[z][x + 1] * DZ;
      if (s->flux_x[z][x] < 0.0f) out -= s->flux_x[z][x] * DZ;
      if (s->flux_z[z + 1][x] > 0.0f) out += s->flux_z[z + 1][x] * DX;
      if (s->flux_z[z][x] < 0.0f) out -= s->flux_z[z][x] * DX;
      const float avail = s->h[z][x] * DX * DZ;
      if (out * dt > avail && out > 0.0f) {
        const float k = avail / (out * dt);
        if (s->flux_x[z][x + 1] > 0.0f) s->flux_x[z][x + 1] *= k;
        if (s->flux_x[z][x] < 0.0f) s->flux_x[z][x] *= k;
        if (s->flux_z[z + 1][x] > 0.0f) s->flux_z[z + 1][x] *= k;
        if (s->flux_z[z][x] < 0.0f) s->flux_z[z][x] *= k;
      }
    }
  }
  // Continuity (backward step with the new fluxes).
  for (int z = 0; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      const float div =
          (s->flux_x[z][x + 1] - s->flux_x[z][x]) * inv_dx + (s->flux_z[z + 1][x] - s->flux_z[z][x]) * inv_dz;
      s->h[z][x] = gm_maxf(0.0f, s->h[z][x] - dt * div);
    }
  }
}

static void smooth(gm_slosh_t* s, float amount) {
  // Conservative diffusion along x (the fine axis): flux form with zero-flux
  // walls, so the total volume is untouched. amount = nu*dt/dx^2 <= 0.25.
  if (amount <= 0.0f) return;
  amount = gm_minf(amount, 0.25f);
  for (int z = 0; z < NZ; ++z) {
    float carry = 0.0f;  // flux into cell x from cell x-1
    for (int x = 0; x < NX; ++x) {
      const float next = x + 1 < NX ? amount * (s->h[z][x + 1] - s->h[z][x]) : 0.0f;
      s->h[z][x] += next - carry;
      carry = next;
    }
  }
}

static void renormalize(gm_slosh_t* s) {
  const float v = gm_slosh_total_volume(s);
  if (v > 1e-3f) {
    const float k = s->target_volume / v;
    for (int z = 0; z < NZ; ++z)
      for (int x = 0; x < NX; ++x) s->h[z][x] *= k;
  }
}

static void slosh_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_slosh_t* s = (gm_slosh_t*)self;
  const gm_vec3_t g_eff = gm_gforce_effective_gravity(ctx->g);
  // Vertical jolts: whole-body bob plus a splash proportional to jerk, both
  // driven by the raw vertical g so bumps stay crisp...
  const float vert_g = gm_gforce_vertical(ctx->g);
  gm_spring_step(&s->bob, -vert_g * s->bob_px_per_g, ctx->dt, s->bob_hz, 0.35f);
  const float jerk = ctx->dt > 0.0f ? fabsf(vert_g - s->last_vert_g) / ctx->dt : 0.0f;
  s->last_vert_g = vert_g;
  if (jerk > 2.0f && s->splash_gain > 0.0f) {
    const float kick = gm_minf(jerk, 30.0f) * s->splash_gain * ctx->dt;
    for (int z = 0; z < NZ; ++z)
      for (int x = 1; x < NX; ++x) s->u[z][x] += kick * gm_rng_symmetric(&s->rng);
    for (int z = 1; z < NZ; ++z)
      for (int x = 0; x < NX; ++x) s->w[z][x] += kick * 0.5f * gm_rng_symmetric(&s->rng);
  }
  if (s->ripple > 0.0f) {
    for (int i = 0; i < 4; ++i) {
      const int x = 1 + (int)(gm_rng_uniform(&s->rng) * (NX - 1));
      const int z = (int)(gm_rng_uniform(&s->rng) * NZ);
      s->u[z][x] += s->ripple * ctx->dt * gm_rng_symmetric(&s->rng);
    }
  }
  // ...while the pressure term sees a ~1 Hz low-passed, clamped vertical g so a
  // pothole does not triple the surface slope for one frame.
  s->gv_filtered = gm_lerpf(s->gv_filtered, gm_clampf(-g_eff.y, 0.2f, 3.0f), ctx->dt / (ctx->dt + 0.16f));
  const float gv = s->gv_filtered * gm_slosh_gravity_px(s);

  // A runtime change of `fill` eases the volume toward the new target.
  const float want = s->fill_fraction * (float)GM_H * NX * NZ;
  s->target_volume += (want - s->target_volume) * gm_minf(1.0f, ctx->dt * 3.0f);

  // CFL from the current state: signal speed |u| + sqrt(gv * h) over both
  // axes, Courant target 0.7. The substep count is capped for cost; when the
  // cap binds the frame runs in slow motion instead of going unstable.
  float hmax = 0.0f, umax = 0.0f;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) hmax = gm_maxf(hmax, s->h[z][x]);
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x <= NX; ++x) umax = gm_maxf(umax, fabsf(s->u[z][x]));
  for (int z = 0; z <= NZ; ++z)
    for (int x = 0; x < NX; ++x) umax = gm_maxf(umax, fabsf(s->w[z][x]));
  const float c = umax + sqrtf(gv * gm_maxf(hmax, 1.0f));
  const float inv_spacing = sqrtf(1.0f / (DX * DX) + 1.0f / (DZ * DZ));
  const float dt_max = 0.7f / (gm_maxf(c, 1e-3f) * inv_spacing);
  // With absurd input (infinite velocities) the ratio is infinite or NaN; the
  // C++ then gets INT_MIN on x86 and clamps it to one substep. to_int does the
  // same without the undefined conversion.
  int n = to_int(ceilf(ctx->dt / dt_max));
  n = gm_clampi(n, 1, (int)s->max_substeps);
  s->last_substeps = n;
  const float dt = gm_minf(ctx->dt / (float)n, dt_max);
  for (int i = 0; i < n; ++i) {
    substep(s, dt, gv, g_eff);
    smooth(s, s->viscosity * dt / (DX * DX));
  }
  renormalize(s);  // only corrects float round-off now that the limiter keeps h >= 0
}

static void slosh_render(gm_effect_t* self, gm_frame_t* f) {
  gm_slosh_t* s = (gm_slosh_t*)self;
  const float bottom = (float)GM_H - 1.0f;
  const float bob = s->bob.pos;
  const int thick = gm_maxi(1, gm_iround(s->surface_px));
  const int style = gm_iround(s->style);
  for (int z = 0; z < NZ; ++z) {
    for (int px = 0; px < GM_W; ++px) {
      // Linear interpolation between cell centres along x.
      const float cx = ((float)px + 0.5f) / DX - 0.5f;
      int i0 = gm_ifloor(cx);
      const float t = cx - (float)i0;
      i0 = gm_clampi(i0, 0, NX - 1);
      const int i1 = gm_clampi(i0 + 1, 0, NX - 1);
      const float h = gm_lerpf(s->h[z][i0], s->h[z][i1], t) + bob;
      // gm_iround(bottom - h); a NaN or huge h (only after absurd vertical g)
      // gives INT_MIN, as the x86 build of the C++ does.
      const int y_surf = to_int(floorf(bottom - h + 0.5f));
      if (h < 0.5f) continue;
      for (int k = 0; k < thick; ++k) gm_frame_set(f, px, y_surf + k, z);
      if (style == 1) {
        // The C++ starts at y_surf + thick; rows above the panel draw nothing,
        // so start at row 0 (a NaN or huge h, only after absurd vertical g,
        // would otherwise mean up to ~2^31 iterations per column).
        for (int y = gm_maxi(y_surf + thick, 0); y <= GM_H - 1; ++y) gm_frame_plot(f, px, y, z, s->body_intensity);
      } else if (style == 2) {
        gm_frame_vline(f, z, px, y_surf + thick, GM_H - 1);
      }
    }
  }
}

// No activate / deactivate hooks (activating resets) and no burn-in offset.
static const gm_effect_vtbl_t k_slosh_vtbl = {
    .name = "slosh",
    .activate = NULL,
    .deactivate = NULL,
    .reset = slosh_reset,
    .update = slosh_update,
    .render = slosh_render,
    .burn_in_offset = NULL,
};

void gm_slosh_init(gm_slosh_t* s) {
  s->base.vt = &k_slosh_vtbl;
  s->fill_fraction = 0.42f;
  s->tank_width_m = 1.0f;
  s->slope_gain = 0.5f;
  s->damping = 0.6f;
  s->viscosity = 8.0f;
  s->ripple = 0.0f;
  s->bob_px_per_g = 10.0f;
  s->bob_hz = 3.0f;
  s->splash_gain = 60.0f;
  s->style = 1.0f;
  s->body_intensity = 0.12f;
  s->surface_px = 1.0f;
  s->max_substeps = 12.0f;
  // The flux buffers are written before every read; cleared for determinism.
  memset(s->flux_x, 0, sizeof(s->flux_x));
  memset(s->flux_z, 0, sizeof(s->flux_z));
  s->target_volume = 0.0f;
  gm_spring_snap(&s->bob, 0.0f);
  s->last_vert_g = 0.0f;
  s->gv_filtered = 1.0f;
  s->last_substeps = 0;
  gm_rng_seed(&s->rng, 12345u);

  gm_param_table_t* p = &s->base.params;
  gm_params_init(p);
  gm_params_add(p, "fill", &s->fill_fraction, 0.1f, 0.8f, "rest depth / panel height");
  gm_params_add(p, "tank_m", &s->tank_width_m, 0.1f, 10.0f, "virtual tank width (m); bigger = slower slosh");
  gm_params_add(p, "slope_gain", &s->slope_gain, 0.1f, 2.0f, "tilt per g (1 = physical)");
  gm_params_add(p, "damping", &s->damping, 0.0f, 10.0f, "velocity damping 1/s");
  gm_params_add(p, "viscosity", &s->viscosity, 0.0f, 60.0f, "surface diffusion px^2/s");
  gm_params_add(p, "ripple", &s->ripple, 0.0f, 50.0f, "idle ripple strength");
  gm_params_add(p, "bob", &s->bob_px_per_g, 0.0f, 30.0f, "vertical bounce px per g");
  gm_params_add(p, "bob_hz", &s->bob_hz, 0.5f, 10.0f, "vertical bounce frequency");
  gm_params_add(p, "splash", &s->splash_gain, 0.0f, 300.0f, "splash on vertical jerk");
  gm_params_add(p, "style", &s->style, 0.0f, 2.0f, "0 surface, 1 surface+body, 2 solid");
  gm_params_add(p, "body_i", &s->body_intensity, 0.0f, 1.0f, "body dither density");
  gm_params_add(p, "surface_px", &s->surface_px, 1.0f, 4.0f, "surface thickness px");
  gm_params_add(p, "substeps", &s->max_substeps, 2.0f, 32.0f,
                "max substeps per frame (cost cap; slow motion if exceeded)");
  slosh_reset(&s->base);
}
