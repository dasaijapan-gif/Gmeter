// Granular liquid: a few hundred particles thrown around the volume by the
// car's effective gravity. A coarse density grid supplies a pressure-like
// repulsion so the particles settle into a pile with volume instead of
// collapsing onto one pixel row (a tiny particle-in-cell fluid).
#include "gm_particles.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define CELL GM_PARTICLES_CELL
#define GX GM_PARTICLES_GX
#define GY GM_PARTICLES_GY
#define GZ GM_PARTICLES_GZ

// (int)v as an x86 build of the C++ computes it: INT_MIN for NaN, infinite and
// out-of-range values (the conversion is undefined there), without the
// undefined behaviour. Only non-finite positions (after NaN or infinite g
// input) reach such values here: in the grid lookups and in render.
static int to_int(float v) { return v >= -2147483648.0f && v < 2147483648.0f ? (int)v : INT_MIN; }

float gm_particles_gravity_px(const gm_particles_t* p) { return GM_STANDARD_GRAVITY * (float)GM_W / p->tank_width_m; }

static void particles_reset(gm_effect_t* self) {
  gm_particles_t* p = (gm_particles_t*)self;
  const gm_vec3_t he = gm_half_extent();
  gm_rng_seed(&p->rng, 777u);
  for (int i = 0; i < GM_PARTICLES_MAX; ++i) {
    // Start as a loose cloud in the lower half so it visibly settles.
    // (Separate statements: the C++ braced initialiser draws x, y, z in order.)
    const float x = gm_rng_range(&p->rng, -he.x * 0.8f, he.x * 0.8f);
    const float y = gm_rng_range(&p->rng, -he.y * 0.9f, 0.0f);
    const float z = gm_rng_range(&p->rng, -he.z * 0.8f, he.z * 0.8f);
    p->pos[i] = gm_vec3(x, y, z);
    p->vel[i] = gm_vec3_zero();
  }
}

// World -> continuous density-grid coordinates (padded grid, cell centres at
// integer + 0.5 offsets handled by the 0.5 shift below).
static inline gm_vec3_t grid_coord(gm_vec3_t world) {
  const gm_vec3_t v = gm_world_to_voxel(world);
  return gm_vec3(v.x / CELL + 0.5f, v.y / CELL + 0.5f, v.z + 1.0f);
}

static void splat_density(gm_particles_t* pe) {
  float(*density)[GY][GX] = pe->density;
  memset(pe->density, 0, sizeof(pe->density));
  const int n = gm_particles_active_count(pe);
  for (int p = 0; p < n; ++p) {
    const gm_vec3_t g = grid_coord(pe->pos[p]);
    const int i0 = gm_clampi(to_int(g.x), 0, GX - 2), j0 = gm_clampi(to_int(g.y), 0, GY - 2),
              k0 = gm_clampi(to_int(g.z), 0, GZ - 2);
    const float fx = gm_clampf(g.x - (float)i0, 0.0f, 1.0f), fy = gm_clampf(g.y - (float)j0, 0.0f, 1.0f),
                fz = gm_clampf(g.z - (float)k0, 0.0f, 1.0f);
    for (int dk = 0; dk < 2; ++dk) {
      const float wz = dk ? fz : 1.0f - fz;
      for (int dj = 0; dj < 2; ++dj) {
        const float wy = dj ? fy : 1.0f - fy;
        for (int di = 0; di < 2; ++di) {
          const float wx = di ? fx : 1.0f - fx;
          density[k0 + dk][j0 + dj][i0 + di] += wx * wy * wz;
        }
      }
    }
  }
  // Fold mass that landed in the padding back into the edge cells, then
  // mirror the edge cells into the padding so the density gradient at a wall
  // points into the pile (Neumann boundary) instead of toward the empty
  // padding, which would suck particles into the wall.
  for (int k = 0; k < GZ; ++k)
    for (int j = 0; j < GY; ++j) {
      density[k][j][1] += density[k][j][0];
      density[k][j][0] = density[k][j][1];
      density[k][j][GX - 2] += density[k][j][GX - 1];
      density[k][j][GX - 1] = density[k][j][GX - 2];
    }
  for (int k = 0; k < GZ; ++k)
    for (int i = 0; i < GX; ++i) {
      density[k][1][i] += density[k][0][i];
      density[k][0][i] = density[k][1][i];
      density[k][GY - 2][i] += density[k][GY - 1][i];
      density[k][GY - 1][i] = density[k][GY - 2][i];
    }
  for (int j = 0; j < GY; ++j)
    for (int i = 0; i < GX; ++i) {
      density[1][j][i] += density[0][j][i];
      density[0][j][i] = density[1][j][i];
      density[GZ - 2][j][i] += density[GZ - 1][j][i];
      density[GZ - 1][j][i] = density[GZ - 2][j][i];
    }
  // Pressure only where the pile is denser than rest, so it can settle.
  for (int k = 0; k < GZ; ++k)
    for (int j = 0; j < GY; ++j)
      for (int i = 0; i < GX; ++i) pe->pressure_field[k][j][i] = gm_maxf(density[k][j][i] - pe->rest_density, 0.0f);
}

