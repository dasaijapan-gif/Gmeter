// Self-contained unit tests for the core library. Build with host/CMakeLists.txt
// (target gmeter_tests) and run; exit code is the number of failures.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gmeter/gmeter.hpp"
// A header exactly as Gmeter Studio exports it; compiling it here pins the format.
#include "fixtures/studio_export.hpp"

using namespace gm;

namespace {
int gFailures = 0;
int gChecks = 0;
const char* gCurrent = "";

void check(bool ok, const char* expr, const char* file, int line) {
  ++gChecks;
  if (!ok) {
    ++gFailures;
    std::printf("  FAIL [%s] %s:%d: %s\n", gCurrent, file, line, expr);
  }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) check(std::fabs((a) - (b)) <= (tol), #a " ~= " #b, __FILE__, __LINE__)

struct Test {
  const char* name;
  std::function<void()> fn;
};
std::vector<Test>& tests() { static std::vector<Test> t; return t; }
struct Reg { Reg(const char* n, std::function<void()> f) { tests().push_back({n, std::move(f)}); } };
#define TEST(name) \
  void test_##name(); \
  Reg reg_##name(#name, test_##name); \
  void test_##name()

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Run an effect through a constant g for `seconds`, returning the final frame.
VoxelFrame runEffect(Effect& e, const GForce& g, float seconds, float fps = 60.0f) {
  e.activate();
  EffectContext ctx;
  ctx.g = g;
  ctx.dt = 1.0f / fps;
  const int n = static_cast<int>(seconds * fps);
  for (int i = 0; i < n; ++i) { ctx.time += ctx.dt; ++ctx.frame; e.update(ctx); }
  VoxelFrame f;
  e.render(f);
  return f;
}

// Centre of lit voxels (voxel coordinates).
Vec3 centroid(const VoxelFrame& f, int* count = nullptr) {
  Vec3 sum; int n = 0;
  for (int z = 0; z < kD; ++z)
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x)
        if (f.get(x, y, z)) { sum += Vec3{(float)x, (float)y, (float)z}; ++n; }
  if (count) *count = n;
  return n ? sum / (float)n : Vec3{};
}
}  // namespace

// ---- math ---------------------------------------------------------------------------------
TEST(vec3_basics) {
  Vec3 a{1, 2, 3}, b{4, 5, 6};
  CHECK_NEAR(a.dot(b), 32.0f, 1e-6f);
  Vec3 c = a.cross(b);
  CHECK_NEAR(c.x, -3.0f, 1e-6f); CHECK_NEAR(c.y, 6.0f, 1e-6f); CHECK_NEAR(c.z, -3.0f, 1e-6f);
  CHECK_NEAR((Vec3{3, 4, 0}.length()), 5.0f, 1e-6f);
  CHECK_NEAR((Vec3{0, 0, 2}.normalized().z), 1.0f, 1e-6f);
  Vec3 x{1, 0, 0}, y{0, 1, 0};
  CHECK_NEAR(x.cross(y).z, 1.0f, 1e-6f);  // right-handed
}

TEST(biquad_lowpass) {
  LowPassBiquad lp;
  lp.configure(4.0f, 200.0f);
  lp.reset(0.0f);
  // DC gain -> 1.
  float y = 0.0f;
  for (int i = 0; i < 2000; ++i) y = lp.process(1.0f);
  CHECK_NEAR(y, 1.0f, 1e-3f);
  // 50 Hz tone strongly attenuated.
  lp.reset(0.0f);
  float peak = 0.0f;
  for (int i = 0; i < 2000; ++i) {
    float v = lp.process(std::sin(kTwoPi * 50.0f * i / 200.0f));
    if (i > 1000) peak = std::max(peak, std::fabs(v));
  }
  CHECK(peak < 0.02f);
  // 0.5 Hz tone passes.
  lp.reset(0.0f);
  peak = 0.0f;
  for (int i = 0; i < 4000; ++i) {
    float v = lp.process(std::sin(kTwoPi * 0.5f * i / 200.0f));
    if (i > 2000) peak = std::max(peak, std::fabs(v));
  }
  CHECK(peak > 0.95f);
}

TEST(spring_converges_without_overshoot) {
  Spring s;
  float maxv = 0.0f;
  for (int i = 0; i < 600; ++i) { s.step(10.0f, 1.0f / 60.0f, 2.0f, 1.0f); maxv = std::max(maxv, s.pos); }
  CHECK_NEAR(s.pos, 10.0f, 0.01f);
  CHECK(maxv <= 10.05f);
  // Closed form: stable at a huge w*dt (hz 8, dt 1/15 -> w*dt = 3.35) where Euler diverges.
  Spring big;
  for (int i = 0; i < 60; ++i) big.step(10.0f, 1.0f / 15.0f, 8.0f, 1.0f);
  CHECK_NEAR(big.pos, 10.0f, 0.01f);
  // Under- and over-damped variants converge too.
  Spring under, over;
  for (int i = 0; i < 1200; ++i) { under.step(5.0f, 1.0f / 60.0f, 2.0f, 0.3f); over.step(5.0f, 1.0f / 60.0f, 2.0f, 1.8f); }
  CHECK_NEAR(under.pos, 5.0f, 0.02f);
  CHECK_NEAR(over.pos, 5.0f, 0.05f);
  // Step response of the critically damped spring matches 1 - e^{-wt}(1 + wt).
  Spring ref;
  for (int i = 0; i < 30; ++i) ref.step(1.0f, 1.0f / 60.0f, 1.0f, 1.0f);
  const float wt = kTwoPi * 0.5f;
  CHECK_NEAR(ref.pos, 1.0f - std::exp(-wt) * (1.0f + wt), 1e-3f);
}

// ---- g-force conventions -------------------------------------------------------------
TEST(gforce_conventions) {
  GForce rest;
  CHECK_NEAR(rest.dynamic().length(), 0.0f, 1e-6f);
  CHECK_NEAR(rest.effectiveGravity().y, -1.0f, 1e-6f);
  GForce braking; braking.proper = Vec3{0, 1, -0.5f};
  CHECK(braking.inertial().z > 0.0f);          // objects fly toward the windshield
  CHECK(braking.effectiveGravity().z > 0.0f);  // liquid pushed deeper into the stack
  CHECK_NEAR(braking.longitudinalG(), -0.5f, 1e-6f);
  GForce rightTurn; rightTurn.proper = Vec3{0.4f, 1, 0};
  CHECK(rightTurn.inertial().x < 0.0f);        // objects slide left
  CHECK_NEAR(rightTurn.horizontalG(), 0.4f, 1e-6f);
}

// ---- voxel frame -------------------------------------------------------------------------
TEST(voxel_set_get_and_page_layout) {
  VoxelFrame f;
  f.set(5, 9, 3);
  CHECK(f.get(5, 9, 3));
  CHECK(!f.get(5, 8, 3));
  CHECK(!f.get(5, 9, 2));
  // SSD130x layout: row 9 -> page 1, bit 1; column 5.
  CHECK(f.layer(3)[1 * kW + 5] == (1u << 1));
  CHECK(f.countLit() == 1);
  f.set(-1, 0, 0); f.set(0, -1, 0); f.set(0, 0, -1); f.set(kW, 0, 0); f.set(0, kH, 0); f.set(0, 0, kD);
  CHECK(f.countLit() == 1);  // out-of-range ignored
  f.reset(5, 9, 3);
  CHECK(f.countLit() == 0);
}

