// G-mapped authored animation. Takes any 1-bit sprite sheet (export one from
// a PNG/GIF with tools/img2sprite.py) and lets the g-force drive it: position
// in the volume (including which layer it sits on), which frame is shown
// (scrubbing by g on a chosen axis) and how fast it plays. This is the
// generic hook for "make my animation react to g".
#include "gm_sprite.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

// (int)v as an x86 build of the C++ computes it: INT_MIN for NaN, infinite and
// out-of-range values (the conversion is undefined there), without the
// undefined behaviour. Only NaN or absurd g input reaches the conversions with
// such values.
static int to_int(float v) { return v >= -2147483648.0f && v < 2147483648.0f ? (int)v : INT_MIN; }
// gm_ifloor / gm_iround with the same out-of-range rule.
static int floor_to_int(float v) { return to_int(floorf(v)); }
static int round_to_int(float v) { return to_int(floorf(v + 0.5f)); }

static void sprite_reset(gm_effect_t* self) {
  gm_sprite_t* s = (gm_sprite_t*)self;
  gm_spring3_snap(&s->pos, gm_vec3_zero());
  s->playhead = 0.0f;
  s->frame_index = 0;
}

static void sprite_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_sprite_t* s = (gm_sprite_t*)self;
  const gm_vec3_t drive = s->inertia > 0.5f ? gm_gforce_inertial(ctx->g) : gm_gforce_dynamic(ctx->g);
  const gm_vec3_t he = gm_half_extent();
  const float hw = s->sheet.width * 0.5f, hh = s->sheet.height * 0.5f;
  gm_vec3_t target = gm_vec3(drive.x * s->px_gx, drive.y * s->px_gy, drive.z * s->px_gz);
  const float mx = gm_maxf(0.0f, he.x - hw), my = gm_maxf(0.0f, he.y - hh);
  target.x = gm_clampf(target.x, -mx, mx);
  target.y = gm_clampf(target.y, -my, my);
  target.z = gm_clampf(target.z, -(he.z - GM_LAYER_PITCH_PX * 0.5f), he.z - GM_LAYER_PITCH_PX * 0.5f);
  gm_spring3_step(&s->pos, target, ctx->dt, s->hz, 1.0f);

  s->playhead += ctx->dt * (s->idle_fps + s->fps_per_g * gm_gforce_horizontal(ctx->g));
  const int axis = round_to_int(s->scrub_axis);
  float scrub = 0.0f;
  if (axis == 0)
    scrub = drive.x * s->frames_per_g;
  else if (axis == 1)
    scrub = drive.y * s->frames_per_g;
  else if (axis == 2)
    scrub = drive.z * s->frames_per_g;
  if (s->sheet.frames > 0) {
    const float total = s->playhead + scrub;
    int idx = floor_to_int(total) % s->sheet.frames;
    if (idx < 0) idx += s->sheet.frames;
    s->frame_index = idx;
    if (s->playhead > 1.0e6f) s->playhead = fmodf(s->playhead, (float)s->sheet.frames);
  }
}

static void sprite_render(gm_effect_t* self, gm_frame_t* f) {
  gm_sprite_t* s = (gm_sprite_t*)self;
  const gm_sprite_sheet_t* sh = &s->sheet;
  if (!sh->bits || sh->frames == 0) return;
  const gm_vec3_t v = gm_world_to_voxel(gm_spring3_pos(&s->pos));
  const int x0 = round_to_int(v.x - sh->width * 0.5f + 0.5f);
  const int y0 = round_to_int(v.y - sh->height * 0.5f + 0.5f);
  const int layer = gm_clampi(round_to_int(v.z), 0, GM_D - 1);
  const int echo = round_to_int(s->echo);
  // A negative echo draws nothing: the layer loop below runs zero times (and
  // for a NaN or huge echo from a mapping, INT_MIN, -echo would overflow; an
  // x86 build of the C++ draws nothing there either).
  if (echo < 0) return;
  for (int dl = -echo; dl <= echo; ++dl) {
    const int z = layer + dl;
    if (z < 0 || z >= GM_D) continue;
    const float intensity = dl == 0 ? 1.0f : 0.5f * (1.0f - (float)(abs(dl) - 1) / (float)(echo + 1));
    for (int y = 0; y < sh->height; ++y)
      for (int x = 0; x < sh->width; ++x)
        if (gm_sprite_sheet_pixel(sh, s->frame_index, x, y)) gm_frame_plot(f, x0 + x, y0 + y, z, intensity);
  }
}

