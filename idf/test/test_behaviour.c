// Behaviour tests of the core: maths, filters, calibration physics, what the
// effects do with g, the engine's overlays and the Japanese text. Each test
// states a property with a tolerance, so these keep meaning when the
// reference digests (test_golden.c) have to be re-recorded.
//
// Core functions used here that the firmware (idf/main) does not call;
// a change that removes one must replace it here:
//   gm_estimator_make_calibration, gm_scenario_dynamic_at, gm_anim_asset_empty,
//   gm_anim_resolve_clip, gm_jp_find_glyph, gm_gball_position, gm_slosh_total_volume,
//   gm_slosh_height, gm_slosh_last_substeps, gm_particles_active_count,
//   gm_particles_position, gm_smoke_total_density, gm_sprite_position, gm_anim_position,
//   gm_hud_title_text, gm_hud_ball_position, gm_hud_tip_position, gm_atfield_active_ripples,
//   gm_frame_reset, gm_frame_hline, gm_frame_vline, gm_frame_line, gm_frame_fill_rect,
//   gm_frame_disc, gm_frame_sphere, gm_frame_line3, gm_frame_crossfade, gm_frame_shift,
//   gm_frame_clear_box, gm_world_to_voxel, gm_layer_z, gm_half_extent,
//   gm_lowpass_configure, gm_lowpass_reset, gm_lowpass_process, gm_spring_step,
//   gm_rng_seed, gm_rng_symmetric, gm_gstate_* (thresholds set through the struct fields)
// Everything else is part of the firmware's API (see test_golden.c).
#include "fixtures/user_animation.h"
#include "test.h"

// ---- helpers ------------------------------------------------------------------------------------
static gm_gforce_t gf(float x, float y, float z) {  // proper acceleration
  gm_gforce_t g;
  g.proper = gm_vec3(x, y, z);
  return g;
}

static gm_gforce_t dyn(float lat, float vert, float lon) {  // rest plus dynamic g
  return gf(lat, 1.0f + vert, lon);
}