TEST(voxel_world_mapping_roundtrip) {
  Vec3 w{10.5f, -3.0f, 2.0f * kLayerPitchPx};
  Vec3 v = VoxelFrame::worldToVoxel(w);
  Vec3 back = VoxelFrame::voxelToWorld(v.x, v.y, v.z);
  CHECK_NEAR(back.x, w.x, 1e-4f); CHECK_NEAR(back.y, w.y, 1e-4f); CHECK_NEAR(back.z, w.z, 1e-4f);
  // Origin maps to the centre; +y world is up (smaller row); +z world is a deeper layer.
  Vec3 c = VoxelFrame::worldToVoxel(Vec3{});
  CHECK_NEAR(c.x, kW * 0.5f - 0.5f, 1e-5f);
  CHECK_NEAR(c.y, kH * 0.5f - 0.5f, 1e-5f);
  CHECK_NEAR(c.z, (kD - 1) * 0.5f, 1e-5f);
  CHECK(VoxelFrame::worldToVoxel(Vec3{0, 5, 0}).y < c.y);
  CHECK(VoxelFrame::worldToVoxel(Vec3{0, 0, kLayerPitchPx}).z > c.z);
  CHECK_NEAR(VoxelFrame::layerZ(0) + VoxelFrame::layerZ(kD - 1), 0.0f, 1e-5f);
}

TEST(voxel_primitives) {
  VoxelFrame f;
  f.hline(2, 10, 20, 7);
  CHECK(f.countLitLayer(2) == 11);
  f.clear();
  f.disc(0, 63.5f, 31.5f, 5.0f);
  const int n = f.countLit();
  CHECK(n > 60 && n < 100);  // ~ pi r^2 = 78
  f.clear();
  f.sphere(Vec3{0, 0, 0}, 6.0f, 1.3f * kLayerPitchPx);
  // Centre layers lit, outer layers not.
  const int mid = (kD - 1) / 2;
  CHECK(f.countLitLayer(mid) > 0);
  CHECK(f.countLitLayer(mid + 1) > 0);
  CHECK(f.countLitLayer(0) == 0);
  CHECK(f.countLitLayer(kD - 1) == 0);
  f.clear();
  CHECK(f.text(0, 2, 2, "1.23G") == 20);
  CHECK(f.countLit() > 20);
  f.clear();
  f.fillRect(1, 0, 0, 7, 7, 0.5f);
  const int half = f.countLitLayer(1);
  CHECK(half >= 28 && half <= 36);  // ordered dither of 64 pixels at 50%
  f.clear();
  f.line3(Vec3{-60, -30, VoxelFrame::layerZ(0)}, Vec3{60, 30, VoxelFrame::layerZ(kD - 1)});
  for (int z = 0; z < kD; ++z) CHECK(f.countLitLayer(z) > 0);
}

TEST(voxel_crossfade) {
  VoxelFrame a, b, out;
  a.fillRect(0, 0, 0, kW - 1, kH - 1);
  out.crossfade(a, b, 0.0f);
  CHECK(out.countLit() == kW * kH);
  out.crossfade(a, b, 1.0f);
  CHECK(out.countLit() == 0);
  out.crossfade(a, b, 0.5f);
  const int n = out.countLit();
  CHECK(n > kW * kH * 0.45f && n < kW * kH * 0.55f);
}

// ---- estimator ------------------------------------------------------------------------------
TEST(estimator_calibration_recovers_car_frame) {
  // Sensor mounted with +Z pointing forward, +Y up-ish but tilted 20 deg back
  // and yawed 10 deg; nominal forward hint says sensor +Z.
  const float tilt = 20.0f * kPi / 180.0f, yaw = 10.0f * kPi / 180.0f;
  // Build the true sensor->car rotation: rows = car right/up/forward in sensor coords.
  Vec3 up{0.0f, std::cos(tilt), std::sin(tilt)};
  Vec3 fwdHint{std::sin(yaw), 0.0f, std::cos(yaw)};
  Calibration truth = GForceEstimator::makeCalibration(up, fwdHint);
  CHECK(truth.valid);

  GForceEstimator est;
  est.setNominalForward(Vec3{0, 0, 1});
  est.setFilter(4.0f, 200.0f);
  Rng rng(1);
  GForce rest;
  est.beginRestCalibration();
  for (int i = 0; i < 200; ++i) est.feedRestSample(simulateRawSensor(rest, truth.sensorToCar, rng, 0.01f));
  CHECK(est.finishRestCalibration());

  // Feed a right turn + braking and confirm the filtered output converges.
  GForce g; g.proper = Vec3{0.3f, 1.0f, -0.4f};
  GForce out;
  for (int i = 0; i < 600; ++i) out = est.update(simulateRawSensor(g, truth.sensorToCar, rng, 0.0f));
  // The 10 deg yaw error of the hint leaks ~0.4*sin(10deg) = 0.07 g between axes.
  CHECK_NEAR(out.proper.x, 0.3f, 0.08f);
  CHECK_NEAR(out.proper.y, 1.0f, 0.01f);
  CHECK_NEAR(out.proper.z, -0.4f, 0.08f);

  // Braking calibration removes the yaw error of the hint entirely.
  GForce brake; brake.proper = Vec3{0.0f, 1.0f, -0.5f};
  Vec3 meanBrake;
  for (int i = 0; i < 100; ++i) meanBrake += simulateRawSensor(brake, truth.sensorToCar, rng, 0.0f);
  meanBrake = meanBrake / 100.0f;
  CHECK(est.calibrateForwardFromBraking(meanBrake));
  for (int i = 0; i < 600; ++i) out = est.update(simulateRawSensor(g, truth.sensorToCar, rng, 0.0f));
  CHECK_NEAR(out.proper.x, 0.3f, 0.005f);
  CHECK_NEAR(out.proper.z, -0.4f, 0.005f);
}

TEST(estimator_rejects_bad_rest) {
  GForceEstimator est;
  est.beginRestCalibration();
  for (int i = 0; i < 20; ++i) est.feedRestSample(Vec3{0, 0.2f, 0});  // not 1 g
  CHECK(!est.finishRestCalibration());
  est.beginRestCalibration();
  for (int i = 0; i < 3; ++i) est.feedRestSample(Vec3{0, 1, 0});  // too few
  CHECK(!est.finishRestCalibration());
}

