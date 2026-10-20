#include "hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../jp_text.hpp"

namespace gm {

namespace {
// Anti burn-in orbit: right, down, left, up around a 2 x 2 pixel square.
constexpr int kOrbitX[4] = {0, 1, 1, 0};
constexpr int kOrbitY[4] = {0, 0, 1, 1};

// Rectangle with 45-degree chamfered corners.
void chamferRect(VoxelFrame& f, int z, int x0, int y0, int x1, int y1, int c) {
  f.hline(z, x0 + c, x1 - c, y0);
  f.hline(z, x0 + c, x1 - c, y1);
  f.vline(z, x0, y0 + c, y1 - c);
  f.vline(z, x1, y0 + c, y1 - c);
  f.line(z, x0, y0 + c, x0 + c, y0);
  f.line(z, x1 - c, y0, x1, y0 + c);
  f.line(z, x1, y1 - c, x1 - c, y1);
  f.line(z, x0 + c, y1, x0, y1 - c);
}
}  // namespace

HudEffect::HudEffect() {
  params_.add("scale_g", &scaleG_, 0.2f, 3.0f, "lateral g at full travel");
  params_.add("vscale_g", &vscaleG_, 0.1f, 3.0f, "vertical g at full travel");
  params_.add("dscale_g", &dscaleG_, 0.2f, 3.0f, "longitudinal g at full depth");
  params_.add("hz", &hz_, 0.3f, 8.0f, "ball follow speed");
  params_.add("inertia", &inertia_, 0.0f, 1.0f, "1 = ball goes where a loose object would");
  params_.add("warn_g", &warnG_, 0.1f, 3.0f, "hazard stripes and 警告 above this g");
  params_.add("danger_g", &dangerG_, 0.2f, 4.0f, "title shows 危険 above this g");
  params_.add("ball_r", &ballR_, 1.5f, 8.0f, "ball radius px");
  params_.add("title_z", &titleZ_, 0.0f, static_cast<float>(kD - 1), "panel for the state kanji (0 = front)");
  params_.add("trail_s", &trailS_, 0.0f, 3.0f, "trail length seconds");
  params_.add("title", &title_, 0.0f, 1.0f, "big state kanji (on the panel set by title_z)");
  params_.add("axes", &axes_, 0.0f, 1.0f, "axis lines and rulers");
  params_.add("frame", &frame_, 0.0f, 1.0f, "border, readouts and status");
  params_.add("enter_g", &enterG_, 0.05f, 1.5f, "g that changes the state word");
  params_.add("hold_s", &holdS_, 0.0f, 3.0f, "minimum seconds per state word");
  params_.add("bump_g", &bumpG_, 0.05f, 2.0f, "vertical spike that shows 衝撃");
  params_.add("orbit_min", &orbitMin_, 0.0f, 30.0f, "minutes between 1-px shifts against burn-in (0 = off)");
  reset();
}

void HudEffect::reset() {
  tracker_.reset();
  ball_.snap(Vec3{});
  tip_ = Vec3{};
  time_ = 0.0f;
  titleAge_ = 0.0f;
  bumpAge_ = 99.0f;
  hazard_ = 0.0f;
  danger_ = false;
  trailHead_ = trailCount_ = 0;
  trailTimer_ = 0.0f;
  // The burn-in orbit keeps its place across re-activation.
}

int HudEffect::orbitX() const { return kOrbitX[orbitStep_ & 3]; }
int HudEffect::orbitY() const { return kOrbitY[orbitStep_ & 3]; }

Vec3 HudEffect::travel() const {
  const Vec3 he = VoxelFrame::halfExtent();
  return {(he.x - 10.0f) / scaleG_, (he.y - 8.0f) / vscaleG_, (he.z - kLayerPitchPx * 0.5f) / dscaleG_};
}

const char* HudEffect::titleText() const {
  if (bumpAge_ < 1.2f) return "衝撃";
  if (danger_) return "危険";
  switch (tracker_.state()) {
    case DriveState::Accelerate: return "加速";
    case DriveState::Brake: return "制動";
    case DriveState::Left: return "左旋回";
    case DriveState::Right: return "右旋回";
    default: return "待機";
  }
}

void HudEffect::update(const EffectContext& ctx) {
  g_ = ctx.g;
  // Blink clock: wrapped so it keeps full float precision after days on.
  time_ += ctx.dt;
  if (time_ >= 60.0f) time_ -= 60.0f;
  if (orbitMin_ > 0.0f) {
    orbitTimer_ += ctx.dt;
    if (orbitTimer_ >= orbitMin_ * 60.0f) {
      orbitTimer_ = 0.0f;
      orbitStep_ = (orbitStep_ + 1) & 3;
    }
  } else {
    orbitTimer_ = 0.0f;
    orbitStep_ = 0;
  }
  titleAge_ += ctx.dt;
  bumpAge_ += ctx.dt;
  tracker_.enterG = enterG_;
  tracker_.holdS = holdS_;
  tracker_.bumpG = bumpG_;
  bool bumped = false;
  if (tracker_.update(ctx.g, ctx.dt, &bumped)) titleAge_ = 0.0f;
  if (bumped) { bumpAge_ = 0.0f; titleAge_ = 0.0f; }
  const float horiz = ctx.g.horizontalG();
  const bool danger = horiz >= dangerG_;
  if (danger != danger_) { danger_ = danger; titleAge_ = 0.0f; }

  // Tip of the g-vector line, clamped inside the volume; the ball follows it.
  const Vec3 drive = inertia_ > 0.5f ? ctx.g.inertial() : ctx.g.dynamic();
  const Vec3 he = VoxelFrame::halfExtent();
  const Vec3 tr = travel();
  tip_ = Vec3{clampf(drive.x * tr.x, -(he.x - 6.0f), he.x - 6.0f), clampf(drive.y * tr.y, -(he.y - 5.0f), he.y - 5.0f),
              clampf(drive.z * tr.z, -(he.z - kLayerPitchPx * 0.5f), he.z - kLayerPitchPx * 0.5f)};
  ball_.step(tip_, ctx.dt, hz_);

  hazard_ += ctx.dt * (10.0f + 30.0f * horiz);
  if (hazard_ > 1000.0f) hazard_ -= 1000.0f;

  const float interval = std::max(0.02f, trailS_ / static_cast<float>(kTrail - 1));
  trailTimer_ += ctx.dt;
  if (trailS_ > 0.0f && trailTimer_ >= interval) {
    trailTimer_ -= interval;
    if (trailTimer_ > interval) trailTimer_ = 0.0f;
    trail_[trailHead_] = ball_.pos();
    trailHead_ = (trailHead_ + 1) % kTrail;
    if (trailCount_ < kTrail) ++trailCount_;
  }
}

void HudEffect::drawFrame(VoxelFrame& f, float horiz) const {
  const int W = kW, H = kH;
  // One pixel of margin on the right and bottom keeps the border on the glass
  // while the burn-in orbit moves everything right or down by a pixel.
  chamferRect(f, 0, 0, 0, W - 2, H - 2, 6);
  // Corner brackets just inside the frame.
  f.hline(0, 3, 9, 3);   f.vline(0, 3, 3, 7);
  f.hline(0, W - 10, W - 4, 3);   f.vline(0, W - 4, 3, 7);
  f.hline(0, 3, 9, H - 4);   f.vline(0, 3, H - 8, H - 4);
  f.hline(0, W - 10, W - 4, H - 4);   f.vline(0, W - 4, H - 8, H - 4);
  // Readouts: 横G (lateral) top-left, 縦G (longitudinal) top-right.
  char buf[12];
  const Vec3 d = g_.dynamic();
  std::snprintf(buf, sizeof(buf), "%+.2f", static_cast<double>(d.x));
  int x = 12 + drawJp(f, kJpSmall, 0, 12, 4, "横G");
  f.text(0, x + 3, 9, buf);
  std::snprintf(buf, sizeof(buf), "%+.2f", static_cast<double>(d.z));
  const int wv = measureJp(kJpSmall, "縦G") + 3 + 4 * 5;
  x = W - 12 - wv;
  x += drawJp(f, kJpSmall, 0, x, 4, "縦G");
  f.text(0, x + 3, 9, buf);
  // Status column (vertical writing) on the right: 正常, or 警告 blinking.
  // It ends on row H-9, clear of the bottom 7 rows where the engine puts its
  // DEMO / NO SENSOR tag.
  const bool warn = horiz >= warnG_;
  const int colX = W - 6 - kJpSmall.glyphs[0].width;
  const int colY = std::min(21, H - 9 - 2 * kJpSmall.height);  // 2 glyphs + 1 px gap
  if (!warn) drawJpVertical(f, kJpSmall, 0, colX, colY, "正常");
  else if (std::fmod(time_, 0.5f) < 0.3f) drawJpVertical(f, kJpSmall, 0, colX, colY, "警告");
  // Bottom band: hazard stripes when over warn_g.
  if (warn) {
    const int off = static_cast<int>(hazard_);
    for (int y = H - 9; y <= H - 6; ++y)
      for (int xx = 12; xx < W - 12; ++xx)
        if (((xx + y + off) / 3) % 2 == 0) f.set(xx, y, 0);
  }
}

void HudEffect::drawAxes(VoxelFrame& f) const {
  const int W = kW, H = kH, D = kD;
  const int cx = W / 2, cy = H / 2;
  const int axisZ = (D - 1) / 2;
  const Vec3 tr = travel();
  // Lateral and vertical axis lines on the middle panel, with ticks.
  f.hline(axisZ, 10, W - 11, cy);
  f.vline(axisZ, cx, 4, H - 5);
  for (int s = -1; s <= 1; s += 2)
    for (int k = 1; k <= 2; ++k) {
      const float gval = 0.5f * static_cast<float>(k);
      const int tx = cx + s * iround(tr.x * gval);
      const int len = k == 2 ? 3 : 1;
      if (tx > 8 && tx < W - 8) f.vline(axisZ, tx, cy - len, cy + len);
      const int ty = cy - s * iround(tr.y * gval);
      if (ty > 3 && ty < H - 4) f.hline(axisZ, cx - len, cx + len, ty);
    }
  // Depth axis: a dotted line through every panel, ticks at 0.5 g / 1 g.
  for (int z = 0; z < D; ++z) f.set(cx, cy, z);
  for (int k = -2; k <= 2; ++k) {
    if (k == 0) continue;
    const float zw = tr.z * 0.5f * static_cast<float>(k);
    const int layer = iround(VoxelFrame::worldToVoxel(Vec3{0, 0, zw}).z);
    if (layer >= 0 && layer < D && layer != axisZ) f.hline(layer, cx - (std::abs(k) == 2 ? 3 : 1), cx + (std::abs(k) == 2 ? 3 : 1), cy);
  }
  // Markers that ride the axes with the ball's components.
  const Vec3 b = VoxelFrame::worldToVoxel(ball_.pos());
  const int bx = iround(b.x), by = iround(b.y), bz = iround(b.z);
  f.vline(axisZ, bx, cy - 2, cy + 2);
  f.hline(axisZ, cx - 2, cx + 2, by);
  if (bz >= 0 && bz < D) f.circle(bz, cx, cy, 2);
  // Rulers on the front panel: lateral along the bottom, vertical on the left.
  if (frame_ > 0.5f) {
    const int ry = H - 5;
    for (int x = 12; x <= W - 13; x += 4) f.set(x, ry, 0);
    f.hline(0, bx - 2, bx + 2, ry + 1);  // caret
    f.set(bx, ry, 0);
    f.set(bx, ry - 1, 0);
    const int rx = 5;
    for (int y = 12; y <= H - 12; y += 4) f.set(rx, y, 0);
    f.vline(0, rx - 1, by - 2, by + 2);
    f.set(rx, by, 0);
    f.set(rx + 1, by, 0);
  }
}

void HudEffect::drawTitle(VoxelFrame& f) const {
  const char* word = titleText();
  // Idle fades after a few seconds so nothing static burns into the panels.
  const bool idle = word[0] && tracker_.state() == DriveState::Idle && bumpAge_ >= 1.2f && !danger_;
  if (idle && titleAge_ > 3.0f) return;
  const int z = clampi(iround(titleZ_), 0, kD - 1);
  const int w = measureJp(kJpLarge, word, 2);
  const int x = (kW - w) / 2, y = (kH - kJpLarge.height) / 2;
  // Reverse-video flash for the first moments (an NGE title-card cut).
  const bool flash = titleAge_ < 0.35f && std::fmod(titleAge_, 0.12f) < 0.08f;
  if (flash) {
    f.fillRect(z, x - 4, y - 3, x + w + 3, y + kJpLarge.height + 2);
    drawJp(f, kJpLarge, z, x, y, word, 2, -1.0f);
  } else {
    drawJp(f, kJpLarge, z, x, y, word, 2);
    // Underline bar, NGE caption style.
    f.hline(z, x, x + w - 1, y + kJpLarge.height + 2);
  }
}

void HudEffect::render(VoxelFrame& f) {
  const float horiz = g_.horizontalG();
  if (title_ > 0.5f) drawTitle(f);
  if (axes_ > 0.5f) drawAxes(f);

  // The g-vector line from the centre to its tip, through the stack.
  f.line3(Vec3{}, tip_);
  // Trail of where the ball has been, fading.
  if (trailS_ > 0.0f) {
    for (int i = 0; i < trailCount_; ++i) {
      const int idx = (trailHead_ - 1 - i + kTrail) % kTrail;
      const float fade = 1.0f - static_cast<float>(i) / static_cast<float>(kTrail);
      f.point(trail_[idx], 0.7f * fade);
    }
  }
  f.sphere(ball_.pos(), ballR_, 0.8f * kLayerPitchPx);
  // Target-lock bracket on the FRONT panel, tracking the ball behind it. Rear
  // panels are dim through the glass, so the brightest layer carries the
  // lock; the bracket shrinks as the ball recedes (a perspective cue).
  {
    const Vec3 b = VoxelFrame::worldToVoxel(ball_.pos());
    const float depth = clampf(b.z / static_cast<float>(kD - 1), 0.0f, 1.0f);
    const int bx = iround(b.x), by = iround(b.y);
    const int r = iround(ballR_ + 2.0f + 5.0f * (1.0f - depth));
    const int arm = std::max(2, r / 2);
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2) {
        f.hline(0, bx + sx * r, bx + sx * (r - arm), by + sy * r);
        f.vline(0, bx + sx * r, by + sy * r, by + sy * (r - arm));
      }
  }

  if (frame_ > 0.5f) {
    drawFrame(f, horiz);
    if (bumpAge_ < 0.3f) chamferRect(f, 1, 2, 2, kW - 3, kH - 3, 6);  // impact flash
  }
}

}  // namespace gm
