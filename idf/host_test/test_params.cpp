// Differential tests of the parameter registry (gm_params.c vs gm::ParamTable).
#include <string>
#include <vector>

#include "diff_util.hpp"

using diff::Expect;
using diff::same;

namespace {

const char* const kNames[] = {"gain", "speed", "hz", "radius", "damping", "trail", "rings", "hud", "scale", "x", "y",
                              "z",    "a",     "b",  "c",      "d",       "e",     "f",     "g",   "h",     "i", "j",
                              "k",    "l",     "m",  "n",      "o",       "p",     "q",     "r"};  // 30 > GM_PARAMS_MAX

bool same_table(const gm_param_table_t& c, const gm::ParamTable& p, const float* cv, const float* pv) {
  if (gm_params_count(&c) != p.count()) return false;
  for (int i = 0; i < p.count(); ++i) {
    const gm_param_t* a = gm_params_at(&c, i);
    const gm::Param& b = p.at(i);
    if (std::strcmp(a->name, b.name) != 0 || std::strcmp(a->help, b.help) != 0) return false;
    if (!same(a->min, b.min) || !same(a->max, b.max)) return false;
    // Each table points at its own storage, at the same index.
    if (a->value != cv + i || b.value != pv + i) return false;
    if (!same(*a->value, *b.value)) return false;
  }
  return true;
}

}  // namespace

TEST(params_registry_matches) {
  Expect ex;
  diff::Rand r(21);
  for (int round = 0; round < 200; ++round) {
    gm_param_table_t c;
    gm_params_init(&c);
    EXPECT(ex, gm_params_count(&c) == 0);
    gm::ParamTable p;
    static float cv[32], pv[32];
    const int n = r.irange(0, 30);
    for (int i = 0; i < n; ++i) {
      cv[i] = pv[i] = r.frange(-10.0f, 10.0f);
      const float lo = r.frange(-5.0f, 0.0f), hi = r.frange(0.0f, 5.0f);
      if (i % 3 == 0) {
        gm_params_add(&c, kNames[i], &cv[i], lo, hi, "");
        p.add(kNames[i], &pv[i], lo, hi);  // C++ default help ""
      } else {
        gm_params_add(&c, kNames[i], &cv[i], lo, hi, "help text");
        p.add(kNames[i], &pv[i], lo, hi, "help text");
      }
    }
    EXPECT(ex, gm_params_count(&c) == (n < GM_PARAMS_MAX ? n : GM_PARAMS_MAX));
    EXPECT(ex, same_table(c, p, cv, pv));
    const float specials[] = {0.0f, -0.0f, 1e30f, -1e30f, diff::kInf, -diff::kInf, diff::kNaN, 4.999f, -4.999f};
    for (int k = 0; k < 300; ++k) {
      const char* name = kNames[r.irange(0, 29)];
      std::string other;
      if (r.chance(0.1)) {
        other = std::string(name) + "x";  // unknown names
        name = other.c_str();
      } else if (r.chance(0.05)) {
        name = "";
      }
      const float v = r.chance(0.3) ? r.pick(specials) : r.frange(-20.0f, 20.0f);
      EXPECT(ex, gm_params_set(&c, name, v) == p.set(name, v));
      EXPECT(ex, same_table(c, p, cv, pv));
      float co = 123.0f, po = 123.0f;
      EXPECT(ex, gm_params_get(&c, name, &co) == p.get(name, po));
      EXPECT(ex, same(co, po));
      const gm_param_t* fc = gm_params_find(&c, name);
      const gm::Param* fp = p.find(name);
      EXPECT(ex, (fc == nullptr) == (fp == nullptr));
      if (fc && fp) EXPECT(ex, fc - gm_params_at(&c, 0) == fp - &p.at(0));
    }
  }
}

TEST(params_duplicate_names_find_the_first) {
  Expect ex;
  gm_param_table_t c;
  gm_params_init(&c);
  gm::ParamTable p;
  static float cv[3] = {1, 2, 3}, pv[3] = {1, 2, 3};
  for (int i = 0; i < 3; ++i) {
    gm_params_add(&c, "dup", &cv[i], 0.0f, 10.0f, "");
    p.add("dup", &pv[i], 0.0f, 10.0f);
  }
  EXPECT(ex, gm_params_set(&c, "dup", 7.5f) && p.set("dup", 7.5f));
  for (int i = 0; i < 3; ++i) EXPECT(ex, same(cv[i], pv[i]));
  EXPECT(ex, gm_params_find(&c, "dup") == gm_params_at(&c, 0));
}
