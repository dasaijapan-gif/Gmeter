// Effect "sprite": a 2D sprite animation driven by g.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int width, height, frames;
  const uint8_t* bits;  // frame-major, row-major, 1 bpp, MSB first, rows byte-padded
} gm_sprite_sheet_t;

static inline int gm_sprite_sheet_row_bytes(const gm_sprite_sheet_t* s) { return (s->width + 7) / 8; }
static inline int gm_sprite_sheet_frame_bytes(const gm_sprite_sheet_t* s) {
  return gm_sprite_sheet_row_bytes(s) * s->height;
}
static inline bool gm_sprite_sheet_pixel(const gm_sprite_sheet_t* s, int frame, int x, int y) {
  if (!s->bits || frame < 0 || frame >= s->frames || x < 0 || x >= s->width || y < 0 || y >= s->height) return false;
  return (s->bits[frame * gm_sprite_sheet_frame_bytes(s) + y * gm_sprite_sheet_row_bytes(s) + (x >> 3)] >>
          (7 - (x & 7))) &
         1u;
}

typedef struct {
  gm_vec3_t px_per_g;   // world displacement per g on each axis
  float smoothing_hz;
  int scrub_axis;       // -1 none, 0 lateral, 1 vertical, 2 longitudinal
  float frames_per_g;   // frames of scrub per g on that axis
  float idle_fps;       // autonomous playback
  float fps_per_g;      // extra playback speed per g (horizontal magnitude)
  float depth_echo;     // ghost copies on neighbouring layers (layers of falloff)
  bool follow_inertia;
} gm_sprite_mapping_t;

#define GM_SPRITE_MAPPING_DEFAULT {{60.0f, 24.0f, 60.0f}, 2.0f, 2, 8.0f, 0.0f, 12.0f, 1.0f, true}

typedef struct {
  gm_effect_t base;
  gm_sprite_sheet_t sheet;
  float px_gx, px_gy, px_gz, hz, scrub_axis, frames_per_g, idle_fps, fps_per_g, echo, inertia;
  gm_spring3_t pos;
  float playhead;
  int frame_index;
} gm_sprite_t;

// `map` may be NULL for the defaults (GM_SPRITE_MAPPING_DEFAULT).
void gm_sprite_init(gm_sprite_t* s, const gm_sprite_sheet_t* sheet, const gm_sprite_mapping_t* map);
static inline void gm_sprite_set_sheet(gm_sprite_t* s, const gm_sprite_sheet_t* sheet) { s->sheet = *sheet; }
static inline int gm_sprite_current_frame(const gm_sprite_t* s) { return s->frame_index; }
static inline gm_vec3_t gm_sprite_position(const gm_sprite_t* s) { return gm_spring3_pos(&s->pos); }

// A built-in 16x16, 12-frame demo sprite (spinning bar inside a ring) so the
// pipeline can be exercised before any artwork exists.
const gm_sprite_sheet_t* gm_sprite_demo_sheet(void);

#ifdef __cplusplus
}
#endif
