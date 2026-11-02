// Authored animation, interpreted by g-force (see gm_anim.h).
#include "gm_anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gm_gstate.h"

static_assert(GM_ANIM_ACCELERATE == (int)GM_DRIVE_ACCELERATE && GM_ANIM_BRAKE == (int)GM_DRIVE_BRAKE &&
                  GM_ANIM_LEFT == (int)GM_DRIVE_LEFT && GM_ANIM_RIGHT == (int)GM_DRIVE_RIGHT,
              "anim states 1-4 are gm_gstate's driving states (see gm_gstate_choose)");

const char* gm_anim_state_name(int state) {
  static const char* const k_names[GM_ANIM_STATE_COUNT] = {"idle", "accelerate", "brake", "left", "right", "bump"};
  return state >= 0 && state < GM_ANIM_STATE_COUNT ? k_names[state] : "?";
}

// An asset with no artwork: default clips and mapping, depth 1.
#define K_ASSET_EMPTY_INIT                                                                  \
  {"", 0, 0, 1, 0, NULL,                                                                    \
   {GM_ANIM_CLIP_DEFAULT, GM_ANIM_CLIP_DEFAULT, GM_ANIM_CLIP_DEFAULT, GM_ANIM_CLIP_DEFAULT, \
    GM_ANIM_CLIP_DEFAULT, GM_ANIM_CLIP_DEFAULT},                                            \
   GM_ANIM_MAPPING_DEFAULT}
static_assert(GM_ANIM_STATE_COUNT == 6, "K_ASSET_EMPTY_INIT lists one clip per state");

gm_anim_asset_t gm_anim_asset_empty(void) {
  const gm_anim_asset_t a = K_ASSET_EMPTY_INIT;
  return a;
}

static void anim_reset(gm_effect_t* self) {
  gm_anim_t* a = (gm_anim_t*)self;
  gm_spring3_snap(&a->pos, gm_vec3_zero());
  a->state = GM_ANIM_IDLE;
  a->state_time = 0.0f;
  a->clip_time = 0.0f;
  a->frame = 0;
  a->mirror = false;
  a->prev_vert = 0.0f;
}

static bool anim_usable(const gm_anim_t* a, const gm_anim_clip_t* c) {
  return c->count > 0 && c->start < a->asset.frames;
}

static gm_anim_clip_t anim_clamp_clip(gm_anim_clip_t c, uint16_t frames) {
  if (c.start + c.count > frames) c.count = (uint16_t)(frames - c.start);
  if (c.fps <= 0.0f) c.fps = 1.0f;
  return c;
}

gm_anim_clip_t gm_anim_resolve_clip(const gm_anim_t* a, int state, bool* mirror) {
  *mirror = false;
  if (state < 0 || state >= GM_ANIM_STATE_COUNT) state = GM_ANIM_IDLE;
  const uint16_t frames = a->asset.frames;
  const gm_anim_clip_t* own = &a->asset.clips[state];
  if (anim_usable(a, own)) return anim_clamp_clip(*own, frames);
  if ((state == GM_ANIM_LEFT || state == GM_ANIM_RIGHT) && a->m.mirror > 0.5f) {
    const gm_anim_clip_t* other = &a->asset.clips[state == GM_ANIM_LEFT ? GM_ANIM_RIGHT : GM_ANIM_LEFT];
    if (anim_usable(a, other)) {
      *mirror = true;
      return anim_clamp_clip(*other, frames);
    }
  }
  const gm_anim_clip_t* idle = &a->asset.clips[GM_ANIM_IDLE];
  if (anim_usable(a, idle)) return anim_clamp_clip(*idle, frames);
  gm_anim_clip_t all = GM_ANIM_CLIP_DEFAULT;  // start 0, 12 fps, looping
  all.count = frames;
  if (idle->fps > 0.0f) all.fps = idle->fps;
  return all;
}

