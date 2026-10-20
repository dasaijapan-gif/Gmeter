// Differential tests of the inline maths in gm_math.h / gm_gforce.h / gm_frame.h
// (vectors, matrices, rng, low-pass, spring, world mapping) against the C++ core.
#include <algorithm>
#include <cmath>

#include "diff_util.hpp"

using diff::Expect;
using diff::same;

namespace {

const float kSpecial[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1e-9f, -1e-9f, 1e-30f, 3.4e38f, -3.4e38f,
                          0.1f, 2.5f, -2.5f, 63.5f, -63.5f, 1e6f, diff::kInf, -diff::kInf, diff::kNaN};

float rnd_float(diff::Rand& r) {
  if (r.chance(0.15)) return r.pick(kSpecial);
  return r.frange(-300.0f, 300.0f);
}

gm_vec3_t rnd_vec(diff::Rand& r) {
  const float x = rnd_float(r), y = rnd_float(r), z = rnd_float(r);
  return gm_vec3(x, y, z);
}

}  // namespace

TEST(math_constants_and_geometry) {
  Expect ex;
  EXPECT(ex, same(GM_PI, gm::kPi));
  EXPECT(ex, same(GM_TWO_PI, gm::kTwoPi));
  EXPECT(ex, same(GM_STANDARD_GRAVITY, gm::kStandardGravity));
  EXPECT(ex, GM_W == gm::kW && GM_H == gm::kH && GM_D == gm::kD && GM_PAGES == gm::kPages);
  EXPECT(ex, GM_BYTES_PER_LAYER == gm::kBytesPerLayer && GM_BYTES_PER_FRAME == gm::kBytesPerFrame);
  EXPECT(ex, same(GM_LAYER_PITCH_PX, gm::kLayerPitchPx));
  EXPECT(ex, same(GM_DEPTH_PX, gm::kDepthPx));
  EXPECT(ex, sizeof(gm_frame_t) == static_cast<size_t>(gm::VoxelFrame::frameBytes()));
  EXPECT(ex, GM_RNG_DEFAULT_SEED == 0x9E3779B9u);
  EXPECT(ex, GM_PARAMS_MAX == gm::ParamTable::kMax);
  EXPECT(ex, GM_ENGINE_MAX_EFFECTS == gm::Engine::kMaxEffects);
}

TEST(math_scalar_helpers) {
  Expect ex;
  diff::Rand r(11);
  for (int i = 0; i < 20000; ++i) {
    const float a = rnd_float(r), b = rnd_float(r), c = rnd_float(r);
    EXPECT(ex, same(gm_clampf(a, b, c), gm::clampf(a, b, c)));
    EXPECT(ex, same(gm_clampf(a, -1.0f, 1.0f), gm::clampf(a, -1.0f, 1.0f)));
    EXPECT(ex, same(gm_lerpf(a, b, c), gm::lerpf(a, b, c)));
    EXPECT(ex, same(gm_smoothstepf(a, b, c), gm::smoothstepf(a, b, c)));
    EXPECT(ex, same(gm_smoothstepf(0.0f, 1.0f, c), gm::smoothstepf(0.0f, 1.0f, c)));
    EXPECT(ex, same(gm_signf(a), gm::signf(a)));
    // Same tie and NaN semantics as std::min / std::max.
    EXPECT(ex, same(gm_minf(a, b), std::min(a, b)));
    EXPECT(ex, same(gm_maxf(a, b), std::max(a, b)));
    if (std::isfinite(a) && std::fabs(a) < 1e9f) {
      EXPECT(ex, gm_ifloor(a) == gm::ifloor(a));
      EXPECT(ex, gm_iround(a) == gm::iround(a));
    }
    const int ia = r.irange(-1000, 1000), ib = r.irange(-1000, 1000), ic = r.irange(-1000, 1000);
    EXPECT(ex, gm_clampi(ia, ib, ic) == gm::clampi(ia, ib, ic));
    EXPECT(ex, gm_mini(ia, ib) == std::min(ia, ib) && gm_maxi(ia, ib) == std::max(ia, ib));
  }
  // Signed zeros: the first argument wins a tie.
  EXPECT(ex, same(gm_maxf(-0.0f, 0.0f), std::max(-0.0f, 0.0f)) && same(gm_minf(0.0f, -0.0f), std::min(0.0f, -0.0f)));
  for (float v : kSpecial)
    for (float lo : kSpecial)
      for (float hi : kSpecial) EXPECT(ex, same(gm_clampf(v, lo, hi), gm::clampf(v, lo, hi)));
}