// ---- scenario -------------------------------------------------------------------------------
TEST(scenario_presets) {
  CHECK(Scenario::presetCount() >= 6);
  CHECK(Scenario::find("drive") != nullptr);
  CHECK(Scenario::find("nope") == nullptr);
  const Scenario& brake = *Scenario::find("brake");
  CHECK(brake.duration() > 5.0f);
  CHECK(brake.dynamicAt(2.0f).z < -0.5f);
  CHECK_NEAR(brake.dynamicAt(0.0f).length(), 0.0f, 1e-6f);
  // Continuity.
  float maxJump = 0.0f;
  Vec3 prev = brake.dynamicAt(0.0f);
  for (float t = 0.0f; t < brake.duration(); t += 0.01f) {
    Vec3 cur = brake.dynamicAt(t);
    maxJump = std::max(maxJump, (cur - prev).length());
    prev = cur;
  }
  CHECK(maxJump < 0.05f);
}

// ---- effects ------------------------------------------------------------------------------------
TEST(gball_tracks_g) {
  GBallEffect ball;
  GForce braking; braking.proper = Vec3{0, 1, -0.5f};
  VoxelFrame f = runEffect(ball, braking, 3.0f);
  CHECK(ball.position().z > 0.0f);  // deeper into the stack
  int n = 0;
  Vec3 c = centroid(f, &n);
  CHECK(n > 0);
  // Most lit voxels (ball + trail) should sit behind the middle layer; HUD on layer 0 pulls it forward a bit.
  int deep = 0, shallow = 0;
  for (int z = 0; z < kD; ++z) (z > (kD - 1) / 2 ? deep : shallow) += f.countLitLayer(z) - (z == 0 ? f.countLitLayer(0) : 0);
  CHECK(deep > shallow);
  GForce rightTurn; rightTurn.proper = Vec3{0.5f, 1, 0};
  runEffect(ball, rightTurn, 3.0f);
  CHECK(ball.position().x < -20.0f);  // thrown left
  (void)c;
}

TEST(slosh_conserves_volume_and_tilts) {
  SloshEffect slosh;
  slosh.params().set("slope_gain", 1.0f);  // physical tilt for this check
  slosh.activate();
  const float v0 = slosh.totalVolume();
  GForce rightTurn; rightTurn.proper = Vec3{0.3f, 1.0f, 0.0f};
  EffectContext ctx; ctx.g = rightTurn; ctx.dt = 1.0f / 60.0f;
  for (int i = 0; i < 60 * 8; ++i) slosh.update(ctx);
  CHECK_NEAR(slosh.totalVolume(), v0, v0 * 1e-3f);
  CHECK(slosh.lastSubsteps() <= 4);
  // Equilibrium slope dh/dx = a_x / g_v = -0.3 (liquid piles up on the left).
  const float left = slosh.height(2, 0), right = slosh.height(SloshEffect::NX - 3, 0);
  CHECK(left > right);
  const float slope = (right - left) / ((SloshEffect::NX - 5) * SloshEffect::DX);
  CHECK_NEAR(slope, -0.3f, 0.05f);
  // Longitudinal: braking pushes liquid toward the windshield (deeper layers).
  slosh.reset();
  GForce braking; braking.proper = Vec3{0, 1.0f, -0.3f};
  ctx.g = braking;
  for (int i = 0; i < 60 * 8; ++i) slosh.update(ctx);
  CHECK(slosh.height(SloshEffect::NX / 2, SloshEffect::NZ - 1) > slosh.height(SloshEffect::NX / 2, 0) + 5.0f);
  VoxelFrame f; slosh.render(f);
  for (int z = 0; z < kD; ++z) CHECK(f.countLitLayer(z) >= kW);  // a surface line on every layer
}

TEST(slosh_stable_under_noisy_drive) {
  SloshEffect slosh;
  slosh.activate();
  const Scenario& drive = *Scenario::find("drive");
  Rng rng(5);
  EffectContext ctx; ctx.dt = 1.0f / 30.0f;
  bool ok = true;
  for (float t = 0.0f; t < drive.duration(); t += ctx.dt) {
    ctx.g = drive.at(t, &rng);
    slosh.update(ctx);
    for (int z = 0; z < SloshEffect::NZ && ok; ++z)
      for (int x = 0; x < SloshEffect::NX; ++x)
        if (!std::isfinite(slosh.height(x, z)) || slosh.height(x, z) < 0.0f || slosh.height(x, z) > kH * 2.0f) { ok = false; break; }
  }
  CHECK(ok);
  CHECK(slosh.lastSubsteps() <= 8);
}

TEST(particles_settle_and_respond) {
  ParticleEffect p;
  GForce rest;
  VoxelFrame f = runEffect(p, rest, 4.0f);
  bool inBounds = true;
  Vec3 mean;
  const Vec3 he = VoxelFrame::halfExtent();
  for (int i = 0; i < p.activeCount(); ++i) {
    const Vec3& x = p.position(i);
    if (!finite(x) || std::fabs(x.x) > he.x || std::fabs(x.y) > he.y || std::fabs(x.z) > he.z) inBounds = false;
    mean += x;
  }
  mean = mean / (float)p.activeCount();
  CHECK(inBounds);
  CHECK(mean.y < -10.0f);                 // settled toward the floor
  CHECK(f.countLit() > p.activeCount() / 2);
  // Pile has some height (pressure keeps it from flattening onto one row).
  int rows = 0;
  for (int y = 0; y < kH; ++y) { bool any = false; for (int z = 0; z < kD && !any; ++z) for (int x = 0; x < kW; ++x) if (f.get(x, y, z)) { any = true; break; } rows += any; }
  CHECK(rows >= 4);
  GForce rightTurn; rightTurn.proper = Vec3{0.5f, 1, 0};
  runEffect(p, rightTurn, 3.0f);
  mean = Vec3{};
  for (int i = 0; i < p.activeCount(); ++i) mean += p.position(i);
  mean = mean / (float)p.activeCount();
  CHECK(mean.x < -15.0f);  // thrown left
}

TEST(smoke_runs_and_leans) {
  SmokeEffect s;
  CHECK(!s.allocated());
  s.activate();
  CHECK(s.allocated());
  GForce rest;
  VoxelFrame f = runEffect(s, rest, 2.0f, 30.0f);
  CHECK(s.totalDensity() > 0.0f);
  CHECK(std::isfinite(s.totalDensity()));
  CHECK(f.countLit() > 0);
  int n = 0; Vec3 cRest = centroid(f, &n);
  GForce braking; braking.proper = Vec3{0, 1, -0.6f};
  f = runEffect(s, braking, 2.0f, 30.0f);
  Vec3 cBrake = centroid(f, &n);
  CHECK(cBrake.z > cRest.z + 0.5f);  // plume pushed toward deeper layers
  s.deactivate();
  CHECK(!s.allocated());
}

TEST(tunnel_and_sprite_render) {
  TunnelEffect t;
  GForce rest;
  VoxelFrame f = runEffect(t, rest, 1.0f);
  for (int z = 0; z < kD; ++z) CHECK(f.countLitLayer(z) > 0);
  SpriteEffect sp(demoSprite());
  CHECK(demoSprite().frames == 12);
  f = runEffect(sp, rest, 1.0f);
  CHECK(f.countLit() > 10);
  GForce braking; braking.proper = Vec3{0, 1, -0.5f};
  runEffect(sp, braking, 3.0f);
  CHECK(sp.position().z > 10.0f);
}