static void anim_enter(gm_anim_t* a, int state) {
  if (state == a->state && a->state_time > 0.0f) return;
  // A state that resolves to the frames already playing (for example accelerate
  // with no clip of its own, falling back to idle) carries on from the same
  // frame instead of restarting. The bump clip always starts from its first frame.
  bool m0 = false, m1 = false;
  const gm_anim_clip_t from = gm_anim_resolve_clip(a, a->state, &m0), to = gm_anim_resolve_clip(a, state, &m1);
  const bool same_frames = state != GM_ANIM_BUMP && a->state != GM_ANIM_BUMP && from.start == to.start &&
                           from.count == to.count && from.loop == to.loop && m0 == m1;
  a->state = state;
  a->state_time = 0.0f;
  a->clip_time = same_frames ? a->clip_time * from.fps / to.fps : 0.0f;  // resolve_clip keeps fps > 0
}

void gm_anim_force_state(gm_anim_t* a, int state) {
  a->forced = (state >= 0 && state < GM_ANIM_STATE_COUNT) ? state : -1;
  if (a->forced >= 0) anim_enter(a, a->forced);
}

static void anim_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_anim_t* a = (gm_anim_t*)self;
  if (!gm_anim_asset_valid(&a->asset)) return;
  const gm_vec3_t dyn = gm_gforce_dynamic(ctx->g);
  a->state_time += ctx->dt;

  // ---- state machine -------------------------------------------------------------
  const float vert = dyn.y;
  const bool spike = fabsf(vert) >= a->m.bump_g && fabsf(a->prev_vert) < a->m.bump_g;
  a->prev_vert = vert;
  bool clip_mirror = false;
  if (a->forced >= 0) {
    if (a->state != a->forced) anim_enter(a, a->forced);
  } else if (a->state == GM_ANIM_BUMP) {
    const gm_anim_clip_t c = gm_anim_resolve_clip(a, GM_ANIM_BUMP, &clip_mirror);
    const float len = (float)c.count / c.fps;
    if (!anim_usable(a, &a->asset.clips[GM_ANIM_BUMP]) || a->clip_time >= len)
      anim_enter(a, gm_gstate_choose(a->state, dyn, a->m.enter_g));
  } else {
    const bool has_bump = anim_usable(a, &a->asset.clips[GM_ANIM_BUMP]);
    if (spike && has_bump) {
      anim_enter(a, GM_ANIM_BUMP);
    } else if (a->state_time >= a->m.hold_s) {
      const int next = gm_gstate_choose(a->state, dyn, a->m.enter_g);
      if (next != a->state) anim_enter(a, next);
    }
  }

  // ---- playback -------------------------------------------------------------------
  const gm_anim_clip_t clip = gm_anim_resolve_clip(a, a->state, &clip_mirror);
  a->mirror = clip_mirror;
  const float speed = 1.0f + a->m.fps_per_g * sqrtf(dyn.x * dyn.x + dyn.z * dyn.z);
  a->clip_time += ctx->dt * speed;
  int idx = (int)(a->clip_time * clip.fps);
  const bool loop = clip.loop != 0 && a->state != GM_ANIM_BUMP;
  const int axis = gm_iround(a->m.scrub_axis);
  if (axis >= 0 && axis <= 2 && a->m.frames_per_g != 0.0f) {
    const float v = axis == 0 ? dyn.x : (axis == 1 ? dyn.y : dyn.z);
    idx += gm_iround(v * a->m.frames_per_g);
  }
  const int count = clip.count > 0 ? clip.count : 1;
  if (loop) {
    idx %= count;
    if (idx < 0) idx += count;
  } else {
    idx = gm_clampi(idx, 0, count - 1);
  }
  a->frame = clip.start + idx;
  // Keep the clip timer bounded: a loop wraps round; a one-shot clip stops at
  // 1e5 frames, which is past its last frame even with the most scrubbing.
  if (a->clip_time * clip.fps > 1.0e5f)
    a->clip_time = loop ? fmodf(a->clip_time, (float)count / clip.fps) : 1.0e5f / clip.fps;

  // ---- motion -----------------------------------------------------------------------
  const gm_vec3_t drive = a->m.inertia > 0.5f ? gm_gforce_inertial(ctx->g) : dyn;
  const gm_vec3_t he = gm_half_extent();
  const float hw = gm_maxf(0.0f, he.x - a->asset.width * 0.5f);
  const float hh = gm_maxf(0.0f, he.y - a->asset.height * 0.5f);
  const float slices_half = (a->asset.depth - 1) * 0.5f * GM_LAYER_PITCH_PX;
  const float hz = gm_maxf(0.0f, he.z - GM_LAYER_PITCH_PX * 0.5f - slices_half);
  gm_vec3_t target = gm_vec3(drive.x * a->m.px_per_gx, drive.y * a->m.px_per_gy, drive.z * a->m.px_per_gz);
  target.x = gm_clampf(target.x, -hw, hw);
  target.y = gm_clampf(target.y, -hh, hh);
  target.z = gm_clampf(target.z, -hz, hz);
  gm_spring3_step(&a->pos, target, ctx->dt, a->m.spring_hz, 1.0f);
}

