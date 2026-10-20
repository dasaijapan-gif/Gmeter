// Differential tests of the driving-state tracker (gm_gstate.c vs gm::GStateTracker).
#include "diff_util.hpp"

using diff::Expect;
using diff::same;

namespace {

bool same_tracker(const gm_gstate_t& c, const gm::GStateTracker& p) {
  return static_cast<int>(gm_gstate_state(&c)) == static_cast<int>(p.state()) &&
         same(gm_gstate_seconds_in_state(&c), p.secondsInState()) && same(c.enter_g, p.enterG) &&
         same(c.hold_s, p.holdS) && same(c.bump_g, p.bumpG);
}

}  // namespace

TEST(gstate_defaults) {
  Expect ex;
  gm_gstate_t c;
  gm_gstate_init(&c);
  gm::GStateTracker p;
  EXPECT(ex, same_tracker(c, p));
  EXPECT(ex, gm_gstate_state(&c) == GM_DRIVE_IDLE);
  EXPECT(ex, static_cast<int>(GM_DRIVE_ACCELERATE) == static_cast<int>(gm::DriveState::Accelerate) &&
                 static_cast<int>(GM_DRIVE_BRAKE) == static_cast<int>(gm::DriveState::Brake) &&
                 static_cast<int>(GM_DRIVE_LEFT) == static_cast<int>(gm::DriveState::Left) &&
                 static_cast<int>(GM_DRIVE_RIGHT) == static_cast<int>(gm::DriveState::Right));
}

TEST(gstate_random_drives) {
  Expect ex;
  diff::Rand r(31);
  for (int run = 0; run < 400; ++run) {
    gm_gstate_t c;
    gm_gstate_init(&c);
    gm::GStateTracker p;
    if (run % 4 != 0) {  // random thresholds, including degenerate ones
      c.enter_g = p.enterG = r.chance(0.1) ? 0.0f : r.frange(0.02f, 0.9f);
      c.hold_s = p.holdS = r.chance(0.1) ? 0.0f : r.frange(0.0f, 1.5f);
      c.bump_g = p.bumpG = r.chance(0.1) ? 0.0f : r.frange(0.05f, 0.8f);
    }
    gm_vec3_t dyn = gm_vec3_zero();
    gm_vec3_t target = gm_vec3_zero();
    for (int i = 0; i < 1500; ++i) {
      if (r.chance(0.02))
        target = gm_vec3(r.frange(-1.2f, 1.2f), r.chance(0.2) ? r.frange(-0.8f, 0.8f) : 0.0f, r.frange(-1.2f, 1.2f));
      // Smooth approach plus occasional vertical spikes and sharp jumps.
      dyn = gm_vec3_lerp(dyn, target, 0.08f);
      gm_vec3_t d = dyn;
      if (r.chance(0.03)) d.y += r.frange(-0.9f, 0.9f);
      if (r.chance(0.01)) d = gm_vec3(r.frange(-2, 2), r.frange(-2, 2), r.frange(-2, 2));
      const gm_gforce_t g = gm_gforce_from_proper(gm_vec3_add(gm_gforce_rest_proper(), d));
      gm::GForce gp;
      gp.proper = diff::to_cpp(g.proper);
      static const float kOddDt[] = {0.0f, 1e-6f, 0.5f, 2.0f};
      const float dt = r.chance(0.05) ? r.pick(kOddDt) : r.frange(0.001f, 0.07f);
      bool cb = false, pb = false;
      bool c_changed, p_changed;
      if (r.chance(0.3)) {
        c_changed = gm_gstate_update(&c, g, dt, nullptr);
        p_changed = p.update(gp, dt);
      } else {
        c_changed = gm_gstate_update(&c, g, dt, &cb);
        p_changed = p.update(gp, dt, &pb);
      }
      EXPECT(ex, c_changed == p_changed);
      EXPECT(ex, cb == pb);
      EXPECT(ex, same_tracker(c, p));
      if (r.chance(0.002)) {
        gm_gstate_reset(&c);
        p.reset();
        EXPECT(ex, same_tracker(c, p));
      }
    }
  }
}

TEST(gstate_exact_thresholds) {
  Expect ex;
  diff::Rand r(32);
  // Dyadic thresholds, components, dt and hold times: comparisons such as
  // |d.y| >= bump_g, held >= hold_s, mine >= 0.6 * enter_g and
  // best >= mine + 0.1 are decided on exact equality.
  const float grid[] = {0.0f,  0.125f, 0.25f,       0.375f,       0.5f,          0.625f,       0.75f,       1.0f,
                        0.15f, 0.6f,   0.5f + 0.1f, 0.25f + 0.1f, 0.375f + 0.1f, 0.6f * 0.25f, 0.6f * 0.5f, 0.3f};
  const float thresholds[] = {0.25f, 0.5f, 0.125f, 0.3f, 0.375f};
  const float holds[] = {0.0f, 0.25f, 0.5f, 0.0625f};
  for (int run = 0; run < 600; ++run) {
    gm_gstate_t c;
    gm_gstate_init(&c);
    gm::GStateTracker p;
    c.enter_g = p.enterG = r.pick(thresholds);
    c.hold_s = p.holdS = r.pick(holds);
    c.bump_g = p.bumpG = r.pick(thresholds);
    for (int i = 0; i < 400; ++i) {
      const float sx = r.chance(0.5) ? 1.0f : -1.0f, sy = r.chance(0.5) ? 1.0f : -1.0f,
                  sz = r.chance(0.5) ? 1.0f : -1.0f;
      const float dx = sx * r.pick(grid), dy = r.chance(0.3) ? sy * r.pick(grid) : 0.0f, dz = sz * r.pick(grid);
      // Rest is (0, 1, 0): x and z are exact, and y is exact for these dyadic values.
      gm::GForce gp;
      gp.proper = gm::Vec3{dx, 1.0f + dy, dz};
      const gm_gforce_t g = gm_gforce_from_proper(diff::to_c(gp.proper));
      const float dt = r.chance(0.8) ? 1.0f / 64.0f : 1.0f / 16.0f;
      bool cb = false, pb = false;
      EXPECT(ex, gm_gstate_update(&c, g, dt, &cb) == p.update(gp, dt, &pb));
      EXPECT(ex, cb == pb);
      EXPECT(ex, same_tracker(c, p));
    }
  }
}