static gm_vec3_t pressure_accel(const gm_particles_t* pe, gm_vec3_t world) {
  const gm_vec3_t g = grid_coord(world);
  // Grid coordinates are non-negative here, so truncation is a floor.
  const int i = gm_clampi(to_int(g.x + 0.5f), 1, GX - 2);
  const int j = gm_clampi(to_int(g.y + 0.5f), 1, GY - 2);
  const int k = gm_clampi(to_int(g.z + 0.5f), 1, GZ - 2);
  const float(*p)[GY][GX] = pe->pressure_field;
  // Central differences in cell units, converted to per-pixel.
  const float dx = (p[k][j][i + 1] - p[k][j][i - 1]) * (0.5f / CELL);
  const float dy = (p[k][j + 1][i] - p[k][j - 1][i]) * (0.5f / CELL);
  const float dz = (p[k + 1][j][i] - p[k - 1][j][i]) * (0.5f / GM_LAYER_PITCH_PX);
  // Grid y points down (screen rows); world y points up. a = -k * grad(p).
  // (Dividing by the local density, as SPH does, over-amplifies the force at
  // the sparse top of the pile and turns it into a fountain.)
  // pressure is the stiffness for a 1 m tank; it scales with gravity so the
  // pile keeps its shape when tank_m changes.
  return gm_vec3_scale(gm_vec3(-dx, dy, -dz), pe->pressure / pe->tank_width_m);
}

static void substep(gm_particles_t* pe, float dt, gm_vec3_t g_eff) {
  const float gpx = gm_particles_gravity_px(pe);
  const gm_vec3_t gravity = gm_vec3_scale(g_eff, gpx);
  const gm_vec3_t he = gm_half_extent();
  const gm_vec3_t lim = gm_vec3(he.x - 0.5f, he.y - 0.5f, he.z - 0.5f);
  const float drag_k = gm_maxf(0.0f, 1.0f - pe->drag * dt);
  // No particle may cross more than 0.8 cell per substep (no tunnelling).
  const float vmax = 0.8f * CELL / dt, vmax2 = vmax * vmax;
  const int n = gm_particles_active_count(pe);
  for (int p = 0; p < n; ++p) {
    gm_vec3_t a = gm_vec3_add(gravity, pressure_accel(pe, pe->pos[p]));
    if (pe->jitter > 0.0f) {
      // (Separate statements: the C++ braced initialiser draws x, y, z in order.)
      const float jx = gm_rng_symmetric(&pe->rng);
      const float jy = gm_rng_symmetric(&pe->rng);
      const float jz = gm_rng_symmetric(&pe->rng);
      a = gm_vec3_add(a, gm_vec3_scale(gm_vec3(jx, jy, jz), pe->jitter));
    }
    gm_vec3_t* v = &pe->vel[p];
    gm_vec3_t* x = &pe->pos[p];
    *v = gm_vec3_scale(gm_vec3_add(*v, gm_vec3_scale(a, dt)), drag_k);
    const float v2 = gm_vec3_length2(*v);
    if (v2 > vmax2) *v = gm_vec3_scale(*v, vmax / sqrtf(v2));
    *x = gm_vec3_add(*x, gm_vec3_scale(*v, dt));
    // Walls.
    if (x->x < -lim.x) {
      x->x = -lim.x;
      if (v->x < 0) v->x = -v->x * pe->restitution;
      v->y *= 0.9f;
      v->z *= 0.9f;
    }
    if (x->x > lim.x) {
      x->x = lim.x;
      if (v->x > 0) v->x = -v->x * pe->restitution;
      v->y *= 0.9f;
      v->z *= 0.9f;
    }
    if (x->y < -lim.y) {
      x->y = -lim.y;
      if (v->y < 0) v->y = -v->y * pe->restitution;
      v->x *= 0.9f;
      v->z *= 0.9f;
    }
    if (x->y > lim.y) {
      x->y = lim.y;
      if (v->y > 0) v->y = -v->y * pe->restitution;
      v->x *= 0.9f;
      v->z *= 0.9f;
    }
    if (x->z < -lim.z) {
      x->z = -lim.z;
      if (v->z < 0) v->z = -v->z * pe->restitution;
      v->x *= 0.9f;
      v->y *= 0.9f;
    }
    if (x->z > lim.z) {
      x->z = lim.z;
      if (v->z > 0) v->z = -v->z * pe->restitution;
      v->x *= 0.9f;
      v->y *= 0.9f;
    }
  }
}