static void anim_render(gm_effect_t* self, gm_frame_t* f) {
  gm_anim_t* a = (gm_anim_t*)self;
  const gm_anim_asset_t* as = &a->asset;
  if (!gm_anim_asset_valid(as)) return;
  const gm_vec3_t v = gm_world_to_voxel(gm_spring3_pos(&a->pos));
  const int x0 = gm_iround(v.x - as->width * 0.5f + 0.5f);
  const int y0 = gm_iround(v.y - as->height * 0.5f + 0.5f);
  const int depth = as->depth;
  // Slice 0 is the front-most; centre the stack of slices on the position.
  // Art with more slices than panels stays centred; out-of-range layers are skipped.
  const int first =
      gm_clampi(gm_iround(v.z - (depth - 1) * 0.5f), gm_mini(0, GM_D - depth), gm_maxi(0, GM_D - depth));
  const int echo = depth == 1 ? gm_iround(a->m.echo) : 0;  // 0..4 (the parameter's range)
  const int w = as->width;
  for (int s = 0; s < depth; ++s) {
    const int layer = first + s;
    for (int dl = -echo; dl <= echo; ++dl) {
      const int z = layer + dl;
      if (z < 0 || z >= GM_D) continue;
      const float intensity = dl == 0 ? 1.0f : 0.5f * (1.0f - (float)(abs(dl) - 1) / (float)(echo + 1));
      for (int y = 0; y < as->height; ++y)
        for (int x = 0; x < w; ++x)
          if (gm_anim_asset_pixel(as, a->frame, s, a->mirror ? w - 1 - x : x, y))
            gm_frame_plot(f, x0 + x, y0 + y, z, intensity);
    }
  }
}

// No deactivate hook and no burn-in offset.
static const gm_effect_vtbl_t k_anim_vtbl = {
    .name = "anim",
    .deactivate = NULL,
    .reset = anim_reset,
    .update = anim_update,
    .render = anim_render,
    .burn_in_offset = NULL,
};

