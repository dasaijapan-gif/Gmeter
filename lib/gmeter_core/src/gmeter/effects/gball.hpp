// Classic g-meter ball, in 3D: the ball slides around the volume like a loose
// marble on the dashboard (lateral g -> left/right, longitudinal g -> toward or
// away from the viewer through the layer stack, vertical g -> up/down), with
// critically damped motion and a fading 3D trail. The front layer carries a
// HUD: reticle, 0.5 g / 1 g rings and a numeric readout.
#pragma once

#include "../effect.hpp"

namespace gm {

class GBallEffect : public Effect {
 public:
  GBallEffect();
  const char* name() const override { return "ball"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  Vec3 position() const { return spring_.pos(); }

 private:
  static constexpr int kTrailMax = 48;

  // tunables
  float fullScaleG_ = 1.0f;        // g that moves the ball to the lateral edge
  float verticalScaleG_ = 0.6f;    // g that moves the ball to the top/bottom edge
  float depthScaleG_ = 1.0f;       // g that moves the ball to the front/back layer
  float naturalHz_ = 1.8f;         // spring natural frequency
  float damping_ = 1.0f;           // 1 = critically damped
  float radius_ = 5.0f;            // px
  float depthRadiusLayers_ = 1.3f; // ellipsoid half-depth in layers
  float trailSeconds_ = 1.2f;      // how long the trail persists
  float trailIntensity_ = 0.55f;
  float followInertia_ = 1.0f;     // 1: ball moves like a loose object; 0: like an acceleration arrow
  float hud_ = 1.0f;
  float rings_ = 1.0f;

  Spring3 spring_;
  Vec3 trail_[kTrailMax];
  float trailAge_[kTrailMax];
  int trailHead_ = 0, trailCount_ = 0;
  float trailTimer_ = 0.0f;
  float peakG_ = 0.0f, peakAge_ = 0.0f;
  GForce lastG_;

  Vec3 travel() const;
};

}  // namespace gm