// No activate / deactivate hooks (activating resets) and no burn-in offset.
static const gm_effect_vtbl_t k_sprite_vtbl = {
    .name = "sprite",
    .activate = NULL,
    .deactivate = NULL,
    .reset = sprite_reset,
    .update = sprite_update,
    .render = sprite_render,
    .burn_in_offset = NULL,
};

static const gm_sprite_mapping_t k_default_mapping = GM_SPRITE_MAPPING_DEFAULT;

void gm_sprite_init(gm_sprite_t* s, const gm_sprite_sheet_t* sheet, const gm_sprite_mapping_t* map) {
  const gm_sprite_mapping_t* m = map ? map : &k_default_mapping;
  s->base.vt = &k_sprite_vtbl;
  s->sheet = *sheet;
  s->px_gx = m->px_per_g.x;
  s->px_gy = m->px_per_g.y;
  s->px_gz = m->px_per_g.z;
  s->hz = m->smoothing_hz;
  s->scrub_axis = (float)m->scrub_axis;
  s->frames_per_g = m->frames_per_g;
  s->idle_fps = m->idle_fps;
  s->fps_per_g = m->fps_per_g;
  s->echo = m->depth_echo;
  s->inertia = m->follow_inertia ? 1.0f : 0.0f;
  gm_spring3_snap(&s->pos, gm_vec3_zero());
  s->playhead = 0.0f;
  s->frame_index = 0;

  gm_param_table_t* p = &s->base.params;
  gm_params_init(p);
  gm_params_add(p, "px_g_x", &s->px_gx, 0.0f, 200.0f, "lateral px per g");
  gm_params_add(p, "px_g_y", &s->px_gy, 0.0f, 100.0f, "vertical px per g");
  gm_params_add(p, "px_g_z", &s->px_gz, 0.0f, 200.0f, "depth px per g");
  gm_params_add(p, "hz", &s->hz, 0.3f, 8.0f, "smoothing");
  gm_params_add(p, "scrub_axis", &s->scrub_axis, -1.0f, 2.0f, "-1 none, 0 lat, 1 vert, 2 lon");
  gm_params_add(p, "frames_g", &s->frames_per_g, -64.0f, 64.0f, "frames scrubbed per g");
  gm_params_add(p, "idle_fps", &s->idle_fps, 0.0f, 60.0f, "autonomous playback fps");
  gm_params_add(p, "fps_g", &s->fps_per_g, 0.0f, 120.0f, "extra fps per g");
  gm_params_add(p, "echo", &s->echo, 0.0f, 4.0f, "depth ghosting layers");
  gm_params_add(p, "inertia", &s->inertia, 0.0f, 1.0f, "1 = thrown like a loose object");
  sprite_reset(&s->base);
}

// ---- demo sprite -----------------------------------------------------------------------
enum { K_DEMO_W = 16, K_DEMO_H = 16, K_DEMO_FRAMES = 12 };
static uint8_t s_demo_bits[K_DEMO_FRAMES * K_DEMO_H * ((K_DEMO_W + 7) / 8)];
static bool s_demo_built = false;

static void demo_set(int frame, int x, int y) {
  if (x < 0 || x >= K_DEMO_W || y < 0 || y >= K_DEMO_H) return;
  s_demo_bits[frame * K_DEMO_H * 2 + y * 2 + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

static void build_demo(void) {
  for (size_t i = 0; i < sizeof(s_demo_bits); ++i) s_demo_bits[i] = 0;
  const float cx = 7.5f, cy = 7.5f;
  for (int fr = 0; fr < K_DEMO_FRAMES; ++fr) {
    // Ring.
    for (int i = 0; i < 40; ++i) {
      const float a = GM_TWO_PI * i / 40.0f;
      demo_set(fr, gm_iround(cx + 7.0f * cosf(a)), gm_iround(cy + 7.0f * sinf(a)));
    }
    // Spinning bar.
    const float a = GM_PI * (float)fr / (float)K_DEMO_FRAMES;
    for (int s = -5; s <= 5; ++s) demo_set(fr, gm_iround(cx + s * cosf(a)), gm_iround(cy + s * sinf(a)));
  }
  s_demo_built = true;
}

const gm_sprite_sheet_t* gm_sprite_demo_sheet(void) {
  static gm_sprite_sheet_t sheet = {0, 0, 0, NULL};
  if (!s_demo_built) {
    build_demo();
    sheet.width = K_DEMO_W;
    sheet.height = K_DEMO_H;
    sheet.frames = K_DEMO_FRAMES;
    sheet.bits = s_demo_bits;
  }
  return &sheet;
}