void gm_anim_init(gm_anim_t* a, const gm_anim_asset_t* asset) {
  a->asset = *asset;
  a->m = asset->mapping;
  a->forced = -1;

  gm_param_table_t* p = gm_effect_init(&a->base, &k_anim_vtbl);
  gm_params_add(p, "px_g_x", &a->m.px_per_gx, 0.0f, 200.0f, "lateral travel px per g");
  gm_params_add(p, "px_g_y", &a->m.px_per_gy, 0.0f, 100.0f, "vertical travel px per g");
  gm_params_add(p, "px_g_z", &a->m.px_per_gz, 0.0f, 200.0f, "depth travel px per g");
  gm_params_add(p, "hz", &a->m.spring_hz, 0.3f, 8.0f, "motion smoothing");
  gm_params_add(p, "inertia", &a->m.inertia, 0.0f, 1.0f, "1 = thrown like a loose object");
  gm_params_add(p, "enter_g", &a->m.enter_g, 0.02f, 1.5f, "horizontal g that leaves idle");
  gm_params_add(p, "hold_s", &a->m.hold_s, 0.0f, 3.0f, "minimum seconds in a state");
  gm_params_add(p, "bump_g", &a->m.bump_g, 0.05f, 2.0f, "vertical g that fires the bump clip");
  gm_params_add(p, "fps_g", &a->m.fps_per_g, 0.0f, 8.0f, "playback speed-up per g");
  gm_params_add(p, "scrub_axis", &a->m.scrub_axis, -1.0f, 2.0f, "-1 off, 0 lat, 1 vert, 2 lon");
  gm_params_add(p, "frames_g", &a->m.frames_per_g, -64.0f, 64.0f, "frames scrubbed per g");
  gm_params_add(p, "echo", &a->m.echo, 0.0f, 4.0f, "ghost layers (single-slice art)");
  gm_params_add(p, "mirror", &a->m.mirror, 0.0f, 1.0f, "mirror an empty left/right clip");
  // A hand-written user_animation.h may hold any values: bring the tunables
  // into their parameter ranges and every positive clip frame rate into
  // 0.001..1000 fps (a rate <= 0 or NaN becomes 0, which gm_anim_resolve_clip
  // treats like any rate <= 0), so the playback arithmetic stays in range.
  for (int i = 0; i < gm_params_count(p); ++i) {
    const gm_param_t* t = gm_params_at(p, i);
    *t->value = gm_clampf(*t->value, t->min, t->max);
  }
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) {
    gm_anim_clip_t* c = &a->asset.clips[s];
    c->fps = c->fps > 0.0f ? gm_clampf(c->fps, 0.001f, 1000.0f) : 0.0f;
  }
  anim_reset(&a->base);
}

// ---- demo artwork ---------------------------------------------------------------------------
enum { K_DW = 32, K_DH = 24, K_DD = 3, K_DF = 32, K_DROW = (K_DW + 7) / 8 };
#define K_DEMO_BYTES ((size_t)K_DF * K_DD * K_DH * K_DROW)
// Built on first use on the heap: 9 KB that the ESP32's static DRAM segment
// cannot spare, and never allocated when a user animation replaces the demo.
static uint8_t* s_demo = NULL;