static void particles_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_particles_t* p = (gm_particles_t*)self;
  const gm_vec3_t g_eff = gm_gforce_effective_gravity(ctx->g);
  const int steps = gm_maxi(1, gm_iround(p->substeps));
  const float dt = ctx->dt / (float)steps;
  for (int s = 0; s < steps; ++s) {
    splat_density(p);
    substep(p, dt, g_eff);
  }
}

static void particles_render(gm_effect_t* self, gm_frame_t* f) {
  gm_particles_t* pe = (gm_particles_t*)self;
  const int n = gm_particles_active_count(pe);
  const bool big = pe->dot_size >= 1.5f;
  for (int p = 0; p < n; ++p) {
    const gm_vec3_t v = gm_world_to_voxel(pe->pos[p]);
    // gm_iround without the undefined conversion of a NaN position.
    const int x = to_int(floorf(v.x + 0.5f)), y = to_int(floorf(v.y + 0.5f)), z = to_int(floorf(v.z + 0.5f));
    gm_frame_set(f, x, y, z);
    if (big) {
      gm_frame_set(f, x + 1, y, z);
      gm_frame_set(f, x, y + 1, z);
      gm_frame_set(f, x + 1, y + 1, z);
    }
  }
}

static const gm_effect_vtbl_t k_particles_vtbl = {
    .name = "particles",
    .activate = NULL,  // reset
    .deactivate = NULL,
    .reset = particles_reset,
    .update = particles_update,
    .render = particles_render,
    .burn_in_offset = NULL,
};

void gm_particles_init(gm_particles_t* p) {
  p->base.vt = &k_particles_vtbl;
  p->count = 300.0f;
  p->tank_width_m = 1.0f;
  p->drag = 0.6f;
  p->restitution = 0.25f;
  p->pressure = 12000.0f;
  p->rest_density = 0.5f;
  p->jitter = 15.0f;
  p->dot_size = 1.0f;
  p->substeps = 2.0f;
  memset(p->pos, 0, sizeof(p->pos));
  memset(p->vel, 0, sizeof(p->vel));
  memset(p->density, 0, sizeof(p->density));
  memset(p->pressure_field, 0, sizeof(p->pressure_field));
  gm_rng_seed(&p->rng, 777u);

  gm_param_table_t* t = &p->base.params;
  gm_params_init(t);
  gm_params_add(t, "count", &p->count, 1.0f, (float)GM_PARTICLES_MAX, "particles");
  gm_params_add(t, "tank_m", &p->tank_width_m, 0.1f, 10.0f, "virtual tank width (m)");
  gm_params_add(t, "drag", &p->drag, 0.0f, 10.0f, "air drag 1/s");
  gm_params_add(t, "bounce", &p->restitution, 0.0f, 1.0f, "wall restitution");
  gm_params_add(t, "pressure", &p->pressure, 0.0f, 80000.0f, "repulsion stiffness");
  gm_params_add(t, "rho0", &p->rest_density, 0.0f, 4.0f, "rest density (particles/cell)");
  gm_params_add(t, "jitter", &p->jitter, 0.0f, 200.0f, "random agitation");
  gm_params_add(t, "dot", &p->dot_size, 1.0f, 2.0f, "particle size px");
  gm_params_add(t, "substeps", &p->substeps, 1.0f, 6.0f, "physics substeps");
  particles_reset(&p->base);
}
