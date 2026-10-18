#include "tunnel.hpp"

#include <cmath>

namespace gm {

TunnelEffect::TunnelEffect() {
  params_.add("scale_g", &scaleG_, 0.2f, 3.0f, "g for full bend");
  params_.add("r_front", &rFront_, 4.0f, 31.0f, "front ring radius px");
  params_.add("r_back", &rBack_, 1.0f, 20.0f, "back ring radius px");
  params_.add("segments", &segments_, 1.0f, 24.0f, "dashes per ring");
  params_.add("spin", &spin_, 0.0f, 5.0f, "idle rotation rad/s");
  params_.add("spin_g", &spinPerG_, 0.0f, 20.0f, "rotation per lateral g");
  params_.add("pulse", &pulseHz_, 0.0f, 3.0f, "idle depth pulses/s");
  params_.add("pulse_g", &pulsePerG_, 0.0f, 10.0f, "depth pulses per longitudinal g");
  params_.add("hz", &hz_, 0.3f, 8.0f, "smoothing");
  reset();
}

void TunnelEffect::reset() {
  offs_.snap(Vec3{});
  phase_ = 0.0f;
  depthPhase_ = 0.0f;
}

void TunnelEffect::update(const EffectContext& ctx) {
  const Vec3 he = VoxelFrame::halfExtent();
  const Vec3 inertial = ctx.g.inertial();
  // Full bend puts the back ring at the edge of the panel.
  const Vec3 target{inertial.x * (he.x - rBack_ - 1.0f) / scaleG_,
                    inertial.y * (he.y - rBack_ - 1.0f) / (scaleG_ * 0.6f), 0.0f};
  offs_.step(target, ctx.dt, hz_);
  const float lat = ctx.g.lateralG();
  phase_ += ctx.dt * (spin_ + spinPerG_ * lat);
  // Braking (inertial.z > 0) sends pulses toward the windshield.
  depthPhase_ += ctx.dt * (pulseHz_ + pulsePerG_ * inertial.z);
  if (phase_ > kTwoPi) phase_ -= kTwoPi;
  if (phase_ < 0.0f) phase_ += kTwoPi;
  depthPhase_ -= std::floor(depthPhase_);
}

void TunnelEffect::render(VoxelFrame& f) {
  const Vec3 he = VoxelFrame::halfExtent();
  const Vec3 off = offs_.pos();
  const int segs = std::max(1, iround(segments_));
  const float segArc = kTwoPi / static_cast<float>(segs);
  for (int k = 0; k < kD; ++k) {
    const float t = kD > 1 ? static_cast<float>(k) / static_cast<float>(kD - 1) : 0.0f;
    const float r = lerpf(rFront_, rBack_, t);
    const float cxw = clampf(off.x * t, -(he.x - r - 1.0f), he.x - r - 1.0f);
    const float cyw = clampf(off.y * t, -(he.y - r - 1.0f), he.y - r - 1.0f);
    const Vec3 c = VoxelFrame::worldToVoxel(Vec3{cxw, cyw, VoxelFrame::layerZ(k)});
    const float pulse = 0.55f + 0.45f * std::cos(kTwoPi * (depthPhase_ - t));
    const int n = std::max(16, static_cast<int>(r * 6.0f));
    for (int i = 0; i < n; ++i) {
      const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
      float local = std::fmod(a + phase_ * (1.0f + 0.5f * t), segArc);
      if (local < 0.0f) local += segArc;
      if (local > segArc * 0.6f) continue;  // gap
      f.plot(iround(c.x + r * std::cos(a)), iround(c.y + r * std::sin(a)), k, pulse);
    }
  }
}

}  // namespace gm
