// Tactical HUD inspired by the NERV screens in Neon Genesis Evangelion.
//
//   front panel   chamfered frame, corner brackets, 横G / 縦G readouts, a
//                 lateral ruler along the bottom and a vertical ruler on the
//                 left with carets that ride the ball's position, a vertical
//                 正常 / 警告 status column, hazard stripes above warn_g
//   middle panel  axis lines (lateral and vertical) with 0.5 g / 1 g ticks;
//                 a dotted depth axis runs through every panel
//   through the   a g-vector line from the centre to the current g, and a
//   stack         ball that follows the line out to its tip with a trail;
//                 small markers ride each axis with the ball's components
//   third panel   the driving state as large kanji (待機 加速 制動 左旋回
//                 右旋回 衝撃 危険) that flash in reverse video when it changes
//                 (title_z moves it; rear panels are dim through the glass)
//   front panel   a target-lock bracket tracks the ball from the brightest
//                 layer and shrinks as the ball recedes into the stack
//
// Built to stay on for hours: every orbit_min minutes the whole picture moves
// one pixel around a 2 x 2 square (right, down, left, up), so the frame and
// labels, which never move otherwise, do not wear the same OLED pixels.
#pragma once

#include "../effect.hpp"
#include "../gstate.hpp"

namespace gm {

class HudEffect : public Effect {
 public:
  HudEffect();
  const char* name() const override { return "hud"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

  Vec3 ballPosition() const { return ball_.pos(); }
  Vec3 tipPosition() const { return tip_; }
  DriveState state() const { return tracker_.state(); }
  const char* titleText() const;
  // Current anti burn-in offset in pixels (0 or 1 on each axis).
  int orbitX() const;
  int orbitY() const;

 private:
  static constexpr int kTrail = 40;

  float scaleG_ = 1.0f;      // lateral g at full travel
  float vscaleG_ = 0.6f;     // vertical g at full travel
  float dscaleG_ = 1.0f;     // longitudinal g at full depth
  float hz_ = 2.5f;          // ball spring
  float inertia_ = 1.0f;     // 1: ball goes where a loose object would
  float warnG_ = 0.7f;       // hazard stripes and 警告 above this
  float dangerG_ = 1.0f;     // title switches to 危険 above this
  float ballR_ = 4.5f;       // ball radius px
  float titleZ_ = 2.0f;      // panel for the state kanji (rear panels are dim through the stack)
  float trailS_ = 0.8f;      // trail length seconds
  float title_ = 1.0f;       // big kanji on the back panel
  float axes_ = 1.0f;        // axis lines and rulers
  float frame_ = 1.0f;       // border, readouts, status column
  float enterG_ = 0.25f, holdS_ = 0.4f, bumpG_ = 0.3f;
  float orbitMin_ = 2.0f;    // minutes per 1-px orbit step (0 = off)

  GStateTracker tracker_;
  Spring3 ball_;
  Vec3 tip_;
  GForce g_;
  float time_ = 0.0f;         // wraps every 60 s (blink timing only)
  float orbitTimer_ = 0.0f;
  int orbitStep_ = 0;
  float titleAge_ = 99.0f;
  float bumpAge_ = 99.0f;
  float hazard_ = 0.0f;
  bool danger_ = false;
  Vec3 trail_[kTrail];
  int trailHead_ = 0, trailCount_ = 0;
  float trailTimer_ = 0.0f;

  Vec3 travel() const;
  void drawFrame(VoxelFrame& f, float horiz) const;
  void drawAxes(VoxelFrame& f) const;
  void drawTitle(VoxelFrame& f) const;
};

}  // namespace gm
