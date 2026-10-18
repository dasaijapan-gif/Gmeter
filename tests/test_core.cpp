// Self-contained unit tests for the core library. Build with host/CMakeLists.txt
// (target gmeter_tests) and run; exit code is the number of failures.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gmeter/gmeter.hpp"

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