static bool finite3(gm_vec3_t v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

// Runs an effect through a constant g for `seconds` (activating it first)
// and renders the final frame into f.
static void run_effect(gm_effect_t* e, gm_gforce_t g, float seconds, float fps, gm_frame_t* f) {
  gm_effect_activate(e);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.g = g;
  ctx.dt = 1.0f / fps;
  const int n = (int)(seconds * fps);
  for (int i = 0; i < n; ++i) gm_effect_update(e, &ctx);
  gm_frame_clear(f);
  gm_effect_render(e, f);
}

// Centre of the lit voxels (voxel coordinates).
static gm_vec3_t centroid(const gm_frame_t* f, int* count) {
  gm_vec3_t sum = gm_vec3_zero();
  int n = 0;
  for (int z = 0; z < GM_D; ++z)
    for (int y = 0; y < GM_H; ++y)
      for (int x = 0; x < GM_W; ++x)
        if (px(f, x, y, z)) {
          sum = gm_vec3_add(sum, gm_vec3((float)x, (float)y, (float)z));
          ++n;
        }
  if (count) *count = n;
  return n ? gm_vec3_div(sum, (float)n) : gm_vec3_zero();
}

// What a sensor mounted with this calibration reads for g, with white noise
// (the transpose of sensor_to_car maps car -> sensor).
static gm_vec3_t sensor_reading(gm_gforce_t g, const gm_mat3_t* m, gm_rng_t* rng, float noise_g) {
  const gm_vec3_t c = g.proper;
  gm_vec3_t s = gm_vec3(m->m[0][0] * c.x + m->m[1][0] * c.y + m->m[2][0] * c.z,
                        m->m[0][1] * c.x + m->m[1][1] * c.y + m->m[2][1] * c.z,
                        m->m[0][2] * c.x + m->m[1][2] * c.y + m->m[2][2] * c.z);
  if (noise_g > 0.0f) {
    const float nx = gm_rng_symmetric(rng);
    const float ny = gm_rng_symmetric(rng);
    const float nz = gm_rng_symmetric(rng);
    s = gm_vec3_add(s, gm_vec3_scale(gm_vec3(nx, ny, nz), noise_g));
  }
  return s;
}

// A scenario's g with road vibration (white noise on every axis).
static gm_gforce_t noisy_scenario(const gm_scenario_t* sc, float t, gm_rng_t* rng, float noise_g) {
  gm_gforce_t g = gm_scenario_at(sc, t, NULL);
  const float nx = gm_rng_symmetric(rng);
  const float ny = gm_rng_symmetric(rng);
  const float nz = gm_rng_symmetric(rng);
  g.proper = gm_vec3_add(g.proper, gm_vec3_scale(gm_vec3(nx, ny, nz), noise_g));
  return g;
}

static gm_frame_t s_f, s_ref, s_tmp;

// ---- maths --------------------------------------------------------------------------------------------
TEST(vec3_basics) {
  const gm_vec3_t a = gm_vec3(1, 2, 3), b = gm_vec3(4, 5, 6);
  CHECK_NEAR(gm_vec3_dot(a, b), 32.0f, 1e-6f);
  const gm_vec3_t c = gm_vec3_cross(a, b);
  CHECK_NEAR(c.x, -3.0f, 1e-6f);
  CHECK_NEAR(c.y, 6.0f, 1e-6f);
  CHECK_NEAR(c.z, -3.0f, 1e-6f);
  CHECK_NEAR(gm_vec3_length(gm_vec3(3, 4, 0)), 5.0f, 1e-6f);
  CHECK_NEAR(gm_vec3_normalized(gm_vec3(0, 0, 2)).z, 1.0f, 1e-6f);
  CHECK_NEAR(gm_vec3_cross(gm_vec3(1, 0, 0), gm_vec3(0, 1, 0)).z, 1.0f, 1e-6f);  // right-handed
}

TEST(biquad_lowpass) {
  gm_lowpass_t lp;
  gm_lowpass_configure(&lp, 4.0f, 200.0f);
  gm_lowpass_reset(&lp, 0.0f);
  // DC gain -> 1.
  float y = 0.0f;
  for (int i = 0; i < 2000; ++i) y = gm_lowpass_process(&lp, 1.0f);
  CHECK_NEAR(y, 1.0f, 1e-3f);
  // 50 Hz tone strongly attenuated.
  gm_lowpass_reset(&lp, 0.0f);
  float peak = 0.0f;
  for (int i = 0; i < 2000; ++i) {
    const float v = gm_lowpass_process(&lp, sinf(GM_TWO_PI * 50.0f * (float)i / 200.0f));
    if (i > 1000) peak = gm_maxf(peak, fabsf(v));
  }
  CHECK(peak < 0.02f);
  // 0.5 Hz tone passes.
  gm_lowpass_reset(&lp, 0.0f);
  peak = 0.0f;
  for (int i = 0; i < 4000; ++i) {
    const float v = gm_lowpass_process(&lp, sinf(GM_TWO_PI * 0.5f * (float)i / 200.0f));
    if (i > 2000) peak = gm_maxf(peak, fabsf(v));
  }
  CHECK(peak > 0.95f);
}

TEST(spring_converges_without_overshoot) {
  gm_spring_t s = {0.0f, 0.0f};
  float maxv = 0.0f;
  for (int i = 0; i < 600; ++i) {
    gm_spring_step(&s, 10.0f, 1.0f / 60.0f, 2.0f, 1.0f);
    maxv = gm_maxf(maxv, s.pos);
  }
  CHECK_NEAR(s.pos, 10.0f, 0.01f);
  CHECK(maxv <= 10.05f);
  // Closed form: stable at a huge w*dt (hz 8, dt 1/15 -> w*dt = 3.35) where Euler diverges.
  gm_spring_t big = {0.0f, 0.0f};
  for (int i = 0; i < 60; ++i) gm_spring_step(&big, 10.0f, 1.0f / 15.0f, 8.0f, 1.0f);
  CHECK_NEAR(big.pos, 10.0f, 0.01f);
  // Under- and over-damped variants converge too.
  gm_spring_t under = {0.0f, 0.0f}, over = {0.0f, 0.0f};
  for (int i = 0; i < 1200; ++i) {
    gm_spring_step(&under, 5.0f, 1.0f / 60.0f, 2.0f, 0.3f);
    gm_spring_step(&over, 5.0f, 1.0f / 60.0f, 2.0f, 1.8f);
  }
  CHECK_NEAR(under.pos, 5.0f, 0.02f);
  CHECK_NEAR(over.pos, 5.0f, 0.05f);
  // Step response of the critically damped spring matches 1 - e^{-wt}(1 + wt).
  gm_spring_t ref = {0.0f, 0.0f};
  for (int i = 0; i < 30; ++i) gm_spring_step(&ref, 1.0f, 1.0f / 60.0f, 1.0f, 1.0f);
  const float wt = GM_TWO_PI * 0.5f;
  CHECK_NEAR(ref.pos, 1.0f - expf(-wt) * (1.0f + wt), 1e-3f);
}

// ---- g-force conventions ---------------------------------------------------------------------------
TEST(gforce_conventions) {
  const gm_gforce_t rest = gm_gforce_rest();
  CHECK_NEAR(gm_vec3_length(gm_gforce_dynamic(rest)), 0.0f, 1e-6f);
  CHECK_NEAR(gm_gforce_effective_gravity(rest).y, -1.0f, 1e-6f);
  const gm_gforce_t braking = gf(0, 1, -0.5f);
  CHECK(gm_gforce_inertial(braking).z > 0.0f);            // objects fly toward the windshield
  CHECK(gm_gforce_effective_gravity(braking).z > 0.0f);   // liquid pushed deeper into the stack
  CHECK_NEAR(gm_gforce_longitudinal(braking), -0.5f, 1e-6f);
  const gm_gforce_t right_turn = gf(0.4f, 1, 0);
  CHECK(gm_gforce_inertial(right_turn).x < 0.0f);  // objects slide left
  CHECK_NEAR(gm_gforce_horizontal(right_turn), 0.4f, 1e-6f);
}

// ---- voxel frame ------------------------------------------------------------------------------------
TEST(voxel_set_get_and_page_layout) {
  gm_frame_t* f = &s_f;
  gm_frame_clear(f);
  gm_frame_set(f, 5, 9, 3);
  CHECK(px(f, 5, 9, 3));
  CHECK(!px(f, 5, 8, 3));
  CHECK(!px(f, 5, 9, 2));
  // SSD130x layout: row 9 -> page 1, bit 1; column 5.
  CHECK(f->bits[3][1 * GM_W + 5] == (1u << 1));
  CHECK(lit(f) == 1);
  gm_frame_set(f, -1, 0, 0);
  gm_frame_set(f, 0, -1, 0);
  gm_frame_set(f, 0, 0, -1);
  gm_frame_set(f, GM_W, 0, 0);
  gm_frame_set(f, 0, GM_H, 0);
  gm_frame_set(f, 0, 0, GM_D);
  CHECK(lit(f) == 1);  // out-of-range ignored
  gm_frame_reset(f, 5, 9, 3);
  CHECK(lit(f) == 0);
}

// The inverse of gm_world_to_voxel, written out here.
static gm_vec3_t world_of_voxel(float x, float y, float z) {
  return gm_vec3(x - GM_W * 0.5f + 0.5f, GM_H * 0.5f - 0.5f - y, (z - (GM_D - 1) * 0.5f) * GM_LAYER_PITCH_PX);
}

TEST(voxel_world_mapping_roundtrip) {
  const gm_vec3_t w = gm_vec3(10.5f, -3.0f, 2.0f * GM_LAYER_PITCH_PX);
  const gm_vec3_t v = gm_world_to_voxel(w);
  const gm_vec3_t back = world_of_voxel(v.x, v.y, v.z);
  CHECK_NEAR(back.x, w.x, 1e-4f);
  CHECK_NEAR(back.y, w.y, 1e-4f);
  CHECK_NEAR(back.z, w.z, 1e-4f);
  // Origin maps to the centre; +y world is up (smaller row); +z world is a deeper layer.
  const gm_vec3_t c = gm_world_to_voxel(gm_vec3_zero());
  CHECK_NEAR(c.x, GM_W * 0.5f - 0.5f, 1e-5f);
  CHECK_NEAR(c.y, GM_H * 0.5f - 0.5f, 1e-5f);
  CHECK_NEAR(c.z, (GM_D - 1) * 0.5f, 1e-5f);
  CHECK(gm_world_to_voxel(gm_vec3(0, 5, 0)).y < c.y);
  CHECK(gm_world_to_voxel(gm_vec3(0, 0, GM_LAYER_PITCH_PX)).z > c.z);
  CHECK_NEAR(gm_layer_z(0) + gm_layer_z(GM_D - 1), 0.0f, 1e-5f);
}

TEST(voxel_primitives) {
  gm_frame_t* f = &s_f;
  gm_frame_clear(f);
  gm_frame_hline(f, 2, 10, 20, 7);
  CHECK(lit_layer(f, 2) == 11);
  gm_frame_clear(f);
  gm_frame_disc(f, 0, 63.5f, 31.5f, 5.0f, 1.0f);
  const int n = lit(f);
  CHECK(n > 60 && n < 100);  // ~ pi r^2 = 78
  gm_frame_clear(f);
  gm_frame_sphere(f, gm_vec3_zero(), 6.0f, 1.3f * GM_LAYER_PITCH_PX, 1.0f);
  // Centre layers lit, outer layers not.
  const int mid = (GM_D - 1) / 2;
  CHECK(lit_layer(f, mid) > 0);
  CHECK(lit_layer(f, mid + 1) > 0);
  CHECK(lit_layer(f, 0) == 0);
  CHECK(lit_layer(f, GM_D - 1) == 0);
  gm_frame_clear(f);
  CHECK(gm_frame_text(f, 0, 2, 2, "1.23G") == 20);
  CHECK(lit(f) > 20);
  gm_frame_clear(f);
  gm_frame_fill_rect(f, 1, 0, 0, 7, 7, 0.5f);
  const int half = lit_layer(f, 1);
  CHECK(half >= 28 && half <= 36);  // ordered dither of 64 pixels at 50%
  gm_frame_clear(f);
  gm_frame_line3(f, gm_vec3(-60, -30, gm_layer_z(0)), gm_vec3(60, 30, gm_layer_z(GM_D - 1)), 1.0f);
  for (int z = 0; z < GM_D; ++z) CHECK(lit_layer(f, z) > 0);
}

TEST(voxel_crossfade) {
  gm_frame_t *a = &s_f, *b = &s_ref, *out = &s_tmp;
  gm_frame_clear(a);
  gm_frame_clear(b);
  gm_frame_clear(out);
  gm_frame_fill_rect(a, 0, 0, 0, GM_W - 1, GM_H - 1, 1.0f);
  gm_frame_crossfade(out, a, b, 0.0f);
  CHECK(lit(out) == GM_W * GM_H);
  gm_frame_crossfade(out, a, b, 1.0f);
  CHECK(lit(out) == 0);
  gm_frame_crossfade(out, a, b, 0.5f);
  const int n = lit(out);
  CHECK(n > GM_W * GM_H * 0.45f && n < GM_W * GM_H * 0.55f);
}

// ---- estimator ------------------------------------------------------------------------------------------
TEST(estimator_calibration_recovers_car_frame) {
  // Sensor mounted with +Z pointing forward, +Y up-ish but tilted 20 deg back
  // and yawed 10 deg; nominal forward hint says sensor +Z.
  const float tilt = 20.0f * GM_PI / 180.0f, yaw = 10.0f * GM_PI / 180.0f;
  // The true sensor->car rotation: rows = car right/up/forward in sensor coords.
  const gm_vec3_t up = gm_vec3(0.0f, cosf(tilt), sinf(tilt));
  const gm_vec3_t fwd_hint = gm_vec3(sinf(yaw), 0.0f, cosf(yaw));
  const gm_calibration_t truth = gm_estimator_make_calibration(up, fwd_hint);
  CHECK(truth.valid);

  static gm_estimator_t est;
  gm_estimator_init(&est);
  gm_estimator_set_nominal_forward(&est, gm_vec3(0, 0, 1));
  gm_estimator_set_filter(&est, 4.0f, 200.0f);
  gm_rng_t rng;
  gm_rng_seed(&rng, 1);
  gm_estimator_begin_rest_calibration(&est);
  for (int i = 0; i < 200; ++i)
    gm_estimator_feed_rest_sample(&est, sensor_reading(gm_gforce_rest(), &truth.sensor_to_car, &rng, 0.01f));
  CHECK(gm_estimator_finish_rest_calibration(&est));

  // Feed a right turn + braking and confirm the filtered output converges.
  const gm_gforce_t g = gf(0.3f, 1.0f, -0.4f);
  gm_gforce_t out = gm_gforce_rest();
  for (int i = 0; i < 600; ++i) out = gm_estimator_update(&est, sensor_reading(g, &truth.sensor_to_car, &rng, 0.0f));
  // The 10 deg yaw error of the hint leaks ~0.4*sin(10deg) = 0.07 g between axes.
  CHECK_NEAR(out.proper.x, 0.3f, 0.08f);
  CHECK_NEAR(out.proper.y, 1.0f, 0.01f);
  CHECK_NEAR(out.proper.z, -0.4f, 0.08f);

  // Braking calibration removes the yaw error of the hint entirely.
  const gm_gforce_t brake = gf(0.0f, 1.0f, -0.5f);
  gm_vec3_t mean_brake = gm_vec3_zero();
  for (int i = 0; i < 100; ++i)
    mean_brake = gm_vec3_add(mean_brake, sensor_reading(brake, &truth.sensor_to_car, &rng, 0.0f));
  mean_brake = gm_vec3_div(mean_brake, 100.0f);
  CHECK(gm_estimator_calibrate_forward_from_braking(&est, mean_brake));
  for (int i = 0; i < 600; ++i) out = gm_estimator_update(&est, sensor_reading(g, &truth.sensor_to_car, &rng, 0.0f));
  CHECK_NEAR(out.proper.x, 0.3f, 0.005f);
  CHECK_NEAR(out.proper.z, -0.4f, 0.005f);
}

TEST(estimator_rejects_bad_rest) {
  static gm_estimator_t est;
  gm_estimator_init(&est);
  gm_estimator_begin_rest_calibration(&est);
  for (int i = 0; i < 20; ++i) gm_estimator_feed_rest_sample(&est, gm_vec3(0, 0.2f, 0));  // not 1 g
  CHECK(!gm_estimator_finish_rest_calibration(&est));
  gm_estimator_begin_rest_calibration(&est);
  for (int i = 0; i < 3; ++i) gm_estimator_feed_rest_sample(&est, gm_vec3(0, 1, 0));  // too few
  CHECK(!gm_estimator_finish_rest_calibration(&est));
}

// ---- scenario --------------------------------------------------------------------------------------------
TEST(scenario_presets) {
  CHECK(gm_scenario_preset_count() >= 6);
  CHECK(gm_scenario_find("drive") != NULL);
  CHECK(gm_scenario_find("nope") == NULL);
  const gm_scenario_t* brake = gm_scenario_find("brake");
  CHECK(gm_scenario_duration(brake) > 5.0f);
  CHECK(gm_scenario_dynamic_at(brake, 2.0f).z < -0.5f);
  CHECK_NEAR(gm_vec3_length(gm_scenario_dynamic_at(brake, 0.0f)), 0.0f, 1e-6f);
  // Continuity.
  float max_jump = 0.0f;
  gm_vec3_t prev = gm_scenario_dynamic_at(brake, 0.0f);
  for (float t = 0.0f; t < gm_scenario_duration(brake); t += 0.01f) {
    const gm_vec3_t cur = gm_scenario_dynamic_at(brake, t);
    max_jump = gm_maxf(max_jump, gm_vec3_length(gm_vec3_sub(cur, prev)));
    prev = cur;
  }
  CHECK(max_jump < 0.05f);
}

// ---- effects ------------------------------------------------------------------------------------------------
TEST(gball_tracks_g) {
  static gm_gball_t ball;
  gm_gball_init(&ball);
  gm_frame_t* f = &s_f;
  run_effect(&ball.base, gf(0, 1, -0.5f), 3.0f, 60.0f, f);
  CHECK(gm_gball_position(&ball).z > 0.0f);  // deeper into the stack
  CHECK(lit(f) > 0);
  // Most lit voxels (ball + trail) should sit behind the middle layer; HUD on layer 0 pulls it forward a bit.
  int deep = 0, shallow = 0;
  for (int z = 0; z < GM_D; ++z) {
    const int n = lit_layer(f, z) - (z == 0 ? lit_layer(f, 0) : 0);
    if (z > (GM_D - 1) / 2)
      deep += n;
    else
      shallow += n;
  }
  CHECK(deep > shallow);
  run_effect(&ball.base, gf(0.5f, 1, 0), 3.0f, 60.0f, f);
  CHECK(gm_gball_position(&ball).x < -20.0f);  // thrown left
}

TEST(slosh_conserves_volume_and_tilts) {
  static gm_slosh_t slosh;
  gm_slosh_init(&slosh);
  gm_params_set(gm_effect_params(&slosh.base), "slope_gain", 1.0f);  // physical tilt for this check
  gm_effect_activate(&slosh.base);
  const float v0 = gm_slosh_total_volume(&slosh);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.g = gf(0.3f, 1.0f, 0.0f);  // right turn
  ctx.dt = 1.0f / 60.0f;
  for (int i = 0; i < 60 * 8; ++i) gm_effect_update(&slosh.base, &ctx);
  CHECK_NEAR(gm_slosh_total_volume(&slosh), v0, v0 * 1e-3f);
  CHECK(gm_slosh_last_substeps(&slosh) <= 4);
  // Equilibrium slope dh/dx = a_x / g_v = -0.3 (liquid piles up on the left).
  const float left = gm_slosh_height(&slosh, 2, 0), right = gm_slosh_height(&slosh, GM_SLOSH_NX - 3, 0);
  CHECK(left > right);
  const float slope = (right - left) / ((GM_SLOSH_NX - 5) * GM_SLOSH_DX);
  CHECK_NEAR(slope, -0.3f, 0.05f);
  // Longitudinal: braking pushes liquid toward the windshield (deeper layers).
  gm_effect_reset(&slosh.base);
  ctx.g = gf(0, 1.0f, -0.3f);
  for (int i = 0; i < 60 * 8; ++i) gm_effect_update(&slosh.base, &ctx);
  CHECK(gm_slosh_height(&slosh, GM_SLOSH_NX / 2, GM_SLOSH_NZ - 1) > gm_slosh_height(&slosh, GM_SLOSH_NX / 2, 0) + 5.0f);
  gm_frame_clear(&s_f);
  gm_effect_render(&slosh.base, &s_f);
  for (int z = 0; z < GM_D; ++z) CHECK(lit_layer(&s_f, z) >= GM_W);  // a surface line on every layer
}

TEST(slosh_stable_under_noisy_drive) {
  static gm_slosh_t slosh;
  gm_slosh_init(&slosh);
  gm_effect_activate(&slosh.base);
  const gm_scenario_t* drive = gm_scenario_find("drive");
  gm_rng_t rng;
  gm_rng_seed(&rng, 5);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.dt = 1.0f / 30.0f;
  bool ok = true;
  for (float t = 0.0f; t < gm_scenario_duration(drive); t += ctx.dt) {
    ctx.g = noisy_scenario(drive, t, &rng, 0.015f);
    gm_effect_update(&slosh.base, &ctx);
    for (int z = 0; z < GM_SLOSH_NZ && ok; ++z)
      for (int x = 0; x < GM_SLOSH_NX; ++x) {
        const float h = gm_slosh_height(&slosh, x, z);
        if (!isfinite(h) || h < 0.0f || h > GM_H * 2.0f) {
          ok = false;
          break;
        }
      }
  }
  CHECK(ok);
  CHECK(gm_slosh_last_substeps(&slosh) <= 8);
}

TEST(particles_settle_and_respond) {
  static gm_particles_t p;
  gm_particles_init(&p);
  gm_frame_t* f = &s_f;
  run_effect(&p.base, gm_gforce_rest(), 4.0f, 60.0f, f);
  bool in_bounds = true;
  gm_vec3_t mean = gm_vec3_zero();
  const gm_vec3_t he = gm_half_extent();
  for (int i = 0; i < gm_particles_active_count(&p); ++i) {
    const gm_vec3_t x = gm_particles_position(&p, i);
    if (!finite3(x) || fabsf(x.x) > he.x || fabsf(x.y) > he.y || fabsf(x.z) > he.z) in_bounds = false;
    mean = gm_vec3_add(mean, x);
  }
  mean = gm_vec3_div(mean, (float)gm_particles_active_count(&p));
  CHECK(in_bounds);
  CHECK(mean.y < -10.0f);  // settled toward the floor
  CHECK(lit(f) > gm_particles_active_count(&p) / 2);
  // Pile has some height (pressure keeps it from flattening onto one row).
  int rows = 0;
  for (int y = 0; y < GM_H; ++y) {
    bool any = false;
    for (int z = 0; z < GM_D && !any; ++z)
      for (int x = 0; x < GM_W; ++x)
        if (px(f, x, y, z)) {
          any = true;
          break;
        }
    rows += any;
  }
  CHECK(rows >= 4);
  run_effect(&p.base, gf(0.5f, 1, 0), 3.0f, 60.0f, f);
  mean = gm_vec3_zero();
  for (int i = 0; i < gm_particles_active_count(&p); ++i) mean = gm_vec3_add(mean, gm_particles_position(&p, i));
  mean = gm_vec3_div(mean, (float)gm_particles_active_count(&p));
  CHECK(mean.x < -15.0f);  // thrown left
}

TEST(smoke_runs_and_leans) {
  static gm_smoke_t s;
  gm_smoke_init(&s);
  CHECK(!gm_smoke_allocated(&s));
  gm_effect_activate(&s.base);
  CHECK(gm_smoke_allocated(&s));
  gm_frame_t* f = &s_f;
  run_effect(&s.base, gm_gforce_rest(), 2.0f, 30.0f, f);
  CHECK(gm_smoke_total_density(&s) > 0.0f);
  CHECK(isfinite(gm_smoke_total_density(&s)));
  CHECK(lit(f) > 0);
  int n = 0;
  const gm_vec3_t c_rest = centroid(f, &n);
  run_effect(&s.base, gf(0, 1, -0.6f), 2.0f, 30.0f, f);
  const gm_vec3_t c_brake = centroid(f, &n);
  CHECK(c_brake.z > c_rest.z + 0.5f);  // plume pushed toward deeper layers
  gm_effect_deactivate(&s.base);
  CHECK(!gm_smoke_allocated(&s));
}

TEST(tunnel_and_sprite_render) {
  static gm_tunnel_t t;
  gm_tunnel_init(&t);
  gm_frame_t* f = &s_f;
  run_effect(&t.base, gm_gforce_rest(), 1.0f, 60.0f, f);
  for (int z = 0; z < GM_D; ++z) CHECK(lit_layer(f, z) > 0);
  static gm_sprite_t sp;
  gm_sprite_init(&sp, gm_sprite_demo_sheet(), NULL);
  CHECK(gm_sprite_demo_sheet()->frames == 12);
  run_effect(&sp.base, gm_gforce_rest(), 1.0f, 60.0f, f);
  CHECK(lit(f) > 10);
  run_effect(&sp.base, gf(0, 1, -0.5f), 3.0f, 60.0f, f);
  CHECK(gm_sprite_position(&sp).z > 10.0f);
}

TEST(engine_switching_and_params) {
  static gm_gball_t ball;
  static gm_slosh_t slosh;
  static gm_tunnel_t tunnel;
  static gm_anim_t anim;
  static gm_engine_t eng;
  gm_gball_init(&ball);
  gm_slosh_init(&slosh);
  gm_tunnel_init(&tunnel);
  gm_anim_init(&anim, gm_anim_demo());
  CHECK(gm_params_count(gm_effect_params(&anim.base)) <= GM_PARAMS_MAX);
  gm_engine_init(&eng);
  CHECK(gm_engine_add_effect(&eng, &ball.base) && gm_engine_add_effect(&eng, &slosh.base) &&
        gm_engine_add_effect(&eng, &tunnel.base));
  CHECK(gm_engine_effect_count(&eng) == 3);
  CHECK(gm_engine_find_effect(&eng, "slosh") == 1);
  const gm_gforce_t g = gf(0.2f, 1, 0);
  gm_engine_step(&eng, g, 1.0f / 60.0f);
  CHECK(lit(gm_engine_frame(&eng)) > 0);
  // `mode slosh`: by name, with a crossfade.
  const int idx = gm_engine_find_effect(&eng, "slosh");
  CHECK(idx >= 0);
  gm_engine_set_effect(&eng, idx, true);
  int total = 0;
  for (int i = 0; i < 60; ++i) {
    gm_engine_step(&eng, g, 1.0f / 60.0f);
    total += lit(gm_engine_frame(&eng));
  }
  CHECK(total > 0);
  CHECK(gm_engine_current_index(&eng) == 1);
  CHECK(engine_set_param(&eng, "fill", 0.3f));
  float v = 0.0f;
  CHECK(gm_params_get(gm_effect_params(gm_engine_current(&eng)), "fill", &v));
  CHECK_NEAR(v, 0.3f, 1e-6f);
  CHECK(!engine_set_param(&eng, "nonexistent", 1.0f));
  engine_set_param(&eng, "fill", 99.0f);  // clamped
  gm_params_get(gm_effect_params(gm_engine_current(&eng)), "fill", &v);
  CHECK(v <= 0.8f);
  gm_engine_next_effect(&eng);
  CHECK(gm_engine_current_index(&eng) == 2);
  gm_engine_set_auto_cycle(&eng, 0.5f);
  for (int i = 0; i < 40; ++i) gm_engine_step(&eng, g, 1.0f / 60.0f);
  CHECK(gm_engine_current_index(&eng) == 0);
}

// ---- authored animation ------------------------------------------------------------------------------
static void anim_drive(gm_anim_t* a, gm_gforce_t g, float seconds) {
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.g = g;
  ctx.dt = 1.0f / 60.0f;
  for (int i = 0; i < (int)(seconds * 60.0f); ++i) gm_effect_update(&a->base, &ctx);
}

static void anim_step(gm_anim_t* a, gm_gforce_t g) {
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.g = g;
  gm_effect_update(&a->base, &ctx);
}

TEST(anim_demo_asset_and_states) {
  const gm_anim_asset_t* demo = gm_anim_demo();
  CHECK(gm_anim_asset_valid(demo));
  CHECK(demo->width == 32 && demo->height == 24 && demo->depth == 3 && demo->frames == 32);
  static gm_anim_t a;
  gm_anim_init(&a, demo);
  gm_effect_activate(&a.base);
  anim_drive(&a, dyn(0, 0, 0), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_IDLE);
  CHECK(gm_anim_current_frame(&a) >= 0 && gm_anim_current_frame(&a) < 8);
  anim_drive(&a, dyn(0, 0, 0.5f), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_ACCELERATE);
  CHECK(gm_anim_current_frame(&a) >= 8 && gm_anim_current_frame(&a) < 14);
  anim_drive(&a, dyn(0, 0, -0.5f), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_BRAKE);
  CHECK(gm_anim_position(&a).z > 5.0f);  // thrown toward the windshield under braking
  anim_drive(&a, dyn(0.5f, 0, 0), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_RIGHT);
  CHECK(!gm_anim_mirrored(&a));
  CHECK(gm_anim_position(&a).x < -5.0f);  // thrown left in a right turn
  anim_drive(&a, dyn(-0.5f, 0, 0), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_LEFT);
  CHECK(gm_anim_mirrored(&a));  // demo has no left clip: right is mirrored
  CHECK(gm_anim_current_frame(&a) >= 20 && gm_anim_current_frame(&a) < 26);
  // Hysteresis: a dip to 70 % of the entry threshold keeps the state.
  anim_drive(&a, dyn(-0.18f, 0, 0), 0.5f);
  CHECK(gm_anim_state(&a) == GM_ANIM_LEFT);
  anim_drive(&a, dyn(0, 0, 0), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_IDLE);
  // A vertical spike fires the one-shot bump clip, which then hands back.
  anim_step(&a, dyn(0, 0.6f, 0));
  CHECK(gm_anim_state(&a) == GM_ANIM_BUMP);
  for (int i = 0; i < 60; ++i) anim_step(&a, dyn(0, 0, 0));
  CHECK(gm_anim_state(&a) == GM_ANIM_IDLE);
  // Forced state for previews.
  gm_anim_force_state(&a, GM_ANIM_BRAKE);
  anim_drive(&a, dyn(0.8f, 0, 0), 0.5f);
  CHECK(gm_anim_state(&a) == GM_ANIM_BRAKE);
  gm_anim_force_state(&a, -1);
  anim_drive(&a, dyn(0.8f, 0, 0), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_RIGHT);
}

TEST(anim_render_slices_and_bounds) {
  static gm_anim_t a;
  gm_anim_init(&a, gm_anim_demo());
  gm_effect_activate(&a.base);
  anim_drive(&a, dyn(0, 0, 0), 0.2f);
  gm_frame_t* f = &s_f;
  gm_frame_clear(f);
  gm_effect_render(&a.base, f);
  int layers_lit = 0;
  for (int z = 0; z < GM_D; ++z) layers_lit += lit_layer(f, z) > 0;
  CHECK(layers_lit == 3);  // three slices on three consecutive panels
  // Extreme g never pushes slices outside the stack or the panel.
  anim_drive(&a, dyn(3.0f, 3.0f, -3.0f), 2.0f);
  gm_frame_clear(f);
  gm_effect_render(&a.base, f);
  CHECK(lit(f) > 20);
}

// fixtures/user_animation.h is a header exactly as Gmeter Studio exports it
// for main/user_animation.h; compiling it here pins the format.
TEST(anim_studio_export_compiles_and_plays) {
  CHECK(gm_anim_asset_valid(&kUserAnimation));
  CHECK(gm_anim_asset_total_bytes(&kUserAnimation) == (int)sizeof(kUserAnimationBits));
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s)
    CHECK(kUserAnimation.clips[s].count == 0 || kUserAnimation.clips[s].start < kUserAnimation.frames);
  static gm_anim_t a;
  gm_anim_init(&a, &kUserAnimation);
  gm_effect_activate(&a.base);
  float v = 0.0f;
  CHECK(gm_params_get(gm_effect_params(&a.base), "enter_g", &v) && fabsf(v - kUserAnimation.mapping.enter_g) < 1e-6f);
  anim_drive(&a, dyn(0, 0, -0.6f), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_BRAKE);
  gm_frame_clear(&s_f);
  gm_effect_render(&a.base, &s_f);
  CHECK(lit(&s_f) > 0);
}