TEST(engine_switching_and_params) {
  GBallEffect ball; SloshEffect slosh; TunnelEffect tunnel;
  AnimationEffect anim(demoAnimation());
  CHECK(anim.params().count() <= ParamTable::kMax);
  Engine eng;
  CHECK(eng.addEffect(&ball) && eng.addEffect(&slosh) && eng.addEffect(&tunnel));
  CHECK(eng.effectCount() == 3);
  CHECK(eng.findEffect("slosh") == 1);
  GForce g; g.proper = Vec3{0.2f, 1, 0};
  eng.step(g, 1.0f / 60.0f);
  CHECK(eng.frame().countLit() > 0);
  CHECK(eng.setEffect("slosh"));
  int lit = 0;
  for (int i = 0; i < 60; ++i) { eng.step(g, 1.0f / 60.0f); lit += eng.frame().countLit(); }
  CHECK(lit > 0);
  CHECK(eng.currentIndex() == 1);
  CHECK(eng.setParam("fill", 0.3f));
  float v = 0.0f;
  CHECK(eng.current()->params().get("fill", v));
  CHECK_NEAR(v, 0.3f, 1e-6f);
  CHECK(!eng.setParam("nonexistent", 1.0f));
  eng.setParam("fill", 99.0f);  // clamped
  eng.current()->params().get("fill", v);
  CHECK(v <= 0.8f);
  eng.nextEffect();
  CHECK(eng.currentIndex() == 2);
  eng.setAutoCycle(0.5f);
  for (int i = 0; i < 40; ++i) eng.step(g, 1.0f / 60.0f);
  CHECK(eng.currentIndex() == 0);
}

// ---- authored animation -----------------------------------------------------------------------
namespace {
GForce dyn(float lat, float vert, float lon) { GForce g; g.proper = GForce::kRestProper() + Vec3{lat, vert, lon}; return g; }
void drive(AnimationEffect& a, const GForce& g, float seconds) {
  EffectContext ctx; ctx.g = g; ctx.dt = 1.0f / 60.0f;
  for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i) a.update(ctx);
}
}  // namespace

TEST(anim_demo_asset_and_states) {
  const AnimationAsset& demo = demoAnimation();
  CHECK(demo.valid());
  CHECK(demo.width == 32 && demo.height == 24 && demo.depth == 3 && demo.frames == 32);
  AnimationEffect a(demo);
  a.activate();
  drive(a, dyn(0, 0, 0), 1.0f);
  CHECK(a.state() == kAnimIdle);
  CHECK(a.currentFrame() >= 0 && a.currentFrame() < 8);
  drive(a, dyn(0, 0, 0.5f), 1.0f);
  CHECK(a.state() == kAnimAccelerate);
  CHECK(a.currentFrame() >= 8 && a.currentFrame() < 14);
  drive(a, dyn(0, 0, -0.5f), 1.0f);
  CHECK(a.state() == kAnimBrake);
  CHECK(a.position().z > 5.0f);  // thrown toward the windshield under braking
  drive(a, dyn(0.5f, 0, 0), 1.0f);
  CHECK(a.state() == kAnimRight);
  CHECK(!a.mirrored());
  CHECK(a.position().x < -5.0f);  // thrown left in a right turn
  drive(a, dyn(-0.5f, 0, 0), 1.0f);
  CHECK(a.state() == kAnimLeft);
  CHECK(a.mirrored());             // demo has no left clip: right is mirrored
  CHECK(a.currentFrame() >= 20 && a.currentFrame() < 26);
  // Hysteresis: a dip to 70 % of the entry threshold keeps the state.
  drive(a, dyn(-0.18f, 0, 0), 0.5f);
  CHECK(a.state() == kAnimLeft);
  drive(a, dyn(0, 0, 0), 1.0f);
  CHECK(a.state() == kAnimIdle);
  // A vertical spike fires the one-shot bump clip, which then hands back.
  EffectContext ctx; ctx.dt = 1.0f / 60.0f;
  ctx.g = dyn(0, 0.6f, 0); a.update(ctx);
  CHECK(a.state() == kAnimBump);
  ctx.g = dyn(0, 0, 0);
  for (int i = 0; i < 60; ++i) a.update(ctx);
  CHECK(a.state() == kAnimIdle);
  // Forced state for previews.
  a.forceState(kAnimBrake);
  drive(a, dyn(0.8f, 0, 0), 0.5f);
  CHECK(a.state() == kAnimBrake);
  a.forceState(-1);
  drive(a, dyn(0.8f, 0, 0), 1.0f);
  CHECK(a.state() == kAnimRight);
}

TEST(anim_render_slices_and_bounds) {
  AnimationEffect a(demoAnimation());
  a.activate();
  drive(a, dyn(0, 0, 0), 0.2f);
  VoxelFrame f;
  a.render(f);
  int layersLit = 0;
  for (int z = 0; z < kD; ++z) layersLit += f.countLitLayer(z) > 0;
  CHECK(layersLit == 3);  // three slices on three consecutive panels
  // Extreme g never pushes slices outside the stack or the panel.
  drive(a, dyn(3.0f, 3.0f, -3.0f), 2.0f);
  f.clear();
  a.render(f);
  CHECK(f.countLit() > 20);
}

TEST(anim_studio_export_compiles_and_plays) {
  CHECK(kUserAnimation.valid());
  CHECK(kUserAnimation.totalBytes() == static_cast<int>(sizeof(kUserAnimationBits)));
  for (int s = 0; s < kAnimStateCount; ++s)
    CHECK(kUserAnimation.clips[s].count == 0 || kUserAnimation.clips[s].start < kUserAnimation.frames);
  AnimationEffect a(kUserAnimation);
  a.activate();
  float v = 0.0f;
  CHECK(a.params().get("enter_g", v) && std::fabs(v - kUserAnimation.mapping.enterG) < 1e-6f);
  drive(a, dyn(0, 0, -0.6f), 1.0f);
  CHECK(a.state() == kAnimBrake);
  VoxelFrame f;
  a.render(f);
  CHECK(f.countLit() > 0);
}

TEST(anim_degenerate_assets) {
  // Empty idle clip: plays all frames. Clip past the end: clamped.
  static const uint8_t bits[2 * 1 * 8] = {0xFF, 0, 0, 0, 0, 0, 0, 0, 0x81, 0, 0, 0, 0, 0, 0, 0};
  AnimationAsset asset;
  asset.name = "tiny"; asset.width = 8; asset.height = 8; asset.depth = 1; asset.frames = 2; asset.bits = bits;
  asset.clips[kAnimBrake] = AnimClip{1, 50, 10.0f, 1};
  AnimationEffect a(asset);
  a.activate();
  bool mirror = false;
  const AnimClip idle = a.resolveClip(kAnimIdle, mirror);
  CHECK(idle.start == 0 && idle.count == 2);
  const AnimClip brake = a.resolveClip(kAnimBrake, mirror);
  CHECK(brake.start == 1 && brake.count == 1);
  const AnimClip left = a.resolveClip(kAnimLeft, mirror);
  CHECK(left.count == 2 && !mirror);
  drive(a, dyn(0, 0, -0.6f), 1.0f);
  CHECK(a.currentFrame() == 1);
  AnimationAsset none;
  AnimationEffect b(none);
  b.activate();
  drive(b, dyn(0.5f, 0, 0), 0.5f);
  VoxelFrame f;
  b.render(f);
  CHECK(f.countLit() == 0);
}