TEST(math_vec3_and_mat3) {
  Expect ex;
  diff::Rand r(12);
  EXPECT(ex, same(gm_vec3_zero(), gm::Vec3{}));
  for (int i = 0; i < 20000; ++i) {
    const gm_vec3_t a = rnd_vec(r), b = rnd_vec(r);
    const float s = rnd_float(r);
    const gm::Vec3 A = diff::to_cpp(a), B = diff::to_cpp(b);
    EXPECT(ex, same(gm_vec3_add(a, b), A + B));
    EXPECT(ex, same(gm_vec3_sub(a, b), A - B));
    EXPECT(ex, same(gm_vec3_scale(a, s), A * s));
    EXPECT(ex, same(gm_vec3_scale(a, s), s * A));
    EXPECT(ex, same(gm_vec3_div(a, s), A / s));
    EXPECT(ex, same(gm_vec3_neg(a), -A));
    EXPECT(ex, same(gm_vec3_dot(a, b), A.dot(B)));
    EXPECT(ex, same(gm_vec3_cross(a, b), A.cross(B)));
    EXPECT(ex, same(gm_vec3_length2(a), A.length2()));
    EXPECT(ex, same(gm_vec3_length(a), A.length()));
    EXPECT(ex, same(gm_vec3_normalized(a), A.normalized()));
    EXPECT(ex, same(gm_vec3_mul(a, b), A.scaled(B)));
    EXPECT(ex, same(gm_vec3_lerp(a, b, s), gm::lerp(A, B, s)));
    gm::Vec3 acc = A;
    acc += B;
    acc -= A * s;
    acc *= s;
    EXPECT(ex, same(gm_vec3_scale(gm_vec3_sub(gm_vec3_add(a, b), gm_vec3_scale(a, s)), s), acc));
    // Tiny vectors around the normalisation threshold.
    const gm_vec3_t t = gm_vec3_scale(a, 1e-12f);
    EXPECT(ex, same(gm_vec3_normalized(t), diff::to_cpp(t).normalized()));

    const gm_vec3_t r0 = rnd_vec(r), r1 = rnd_vec(r), r2 = rnd_vec(r);
    const gm_mat3_t m = gm_mat3_from_rows(r0, r1, r2);
    const gm::Mat3 M = gm::Mat3::fromRows(diff::to_cpp(r0), diff::to_cpp(r1), diff::to_cpp(r2));
    EXPECT(ex, same(m, M));
    for (int k = 0; k < 3; ++k) EXPECT(ex, same(gm_mat3_row(&m, k), M.row(k)));
    EXPECT(ex, same(gm_mat3_apply(&m, a), M.apply(A)));
  }
  const gm_mat3_t id = gm_mat3_identity();
  EXPECT(ex, same(id, gm::Mat3::identity()) && same(id, gm::Mat3{}));
}

TEST(math_rng_sequences) {
  Expect ex;
  const uint32_t seeds[] = {0u, 1u, 2u, GM_RNG_DEFAULT_SEED, 0xFFFFFFFFu, 0x80000000u, 12345u, 0xDEADBEEFu};
  for (uint32_t seed : seeds) {
    gm_rng_t c;
    gm_rng_seed(&c, seed);
    gm::Rng p(seed);
    for (int i = 0; i < 5000; ++i) {
      switch (i % 4) {
        case 0: EXPECT(ex, gm_rng_next(&c) == p.next()); break;
        case 1: EXPECT(ex, same(gm_rng_uniform(&c), p.uniform())); break;
        case 2: EXPECT(ex, same(gm_rng_range(&c, -3.5f, 7.25f), p.range(-3.5f, 7.25f))); break;
        default: EXPECT(ex, same(gm_rng_symmetric(&c), p.symmetric())); break;
      }
    }
    // Re-seeding mid-stream.
    gm_rng_seed(&c, seed ^ 0x5555u);
    p.seed(seed ^ 0x5555u);
    for (int i = 0; i < 100; ++i) EXPECT(ex, gm_rng_next(&c) == p.next());
  }
  // The default-constructed C++ Rng starts from GM_RNG_DEFAULT_SEED.
  gm_rng_t c;
  gm_rng_seed(&c, GM_RNG_DEFAULT_SEED);
  gm::Rng p;
  for (int i = 0; i < 1000; ++i) EXPECT(ex, same(gm_rng_uniform(&c), p.uniform()));
}

