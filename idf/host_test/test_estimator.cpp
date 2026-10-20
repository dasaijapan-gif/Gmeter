// Differential tests of the g-force estimator (gm_estimator.c vs
// gm::GForceEstimator): filters, calibration paths, auto-zero and reset.
#include <memory>

#include "diff_util.hpp"

using diff::Expect;
using diff::same;

namespace {

bool same_cal(const gm_calibration_t& c, const gm::Calibration& p) {
  return c.valid == p.valid && same(c.sensor_to_car, p.sensorToCar);
}

// Every observable piece of estimator state.
bool same_est(const gm_estimator_t& c, const gm::GForceEstimator& p) {
  return same(gm_estimator_current(&c).proper, p.current().proper) && same(gm_estimator_raw_car(&c), p.rawCar()) &&
         same_cal(gm_estimator_calibration(&c), p.calibration()) &&
         gm_estimator_forward_trusted(&c) == p.forwardTrusted() &&
         same(gm_estimator_nominal_forward(&c), p.nominalForward()) &&
         gm_estimator_rest_sample_count(&c) == p.restSampleCount() &&
         same(gm_estimator_cutoff_hz(&c), p.cutoffHz()) && same(gm_estimator_sample_hz(&c), p.sampleHz());
}

gm::Vec3 rnd_unit(diff::Rand& r) {
  for (;;) {
    gm::Vec3 v{r.frange(-1, 1), r.frange(-1, 1), r.frange(-1, 1)};
    if (v.length2() > 0.05f && v.length2() <= 1.0f) return v.normalized();
  }
}

// A random physical mounting: car->sensor rotation as a calibration matrix.
gm::Mat3 rnd_mounting(diff::Rand& r) {
  for (;;) {
    const gm::Calibration cal = gm::GForceEstimator::makeCalibration(rnd_unit(r), rnd_unit(r));
    if (cal.valid) return cal.sensorToCar;
  }
}

struct Pair {
  gm_estimator_t c;
  gm::GForceEstimator p;
  Pair() { gm_estimator_init(&c); }
  // Feeds one raw sample to both and compares the result and all state.
  void update(Expect& ex, const gm::Vec3& raw) {
    const gm_gforce_t gc = gm_estimator_update(&c, diff::to_c(raw));
    const gm::GForce& gp = p.update(raw);
    EXPECT(ex, same(gc.proper, gp.proper));
    EXPECT(ex, same_est(c, p));
  }
};

}  // namespace

TEST(estimator_defaults_and_filters) {
  Expect ex;
  auto e = std::make_unique<Pair>();
  EXPECT(ex, same_est(e->c, e->p));
  EXPECT(ex, same(e->c.g.proper, gm::GForce{}.proper));
  diff::Rand r(51);
  struct Cfg {
    float cutoff, sample;
  };
  const Cfg cfgs[] = {{4.0f, 200.0f}, {3.0f, 100.0f}, {0.0f, 200.0f}, {150.0f, 200.0f}, {10.0f, 1000.0f},
                      {4.0f, 0.0f},   {1.0f, 50.0f},  {-2.0f, 200.0f}};
  for (const Cfg& cfg : cfgs) {
    auto s = std::make_unique<Pair>();
    gm_estimator_set_filter(&s->c, cfg.cutoff, cfg.sample);
    s->p.setFilter(cfg.cutoff, cfg.sample);
    EXPECT(ex, same_est(s->c, s->p));
    gm::Vec3 level{0, 1, 0};
    for (int i = 0; i < 4000; ++i) {
      if (r.chance(0.01)) level = gm::Vec3{r.frange(-1, 1), r.frange(0, 2), r.frange(-1, 1)};
      s->update(ex, level + gm::Vec3{r.frange(-0.1f, 0.1f), r.frange(-0.1f, 0.1f), r.frange(-0.1f, 0.1f)});
      if (i == 2000) {  // change the filter mid-stream
        gm_estimator_set_filter(&s->c, cfg.cutoff * 0.5f, cfg.sample);
        s->p.setFilter(cfg.cutoff * 0.5f, cfg.sample);
      }
    }
  }
}