static void dset(int f, int s, int x, int y) {
  if (x < 0 || x >= K_DW || y < 0 || y >= K_DH) return;
  s_demo[((f * K_DD + s) * K_DH + y) * K_DROW + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}
static void dline(int f, int s, int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 128; ++guard) {
    dset(f, s, x0, y0);
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
static void dring(int f, int s, float cx, float cy, float r) {
  const int n = 48;
  for (int i = 0; i < n; ++i) {
    const float a = GM_TWO_PI * (float)i / (float)n;
    dset(f, s, gm_iround(cx + r * cosf(a)), gm_iround(cy + r * sinf(a)));
  }
}
// Chevron pointing in direction (dx, dy) (unit axis), tip at (tx, ty).
static void dchevron(int f, int s, int tx, int ty, int dx, int dy, int size) {
  // Perpendicular axis.
  const int px = -dy, py = dx;
  for (int t = 0; t < 2; ++t) {  // two pixels thick
    const int bx = tx - dx * t, by = ty - dy * t;
    dline(f, s, bx, by, bx - dx * size + px * size, by - dy * size + py * size);
    dline(f, s, bx, by, bx - dx * size - px * size, by - dy * size - py * size);
  }
}

static void build_demo(void) {
  memset(s_demo, 0, K_DEMO_BYTES);
  const float cx = (K_DW - 1) * 0.5f, cy = (K_DH - 1) * 0.5f;
  // Idle 0..7: a breathing shell (smaller rings on the outer slices).
  for (int f = 0; f < 8; ++f) {
    const float r = 7.0f + 2.0f * sinf(GM_TWO_PI * (float)f / 8.0f);
    dring(f, 1, cx, cy, r);
    dring(f, 0, cx, cy, r * 0.6f);
    dring(f, 2, cx, cy, r * 0.6f);
    dset(f, 1, gm_iround(cx), gm_iround(cy));
  }
  // Accelerate 8..13: chevrons pointing up, marching up; back slices lag.
  for (int i = 0; i < 6; ++i)
    for (int s = 0; s < K_DD; ++s)
      for (int k = 0; k < 3; ++k) {
        const int y = ((K_DH + 6 - (i * 2 + k * 8 + s * 3)) % (K_DH + 6)) - 3;
        dchevron(8 + i, s, gm_iround(cx), y, 0, -1, 6 - s);
      }
  // Brake 14..19: chevrons pointing down, marching down, plus a bar on the front slice.
  for (int i = 0; i < 6; ++i) {
    for (int s = 0; s < K_DD; ++s)
      for (int k = 0; k < 3; ++k) {
        const int y = ((i * 2 + k * 8 + s * 3) % (K_DH + 6)) - 3;
        dchevron(14 + i, s, gm_iround(cx), y, 0, 1, 6 - s);
      }
    if (i % 2 == 0) dline(14 + i, 0, 4, K_DH - 2, K_DW - 5, K_DH - 2);
  }
  // Right 20..25: chevrons pointing right, marching right (left is mirrored).
  for (int i = 0; i < 6; ++i)
    for (int s = 0; s < K_DD; ++s)
      for (int k = 0; k < 3; ++k) {
        const int x = ((i * 2 + k * 11 + s * 3) % (K_DW + 6)) - 3;
        dchevron(20 + i, s, x, gm_iround(cy), 1, 0, 6 - s);
      }
  // Bump 26..31: starburst growing outward, front slice leads.
  for (int i = 0; i < 6; ++i)
    for (int s = 0; s < K_DD; ++s) {
      const float r0 = 2.0f + (float)i * 1.6f - (float)s;
      const float r1 = r0 + 4.0f;
      if (r1 <= 0.0f) continue;
      for (int ray = 0; ray < 8; ++ray) {
        const float a = GM_TWO_PI * (float)ray / 8.0f + 0.2f * (float)s;
        dline(26 + i, s, gm_iround(cx + gm_maxf(0.0f, r0) * cosf(a)), gm_iround(cy + gm_maxf(0.0f, r0) * sinf(a)),
              gm_iround(cx + r1 * cosf(a)), gm_iround(cy + r1 * sinf(a)));
      }
    }
}

const gm_anim_asset_t* gm_anim_demo(void) {
  static gm_anim_asset_t asset = K_ASSET_EMPTY_INIT;
  static bool built = false;
  if (!built) {
    if (!s_demo) s_demo = (uint8_t*)malloc(K_DEMO_BYTES);
    if (!s_demo) return &asset;  // out of memory: an empty (invalid) asset, retried next call
    build_demo();
    asset.name = "demo";
    asset.width = K_DW;
    asset.height = K_DH;
    asset.depth = K_DD;
    asset.frames = K_DF;
    asset.bits = s_demo;
    asset.clips[GM_ANIM_IDLE] = (gm_anim_clip_t){0, 8, 8.0f, 1};
    asset.clips[GM_ANIM_ACCELERATE] = (gm_anim_clip_t){8, 6, 12.0f, 1};
    asset.clips[GM_ANIM_BRAKE] = (gm_anim_clip_t){14, 6, 12.0f, 1};
    asset.clips[GM_ANIM_LEFT] = (gm_anim_clip_t){0, 0, 12.0f, 1};  // mirrored from right
    asset.clips[GM_ANIM_RIGHT] = (gm_anim_clip_t){20, 6, 12.0f, 1};
    asset.clips[GM_ANIM_BUMP] = (gm_anim_clip_t){26, 6, 18.0f, 0};
    built = true;
  }
  return &asset;
}