namespace {
// Lowest and highest panel lit by the effect after driving it with `g`.
void litLayers(AnimationEffect& a, const GForce& g, float seconds, int& lo, int& hi) {
  drive(a, g, seconds);
  VoxelFrame f;
  a.render(f);
  lo = kD; hi = -1;
  for (int z = 0; z < kD; ++z)
    if (f.countLitLayer(z) > 0) { lo = std::min(lo, z); hi = std::max(hi, z); }
}
}  // namespace

TEST(anim_even_slice_counts_reach_both_ends_of_the_stack) {
  // All-lit 8x8 art. Before the fix, even slice counts sat half a layer too deep
  // (depth 2 never reached panel 0, depth 6 did not move under braking).
  static uint8_t bits[8 * 8 * 8];
  std::memset(bits, 0xFF, sizeof(bits));
  for (int depth : {2, 4, 6}) {
    if (depth > kD) continue;
    AnimationAsset asset;
    asset.name = "block"; asset.width = 8; asset.height = 8;
    asset.depth = static_cast<uint16_t>(depth); asset.frames = 1; asset.bits = bits;
    AnimationEffect a(asset);
    a.activate();
    int lo = 0, hi = 0;
    litLayers(a, dyn(0, 0, 0), 2.0f, lo, hi);
    CHECK(hi - lo == depth - 1);
    CHECK(lo == (kD - depth) / 2);
    litLayers(a, dyn(0, 0, -3.0f), 3.0f, lo, hi);  // braking: thrown to the back
    CHECK(hi == kD - 1 && lo == kD - depth);
    litLayers(a, dyn(0, 0, 3.0f), 3.0f, lo, hi);   // accelerating: pressed to the front
    CHECK(lo == 0 && hi == depth - 1);
  }
}

TEST(anim_hysteresis_keeps_state_when_stronger_axis_cannot_enter) {
  AnimationEffect a(demoAnimation());
  a.activate();
  a.params().set("enter_g", 0.5f);
  drive(a, dyn(0, 0, -0.6f), 1.0f);
  CHECK(a.state() == kAnimBrake);
  // Trail braking: lateral 0.45 g is stronger but below enter_g; braking 0.35 g
  // is above the 60 % exit level, so the state must stay brake (was idle).
  drive(a, dyn(0.45f, 0, -0.35f), 1.0f);
  CHECK(a.state() == kAnimBrake);
  // Once the corner itself can enter, it takes over.
  drive(a, dyn(0.6f, 0, -0.35f), 1.0f);
  CHECK(a.state() == kAnimRight);

  GStateTracker t;
  t.enterG = 0.5f;
  t.holdS = 0.0f;
  for (int i = 0; i < 10; ++i) t.update(dyn(0, 0, -0.6f), 0.1f);
  CHECK(t.state() == DriveState::Brake);
  for (int i = 0; i < 10; ++i) t.update(dyn(0.45f, 0, -0.35f), 0.1f);
  CHECK(t.state() == DriveState::Brake);
  for (int i = 0; i < 10; ++i) t.update(dyn(0.6f, 0, -0.35f), 0.1f);
  CHECK(t.state() == DriveState::Right);
}

TEST(anim_bump_clip_past_the_end_never_fires) {
  static const uint8_t bits[2 * 8] = {0xFF, 0, 0, 0, 0, 0, 0, 0, 0x81, 0, 0, 0, 0, 0, 0, 0};
  AnimationAsset asset;
  asset.name = "tiny"; asset.width = 8; asset.height = 8; asset.depth = 1; asset.frames = 2; asset.bits = bits;
  asset.clips[kAnimBump] = AnimClip{2, 4, 10.0f, 0};  // starts one past the last frame
  AnimationEffect a(asset);
  a.activate();
  drive(a, dyn(0, 0, 0), 0.5f);
  EffectContext ctx; ctx.dt = 1.0f / 60.0f;
  ctx.g = dyn(0, 0.9f, 0); a.update(ctx);
  CHECK(a.state() == kAnimIdle);
  // And corners still work straight after the spike.
  drive(a, dyn(0.6f, 0, 0), 1.0f);
  CHECK(a.state() == kAnimRight);
}

TEST(anim_states_sharing_the_idle_clip_do_not_restart_it) {
  // Idle-only asset: accelerate falls back to idle and must carry on from the
  // frame already showing rather than jumping back to frame 0.
  static uint8_t bits[8 * 8];
  for (int i = 0; i < 8; ++i) bits[i * 8] = static_cast<uint8_t>(1u << i);
  AnimationAsset asset;
  asset.name = "idle"; asset.width = 8; asset.height = 8; asset.depth = 1; asset.frames = 8; asset.bits = bits;
  asset.clips[kAnimIdle] = AnimClip{0, 8, 4.0f, 1};
  asset.mapping.fpsPerG = 0.0f;
  asset.mapping.holdS = 0.0f;
  AnimationEffect a(asset);
  a.activate();
  drive(a, dyn(0, 0, 0), 0.55f);  // about frame 2 at 4 fps
  const int before = a.currentFrame();
  CHECK(before >= 1);
  EffectContext ctx; ctx.dt = 1.0f / 60.0f;
  ctx.g = dyn(0, 0, 0.6f);
  a.update(ctx);
  CHECK(a.state() == kAnimAccelerate);
  CHECK(a.currentFrame() >= before);
}

TEST(jp_ascii_knockout_clears_exactly_the_glyph) {
  // Reverse video over ASCII: the knock-out must clear the same pixels the
  // 3x5 font sets, without a scratch frame on the stack.
  VoxelFrame lit, ref;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < 40; ++x) lit.set(x, y, 2);
  const int x0 = 3, y0 = 4;
  drawJp(lit, kJpSmall, 2, x0, y0, "A7%", 1, -1.0f);
  // Where text() draws the same string (each ASCII advance is 4 + spacing 1).
  const int top = y0 + (kJpSmall.height - 5) / 2;
  const char* chars = "A7%";
  for (int i = 0; i < 3; ++i) { const char one[2] = {chars[i], '\0'}; ref.text(2, x0 + i * 5, top, one); }
  int mismatches = 0, cleared = 0;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < 40; ++x) {
      const bool shouldClear = ref.get(x, y, 2);
      cleared += shouldClear;
      if (lit.get(x, y, 2) == shouldClear) ++mismatches;
    }
  CHECK(cleared > 10);
  CHECK(mismatches == 0);
}

