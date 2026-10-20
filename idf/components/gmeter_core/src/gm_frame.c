// Bit-packed W x H x D monochrome voxel frame: drawing primitives, the 3x5
// HUD font, shift and crossfade. See gm_frame.h for the memory layout.
#include "gm_frame.h"

#include <stdlib.h>
#include <string.h>

// Standard 8x8 Bayer matrix (values 0..63).
const uint8_t gm_bayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21},
};

static inline void swap_int(int* a, int* b) {
  const int t = *a;
  *a = *b;
  *b = t;
}

void gm_frame_hline(gm_frame_t* f, int z, int x0, int x1, int y) {
  if ((unsigned)z >= (unsigned)GM_D || (unsigned)y >= (unsigned)GM_H) return;
  if (x0 > x1) swap_int(&x0, &x1);
  if (x1 < 0 || x0 >= GM_W) return;
  x0 = gm_clampi(x0, 0, GM_W - 1);
  x1 = gm_clampi(x1, 0, GM_W - 1);
  uint8_t* row = f->bits[z] + (y >> 3) * GM_W;
  const uint8_t bit = (uint8_t)(1u << (y & 7));
  for (int x = x0; x <= x1; ++x) row[x] |= bit;
}

void gm_frame_vline(gm_frame_t* f, int z, int x, int y0, int y1) {
  if ((unsigned)z >= (unsigned)GM_D || (unsigned)x >= (unsigned)GM_W) return;
  if (y0 > y1) swap_int(&y0, &y1);
  if (y1 < 0 || y0 >= GM_H) return;
  y0 = gm_clampi(y0, 0, GM_H - 1);
  y1 = gm_clampi(y1, 0, GM_H - 1);
  for (int y = y0; y <= y1; ++y) f->bits[z][(y >> 3) * GM_W + x] |= (uint8_t)(1u << (y & 7));
}

void gm_frame_line(gm_frame_t* f, int z, int x0, int y0, int x1, int y1) {
  // Trivial reject when both ends are beyond the same edge, and keep the
  // coordinates in a range where Bresenham's arithmetic cannot overflow.
  if ((x0 < 0 && x1 < 0) || (x0 >= GM_W && x1 >= GM_W) || (y0 < 0 && y1 < 0) || (y0 >= GM_H && y1 >= GM_H)) return;
  enum { kLim = 1 << 14 };
  x0 = gm_clampi(x0, -kLim, kLim);
  x1 = gm_clampi(x1, -kLim, kLim);
  y0 = gm_clampi(y0, -kLim, kLim);
  y1 = gm_clampi(y1, -kLim, kLim);
  // Bresenham; the loop ends at the far endpoint (guard is a safety net).
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int steps = gm_maxi(dx, -dy) + 1;
  for (int guard = 0; guard <= steps; ++guard) {
    gm_frame_set(f, x0, y0, z);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
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

void gm_frame_rect(gm_frame_t* f, int z, int x0, int y0, int x1, int y1) {
  gm_frame_hline(f, z, x0, x1, y0);
  gm_frame_hline(f, z, x0, x1, y1);
  gm_frame_vline(f, z, x0, y0, y1);
  gm_frame_vline(f, z, x1, y0, y1);
}

void gm_frame_fill_rect(gm_frame_t* f, int z, int x0, int y0, int x1, int y1, float intensity) {
  if (x0 > x1) swap_int(&x0, &x1);
  if (y0 > y1) swap_int(&y0, &y1);
  if (x1 < 0 || x0 >= GM_W || y1 < 0 || y0 >= GM_H) return;
  x0 = gm_clampi(x0, 0, GM_W - 1);
  x1 = gm_clampi(x1, 0, GM_W - 1);
  y0 = gm_clampi(y0, 0, GM_H - 1);
  y1 = gm_clampi(y1, 0, GM_H - 1);
  if (intensity >= 1.0f) {
    for (int y = y0; y <= y1; ++y) gm_frame_hline(f, z, x0, x1, y);
    return;
  }
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) gm_frame_plot(f, x, y, z, intensity);
}

void gm_frame_circle(gm_frame_t* f, int z, int cx, int cy, int r) {
  if (r < 0) return;
  if (r == 0) {
    gm_frame_set(f, cx, cy, z);
    return;
  }
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    gm_frame_set(f, cx + x, cy + y, z);
    gm_frame_set(f, cx - x, cy + y, z);
    gm_frame_set(f, cx + x, cy - y, z);
    gm_frame_set(f, cx - x, cy - y, z);
    gm_frame_set(f, cx + y, cy + x, z);
    gm_frame_set(f, cx - y, cy + x, z);
    gm_frame_set(f, cx + y, cy - x, z);
    gm_frame_set(f, cx - y, cy - x, z);
    ++y;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      --x;
      err += 2 * (y - x) + 1;
    }
  }
}

