// "A.T. Field" shockwaves, after the concentric octagons in Neon Genesis
// Evangelion. Ripples start where the g-force pushes (the wall a loose object
// would hit) and spread as a spherical wavefront through the panel stack, so
// each panel shows an octagon of a different size. Sustained g keeps
// emitting; every 0.25 g crossed and every bump fires a stronger ripple.
#include "gm_atfield.h"

#include <math.h>
#include <stdlib.h>

#include "gm_jp_text.h"

// Line with dithered intensity (gm_frame_line is always full on).
static void dim_line(gm_frame_t* f, int z, int x0, int y0, int x1, int y1, float intensity) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int steps = gm_maxi(dx, -dy) + 1;
  for (int i = 0; i <= steps; ++i) {
    gm_frame_plot(f, x0, y0, z, intensity);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// Regular octagon with flat top/bottom (vertices at 22.5 + k * 45 degrees).
static void octagon(gm_frame_t* f, int z, float cx, float cy, float r, float intensity) {
  if (r < 1.0f) return;
  int px[8], py[8];
  for (int k = 0; k < 8; ++k) {
    const float a = GM_PI / 8.0f + (float)k * GM_PI / 4.0f;
    px[k] = gm_iround(cx + r * cosf(a));
    py[k] = gm_iround(cy + r * sinf(a));
  }
  for (int k = 0; k < 8; ++k) dim_line(f, z, px[k], py[k], px[(k + 1) % 8], py[(k + 1) % 8], intensity);
}

static void atfield_reset(gm_effect_t* self) {
  gm_atfield_t* a = (gm_atfield_t*)self;
  for (int i = 0; i < GM_ATFIELD_MAX_RIPPLES; ++i) {
    gm_atfield_ripple_t* r = &a->ripples[i];
    r->origin = gm_vec3_zero();
    r->age = 0.0f;
    r->strength = 0.0f;
    r->active = false;
  }
  gm_gstate_reset(&a->tracker);
  a->emit_accum = 0.0f;
  a->idle_timer = 0.0f;
  a->last_step = 0;
  a->horiz = 0.0f;
  a->time = 0.0f;
  a->caption_age = 99.0f;
}

int gm_atfield_active_ripples(const gm_atfield_t* a) {
  int n = 0;
  for (int i = 0; i < GM_ATFIELD_MAX_RIPPLES; ++i) n += a->ripples[i].active ? 1 : 0;
  return n;
}

static void spawn(gm_atfield_t* a, gm_vec3_t origin, float strength) {
  gm_atfield_ripple_t* slot = &a->ripples[0];
  for (int i = 0; i < GM_ATFIELD_MAX_RIPPLES; ++i) {
    gm_atfield_ripple_t* r = &a->ripples[i];
    if (!r->active) {
      slot = r;
      break;
    }
    if (r->age > slot->age) slot = r;  // reuse the oldest
  }
  slot->origin = origin;
  slot->age = 0.0f;
  slot->strength = gm_clampf(strength, 0.1f, 1.0f);
  slot->active = true;
  if (strength >= 0.6f) a->caption_age = 0.0f;
}

static void atfield_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_atfield_t* a = (gm_atfield_t*)self;
  a->time += ctx->dt;
  if (a->time >= 60.0f) a->time -= 60.0f;  // blink clock only; keeps float precision after days on
  a->caption_age += ctx->dt;
  for (int i = 0; i < GM_ATFIELD_MAX_RIPPLES; ++i) {
    gm_atfield_ripple_t* r = &a->ripples[i];
    if (r->active && (r->age += ctx->dt) > a->life) r->active = false;
  }

  a->tracker.bump_g = a->bump_g;
  bool bumped = false;
  gm_gstate_update(&a->tracker, ctx->g, ctx->dt, &bumped);
  const gm_vec3_t he = gm_half_extent();
  if (bumped) spawn(a, gm_vec3(0.0f, -he.y, 0.0f), 1.0f);  // from the floor

  // Impact point: where the inertial force pushes, on the volume's boundary.
  const gm_vec3_t in = gm_gforce_inertial(ctx->g);
  a->horiz = gm_gforce_horizontal(ctx->g);
  const float mag = gm_vec3_length(in);
  gm_vec3_t origin = gm_vec3_zero();
  if (mag > 1e-3f) {
    const gm_vec3_t dir = gm_vec3_div(in, mag);
    float t = 1e9f;
    if (fabsf(dir.x) > 1e-4f) t = gm_minf(t, he.x / fabsf(dir.x));
    if (fabsf(dir.y) > 1e-4f) t = gm_minf(t, he.y / fabsf(dir.y));
    if (fabsf(dir.z) > 1e-4f) t = gm_minf(t, he.z / fabsf(dir.z));
    origin = gm_vec3_scale(dir, t * 0.75f);  // inside the wall, so the octagons stay on the panels
  }

  // Each multiple of step_g crossed upward fires a strong ripple.
  const int step = (int)(a->horiz / a->step_g);
  if (step > a->last_step) spawn(a, origin, 0.6f + 0.2f * (float)step);
  a->last_step = step;

  // Sustained g keeps emitting, faster for stronger g.
  if (a->horiz > a->min_g) {
    a->emit_accum += ctx->dt * a->rate_g * (a->horiz - a->min_g);
    if (a->emit_accum >= 1.0f) {
      a->emit_accum -= 1.0f;
      spawn(a, origin, 0.3f + a->horiz);
    }
    a->idle_timer = 0.0f;
  } else {
    a->emit_accum = 0.0f;
    a->idle_timer += ctx->dt;
    if (a->idle > 0.5f && a->idle_timer > 2.8f) {
      a->idle_timer = 0.0f;
      spawn(a, gm_vec3(0.0f, 0.0f, he.z * 0.6f), 0.45f);  // calm heartbeat from deep in the stack
    }
  }
}

