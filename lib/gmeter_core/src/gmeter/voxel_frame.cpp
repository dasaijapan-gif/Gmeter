#include "voxel_frame.hpp"

#include <algorithm>
#include <cmath>

namespace gm {

// Standard 8x8 Bayer matrix (values 0..63).
const uint8_t VoxelFrame::kBayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21},
};

void VoxelFrame::hline(int z, int x0, int x1, int y) {
  if (static_cast<unsigned>(z) >= static_cast<unsigned>(D) || static_cast<unsigned>(y) >= static_cast<unsigned>(H)) return;
  if (x0 > x1) std::swap(x0, x1);
  x0 = clampi(x0, 0, W - 1);
  x1 = clampi(x1, 0, W - 1);
  uint8_t* row = bits_[z] + (y >> 3) * W;
  const uint8_t bit = static_cast<uint8_t>(1u << (y & 7));
  for (int x = x0; x <= x1; ++x) row[x] |= bit;
}

void VoxelFrame::vline(int z, int x, int y0, int y1) {
  if (y0 > y1) std::swap(y0, y1);
  for (int y = y0; y <= y1; ++y) set(x, y, z);
}

void VoxelFrame::line(int z, int x0, int y0, int x1, int y1) {
  // Bresenham.
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < W + H + 2; ++guard) {
    set(x0, y0, z);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void VoxelFrame::rect(int z, int x0, int y0, int x1, int y1) {
  hline(z, x0, x1, y0);
  hline(z, x0, x1, y1);
  vline(z, x0, y0, y1);
  vline(z, x1, y0, y1);
}

void VoxelFrame::fillRect(int z, int x0, int y0, int x1, int y1, float intensity) {
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  x0 = clampi(x0, 0, W - 1); x1 = clampi(x1, 0, W - 1);
  y0 = clampi(y0, 0, H - 1); y1 = clampi(y1, 0, H - 1);
  if (intensity >= 1.0f) {
    for (int y = y0; y <= y1; ++y) hline(z, x0, x1, y);
    return;
  }
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) plot(x, y, z, intensity);
}

void VoxelFrame::circle(int z, int cx, int cy, int r) {
  if (r < 0) return;
  if (r == 0) { set(cx, cy, z); return; }
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    set(cx + x, cy + y, z); set(cx - x, cy + y, z);
    set(cx + x, cy - y, z); set(cx - x, cy - y, z);
    set(cx + y, cy + x, z); set(cx - y, cy + x, z);
    set(cx + y, cy - x, z); set(cx - y, cy - x, z);
    ++y;
    if (err < 0) err += 2 * y + 1;
    else { --x; err += 2 * (y - x) + 1; }
  }
}

void VoxelFrame::disc(int z, float cx, float cy, float r, float intensity) {
  if (static_cast<unsigned>(z) >= static_cast<unsigned>(D) || r <= 0.0f || intensity <= 0.0f) return;
  const int x0 = std::max(0, ifloor(cx - r)), x1 = std::min(W - 1, ifloor(cx + r) + 1);
  const int y0 = std::max(0, ifloor(cy - r)), y1 = std::min(H - 1, ifloor(cy + r) + 1);
  const float r2 = r * r;
  for (int y = y0; y <= y1; ++y) {
    const float dy = static_cast<float>(y) - cy;
    for (int x = x0; x <= x1; ++x) {
      const float dx = static_cast<float>(x) - cx;
      if (dx * dx + dy * dy <= r2) plot(x, y, z, intensity);
    }
  }
}

void VoxelFrame::sphere(const Vec3& worldCenter, float rxy, float rz, float intensity) {
  if (rxy <= 0.0f || rz <= 0.0f) return;
  const Vec3 v = worldToVoxel(worldCenter);
  for (int k = 0; k < D; ++k) {
    const float dz = (layerZ(k) - worldCenter.z) / rz;
    if (dz <= -1.0f || dz >= 1.0f) continue;
    const float r = rxy * std::sqrt(1.0f - dz * dz);
    if (r < 0.35f) continue;
    disc(k, v.x, v.y, r, intensity);
  }
}