void gm_frame_disc(gm_frame_t* f, int z, float cx, float cy, float r, float intensity) {
  if ((unsigned)z >= (unsigned)GM_D || r <= 0.0f || intensity <= 0.0f) return;
  const int x0 = gm_maxi(0, gm_ifloor(cx - r)), x1 = gm_mini(GM_W - 1, gm_ifloor(cx + r) + 1);
  const int y0 = gm_maxi(0, gm_ifloor(cy - r)), y1 = gm_mini(GM_H - 1, gm_ifloor(cy + r) + 1);
  const float r2 = r * r;
  for (int y = y0; y <= y1; ++y) {
    const float dy = (float)y - cy;
    for (int x = x0; x <= x1; ++x) {
      const float dx = (float)x - cx;
      if (dx * dx + dy * dy <= r2) gm_frame_plot(f, x, y, z, intensity);
    }
  }
}

void gm_frame_sphere(gm_frame_t* f, gm_vec3_t world_center, float rxy, float rz, float intensity) {
  if (rxy <= 0.0f || rz <= 0.0f) return;
  // Always reach at least one layer plane, wherever the centre sits between layers.
  rz = gm_maxf(rz, 0.6f * GM_LAYER_PITCH_PX);
  const gm_vec3_t v = gm_world_to_voxel(world_center);
  for (int k = 0; k < GM_D; ++k) {
    const float dz = (gm_layer_z(k) - world_center.z) / rz;
    if (dz <= -1.0f || dz >= 1.0f) continue;
    const float r = rxy * sqrtf(1.0f - dz * dz);
    if (r < 0.35f) continue;
    gm_frame_disc(f, k, v.x, v.y, r, intensity);
  }
}

void gm_frame_point(gm_frame_t* f, gm_vec3_t world, float intensity) {
  const gm_vec3_t v = gm_world_to_voxel(world);
  gm_frame_plot(f, gm_iround(v.x), gm_iround(v.y), gm_iround(v.z), intensity);
}

void gm_frame_line3(gm_frame_t* f, gm_vec3_t a, gm_vec3_t b, float intensity) {
  const gm_vec3_t va = gm_world_to_voxel(a), vb = gm_world_to_voxel(b);
  // Largest of the three spans and 1 (the first wins a tie, as in std::max).
  const float n = gm_maxf(gm_maxf(gm_maxf(fabsf(vb.x - va.x), fabsf(vb.y - va.y)), fabsf(vb.z - va.z)), 1.0f);
  const int steps = (int)ceilf(n);
  for (int i = 0; i <= steps; ++i) {
    const float t = (float)i / (float)steps;
    const gm_vec3_t v = gm_vec3_lerp(va, vb, t);
    gm_frame_plot(f, gm_iround(v.x), gm_iround(v.y), gm_iround(v.z), intensity);
  }
}