TEST(estimator_scenarios_through_random_mountings) {
  Expect ex;
  diff::Rand r(52);
  for (int run = 0; run < 40; ++run) {
    auto s = std::make_unique<Pair>();
    const gm::Mat3 mount = rnd_mounting(r);
    const gm_mat3_t cmount = diff::to_c(mount);
    // Calibrate both from the known mounting, or not at all.
    if (run % 4 != 0) {
      gm_calibration_t cc;
      cc.sensor_to_car = cmount;
      cc.valid = true;
      gm::Calibration pc;
      pc.sensorToCar = mount;
      pc.valid = true;
      gm_estimator_set_calibration(&s->c, cc);
      s->p.setCalibration(pc);
    }
    if (run % 3 == 1) {
      gm_estimator_set_filter(&s->c, 3.0f, 100.0f);
      s->p.setFilter(3.0f, 100.0f);
    }
    const gm::Scenario& sc = gm::Scenario::preset(run % gm::Scenario::presetCount());
    const gm_scenario_t* csc = gm_scenario_preset(run % gm_scenario_preset_count());
    gm::Rng prng(1000u + static_cast<uint32_t>(run));
    gm_rng_t crng;
    gm_rng_seed(&crng, 1000u + static_cast<uint32_t>(run));
    const float hz = gm_estimator_sample_hz(&s->c);
    for (int i = 0; i < static_cast<int>((sc.duration() + 1.0f) * hz); ++i) {
      const float t = static_cast<float>(i) / hz;
      const gm::GForce pg = sc.at(t, &prng);
      const gm_gforce_t cg = gm_scenario_at(csc, t, &crng);
      EXPECT(ex, same(cg.proper, pg.proper));
      const gm::Vec3 raw = gm::simulateRawSensor(pg, mount, prng, 0.02f);
      const gm_vec3_t craw = gm_simulate_raw_sensor(cg, &cmount, &crng, 0.02f);
      EXPECT(ex, same(craw, raw));
      s->update(ex, raw);
      if (i == 300 && run % 5 == 2) {
        gm_estimator_reset(&s->c);
        s->p.reset();
        EXPECT(ex, same_est(s->c, s->p));
      }
    }
  }
}

TEST(estimator_rest_and_brake_calibration) {
  Expect ex;
  diff::Rand r(53);
  for (int run = 0; run < 600; ++run) {
    auto s = std::make_unique<Pair>();
    const gm::Mat3 mount = rnd_mounting(r);
    gm::Rng noise(77u + static_cast<uint32_t>(run));
    // Nominal forward: right, slightly off, vertical (needs braking), or degenerate.
    const int hint = run % 6;
    gm::Vec3 nominal{1, 0, 0};
    if (hint == 1) nominal = mount.row(2) + gm::Vec3{0.2f, -0.1f, 0.15f};
    if (hint == 2) nominal = mount.row(1);  // vertical: up-only fallback, forward untrusted
    if (hint == 3) nominal = gm::Vec3{0, 0, 0};
    if (hint == 4) nominal = mount.row(2) * 1e-12f;
    if (hint == 5) nominal = rnd_unit(r);
    gm_estimator_set_nominal_forward(&s->c, diff::to_c(nominal));
    s->p.setNominalForward(nominal);
    EXPECT(ex, same_est(s->c, s->p));

    gm_estimator_begin_rest_calibration(&s->c);
    s->p.beginRestCalibration();
    // Sample count around the minimum of 8, and a rest magnitude sometimes out of range.
    const int samples = run % 7 == 0 ? r.irange(0, 9) : r.irange(8, 400);
    static const float kScales[] = {0.5f, 0.69f, 1.31f, 2.0f, 0.0f};
    const float scale = run % 11 == 0 ? r.pick(kScales) : 1.0f;
    gm::GForce rest;
    for (int i = 0; i < samples; ++i) {
      const gm::Vec3 raw = gm::simulateRawSensor(rest, mount, noise, 0.02f) * scale;
      gm_estimator_feed_rest_sample(&s->c, diff::to_c(raw));
      s->p.feedRestSample(raw);
      EXPECT(ex, gm_estimator_rest_sample_count(&s->c) == s->p.restSampleCount());
    }
    const bool ok_c = gm_estimator_finish_rest_calibration(&s->c);
    const bool ok_p = s->p.finishRestCalibration();
    EXPECT(ex, ok_c == ok_p);
    EXPECT(ex, same_est(s->c, s->p));
    // A few updates after calibration.
    for (int i = 0; i < 50; ++i) s->update(ex, gm::simulateRawSensor(rest, mount, noise, 0.02f));

    // Braking captures: strong, weak (rejected), straight up/down, and random.
    for (int k = 0; k < 4; ++k) {
      gm::GForce brake;
      const float decel = k == 0 ? -0.6f : k == 1 ? -0.02f : r.frange(-1.0f, 0.3f);
      brake.proper = gm::Vec3{k == 3 ? r.frange(-0.3f, 0.3f) : 0.0f, 1.0f, decel};
      gm::Vec3 mean = gm::simulateRawSensor(brake, mount, noise, 0.01f);
      if (k == 2 && run % 2) mean = mount.row(1) * 1.3f;  // no horizontal part at all
      const bool bc = gm_estimator_calibrate_forward_from_braking(&s->c, diff::to_c(mean));
      const bool bp = s->p.calibrateForwardFromBraking(mean);
      EXPECT(ex, bc == bp);
      EXPECT(ex, same_est(s->c, s->p));
      for (int i = 0; i < 20; ++i) s->update(ex, gm::simulateRawSensor(brake, mount, noise, 0.02f));
    }
  }
  // Braking without any calibration is rejected the same way.
  auto s = std::make_unique<Pair>();
  EXPECT(ex, gm_estimator_calibrate_forward_from_braking(&s->c, gm_vec3(0, 1, -0.5f)) ==
                 s->p.calibrateForwardFromBraking(gm::Vec3{0, 1, -0.5f}));
  EXPECT(ex, same_est(s->c, s->p));
}

