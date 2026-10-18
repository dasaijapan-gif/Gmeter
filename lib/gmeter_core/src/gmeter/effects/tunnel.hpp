// Depth tunnel: one ring per layer, shrinking toward the back, whose centres
// are displaced progressively by the inertial force so the whole tunnel
// bends toward where a loose object would fly. Dashes rotate with lateral g
// and brightness pulses travel through the depth with longitudinal g, which
// makes braking/acceleration readable as motion along the stack.
#pragma once

#include "../effect.hpp"

namespace gm {

class TunnelEffect : public Effect {
 public:
  TunnelEffect();
  const char* name() const override { return "tunnel"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

 private:
  float scaleG_ = 1.0f;       // g for full bend
  float rFront_ = static_cast<float>(kH / 2 - 2);
  float rBack_ = 5.0f;
  float segments_ = 10.0f;    // dashes per ring
  float spin_ = 0.4f;         // rad/s at rest
  float spinPerG_ = 4.0f;     // extra rad/s per g lateral
  float pulseHz_ = 0.3f;      // pulses/s at rest
  float pulsePerG_ = 2.5f;    // pulses/s per g longitudinal
  float hz_ = 2.5f;           // smoothing

  Spring3 offs_;
  float phase_ = 0.0f;
  float depthPhase_ = 0.0f;
};

}  // namespace gm