TEST(anim_degenerate_assets) {
  // Empty idle clip: plays all frames. Clip past the end: clamped.
  static const uint8_t bits[2 * 1 * 8] = {0xFF, 0, 0, 0, 0, 0, 0, 0, 0x81, 0, 0, 0, 0, 0, 0, 0};
  gm_anim_asset_t asset = gm_anim_asset_empty();
  asset.name = "tiny";
  asset.width = 8;
  asset.height = 8;
  asset.depth = 1;
  asset.frames = 2;
  asset.bits = bits;
  asset.clips[GM_ANIM_BRAKE] = (gm_anim_clip_t){1, 50, 10.0f, 1};
  static gm_anim_t a;
  gm_anim_init(&a, &asset);
  gm_effect_activate(&a.base);
  bool mirror = false;
  const gm_anim_clip_t idle = gm_anim_resolve_clip(&a, GM_ANIM_IDLE, &mirror);
  CHECK(idle.start == 0 && idle.count == 2);
  const gm_anim_clip_t brake = gm_anim_resolve_clip(&a, GM_ANIM_BRAKE, &mirror);
  CHECK(brake.start == 1 && brake.count == 1);
  const gm_anim_clip_t left = gm_anim_resolve_clip(&a, GM_ANIM_LEFT, &mirror);
  CHECK(left.count == 2 && !mirror);
  anim_drive(&a, dyn(0, 0, -0.6f), 1.0f);
  CHECK(gm_anim_current_frame(&a) == 1);
  const gm_anim_asset_t none = gm_anim_asset_empty();
  static gm_anim_t b;
  gm_anim_init(&b, &none);
  gm_effect_activate(&b.base);
  anim_drive(&b, dyn(0.5f, 0, 0), 0.5f);
  gm_frame_clear(&s_f);
  gm_effect_render(&b.base, &s_f);
  CHECK(lit(&s_f) == 0);
}