TEST(estimator_make_calibration) {
  Expect ex;
  diff::Rand r(54);
  const gm::Vec3 axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 0},
                           {1e-12f, 0, 0}, {1, 1, 0}, {0.3f, 0.9f, 0.1f}};
  for (const gm::Vec3& up : axes)
    for (const gm::Vec3& fwd : axes) {
      EXPECT(ex, same_cal(gm_estimator_make_calibration(diff::to_c(up), diff::to_c(fwd)),
                          gm::GForceEstimator::makeCalibration(up, fwd)));
      EXPECT(ex, same_cal(gm_estimator_make_up_only_calibration(diff::to_c(up)),
                          gm::GForceEstimator::makeUpOnlyCalibration(up)));
    }
  for (int i = 0; i < 20000; ++i) {
    gm::Vec3 up = rnd_unit(r) * r.frange(0.0f, 2.0f);
    gm::Vec3 fwd = rnd_unit(r) * r.frange(0.0f, 2.0f);
    if (i % 4 == 0) {
      // Around the 30-degree limit: fwd = cos(a) * up + sin(a) * perpendicular.
      const float a = r.frange(0.4f, 0.65f);
      const gm::Vec3 u = up.normalized();
      const gm::Vec3 perp = u.cross(rnd_unit(r)).normalized();
      fwd = u * std::cos(a) + perp * std::sin(a);
    }
    EXPECT(ex, same_cal(gm_estimator_make_calibration(diff::to_c(up), diff::to_c(fwd)),
                        gm::GForceEstimator::makeCalibration(up, fwd)));
    EXPECT(ex, same_cal(gm_estimator_make_up_only_calibration(diff::to_c(up)),
                        gm::GForceEstimator::makeUpOnlyCalibration(up)));
  }
  const gm_calibration_t none = gm_calibration_none();
  EXPECT(ex, same_cal(none, gm::Calibration{}));
}

TEST(estimator_auto_zero_long_runs) {
  Expect ex;
  diff::Rand r(55);
  const float taus[] = {60.0f, 5.0f, 0.5f, 0.0f};
  for (int run = 0; run < 8; ++run) {
    auto s = std::make_unique<Pair>();
    const gm::Mat3 mount = rnd_mounting(r);
    gm::Calibration pc;
    pc.sensorToCar = mount;
    pc.valid = true;
    gm_calibration_t cc;
    cc.sensor_to_car = diff::to_c(mount);
    cc.valid = true;
    gm_estimator_set_calibration(&s->c, cc);
    s->p.setCalibration(pc);
    const bool on = run % 4 != 3;
    const float tau = taus[run % 4];
    if (run % 2) {
      gm_estimator_set_auto_zero(&s->c, on, tau);
      s->p.setAutoZero(on, tau);
    } else {
      gm_estimator_set_auto_zero(&s->c, on, 60.0f);  // the C++ default tau
      s->p.setAutoZero(on);
    }
    gm::Rng noise(500u + static_cast<uint32_t>(run));
    // Ten simulated minutes at 200 Hz: parked on a slope (auto-zero acts),
    // driving (it must not), parked again, a reset, and switching it off.
    gm::GForce g;
    g.proper = gm::Vec3{0.05f, 0.998f, -0.04f};
    for (int i = 0; i < 200 * 600; ++i) {
      const int sec = i / 200;
      float jitter = 0.002f;
      if (sec >= 200 && sec < 320) {
        jitter = 0.08f;
        g.proper = gm::Vec3{0.3f * std::sin(static_cast<float>(i) * 0.003f), 1.0f, 0.2f};
      } else if (sec >= 320) {
        g.proper = gm::Vec3{-0.03f, 0.999f, 0.06f};
      }
      if (i == 200 * 400) {
        gm_estimator_reset(&s->c);
        s->p.reset();
      }
      if (i == 200 * 500) {
        gm_estimator_set_auto_zero(&s->c, !on, tau);
        s->p.setAutoZero(!on, tau);
      }
      s->update(ex, gm::simulateRawSensor(g, mount, noise, jitter));
    }
  }
}
