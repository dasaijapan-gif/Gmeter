// Differential tests of the scripted g profiles (gm_scenario.c vs gm::Scenario)
// and the simulated raw sensor.
#include <climits>
#include <string>
#include <vector>

#include "diff_util.hpp"

using diff::Expect;
using diff::same;

namespace {

bool same_g(gm_gforce_t c, const gm::GForce& p) { return same(c.proper, p.proper); }

// dynamic_at / at over a time grid from before the start to past the end.
void compare_timeline(Expect& ex, const gm_scenario_t* c, const gm::Scenario& p, float step, uint32_t seed) {
  const float end = gm_scenario_duration(c) + 3.0f;
  gm_rng_t crng;
  gm_rng_seed(&crng, seed);
  gm::Rng prng(seed);
  int n = 0;
  for (float t = -2.0f; t <= end; t += step, ++n) {
    EXPECT(ex, same(gm_scenario_dynamic_at(c, t), p.dynamicAt(t)));
    EXPECT(ex, same_g(gm_scenario_at(c, t, nullptr), p.at(t)));
    EXPECT(ex, same_g(gm_scenario_at(c, t, &crng), p.at(t, &prng)));
  }
  EXPECT(ex, gm_rng_next(&crng) == prng.next());  // the same number of draws
  const float odd[] = {diff::kNaN, diff::kInf, -diff::kInf, 0.0f, -0.0f, 1e30f, -1e30f, 1e-30f};
  for (float t : odd) {
    EXPECT(ex, same(gm_scenario_dynamic_at(c, t), p.dynamicAt(t)));
    EXPECT(ex, same_g(gm_scenario_at(c, t, &crng), p.at(t, &prng)));
  }
}

}  // namespace

TEST(scenario_presets_match) {
  Expect ex;
  EXPECT(ex, gm_scenario_preset_count() == gm::Scenario::presetCount());
  EXPECT(ex, std::strcmp(gm_scenario_name(gm_scenario_preset(gm_scenario_preset_count() - 1)), "drive") == 0);
  for (int i = 0; i < gm_scenario_preset_count(); ++i) {
    const gm_scenario_t* c = gm_scenario_preset(i);
    const gm::Scenario& p = gm::Scenario::preset(i);
    EXPECT(ex, std::strcmp(gm_scenario_name(c), p.name()) == 0);
    EXPECT(ex, same(gm_scenario_duration(c), p.duration()));
    EXPECT(ex, same(c->noise_g, p.noiseG()));
    EXPECT(ex, same(c->loop_after, 0.0f));
    // Every key, exactly at its time and just around it.
    for (int k = 0; k < c->count; ++k) {
      const float t = c->keys[k].t;
      EXPECT(ex, same(gm_scenario_dynamic_at(c, t), p.dynamicAt(t)));
      EXPECT(ex, same(gm_scenario_dynamic_at(c, std::nextafter(t, -1e9f)), p.dynamicAt(std::nextafter(t, -1e9f))));
      EXPECT(ex, same(gm_scenario_dynamic_at(c, std::nextafter(t, 1e9f)), p.dynamicAt(std::nextafter(t, 1e9f))));
    }
    compare_timeline(ex, c, p, 0.0007f, 100u + static_cast<uint32_t>(i));
  }
}

TEST(scenario_find_and_clamp) {
  Expect ex;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const char* name = gm::Scenario::preset(i).name();
    const gm_scenario_t* c = gm_scenario_find(name);
    const gm::Scenario* p = gm::Scenario::find(name);
    EXPECT(ex, c != nullptr && p != nullptr);
    EXPECT(ex, c == gm_scenario_preset(i) && p == &gm::Scenario::preset(i));
  }
  const char* missing[] = {"", "driv", "drivex", "Drive", "DRIVE", " idle", "idle ", "nope"};
  for (const char* m : missing) EXPECT(ex, gm_scenario_find(m) == nullptr && gm::Scenario::find(m) == nullptr);
  const int idx[] = {INT_MIN, -1000, -2, -1, 0, 1, 5, 6, 7, 8, 100, INT_MAX};
  for (int i : idx) {
    const gm_scenario_t* c = gm_scenario_preset(i);
    const gm::Scenario& p = gm::Scenario::preset(i);
    EXPECT(ex, c - gm_scenario_preset(0) == &p - &gm::Scenario::preset(0));
    EXPECT(ex, std::strcmp(gm_scenario_name(c), p.name()) == 0);
  }
}

