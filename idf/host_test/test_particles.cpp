// Differential tests of the "particles" effect (gm_particles.c vs
// gm::ParticleEffect): the parameter table and grid constants, the initial
// cloud, every scenario preset at 60 fps and with irregular dt, every tunable at
// its min, max and a middle value, particle count changes up and down mid-run,
// gravity_px, reset/activate/deactivate cycles mid-run, NaN/inf g, a long run
// and random fuzzing. After every update every one of the GM_PARTICLES_MAX
// positions (active or not) must match bit for bit, and every update batch is
// followed by a render into cleared frames that must match byte for byte.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "diff_util.hpp"

using diff::Expect;
using Particles = gm::ParticleEffect;

namespace {

gm::GForce dyn_g(float x, float y, float z) {
  gm::GForce g;
  g.proper = gm::GForce::kRestProper() + gm::Vec3{x, y, z};
  return g;
}

// dt for the irregular runs: mostly 20..500 fps, sometimes tiny, zero or far
// longer than the engine's 1/15 s clamp (the effect itself must cope).
float odd_dt(diff::Rand& r) {
  static const float kOdd[] = {0.0f,  1e-6f, 1e-4f, 1.0f / 1000.0f, 1.0f / 15.0f, 0.07f,
                               0.1f, 0.2f,  0.5f,  1.0f,           1.7f,         2.5f};
  return r.chance(0.12) ? r.pick(kOdd) : r.frange(0.002f, 0.05f);
}

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_particles_t> c{new gm_particles_t};
  std::unique_ptr<Particles> p{new Particles};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters (rendered frames).
  int on_wall = 0;     // active particles touching a wall
  int nan_pos = 0;     // active particles with a NaN coordinate
  int big_frames = 0;  // frames drawn with 2x2 dots

  Fx() { gm_particles_init(c.get()); }
  gm_effect_t* ce() { return &c->base; }

  void activate() {
    gm_effect_activate(ce());
    p->activate();
  }
  void deactivate() {
    gm_effect_deactivate(ce());
    p->deactivate();
  }
  void reset() {
    gm_effect_reset(ce());
    p->reset();
  }
  // Sets a tunable on both sides; true when both agree on the result.
  bool set(const char* name, float v) {
    const bool okc = gm_params_set(gm_effect_params(ce()), name, v);
    const bool okp = p->params().set(name, v);
    float vc = 0.0f, vp = 0.0f;
    const bool gc = gm_params_get(gm_effect_params(ce()), name, &vc);
    const bool gp = p->params().get(name, vp);
    return okc == okp && gc == gp && diff::same(vc, vp);
  }
  // Active count, gravity and every position (active or not).
  bool same_state() const {
    const gm_particles_t* s = c.get();
    if (gm_particles_active_count(s) != p->activeCount()) {
      harness::note("active count: C %d, C++ %d", gm_particles_active_count(s), p->activeCount());
      return false;
    }
    if (!diff::same(gm_particles_gravity_px(s), p->gravityPx())) {
      harness::note("gravity_px: C %.9g, C++ %.9g", static_cast<double>(gm_particles_gravity_px(s)),
                    static_cast<double>(p->gravityPx()));
      return false;
    }
    for (int i = 0; i < GM_PARTICLES_MAX; ++i)
      if (!diff::same(gm_particles_position(s, i), p->position(i))) {
        const gm_vec3_t a = gm_particles_position(s, i);
        const gm::Vec3& b = p->position(i);
        harness::note("particle %d: C (%.9g, %.9g, %.9g), C++ (%.9g, %.9g, %.9g)", i, static_cast<double>(a.x),
                      static_cast<double>(a.y), static_cast<double>(a.z), static_cast<double>(b.x),
                      static_cast<double>(b.y), static_cast<double>(b.z));
        return false;
      }
    return true;
  }
  // Advances both; true when the states match afterwards.
  bool update(const gm::GForce& g, float dt) {
    pctx.g = g;
    pctx.dt = dt;
    pctx.time += dt;
    ++pctx.frame;
    cctx.g = gm_gforce_from_proper(diff::to_c(g.proper));
    cctx.dt = dt;
    cctx.time = pctx.time;
    cctx.frame = pctx.frame;
    gm_effect_update(ce(), &cctx);
    p->update(pctx);
    return same_state();
  }
  // Renders both into cleared frames; true when the frames (and states) match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    bool ok = f.same(label);
    if (!same_state()) {
      harness::note("%s: state differs", label);
      ok = false;
    }
    // Coverage.
    const gm_vec3_t he = gm_half_extent();
    for (int i = 0; i < gm_particles_active_count(c.get()); ++i) {
      const gm_vec3_t x = c->pos[i];
      if (std::isnan(x.x) || std::isnan(x.y) || std::isnan(x.z))
        ++nan_pos;
      else if (std::fabs(x.x) >= he.x - 0.5f || std::fabs(x.y) >= he.y - 0.5f || std::fabs(x.z) >= he.z - 0.5f)
        ++on_wall;
    }
    if (c->dot_size >= 1.5f) ++big_frames;
    return ok;
  }
};