TEST(voxel_frame_shift_moves_every_layer) {
  VoxelFrame a;
  a.set(0, 0, 0); a.set(10, 7, 3); a.set(kW - 1, kH - 1, kD - 1); a.set(5, 8, 1); a.set(64, 31, 2);
  for (int dy = -9; dy <= 9; dy += 3)
    for (int dx = -2; dx <= 2; ++dx) {
      VoxelFrame b = a;
      b.shift(dx, dy);
      int bad = 0;
      for (int z = 0; z < kD; ++z)
        for (int y = 0; y < kH; ++y)
          for (int x = 0; x < kW; ++x)
            if (b.get(x, y, z) != a.get(x - dx, y - dy, z)) ++bad;
      CHECK(bad == 0);
    }
  VoxelFrame c = a;
  c.shift(kW, 0);
  CHECK(c.countLit() == 0);
}

TEST(hud_orbits_one_pixel_and_keeps_the_frame_on_the_glass) {
  HudEffect hud;
  Engine e;
  e.addEffect(&hud);
  e.setStatusLabel("DEMO");
  hud.params().set("orbit_min", 0.05f);  // 3 s per step for the test
  EffectContext ctx; ctx.dt = 1.0f / 60.0f;
  ctx.g.proper = GForce::kRestProper();
  auto runFor = [&](float secs) {
    for (int i = 0; i < static_cast<int>(secs * 60.0f + 0.5f); ++i) e.step(ctx.g, 1.0f / 60.0f);
  };
  VoxelFrame tag;  // the DEMO tag where the engine draws it with no offset
  tag.text(0, kW - 2 - (4 * 4 - 1), kH - 6, "DEMO");
  const int seq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  runFor(0.1f);
  for (int step = 0; step < 8; ++step) {
    CHECK(hud.orbitX() == seq[step & 3][0] && hud.orbitY() == seq[step & 3][1]);
    const VoxelFrame& f = e.frame();
    const int dx = hud.orbitX(), dy = hud.orbitY();
    // The border's top edge (front panel) sits on row orbitY and its left edge
    // on column orbitX; the right and bottom edges are still on the glass.
    CHECK(f.get(kW / 2, dy, 0));
    CHECK(f.get(dx, kH / 2, 0));
    CHECK(f.get(kW - 2 + dx, kH / 2, 0));
    CHECK(f.get(kW / 2, kH - 2 + dy, 0));
    // The engine's tag moves with the picture.
    int want = 0, match = 0;
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x)
        if (tag.get(x, y, 0)) { ++want; match += (x + dx < kW && y + dy < kH && f.get(x + dx, y + dy, 0)); }
    CHECK(want > 10 && match == want);
    runFor(3.05f);
  }
  // Re-activation keeps the orbit's place; setOrbitStep picks it.
  hud.params().set("orbit_min", 30.0f);
  hud.setOrbitStep(2);
  hud.activate();
  CHECK(hud.orbitX() == 1 && hud.orbitY() == 1);
  hud.setOrbitStep(7);
  CHECK(hud.orbitX() == 0 && hud.orbitY() == 1);
  hud.params().set("orbit_min", 0.0f);
  runFor(0.1f);
  CHECK(hud.orbitX() == 0 && hud.orbitY() == 0);
  // Eight days switched on (1 s steps): the warning blink still toggles,
  // because its clock wraps instead of running out of float precision.
  hud.params().set("warn_g", 0.1f);
  ctx.g.proper = GForce::kRestProper() + Vec3{0.5f, 0, 0};
  ctx.dt = 1.0f;
  for (int i = 0; i < 8 * 24 * 3600; ++i) hud.update(ctx);
  ctx.dt = 1.0f / 60.0f;
  int onFrames = 0, offFrames = 0;
  for (int i = 0; i < 60; ++i) {
    hud.update(ctx);
    VoxelFrame f;
    hud.render(f);
    int lit = 0;  // status-column pixels on the right
    for (int y = 18; y < kH - 10; ++y)
      for (int x = kW - 22; x < kW - 4; ++x) lit += f.get(x, y, 0);
    (lit > 20 ? onFrames : offFrames)++;
  }
  CHECK(onFrames > 10 && offFrames > 10);
}

TEST(engine_status_label_marks_non_live_data) {
  Engine e;
  HudEffect hud;
  e.addEffect(&hud);
  GForce g;
  g.proper = GForce::kRestProper();
  auto boxLit = [&]() {
    int lit = 0;
    for (int y = kH - 7; y < kH; ++y)
      for (int x = kW - 40; x < kW; ++x) lit += e.frame().get(x, y, 0);
    return lit;
  };
  e.setStatusLabel("DEMO");
  for (int i = 0; i < 120; ++i) e.step(g, 1.0f / 60.0f);
  const int withDemo = boxLit();
  VoxelFrame ref;
  ref.text(0, kW - 2 - (4 * 4 - 1), kH - 6, "DEMO");
  int match = 0, want = 0;
  for (int y = kH - 7; y < kH; ++y)
    for (int x = kW - 40; x < kW; ++x)
      if (ref.get(x, y, 0)) { ++want; match += e.frame().get(x, y, 0); }
  CHECK(want > 10 && match == want);
  (void)withDemo;
  // Without a tag the frame is exactly what an untagged engine draws.
  Engine plain;
  HudEffect hud2;
  plain.addEffect(&hud2);
  for (int i = 0; i < 121; ++i) plain.step(g, 1.0f / 60.0f);
  e.setStatusLabel(nullptr);
  e.step(g, 1.0f / 60.0f);
  CHECK(std::memcmp(e.frame().data(), plain.frame().data(), VoxelFrame::frameBytes()) == 0);
  e.setStatusLabel("NO SENSOR");
  e.step(g, 1.0f / 60.0f);
  CHECK(std::memcmp(e.frame().data(), plain.frame().data(), VoxelFrame::frameBytes()) != 0);
  // The HUD's status column (正常) ends above the tag's box, so the tag never
  // cuts it: nothing in the column's x range on the row above the box or in it,
  // apart from the ruler tick.
  plain.step(g, 1.0f / 60.0f);
  const int colX = kW - 6 - kJpSmall.glyphs[0].width;
  int column = 0, inBox = 0;
  for (int y = 0; y < kH - 8; ++y)
    for (int x = colX; x < colX + kJpSmall.glyphs[0].width; ++x) column += plain.frame().get(x, y, 0);
  for (int y = kH - 8; y < kH - 5; ++y)
    for (int x = colX; x < colX + kJpSmall.glyphs[0].width; ++x) inBox += plain.frame().get(x, y, 0);
  CHECK(column > 40);
  CHECK(inBox == 0);
}

