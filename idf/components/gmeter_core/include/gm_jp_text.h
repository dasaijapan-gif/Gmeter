// Japanese text on the voxel frame: small fixed-height 1-bit glyph sets
// (only the characters the effects use; see gm_jp_glyphs.c).
#pragma once

#include <stdint.h>

#include "gm_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t codepoint;
  uint8_t width;
  const uint8_t* bits;  // `height` rows, MSB = leftmost pixel, rows byte-padded
} gm_jp_glyph_t;

typedef struct {
  uint8_t height;
  uint16_t count;
  const gm_jp_glyph_t* glyphs;  // sorted by codepoint
} gm_jp_font_t;

extern const gm_jp_font_t gm_jp_large;  // 24 px, Noto Serif JP Black, condensed (title cards)
extern const gm_jp_font_t gm_jp_small;  // 14 px, Noto Sans JP Bold (labels)

// NULL when the font has no glyph for the codepoint.
const gm_jp_glyph_t* gm_jp_find_glyph(const gm_jp_font_t* font, uint32_t codepoint);

// Draws UTF-8 text; ASCII falls back to the 3x5 HUD font (vertically centred).
// Characters missing from the font are skipped (their width still advances).
// A negative intensity clears pixels instead of setting them. Returns the
// advance in pixels (width for horizontal, height for vertical). The usual
// spacing is 1 and intensity 1.0f.
int gm_jp_draw(gm_frame_t* f, const gm_jp_font_t* font, int z, int x, int y, const char* utf8, int spacing,
               float intensity);
int gm_jp_draw_vertical(gm_frame_t* f, const gm_jp_font_t* font, int z, int x, int y, const char* utf8, int spacing,
                        float intensity);
int gm_jp_measure(const gm_jp_font_t* font, const char* utf8, int spacing);

#ifdef __cplusplus
}
#endif