// 3x5 glyphs: 5 rows of 3 bits, MSB = leftmost pixel, packed row0 in the
// highest bits.
#define G(r0, r1, r2, r3, r4) ((uint16_t)(((r0) << 12) | ((r1) << 9) | ((r2) << 6) | ((r3) << 3) | (r4)))
static const uint16_t k_digits[10] = {
    G(0b111, 0b101, 0b101, 0b101, 0b111), G(0b010, 0b110, 0b010, 0b010, 0b111),
    G(0b111, 0b001, 0b111, 0b100, 0b111), G(0b111, 0b001, 0b111, 0b001, 0b111),
    G(0b101, 0b101, 0b111, 0b001, 0b001), G(0b111, 0b100, 0b111, 0b001, 0b111),
    G(0b111, 0b100, 0b111, 0b101, 0b111), G(0b111, 0b001, 0b001, 0b001, 0b001),
    G(0b111, 0b101, 0b111, 0b101, 0b111), G(0b111, 0b101, 0b111, 0b001, 0b111),
};
static const uint16_t k_letters[26] = {
    G(0b010, 0b101, 0b111, 0b101, 0b101),  // A
    G(0b110, 0b101, 0b110, 0b101, 0b110),  // B
    G(0b111, 0b100, 0b100, 0b100, 0b111),  // C
    G(0b110, 0b101, 0b101, 0b101, 0b110),  // D
    G(0b111, 0b100, 0b111, 0b100, 0b111),  // E
    G(0b111, 0b100, 0b111, 0b100, 0b100),  // F
    G(0b111, 0b100, 0b101, 0b101, 0b111),  // G
    G(0b101, 0b101, 0b111, 0b101, 0b101),  // H
    G(0b111, 0b010, 0b010, 0b010, 0b111),  // I
    G(0b001, 0b001, 0b001, 0b101, 0b111),  // J
    G(0b101, 0b101, 0b110, 0b101, 0b101),  // K
    G(0b100, 0b100, 0b100, 0b100, 0b111),  // L
    G(0b101, 0b111, 0b111, 0b101, 0b101),  // M
    G(0b110, 0b101, 0b101, 0b101, 0b101),  // N
    G(0b111, 0b101, 0b101, 0b101, 0b111),  // O
    G(0b111, 0b101, 0b111, 0b100, 0b100),  // P
    G(0b111, 0b101, 0b101, 0b111, 0b001),  // Q
    G(0b111, 0b101, 0b110, 0b101, 0b101),  // R
    G(0b111, 0b100, 0b111, 0b001, 0b111),  // S
    G(0b111, 0b010, 0b010, 0b010, 0b010),  // T
    G(0b101, 0b101, 0b101, 0b101, 0b111),  // U
    G(0b101, 0b101, 0b101, 0b101, 0b010),  // V
    G(0b101, 0b101, 0b111, 0b111, 0b101),  // W
    G(0b101, 0b101, 0b010, 0b101, 0b101),  // X
    G(0b101, 0b101, 0b010, 0b010, 0b010),  // Y
    G(0b111, 0b001, 0b010, 0b100, 0b111),  // Z
};

uint16_t gm_glyph3x5(char c) {
  if (c >= '0' && c <= '9') return k_digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return k_letters[c - 'A'];
  if (c >= 'a' && c <= 'z') return k_letters[c - 'a'];
  switch (c) {
    case '.': return G(0b000, 0b000, 0b000, 0b000, 0b010);
    case '-': return G(0b000, 0b000, 0b111, 0b000, 0b000);
    case '+': return G(0b000, 0b010, 0b111, 0b010, 0b000);
    case ':': return G(0b000, 0b010, 0b000, 0b010, 0b000);
    case '/': return G(0b001, 0b001, 0b010, 0b100, 0b100);
    case '%': return G(0b101, 0b001, 0b010, 0b100, 0b101);
    case '<': return G(0b001, 0b010, 0b100, 0b010, 0b001);
    case '>': return G(0b100, 0b010, 0b001, 0b010, 0b100);
    case '^': return G(0b010, 0b101, 0b000, 0b000, 0b000);
    case 'v': return G(0b000, 0b000, 0b000, 0b101, 0b010);
    default: return 0;
  }
}
#undef G

int gm_frame_text(gm_frame_t* f, int z, int x, int y, const char* s) {
  int cursor = x;
  for (; *s; ++s) {
    const uint16_t g = gm_glyph3x5(*s);
    for (int row = 0; row < 5; ++row) {
      const unsigned bits = (g >> (12 - 3 * row)) & 7u;
      if (bits & 4u) gm_frame_set(f, cursor, y + row, z);
      if (bits & 2u) gm_frame_set(f, cursor + 1, y + row, z);
      if (bits & 1u) gm_frame_set(f, cursor + 2, y + row, z);
    }
    cursor += 4;
  }
  return cursor - x;
}

