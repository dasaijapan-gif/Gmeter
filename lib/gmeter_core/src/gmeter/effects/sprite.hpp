// G-mapped authored animation. Takes any 1-bit sprite sheet (export one from
// a PNG/GIF with tools/img2sprite.py) and lets the g-force drive it: position
// in the volume (including which layer it sits on), which frame is shown
// (scrubbing by g on a chosen axis) and how fast it plays. This is the
// generic hook for "make my animation react to g".
#pragma once

#include "../effect.hpp"

namespace gm {

struct SpriteSheet {
  int width = 0, height = 0, frames = 0;
  const uint8_t* bits = nullptr;  // frame-major, row-major, 1 bpp, MSB first, rows byte-padded

  int rowBytes() const { return (width + 7) / 8; }
  int frameBytes() const { return rowBytes() * height; }
  bool pixel(int frame, int x, int y) const {
    if (!bits || frame < 0 || frame >= frames || x < 0 || x >= width || y < 0 || y >= height) return false;
    return (bits[frame * frameBytes() + y * rowBytes() + (x >> 3)] >> (7 - (x & 7))) & 1u;
  }
};

struct SpriteMapping {
  Vec3 pxPerG{60.0f, 24.0f, 60.0f};  // world displacement per g on each axis
  float smoothingHz = 2.0f;
  int scrubAxis = 2;        // -1 none, 0 lateral, 1 vertical, 2 longitudinal
  float framesPerG = 8.0f;  // frames of scrub per g on that axis
  float idleFps = 0.0f;     // autonomous playback
  float fpsPerG = 12.0f;    // extra playback speed per g (horizontal magnitude)
  float depthEcho = 1.0f;   // ghost copies on neighbouring layers (layers of falloff)
  bool followInertia = true;
};

class SpriteEffect : public Effect {
 public:
  SpriteEffect(const SpriteSheet& sheet, const SpriteMapping& map = SpriteMapping{});
  const char* name() const override { return "sprite"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  void setSheet(const SpriteSheet& s) { sheet_ = s; }
  int currentFrame() const { return frameIndex_; }
  Vec3 position() const { return pos_.pos(); }

 private:
  SpriteSheet sheet_;
  float pxGx_, pxGy_, pxGz_, hz_, scrubAxis_, framesPerG_, idleFps_, fpsPerG_, echo_, inertia_;
  Spring3 pos_;
  float playhead_ = 0.0f;
  int frameIndex_ = 0;
};

// A built-in 16x16, 12-frame demo sprite (spinning bar inside a ring) so the
// pipeline can be exercised before any artwork exists.
const SpriteSheet& demoSprite();

}  // namespace gm
