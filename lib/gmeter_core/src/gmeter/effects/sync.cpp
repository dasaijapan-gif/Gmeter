#include "sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../jp_text.hpp"

namespace gm {

SyncEffect::SyncEffect() {
  params_.add("rate", &rate_, 5.0f, 120.0f, "samples per second (window = 128 / rate)");
  params_.add("gain", &gain_, 2.0f, 30.0f, "trace px per g");
  params_.add("grid", &grid_, 0.0f, 1.0f, "grid lines");
  params_.add("labels", &labels_, 0.0f, 1.0f, "横 / 縦 / 上下 labels and 同期率");
  reset();
}

void SyncEffect::reset() {
  std::memset(hist_, 0, sizeof(hist_));
  head_ = 0;
  accum_ = 0.0f;
  scroll_ = 0.0f;
  syncPct_ = 0.0f;
}

void SyncEffect::update(const EffectContext& ctx) {
  const Vec3 d = ctx.g.dynamic();
  accum_ += ctx.dt * rate_;
  int n = 0;
  while (accum_ >= 1.0f && n < 16) {
    accum_ -= 1.0f;
    ++n;
    hist_[0][head_] = d.x;   // lateral
    hist_[1][head_] = d.z;   // longitudinal
    hist_[2][head_] = d.y;   // vertical
    head_ = (head_ + 1) % kHist;
    scroll_ += 1.0f;
    if (scroll_ >= 16.0f) scroll_ -= 16.0f;
  }
  if (accum_ > 1.0f) accum_ = 0.0f;
  syncPct_ = lerpf(syncPct_, 100.0f * ctx.g.horizontalG(), std::min(1.0f, ctx.dt * 6.0f));
}

void SyncEffect::render(VoxelFrame& f) {
  const int W = kW, H = kH, D = kD;
  const int bandH = H / 3;
  const int x0 = labels_ > 0.5f ? 18 : 2;
  const int x1 = labels_ > 0.5f ? W - 16 : W - 3;
  const char* names[3] = {"横", "縦", "上下"};
  for (int b = 0; b < 3; ++b) {
    const int z = std::min(D - 1, 1 + b);  // panels 2-4: separated in depth, still bright
    const int top = b * bandH, bottom = top + bandH - 1;
    const int cy = top + bandH / 2;
    const int half = bandH / 2 - 1;
    if (grid_ > 0.5f) {
      for (int x = x0; x <= x1; x += 3) f.set(x, cy, z);  // dotted zero line
      // Vertical divisions scroll with the trace.
      const int s = static_cast<int>(scroll_);
      for (int x = x1 - s; x >= x0; x -= 16)
        for (int y = top + 1; y < bottom; y += 2) f.set(x, y, D - 1);
    }
    // Trace: newest sample at the right edge, joined by line segments.
    int prevX = -1, prevY = 0;
    for (int x = x1; x >= x0; --x) {
      const int age = x1 - x;
      if (age >= kHist) break;
      const float v = hist_[b][(head_ - 1 - age + 2 * kHist) % kHist];
      const int y = clampi(cy - iround(v * gain_), cy - half, cy + half);
      if (prevX >= 0) f.line(z, prevX, prevY, x, y);
      else f.set(x, y, z);
      prevX = x;
      prevY = y;
    }
    if (labels_ > 0.5f) drawJp(f, kJpSmall, 0, 1, cy - kJpSmall.height / 2, names[b], 0);
    if (b > 0) for (int x = x0; x <= x1; x += 2) f.set(x, top, 0);  // band divider
  }
  if (labels_ > 0.5f) {
    // 同期率 written vertically on the right, percentage underneath.
    const int colX = W - 1 - kJpSmall.glyphs[0].width;
    const int used = drawJpVertical(f, kJpSmall, 0, colX, 0, "同期率", 0);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(syncPct_ + 0.5f));
    f.text(0, W - 4 * static_cast<int>(std::strlen(buf)), std::min(H - 6, used + 2), buf);
  }
}

}  // namespace gm