void gm_frame_or(gm_frame_t* f, const gm_frame_t* o) {
  uint8_t* d = gm_frame_data(f);
  const uint8_t* s = gm_frame_cdata(o);
  for (int i = 0; i < GM_BYTES_PER_FRAME; ++i) d[i] |= s[i];
}

void gm_frame_crossfade(gm_frame_t* f, const gm_frame_t* a, const gm_frame_t* b, float t) {
  if (t <= 0.0f) {
    if (f != a) memcpy(f->bits, a->bits, sizeof(f->bits));
    return;
  }
  if (t >= 1.0f) {
    if (f != b) memcpy(f->bits, b->bits, sizeof(f->bits));
    return;
  }
  for (int z = 0; z < GM_D; ++z) {
    for (int page = 0; page < GM_PAGES; ++page) {
      for (int x = 0; x < GM_W; ++x) {
        uint8_t mask = 0;  // 1 => take b
        for (int bit = 0; bit < 8; ++bit)
          if (t > gm_frame_bayer_threshold(x, page * 8 + bit, z)) mask |= (uint8_t)(1u << bit);
        const int i = page * GM_W + x;
        f->bits[z][i] = (uint8_t)((a->bits[z][i] & ~mask) | (b->bits[z][i] & mask));
      }
    }
  }
}

void gm_frame_shift(gm_frame_t* f, int dx, int dy) {
  if (dx == 0 && dy == 0) return;
  if (dx <= -GM_W || dx >= GM_W || dy <= -GM_H || dy >= GM_H) {
    gm_frame_clear(f);
    return;
  }
#if GM_H >= 64
  const uint64_t mask = ~0ull;
#else
  const uint64_t mask = (1ull << GM_H) - 1ull;
#endif
  for (int z = 0; z < GM_D; ++z) {
    uint8_t* L = f->bits[z];
    if (dx != 0) {
      for (int page = 0; page < GM_PAGES; ++page) {
        uint8_t* row = L + page * GM_W;
        if (dx > 0) {
          memmove(row + dx, row, (size_t)(GM_W - dx));
          memset(row, 0, (size_t)dx);
        } else {
          memmove(row, row - dx, (size_t)(GM_W + dx));
          memset(row + GM_W + dx, 0, (size_t)(-dx));
        }
      }
    }
    if (dy != 0) {
      for (int x = 0; x < GM_W; ++x) {
        uint64_t col = 0;
        for (int page = 0; page < GM_PAGES; ++page) col |= (uint64_t)L[page * GM_W + x] << (8 * page);
        col = dy > 0 ? (col << dy) : (col >> -dy);
        col &= mask;
        for (int page = 0; page < GM_PAGES; ++page) L[page * GM_W + x] = (uint8_t)(col >> (8 * page));
      }
    }
  }
}

void gm_frame_clear_box(gm_frame_t* f, int x0, int y0, int x1, int y1) {
  if (x0 > x1) swap_int(&x0, &x1);
  if (y0 > y1) swap_int(&y0, &y1);
  if (x1 < 0 || x0 >= GM_W || y1 < 0 || y0 >= GM_H) return;
  x0 = gm_clampi(x0, 0, GM_W - 1);
  x1 = gm_clampi(x1, 0, GM_W - 1);
  y0 = gm_clampi(y0, 0, GM_H - 1);
  y1 = gm_clampi(y1, 0, GM_H - 1);
  for (int z = 0; z < GM_D; ++z)
    for (int y = y0; y <= y1; ++y) {
      const uint8_t mask = (uint8_t)~(1u << (y & 7));
      uint8_t* row = f->bits[z] + (y >> 3) * GM_W;
      for (int x = x0; x <= x1; ++x) row[x] &= mask;
    }
}

int gm_frame_count_lit_layer(const gm_frame_t* f, int z) {
  int n = 0;
  for (int i = 0; i < GM_BYTES_PER_LAYER; ++i) n += __builtin_popcount(f->bits[z][i]);
  return n;
}

int gm_frame_count_lit(const gm_frame_t* f) {
  int n = 0;
  for (int z = 0; z < GM_D; ++z) n += gm_frame_count_lit_layer(f, z);
  return n;
}
