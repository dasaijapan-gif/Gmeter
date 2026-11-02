// Japanese text on the voxel frame: glyph lookup, UTF-8 decoding and drawing
// (horizontal and tategaki). The glyph data lives in gm_jp_glyphs.c.
#include "gm_jp_text.h"

#include <stddef.h>
#include <stdlib.h>

// Decodes one UTF-8 code point and advances `*s`; invalid bytes yield U+FFFD.
static uint32_t next_codepoint(const char** s) {
  const unsigned char c = (unsigned char)*(*s)++;
  if (c < 0x80) return c;
  int extra = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    extra = 3;
  } else {
    return 0xFFFD;
  }
  for (int i = 0; i < extra; ++i) {
    const unsigned char d = (unsigned char)**s;
    if ((d & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (d & 0x3F);
    ++*s;
  }
  return cp;
}

static void blit_glyph(gm_frame_t* f, const gm_jp_glyph_t* g, int height, int z, int x, int y, float intensity) {
  const int rb = (g->width + 7) / 8;
  for (int row = 0; row < height; ++row)
    for (int col = 0; col < g->width; ++col)
      if ((g->bits[row * rb + (col >> 3)] >> (7 - (col & 7))) & 1u) {
        if (intensity < 0.0f)
          gm_frame_reset(f, x + col, y + row, z);  // knock-out (reverse video)
        else
          gm_frame_plot(f, x + col, y + row, z, intensity);
      }
}

static int glyph_advance(const gm_jp_font_t* font, uint32_t cp, const gm_jp_glyph_t** out) {
  if (cp < 0x80) {  // 3x5 HUD font
    *out = NULL;
    return 4;
  }
  const gm_jp_glyph_t* g = gm_jp_find_glyph(font, cp);
  *out = g;
  return g ? g->width : font->height * 3 / 4;
}

// A 3x5 HUD font character, full on at any intensity like gm_frame_text; a negative one knocks it out.
static void draw_ascii(gm_frame_t* f, const gm_jp_font_t* font, int z, int x, int y, char c, float intensity) {
  const int top = y + (font->height - 5) / 2;
  const uint16_t g = gm_glyph3x5(c);
  for (int row = 0; row < 5; ++row)
    for (int col = 0; col < 3; ++col)
      if ((g >> (12 - 3 * row)) & (4u >> col)) {
        if (intensity < 0.0f)
          gm_frame_reset(f, x + col, top + row, z);
        else
          gm_frame_set(f, x + col, top + row, z);
      }
}

static int compare_codepoint(const void* key, const void* glyph) {
  const uint32_t a = *(const uint32_t*)key, b = ((const gm_jp_glyph_t*)glyph)->codepoint;
  return (a > b) - (a < b);
}

const gm_jp_glyph_t* gm_jp_find_glyph(const gm_jp_font_t* font, uint32_t cp) {
  if (font->count == 0) return NULL;  // bsearch needs a valid array even when it is empty
  return bsearch(&cp, font->glyphs, font->count, sizeof *font->glyphs, compare_codepoint);
}

int gm_jp_draw(gm_frame_t* f, const gm_jp_font_t* font, int z, int x, int y, const char* utf8, int spacing,
               float intensity) {
  int cx = x;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = next_codepoint(&s);
    const gm_jp_glyph_t* g = NULL;
    const int adv = glyph_advance(font, cp, &g);
    if (g)
      blit_glyph(f, g, font->height, z, cx, y, intensity);
    else if (cp < 0x80 && cp > 0x20)
      draw_ascii(f, font, z, cx, y, (char)cp, intensity);
    cx += adv + spacing;
  }
  return cx - x - (cx > x ? spacing : 0);
}

int gm_jp_draw_vertical(gm_frame_t* f, const gm_jp_font_t* font, int z, int x, int y, const char* utf8, int spacing,
                        float intensity) {
  int cy = y;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = next_codepoint(&s);
    if (cp == 0x30FC) {  // long-vowel mark turns vertical in tategaki
      const int mx = x + font->height * 3 / 8;
      for (int yy = cy + 2; yy < cy + font->height - 2; ++yy) {
        if (intensity < 0.0f)
          gm_frame_reset(f, mx, yy, z);
        else
          gm_frame_plot(f, mx, yy, z, intensity);
      }
      cy += font->height + spacing;
      continue;
    }
    const gm_jp_glyph_t* g = NULL;
    glyph_advance(font, cp, &g);
    if (g) {
      blit_glyph(f, g, font->height, z, x, cy, intensity);
      cy += font->height + spacing;
    } else if (cp < 0x80 && cp > 0x20) {
      draw_ascii(f, font, z, x, cy - (font->height - 5) / 2, (char)cp, intensity);
      cy += 5 + spacing;
    } else {
      cy += font->height + spacing;
    }
  }
  return cy - y;
}

int gm_jp_measure(const gm_jp_font_t* font, const char* utf8, int spacing) {
  int w = 0;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = next_codepoint(&s);
    const gm_jp_glyph_t* g = NULL;
    w += glyph_advance(font, cp, &g) + spacing;
  }
  return *utf8 ? w - spacing : 0;
}