bool same_param_tables(const gm_param_table_t* c, const gm::ParamTable& p) {
  if (gm_params_count(c) != p.count()) {
    harness::note("param count C %d, C++ %d", gm_params_count(c), p.count());
    return false;
  }
  bool ok = true;
  for (int i = 0; i < p.count(); ++i) {
    const gm_param_t* a = gm_params_at(c, i);
    const gm::Param& b = p.at(i);
    if (std::strcmp(a->name, b.name) != 0 || !harness::same_float(a->min, b.min) ||
        !harness::same_float(a->max, b.max) || !harness::same_float(*a->value, *b.value) ||
        std::strcmp(a->help, b.help) != 0) {
      harness::note("param %d differs: C %s [%g, %g] = %g \"%s\", C++ %s [%g, %g] = %g \"%s\"", i, a->name,
                    static_cast<double>(a->min), static_cast<double>(a->max), static_cast<double>(*a->value), a->help,
                    b.name, static_cast<double>(b.min), static_cast<double>(b.max), static_cast<double>(*b.value),
                    b.help);
      ok = false;
    }
  }
  return ok;
}

enum class Dt { k60, kIrregular };

// Plays preset `sc` (looped) for `seconds` into both effects, comparing every
// position after every update and rendering and comparing after every update
// batch. With `events`, resets and deactivate/activate cycles happen mid-run
// (some at fixed times, some at random). Stops at the first mismatch. Returns
// the rendered frame count.
int play(Expect& ex, Fx& fx, const gm::Scenario& sc, gm::Rng& rng, diff::Rand& r, Dt mode, float seconds,
         const char* tag, bool events) {
  const float loop = sc.duration() + 0.5f;
  float t = 0.0f;
  int frames = 0;
  char label[192];
  while (t < seconds && frames < 200000) {
    const int updates = mode == Dt::kIrregular && r.chance(0.15) ? r.irange(0, 3) : 1;
    bool ok = true;
    for (int u = 0; u < updates && ok; ++u) {
      const float dt = mode == Dt::k60 ? 1.0f / 60.0f : odd_dt(r);
      t += dt;
      ok = fx.update(sc.at(std::fmod(t, loop), &rng), dt);
    }
    ++frames;
    std::snprintf(label, sizeof(label), "%s %s frame %d t=%.4f", tag, sc.name(), frames, static_cast<double>(t));
    if (!EXPECT(ex, ok)) {
      harness::note("%s: positions differ after an update", label);
      break;
    }
    if (events) {
      if (frames == 420) fx.reset();
      if (frames == 840) {
        fx.deactivate();
        fx.activate();
      }
      if (r.chance(0.004)) {
        switch (r.irange(0, 3)) {
          case 0: fx.reset(); break;
          case 1: fx.deactivate(); fx.activate(); break;
          case 2: fx.activate(); break;
          default: fx.deactivate(); break;  // keeps running after a deactivate, like a crossfade-out
        }
      }
    }
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

}  // namespace

TEST(particles_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "particles") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // The vtable: activate falls back to reset, nothing to free, no burn-in offset.
  EXPECT(ex, fx.ce()->vt->activate == nullptr);
  EXPECT(ex, fx.ce()->vt->deactivate == nullptr);
  EXPECT(ex, fx.ce()->vt->reset != nullptr);
  EXPECT(ex, fx.ce()->vt->burn_in_offset == nullptr);
  int dxc = 5, dyc = 5, dxp = 5, dyp = 5;
  gm_effect_burn_in_offset(fx.ce(), &dxc, &dyc);
  fx.p->burnInOffset(dxp, dyp);
  EXPECT(ex, dxc == 0 && dyc == 0 && dxp == 0 && dyp == 0);
  // Grid geometry.
  EXPECT(ex, GM_PARTICLES_MAX == Particles::kMaxParticles && GM_PARTICLES_CELL == Particles::CELL);
  EXPECT(ex, GM_PARTICLES_GX == Particles::GX && GM_PARTICLES_GY == Particles::GY && GM_PARTICLES_GZ == Particles::GZ);
  harness::note("density grid %dx%dx%d (padded), struct %d bytes", GM_PARTICLES_GX, GM_PARTICLES_GY, GM_PARTICLES_GZ,
                static_cast<int>(sizeof(gm_particles_t)));
  // Each name points at the field the header documents.
  gm_particles_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"count", &c->count},       {"tank_m", &c->tank_width_m}, {"drag", &c->drag},
                {"bounce", &c->restitution}, {"pressure", &c->pressure},  {"rho0", &c->rest_density},
                {"jitter", &c->jitter},     {"dot", &c->dot_size},        {"substeps", &c->substeps}};
  const int nfields = static_cast<int>(sizeof(fields) / sizeof(fields[0]));
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == nfields);
  for (int i = 0; i < nfields; ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));
  }
  // The constructor resets: the initial cloud (all GM_PARTICLES_MAX particles)
  // matches, velocities are zero, and the rng is where the C++ one is.
  EXPECT(ex, fx.same_state());
  bool still = true;
  for (int i = 0; i < GM_PARTICLES_MAX; ++i) still = still && diff::same(c->vel[i], gm::Vec3{});
  EXPECT(ex, still);
  gm::Rng after(777u);
  for (int i = 0; i < GM_PARTICLES_MAX * 3; ++i) after.next();
  gm_rng_t crng = c->rng;
  EXPECT(ex, gm_rng_next(&crng) == after.next());
  EXPECT(ex, fx.render_same("fresh render"));
  EXPECT(ex, gm_frame_count_lit(fx.f.c.get()) > 0);
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.1f, 0.5f, 1.0f, 1.5f, 2.0f, 5.0f, 16.0f, 399.9f, 400.0f, 401.0f,
                          1e9f,  diff::kInf, -diff::kInf, diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
}

