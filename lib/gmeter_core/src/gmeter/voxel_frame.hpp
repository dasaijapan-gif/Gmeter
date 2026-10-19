// Bit-packed W x H x D monochrome voxel frame.
//
// Each layer is stored exactly as an SSD1306/SSD1309 expects its GDDRAM in
// horizontal addressing mode: kPages rows of kW bytes, each byte holding 8
// vertically stacked pixels with bit 0 at the top. The display driver can
// therefore DMA a layer straight out of this buffer with no repacking.
//
// Coordinates: x = column 0..kW-1 (left -> right), y = row 0..kH-1 (top ->
// bottom), z = layer 0..kD-1 (nearest the viewer -> farthest / windshield).
// Lit voxels on different layers add visually; nothing occludes.
#pragma once

#include <cstdint>
#include <cstring>

#include "config.hpp"
#include "math.hpp"

namespace gm {

class VoxelFrame {
 public:
  static constexpr int W = kW, H = kH, D = kD;

  VoxelFrame() { clear(); }

  void clear() { std::memset(bits_, 0, sizeof(bits_)); }
  void fill() { std::memset(bits_, 0xFF, sizeof(bits_)); }

  static bool inBounds(int x, int y, int z) {
    return static_cast<unsigned>(x) < static_cast<unsigned>(W) &&
           static_cast<unsigned>(y) < static_cast<unsigned>(H) &&
           static_cast<unsigned>(z) < static_cast<unsigned>(D);
  }

  // Set a voxel (bounds-checked; out-of-range is a silent no-op).
  void set(int x, int y, int z) {
    if (inBounds(x, y, z)) bits_[z][(y >> 3) * W + x] |= static_cast<uint8_t>(1u << (y & 7));
  }
  void reset(int x, int y, int z) {
    if (inBounds(x, y, z)) bits_[z][(y >> 3) * W + x] &= static_cast<uint8_t>(~(1u << (y & 7)));
  }
  bool get(int x, int y, int z) const {
    return inBounds(x, y, z) && (bits_[z][(y >> 3) * W + x] >> (y & 7)) & 1u;
  }

  // Ordered-dither plot: lights the voxel if `intensity` (0..1) exceeds the
  // Bayer 8x8 threshold at this position. Each layer uses a different offset
  // into the matrix so patterns on adjacent layers do not line up and moire.
  void plot(int x, int y, int z, float intensity) {
    if (intensity >= 1.0f) { set(x, y, z); return; }
    if (intensity <= 0.0f) return;
    if (intensity > bayerThreshold(x, y, z)) set(x, y, z);
  }
  static float bayerThreshold(int x, int y, int z) {
    const int ox = (z * 3) & 7, oy = (z * 5) & 7;
    return (kBayer8[(y + oy) & 7][(x + ox) & 7] + 0.5f) * (1.0f / 64.0f);
  }

  // ---- 2D primitives on one layer -----------------------------------------
  void hline(int z, int x0, int x1, int y);
  void vline(int z, int x, int y0, int y1);
  void line(int z, int x0, int y0, int x1, int y1);
  void rect(int z, int x0, int y0, int x1, int y1);         // outline
  void fillRect(int z, int x0, int y0, int x1, int y1, float intensity = 1.0f);
  void circle(int z, int cx, int cy, int r);                 // outline, integer
  void disc(int z, float cx, float cy, float r, float intensity = 1.0f);  // filled, sub-pixel centre

  // ---- 3D primitives ---------------------------------------------------------
  // Sphere/ellipsoid centred at world position (see worldToVoxel): rxy is the
  // radius in pixels on a layer, rz the radius along the depth axis in
  // *pixel units* (so rz = 1.5 * kLayerPitchPx spans about three layers).
  void sphere(const Vec3& worldCenter, float rxy, float rz, float intensity = 1.0f);
  // Single voxel at a world position (nearest).
  void point(const Vec3& world, float intensity = 1.0f);
  // Straight 3D line between two world positions, stepping through layers.
  void line3(const Vec3& a, const Vec3& b, float intensity = 1.0f);

  // Tiny 3x5 font for HUD text; returns advance in pixels.
  int text(int z, int x, int y, const char* s);
  // The 3x5 glyph for `c`: 5 rows of 3 bits, row 0 in bits 14..12, MSB = left.
  static uint16_t glyph3x5(char c);
  // Clears a box through every layer (e.g. behind HUD text so it stays legible).
  void clearBox(int x0, int y0, int x1, int y1);

  // ---- world <-> voxel mapping ------------------------------------------------
  // World frame = car frame in pixel units, origin at the volume centre:
  // x right, y up, z forward (deeper). Layer k sits at z = (k - (D-1)/2) * pitch.
  static Vec3 voxelToWorld(float x, float y, float z) {
    return {x - W * 0.5f + 0.5f, H * 0.5f - 0.5f - y, (z - (D - 1) * 0.5f) * kLayerPitchPx};
  }
  static Vec3 worldToVoxel(const Vec3& w) {
    return {w.x + W * 0.5f - 0.5f, H * 0.5f - 0.5f - w.y, w.z / kLayerPitchPx + (D - 1) * 0.5f};
  }
  static float layerZ(int k) { return (static_cast<float>(k) - (D - 1) * 0.5f) * kLayerPitchPx; }
  // Half-extents of the simulation box in world units.
  static Vec3 halfExtent() { return {W * 0.5f, H * 0.5f, kDepthPx * 0.5f}; }

  // ---- raw access ---------------------------------------------------------------
  uint8_t* layer(int z) { return bits_[z]; }
  const uint8_t* layer(int z) const { return bits_[z]; }
  uint8_t* data() { return &bits_[0][0]; }
  const uint8_t* data() const { return &bits_[0][0]; }
  static constexpr int layerBytes() { return kBytesPerLayer; }
  static constexpr int frameBytes() { return kBytesPerFrame; }

  // Combine: this |= other.
  void orWith(const VoxelFrame& o) {
    for (int i = 0; i < kBytesPerFrame; ++i) data()[i] |= o.data()[i];
  }
  // Dithered crossfade between a and b (t = 0 -> a, 1 -> b).
  void crossfade(const VoxelFrame& a, const VoxelFrame& b, float t);

  int countLit() const;
  int countLitLayer(int z) const;

 private:
  static const uint8_t kBayer8[8][8];
  uint8_t bits_[D][kBytesPerLayer];
};

}  // namespace gm
