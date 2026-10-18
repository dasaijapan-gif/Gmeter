// Granular liquid: a few hundred particles thrown around the volume by the
// car's effective gravity. A coarse density grid supplies a pressure-like
// repulsion so the particles settle into a pile with volume instead of
// collapsing onto one pixel row (a tiny particle-in-cell fluid).
#pragma once

#include "../effect.hpp"

namespace gm {

class ParticleEffect : public Effect {
 public:
  static constexpr int kMaxParticles = 400;
  static constexpr int CELL = 8;                       // px per density cell on a layer
  static constexpr int GX = kW / CELL + 2, GY = kH / CELL + 2, GZ = kD + 2;  // padded

  ParticleEffect();
  const char* name() const override { return "particles"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  int activeCount() const { return static_cast<int>(count_); }
  const Vec3& position(int i) const { return pos_[i]; }
  float gravityPx() const;

 private:
  float count_ = 300.0f;
  float tankWidthM_ = 1.0f;
  float drag_ = 0.6f;          // 1/s
  float restitution_ = 0.25f;
  float pressure_ = 12000.0f;  // stiffness: acceleration = -(pressure / tank_m) * grad(max(rho - rho0, 0))
  float restDensity_ = 0.5f;   // particles per cell below which there is no repulsion
  float jitter_ = 15.0f;       // random px/s^2 to keep the pile alive
  float dotSize_ = 1.0f;
  float substeps_ = 2.0f;

  Vec3 pos_[kMaxParticles];
  Vec3 vel_[kMaxParticles];
  float density_[GZ][GY][GX];
  float pressureField_[GZ][GY][GX];
  Rng rng_{777u};

  void splatDensity();
  Vec3 pressureAccel(const Vec3& world) const;
  void substep(float dt, const Vec3& gEff);
};

}  // namespace gm
