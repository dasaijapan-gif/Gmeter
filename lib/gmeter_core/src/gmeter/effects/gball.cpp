#include "gball.hpp"

#include <algorithm>

#include <cstdio>

namespace gm {

GBallEffect::GBallEffect() {
  params_.add("scale_g", &fullScaleG_, 0.2f, 3.0f, "lateral g at full travel");
  params_.add("vscale_g", &verticalScaleG_, 0.1f, 3.0f, "vertical g at full travel");
  params_.add("dscale_g", &depthScaleG_, 0.2f, 3.0f, "longitudinal g at full depth");
  params_.add("hz", &naturalHz_, 0.3f, 8.0f, "spring natural frequency");
  params_.add("zeta", &damping_, 0.2f, 2.0f, "spring damping ratio (1 = critical)");
  params_.add("radius", &radius_, 1.0f, 16.0f, "ball radius px");
  params_.add("depth_r", &depthRadiusLayers_, 0.6f, 4.0f, "ball half-depth in layers");
  params_.add("trail_s", &trailSeconds_, 0.0f, 4.0f, "trail persistence seconds");
  params_.add("trail_i", &trailIntensity_, 0.0f, 1.0f, "trail brightness");
  params_.add("inertia", &followInertia_, 0.0f, 1.0f, "1 = ball is thrown like a loose object");
  params_.add("hud", &hud_, 0.0f, 1.0f, "numeric readout on front layer");
  params_.add("rings", &rings_, 0.0f, 1.0f, "0.5g/1g reference rings");
  reset();
}

void GBallEffect::reset() {
  spring_.snap(Vec3{});
  trailHead_ = trailCount_ = 0;
  trailTimer_ = 0.0f;
  peakG_ = 0.0f;
  peakAge_ = 0.0f;
}

Vec3 GBallEffect::travel() const {
  const Vec3 he = VoxelFrame::halfExtent();
  return {(he.x - radius_ - 1.0f) / fullScaleG_,
          (he.y - radius_ - 1.0f) / verticalScaleG_,
          (he.z - kLayerPitchPx * 0.5f) / depthScaleG_};
}

void GBallEffect::update(const EffectContext& ctx) {
  lastG_ = ctx.g;
  const Vec3 drive = followInertia_ > 0.5f ? ctx.g.inertial() : ctx.g.dynamic();
  const Vec3 tr = travel();
  const Vec3 he = VoxelFrame::halfExtent();
  Vec3 target = drive.scaled(tr);
  target.x = clampf(target.x, -(he.x - radius_ - 1.0f), he.x - radius_ - 1.0f);
  target.y = clampf(target.y, -(he.y - radius_ - 1.0f), he.y - radius_ - 1.0f);
  target.z = clampf(target.z, -(he.z - kLayerPitchPx * 0.5f), he.z - kLayerPitchPx * 0.5f);
  spring_.step(target, ctx.dt, naturalHz_, damping_);

  // Trail bookkeeping: sample at ~40 Hz, age out.
  for (int i = 0; i < trailCount_; ++i) trailAge_[i] += ctx.dt;
  // Space samples so the ring buffer spans the whole trail length.
  trailTimer_ += ctx.dt;
  const float interval = std::max(0.025f, trailSeconds_ / static_cast<float>(kTrailMax - 1));
  if (trailSeconds_ > 0.0f && trailTimer_ >= interval) {
    trailTimer_ -= interval;
    if (trailTimer_ > interval) trailTimer_ = 0.0f;
    trail_[trailHead_] = spring_.pos();
    trailAge_[trailHead_] = 0.0f;
    trailHead_ = (trailHead_ + 1) % kTrailMax;
    if (trailCount_ < kTrailMax) ++trailCount_;
  }

  // Peak hold with slow decay.
  const float hg = ctx.g.horizontalG();
  if (hg >= peakG_) { peakG_ = hg; peakAge_ = 0.0f; }
  else {
    peakAge_ += ctx.dt;
    if (peakAge_ > 3.0f) peakG_ = std::max(hg, peakG_ - 0.5f * ctx.dt);
  }
}

void GBallEffect::render(VoxelFrame& f) {
  const Vec3 pos = spring_.pos();

  // Trail: older samples dimmer (dithered sparser).
  if (trailSeconds_ > 0.0f && trailIntensity_ > 0.0f) {
    for (int i = 0; i < trailCount_; ++i) {
      const float age = trailAge_[i];
      if (age > trailSeconds_) continue;
      const float fade = 1.0f - age / trailSeconds_;
      f.point(trail_[i], trailIntensity_ * fade * fade);
    }
  }

  f.sphere(pos, radius_, depthRadiusLayers_ * kLayerPitchPx, 1.0f);

  if (rings_ > 0.5f) {
    // Reference ellipses at 0.5 g and 1 g on the front layer, using the same
    // per-axis mapping as the ball (clipped where they leave the panel), drawn
    // as sparse dashes so they stay dim but legible.
    const Vec3 tr = travel();
    const Vec3 c = VoxelFrame::worldToVoxel(Vec3{});
    for (int ring = 1; ring <= 2; ++ring) {
      const float gval = 0.5f * static_cast<float>(ring);
      const float rx = tr.x * gval, ry = tr.y * gval;
      const float perimeter = kPi * (3.0f * (rx + ry) - std::sqrt((3.0f * rx + ry) * (rx + 3.0f * ry)));
      const int n = std::max(24, static_cast<int>(perimeter));
      const int every = ring == 2 ? 3 : 5;
      for (int i = 0; i < n; i += every) {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        f.set(iround(c.x + rx * std::cos(a)), iround(c.y + ry * std::sin(a)), 0);
      }
    }
    // Reticle ticks.
    f.hline(0, iround(c.x) - 3, iround(c.x) + 3, iround(c.y));
    f.vline(0, iround(c.x), iround(c.y) - 2, iround(c.y) + 2);
  }

  if (hud_ > 0.5f) {
    char buf[12];
    const float hg = lastG_.horizontalG();
    std::snprintf(buf, sizeof(buf), "%d.%02dG", static_cast<int>(hg), static_cast<int>(hg * 100.0f) % 100);
    f.text(0, 1, VoxelFrame::H - 6, buf);
    std::snprintf(buf, sizeof(buf), "P%d.%02d", static_cast<int>(peakG_), static_cast<int>(peakG_ * 100.0f) % 100);
    f.text(0, VoxelFrame::W - 1 - 4 * 5, VoxelFrame::H - 6, buf);
    // Longitudinal bar along the top edge: braking fills to the right of
    // centre (ball recedes), acceleration to the left (ball approaches).
    const int cx = VoxelFrame::W / 2;
    const float lon = clampf(lastG_.longitudinalG() / depthScaleG_, -1.0f, 1.0f);
    const int len = iround(lon * (VoxelFrame::W / 2 - 2));
    f.hline(0, cx, cx - len, 0);
    f.set(cx, 0, 0); f.set(cx, 1, 0);
  }
}

}  // namespace gm