// Lowest and highest panel lit by the effect after driving it with `g`.
static void lit_layers(gm_anim_t* a, gm_gforce_t g, float seconds, int* lo, int* hi) {
  anim_drive(a, g, seconds);
  gm_frame_clear(&s_f);
  gm_effect_render(&a->base, &s_f);
  *lo = GM_D;
  *hi = -1;
  for (int z = 0; z < GM_D; ++z)
    if (lit_layer(&s_f, z) > 0) {
      *lo = gm_mini(*lo, z);
      *hi = gm_maxi(*hi, z);
    }
}

TEST(anim_even_slice_counts_reach_both_ends_of_the_stack) {
  // All-lit 8x8 art. Before the fix, even slice counts sat half a layer too deep
  // (depth 2 never reached panel 0, depth 6 did not move under braking).
  static uint8_t bits[8 * 8 * 8];
  memset(bits, 0xFF, sizeof(bits));
  static const int k_depths[] = {2, 4, 6};
  for (int i = 0; i < 3; ++i) {
    const int depth = k_depths[i];
    if (depth > GM_D) continue;
    gm_anim_asset_t asset = gm_anim_asset_empty();
    asset.name = "block";
    asset.width = 8;
    asset.height = 8;
    asset.depth = (uint16_t)depth;
    asset.frames = 1;
    asset.bits = bits;
    static gm_anim_t a;
    gm_anim_init(&a, &asset);
    gm_effect_activate(&a.base);
    int lo = 0, hi = 0;
    lit_layers(&a, dyn(0, 0, 0), 2.0f, &lo, &hi);
    CHECK(hi - lo == depth - 1);
    CHECK(lo == (GM_D - depth) / 2);
    lit_layers(&a, dyn(0, 0, -3.0f), 3.0f, &lo, &hi);  // braking: thrown to the back
    CHECK(hi == GM_D - 1 && lo == GM_D - depth);
    lit_layers(&a, dyn(0, 0, 3.0f), 3.0f, &lo, &hi);  // accelerating: pressed to the front
    CHECK(lo == 0 && hi == depth - 1);
  }
}

