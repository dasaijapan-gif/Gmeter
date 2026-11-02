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
  gm_effect_t base;
  gm_sprite_sheet_t sheet;
  // The tunables (see the parameter table set up by gm_sprite_init).
  float px_gx, px_gy, px_gz, hz, scrub_axis, frames_per_g, idle_fps, fps_per_g, echo, inertia;
  gm_spring3_t pos;
  float playhead;
  int frame_index;
} gm_sprite_t;

// Copies the sheet descriptor (the bits are not copied) and sets the defaults.
void gm_sprite_init(gm_sprite_t* s, const gm_sprite_sheet_t* sheet);
static inline int gm_sprite_current_frame(const gm_sprite_t* s) { return s->frame_index; }
static inline gm_vec3_t gm_sprite_position(const gm_sprite_t* s) { return gm_spring3_pos(&s->pos); }

// A built-in 16x16, 12-frame demo sprite (spinning bar inside a ring) so the
// pipeline can be exercised before any artwork exists.
const gm_sprite_sheet_t* gm_sprite_demo_sheet(void);

#ifdef __cplusplus
}
#endif