static void atfield_render(gm_effect_t* self, gm_frame_t* f) {
  gm_atfield_t* a = (gm_atfield_t*)self;
  for (int i = 0; i < GM_ATFIELD_MAX_RIPPLES; ++i) {
    const gm_atfield_ripple_t* r = &a->ripples[i];
    if (!r->active) continue;
    const float R = a->speed * r->age;
    const float fade = r->strength * (1.0f - r->age / a->life);
    const gm_vec3_t c = gm_world_to_voxel(r->origin);
    for (int k = 0; k < GM_D; ++k) {
      const float dz = gm_layer_z(k) - r->origin.z;
      if (fabsf(dz) >= R) continue;
      const float rl = sqrtf(R * R - dz * dz);
      octagon(f, k, c.x, c.y, rl, fade);
      if (a->double_ring > 0.5f && rl > 6.0f) octagon(f, k, c.x, c.y, rl - 3.0f, fade * 0.6f);
    }
  }
  if (a->label > 0.5f && a->caption_age < 1.5f) {
    gm_jp_draw(f, &gm_jp_small, 0, 4, GM_H - gm_jp_small.height - 2, "A.T.フィールド", 1, 1.0f);
    if (a->horiz > 0.8f && fmodf(a->time, 0.4f) < 0.25f) {
      const int w = gm_jp_measure(&gm_jp_small, "全開", 1);
      gm_jp_draw(f, &gm_jp_small, 0, GM_W - 4 - w, 2, "全開", 1, 1.0f);
    }
  }
}

static const gm_effect_vtbl_t k_atfield_vtbl = {
    .name = "atfield",
    .activate = NULL,  // reset
    .deactivate = NULL,
    .reset = atfield_reset,
    .update = atfield_update,
    .render = atfield_render,
    .burn_in_offset = NULL,
};

void gm_atfield_init(gm_atfield_t* a) {
  a->base.vt = &k_atfield_vtbl;
  a->speed = 70.0f;
  a->life = 1.6f;
  a->rate_g = 2.5f;
  a->min_g = 0.15f;
  a->step_g = 0.25f;
  a->idle = 1.0f;
  a->double_ring = 1.0f;
  a->label = 1.0f;
  a->bump_g = 0.3f;
  for (int i = 0; i < GM_ATFIELD_MAX_RIPPLES; ++i) {
    gm_atfield_ripple_t* r = &a->ripples[i];
    r->origin = gm_vec3_zero();
    r->age = 0.0f;
    r->strength = 0.0f;
    r->active = false;
  }
  gm_gstate_init(&a->tracker);
  a->emit_accum = 0.0f;
  a->idle_timer = 0.0f;
  a->last_step = 0;
  a->horiz = 0.0f;
  a->time = 0.0f;
  a->caption_age = 99.0f;

  gm_param_table_t* p = &a->base.params;
  gm_params_init(p);
  gm_params_add(p, "speed", &a->speed, 10.0f, 300.0f, "wavefront speed px/s");
  gm_params_add(p, "life", &a->life, 0.3f, 5.0f, "seconds a ripple lives");
  gm_params_add(p, "rate_g", &a->rate_g, 0.0f, 20.0f, "ripples per second per g");
  gm_params_add(p, "min_g", &a->min_g, 0.0f, 1.0f, "g below which only the idle pulse runs");
  gm_params_add(p, "step_g", &a->step_g, 0.05f, 1.0f, "strong ripple each time g crosses a multiple");
  gm_params_add(p, "idle", &a->idle, 0.0f, 1.0f, "slow pulse from the centre when calm");
  gm_params_add(p, "double", &a->double_ring, 0.0f, 1.0f, "inner second octagon");
  gm_params_add(p, "label", &a->label, 0.0f, 1.0f, "A.T.フィールド / 全開 captions");
  gm_params_add(p, "bump_g", &a->bump_g, 0.05f, 2.0f, "vertical spike that fires a ripple");
  atfield_reset(&a->base);
}
