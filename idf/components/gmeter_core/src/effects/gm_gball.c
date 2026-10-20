// Classic g-meter ball, in 3D: the ball slides around the volume like a loose
// marble on the dashboard (lateral g -> left/right, longitudinal g -> toward or
// away from the viewer through the layer stack, vertical g -> up/down), with
// critically damped motion and a fading 3D trail. The front layer carries a
// HUD: reticle, 0.5 g / 1 g rings and a numeric readout.
#include "gm_gball.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// (int)v as an x86 build of the C++ computes it: INT_MIN for NaN, infinite and
// out-of-range values (the conversion is undefined there), without the
// undefined behaviour. Only absurd g input (NaN, inf, beyond ~2e7 g) reaches
// the readout with such values.
static int to_int(float v) { return v >= -2147483648.0f && v < 2147483648.0f ? (int)v : INT_MIN; }

static void gball_reset(gm_effect_t* self) {
  gm_gball_t* b = (gm_gball_t*)self;
  gm_spring3_snap(&b->spring, gm_vec3_zero());
  b->trail_head = b->trail_count = 0;
  b->trail_timer = 0.0f;
  b->peak_g = 0.0f;
  b->peak_age = 0.0f;
}

static gm_vec3_t gball_travel(const gm_gball_t* b) {
  const gm_vec3_t he = gm_half_extent();
  return gm_vec3((he.x - b->radius - 1.0f) / b->full_scale_g, (he.y - b->radius - 1.0f) / b->vertical_scale_g,
                 (he.z - GM_LAYER_PITCH_PX * 0.5f) / b->depth_scale_g);
}

static void gball_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_gball_t* b = (gm_gball_t*)self;
  b->last_g = ctx->g;
  const gm_vec3_t drive = b->follow_inertia > 0.5f ? gm_gforce_inertial(ctx->g) : gm_gforce_dynamic(ctx->g);
  const gm_vec3_t tr = gball_travel(b);
  const gm_vec3_t he = gm_half_extent();
  gm_vec3_t target = gm_vec3_mul(drive, tr);
  target.x = gm_clampf(target.x, -(he.x - b->radius - 1.0f), he.x - b->radius - 1.0f);
  target.y = gm_clampf(target.y, -(he.y - b->radius - 1.0f), he.y - b->radius - 1.0f);
  target.z = gm_clampf(target.z, -(he.z - GM_LAYER_PITCH_PX * 0.5f), he.z - GM_LAYER_PITCH_PX * 0.5f);
  gm_spring3_step(&b->spring, target, ctx->dt, b->natural_hz, b->damping);

  // Trail bookkeeping: sample at ~40 Hz, age out.
  for (int i = 0; i < b->trail_count; ++i) b->trail_age[i] += ctx->dt;
  // Space samples so the ring buffer spans the whole trail length.
  b->trail_timer += ctx->dt;
  const float interval = gm_maxf(0.025f, b->trail_seconds / (float)(GM_GBALL_TRAIL_MAX - 1));
  if (b->trail_seconds > 0.0f && b->trail_timer >= interval) {
    b->trail_timer -= interval;
    if (b->trail_timer > interval) b->trail_timer = 0.0f;
    b->trail[b->trail_head] = gm_spring3_pos(&b->spring);
    b->trail_age[b->trail_head] = 0.0f;
    b->trail_head = (b->trail_head + 1) % GM_GBALL_TRAIL_MAX;
    if (b->trail_count < GM_GBALL_TRAIL_MAX) ++b->trail_count;
  }

  // Peak hold with slow decay.
  const float hg = gm_gforce_horizontal(ctx->g);
  if (hg >= b->peak_g) {
    b->peak_g = hg;
    b->peak_age = 0.0f;
  } else {
    b->peak_age += ctx->dt;
    if (b->peak_age > 3.0f) b->peak_g = gm_maxf(hg, b->peak_g - 0.5f * ctx->dt);
  }
}