void VoxelFrame::point(const Vec3& world, float intensity) {
  const Vec3 v = worldToVoxel(world);
  plot(iround(v.x), iround(v.y), iround(v.z), intensity);
}

void VoxelFrame::line3(const Vec3& a, const Vec3& b, float intensity) {
  const Vec3 va = worldToVoxel(a), vb = worldToVoxel(b);
  const float n = std::max({std::fabs(vb.x - va.x), std::fabs(vb.y - va.y), std::fabs(vb.z - va.z), 1.0f});
  const int steps = static_cast<int>(std::ceil(n));
  for (int i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const Vec3 v = lerp(va, vb, t);
    plot(iround(v.x), iround(v.y), iround(v.z), intensity);
  }
}

namespace {
// 3x5 glyphs: 5 rows of 3 bits, MSB = leftmost pixel, packed row0 in the
// highest bits.
constexpr uint16_t G(unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4) {
  return static_cast<uint16_t>((r0 << 12) | (r1 << 9) | (r2 << 6) | (r3 << 3) | r4);
}
constexpr uint16_t kDigits[10] = {
    G(0b111, 0b101, 0b101, 0b101, 0b111), G(0b010, 0b110, 0b010, 0b010, 0b111),
    G(0b111, 0b001, 0b111, 0b100, 0b111), G(0b111, 0b001, 0b111, 0b001, 0b111),
    G(0b101, 0b101, 0b111, 0b001, 0b001), G(0b111, 0b100, 0b111, 0b001, 0b111),
    G(0b111, 0b100, 0b111, 0b101, 0b111), G(0b111, 0b001, 0b001, 0b001, 0b001),
    G(0b111, 0b101, 0b111, 0b101, 0b111), G(0b111, 0b101, 0b111, 0b001, 0b111),
};
constexpr uint16_t kLetters[26] = {
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

uint16_t glyphFor(char c) {
  if (c >= '0' && c <= '9') return kDigits[c - '0'];
  if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
  if (c >= 'a' && c <= 'z') return kLetters[c - 'a'];
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
}  // namespace

int VoxelFrame::text(int z, int x, int y, const char* s) {
  int cursor = x;
  for (; *s; ++s) {
    const uint16_t g = glyphFor(*s);
    for (int row = 0; row < 5; ++row) {
      const unsigned bits = (g >> (12 - 3 * row)) & 7u;
      if (bits & 4u) set(cursor, y + row, z);
      if (bits & 2u) set(cursor + 1, y + row, z);
      if (bits & 1u) set(cursor + 2, y + row, z);
    }
    cursor += 4;
  }
  return cursor - x;
}

void VoxelFrame::crossfade(const VoxelFrame& a, const VoxelFrame& b, float t) {
  if (t <= 0.0f) { *this = a; return; }
  if (t >= 1.0f) { *this = b; return; }
  for (int z = 0; z < D; ++z) {
    for (int page = 0; page < kPages; ++page) {
      for (int x = 0; x < W; ++x) {
        uint8_t mask = 0;  // 1 => take b
        for (int bit = 0; bit < 8; ++bit)
          if (t > bayerThreshold(x, page * 8 + bit, z)) mask |= static_cast<uint8_t>(1u << bit);
        const int i = page * W + x;
        bits_[z][i] = static_cast<uint8_t>((a.bits_[z][i] & ~mask) | (b.bits_[z][i] & mask));
      }
    }
  }
}

int VoxelFrame::countLitLayer(int z) const {
  int n = 0;
  for (int i = 0; i < kBytesPerLayer; ++i) n += __builtin_popcount(bits_[z][i]);
  return n;
}

int VoxelFrame::countLit() const {
  int n = 0;
  for (int z = 0; z < D; ++z) n += countLitLayer(z);
  return n;
}

}  // namespace gm
