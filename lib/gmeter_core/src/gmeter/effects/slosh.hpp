// Sloshing liquid: a height field on the horizontal (lateral x depth) plane
// integrated with the nonlinear shallow-water equations in flux form, driven
// by the car's effective gravity. The free surface tilts so its slope equals
// a_horizontal / g_vertical, exactly like coffee in a cup holder, and waves
// travel at sqrt(g * depth).
//
// Grid: NX cells across the panel (dx = kW/NX pixels), NZ = kD cells through
// the stack (dz = layer pitch). Staggered (Arakawa C) layout: h at cell
// centres, u on x-faces, w on z-faces. Units are pixels and seconds; gravity
// is scaled by a "virtual tank width" so a 35 mm display sloshes like a
// 1 m tank instead of buzzing at 5 Hz.
#pragma once

#include "../effect.hpp"

namespace gm {

class SloshEffect : public Effect {
 public:
  static constexpr int NX = kW / 2;
  static constexpr int NZ = kD;
  static constexpr float DX = static_cast<float>(kW) / NX;
  static constexpr float DZ = kLayerPitchPx;

  SloshEffect();
  const char* name() const override { return "slosh"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  // Accessors for tests.
  float height(int x, int z) const { return h_[z][x]; }
  float totalVolume() const;
  float gravityPx() const;
  int lastSubsteps() const { return lastSubsteps_; }

 private:
  // tunables
  float fillFraction_ = 0.42f;   // rest depth as fraction of panel height
  float tankWidthM_ = 1.0f;      // virtual physical width of the tank in metres
  float slopeGain_ = 0.5f;       // 1.0 = physically exact tilt (1 g lateral dries out the tank)
  float damping_ = 0.6f;         // 1/s velocity damping
  float viscosity_ = 8.0f;       // surface diffusion px^2/s (kills checkerboarding)
  float ripple_ = 0.0f;          // idle ripple injection (px/s per step)
  float bobPxPerG_ = 10.0f;      // vertical bounce of the whole body per g of vertical jolt
  float bobHz_ = 3.0f;
  float splashGain_ = 60.0f;     // random velocity injected per g/s of vertical jerk
  float style_ = 1.0f;           // 0 surface only, 1 surface + dithered body, 2 solid
  float bodyIntensity_ = 0.12f;  // dither density of the body in style 1
  float surfacePx_ = 1.0f;       // surface line thickness
  float maxSubsteps_ = 12.0f;

  float h_[NZ][NX];
  float u_[NZ][NX + 1];
  float w_[NZ + 1][NX];
  float fluxX_[NZ][NX + 1];
  float fluxZ_[NZ + 1][NX];
  float targetVolume_ = 0.0f;
  Spring bob_;
  float lastVertG_ = 0.0f;
  float gvFiltered_ = 1.0f;      // ~1 Hz low-passed vertical g for the pressure term
  int lastSubsteps_ = 0;
  Rng rng_{12345u};

  void substep(float dt, float gv, const Vec3& gEff);
  void smooth(float amount);
  void renormalize();
};

}  // namespace gm