TEST(engine_burn_in_offset_moves_overlays_and_only_the_hud) {
  GForce g;
  g.proper = GForce::kRestProper();
  auto lit = [](const VoxelFrame& f, int x, int y) { return x < kW && y < kH && f.get(x, y, 0); };
  // The mode name (first 1.5 s) and the readout move with the HUD's offset.
  HudEffect hud;
  TunnelEffect tunnel;
  Engine e;
  e.addEffect(&tunnel);
  e.addEffect(&hud);
  e.setReadout(true);
  hud.setOrbitStep(2);  // offset (1, 1)
  e.setEffect(1, false);
  e.step(g, 1.0f / 60.0f);
  VoxelFrame ref;
  ref.text(0, 1, 1, "HUD");
  ref.text(0, 1, kH - 6, "0.00G");
  int want = 0, match = 0;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if (ref.get(x, y, 0)) { ++want; match += lit(e.frame(), x + 1, y + 1); }
  CHECK(want > 20 && match == want);
  // Crossfading away from the HUD keeps its picture at its own offset: the
  // border's left edge stays on column 1, not 0.
  e.setEffect(0, true);
  e.step(g, 1.0f / 60.0f);
  int col0 = 0, col1 = 0;
  for (int y = 12; y < kH - 12; ++y) { col0 += lit(e.frame(), 0, y); col1 += lit(e.frame(), 1, y); }
  CHECK(col1 > (kH - 24) / 2 && col0 < col1 / 4);
  // Every other effect leaves the picture where it is.
  GBallEffect ball;
  SloshEffect slosh;
  ParticleEffect particles;
  SmokeEffect smoke;
  SpriteEffect sprite(demoSprite());
  AnimationEffect anim(demoAnimation());
  AtFieldEffect atfield;
  SyncEffect sync;
  const Effect* others[] = {&ball, &slosh, &particles, &smoke, &tunnel, &sprite, &anim, &atfield, &sync};
  for (const Effect* o : others) {
    int dx = 7, dy = 7;
    o->burnInOffset(dx, dy);
    CHECK(dx == 0 && dy == 0);
  }
}

TEST(hud_warning_column_clears_the_hazard_stripes) {
  // 警告 blinks in the status column while hazard stripes scroll below it; one
  // empty row must separate them (on 56-row panels they used to touch).
  HudEffect hud;
  hud.activate();
  hud.params().set("warn_g", 0.1f);
  EffectContext ctx;
  ctx.dt = 1.0f / 60.0f;
  ctx.g.proper = GForce::kRestProper() + Vec3{0.5f, 0, 0};
  const int colX = kW - 6 - kJpSmall.glyphs[0].width;
  int glyphFrames = 0, gapLit = 0;
  for (int i = 0; i < 90; ++i) {
    hud.update(ctx);
    VoxelFrame f;
    hud.render(f);
    int column = 0;
    for (int y = 0; y < kH - 10; ++y)
      for (int x = colX; x < colX + kJpSmall.glyphs[0].width; ++x) column += f.get(x, y, 0);
    if (column > 40) ++glyphFrames;
    for (int x = colX; x < colX + kJpSmall.glyphs[0].width; ++x) gapLit += f.get(x, kH - 10, 0);
  }
  CHECK(glyphFrames > 20);
  CHECK(gapLit == 0);
}

// ---- review regressions --------------------------------------------------------------------
TEST(calibration_matches_a_physical_right_handed_sensor) {
  // A GY-521 lying flat: chip +Z up, chip +Y toward the windshield, so chip +X
  // (= Y x Z for a right-handed sensor) points to the car's right. These raw
  // vectors are written from physics, not produced by makeCalibration.
  GForceEstimator est;
  est.setNominalForward(Vec3{0, 1, 0});
  est.setFilter(0.0f, 200.0f);  // pass-through for exact numbers
  est.beginRestCalibration();
  for (int i = 0; i < 50; ++i) est.feedRestSample(Vec3{0, 0, 1});
  CHECK(est.finishRestCalibration());
  CHECK(est.forwardTrusted());
  GForce g;
  for (int i = 0; i < 5; ++i) g = est.update(Vec3{0.5f, 0, 1});   // right turn: pushed toward +X
  CHECK_NEAR(g.lateralG(), 0.5f, 1e-4f);
  CHECK_NEAR(g.longitudinalG(), 0.0f, 1e-4f);
  for (int i = 0; i < 5; ++i) g = est.update(Vec3{0, -0.5f, 1});  // braking: pushed toward -Y
  CHECK_NEAR(g.longitudinalG(), -0.5f, 1e-4f);
  CHECK_NEAR(g.lateralG(), 0.0f, 1e-4f);
  CHECK(g.inertial().z > 0.0f);  // loose objects go toward the windshield
  // Upright board facing forward (chip +Z forward, +Y up, so +X = Y x Z = left).
  GForceEstimator up;
  up.setNominalForward(Vec3{0, 0, 1});
  up.setFilter(0.0f, 200.0f);
  up.beginRestCalibration();
  for (int i = 0; i < 50; ++i) up.feedRestSample(Vec3{0, 1, 0});
  CHECK(up.finishRestCalibration());
  for (int i = 0; i < 5; ++i) g = up.update(Vec3{-0.4f, 1, 0});  // right turn: toward the car's right = chip -X
  CHECK_NEAR(g.lateralG(), 0.4f, 1e-4f);
}

TEST(calibration_vertical_hint_needs_braking) {
  GForceEstimator est;
  est.setNominalForward(Vec3{0, 0, 1});  // default hint, but the board lies flat (+Z up)
  est.setFilter(0.0f, 200.0f);
  est.beginRestCalibration();
  for (int i = 0; i < 50; ++i) est.feedRestSample(Vec3{0.02f, 0.01f, 1.0f});
  CHECK(est.finishRestCalibration());  // up is known...
  CHECK(!est.forwardTrusted());        // ...but forward is not
  CHECK(!GForceEstimator::makeCalibration(Vec3{0, 0, 1}, Vec3{0.3f, 0, 1}).valid);  // 17 deg from vertical
  // A firm straight brake: chip +Y toward the windshield, so braking pushes -Y.
  Vec3 brake{0.0f, -0.5f, 1.0f};
  CHECK(est.calibrateForwardFromBraking(brake));
  CHECK(est.forwardTrusted());
  GForce g;
  for (int i = 0; i < 5; ++i) g = est.update(Vec3{0.5f, 0, 1});
  CHECK_NEAR(g.lateralG(), 0.5f, 0.03f);
  // A rejected rest capture keeps the previous calibration.
  const Mat3 before = est.calibration().sensorToCar;
  est.beginRestCalibration();
  for (int i = 0; i < 50; ++i) est.feedRestSample(Vec3{0.5f, 1.3f, 0.2f});
  CHECK(!est.finishRestCalibration());
  CHECK_NEAR(est.calibration().sensorToCar.m[2][1], before.m[2][1], 1e-6f);
}

TEST(params_and_clamp_reject_nan) {
  CHECK_NEAR(clampf(std::nanf(""), 1.0f, 2.0f), 1.0f, 0.0f);
  CHECK_NEAR(clampf(5.0f, 1.0f, 2.0f), 2.0f, 0.0f);
  SmokeEffect smoke;
  CHECK(!smoke.params().set("iters", std::nanf("")));
  CHECK(!smoke.params().set("iters", INFINITY));
  float v = 0;
  smoke.params().get("iters", v);
  CHECK(std::isfinite(v));
}

