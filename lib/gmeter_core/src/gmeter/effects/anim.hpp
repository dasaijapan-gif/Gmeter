// Authored animation, interpreted by g-force.
//
// An AnimationAsset is pixel artwork: `frames` frames of `width` x `height`
// pixels, each frame made of `depth` slices that sit on consecutive panels
// (slice 0 nearest the driver), so artwork can itself be volumetric.
// Clips map the car's state to frame ranges:
//
//   idle        nothing much happening
//   accelerate  longitudinal g forward
//   brake       longitudinal g backward
//   left/right  cornering (a right turn is lateral g to the right)
//   bump        a vertical spike; plays once, then the effect returns to
//               whichever state the g-force selects
//
// A clip with count 0 falls back: left/right borrow the other side mirrored
// (when `mirror` is on), everything else falls back to idle, and an empty idle
// clip plays all frames. On top of the clip choice the effect moves the
// artwork through the volume like a loose object (spring-smoothed), can speed
// playback up with g, scrub frames with g, and ghost a flat sprite onto
// neighbouring layers.
//
// Assets are plain aggregates so Gmeter Studio (host/web) can export them as
// a C++ header; see src/user_animation.hpp.
#pragma once

#include "../effect.hpp"

namespace gm {

enum AnimState : uint8_t {
  kAnimIdle = 0,
  kAnimAccelerate,
  kAnimBrake,
  kAnimLeft,
  kAnimRight,
  kAnimBump,
  kAnimStateCount
};
const char* animStateName(int state);

struct AnimClip {
  uint16_t start = 0;  // first frame of the clip
  uint16_t count = 0;  // frames in the clip; 0 = fall back (see above)
  float fps = 12.0f;   // playback rate
  uint8_t loop = 1;    // 0 = play once and hold the last frame
};

// Default values for the effect's tunables. Field order is part of the
// exported header format: append new fields at the end only.
struct AnimMapping {
  float pxPerGx = 50.0f;    // lateral travel, pixels per g
  float pxPerGy = 16.0f;    // vertical travel, pixels per g
  float pxPerGz = 60.0f;    // depth travel, pixels per g (layer pitch is GM_LAYER_PITCH_PX)
  float springHz = 2.0f;    // motion smoothing
  float inertia = 1.0f;     // 1: thrown like a loose object; 0: follows the acceleration arrow
  float enterG = 0.25f;     // horizontal g that leaves idle
  float holdS = 0.30f;      // minimum time in a state before switching
  float bumpG = 0.30f;      // vertical g spike that fires the bump clip
  float fpsPerG = 0.0f;     // extra playback speed per g (1.0 doubles speed at 1 g)
  float scrubAxis = -1.0f;  // -1 off, 0 lateral, 1 vertical, 2 longitudinal
  float framesPerG = 0.0f;  // frames offset per g on the scrub axis
  float echo = 0.0f;        // ghost layers either side of a single-slice sprite
  float mirror = 1.0f;      // mirror left/right clips when one side is empty
};

struct AnimationAsset {
  const char* name = "";
  uint16_t width = 0, height = 0, depth = 1, frames = 0;
  // [frame][slice][row][byte]: 1 bit per pixel, MSB = leftmost, rows byte-padded.
  const uint8_t* bits = nullptr;
  AnimClip clips[kAnimStateCount] = {};
  AnimMapping mapping = {};

  int rowBytes() const { return (width + 7) / 8; }
  int sliceBytes() const { return rowBytes() * height; }
  int frameBytes() const { return sliceBytes() * depth; }
  int totalBytes() const { return frameBytes() * frames; }
  bool valid() const { return bits && width > 0 && height > 0 && depth > 0 && frames > 0; }
  bool pixel(int frame, int slice, int x, int y) const {
    if (!bits || frame < 0 || frame >= frames || slice < 0 || slice >= depth || x < 0 || x >= width || y < 0 || y >= height)
      return false;
    const int i = frame * frameBytes() + slice * sliceBytes() + y * rowBytes() + (x >> 3);
    return (bits[i] >> (7 - (x & 7))) & 1u;
  }
};

class AnimationEffect : public Effect {
 public:
  explicit AnimationEffect(const AnimationAsset& asset);
  const char* name() const override { return "anim"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  // Swap artwork at runtime (e.g. from Gmeter Studio). When applyMapping is
  // true the asset's mapping overwrites the current tunables; resetMotion
  // false keeps the current position, state and clip time (live editing).
  void setAsset(const AnimationAsset& asset, bool applyMapping = true, bool resetMotion = true);
  const AnimationAsset& asset() const { return asset_; }

  // -1 = choose the state from g-force (normal); 0..5 = hold that state (preview).
  void forceState(int state);
  int state() const { return state_; }
  int currentFrame() const { return frame_; }
  bool mirrored() const { return mirror_; }
  Vec3 position() const { return pos_.pos(); }

  // Resolved clip for a state after fallbacks; `mirror` set when the clip is
  // the other side's frames flipped horizontally.
  AnimClip resolveClip(int state, bool& mirror) const;

 private:
  AnimationAsset asset_;
  AnimMapping m_;
  Spring3 pos_;
  int state_ = kAnimIdle;
  int forced_ = -1;
  float stateTime_ = 0.0f;
  float clipTime_ = 0.0f;
  int frame_ = 0;
  bool mirror_ = false;
  float prevVert_ = 0.0f;

  void enter(int state);
  int chooseState(const Vec3& dyn) const;
};

// Built-in 32x24, 3-slice demo (breathing ring, chevrons per direction,
// starburst on bumps) so the effect works before any artwork exists.
const AnimationAsset& demoAnimation();

}  // namespace gm
