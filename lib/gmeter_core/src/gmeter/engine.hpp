// Runs the active effect, handles mode switching with a dithered crossfade,
// and overlays optional HUD text. Owns no hardware: feed it a GForce and a
// dt, read back the VoxelFrame.
#pragma once

#include <cstdint>

#include "effect.hpp"

namespace gm {

class Engine {
 public:
  static constexpr int kMaxEffects = 8;

  // Effects are not owned; keep them alive for the engine's lifetime.
  bool addEffect(Effect* e);
  int effectCount() const { return count_; }
  Effect* effect(int i) const { return (i >= 0 && i < count_) ? effects_[i] : nullptr; }
  Effect* current() const { return effect(current_); }
  int currentIndex() const { return current_; }
  int findEffect(const char* name) const;

  void setEffect(int index, bool crossfade = true);
  bool setEffect(const char* name, bool crossfade = true);
  void nextEffect() { if (count_) setEffect((current_ + 1) % count_); }

  // Seconds between automatic mode changes; 0 disables.
  void setAutoCycle(float seconds) { autoCycle_ = seconds; }
  void setCrossfadeSeconds(float s) { crossfadeSeconds_ = s; }
  // Shows the effect name on the front layer briefly after a switch.
  void setShowModeName(bool on) { showModeName_ = on; }
  // Draws the horizontal g readout on the front layer for every effect.
  void setReadout(bool on) { readout_ = on; }

  // Advance by dt seconds with the given (already filtered) g and render.
  void step(const GForce& g, float dt);
  const VoxelFrame& frame() const { return out_; }
  float time() const { return ctx_.time; }
  uint32_t frameCount() const { return ctx_.frame; }

  // Parameter access on the current effect.
  bool setParam(const char* name, float v) { return current() ? current()->params().set(name, v) : false; }

 private:
  Effect* effects_[kMaxEffects] = {};
  int count_ = 0;
  int current_ = -1;
  int outgoing_ = -1;
  float crossfadeT_ = 1.0f;
  float crossfadeSeconds_ = 0.4f;
  float autoCycle_ = 0.0f;
  float sinceSwitch_ = 0.0f;
  bool showModeName_ = true;
  bool readout_ = false;
  EffectContext ctx_;
  VoxelFrame out_, scratchA_, scratchB_;
};

}  // namespace gm