TEST(anim_hysteresis_keeps_state_when_stronger_axis_cannot_enter) {
  static gm_anim_t a;
  gm_anim_init(&a, gm_anim_demo());
  gm_effect_activate(&a.base);
  gm_params_set(gm_effect_params(&a.base), "enter_g", 0.5f);
  anim_drive(&a, dyn(0, 0, -0.6f), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_BRAKE);
  // Trail braking: lateral 0.45 g is stronger but below enter_g; braking 0.35 g
  // is above the 60 % exit level, so the state must stay brake (was idle).
  anim_drive(&a, dyn(0.45f, 0, -0.35f), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_BRAKE);
  // Once the corner itself can enter, it takes over.
  anim_drive(&a, dyn(0.6f, 0, -0.35f), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_RIGHT);

  gm_gstate_t t;
  gm_gstate_init(&t);
  t.enter_g = 0.5f;
  t.hold_s = 0.0f;
  for (int i = 0; i < 10; ++i) gm_gstate_update(&t, dyn(0, 0, -0.6f), 0.1f, NULL);
  CHECK(gm_gstate_state(&t) == GM_DRIVE_BRAKE);
  for (int i = 0; i < 10; ++i) gm_gstate_update(&t, dyn(0.45f, 0, -0.35f), 0.1f, NULL);
  CHECK(gm_gstate_state(&t) == GM_DRIVE_BRAKE);
  for (int i = 0; i < 10; ++i) gm_gstate_update(&t, dyn(0.6f, 0, -0.35f), 0.1f, NULL);
  CHECK(gm_gstate_state(&t) == GM_DRIVE_RIGHT);
}

TEST(anim_bump_clip_past_the_end_never_fires) {
  static const uint8_t bits[2 * 8] = {0xFF, 0, 0, 0, 0, 0, 0, 0, 0x81, 0, 0, 0, 0, 0, 0, 0};
  gm_anim_asset_t asset = gm_anim_asset_empty();
  asset.name = "tiny";
  asset.width = 8;
  asset.height = 8;
  asset.depth = 1;
  asset.frames = 2;
  asset.bits = bits;
  asset.clips[GM_ANIM_BUMP] = (gm_anim_clip_t){2, 4, 10.0f, 0};  // starts one past the last frame
  static gm_anim_t a;
  gm_anim_init(&a, &asset);
  gm_effect_activate(&a.base);
  anim_drive(&a, dyn(0, 0, 0), 0.5f);
  anim_step(&a, dyn(0, 0.9f, 0));
  CHECK(gm_anim_state(&a) == GM_ANIM_IDLE);
  // And corners still work straight after the spike.
  anim_drive(&a, dyn(0.6f, 0, 0), 1.0f);
  CHECK(gm_anim_state(&a) == GM_ANIM_RIGHT);
}

TEST(anim_states_sharing_the_idle_clip_do_not_restart_it) {
  // Idle-only asset: accelerate falls back to idle and must carry on from the
  // frame already showing rather than jumping back to frame 0.
  static uint8_t bits[8 * 8];
  for (int i = 0; i < 8; ++i) bits[i * 8] = (uint8_t)(1u << i);
  gm_anim_asset_t asset = gm_anim_asset_empty();
  asset.name = "idle";
  asset.width = 8;
  asset.height = 8;
  asset.depth = 1;
  asset.frames = 8;
  asset.bits = bits;
  asset.clips[GM_ANIM_IDLE] = (gm_anim_clip_t){0, 8, 4.0f, 1};
  asset.mapping.fps_per_g = 0.0f;
  asset.mapping.hold_s = 0.0f;
  static gm_anim_t a;
  gm_anim_init(&a, &asset);
  gm_effect_activate(&a.base);
  anim_drive(&a, dyn(0, 0, 0), 0.55f);  // about frame 2 at 4 fps
  const int before = gm_anim_current_frame(&a);
  CHECK(before >= 1);
  anim_step(&a, dyn(0, 0, 0.6f));
  CHECK(gm_anim_state(&a) == GM_ANIM_ACCELERATE);
  CHECK(gm_anim_current_frame(&a) >= before);
}

TEST(jp_ascii_knockout_clears_exactly_the_glyph) {
  // Reverse video over ASCII: the knock-out must clear the same pixels the
  // 3x5 font sets, without a scratch frame on the stack.
  gm_frame_t *lit_f = &s_f, *ref = &s_ref;
  gm_frame_clear(lit_f);
  gm_frame_clear(ref);
  for (int y = 0; y < GM_H; ++y)
    for (int x = 0; x < 40; ++x) gm_frame_set(lit_f, x, y, 2);
  const int x0 = 3, y0 = 4;
  gm_jp_draw(lit_f, &gm_jp_small, 2, x0, y0, "A7%", 1, -1.0f);
  // Where gm_frame_text draws the same string (each ASCII advance is 4 + spacing 1).
  const int top = y0 + (gm_jp_small.height - 5) / 2;
  const char* chars = "A7%";
  for (int i = 0; i < 3; ++i) {
    const char one[2] = {chars[i], '\0'};
    gm_frame_text(ref, 2, x0 + i * 5, top, one);
  }
  int mismatches = 0, cleared = 0;
  for (int y = 0; y < GM_H; ++y)
    for (int x = 0; x < 40; ++x) {
      const bool should_clear = px(ref, x, y, 2);
      cleared += should_clear;
      if (px(lit_f, x, y, 2) == should_clear) ++mismatches;
    }
  CHECK(cleared > 10);
  CHECK(mismatches == 0);
}