TEST(math_lowpass) {
  Expect ex;
  diff::Rand r(13);
  struct Cfg {
    float cutoff, sample;
  };
  const Cfg cfgs[] = {{4.0f, 200.0f},  {0.0f, 200.0f},   {-1.0f, 200.0f}, {4.0f, 0.0f},     {4.0f, -200.0f},
                      {98.0f, 200.0f}, {97.99f, 200.0f}, {99.0f, 200.0f}, {0.001f, 200.0f}, {1.0f, 60.0f},
                      {25.0f, 1000.0f}, {3.0f, 100.0f},  {diff::kNaN, 200.0f}, {4.0f, diff::kInf}};
  // Default (gm_lowpass_init) is a pass-through like the C++ default.
  {
    gm_lowpass_t c;
    gm_lowpass_init(&c);
    gm::LowPassBiquad p;
    for (int i = 0; i < 200; ++i) {
      const float x = rnd_float(r);
      EXPECT(ex, same(gm_lowpass_process(&c, x), p.process(x)));
      EXPECT(ex, same(gm_lowpass_value(&c), p.value()));
    }
  }
  for (const Cfg& cfg : cfgs) {
    gm_lowpass_t c;
    gm_lowpass_init(&c);
    gm::LowPassBiquad p;
    gm_lowpass_configure(&c, cfg.cutoff, cfg.sample);
    p.configure(cfg.cutoff, cfg.sample);
    float level = 0.0f;
    for (int i = 0; i < 3000; ++i) {
      if (i % 700 == 0) {
        const float v = r.frange(-2.0f, 2.0f);
        gm_lowpass_reset(&c, v);
        p.reset(v);
      }
      if (i == 1500) {  // reconfigure mid-stream, keeping the history
        gm_lowpass_configure(&c, cfg.cutoff * 0.5f, cfg.sample);
        p.configure(cfg.cutoff * 0.5f, cfg.sample);
      }
      if (r.chance(0.02)) level = r.frange(-3.0f, 3.0f);
      const float x = level + r.frange(-0.2f, 0.2f);
      EXPECT(ex, same(gm_lowpass_process(&c, x), p.process(x)));
      EXPECT(ex, same(gm_lowpass_value(&c), p.value()));
    }
    // C++ reset() defaults to 0.
    gm_lowpass_reset(&c, 0.0f);
    p.reset();
    EXPECT(ex, same(gm_lowpass_process(&c, 1.0f), p.process(1.0f)));
  }
}

TEST(math_spring) {
  Expect ex;
  diff::Rand r(14);
  const float zetas[] = {-1.0f, 0.0f,    0.005f,  0.01f, 0.2f,  0.5f,   0.9f, 0.998f, 0.999f, 0.9995f, 1.0f,
                         1.0005f, 1.001f, 1.002f, 1.5f,  2.0f,  5.0f,   30.0f, 1000.0f};
  const float dts[] = {0.0f, 1e-9f, 1e-7f, 1e-6f, 1e-4f, 1.0f / 1000.0f, 1.0f / 200.0f, 1.0f / 60.0f,
                       1.0f / 15.0f, 0.1f, 0.5f, 1.0f, 10.0f, 100.0f};
  const float hzs[] = {0.0f, 1e-7f, 1e-3f, 0.3f, 1.0f, 2.5f, 6.0f, 20.0f, 100.0f, 2000.0f};
  for (float zeta : zetas)
    for (float dt : dts)
      for (float hz : hzs) {
        gm_spring_t c = {0.0f, 0.0f};
        gm::Spring p;
        const float start = r.frange(-50.0f, 50.0f);
        gm_spring_snap(&c, start);
        p.snap(start);
        EXPECT(ex, same(c.pos, p.pos) && same(c.vel, p.vel));
        float target = r.frange(-50.0f, 50.0f);
        for (int i = 0; i < 40; ++i) {
          if (i % 10 == 0) target = r.frange(-50.0f, 50.0f);
          gm_spring_step(&c, target, dt, hz, zeta);
          p.step(target, dt, hz, zeta);
          EXPECT(ex, same(c.pos, p.pos) && same(c.vel, p.vel));
        }
      }
  // Random parameters, and the C++ default zeta of 1.
  for (int n = 0; n < 2000; ++n) {
    gm_spring_t c = {0.0f, 0.0f};
    gm::Spring p;
    const float hz = r.frange(0.0f, 30.0f), dt = r.frange(0.0f, 0.2f), zeta = r.frange(0.0f, 3.0f);
    for (int i = 0; i < 30; ++i) {
      const float target = r.frange(-64.0f, 64.0f);
      if (n % 2) {
        gm_spring_step(&c, target, dt, hz, 1.0f);
        p.step(target, dt, hz);
      } else {
        gm_spring_step(&c, target, dt, hz, zeta);
        p.step(target, dt, hz, zeta);
      }
      EXPECT(ex, same(c.pos, p.pos) && same(c.vel, p.vel));
    }
  }
  // Spring3.
  for (int n = 0; n < 300; ++n) {
    gm_spring3_t c;
    std::memset(&c, 0, sizeof(c));
    gm::Spring3 p;
    const gm_vec3_t s0 = gm_vec3(r.frange(-60, 60), r.frange(-30, 30), r.frange(-60, 60));
    gm_spring3_snap(&c, s0);
    p.snap(diff::to_cpp(s0));
    const float hz = r.frange(0.5f, 12.0f), zeta = r.chance(0.5) ? 1.0f : r.frange(0.05f, 2.0f);
    for (int i = 0; i < 60; ++i) {
      const gm_vec3_t t = gm_vec3(r.frange(-60, 60), r.frange(-30, 30), r.frange(-60, 60));
      const float dt = r.frange(0.001f, 0.07f);
      gm_spring3_step(&c, t, dt, hz, zeta);
      p.step(diff::to_cpp(t), dt, hz, zeta);
      EXPECT(ex, same(gm_spring3_pos(&c), p.pos()));
      EXPECT(ex, same(c.x.vel, p.x.vel) && same(c.y.vel, p.y.vel) && same(c.z.vel, p.z.vel));
    }
  }
}