TEST(slosh_stays_stable_when_substeps_are_capped) {
  SloshEffect slosh;
  slosh.params().set("tank_m", 0.1f);      // fast waves
  slosh.params().set("substeps", 2.0f);    // tight cost cap
  slosh.params().set("damping", 0.0f);
  slosh.activate();
  EffectContext ctx; ctx.dt = 1.0f / 15.0f;
  const Scenario& drive = *Scenario::find("slalom");
  float worst = 0.0f;
  for (float t = 0.0f; t < drive.duration(); t += ctx.dt) {
    ctx.g = drive.at(t);
    slosh.update(ctx);
    for (int z = 0; z < SloshEffect::NZ; ++z)
      for (int x = 0; x < SloshEffect::NX; ++x) worst = std::max(worst, slosh.height(x, z));
  }
  CHECK(std::isfinite(worst));
  CHECK(worst < kH * 1.6f);
  // Runtime fill change takes effect.
  SloshEffect s2;
  s2.activate();
  const float v0 = s2.totalVolume();
  s2.params().set("fill", 0.2f);
  EffectContext c2; c2.dt = 1.0f / 60.0f;
  for (int i = 0; i < 180; ++i) s2.update(c2);
  CHECK(s2.totalVolume() < v0 * 0.6f);
}

TEST(voxel_clipping_edges) {
  VoxelFrame f;
  f.hline(0, 200, 300, 5);
  f.hline(0, -50, -10, 5);
  f.fillRect(0, -10, -10, -5, -5);
  f.fillRect(0, 130, 60, 140, 70);
  f.vline(0, 5, -1000000, -10);
  CHECK(f.countLit() == 0);
  f.line(0, -300, 10, 300, 10);
  CHECK(f.countLitLayer(0) == kW);
  f.clear();
  f.vline(0, 3, -100000, 100000);
  CHECK(f.countLit() == kH);
  f.clearBox(0, 0, kW - 1, kH - 1);
  CHECK(f.countLit() == 0);
  // A ball centred exactly between two layers is still drawn.
  f.sphere(Vec3{0, 0, 0}, 5.0f, 0.3f * kLayerPitchPx);
  CHECK(f.countLit() > 0);
}

// ---- NGE-inspired effects and Japanese text -------------------------------------------------
TEST(jp_text_covers_every_string_the_effects_draw) {
  const char* large[] = {"待機", "加速", "制動", "左旋回", "右旋回", "衝撃", "危険", "警告"};
  const char* small[] = {"横G", "縦G", "正常", "警告", "A.T.フィールド", "全開", "横", "縦", "上下", "同期率"};
  auto allPresent = [](const JpFont& font, const char* s) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;) {
      uint32_t cp = *p;
      int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
      if (extra) cp &= (0x3Fu >> extra);
      ++p;
      for (int i = 0; i < extra; ++i) cp = (cp << 6) | (*p++ & 0x3Fu);
      if (cp >= 0x80 && !findGlyph(font, cp)) return false;
    }
    return true;
  };
  for (const char* w : large) { CHECK(allPresent(kJpLarge, w)); CHECK(measureJp(kJpLarge, w, 2) <= kW - 8); }
  for (const char* w : small) CHECK(allPresent(kJpSmall, w));
  CHECK(kJpLarge.height == 24 && kJpSmall.height == 14);
  for (int i = 1; i < kJpLarge.count; ++i) CHECK(kJpLarge.glyphs[i - 1].codepoint < kJpLarge.glyphs[i].codepoint);
  VoxelFrame f;
  const int w = drawJp(f, kJpLarge, 2, 10, 10, "制動", 2);
  CHECK(w == measureJp(kJpLarge, "制動", 2));
  CHECK(f.countLitLayer(2) > 80);
  const int lit = f.countLit();
  f.fillRect(3, 0, 0, kW - 1, kH - 1);
  drawJp(f, kJpLarge, 3, 10, 10, "制動", 2, -1.0f);  // knock-out
  CHECK(f.countLitLayer(3) == kW * kH - lit);
  f.clear();
  CHECK(drawJpVertical(f, kJpSmall, 0, 100, 0, "正常", 1) == 2 * (kJpSmall.height + 1));
}

TEST(hud_follows_g_and_names_the_state) {
  HudEffect hud;
  hud.activate();
  {
    EffectContext ctx; ctx.dt = 1.0f / 60.0f;
    auto run = [&](float lat, float vert, float lon, float secs) {
      ctx.g.proper = GForce::kRestProper() + Vec3{lat, vert, lon};
      for (int i = 0; i < static_cast<int>(secs * 60.0f); ++i) hud.update(ctx);
    };
    run(0, 0, 0, 1.0f);
    CHECK(std::strcmp(hud.titleText(), "待機") == 0);
    run(0, 0, -0.5f, 2.0f);
    CHECK(std::strcmp(hud.titleText(), "制動") == 0);
    CHECK(hud.ballPosition().z > 10.0f);                       // thrown toward the windshield
    CHECK((hud.ballPosition() - hud.tipPosition()).length() < 2.0f);  // ball has caught the line's tip
    run(0.5f, 0, 0, 2.0f);
    CHECK(std::strcmp(hud.titleText(), "右旋回") == 0);
    CHECK(hud.ballPosition().x < -10.0f);
    run(1.2f, 0, 0, 1.0f);
    CHECK(std::strcmp(hud.titleText(), "危険") == 0);
    VoxelFrame f;
    hud.render(f);
    for (int z = 0; z < 3; ++z) CHECK(f.countLitLayer(z) > 0);
    run(0, 0, 0, 2.0f);
    ctx.g.proper = GForce::kRestProper() + Vec3{0, 0.6f, 0};
    hud.update(ctx);
    CHECK(std::strcmp(hud.titleText(), "衝撃") == 0);
  }
}

TEST(atfield_and_sync_react) {
  AtFieldEffect at;
  at.activate();
  EffectContext ctx; ctx.dt = 1.0f / 60.0f;
  ctx.g.proper = GForce::kRestProper() + Vec3{0.6f, 0, 0};
  for (int i = 0; i < 60; ++i) at.update(ctx);
  CHECK(at.activeRipples() >= 2);
  VoxelFrame f;
  at.render(f);
  int layers = 0;
  for (int z = 0; z < kD; ++z) layers += f.countLitLayer(z) > 0;
  CHECK(layers >= 3);  // the wavefront crosses several panels
  ctx.g.proper = GForce::kRestProper();
  for (int i = 0; i < 400; ++i) at.update(ctx);
  CHECK(at.activeRipples() <= 2);  // only the idle heartbeat remains

  SyncEffect sync;
  sync.activate();
  ctx.g.proper = GForce::kRestProper() + Vec3{0.5f, 0, -0.5f};
  for (int i = 0; i < 120; ++i) sync.update(ctx);
  VoxelFrame s;
  sync.render(s);
  CHECK(s.countLitLayer(0) > 50);  // labels
  CHECK(s.countLitLayer(1) > 20 && s.countLitLayer(2) > 20 && s.countLitLayer(3) > 20);  // three traces
}

int main() {
  for (const Test& t : tests()) {
    gCurrent = t.name;
    const int before = gFailures;
    t.fn();
    std::printf("%s %s\n", gFailures == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
  return gFailures;
}
