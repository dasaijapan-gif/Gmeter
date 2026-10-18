// Tiny parameter registry so every effect's tunables can be listed and set by
// name from the serial CLI, the host simulator's command line, or a web page.
#pragma once

#include <cmath>
#include <cstring>

#include "math.hpp"

namespace gm {

struct Param {
  const char* name;
  float* value;
  float min, max;
  const char* help;
};

class ParamTable {
 public:
  static constexpr int kMax = 24;

  void add(const char* name, float* value, float min, float max, const char* help = "") {
    if (count_ < kMax) params_[count_++] = Param{name, value, min, max, help};
  }
  int count() const { return count_; }
  const Param& at(int i) const { return params_[i]; }
  const Param* find(const char* name) const {
    for (int i = 0; i < count_; ++i)
      if (std::strcmp(params_[i].name, name) == 0) return &params_[i];
    return nullptr;
  }
  bool set(const char* name, float v) {
    const Param* p = find(name);
    if (!p || !std::isfinite(v)) return false;
    *p->value = clampf(v, p->min, p->max);
    return true;
  }
  bool get(const char* name, float& out) const {
    const Param* p = find(name);
    if (!p) return false;
    out = *p->value;
    return true;
  }

 private:
  Param params_[kMax] = {};
  int count_ = 0;
};

}  // namespace gm
