#include "engine.hpp"

#include <cstdio>
#include <cstring>

namespace gm {

bool Engine::addEffect(Effect* e) {
  if (!e || count_ >= kMaxEffects) return false;
  effects_[count_++] = e;
  if (current_ < 0) {
    current_ = 0;
    e->activate();
  }
  return true;
}

int Engine::findEffect(const char* name) const {
  for (int i = 0; i < count_; ++i)
    if (std::strcmp(effects_[i]->name(), name) == 0) return i;
  return -1;
}

void Engine::setEffect(int index, bool crossfade) {
  if (index < 0 || index >= count_ || index == current_) return;
  if (outgoing_ >= 0 && outgoing_ != index) effects_[outgoing_]->deactivate();
  outgoing_ = crossfade && crossfadeSeconds_ > 0.0f ? current_ : -1;
  if (!crossfade && current_ >= 0) effects_[current_]->deactivate();
  current_ = index;
  effects_[current_]->activate();
  crossfadeT_ = outgoing_ >= 0 ? 0.0f : 1.0f;
  sinceSwitch_ = 0.0f;
}

bool Engine::setEffect(const char* name, bool crossfade) {
  const int i = findEffect(name);
  if (i < 0) return false;
  setEffect(i, crossfade);
  return true;
}

void Engine::step(const GForce& g, float dt) {
  dt = clampf(dt, 1.0f / 1000.0f, 1.0f / 15.0f);
  ctx_.g = g;
  ctx_.dt = dt;
  ctx_.time += dt;
  ++ctx_.frame;
  sinceSwitch_ += dt;

  if (autoCycle_ > 0.0f && sinceSwitch_ >= autoCycle_ && count_ > 1) nextEffect();

  Effect* cur = current();
  if (!cur) { out_.clear(); return; }

  cur->update(ctx_);
  if (outgoing_ >= 0) {
    Effect* old = effects_[outgoing_];
    old->update(ctx_);
    crossfadeT_ += dt / crossfadeSeconds_;
    scratchA_.clear();
    scratchB_.clear();
    old->render(scratchA_);
    cur->render(scratchB_);
    out_.crossfade(scratchA_, scratchB_, crossfadeT_);
    if (crossfadeT_ >= 1.0f) {
      old->deactivate();
      outgoing_ = -1;
    }
  } else {
    out_.clear();
    cur->render(out_);
  }

  if (showModeName_ && sinceSwitch_ < 1.5f) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%s", cur->name());
    for (char* p = buf; *p; ++p) *p = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 32) : *p;
    out_.clearBox(0, 0, 1 + 4 * static_cast<int>(std::strlen(buf)), 6);
    out_.text(0, 1, 1, buf);
  }
  if (readout_) {
    char buf[12];
    const float hg = g.horizontalG();
    std::snprintf(buf, sizeof(buf), "%d.%02dG", static_cast<int>(hg), static_cast<int>(hg * 100.0f) % 100);
    out_.clearBox(0, VoxelFrame::H - 7, 1 + 4 * static_cast<int>(std::strlen(buf)), VoxelFrame::H - 1);
    out_.text(0, 1, VoxelFrame::H - 6, buf);
  }
}

}  // namespace gm
