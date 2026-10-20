// Differential tests of the "ball" effect (gm_gball.c vs gm::GBallEffect): the
// parameter table, every scenario preset at 60 fps and with irregular dt,
// every tunable at its min, max and a middle value, reset/activate/deactivate
// cycles mid-run, peak hold and trail bookkeeping, readouts of huge g, NaN/inf
// g, long runs and random fuzzing. After every update the ball position must
// match bit for bit, and every update batch is followed by a render into
// cleared frames that must match byte for byte.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "diff_util.hpp"

using diff::Expect;

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
  std::unique_ptr<gm_gball_t> c{new gm_gball_t};
  std::unique_ptr<gm::GBallEffect> p{new gm::GBallEffect};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters (rendered frames).
  int full_trail = 0;   // trail ring buffer full (it wraps)
  int decaying = 0;     // peak held longer than 3 s (decaying)
  int pinned = 0;       // ball near a clamp limit
  int big_readout = 0;  // readout of 10 g or more, or non-finite

  Fx() { gm_gball_init(c.get()); }
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
  bool same_position() const { return diff::same(gm_gball_position(c.get()), p->position()); }
  // Advances both; true when the ball positions match afterwards.
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
    return same_position();
  }
  // Renders both into cleared frames; true when the frames (and positions) match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    bool ok = f.same(label);
    if (!same_position()) {
      harness::note("%s: position differs", label);
      ok = false;
    }
    // Coverage.
    if (c->trail_count == GM_GBALL_TRAIL_MAX) ++full_trail;
    if (c->peak_age > 3.0f) ++decaying;
    const gm_vec3_t he = gm_half_extent();
    const gm_vec3_t pos = gm_gball_position(c.get());
    if (std::fabs(pos.x) > 0.95f * (he.x - c->radius - 1.0f) || std::fabs(pos.y) > 0.95f * (he.y - c->radius - 1.0f) ||
        std::fabs(pos.z) > 0.95f * (he.z - GM_LAYER_PITCH_PX * 0.5f))
      ++pinned;
    const float hg = gm_gforce_horizontal(c->last_g);
    if (!(hg < 10.0f) || !(c->peak_g < 10.0f)) ++big_readout;
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

// Plays preset `sc` (looped) for `seconds` into both effects, comparing the
// positions after every update and rendering and comparing after every update
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
    for (int u = 0; u < updates; ++u) {
      const float dt = mode == Dt::k60 ? 1.0f / 60.0f : odd_dt(r);
      t += dt;
      ok = fx.update(sc.at(std::fmod(t, loop), &rng), dt) && ok;
    }
    ++frames;
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
    std::snprintf(label, sizeof(label), "%s %s frame %d t=%.4f", tag, sc.name(), frames, static_cast<double>(t));
    if (!EXPECT(ex, ok)) {
      harness::note("%s: position differs after an update", label);
      break;
    }
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

}  // namespace

TEST(gball_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "ball") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // The vtable: activate falls back to reset, no burn-in offset.
  EXPECT(ex, fx.ce()->vt->activate == nullptr);
  EXPECT(ex, fx.ce()->vt->deactivate == nullptr);
  EXPECT(ex, fx.ce()->vt->reset != nullptr);
  EXPECT(ex, fx.ce()->vt->burn_in_offset == nullptr);
  int dxc = 5, dyc = 5, dxp = 5, dyp = 5;
  gm_effect_burn_in_offset(fx.ce(), &dxc, &dyc);
  fx.p->burnInOffset(dxp, dyp);
  EXPECT(ex, dxc == 0 && dyc == 0 && dxp == 0 && dyp == 0);
  EXPECT(ex, GM_GBALL_TRAIL_MAX == 48);
  // Each name points at the field the header documents.
  gm_gball_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"scale_g", &c->full_scale_g},  {"vscale_g", &c->vertical_scale_g},
                {"dscale_g", &c->depth_scale_g}, {"hz", &c->natural_hz},
                {"zeta", &c->damping},           {"radius", &c->radius},
                {"depth_r", &c->depth_radius_layers}, {"trail_s", &c->trail_seconds},
                {"trail_i", &c->trail_intensity}, {"inertia", &c->follow_inertia},
                {"hud", &c->hud},                {"rings", &c->rings}};
  const int nfields = static_cast<int>(sizeof(fields) / sizeof(fields[0]));
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == nfields);
  for (int i = 0; i < nfields; ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));
  }
  // The rest of the state starts at rest: ball centred and still, no trail,
  // no peak, last g = a car at rest (not free fall).
  EXPECT(ex, c->trail_head == 0 && c->trail_count == 0 && c->trail_timer == 0.0f);
  EXPECT(ex, c->peak_g == 0.0f && c->peak_age == 0.0f);
  EXPECT(ex, c->spring.x.pos == 0.0f && c->spring.y.pos == 0.0f && c->spring.z.pos == 0.0f);
  EXPECT(ex, c->spring.x.vel == 0.0f && c->spring.y.vel == 0.0f && c->spring.z.vel == 0.0f);
  EXPECT(ex, diff::same(c->last_g.proper, gm::GForce().proper));
  EXPECT(ex, fx.same_position());
  EXPECT(ex, fx.render_same("fresh render"));
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.1f, 0.5f, 1.0f, 2.0f, 5.0f, 16.0f, 121.0f, 1e9f, diff::kInf,
                          -diff::kInf, diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
}