TEST(voxel_frame_shift_moves_every_layer) {
  gm_frame_t *a = &s_f, *b = &s_tmp;
  gm_frame_clear(a);
  gm_frame_set(a, 0, 0, 0);
  gm_frame_set(a, 10, 7, 3);
  gm_frame_set(a, GM_W - 1, GM_H - 1, GM_D - 1);
  gm_frame_set(a, 5, 8, 1);
  gm_frame_set(a, 64, 31, 2);
  for (int dy = -9; dy <= 9; dy += 3)
    for (int dx = -2; dx <= 2; ++dx) {
      *b = *a;
      gm_frame_shift(b, dx, dy);
      int bad = 0;
      for (int z = 0; z < GM_D; ++z)
        for (int y = 0; y < GM_H; ++y)
          for (int x = 0; x < GM_W; ++x)
            if (px(b, x, y, z) != px(a, x - dx, y - dy, z)) ++bad;
      CHECK(bad == 0);
    }
  *b = *a;
  gm_frame_shift(b, GM_W, 0);
  CHECK(lit(b) == 0);
}

static void burn_in(const gm_effect_t* e, int* dx, int* dy) { gm_effect_burn_in_offset(e, dx, dy); }

TEST(hud_orbits_one_pixel_and_keeps_the_frame_on_the_glass) {
  static gm_hud_t hud;
  static gm_engine_t e;
  gm_hud_init(&hud);
  gm_engine_init(&e);
  gm_engine_add_effect(&e, &hud.base);
  gm_engine_set_status_label(&e, "DEMO");
  gm_params_set(gm_effect_params(&hud.base), "orbit_min", 0.05f);  // 3 s per step for the test
  gm_frame_t* tag = &s_ref;  // the DEMO tag where the engine draws it with no offset
  gm_frame_clear(tag);
  gm_frame_text(tag, 0, GM_W - 2 - (4 * 4 - 1), GM_H - 6, "DEMO");
  static const int seq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
#define RUN_FOR(secs)                                                    \
  for (int i_ = 0; i_ < (int)((secs) * 60.0f + 0.5f); ++i_) gm_engine_step(&e, gm_gforce_rest(), 1.0f / 60.0f)
  RUN_FOR(0.1f);
  for (int step = 0; step < 8; ++step) {
    int dx = 0, dy = 0;
    burn_in(&hud.base, &dx, &dy);
    CHECK(dx == seq[step & 3][0] && dy == seq[step & 3][1]);
    const gm_frame_t* f = gm_engine_frame(&e);
    // The border's top edge (front panel) sits on row dy and its left edge
    // on column dx; the right and bottom edges are still on the glass.
    CHECK(px(f, GM_W / 2, dy, 0));
    CHECK(px(f, dx, GM_H / 2, 0));
    CHECK(px(f, GM_W - 2 + dx, GM_H / 2, 0));
    CHECK(px(f, GM_W / 2, GM_H - 2 + dy, 0));
    // The engine's tag moves with the picture.
    int want = 0, match = 0;
    for (int y = 0; y < GM_H; ++y)
      for (int x = 0; x < GM_W; ++x)
        if (px(tag, x, y, 0)) {
          ++want;
          match += px(f, x + dx, y + dy, 0);
        }
    CHECK(want > 10 && match == want);
    RUN_FOR(3.05f);
  }
  // Re-activation keeps the orbit's place; gm_hud_set_orbit_step picks it.
  int dx = 0, dy = 0;
  gm_params_set(gm_effect_params(&hud.base), "orbit_min", 30.0f);
  gm_hud_set_orbit_step(&hud, 2);
  gm_effect_activate(&hud.base);
  burn_in(&hud.base, &dx, &dy);
  CHECK(dx == 1 && dy == 1);
  gm_hud_set_orbit_step(&hud, 7);
  burn_in(&hud.base, &dx, &dy);
  CHECK(dx == 0 && dy == 1);
  gm_params_set(gm_effect_params(&hud.base), "orbit_min", 0.0f);
  RUN_FOR(0.1f);
  burn_in(&hud.base, &dx, &dy);
  CHECK(dx == 0 && dy == 0);
#undef RUN_FOR
  // Eight days switched on (1 s steps): the warning blink still toggles,
  // because its clock wraps instead of running out of float precision.
  gm_params_set(gm_effect_params(&hud.base), "warn_g", 0.1f);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.g = dyn(0.5f, 0, 0);
  ctx.dt = 1.0f;
  for (int i = 0; i < 8 * 24 * 3600; ++i) gm_effect_update(&hud.base, &ctx);
  ctx.dt = 1.0f / 60.0f;
  int on_frames = 0, off_frames = 0;
  for (int i = 0; i < 60; ++i) {
    gm_effect_update(&hud.base, &ctx);
    gm_frame_clear(&s_f);
    gm_effect_render(&hud.base, &s_f);
    int n = 0;  // status-column pixels on the right
    for (int y = 18; y < GM_H - 10; ++y)
      for (int x = GM_W - 22; x < GM_W - 4; ++x) n += px(&s_f, x, y, 0);
    if (n > 20)
      ++on_frames;
    else
      ++off_frames;
  }
  CHECK(on_frames > 10 && off_frames > 10);
}

TEST(engine_status_label_marks_non_live_data) {
  static gm_engine_t e, plain;
  static gm_hud_t hud, hud2;
  gm_engine_init(&e);
  gm_hud_init(&hud);
  gm_engine_add_effect(&e, &hud.base);
  const gm_gforce_t g = gm_gforce_rest();
  gm_engine_set_status_label(&e, "DEMO");
  for (int i = 0; i < 120; ++i) gm_engine_step(&e, g, 1.0f / 60.0f);
  gm_frame_t* ref = &s_ref;
  gm_frame_clear(ref);
  gm_frame_text(ref, 0, GM_W - 2 - (4 * 4 - 1), GM_H - 6, "DEMO");
  int match = 0, want = 0;
  for (int y = GM_H - 7; y < GM_H; ++y)
    for (int x = GM_W - 40; x < GM_W; ++x)
      if (px(ref, x, y, 0)) {
        ++want;
        match += px(gm_engine_frame(&e), x, y, 0);
      }
  CHECK(want > 10 && match == want);
  // Without a tag the frame is exactly what an untagged engine draws.
  gm_engine_init(&plain);
  gm_hud_init(&hud2);
  gm_engine_add_effect(&plain, &hud2.base);
  for (int i = 0; i < 121; ++i) gm_engine_step(&plain, g, 1.0f / 60.0f);
  gm_engine_set_status_label(&e, NULL);
  gm_engine_step(&e, g, 1.0f / 60.0f);
  CHECK(memcmp(gm_engine_frame(&e)->bits, gm_engine_frame(&plain)->bits, sizeof(gm_frame_t)) == 0);
  gm_engine_set_status_label(&e, "NO SENSOR");
  gm_engine_step(&e, g, 1.0f / 60.0f);
  CHECK(memcmp(gm_engine_frame(&e)->bits, gm_engine_frame(&plain)->bits, sizeof(gm_frame_t)) != 0);
  // The HUD's status column (正常) ends above the tag's box, so the tag never
  // cuts it: nothing in the column's x range on the row above the box or in it,
  // apart from the ruler tick.
  gm_engine_step(&plain, g, 1.0f / 60.0f);
  const int col_x = GM_W - 6 - gm_jp_small.glyphs[0].width;
  int column = 0, in_box = 0;
  for (int y = 0; y < GM_H - 8; ++y)
    for (int x = col_x; x < col_x + gm_jp_small.glyphs[0].width; ++x) column += px(gm_engine_frame(&plain), x, y, 0);
  for (int y = GM_H - 8; y < GM_H - 5; ++y)
    for (int x = col_x; x < col_x + gm_jp_small.glyphs[0].width; ++x) in_box += px(gm_engine_frame(&plain), x, y, 0);
  CHECK(column > 40);
  CHECK(in_box == 0);
}

