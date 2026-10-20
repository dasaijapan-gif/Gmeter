// Effect "anim": authored animation, interpreted by g-force.
//
// An animation asset is pixel artwork: `frames` frames of `width` x `height`
// pixels, each frame made of `depth` slices that sit on consecutive panels
// (slice 0 nearest the driver), so artwork can itself be volumetric. Clips
// map the car's state to frame ranges:
//
//   idle        nothing much happening
//   accelerate  longitudinal g forward
//   brake       longitudinal g backward
//   left/right  cornering (a right turn is lateral g to the right)
//   bump        a vertical spike; plays once, then the effect returns to
//               whichever state the g-force selects
//
// A clip with count 0 falls back: left/right borrow the other side mirrored
// (when `mirror` is on), everything else falls back to idle, and an empty idle
// clip plays all frames. On top of the clip choice the effect moves the
// artwork through the volume like a loose object (spring-smoothed), can speed
// playback up with g, scrub frames with g, and ghost a flat sprite onto
// neighbouring layers.
//
// Assets are plain aggregates so Gmeter Studio can export them as a header;
// see main/user_animation.h in the firmware. The field order is part of that
// header format: append new fields at the end only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GM_ANIM_IDLE = 0,
  GM_ANIM_ACCELERATE,
  GM_ANIM_BRAKE,
  GM_ANIM_LEFT,
  GM_ANIM_RIGHT,
  GM_ANIM_BUMP,
  GM_ANIM_STATE_COUNT
} gm_anim_state_t;

// "idle", "accelerate", ...; "?" for an out-of-range state.
const char* gm_anim_state_name(int state);

typedef struct {
  uint16_t start;  // first frame of the clip
  uint16_t count;  // frames in the clip; 0 = fall back (see above)
  float fps;       // playback rate
  uint8_t loop;    // 0 = play once and hold the last frame
} gm_anim_clip_t;

#define GM_ANIM_CLIP_DEFAULT {0, 0, 12.0f, 1}

// Default values for the effect's tunables.
typedef struct {
  float px_per_gx;     // lateral travel, pixels per g
  float px_per_gy;     // vertical travel, pixels per g
  float px_per_gz;     // depth travel, pixels per g (layer pitch is GM_LAYER_PITCH_PX)
  float spring_hz;     // motion smoothing
  float inertia;       // 1: thrown like a loose object; 0: follows the acceleration arrow
  float enter_g;       // horizontal g that leaves idle
  float hold_s;        // minimum time in a state before switching
  float bump_g;        // vertical g spike that fires the bump clip
  float fps_per_g;     // extra playback speed per g (1.0 doubles speed at 1 g)
  float scrub_axis;    // -1 off, 0 lateral, 1 vertical, 2 longitudinal
  float frames_per_g;  // frames offset per g on the scrub axis
  float echo;          // ghost layers either side of a single-slice sprite
  float mirror;        // mirror left/right clips when one side is empty
} gm_anim_mapping_t;

#define GM_ANIM_MAPPING_DEFAULT {50.0f, 16.0f, 60.0f, 2.0f, 1.0f, 0.25f, 0.30f, 0.30f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f}

typedef struct {
  const char* name;
  uint16_t width, height, depth, frames;
  // [frame][slice][row][byte]: 1 bit per pixel, MSB = leftmost, rows byte-padded.
  const uint8_t* bits;
  gm_anim_clip_t clips[GM_ANIM_STATE_COUNT];
  gm_anim_mapping_t mapping;
} gm_anim_asset_t;

// An empty asset (no artwork, default clips and mapping, depth 1).
gm_anim_asset_t gm_anim_asset_empty(void);
static inline int gm_anim_asset_row_bytes(const gm_anim_asset_t* a) { return (a->width + 7) / 8; }
static inline int gm_anim_asset_slice_bytes(const gm_anim_asset_t* a) {
  return gm_anim_asset_row_bytes(a) * a->height;
}
static inline int gm_anim_asset_frame_bytes(const gm_anim_asset_t* a) {
  return gm_anim_asset_slice_bytes(a) * a->depth;
}
static inline int gm_anim_asset_total_bytes(const gm_anim_asset_t* a) {
  return gm_anim_asset_frame_bytes(a) * a->frames;
}
static inline bool gm_anim_asset_valid(const gm_anim_asset_t* a) {
  return a->bits && a->width > 0 && a->height > 0 && a->depth > 0 && a->frames > 0;
}
static inline bool gm_anim_asset_pixel(const gm_anim_asset_t* a, int frame, int slice, int x, int y) {
  if (!a->bits || frame < 0 || frame >= a->frames || slice < 0 || slice >= a->depth || x < 0 || x >= a->width ||
      y < 0 || y >= a->height)
    return false;
  const int i = frame * gm_anim_asset_frame_bytes(a) + slice * gm_anim_asset_slice_bytes(a) +
                y * gm_anim_asset_row_bytes(a) + (x >> 3);
  return (a->bits[i] >> (7 - (x & 7))) & 1u;
}

typedef struct {
  gm_effect_t base;
  gm_anim_asset_t asset;  // a copy; the artwork bits are not copied
  gm_anim_mapping_t m;    // the tunables
  gm_spring3_t pos;
  int state;
  int forced;
  float state_time;
  float clip_time;
  int frame;
  bool mirror;
  float prev_vert;
} gm_anim_t;

void gm_anim_init(gm_anim_t* a, const gm_anim_asset_t* asset);

// Swap artwork at runtime. When apply_mapping is true the asset's mapping
// overwrites the current tunables; reset_motion false keeps the current
// position, state and clip time (live editing).
void gm_anim_set_asset(gm_anim_t* a, const gm_anim_asset_t* asset, bool apply_mapping, bool reset_motion);
static inline const gm_anim_asset_t* gm_anim_asset(const gm_anim_t* a) { return &a->asset; }

// -1 = choose the state from g-force (normal); 0..5 = hold that state (preview).
void gm_anim_force_state(gm_anim_t* a, int state);
static inline int gm_anim_state(const gm_anim_t* a) { return a->state; }
static inline int gm_anim_current_frame(const gm_anim_t* a) { return a->frame; }
static inline bool gm_anim_mirrored(const gm_anim_t* a) { return a->mirror; }
static inline gm_vec3_t gm_anim_position(const gm_anim_t* a) { return gm_spring3_pos(&a->pos); }

// Resolved clip for a state after fallbacks; *mirror set when the clip is the
// other side's frames flipped horizontally.
gm_anim_clip_t gm_anim_resolve_clip(const gm_anim_t* a, int state, bool* mirror);

// Built-in 32x24, 3-slice demo (breathing ring, chevrons per direction,
// starburst on bumps) so the effect works before any artwork exists.
const gm_anim_asset_t* gm_anim_demo(void);

#ifdef __cplusplus
}
#endif
