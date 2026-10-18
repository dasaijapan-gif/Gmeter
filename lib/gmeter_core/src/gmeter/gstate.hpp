// Turns g-force into a discrete driving state (idle, accelerate, brake, left,
// right) with hysteresis and a minimum hold time, plus bump detection on
// vertical spikes. Shared by effects that react to "what the car is doing".
#pragma once

#include <cmath>

#include "gforce.hpp"

namespace gm {

enum class DriveState : uint8_t { Idle, Accelerate, Brake, Left, Right };

class GStateTracker {
 public:
  float enterG = 0.25f;  // horizontal g that leaves idle
  float holdS = 0.4f;    // minimum seconds in a state
  float bumpG = 0.3f;    // vertical g spike that counts as a bump

  // Returns true when the state changed this update. `bumped` is set on the
  // update where a vertical spike starts.
  bool update(const GForce& g, float dt, bool* bumped = nullptr) {
    const Vec3 d = g.dynamic();
    const bool spike = std::fabs(d.y) >= bumpG && std::fabs(prevVert_) < bumpG;
    prevVert_ = d.y;
    if (bumped) *bumped = spike;
    held_ += dt;
    if (held_ < holdS) return false;
    const DriveState next = choose(d);
    if (next == state_) return false;
    state_ = next;
    held_ = 0.0f;
    return true;
  }
  DriveState state() const { return state_; }
  float secondsInState() const { return held_; }
  void reset() { state_ = DriveState::Idle; held_ = 0.0f; prevVert_ = 0.0f; }

 private:
  DriveState state_ = DriveState::Idle;
  float held_ = 0.0f;
  float prevVert_ = 0.0f;

  static float support(DriveState s, const Vec3& d) {
    switch (s) {
      case DriveState::Accelerate: return d.z;
      case DriveState::Brake: return -d.z;
      case DriveState::Right: return d.x;
      case DriveState::Left: return -d.x;
      default: return 0.0f;
    }
  }
  DriveState choose(const Vec3& d) const {
    DriveState best = DriveState::Idle;
    float bestV = 0.0f;
    static const DriveState kActive[4] = {DriveState::Accelerate, DriveState::Brake, DriveState::Left, DriveState::Right};
    for (DriveState s : kActive) {
      if (support(s, d) > bestV) { bestV = support(s, d); best = s; }
    }
    if (state_ != DriveState::Idle) {
      const float mine = support(state_, d);
      if (mine >= 0.6f * enterG && (best == state_ || bestV < mine + 0.1f)) return state_;
    }
    return bestV >= enterG ? best : DriveState::Idle;
  }
};

}  // namespace gm
