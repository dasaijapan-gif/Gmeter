#include "anim.hpp"

#include <algorithm>

#include <cmath>
#include <cstring>

namespace gm {

const char* animStateName(int state) {
  static const char* kNames[kAnimStateCount] = {"idle", "accelerate", "brake", "left", "right", "bump"};
  return state >= 0 && state < kAnimStateCount ? kNames[state] : "?";
}

AnimationEffect::AnimationEffect(const AnimationAsset& asset) : asset_(asset), m_(asset.mapping) {
  params_.add("px_g_x", &m_.pxPerGx, 0.0f, 200.0f, "lateral travel px per g");
  params_.add("px_g_y", &m_.pxPerGy, 0.0f, 100.0f, "vertical travel px per g");
  params_.add("px_g_z", &m_.pxPerGz, 0.0f, 200.0f, "depth travel px per g");
  params_.add("hz", &m_.springHz, 0.3f, 8.0f, "motion smoothing");
  params_.add("inertia", &m_.inertia, 0.0f, 1.0f, "1 = thrown like a loose object");
  params_.add("enter_g", &m_.enterG, 0.02f, 1.5f, "horizontal g that leaves idle");
  params_.add("hold_s", &m_.holdS, 0.0f, 3.0f, "minimum seconds in a state");
  params_.add("bump_g", &m_.bumpG, 0.05f, 2.0f, "vertical g that fires the bump clip");
  params_.add("fps_g", &m_.fpsPerG, 0.0f, 8.0f, "playback speed-up per g");
  params_.add("scrub_axis", &m_.scrubAxis, -1.0f, 2.0f, "-1 off, 0 lat, 1 vert, 2 lon");
  params_.add("frames_g", &m_.framesPerG, -64.0f, 64.0f, "frames scrubbed per g");
  params_.add("echo", &m_.echo, 0.0f, 4.0f, "ghost layers (single-slice art)");
  params_.add("mirror", &m_.mirror, 0.0f, 1.0f, "mirror an empty left/right clip");
  reset();
}

void AnimationEffect::setAsset(const AnimationAsset& asset, bool applyMapping, bool resetMotion) {
  asset_ = asset;
  if (applyMapping) m_ = asset.mapping;
  if (resetMotion) reset();
  else frame_ = asset_.frames > 0 ? clampi(frame_, 0, asset_.frames - 1) : 0;
}

void AnimationEffect::reset() {
  pos_.snap(Vec3{});
  state_ = kAnimIdle;
  stateTime_ = 0.0f;
  clipTime_ = 0.0f;
  frame_ = 0;
  mirror_ = false;
  prevVert_ = 0.0f;
}

void AnimationEffect::forceState(int state) {
  forced_ = (state >= 0 && state < kAnimStateCount) ? state : -1;
  if (forced_ >= 0) enter(forced_);
}

AnimClip AnimationEffect::resolveClip(int state, bool& mirror) const {
  mirror = false;
  if (state < 0 || state >= kAnimStateCount) state = kAnimIdle;
  const uint16_t frames = asset_.frames;
  auto clampClip = [&](AnimClip c) {
    if (c.start + c.count > frames) c.count = static_cast<uint16_t>(frames - c.start);
    if (c.fps <= 0.0f) c.fps = 1.0f;
    return c;
  };
  const AnimClip& own = asset_.clips[state];
  if (usable(own)) return clampClip(own);
  if ((state == kAnimLeft || state == kAnimRight) && m_.mirror > 0.5f) {
    const AnimClip& other = asset_.clips[state == kAnimLeft ? kAnimRight : kAnimLeft];
    if (usable(other)) { mirror = true; return clampClip(other); }
  }
  const AnimClip& idle = asset_.clips[kAnimIdle];
  if (usable(idle)) return clampClip(idle);
  AnimClip all;
  all.start = 0;
  all.count = frames;
  all.fps = idle.fps > 0.0f ? idle.fps : 12.0f;
  all.loop = 1;
  return all;
}

bool AnimationEffect::usable(const AnimClip& c) const { return c.count > 0 && c.start < asset_.frames; }

void AnimationEffect::enter(int state) {
  if (state == state_ && stateTime_ > 0.0f) return;
  // A state that resolves to the frames already playing (for example accelerate
  // with no clip of its own, falling back to idle) carries on from the same
  // frame instead of restarting. The bump clip always starts from its first frame.
  bool m0 = false, m1 = false;
  const AnimClip from = resolveClip(state_, m0), to = resolveClip(state, m1);
  const bool sameFrames = state != kAnimBump && state_ != kAnimBump && from.start == to.start &&
                          from.count == to.count && from.loop == to.loop && m0 == m1;
  state_ = state;
  stateTime_ = 0.0f;
  clipTime_ = sameFrames ? clipTime_ * from.fps / to.fps : 0.0f;  // resolveClip keeps fps > 0
}

int AnimationEffect::chooseState(const Vec3& dyn) const {
  const float lat = dyn.x, lon = dyn.z;
  // How strongly the g-force supports a given state.
  auto support = [&](int s) {
    switch (s) {
      case kAnimAccelerate: return lon;
      case kAnimBrake: return -lon;
      case kAnimRight: return lat;
      case kAnimLeft: return -lat;
      default: return 0.0f;
    }
  };
  int candidate = kAnimIdle;
  float best = 0.0f;
  for (int s = kAnimAccelerate; s <= kAnimRight; ++s) {
    if (support(s) > best) { best = support(s); candidate = s; }
  }
  const bool active = state_ >= kAnimAccelerate && state_ <= kAnimRight;
  // Hysteresis: stay in an active state until its support drops below 60 %
  // of the entry threshold, and only hand over to another active state when
  // that one is clearly stronger.
  if (active) {
    const float mine = support(state_);
    if (mine >= 0.6f * m_.enterG) {
      // Hand over only to a state that is clearly stronger and could itself be
      // entered; otherwise keep the current one (never drop to idle while the
      // current state is still above its exit level).
      const bool handOver = candidate != state_ && best >= mine + 0.1f && best >= m_.enterG;
      return handOver ? candidate : state_;
    }
  }
  return best >= m_.enterG ? candidate : kAnimIdle;
}

void AnimationEffect::update(const EffectContext& ctx) {
  if (!asset_.valid()) return;
  const Vec3 dyn = ctx.g.dynamic();
  stateTime_ += ctx.dt;

  // ---- state machine -------------------------------------------------------------
  const float vert = dyn.y;
  const bool spike = std::fabs(vert) >= m_.bumpG && std::fabs(prevVert_) < m_.bumpG;
  prevVert_ = vert;
  bool clipMirror = false;
  if (forced_ >= 0) {
    if (state_ != forced_) enter(forced_);
  } else if (state_ == kAnimBump) {
    const AnimClip c = resolveClip(kAnimBump, clipMirror);
    const float len = static_cast<float>(c.count) / c.fps;
    if (!usable(asset_.clips[kAnimBump]) || clipTime_ >= len) enter(chooseState(dyn));
  } else {
    const bool hasBump = usable(asset_.clips[kAnimBump]);
    if (spike && hasBump) {
      enter(kAnimBump);
    } else if (stateTime_ >= m_.holdS) {
      const int next = chooseState(dyn);
      if (next != state_) enter(next);
    }
  }

  // ---- playback -------------------------------------------------------------------
  const AnimClip clip = resolveClip(state_, clipMirror);
  mirror_ = clipMirror;
  const float speed = 1.0f + m_.fpsPerG * std::sqrt(dyn.x * dyn.x + dyn.z * dyn.z);
  clipTime_ += ctx.dt * speed;
  int idx = static_cast<int>(clipTime_ * clip.fps);
  const bool loop = clip.loop != 0 && state_ != kAnimBump;
  const int axis = iround(m_.scrubAxis);
  if (axis >= 0 && axis <= 2 && m_.framesPerG != 0.0f) {
    const float v = axis == 0 ? dyn.x : (axis == 1 ? dyn.y : dyn.z);
    idx += iround(v * m_.framesPerG);
  }
  const int count = clip.count > 0 ? clip.count : 1;
  if (loop) {
    idx %= count;
    if (idx < 0) idx += count;
  } else {
    idx = clampi(idx, 0, count - 1);
  }
  frame_ = clip.start + idx;
  // Keep the clip timer bounded on long loops.
  if (loop && clipTime_ * clip.fps > 1.0e5f) clipTime_ = std::fmod(clipTime_, static_cast<float>(count) / clip.fps);

  // ---- motion -----------------------------------------------------------------------
  const Vec3 drive = m_.inertia > 0.5f ? ctx.g.inertial() : dyn;
  const Vec3 he = VoxelFrame::halfExtent();
  const float hw = std::max(0.0f, he.x - asset_.width * 0.5f);
  const float hh = std::max(0.0f, he.y - asset_.height * 0.5f);
  const float slicesHalf = (asset_.depth - 1) * 0.5f * kLayerPitchPx;
  const float hz = std::max(0.0f, he.z - kLayerPitchPx * 0.5f - slicesHalf);
  Vec3 target{drive.x * m_.pxPerGx, drive.y * m_.pxPerGy, drive.z * m_.pxPerGz};
  target.x = clampf(target.x, -hw, hw);
  target.y = clampf(target.y, -hh, hh);
  target.z = clampf(target.z, -hz, hz);
  pos_.step(target, ctx.dt, m_.springHz);
}

void AnimationEffect::render(VoxelFrame& f) {
  if (!asset_.valid()) return;
  const Vec3 v = VoxelFrame::worldToVoxel(pos_.pos());
  const int x0 = iround(v.x - asset_.width * 0.5f + 0.5f);
  const int y0 = iround(v.y - asset_.height * 0.5f + 0.5f);
  const int depth = asset_.depth;
  // Slice 0 is the front-most; centre the stack of slices on the position.
  // Art with more slices than panels stays centred; out-of-range layers are skipped.
  const int first = clampi(iround(v.z - (depth - 1) * 0.5f), std::min(0, kD - depth), std::max(0, kD - depth));
  const int echo = depth == 1 ? iround(m_.echo) : 0;
  const int w = asset_.width;
  for (int s = 0; s < depth; ++s) {
    const int layer = first + s;
    for (int dl = -echo; dl <= echo; ++dl) {
      const int z = layer + dl;
      if (z < 0 || z >= kD) continue;
      const float intensity =
          dl == 0 ? 1.0f : 0.5f * (1.0f - static_cast<float>(std::abs(dl) - 1) / static_cast<float>(echo + 1));
      for (int y = 0; y < asset_.height; ++y)
        for (int x = 0; x < w; ++x)
          if (asset_.pixel(frame_, s, mirror_ ? w - 1 - x : x, y)) f.plot(x0 + x, y0 + y, z, intensity);
    }
  }
}

// ---- demo artwork ---------------------------------------------------------------------------
namespace {
constexpr int kDW = 32, kDH = 24, kDD = 3, kDF = 32;
constexpr int kDRow = (kDW + 7) / 8;
uint8_t gDemo[kDF * kDD * kDH * kDRow];

void dset(int f, int s, int x, int y) {
  if (x < 0 || x >= kDW || y < 0 || y >= kDH) return;
  gDemo[((f * kDD + s) * kDH + y) * kDRow + (x >> 3)] |= static_cast<uint8_t>(0x80u >> (x & 7));
}
void dline(int f, int s, int x0, int y0, int x1, int y1) {
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 128; ++guard) {
    dset(f, s, x0, y0);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void dring(int f, int s, float cx, float cy, float r) {
  const int n = 48;
  for (int i = 0; i < n; ++i) {
    const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
    dset(f, s, iround(cx + r * std::cos(a)), iround(cy + r * std::sin(a)));
  }
}
// Chevron pointing in direction (dx, dy) (unit axis), tip at (tx, ty).
void dchevron(int f, int s, int tx, int ty, int dx, int dy, int size) {
  // Perpendicular axis.
  const int px = -dy, py = dx;
  for (int t = 0; t < 2; ++t) {  // two pixels thick
    const int bx = tx - dx * t, by = ty - dy * t;
    dline(f, s, bx, by, bx - dx * size + px * size, by - dy * size + py * size);
    dline(f, s, bx, by, bx - dx * size - px * size, by - dy * size - py * size);
  }
}

void buildDemo() {
  std::memset(gDemo, 0, sizeof(gDemo));
  const float cx = (kDW - 1) * 0.5f, cy = (kDH - 1) * 0.5f;
  // Idle 0..7: a breathing shell (smaller rings on the outer slices).
  for (int f = 0; f < 8; ++f) {
    const float r = 7.0f + 2.0f * std::sin(kTwoPi * static_cast<float>(f) / 8.0f);
    dring(f, 1, cx, cy, r);
    dring(f, 0, cx, cy, r * 0.6f);
    dring(f, 2, cx, cy, r * 0.6f);
    dset(f, 1, iround(cx), iround(cy));
  }
  // Accelerate 8..13: chevrons pointing up, marching up; back slices lag.
  for (int i = 0; i < 6; ++i)
    for (int s = 0; s < kDD; ++s)
      for (int k = 0; k < 3; ++k) {
        const int y = ((kDH + 6 - (i * 2 + k * 8 + s * 3)) % (kDH + 6)) - 3;
        dchevron(8 + i, s, iround(cx), y, 0, -1, 6 - s);
      }
  // Brake 14..19: chevrons pointing down, marching down, plus a bar on the front slice.
  for (int i = 0; i < 6; ++i) {
    for (int s = 0; s < kDD; ++s)
      for (int k = 0; k < 3; ++k) {
        const int y = ((i * 2 + k * 8 + s * 3) % (kDH + 6)) - 3;
        dchevron(14 + i, s, iround(cx), y, 0, 1, 6 - s);
      }
    if (i % 2 == 0) dline(14 + i, 0, 4, kDH - 2, kDW - 5, kDH - 2);
  }
  // Right 20..25: chevrons pointing right, marching right (left is mirrored).
  for (int i = 0; i < 6; ++i)
    for (int s = 0; s < kDD; ++s)
      for (int k = 0; k < 3; ++k) {
        const int x = ((i * 2 + k * 11 + s * 3) % (kDW + 6)) - 3;
        dchevron(20 + i, s, x, iround(cy), 1, 0, 6 - s);
      }
  // Bump 26..31: starburst growing outward, front slice leads.
  for (int i = 0; i < 6; ++i)
    for (int s = 0; s < kDD; ++s) {
      const float r0 = 2.0f + static_cast<float>(i) * 1.6f - static_cast<float>(s);
      const float r1 = r0 + 4.0f;
      if (r1 <= 0.0f) continue;
      for (int ray = 0; ray < 8; ++ray) {
        const float a = kTwoPi * static_cast<float>(ray) / 8.0f + 0.2f * static_cast<float>(s);
        dline(26 + i, s, iround(cx + std::max(0.0f, r0) * std::cos(a)), iround(cy + std::max(0.0f, r0) * std::sin(a)),
              iround(cx + r1 * std::cos(a)), iround(cy + r1 * std::sin(a)));
      }
    }
}
}  // namespace

const AnimationAsset& demoAnimation() {
  static AnimationAsset asset;
  static bool built = false;
  if (!built) {
    buildDemo();
    asset.name = "demo";
    asset.width = kDW;
    asset.height = kDH;
    asset.depth = kDD;
    asset.frames = kDF;
    asset.bits = gDemo;
    asset.clips[kAnimIdle] = AnimClip{0, 8, 8.0f, 1};
    asset.clips[kAnimAccelerate] = AnimClip{8, 6, 12.0f, 1};
    asset.clips[kAnimBrake] = AnimClip{14, 6, 12.0f, 1};
    asset.clips[kAnimLeft] = AnimClip{0, 0, 12.0f, 1};  // mirrored from right
    asset.clips[kAnimRight] = AnimClip{20, 6, 12.0f, 1};
    asset.clips[kAnimBump] = AnimClip{26, 6, 18.0f, 0};
    built = true;
  }
  return asset;
}

}  // namespace gm