TEST(gball_presets_60fps) {
  Expect ex;
  diff::Rand r(2201);
  int full = 0, decaying = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", false);
    EXPECT(ex, frames >= 22 * 60);
    full += fx.full_trail;
    decaying += fx.decaying;
  }
  harness::note("frames with a full trail: %d, with a decaying peak: %d", full, decaying);
  CHECK(full > 1000);
  CHECK(decaying > 100);
}

TEST(gball_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(2202);
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      if (rep) EXPECT(ex, fx.set("zeta", 0.45f));  // also the under-damped spring branch
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", false);
    }
}

TEST(gball_events) {
  Expect ex;
  diff::Rand r(2203);
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

TEST(gball_tunables) {
  Expect ex;
  diff::Rand r(2204);
  gm::GBallEffect proto;
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
  int pinned = 0;
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
      // Hard driving: the ball pinned against every wall in turn, readouts over 1 g.
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
      pinned += fx.pinned;
    }
  }
  harness::note("frames with the ball near a wall: %d", pinned);
  CHECK(pinned > 5000);
}

TEST(gball_peak_and_trail) {
  Expect ex;
  diff::Rand r(2205);
  // Readouts: around the .00/.99 boundaries, beyond 10 g, and finite values
  // whose hundredths (or the whole value) overflow int (the C++ conversion is
  // undefined there; x86 gives INT_MIN, and so does the C).
  const float levels[] = {0.0f,  0.004f, 0.005f, 0.0051f, 0.099f, 0.1f,   0.995f, 0.999f, 1.0f,
                          1.005f, 2.37f, 9.99f,  10.0f,   12.34f, 99.99f, 123.4f, 5e6f,   2.1e7f,
                          2.2e7f, 1e9f,  2.2e9f, 3e9f,    1e20f};
  // Trail lengths around the 0.025 s minimum sample interval (47 * 0.025 = 1.175 s).
  const float trails[] = {0.0f, 0.001f, 0.3f, 1.175f, 1.2f, 2.0f, 4.0f};
  for (int run = 0; run < 70; ++run) {
    Fx fx;
    EXPECT(ex, fx.set("trail_s", trails[run % 7]));
    if (run % 5 == 1) EXPECT(ex, fx.set("trail_i", 1.0f));
    if (run % 5 == 2) EXPECT(ex, fx.set("trail_i", 0.0f));
    if (run % 4 == 3) EXPECT(ex, fx.set("inertia", 0.0f));
    fx.activate();
    char label[160];
    float level = 0.0f, angle = 0.0f;
    for (int frame = 0; frame < 900; ++frame) {
      // Holds a level for a while (peak held 3 s, then decaying), jumps around.
      if (frame % 150 == 0) level = r.chance(0.7) ? r.pick(levels) : r.frange(0.0f, 3.0f);
      if (frame % 150 == 40 && r.chance(0.6)) level = 0.0f;
      angle += 0.05f;
      const float dt = run % 2 ? odd_dt(r) : (run % 3 ? 1.0f / 60.0f : r.pick(trails) * 0.6f + 0.004f);
      const gm::GForce g = dyn_g(level * std::cos(angle), r.frange(-0.1f, 0.1f), level * std::sin(angle));
      if (!EXPECT(ex, fx.update(g, dt))) break;
      if (r.chance(0.01)) fx.reset();
      std::snprintf(label, sizeof(label), "peak/trail run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
  // Binary-exact steps: the peak age lands exactly on 3 s (still held there,
  // decaying after) and trail samples exactly on the sample interval.
  const float exact[] = {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 0.025f};
  int held_at_3 = 0;
  for (float step : exact)
    for (int variant = 0; variant < 2; ++variant) {
      Fx fx;
      if (variant) EXPECT(ex, fx.set("trail_s", 0.5f));
      fx.activate();
      char label[160];
      for (int frame = 0; frame < 400; ++frame) {
        const float level = frame % 100 < 3 ? 1.5f + 0.25f * static_cast<float>(variant) : 0.375f;
        if (!EXPECT(ex, fx.update(dyn_g(level, 0.0f, 0.0f), step))) break;
        if (fx.c->peak_age == 3.0f) ++held_at_3;
        std::snprintf(label, sizeof(label), "exact step %g variant %d frame %d", static_cast<double>(step), variant,
                      frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
    }
  harness::note("updates with the peak age exactly 3 s: %d", held_at_3);
  CHECK(held_at_3 >= 10);
}

TEST(gball_nan_inf_g) {
  Expect ex;
  diff::Rand r(2206);
  // The C++ handles all of these without undefined behaviour except the
  // int conversions in the readout (x86: INT_MIN, which the C reproduces);
  // the ball itself stays finite because its target is clamped NaN-safely.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, -3e38f, 1e6f, -1e6f, 1e-30f};
  int big = 0;
  for (int run = 0; run < 40; ++run) {
    Fx fx;
    if (run % 3 == 1) EXPECT(ex, fx.set("inertia", 0.0f));
    if (run % 4 == 2) EXPECT(ex, fx.set("zeta", 0.3f));
    if (run % 5 == 3) EXPECT(ex, fx.set("zeta", 2.0f));
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
      if (!EXPECT(ex, fx.update(g, dt))) break;
      const gm_vec3_t pos = gm_gball_position(fx.c.get());
      EXPECT(ex, std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z));
      if (r.chance(0.01)) fx.reset();
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    big += fx.big_readout;
  }
  harness::note("frames with a huge or non-finite readout: %d", big);
  CHECK(big > 1000);
}

TEST(gball_long_run) {
  Expect ex;
  diff::Rand r(2207);
  // 10 minutes at 60 fps (the trail ring wraps thousands of times) ...
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps", false);
    EXPECT(ex, frames >= 600 * 60);
  }
  // ... and an hour at the engine's slowest step with a lively spring.
  {
    Fx fx;
    EXPECT(ex, fx.set("hz", 8.0f));
    EXPECT(ex, fx.set("zeta", 0.2f));
    fx.activate();
    gm::Rng rng(7001u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 3600 * 15; ++frame) {
      t += 1.0f / 15.0f;
      if (!EXPECT(ex, fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f))) break;
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(gball_fuzz) {
  Expect ex;
  diff::Rand r(2208);
  gm::GBallEffect proto;
  for (int run = 0; run < 120; ++run) {
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
      for (int u = 0; u < updates; ++u) ok = fx.update(dyn_g(d.x, d.y, d.z), odd_dt(r)) && ok;
      if (!EXPECT(ex, ok)) break;
      if (r.chance(0.01)) {
        const gm::Param& prm = proto.params().at(r.irange(0, proto.params().count() - 1));
        EXPECT(ex, fx.set(prm.name, r.frange(prm.min - 1.0f, prm.max + 1.0f)));
      }
      if (r.chance(0.005)) fx.reset();
      if (r.chance(0.005)) {
        fx.deactivate();
        fx.activate();
      }
      std::snprintf(label, sizeof(label), "fuzz run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}