TEST(engine_burn_in_offset_moves_overlays_and_only_the_hud) {
  const gm_gforce_t g = gm_gforce_rest();
  // The mode name (first 1.5 s) and the readout move with the HUD's offset.
  static gm_hud_t hud;
  static gm_tunnel_t tunnel;
  static gm_engine_t e;
  gm_hud_init(&hud);
  gm_tunnel_init(&tunnel);
  gm_engine_init(&e);
  gm_engine_add_effect(&e, &tunnel.base);
  gm_engine_add_effect(&e, &hud.base);
  gm_engine_set_readout(&e, true);
  gm_hud_set_orbit_step(&hud, 2);  // offset (1, 1)
  gm_engine_set_effect(&e, 1, false);
  gm_engine_step(&e, g, 1.0f / 60.0f);
  gm_frame_t* ref = &s_ref;
  gm_frame_clear(ref);
  gm_frame_text(ref, 0, 1, 1, "HUD");
  gm_frame_text(ref, 0, 1, GM_H - 6, "0.00G");
  int want = 0, match = 0;
  for (int y = 0; y < GM_H; ++y)
    for (int x = 0; x < GM_W; ++x)
      if (px(ref, x, y, 0)) {
        ++want;
        match += px(gm_engine_frame(&e), x + 1, y + 1, 0);
      }
  CHECK(want > 20 && match == want);
  // Crossfading away from the HUD keeps its picture at its own offset: the
  // border's left edge stays on column 1, not 0.
  gm_engine_set_effect(&e, 0, true);
  gm_engine_step(&e, g, 1.0f / 60.0f);
  int col0 = 0, col1 = 0;
  for (int y = 12; y < GM_H - 12; ++y) {
    col0 += px(gm_engine_frame(&e), 0, y, 0);
    col1 += px(gm_engine_frame(&e), 1, y, 0);
  }
  CHECK(col1 > (GM_H - 24) / 2 && col0 < col1 / 4);
  // Every other effect leaves the picture where it is.
  static gm_gball_t ball;
  static gm_slosh_t slosh;
  static gm_particles_t particles;
  static gm_smoke_t smoke;
  static gm_sprite_t sprite;
  static gm_anim_t anim;
  static gm_atfield_t atfield;
  static gm_sync_t sync;
  gm_gball_init(&ball);
  gm_slosh_init(&slosh);
  gm_particles_init(&particles);
  gm_smoke_init(&smoke);
  gm_sprite_init(&sprite, gm_sprite_demo_sheet(), NULL);
  gm_anim_init(&anim, gm_anim_demo());
  gm_atfield_init(&atfield);
  gm_sync_init(&sync);
  const gm_effect_t* others[] = {&ball.base, &slosh.base,  &particles.base, &smoke.base, &tunnel.base,
                                 &sprite.base, &anim.base, &atfield.base,   &sync.base};
  for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
    int dx = 7, dy = 7;
    gm_effect_burn_in_offset(others[i], &dx, &dy);
    CHECK(dx == 0 && dy == 0);
  }
}

TEST(hud_warning_column_clears_the_hazard_stripes) {
  // 警告 blinks in the status column while hazard stripes scroll below it; one
  // empty row must separate them (on 56-row panels they used to touch).
  static gm_hud_t hud;
  gm_hud_init(&hud);
  gm_effect_activate(&hud.base);
  gm_params_set(gm_effect_params(&hud.base), "warn_g", 0.1f);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.dt = 1.0f / 60.0f;
  ctx.g = dyn(0.5f, 0, 0);
  const int col_x = GM_W - 6 - gm_jp_small.glyphs[0].width;
  int glyph_frames = 0, gap_lit = 0;
  for (int i = 0; i < 90; ++i) {
    gm_effect_update(&hud.base, &ctx);
    gm_frame_clear(&s_f);
    gm_effect_render(&hud.base, &s_f);
    int column = 0;
    for (int y = 0; y < GM_H - 10; ++y)
      for (int x = col_x; x < col_x + gm_jp_small.glyphs[0].width; ++x) column += px(&s_f, x, y, 0);
    if (column > 40) ++glyph_frames;
    for (int x = col_x; x < col_x + gm_jp_small.glyphs[0].width; ++x) gap_lit += px(&s_f, x, GM_H - 10, 0);
  }
  CHECK(glyph_frames > 20);
  CHECK(gap_lit == 0);
}

// ---- calibration physics ------------------------------------------------------------------------------
TEST(calibration_matches_a_physical_right_handed_sensor) {
  // A GY-521 lying flat: chip +Z up, chip +Y toward the windshield, so chip +X
  // (= Y x Z for a right-handed sensor) points to the car's right. These raw
  // vectors are written from physics, not produced by the calibration code.
  static gm_estimator_t est, up;
  gm_estimator_init(&est);
  gm_estimator_set_nominal_forward(&est, gm_vec3(0, 1, 0));
  gm_estimator_set_filter(&est, 0.0f, 200.0f);  // pass-through for exact numbers
  gm_estimator_begin_rest_calibration(&est);
  for (int i = 0; i < 50; ++i) gm_estimator_feed_rest_sample(&est, gm_vec3(0, 0, 1));
  CHECK(gm_estimator_finish_rest_calibration(&est));
  CHECK(gm_estimator_forward_trusted(&est));
  gm_gforce_t g = gm_gforce_rest();
  for (int i = 0; i < 5; ++i) g = gm_estimator_update(&est, gm_vec3(0.5f, 0, 1));  // right turn: pushed toward +X
  CHECK_NEAR(gm_gforce_lateral(g), 0.5f, 1e-4f);
  CHECK_NEAR(gm_gforce_longitudinal(g), 0.0f, 1e-4f);
  for (int i = 0; i < 5; ++i) g = gm_estimator_update(&est, gm_vec3(0, -0.5f, 1));  // braking: pushed toward -Y
  CHECK_NEAR(gm_gforce_longitudinal(g), -0.5f, 1e-4f);
  CHECK_NEAR(gm_gforce_lateral(g), 0.0f, 1e-4f);
  CHECK(gm_gforce_inertial(g).z > 0.0f);  // loose objects go toward the windshield
  // Upright board facing forward (chip +Z forward, +Y up, so +X = Y x Z = left).
  gm_estimator_init(&up);
  gm_estimator_set_nominal_forward(&up, gm_vec3(0, 0, 1));
  gm_estimator_set_filter(&up, 0.0f, 200.0f);
  gm_estimator_begin_rest_calibration(&up);
  for (int i = 0; i < 50; ++i) gm_estimator_feed_rest_sample(&up, gm_vec3(0, 1, 0));
  CHECK(gm_estimator_finish_rest_calibration(&up));
  for (int i = 0; i < 5; ++i) g = gm_estimator_update(&up, gm_vec3(-0.4f, 1, 0));  // right turn: toward chip -X
  CHECK_NEAR(gm_gforce_lateral(g), 0.4f, 1e-4f);
}

TEST(calibration_vertical_hint_needs_braking) {
  static gm_estimator_t est;
  gm_estimator_init(&est);
  gm_estimator_set_nominal_forward(&est, gm_vec3(0, 0, 1));  // default hint, but the board lies flat (+Z up)
  gm_estimator_set_filter(&est, 0.0f, 200.0f);
  gm_estimator_begin_rest_calibration(&est);
  for (int i = 0; i < 50; ++i) gm_estimator_feed_rest_sample(&est, gm_vec3(0.02f, 0.01f, 1.0f));
  CHECK(gm_estimator_finish_rest_calibration(&est));  // up is known...
  CHECK(!gm_estimator_forward_trusted(&est));         // ...but forward is not
  CHECK(!gm_estimator_make_calibration(gm_vec3(0, 0, 1), gm_vec3(0.3f, 0, 1)).valid);  // 17 deg from vertical
  // A firm straight brake: chip +Y toward the windshield, so braking pushes -Y.
  CHECK(gm_estimator_calibrate_forward_from_braking(&est, gm_vec3(0.0f, -0.5f, 1.0f)));
  CHECK(gm_estimator_forward_trusted(&est));
  gm_gforce_t g = gm_gforce_rest();
  for (int i = 0; i < 5; ++i) g = gm_estimator_update(&est, gm_vec3(0.5f, 0, 1));
  CHECK_NEAR(gm_gforce_lateral(g), 0.5f, 0.03f);
  // A rejected rest capture keeps the previous calibration.
  const gm_mat3_t before = gm_estimator_calibration(&est).sensor_to_car;
  gm_estimator_begin_rest_calibration(&est);
  for (int i = 0; i < 50; ++i) gm_estimator_feed_rest_sample(&est, gm_vec3(0.5f, 1.3f, 0.2f));
  CHECK(!gm_estimator_finish_rest_calibration(&est));
  CHECK_NEAR(gm_estimator_calibration(&est).sensor_to_car.m[2][1], before.m[2][1], 1e-6f);
}

TEST(params_and_clamp_reject_nan) {
  CHECK_NEAR(gm_clampf(nanf(""), 1.0f, 2.0f), 1.0f, 0.0f);
  CHECK_NEAR(gm_clampf(5.0f, 1.0f, 2.0f), 2.0f, 0.0f);
  static gm_smoke_t smoke;
  gm_smoke_init(&smoke);
  CHECK(!gm_params_set(gm_effect_params(&smoke.base), "iters", nanf("")));
  CHECK(!gm_params_set(gm_effect_params(&smoke.base), "iters", INFINITY));
  float v = 0.0f;
  gm_params_get(gm_effect_params(&smoke.base), "iters", &v);
  CHECK(isfinite(v));
}

