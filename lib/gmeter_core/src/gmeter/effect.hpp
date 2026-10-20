// An Effect turns the current g-force into voxels. Effects are pure C++ and
// run identically in the ESP32 firmware and the host simulator.
#pragma once

#include <cstdint>

#include "gforce.hpp"
#include "params.hpp"
#include "voxel_frame.hpp"

namespace gm {

struct EffectContext {
  GForce g;            // filtered g-force, car frame
  float dt = 1.0f / 60.0f;  // seconds since the previous update (clamped by the engine)
  float time = 0.0f;   // seconds since the engine started
  uint32_t frame = 0;  // frame counter
};

class Effect {
 public:
  virtual ~Effect() = default;
  virtual const char* name() const = 0;

  // Called when the effect becomes active / inactive. Heavy effects allocate
  // their grids in activate() and free them in deactivate() so several big
  // effects can coexist on a small heap.
  virtual void activate() { reset(); }
  virtual void deactivate() {}
  virtual void reset() {}

  // Advance the simulation. May be called more than once per rendered frame.
  virtual void update(const EffectContext& ctx) = 0;
  // Draw into a cleared frame.
  virtual void render(VoxelFrame& frame) = 0;
  // Offset in pixels for the whole picture, against OLED burn-in. The Engine
  // applies it after its own overlays, so everything on the glass moves.
  virtual void burnInOffset(int& dx, int& dy) const { dx = dy = 0; }

  ParamTable& params() { return params_; }
  const ParamTable& params() const { return params_; }

 protected:
  ParamTable params_;
};

}  // namespace gm
