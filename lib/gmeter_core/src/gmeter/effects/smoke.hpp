// Low-resolution 3D "stable fluids" (Jos Stam) smoke. A plume rises from an
// emitter at the centre of the volume and is blown around by the inertial
// force, so it leans back under acceleration, surges toward the windshield
// under braking and smears sideways in corners.
//
// Grid resolution is a compile-time choice: GM_SMOKE_NX x GM_SMOKE_NY cells
// per layer and one cell per layer in depth. NX must divide the panel width
// and NY the panel height. The default is 4x4-pixel cells (32x14 at 128x56,
// 32x16 at 128x64); the ESP32 builds use 16x8 (platformio.ini) to stay within
// the time and memory budget. wind/buoyancy/emit_r are in cell units, so
// tune on the same grid the device uses (Gmeter Studio builds 16x8).
// Buffers are heap-allocated only while active.
#pragma once

#include "../effect.hpp"

#ifndef GM_SMOKE_NX
#define GM_SMOKE_NX 32
#endif
#ifndef GM_SMOKE_NY
#define GM_SMOKE_NY (GM_PANEL_H / 4)
#endif

namespace gm {

class SmokeEffect : public Effect {
 public:
  static constexpr int NX = GM_SMOKE_NX, NY = GM_SMOKE_NY, NZ = kD;
  static constexpr int SX = NX + 2, SY = NY + 2, SZ = NZ + 2;
  static constexpr int N = SX * SY * SZ;
  static constexpr int CX = kW / NX, CY = kH / NY;  // pixels per cell
  static_assert(kW % NX == 0 && kH % NY == 0, "smoke grid must tile the panel");

  SmokeEffect();
  ~SmokeEffect() override;
  const char* name() const override { return "smoke"; }
  void activate() override;
  void deactivate() override;
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  bool allocated() const { return d_ != nullptr; }
  static constexpr int idx(int i, int j, int k) { return (k * SY + j) * SX + i; }
  float density(int i, int j, int k) const { return d_ ? d_[idx(i, j, k)] : 0.0f; }
  float totalDensity() const;
  static size_t memoryBytes() { return sizeof(float) * N * 8; }

 private:
  float wind_ = 30.0f;        // cells/s^2 per g of inertial force
  float buoyancy_ = 3.0f;     // cells/s^2 per unit density
  float emit_ = 4.0f;         // density per second at the emitter
  float emitRadius_ = 1.5f;   // cells
  float emitDepth_ = 1.0f;    // layers (half extent)
  float dissipation_ = 1.2f;  // 1/s
  float iters_ = 6.0f;        // Gauss-Seidel iterations per projection
  float rateHz_ = 30.0f;      // physics steps per second (render keeps the frame rate)
  float accum_ = 0.0f;        // schedule phase (keeps its remainder)
  float sinceStep_ = 0.0f;    // real time since the last solver step
  float gain_ = 1.5f;         // density -> pixel intensity
  float swirl_ = 2.0f;        // random velocity injected at the emitter

  float *u_ = nullptr, *v_ = nullptr, *w_ = nullptr;
  float *u0_ = nullptr, *v0_ = nullptr, *w0_ = nullptr;
  float *d_ = nullptr, *d0_ = nullptr;
  Rng rng_{4242u};

  void allocate();
  void release();
  void setBnd(int b, float* x);
  void linSolve(int b, float* x, const float* x0, float a, float c, int iters);
  void advect(int b, float* d, const float* d0, const float* u, const float* v, const float* w, float dt);
  void project(float* u, float* v, float* w, float* p, float* div);
};

}  // namespace gm