TEST(slosh_stays_stable_when_substeps_are_capped) {
  static gm_slosh_t slosh, s2;
  gm_slosh_init(&slosh);
  gm_param_table_t* t = gm_effect_params(&slosh.base);
  gm_params_set(t, "tank_m", 0.1f);    // fast waves
  gm_params_set(t, "substeps", 2.0f);  // tight cost cap
  gm_params_set(t, "damping", 0.0f);
  gm_effect_activate(&slosh.base);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.dt = 1.0f / 15.0f;
  const gm_scenario_t* drive = gm_scenario_find("slalom");
  float worst = 0.0f;
  for (float tt = 0.0f; tt < gm_scenario_duration(drive); tt += ctx.dt) {
    ctx.g = gm_scenario_at(drive, tt, NULL);
    gm_effect_update(&slosh.base, &ctx);
    for (int z = 0; z < GM_SLOSH_NZ; ++z)
      for (int x = 0; x < GM_SLOSH_NX; ++x) worst = gm_maxf(worst, gm_slosh_height(&slosh, x, z));
  }
  CHECK(isfinite(worst));
  CHECK(worst < GM_H * 1.6f);
  // Runtime fill change takes effect.
  gm_slosh_init(&s2);
  gm_effect_activate(&s2.base);
  const float v0 = gm_slosh_total_volume(&s2);
  gm_params_set(gm_effect_params(&s2.base), "fill", 0.2f);
  const gm_effect_ctx_t c2 = GM_EFFECT_CTX_INIT;
  for (int i = 0; i < 180; ++i) gm_effect_update(&s2.base, &c2);
  CHECK(gm_slosh_total_volume(&s2) < v0 * 0.6f);
}

TEST(voxel_clipping_edges) {
  gm_frame_t* f = &s_f;
  gm_frame_clear(f);
  gm_frame_hline(f, 0, 200, 300, 5);
  gm_frame_hline(f, 0, -50, -10, 5);
  gm_frame_fill_rect(f, 0, -10, -10, -5, -5, 1.0f);
  gm_frame_fill_rect(f, 0, 130, 60, 140, 70, 1.0f);
  gm_frame_vline(f, 0, 5, -1000000, -10);
  CHECK(lit(f) == 0);
  gm_frame_line(f, 0, -300, 10, 300, 10);
  CHECK(lit_layer(f, 0) == GM_W);
  gm_frame_clear(f);
  gm_frame_vline(f, 0, 3, -100000, 100000);
  CHECK(lit(f) == GM_H);
  gm_frame_clear_box(f, 0, 0, GM_W - 1, GM_H - 1);
  CHECK(lit(f) == 0);
  // A ball centred exactly between two layers is still drawn.
  gm_frame_sphere(f, gm_vec3_zero(), 5.0f, 0.3f * GM_LAYER_PITCH_PX, 1.0f);
  CHECK(lit(f) > 0);
}

// ---- NGE-inspired effects and Japanese text --------------------------------------------------------
static bool all_present(const gm_jp_font_t* font, const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; *p;) {
    uint32_t cp = *p;
    const int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= (0x3Fu >> extra);
    ++p;
    for (int i = 0; i < extra; ++i) cp = (cp << 6) | (*p++ & 0x3Fu);
    if (cp >= 0x80 && !gm_jp_find_glyph(font, cp)) return false;
  }
  return true;
}

TEST(jp_text_covers_every_string_the_effects_draw) {
  // Each word in the font it is drawn in (警告 only ever in the small one).
  static const char* const large[] = {"待機", "加速", "制動", "左旋回", "右旋回", "衝撃", "危険"};
  static const char* const small[] = {"横G", "縦G", "正常", "警告", "A.T.フィールド",
                                      "全開", "横", "縦", "上下", "同期率"};
  for (size_t i = 0; i < sizeof(large) / sizeof(large[0]); ++i) {
    CHECK(all_present(&gm_jp_large, large[i]));
    CHECK(gm_jp_measure(&gm_jp_large, large[i], 2) <= GM_W - 8);
  }
  for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); ++i) CHECK(all_present(&gm_jp_small, small[i]));
  CHECK(gm_jp_large.height == 24 && gm_jp_small.height == 14);
  for (int i = 1; i < gm_jp_large.count; ++i)
    CHECK(gm_jp_large.glyphs[i - 1].codepoint < gm_jp_large.glyphs[i].codepoint);
  gm_frame_t* f = &s_f;
  gm_frame_clear(f);
  const int w = gm_jp_draw(f, &gm_jp_large, 2, 10, 10, "制動", 2, 1.0f);
  CHECK(w == gm_jp_measure(&gm_jp_large, "制動", 2));
  CHECK(lit_layer(f, 2) > 80);
  const int n = lit(f);
  gm_frame_fill_rect(f, 3, 0, 0, GM_W - 1, GM_H - 1, 1.0f);
  gm_jp_draw(f, &gm_jp_large, 3, 10, 10, "制動", 2, -1.0f);  // knock-out
  CHECK(lit_layer(f, 3) == GM_W * GM_H - n);
  gm_frame_clear(f);
  CHECK(gm_jp_draw_vertical(f, &gm_jp_small, 0, 100, 0, "正常", 1, 1.0f) == 2 * (gm_jp_small.height + 1));
}

TEST(hud_follows_g_and_names_the_state) {
  static gm_hud_t hud;
  gm_hud_init(&hud);
  gm_effect_activate(&hud.base);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.dt = 1.0f / 60.0f;
#define RUN(lat, vert, lon, secs)                                                    \
  do {                                                                               \
    ctx.g = dyn(lat, vert, lon);                                                     \
    for (int i_ = 0; i_ < (int)((secs) * 60.0f); ++i_) gm_effect_update(&hud.base, &ctx); \
  } while (0)
  RUN(0, 0, 0, 1.0f);
  CHECK(strcmp(gm_hud_title_text(&hud), "待機") == 0);
  RUN(0, 0, -0.5f, 2.0f);
  CHECK(strcmp(gm_hud_title_text(&hud), "制動") == 0);
  CHECK(gm_hud_ball_position(&hud).z > 10.0f);  // thrown toward the windshield
  CHECK(gm_vec3_length(gm_vec3_sub(gm_hud_ball_position(&hud), gm_hud_tip_position(&hud))) < 2.0f);  // caught the tip
  RUN(0.5f, 0, 0, 2.0f);
  CHECK(strcmp(gm_hud_title_text(&hud), "右旋回") == 0);
  CHECK(gm_hud_ball_position(&hud).x < -10.0f);
  RUN(1.2f, 0, 0, 1.0f);
  CHECK(strcmp(gm_hud_title_text(&hud), "危険") == 0);
  gm_frame_clear(&s_f);
  gm_effect_render(&hud.base, &s_f);
  for (int z = 0; z < 3; ++z) CHECK(lit_layer(&s_f, z) > 0);
  RUN(0, 0, 0, 2.0f);
  ctx.g = dyn(0, 0.6f, 0);
  gm_effect_update(&hud.base, &ctx);
  CHECK(strcmp(gm_hud_title_text(&hud), "衝撃") == 0);
#undef RUN
}

TEST(atfield_and_sync_react) {
  static gm_atfield_t at;
  gm_atfield_init(&at);
  gm_effect_activate(&at.base);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  ctx.dt = 1.0f / 60.0f;
  ctx.g = dyn(0.6f, 0, 0);
  for (int i = 0; i < 60; ++i) gm_effect_update(&at.base, &ctx);
  CHECK(gm_atfield_active_ripples(&at) >= 2);
  gm_frame_clear(&s_f);
  gm_effect_render(&at.base, &s_f);
  int layers = 0;
  for (int z = 0; z < GM_D; ++z) layers += lit_layer(&s_f, z) > 0;
  CHECK(layers >= 3);  // the wavefront crosses several panels
  ctx.g = gm_gforce_rest();
  for (int i = 0; i < 400; ++i) gm_effect_update(&at.base, &ctx);
  CHECK(gm_atfield_active_ripples(&at) <= 2);  // only the idle heartbeat remains

  static gm_sync_t sync;
  gm_sync_init(&sync);
  gm_effect_activate(&sync.base);
  ctx.g = dyn(0.5f, 0, -0.5f);
  for (int i = 0; i < 120; ++i) gm_effect_update(&sync.base, &ctx);
  gm_frame_clear(&s_f);
  gm_effect_render(&sync.base, &s_f);
  CHECK(lit_layer(&s_f, 0) > 50);                                                           // labels
  CHECK(lit_layer(&s_f, 1) > 20 && lit_layer(&s_f, 2) > 20 && lit_layer(&s_f, 3) > 20);  // three traces
}