TEST(particles_gravity_px) {
  Expect ex;
  diff::Rand r(4101);
  Fx fx;
  const float tanks[] = {0.1f, 0.10001f, 0.333f, 1.0f, 2.5f, 5.05f, 7.77f, 9.99f, 10.0f};
  for (float t : tanks) {
    EXPECT(ex, fx.set("tank_m", t));
    EXPECT(ex, diff::same(gm_particles_gravity_px(fx.c.get()), fx.p->gravityPx()));
  }
  for (int i = 0; i < 2000; ++i) {
    EXPECT(ex, fx.set("tank_m", r.frange(0.05f, 11.0f)));
    EXPECT(ex, diff::same(gm_particles_gravity_px(fx.c.get()), fx.p->gravityPx()));
  }
  // A virtual tank of 1 m: 1 g is 9.80665 m/s^2 over 128 px per metre.
  EXPECT(ex, fx.set("tank_m", 1.0f));
  EXPECT(ex, gm_particles_gravity_px(fx.c.get()) == GM_STANDARD_GRAVITY * 128.0f);
}

TEST(particles_presets_60fps) {
  Expect ex;
  diff::Rand r(4102);
  int on_wall = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", false);
    EXPECT(ex, frames >= 22 * 60);
    on_wall += fx.on_wall;
  }
  harness::note("particle-frames touching a wall: %d", on_wall);
  CHECK(on_wall > 100000);
}

TEST(particles_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(4103);
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      if (rep) EXPECT(ex, fx.set("dot", 2.0f));  // also the 2x2 dots
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", false);
    }
}

TEST(particles_events) {
  Expect ex;
  diff::Rand r(4104);
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    for (int mode = 0; mode < 2; ++mode) {
      Fx fx;
      fx.activate();
      gm::Rng rng(2500u + static_cast<uint32_t>(i * 3 + mode));
      play(ex, fx, sc, rng, r, mode ? Dt::kIrregular : Dt::k60, 24.0f, mode ? "events irregular" : "events 60fps",
           true);
    }
  }
  for (int k = 0; k < 2; ++k) {
    Fx fx;
    if (k) fx.deactivate();
    gm::Rng rng(2600u + static_cast<uint32_t>(k));
    play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 21.0f, k ? "deactivated" : "never activated", false);
  }
}