TEST(math_gforce_helpers) {
  Expect ex;
  diff::Rand r(15);
  const gm_gforce_t rest = GM_GFORCE_REST_INIT;
  EXPECT(ex, same(rest.proper, gm::GForce{}.proper));
  EXPECT(ex, same(gm_gforce_rest().proper, gm::GForce{}.proper));
  EXPECT(ex, same(gm_gforce_rest_proper(), gm::GForce::kRestProper()));
  for (int i = 0; i < 20000; ++i) {
    const gm_vec3_t v = rnd_vec(r);
    const gm_gforce_t c = gm_gforce_from_proper(v);
    gm::GForce p;
    p.proper = diff::to_cpp(v);
    EXPECT(ex, same(gm_gforce_dynamic(c), p.dynamic()));
    EXPECT(ex, same(gm_gforce_inertial(c), p.inertial()));
    EXPECT(ex, same(gm_gforce_effective_gravity(c), p.effectiveGravity()));
    EXPECT(ex, same(gm_gforce_horizontal(c), p.horizontalG()));
    EXPECT(ex, same(gm_gforce_lateral(c), p.lateralG()));
    EXPECT(ex, same(gm_gforce_longitudinal(c), p.longitudinalG()));
    EXPECT(ex, same(gm_gforce_vertical(c), p.verticalG()));
  }
}

TEST(math_world_mapping_and_bayer) {
  Expect ex;
  diff::Rand r(16);
  using VF = gm::VoxelFrame;
  for (int i = 0; i < 20000; ++i) {
    const float x = r.frange(-200.0f, 200.0f), y = r.frange(-200.0f, 200.0f), z = r.frange(-20.0f, 20.0f);
    EXPECT(ex, same(gm_voxel_to_world(x, y, z), VF::voxelToWorld(x, y, z)));
    const gm_vec3_t w = gm_vec3(x, y, z * 10.0f);
    EXPECT(ex, same(gm_world_to_voxel(w), VF::worldToVoxel(diff::to_cpp(w))));
  }
  for (int k = -3; k < GM_D + 3; ++k) EXPECT(ex, same(gm_layer_z(k), VF::layerZ(k)));
  EXPECT(ex, same(gm_half_extent(), VF::halfExtent()));
  for (int z = -9; z < 40; ++z)
    for (int y = -9; y < 20; ++y)
      for (int x = -9; x < 20; ++x) {
        EXPECT(ex, same(gm_frame_bayer_threshold(x, y, z), VF::bayerThreshold(x, y, z)));
        EXPECT(ex, gm_frame_in_bounds(x, y, z) == VF::inBounds(x, y, z));
      }
  const int edges[] = {-1000000, -129, -128, -65, -64, -57, -56, -1, 0, 1, 7, 8, 31, 32, 55, 56, 63, 64, 127, 128, 1000000};
  for (int x : edges)
    for (int y : edges)
      for (int z : edges) EXPECT(ex, gm_frame_in_bounds(x, y, z) == VF::inBounds(x, y, z));
}
