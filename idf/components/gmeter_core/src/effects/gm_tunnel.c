// Depth tunnel: one ring per layer, shrinking toward the back, whose centres
// are displaced progressively by the inertial force so the whole tunnel
// bends toward where a loose object would fly. Dashes rotate with lateral g
// and brightness pulses travel through the depth with longitudinal g, which
// makes braking/acceleration readable as motion along the stack.
#include "gm_tunnel.h"

#include <math.h>

static void tunnel_reset(gm_effect_t* self) {
  gm_tunnel_t* t = (gm_tunnel_t*)self;
  gm_spring3_snap(&t->offs, gm_vec3_zero());
  t->phase = 0.0f;
  t->depth_phase = 0.0f;
}

static void tunnel_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_tunnel_t* t = (gm_tunnel_t*)self;
  const gm_vec3_t he = gm_half_extent();
  const gm_vec3_t inertial = gm_gforce_inertial(ctx->g);
  // Full bend puts the back ring at the edge of the panel.
  const gm_vec3_t target = gm_vec3(inertial.x * gm_maxf(0.0f, he.x - t->r_back - 1.0f) / t->scale_g,
                                   inertial.y * gm_maxf(0.0f, he.y - t->r_back - 1.0f) / (t->scale_g * 0.6f), 0.0f);
  gm_spring3_step(&t->offs, target, ctx->dt, t->hz, 1.0f);
  const float lat = gm_gforce_lateral(ctx->g);
  t->phase += ctx->dt * (t->spin + t->spin_per_g * lat);
  // Braking (inertial.z > 0) sends pulses toward the windshield.
  t->depth_phase += ctx->dt * (t->pulse_hz + t->pulse_per_g * inertial.z);
  if (t->phase > GM_TWO_PI) t->phase -= GM_TWO_PI;
  if (t->phase < 0.0f) t->phase += GM_TWO_PI;
  t->depth_phase -= floorf(t->depth_phase);
}

static void tunnel_render(gm_effect_t* self, gm_frame_t* f) {
  gm_tunnel_t* tn = (gm_tunnel_t*)self;
  const gm_vec3_t he = gm_half_extent();
  const gm_vec3_t off = gm_spring3_pos(&tn->offs);
  const int segs = gm_maxi(1, gm_iround(tn->segments));
  const float seg_arc = GM_TWO_PI / (float)segs;
  for (int k = 0; k < GM_D; ++k) {
    const float t = GM_D > 1 ? (float)k / (float)(GM_D - 1) : 0.0f;
    const float r = gm_lerpf(tn->r_front, tn->r_back, t);
    const float mx = gm_maxf(0.0f, he.x - r - 1.0f), my = gm_maxf(0.0f, he.y - r - 1.0f);
    const float cxw = gm_clampf(off.x * t, -mx, mx);
    const float cyw = gm_clampf(off.y * t, -my, my);
    const gm_vec3_t c = gm_world_to_voxel(gm_vec3(cxw, cyw, gm_layer_z(k)));
    const float pulse = 0.55f + 0.45f * cosf(GM_TWO_PI * (tn->depth_phase - t));
    const int n = gm_maxi(16, (int)(r * 6.0f));
    for (int i = 0; i < n; ++i) {
      const float a = GM_TWO_PI * (float)i / (float)n;
      float local = fmodf(a + tn->phase * (1.0f + 0.5f * t), seg_arc);
      if (local < 0.0f) local += seg_arc;
      if (local > seg_arc * 0.6f) continue;  // gap
      gm_frame_plot(f, gm_iround(c.x + r * cosf(a)), gm_iround(c.y + r * sinf(a)), k, pulse);
    }
  }
}

static const gm_effect_vtbl_t k_tunnel_vtbl = {
    .name = "tunnel",
    .activate = NULL,  // reset
    .deactivate = NULL,
    .reset = tunnel_reset,
    .update = tunnel_update,
    .render = tunnel_render,
    .burn_in_offset = NULL,
};

void gm_tunnel_init(gm_tunnel_t* t) {
  t->base.vt = &k_tunnel_vtbl;
  t->scale_g = 1.0f;
  t->r_front = (float)(GM_H / 2 - 2);
  t->r_back = 5.0f;
  t->segments = 10.0f;
  t->spin = 0.4f;
  t->spin_per_g = 4.0f;
  t->pulse_hz = 0.3f;
  t->pulse_per_g = 2.5f;
  t->hz = 2.5f;
  gm_spring3_snap(&t->offs, gm_vec3_zero());
  t->phase = 0.0f;
  t->depth_phase = 0.0f;

  gm_param_table_t* p = &t->base.params;
  gm_params_init(p);
  gm_params_add(p, "scale_g", &t->scale_g, 0.2f, 3.0f, "g for full bend");
  gm_params_add(p, "r_front", &t->r_front, 4.0f, (float)(GM_H / 2 - 1), "front ring radius px");
  gm_params_add(p, "r_back", &t->r_back, 1.0f, 20.0f, "back ring radius px");
  gm_params_add(p, "segments", &t->segments, 1.0f, 24.0f, "dashes per ring");
  gm_params_add(p, "spin", &t->spin, 0.0f, 5.0f, "idle rotation rad/s");
  gm_params_add(p, "spin_g", &t->spin_per_g, 0.0f, 20.0f, "rotation per lateral g");
  gm_params_add(p, "pulse", &t->pulse_hz, 0.0f, 3.0f, "idle depth pulses/s");
  gm_params_add(p, "pulse_g", &t->pulse_per_g, 0.0f, 10.0f, "depth pulses per longitudinal g");
  gm_params_add(p, "hz", &t->hz, 0.3f, 8.0f, "smoothing");
  tunnel_reset(&t->base);
}