static void gball_render(gm_effect_t* self, gm_frame_t* f) {
  gm_gball_t* b = (gm_gball_t*)self;
  const gm_vec3_t pos = gm_spring3_pos(&b->spring);

  // Trail: older samples dimmer (dithered sparser).
  if (b->trail_seconds > 0.0f && b->trail_intensity > 0.0f) {
    for (int i = 0; i < b->trail_count; ++i) {
      const float age = b->trail_age[i];
      if (age > b->trail_seconds) continue;
      const float fade = 1.0f - age / b->trail_seconds;
      gm_frame_point(f, b->trail[i], b->trail_intensity * fade * fade);
    }
  }

  gm_frame_sphere(f, pos, b->radius, b->depth_radius_layers * GM_LAYER_PITCH_PX, 1.0f);

  if (b->rings > 0.5f) {
    // Reference ellipses at 0.5 g and 1 g on the front layer, using the same
    // per-axis mapping as the ball (clipped where they leave the panel), drawn
    // as sparse dashes so they stay dim but legible.
    const gm_vec3_t tr = gball_travel(b);
    const gm_vec3_t c = gm_world_to_voxel(gm_vec3_zero());
    for (int ring = 1; ring <= 2; ++ring) {
      const float gval = 0.5f * (float)ring;
      const float rx = tr.x * gval, ry = tr.y * gval;
      const float perimeter = GM_PI * (3.0f * (rx + ry) - sqrtf((3.0f * rx + ry) * (rx + 3.0f * ry)));
      const int n = gm_maxi(24, (int)perimeter);
      const int every = ring == 2 ? 3 : 5;
      for (int i = 0; i < n; i += every) {
        const float a = GM_TWO_PI * (float)i / (float)n;
        gm_frame_set(f, gm_iround(c.x + rx * cosf(a)), gm_iround(c.y + ry * sinf(a)), 0);
      }
    }
    // Reticle ticks.
    gm_frame_hline(f, 0, gm_iround(c.x) - 3, gm_iround(c.x) + 3, gm_iround(c.y));
    gm_frame_vline(f, 0, gm_iround(c.x), gm_iround(c.y) - 2, gm_iround(c.y) + 2);
  }

  if (b->hud > 0.5f) {
    char buf[12];
    const float hg = gm_gforce_horizontal(b->last_g);
    snprintf(buf, sizeof(buf), "%d.%02dG", to_int(hg), to_int(hg * 100.0f) % 100);
    gm_frame_text(f, 0, 1, GM_H - 6, buf);
    snprintf(buf, sizeof(buf), "P%d.%02d", to_int(b->peak_g), to_int(b->peak_g * 100.0f) % 100);
    gm_frame_text(f, 0, GM_W - 1 - 4 * 5, GM_H - 6, buf);
    // Longitudinal bar along the top edge: braking fills to the right of
    // centre (ball recedes), acceleration to the left (ball approaches).
    const int cx = GM_W / 2;
    const float lon = gm_clampf(gm_gforce_longitudinal(b->last_g) / b->depth_scale_g, -1.0f, 1.0f);
    const int len = gm_iround(lon * (GM_W / 2 - 2));
    gm_frame_hline(f, 0, cx, cx - len, 0);
    gm_frame_set(f, cx, 0, 0);
    gm_frame_set(f, cx, 1, 0);
  }
}

// No activate / deactivate hooks (activating resets) and no burn-in offset.
static const gm_effect_vtbl_t k_gball_vtbl = {
    .name = "ball",
    .activate = NULL,
    .deactivate = NULL,
    .reset = gball_reset,
    .update = gball_update,
    .render = gball_render,
    .burn_in_offset = NULL,
};

void gm_gball_init(gm_gball_t* b) {
  b->base.vt = &k_gball_vtbl;
  b->full_scale_g = 1.0f;
  b->vertical_scale_g = 0.6f;
  b->depth_scale_g = 1.0f;
  b->natural_hz = 1.8f;
  b->damping = 1.0f;
  b->radius = 5.0f;
  b->depth_radius_layers = 1.3f;
  b->trail_seconds = 1.2f;
  b->trail_intensity = 0.55f;
  b->follow_inertia = 1.0f;
  b->hud = 1.0f;
  b->rings = 1.0f;
  // The trail samples are only read once written; cleared for determinism.
  memset(b->trail, 0, sizeof(b->trail));
  memset(b->trail_age, 0, sizeof(b->trail_age));
  b->spring.x.pos = b->spring.x.vel = 0.0f;
  b->spring.y.pos = b->spring.y.vel = 0.0f;
  b->spring.z.pos = b->spring.z.vel = 0.0f;
  b->trail_head = b->trail_count = 0;
  b->trail_timer = 0.0f;
  b->peak_g = 0.0f;
  b->peak_age = 0.0f;
  b->last_g = gm_gforce_rest();

  gm_param_table_t* p = &b->base.params;
  gm_params_init(p);
  gm_params_add(p, "scale_g", &b->full_scale_g, 0.2f, 3.0f, "lateral g at full travel");
  gm_params_add(p, "vscale_g", &b->vertical_scale_g, 0.1f, 3.0f, "vertical g at full travel");
  gm_params_add(p, "dscale_g", &b->depth_scale_g, 0.2f, 3.0f, "longitudinal g at full depth");
  gm_params_add(p, "hz", &b->natural_hz, 0.3f, 8.0f, "spring natural frequency");
  gm_params_add(p, "zeta", &b->damping, 0.2f, 2.0f, "spring damping ratio (1 = critical)");
  gm_params_add(p, "radius", &b->radius, 1.0f, 16.0f, "ball radius px");
  gm_params_add(p, "depth_r", &b->depth_radius_layers, 0.6f, 4.0f, "ball half-depth in layers");
  gm_params_add(p, "trail_s", &b->trail_seconds, 0.0f, 4.0f, "trail persistence seconds");
  gm_params_add(p, "trail_i", &b->trail_intensity, 0.0f, 1.0f, "trail brightness");
  gm_params_add(p, "inertia", &b->follow_inertia, 0.0f, 1.0f, "1 = ball is thrown like a loose object");
  gm_params_add(p, "hud", &b->hud, 0.0f, 1.0f, "numeric readout on front layer");
  gm_params_add(p, "rings", &b->rings, 0.0f, 1.0f, "0.5g/1g reference rings");
  gball_reset(&b->base);
}
