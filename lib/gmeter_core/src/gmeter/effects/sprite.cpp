#include "sprite.hpp"

#include <algorithm>
#include <cmath>

namespace gm {

SpriteEffect::SpriteEffect(const SpriteSheet& sheet, const SpriteMapping& m)
    : sheet_(sheet),
      pxGx_(m.pxPerG.x), pxGy_(m.pxPerG.y), pxGz_(m.pxPerG.z), hz_(m.smoothingHz),
      scrubAxis_(static_cast<float>(m.scrubAxis)), framesPerG_(m.framesPerG), idleFps_(m.idleFps),
      fpsPerG_(m.fpsPerG), echo_(m.depthEcho), inertia_(m.followInertia ? 1.0f : 0.0f) {
  params_.add("px_g_x", &pxGx_, 0.0f, 200.0f, "lateral px per g");
  params_.add("px_g_y", &pxGy_, 0.0f, 100.0f, "vertical px per g");
  params_.add("px_g_z", &pxGz_, 0.0f, 200.0f, "depth px per g");
  params_.add("hz", &hz_, 0.3f, 8.0f, "smoothing");
  params_.add("scrub_axis", &scrubAxis_, -1.0f, 2.0f, "-1 none, 0 lat, 1 vert, 2 lon");
  params_.add("frames_g", &framesPerG_, -64.0f, 64.0f, "frames scrubbed per g");
  params_.add("idle_fps", &idleFps_, 0.0f, 60.0f, "autonomous playback fps");
  params_.add("fps_g", &fpsPerG_, 0.0f, 120.0f, "extra fps per g");
  params_.add("echo", &echo_, 0.0f, 4.0f, "depth ghosting layers");
  params_.add("inertia", &inertia_, 0.0f, 1.0f, "1 = thrown like a loose object");
  reset();
}

void SpriteEffect::reset() {
  pos_.snap(Vec3{});
  playhead_ = 0.0f;
  frameIndex_ = 0;
}

void SpriteEffect::update(const EffectContext& ctx) {
  const Vec3 drive = inertia_ > 0.5f ? ctx.g.inertial() : ctx.g.dynamic();
  const Vec3 he = VoxelFrame::halfExtent();
  const float hw = sheet_.width * 0.5f, hh = sheet_.height * 0.5f;
  Vec3 target{drive.x * pxGx_, drive.y * pxGy_, drive.z * pxGz_};
  const float mx = std::max(0.0f, he.x - hw), my = std::max(0.0f, he.y - hh);
  target.x = clampf(target.x, -mx, mx);
  target.y = clampf(target.y, -my, my);
  target.z = clampf(target.z, -(he.z - kLayerPitchPx * 0.5f), he.z - kLayerPitchPx * 0.5f);
  pos_.step(target, ctx.dt, hz_);

  playhead_ += ctx.dt * (idleFps_ + fpsPerG_ * ctx.g.horizontalG());
  const int axis = iround(scrubAxis_);
  float scrub = 0.0f;
  if (axis == 0) scrub = drive.x * framesPerG_;
  else if (axis == 1) scrub = drive.y * framesPerG_;
  else if (axis == 2) scrub = drive.z * framesPerG_;
  if (sheet_.frames > 0) {
    const float total = playhead_ + scrub;
    int idx = ifloor(total) % sheet_.frames;
    if (idx < 0) idx += sheet_.frames;
    frameIndex_ = idx;
    if (playhead_ > 1.0e6f) playhead_ = std::fmod(playhead_, static_cast<float>(sheet_.frames));
  }
}

void SpriteEffect::render(VoxelFrame& f) {
  if (!sheet_.bits || sheet_.frames == 0) return;
  const Vec3 v = VoxelFrame::worldToVoxel(pos_.pos());
  const int x0 = iround(v.x - sheet_.width * 0.5f + 0.5f);
  const int y0 = iround(v.y - sheet_.height * 0.5f + 0.5f);
  const int layer = clampi(iround(v.z), 0, kD - 1);
  const int echo = iround(echo_);
  for (int dl = -echo; dl <= echo; ++dl) {
    const int z = layer + dl;
    if (z < 0 || z >= kD) continue;
    const float intensity = dl == 0 ? 1.0f : 0.5f * (1.0f - static_cast<float>(std::abs(dl) - 1) / static_cast<float>(echo + 1));
    for (int y = 0; y < sheet_.height; ++y)
      for (int x = 0; x < sheet_.width; ++x)
        if (sheet_.pixel(frameIndex_, x, y)) f.plot(x0 + x, y0 + y, z, intensity);
  }
}

// ---- demo sprite -----------------------------------------------------------------------
namespace {
constexpr int kDemoW = 16, kDemoH = 16, kDemoFrames = 12;
uint8_t gDemoBits[kDemoFrames * kDemoH * ((kDemoW + 7) / 8)];
bool gDemoBuilt = false;

void demoSet(int frame, int x, int y) {
  if (x < 0 || x >= kDemoW || y < 0 || y >= kDemoH) return;
  gDemoBits[frame * kDemoH * 2 + y * 2 + (x >> 3)] |= static_cast<uint8_t>(0x80u >> (x & 7));
}

void buildDemo() {
  for (auto& b : gDemoBits) b = 0;
  const float cx = 7.5f, cy = 7.5f;
  for (int fr = 0; fr < kDemoFrames; ++fr) {
    // Ring.
    for (int i = 0; i < 40; ++i) {
      const float a = kTwoPi * i / 40.0f;
      demoSet(fr, iround(cx + 7.0f * std::cos(a)), iround(cy + 7.0f * std::sin(a)));
    }
    // Spinning bar.
    const float a = kPi * static_cast<float>(fr) / static_cast<float>(kDemoFrames);
    for (int s = -5; s <= 5; ++s)
      demoSet(fr, iround(cx + s * std::cos(a)), iround(cy + s * std::sin(a)));
  }
  gDemoBuilt = true;
}
}  // namespace

const SpriteSheet& demoSprite() {
  static SpriteSheet sheet;
  if (!gDemoBuilt) {
    buildDemo();
    sheet.width = kDemoW;
    sheet.height = kDemoH;
    sheet.frames = kDemoFrames;
    sheet.bits = gDemoBits;
  }
  return sheet;
}

}  // namespace gm
