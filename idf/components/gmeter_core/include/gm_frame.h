// Bit-packed W x H x D monochrome voxel frame.
//
// Each layer is stored exactly as an SSD1306/SSD1309 expects its GDDRAM in
// horizontal addressing mode: GM_PAGES rows of GM_W bytes, each byte holding
// 8 vertically stacked pixels with bit 0 at the top. The display driver can
// therefore send a layer straight out of this buffer with no repacking.
//
// Coordinates: x = column 0..GM_W-1 (left -> right), y = row 0..GM_H-1 (top
// -> bottom), z = layer 0..GM_D-1 (nearest the viewer -> farthest /
// windshield). Lit voxels on different layers add visually; nothing occludes.
//
// A gm_frame_t is about 7-8 KB: keep frames in static or heap memory, never
// on a task stack, and clear one with gm_frame_clear before drawing.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gm_config.h"
#include "gm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t bits[GM_D][GM_BYTES_PER_LAYER];
} gm_frame_t;

extern const uint8_t gm_bayer8[8][8];

static inline void gm_frame_clear(gm_frame_t* f) { memset(f->bits, 0, sizeof(f->bits)); }
static inline void gm_frame_fill(gm_frame_t* f) { memset(f->bits, 0xFF, sizeof(f->bits)); }

static inline bool gm_frame_in_bounds(int x, int y, int z) {
  return (unsigned)x < (unsigned)GM_W && (unsigned)y < (unsigned)GM_H && (unsigned)z < (unsigned)GM_D;
}

// Set a voxel (bounds-checked; out-of-range is a silent no-op).
static inline void gm_frame_set(gm_frame_t* f, int x, int y, int z) {
  if (gm_frame_in_bounds(x, y, z)) f->bits[z][(y >> 3) * GM_W + x] |= (uint8_t)(1u << (y & 7));
}
static inline void gm_frame_reset(gm_frame_t* f, int x, int y, int z) {
  if (gm_frame_in_bounds(x, y, z)) f->bits[z][(y >> 3) * GM_W + x] &= (uint8_t)~(1u << (y & 7));
}
static inline bool gm_frame_get(const gm_frame_t* f, int x, int y, int z) {
  return gm_frame_in_bounds(x, y, z) && ((f->bits[z][(y >> 3) * GM_W + x] >> (y & 7)) & 1u);
}

// Ordered-dither threshold: each layer uses a different offset into the 8x8
// Bayer matrix so patterns on adjacent layers do not line up and moire.
static inline float gm_frame_bayer_threshold(int x, int y, int z) {
  const int ox = (z * 3) & 7, oy = (z * 5) & 7;
  return (gm_bayer8[(y + oy) & 7][(x + ox) & 7] + 0.5f) * (1.0f / 64.0f);
}
// Ordered-dither plot: lights the voxel if `intensity` (0..1) exceeds the
// Bayer threshold at this position.
static inline void gm_frame_plot(gm_frame_t* f, int x, int y, int z, float intensity) {
  if (intensity >= 1.0f) {
    gm_frame_set(f, x, y, z);
    return;
  }
  if (intensity <= 0.0f) return;
  if (intensity > gm_frame_bayer_threshold(x, y, z)) gm_frame_set(f, x, y, z);
}

// ---- 2D primitives on one layer -------------------------------------------------------
void gm_frame_hline(gm_frame_t* f, int z, int x0, int x1, int y);
void gm_frame_vline(gm_frame_t* f, int z, int x, int y0, int y1);
void gm_frame_line(gm_frame_t* f, int z, int x0, int y0, int x1, int y1);
void gm_frame_rect(gm_frame_t* f, int z, int x0, int y0, int x1, int y1);  // outline
void gm_frame_fill_rect(gm_frame_t* f, int z, int x0, int y0, int x1, int y1, float intensity);
void gm_frame_circle(gm_frame_t* f, int z, int cx, int cy, int r);  // outline, integer
void gm_frame_disc(gm_frame_t* f, int z, float cx, float cy, float r, float intensity);  // filled, sub-pixel centre

// ---- 3D primitives ---------------------------------------------------------------------
// Sphere/ellipsoid centred at a world position (see gm_world_to_voxel): rxy is
// the radius in pixels on a layer, rz the radius along the depth axis in
// *pixel units* (so rz = 1.5 * GM_LAYER_PITCH_PX spans about three layers).
void gm_frame_sphere(gm_frame_t* f, gm_vec3_t world_center, float rxy, float rz, float intensity);
// Single voxel at a world position (nearest).
void gm_frame_point(gm_frame_t* f, gm_vec3_t world, float intensity);
// Straight 3D line between two world positions, stepping through layers.
void gm_frame_line3(gm_frame_t* f, gm_vec3_t a, gm_vec3_t b, float intensity);

// Tiny 3x5 font for HUD text; returns the advance in pixels.
int gm_frame_text(gm_frame_t* f, int z, int x, int y, const char* s);
// The 3x5 glyph for `c`: 5 rows of 3 bits, row 0 in bits 14..12, MSB = left.
uint16_t gm_glyph3x5(char c);
// Clears a box through every layer (e.g. behind HUD text so it stays legible).
void gm_frame_clear_box(gm_frame_t* f, int x0, int y0, int x1, int y1);
// Moves the whole frame (every layer) by dx columns and dy rows; pixels pushed
// past an edge are lost and the uncovered strip is cleared. Used for the HUD's
// anti burn-in orbit, so it works on bytes, not pixel by pixel.
void gm_frame_shift(gm_frame_t* f, int dx, int dy);

// ---- world <-> voxel mapping -----------------------------------------------------------
// World frame = car frame in pixel units, origin at the volume centre: x right,
// y up, z forward (deeper). Layer k sits at z = (k - (D-1)/2) * pitch.
static inline gm_vec3_t gm_voxel_to_world(float x, float y, float z) {
  return gm_vec3(x - GM_W * 0.5f + 0.5f, GM_H * 0.5f - 0.5f - y, (z - (GM_D - 1) * 0.5f) * GM_LAYER_PITCH_PX);
}
static inline gm_vec3_t gm_world_to_voxel(gm_vec3_t w) {
  return gm_vec3(w.x + GM_W * 0.5f - 0.5f, GM_H * 0.5f - 0.5f - w.y, w.z / GM_LAYER_PITCH_PX + (GM_D - 1) * 0.5f);
}
static inline float gm_layer_z(int k) { return ((float)k - (GM_D - 1) * 0.5f) * GM_LAYER_PITCH_PX; }
// Half-extents of the simulation box in world units.
static inline gm_vec3_t gm_half_extent(void) { return gm_vec3(GM_W * 0.5f, GM_H * 0.5f, GM_DEPTH_PX * 0.5f); }

// ---- raw access ------------------------------------------------------------------------
static inline uint8_t* gm_frame_layer(gm_frame_t* f, int z) { return f->bits[z]; }
static inline uint8_t* gm_frame_data(gm_frame_t* f) { return &f->bits[0][0]; }
static inline const uint8_t* gm_frame_cdata(const gm_frame_t* f) { return &f->bits[0][0]; }

// Combine: f |= o.
void gm_frame_or(gm_frame_t* f, const gm_frame_t* o);
// Dithered crossfade between a and b into f (t = 0 -> a, 1 -> b).
void gm_frame_crossfade(gm_frame_t* f, const gm_frame_t* a, const gm_frame_t* b, float t);

int gm_frame_count_lit(const gm_frame_t* f);
int gm_frame_count_lit_layer(const gm_frame_t* f, int z);

#ifdef __cplusplus
}
#endif
