#include "atfield.hpp"

#include <algorithm>
#include <cmath>

#include "../jp_text.hpp"

namespace gm {

namespace {
// Line with dithered intensity (VoxelFrame::line is always full on).
void dimLine(VoxelFrame& f, int z, int x0, int y0, int x1, int y1, float intensity) {
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int steps = std::max(dx, -dy) + 1;
  for (int i = 0; i <= steps; ++i) {
    f.plot(x0, y0, z, intensity);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
// Regular octagon with flat top/bottom (vertices at 22.5 + k * 45 degrees).
void octagon(VoxelFrame& f, int z, float cx, float cy, float r, float intensity) {
  if (r < 1.0f) return;
  int px[8], py[8];
  for (int k = 0; k < 8; ++k) {
    const float a = kPi / 8.0f + static_cast<float>(k) * kPi / 4.0f;
    px[k] = iround(cx + r * std::cos(a));
    py[k] = iround(cy + r * std::sin(a));
  }
  for (int k = 0; k < 8; ++k) dimLine(f, z, px[k], py[k], px[(k + 1) % 8], py[(k + 1) % 8], intensity);
}
}  // namespace

AtFieldEffect::AtFieldEffect() {
  params_.add("speed", &speed_, 10.0f, 300.0f, "wavefront speed px/s");
  params_.add("life", &life_, 0.3f, 5.0f, "seconds a ripple lives");
  params_.add("rate_g", &rateG_, 0.0f, 20.0f, "ripples per second per g");
  params_.add("min_g", &minG_, 0.0f, 1.0f, "g below which only the idle pulse runs");
  params_.add("step_g", &stepG_, 0.05f, 1.0f, "strong ripple each time g crosses a multiple");
  params_.add("idle", &idle_, 0.0f, 1.0f, "slow pulse from the centre when calm");
  params_.add("double", &double_, 0.0f, 1.0f, "inner second octagon");
  params_.add("label", &label_, 0.0f, 1.0f, "A.T.フィールド / 全開 captions");
  params_.add("bump_g", &bumpG_, 0.05f, 2.0f, "vertical spike that fires a ripple");
  reset();
}

void AtFieldEffect::reset() {
  for (Ripple& r : ripples_) r = Ripple{};
  tracker_.reset();
  emitAccum_ = 0.0f;
  idleTimer_ = 0.0f;
  lastStep_ = 0;
  horiz_ = 0.0f;
  time_ = 0.0f;
  captionAge_ = 99.0f;
}

int AtFieldEffect::activeRipples() const {
  int n = 0;
  for (const Ripple& r : ripples_) n += r.active ? 1 : 0;
  return n;
}

void AtFieldEffect::spawn(const Vec3& origin, float strength) {
  Ripple* slot = &ripples_[0];
  for (Ripple& r : ripples_) {
    if (!r.active) { slot = &r; break; }
    if (r.age > slot->age) slot = &r;  // reuse the oldest
  }
  slot->origin = origin;
  slot->age = 0.0f;
  slot->strength = clampf(strength, 0.1f, 1.0f);
  slot->active = true;
  if (strength >= 0.6f) captionAge_ = 0.0f;
}

void AtFieldEffect::update(const EffectContext& ctx) {
  time_ += ctx.dt;
  if (time_ >= 60.0f) time_ -= 60.0f;  // blink clock only; keeps float precision after days on
  captionAge_ += ctx.dt;
  for (Ripple& r : ripples_)
    if (r.active && (r.age += ctx.dt) > life_) r.active = false;

  tracker_.bumpG = bumpG_;
  bool bumped = false;
  tracker_.update(ctx.g, ctx.dt, &bumped);
  const Vec3 he = VoxelFrame::halfExtent();
  if (bumped) spawn(Vec3{0.0f, -he.y, 0.0f}, 1.0f);  // from the floor

  // Impact point: where the inertial force pushes, on the volume's boundary.
  const Vec3 in = ctx.g.inertial();
  horiz_ = ctx.g.horizontalG();
  const float mag = in.length();
  Vec3 origin;
  if (mag > 1e-3f) {
    const Vec3 dir = in / mag;
    float t = 1e9f;
    if (std::fabs(dir.x) > 1e-4f) t = std::min(t, he.x / std::fabs(dir.x));
    if (std::fabs(dir.y) > 1e-4f) t = std::min(t, he.y / std::fabs(dir.y));
    if (std::fabs(dir.z) > 1e-4f) t = std::min(t, he.z / std::fabs(dir.z));
    origin = dir * (t * 0.75f);  // inside the wall, so the octagons stay on the panels
  }

  // Each multiple of step_g crossed upward fires a strong ripple.
  const int step = static_cast<int>(horiz_ / stepG_);
  if (step > lastStep_) spawn(origin, 0.6f + 0.2f * static_cast<float>(step));
  lastStep_ = step;

  // Sustained g keeps emitting, faster for stronger g.
  if (horiz_ > minG_) {
    emitAccum_ += ctx.dt * rateG_ * (horiz_ - minG_);
    if (emitAccum_ >= 1.0f) { emitAccum_ -= 1.0f; spawn(origin, 0.3f + horiz_); }
    idleTimer_ = 0.0f;
  } else {
    emitAccum_ = 0.0f;
    idleTimer_ += ctx.dt;
    if (idle_ > 0.5f && idleTimer_ > 2.8f) {
      idleTimer_ = 0.0f;
      spawn(Vec3{0.0f, 0.0f, he.z * 0.6f}, 0.45f);  // calm heartbeat from deep in the stack
    }
  }
}

void AtFieldEffect::render(VoxelFrame& f) {
  for (const Ripple& r : ripples_) {
    if (!r.active) continue;
    const float R = speed_ * r.age;
    const float fade = r.strength * (1.0f - r.age / life_);
    const Vec3 c = VoxelFrame::worldToVoxel(r.origin);
    for (int k = 0; k < kD; ++k) {
      const float dz = VoxelFrame::layerZ(k) - r.origin.z;
      if (std::fabs(dz) >= R) continue;
      const float rl = std::sqrt(R * R - dz * dz);
      octagon(f, k, c.x, c.y, rl, fade);
      if (double_ > 0.5f && rl > 6.0f) octagon(f, k, c.x, c.y, rl - 3.0f, fade * 0.6f);
    }
  }
  if (label_ > 0.5f && captionAge_ < 1.5f) {
    drawJp(f, kJpSmall, 0, 4, kH - kJpSmall.height - 2, "A.T.フィールド");
    if (horiz_ > 0.8f && std::fmod(time_, 0.4f) < 0.25f) {
      const int w = measureJp(kJpSmall, "全開");
      drawJp(f, kJpSmall, 0, kW - 4 - w, 2, "全開");
    }
  }
}

}  // namespace gm