TEST(particles_tunables) {
  Expect ex;
  diff::Rand r(4105);
  Particles proto;
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
  int big = 0;
  for (int i = 0; i < proto.params().count(); ++i) {
    const gm::Param& prm = proto.params().at(i);
    const float values[] = {prm.min, prm.max, 0.5f * (prm.min + prm.max)};
    for (float v : values) {
      char tag[96];
      std::snprintf(tag, sizeof(tag), "%s=%g", prm.name, static_cast<double>(v));
      Fx fx;
      EXPECT(ex, fx.set(prm.name, v));
      fx.activate();
      EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
      gm::Rng rng(3000u + static_cast<uint32_t>(i));
      play(ex, fx, drive, rng, r, Dt::k60, 22.0f, tag, false);
      play(ex, fx, bump, rng, r, Dt::kIrregular, 6.0f, tag, true);
      // Hard driving: the pile thrown against every wall in turn.
      char label[160];
      float t = 0.0f;
      for (int frame = 0; frame < 600; ++frame) {
        t += 1.0f / 60.0f;
        const float s = std::sin(t * 1.7f), q = std::cos(t * 0.9f);
        if (!EXPECT(ex, fx.update(dyn_g(2.5f * s, 1.2f * s * q, -2.0f * q), 1.0f / 60.0f))) break;
        std::snprintf(label, sizeof(label), "%s hard frame %d", tag, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
      // Change it again mid-run.
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag, false);
      big += fx.big_frames;
    }
  }
  harness::note("frames with 2x2 dots: %d", big);
  CHECK(big > 1000);
}

TEST(particles_count_changes) {
  Expect ex;
  diff::Rand r(4106);
  // Fewer particles freeze the rest where they are; more particles bring them
  // back from where they were frozen (or from the initial cloud).
  const float counts[] = {300.0f, 1.0f, 400.0f, 37.0f, 2.9f, 399.99f, 150.5f, 1.0f, 250.0f, 400.0f, 12.0f, 300.0f};
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  for (int mode = 0; mode < 2; ++mode) {
    Fx fx;
    fx.activate();
    gm::Rng rng(4200u + static_cast<uint32_t>(mode));
    int ups = 0, downs = 0;
    for (int step = 0; step < static_cast<int>(sizeof(counts) / sizeof(counts[0])); ++step) {
      const int before = gm_particles_active_count(fx.c.get());
      EXPECT(ex, fx.set("count", counts[step]));
      const int after = gm_particles_active_count(fx.c.get());
      if (after > before) ++ups;
      if (after < before) ++downs;
      // Inactive particles stay where they are.
      std::unique_ptr<gm_vec3_t[]> frozen(new gm_vec3_t[GM_PARTICLES_MAX]);
      std::memcpy(frozen.get(), fx.c->pos, sizeof(fx.c->pos));
      char tag[64];
      std::snprintf(tag, sizeof(tag), "count=%g", static_cast<double>(counts[step]));
      play(ex, fx, drive, rng, r, mode ? Dt::kIrregular : Dt::k60, 2.5f, tag, false);
      bool kept = true;
      for (int i = after; i < GM_PARTICLES_MAX; ++i) kept = kept && diff::same(fx.c->pos[i], frozen[i]);
      EXPECT(ex, kept);
    }
    EXPECT(ex, ups >= 4 && downs >= 4);
  }
  // Random count changes on every few frames.
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(4300u);
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 1500; ++frame) {
      if (frame % 7 == 0) EXPECT(ex, fx.set("count", r.frange(-10.0f, 420.0f)));
      const float dt = odd_dt(r);
      t += dt;
      if (!EXPECT(ex, fx.update(drive.at(std::fmod(t, drive.duration()), &rng), dt))) break;
      std::snprintf(label, sizeof(label), "random count frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(particles_nan_inf_g) {
  Expect ex;
  diff::Rand r(4107);
  // NaN or infinite g makes positions NaN (as in the C++) until the next reset;
  // the grid lookups and the render must cope without touching memory out of
  // range, and the frames must keep matching.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, -3e38f, 1e6f, -1e6f, 1e-30f};
  int nan_pos = 0;
  for (int run = 0; run < 40; ++run) {
    Fx fx;
    if (run % 3 == 1) EXPECT(ex, fx.set("dot", 2.0f));
    if (run % 4 == 2) EXPECT(ex, fx.set("jitter", 0.0f));
    if (run % 5 == 3) EXPECT(ex, fx.set("substeps", 6.0f));
    fx.activate();
    gm::Rng rng(6000u + static_cast<uint32_t>(run));
    const gm::Scenario& sc = gm::Scenario::preset(run % gm::Scenario::presetCount());
    char label[160];
    float t = 0.0f;
    for (int frame = 0; frame < 900; ++frame) {
      const float dt = run % 2 ? odd_dt(r) : 1.0f / 60.0f;
      t += dt;
      gm::GForce g = sc.at(std::fmod(t, sc.duration() + 0.5f), &rng);
      if (frame % 150 >= 60 && frame % 150 < 60 + (run % 5) * 6 + 1) {
        // A burst of bad samples: some components, or all of them.
        const int mask = r.irange(1, 7);
        float* comp[3] = {&g.proper.x, &g.proper.y, &g.proper.z};
        for (int k = 0; k < 3; ++k)
          if (mask & (1 << k)) *comp[k] = r.pick(bad);
      }
      if (!EXPECT(ex, fx.update(g, dt))) {
        harness::note("bad g run %d frame %d: positions differ", run, frame);
        break;
      }
      if (r.chance(0.01)) fx.reset();
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    nan_pos += fx.nan_pos;
  }
  harness::note("particle-frames with a NaN coordinate: %d", nan_pos);
  CHECK(nan_pos > 100000);
  // All-NaN from the very first update, with 2x2 dots.
  {
    Fx fx;
    EXPECT(ex, fx.set("dot", 2.0f));
    fx.activate();
    gm::GForce g;
    g.proper = gm::Vec3{diff::kNaN, diff::kNaN, diff::kNaN};
    for (int frame = 0; frame < 120; ++frame) {
      if (!EXPECT(ex, fx.update(g, 1.0f / 60.0f))) break;
      if (!EXPECT(ex, fx.render_same("all NaN"))) break;
    }
    EXPECT(ex, gm_frame_count_lit(fx.f.c.get()) == 0);
    fx.reset();
    EXPECT(ex, fx.same_state());
    EXPECT(ex, fx.render_same("after reset"));
    EXPECT(ex, gm_frame_count_lit(fx.f.c.get()) > 0);
  }
}

TEST(particles_long_run) {
  Expect ex;
  diff::Rand r(4108);
  // 10 minutes at 60 fps ...
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps", false);
    EXPECT(ex, frames >= 600 * 60);
  }
  // ... and 30 minutes at the engine's slowest step, all particles, stiff and bouncy.
  {
    Fx fx;
    EXPECT(ex, fx.set("count", 400.0f));
    EXPECT(ex, fx.set("bounce", 1.0f));
    EXPECT(ex, fx.set("pressure", 80000.0f));
    EXPECT(ex, fx.set("tank_m", 0.1f));
    fx.activate();
    gm::Rng rng(7001u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 1800 * 15; ++frame) {
      t += 1.0f / 15.0f;
      if (!EXPECT(ex, fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f))) break;
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(particles_fuzz) {
  Expect ex;
  diff::Rand r(4109);
  Particles proto;
  for (int run = 0; run < 100; ++run) {
    Fx fx;
    for (int i = 0; i < proto.params().count(); ++i) {
      const gm::Param& prm = proto.params().at(i);
      if (r.chance(0.6)) EXPECT(ex, fx.set(prm.name, r.frange(prm.min, prm.max)));
    }
    fx.activate();
    gm::Vec3 dyn, target;
    char label[128];
    for (int frame = 0; frame < 600; ++frame) {
      if (r.chance(0.03))
        target = gm::Vec3{r.frange(-2.5f, 2.5f), r.chance(0.2) ? r.frange(-0.8f, 0.8f) : 0.0f, r.frange(-2.5f, 2.5f)};
      dyn = gm::lerp(dyn, target, 0.1f);
      gm::Vec3 d = dyn;
      if (r.chance(0.04)) d.y += r.frange(-1.5f, 1.5f);
      if (r.chance(0.01)) d = gm::Vec3{r.frange(-6, 6), r.frange(-6, 6), r.frange(-6, 6)};
      const int updates = r.chance(0.1) ? r.irange(0, 3) : 1;
      bool ok = true;
      for (int u = 0; u < updates && ok; ++u) ok = fx.update(dyn_g(d.x, d.y, d.z), odd_dt(r));
      std::snprintf(label, sizeof(label), "fuzz run %d frame %d", run, frame);
      if (!EXPECT(ex, ok)) {
        harness::note("%s: positions differ after an update", label);
        break;
      }
      if (r.chance(0.01)) {
        const gm::Param& prm = proto.params().at(r.irange(0, proto.params().count() - 1));
        EXPECT(ex, fx.set(prm.name, r.frange(prm.min - 1.0f, prm.max + 1.0f)));
      }
      if (r.chance(0.005)) fx.reset();
      if (r.chance(0.005)) {
        fx.deactivate();
        fx.activate();
      }
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}
