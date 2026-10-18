// Sync-graph oscilloscope, after the plug-suit sync displays in Neon Genesis
// Evangelion. Lateral, longitudinal and vertical g scroll right to left as
// three traces, each in its own band and on its own panel (panels 2-4, so
// they separate in depth as well), labelled 横 / 縦 / 上下, with a 同期率 readout of the
// combined horizontal g as a percentage of 1 g.
#pragma once

#include "../effect.hpp"

namespace gm {

class SyncEffect : public Effect {
 public:
  static constexpr int kHist = kW;

  SyncEffect();
  const char* name() const override { return "sync"; }
  void reset() override;
  void update(const EffectContext& ctx) override;
  void render(VoxelFrame& f) override;

 private:
  float rate_ = 30.0f;   // samples per second (window = 128 / rate)
  float gain_ = 9.0f;    // px per g
  float grid_ = 1.0f;
  float labels_ = 1.0f;

  float hist_[3][kHist];
  int head_ = 0;
  float accum_ = 0.0f;
  float scroll_ = 0.0f;
  float syncPct_ = 0.0f;
};

}  // namespace gm