TEST(scenario_custom_profiles) {
  Expect ex;
  diff::Rand r(41);
  // Unsorted, duplicate and single keys, empty profiles and looping ones.
  for (int run = 0; run < 300; ++run) {
    const int n = run < 4 ? run : r.irange(0, 12);
    std::vector<gm_gkey_t> ck(static_cast<size_t>(n) + 1);
    std::vector<gm::GKey> pk(static_cast<size_t>(n) + 1);
    float t = r.frange(-1.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
      if (r.chance(0.8)) t += r.chance(0.1) ? 0.0f : r.frange(1e-7f, 2.0f);  // sometimes zero spans
      else t = r.frange(-1.0f, 10.0f);                                       // sometimes out of order
      const gm_vec3_t d = gm_vec3(r.frange(-1, 1), r.frange(-1, 1), r.frange(-1, 1));
      ck[static_cast<size_t>(i)] = gm_gkey_t{t, d};
      pk[static_cast<size_t>(i)] = gm::GKey{t, diff::to_cpp(d)};
    }
    const float noise = r.chance(0.3) ? 0.0f : r.frange(-0.01f, 0.05f);
    const float loop = r.chance(0.5) ? 0.0f : r.chance(0.2) ? -1.0f : r.frange(0.1f, 12.0f);
    const gm_scenario_t c = {"custom", ck.data(), n, noise, loop};
    const gm::Scenario p("custom", pk.data(), n, noise, loop);
    EXPECT(ex, same(gm_scenario_duration(&c), p.duration()));
    compare_timeline(ex, &c, p, 0.013f, 200u + static_cast<uint32_t>(run));
    for (int i = 0; i < 500; ++i) {
      const float tt = r.frange(-5.0f, 40.0f);
      EXPECT(ex, same(gm_scenario_dynamic_at(&c, tt), p.dynamicAt(tt)));
    }
  }
  // The C++ default noise and loop.
  const gm_gkey_t keys[] = {{0.0f, {0, 0, 0}}, {1.0f, {0.5f, 0, -0.5f}}};
  const gm::GKey pkeys[] = {{0.0f, {0, 0, 0}}, {1.0f, {0.5f, 0, -0.5f}}};
  const gm_scenario_t c = {"two", keys, 2, 0.015f, 0.0f};
  const gm::Scenario p("two", pkeys, 2);
  EXPECT(ex, same(c.noise_g, p.noiseG()));
  compare_timeline(ex, &c, p, 0.001f, 7u);
}

TEST(scenario_simulate_raw_sensor) {
  Expect ex;
  diff::Rand r(42);
  for (int run = 0; run < 3000; ++run) {
    // Random mountings from the calibration builder (orthonormal), plus some
    // arbitrary matrices: the transpose formula must match either way.
    gm::Mat3 m;
    if (run % 5 == 4) {
      for (auto& row : m.m)
        for (float& v : row) v = r.frange(-2.0f, 2.0f);
    } else {
      gm::Vec3 up{r.frange(-1, 1), r.frange(-1, 1), r.frange(-1, 1)};
      gm::Vec3 fwd{r.frange(-1, 1), r.frange(-1, 1), r.frange(-1, 1)};
      m = gm::GForceEstimator::makeCalibration(up, fwd).sensorToCar;
    }
    const gm_mat3_t cm = diff::to_c(m);
    const uint32_t seed = r.u32();
    gm_rng_t crng;
    gm_rng_seed(&crng, seed);
    gm::Rng prng(seed);
    for (int i = 0; i < 20; ++i) {
      const gm_vec3_t proper = gm_vec3(r.frange(-2, 2), r.frange(-1, 3), r.frange(-2, 2));
      gm::GForce pg;
      pg.proper = diff::to_cpp(proper);
      const float noise = (i % 4 == 0) ? 0.0f : (i % 4 == 1) ? -0.01f : r.frange(0.0f, 0.1f);
      EXPECT(ex, same(gm_simulate_raw_sensor(gm_gforce_from_proper(proper), &cm, &crng, noise),
                      gm::simulateRawSensor(pg, m, prng, noise)));
    }
    EXPECT(ex, gm_rng_next(&crng) == prng.next());
  }
}
