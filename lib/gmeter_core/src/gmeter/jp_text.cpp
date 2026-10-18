#include "jp_text.hpp"

namespace gm {

namespace {
// Decodes one UTF-8 code point and advances `s`; invalid bytes yield U+FFFD.
uint32_t nextCodepoint(const char*& s) {
  const unsigned char c = static_cast<unsigned char>(*s++);
  if (c < 0x80) return c;
  int extra = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
  else return 0xFFFD;
  for (int i = 0; i < extra; ++i) {
    const unsigned char d = static_cast<unsigned char>(*s);
    if ((d & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (d & 0x3F);
    ++s;
  }
  return cp;
}

void blitGlyph(VoxelFrame& f, const JpGlyph& g, int height, int z, int x, int y, float intensity) {
  const int rb = (g.width + 7) / 8;
  for (int row = 0; row < height; ++row)
    for (int col = 0; col < g.width; ++col)
      if ((g.bits[row * rb + (col >> 3)] >> (7 - (col & 7))) & 1u) {
        if (intensity < 0.0f) f.reset(x + col, y + row, z);  // knock-out (reverse video)
        else f.plot(x + col, y + row, z, intensity);
      }
}

int glyphAdvance(const JpFont& font, uint32_t cp, const JpGlyph** out) {
  if (cp < 0x80) { *out = nullptr; return 4; }  // 3x5 HUD font
  const JpGlyph* g = findGlyph(font, cp);
  *out = g;
  return g ? g->width : font.height * 3 / 4;
}

void drawAscii(VoxelFrame& f, const JpFont& font, int z, int x, int y, char c, float intensity) {
  const char s[2] = {c, '\0'};
  if (intensity < 0.0f) {
    VoxelFrame tmp;  // knock-out for ASCII: render to a scratch frame, then clear
    tmp.text(0, x, y + (font.height - 5) / 2, s);
    for (int yy = 0; yy < 5; ++yy)
      for (int xx = 0; xx < 3; ++xx)
        if (tmp.get(x + xx, y + (font.height - 5) / 2 + yy, 0)) f.reset(x + xx, y + (font.height - 5) / 2 + yy, z);
    return;
  }
  f.text(z, x, y + (font.height - 5) / 2, s);
}
}  // namespace

const JpGlyph* findGlyph(const JpFont& font, uint32_t cp) {
  int lo = 0, hi = static_cast<int>(font.count) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint32_t c = font.glyphs[mid].codepoint;
    if (c == cp) return &font.glyphs[mid];
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

int drawJp(VoxelFrame& f, const JpFont& font, int z, int x, int y, const char* utf8, int spacing, float intensity) {
  int cx = x;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = nextCodepoint(s);
    const JpGlyph* g = nullptr;
    const int adv = glyphAdvance(font, cp, &g);
    if (g) blitGlyph(f, *g, font.height, z, cx, y, intensity);
    else if (cp < 0x80 && cp > 0x20) drawAscii(f, font, z, cx, y, static_cast<char>(cp), intensity);
    cx += adv + spacing;
  }
  return cx - x - (cx > x ? spacing : 0);
}

int drawJpVertical(VoxelFrame& f, const JpFont& font, int z, int x, int y, const char* utf8, int spacing,
                   float intensity) {
  int cy = y;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = nextCodepoint(s);
    if (cp == 0x30FC) {  // long-vowel mark turns vertical in tategaki
      const int mx = x + font.height * 3 / 8;
      for (int yy = cy + 2; yy < cy + font.height - 2; ++yy) {
        if (intensity < 0.0f) f.reset(mx, yy, z); else f.plot(mx, yy, z, intensity);
      }
      cy += font.height + spacing;
      continue;
    }
    const JpGlyph* g = nullptr;
    glyphAdvance(font, cp, &g);
    if (g) {
      blitGlyph(f, *g, font.height, z, x, cy, intensity);
      cy += font.height + spacing;
    } else if (cp < 0x80 && cp > 0x20) {
      drawAscii(f, font, z, x, cy - (font.height - 5) / 2, static_cast<char>(cp), intensity);
      cy += 5 + spacing;
    } else {
      cy += font.height + spacing;
    }
  }
  return cy - y;
}

int measureJp(const JpFont& font, const char* utf8, int spacing) {
  int w = 0;
  bool any = false;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = nextCodepoint(s);
    const JpGlyph* g = nullptr;
    w += glyphAdvance(font, cp, &g) + spacing;
    any = true;
  }
  return any ? w - spacing : 0;
}

}  // namespace gm
