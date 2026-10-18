// "A.T. Field" shockwaves, after the concentric octagons in Neon Genesis
// Evangelion. Ripples start where the g-force pushes (the wall a loose object
// would hit) and spread as a spherical wavefront through the panel stack, so
// each panel shows an octagon of a different size. Sustained g keeps
// emitting; every 0.25 g crossed and every bump fires a stronger ripple.
#pragma once

#include "../effect.hpp"
#include "../gstate.hpp"

namespace gm {

class AtFieldEffect : public Effect {
 public:
  static constexpr int kMaxRipples = 12;

  AtFieldEffect();
  const char* name() const override { return "atfield"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  int activeRipples() const;

 private:
  struct Ripple {
    Vec3 origin;
    float age = 0.0f;
    float strength = 0.0f;
    bool active = false;
  };

  float speed_ = 70.0f;     // wavefront speed px/s
  float life_ = 1.6f;       // seconds a ripple lives
  float rateG_ = 2.5f;      // ripples per second per g above min_g
  float minG_ = 0.15f;      // horizontal g below which only the idle pulse runs
  float stepG_ = 0.25f;     // a strong ripple each time g crosses a multiple
  float idle_ = 1.0f;       // slow idle pulse from the centre
  float double_ = 1.0f;     // second, inner octagon
  float label_ = 1.0f;      // A.T.フィールド / 全開 captions
  float bumpG_ = 0.3f;

  Ripple ripples_[kMaxRipples];
  GStateTracker tracker_;
  float emitAccum_ = 0.0f;
  float idleTimer_ = 0.0f;
  int lastStep_ = 0;
  float horiz_ = 0.0f;
  float time_ = 0.0f;
  float captionAge_ = 99.0f;  // time since the last strong ripple

  void spawn(const Vec3& origin, float strength);
};

}  // namespace gm
