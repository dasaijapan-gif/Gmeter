// Differential tests of the "tunnel" effect (gm_tunnel.c vs gm::TunnelEffect):
// the parameter table, every scenario preset at 60 fps and with irregular dt,
// every tunable at its min, max and a middle value, the bend at its limits,
// spin and depth-pulse wrap-around in both directions, fractional segment
// counts, reset/activate/deactivate cycles, NaN/inf g and long runs. Every
// update is followed by a render into cleared frames that must match byte for
// byte.
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
  static const float kOdd[] = {0.0f,  1e-7f, 1e-6f, 1e-4f, 1.0f / 1000.0f, 1.0f / 15.0f, 0.07f,
                               0.1f, 0.2f,  0.5f,  1.0f,  1.7f,           2.5f};
  return r.chance(0.12) ? r.pick(kOdd) : r.frange(0.002f, 0.05f);
}

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_tunnel_t> c{new gm_tunnel_t};
  std::unique_ptr<gm::TunnelEffect> p{new gm::TunnelEffect};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  int lit_frames = 0;  // coverage: frames with something drawn
  int renders = 0;
  int wraps_up = 0, wraps_down = 0;  // coverage: spin phase wrapped past 2 pi / below 0

  Fx() { gm_tunnel_init(c.get()); }
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
  void update(const gm::GForce& g, float dt) {
    pctx.g = g;
    pctx.dt = dt;
    pctx.time += dt;
    ++pctx.frame;
    cctx.g = gm_gforce_from_proper(diff::to_c(g.proper));
    cctx.dt = dt;
    cctx.time = pctx.time;
    cctx.frame = pctx.frame;
    const float before = c->phase;
    const float rate = c->spin + c->spin_per_g * gm_gforce_lateral(cctx.g);
    gm_effect_update(ce(), &cctx);
    p->update(pctx);
    if (rate > 0.0f && c->phase < before) ++wraps_up;
    if (rate < 0.0f && c->phase > before) ++wraps_down;
  }
  // Renders both into cleared frames; true when the frames match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    ++renders;
    if (gm_frame_count_lit(f.c.get()) > 0) ++lit_frames;
    return f.same(label);
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

// Plays preset `sc` (looped) for `seconds` into both effects, rendering and
// comparing after every update batch. With `events`, resets and
// deactivate/activate cycles happen mid-run (some at fixed times, some at
// random). Stops at the first mismatch. Returns the rendered frame count.
int play(Expect& ex, Fx& fx, const gm::Scenario& sc, gm::Rng& rng, diff::Rand& r, Dt mode, float seconds,
         const char* tag, bool events) {
  const float loop = sc.duration() + 0.5f;
  float t = 0.0f;
  int frames = 0;
  char label[192];
  while (t < seconds && frames < 200000) {
    const int updates = mode == Dt::kIrregular && r.chance(0.15) ? r.irange(0, 3) : 1;
    for (int u = 0; u < updates; ++u) {
      const float dt = mode == Dt::k60 ? 1.0f / 60.0f : odd_dt(r);
      t += dt;
      fx.update(sc.at(std::fmod(t, loop), &rng), dt);
    }
    ++frames;
    if (events) {
      if (frames == 420) fx.reset();
      if (frames == 840) {
        fx.deactivate();
        fx.activate();
      }
      if (mode == Dt::kIrregular && r.chance(0.004)) {
        switch (r.irange(0, 3)) {
          case 0: fx.reset(); break;
          case 1: fx.deactivate(); fx.activate(); break;
          case 2: fx.activate(); break;
          default: fx.deactivate(); break;  // keeps running after a deactivate, like a crossfade-out
        }
      }
    }
    std::snprintf(label, sizeof(label), "%s %s frame %d t=%.4f", tag, sc.name(), frames, static_cast<double>(t));
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

// Holds g for `frames` updates of `dt`, rendering and comparing after each.
bool hold(Expect& ex, Fx& fx, const gm::GForce& g, float dt, int frames, const char* tag, int& counter) {
  char label[160];
  for (int i = 0; i < frames; ++i) {
    fx.update(g, dt);
    std::snprintf(label, sizeof(label), "%s frame %d", tag, ++counter);
    if (!EXPECT(ex, fx.render_same(label))) return false;
  }
  return true;
}

}  // namespace

TEST(tunnel_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "tunnel") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // Each name points at the field the header documents.
  gm_tunnel_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"scale_g", &c->scale_g}, {"r_front", &c->r_front},   {"r_back", &c->r_back},
                {"segments", &c->segments}, {"spin", &c->spin},     {"spin_g", &c->spin_per_g},
                {"pulse", &c->pulse_hz},  {"pulse_g", &c->pulse_per_g}, {"hz", &c->hz}};
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == static_cast<int>(sizeof(fields) / sizeof(fields[0])));
  for (const auto& fd : fields) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fd.name);
    EXPECT(ex, prm && prm->value == fd.field);
  }
  // The panel-height dependent default and range.
  EXPECT(ex, diff::same(c->r_front, static_cast<float>(GM_H / 2 - 2)));
  const gm_param_t* rf = gm_params_find(gm_effect_params(fx.ce()), "r_front");
  EXPECT(ex, rf && diff::same(rf->max, static_cast<float>(GM_H / 2 - 1)));
  // At rest after init.
  EXPECT(ex, diff::same(c->phase, 0.0f) && diff::same(c->depth_phase, 0.0f));
  EXPECT(ex, diff::same(gm_spring3_pos(&c->offs), gm_vec3_zero()));
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.05f, 0.5f, 1.0f, 7.0f, 25.0f, 1e9f, diff::kInf, -diff::kInf,
                          diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // A fresh effect after all that still has the defaults; its first frame matches.
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
  EXPECT(ex, fresh.render_same("fresh, no update"));
  fresh.activate();
  EXPECT(ex, fresh.render_same("fresh, activated"));
  CHECK(fresh.lit_frames == 2);
}

TEST(tunnel_presets_60fps) {
  Expect ex;
  diff::Rand r(801);
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1100u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", true);
    EXPECT(ex, frames >= 22 * 60);
    CHECK(fx.lit_frames == fx.renders);  // coverage: the tunnel is always drawn
  }
}

TEST(tunnel_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(802);
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      fx.activate();
      gm::Rng rng(2100u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", true);
    }
}

TEST(tunnel_tunables) {
  Expect ex;
  diff::Rand r(803);
  gm::TunnelEffect proto;
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& slalom = *gm::Scenario::find("slalom");
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
      gm::Rng rng(3100u + static_cast<uint32_t>(i));
      play(ex, fx, drive, rng, r, Dt::k60, 22.0f, tag, false);
      play(ex, fx, slalom, rng, r, Dt::kIrregular, 6.0f, tag, true);
      // Change it again mid-run, while the tunnel is bent and spinning.
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag, false);
    }
  }
}

TEST(tunnel_bend_limits) {
  Expect ex;
  // Strong inertial force in every direction with the bend at its stiffest
  // and loosest, ring radii that leave no room to move (mx or my = 0) and
  // back rings larger than the front.
  const float scales[] = {0.2f, 1.0f, 3.0f};
  const float backs[] = {1.0f, 5.0f, 20.0f};
  const float fronts[] = {4.0f, static_cast<float>(GM_H / 2 - 2), static_cast<float>(GM_H / 2 - 1)};
  const gm::Vec3 dirs[] = {{3.0f, 0.0f, 0.0f},   {-3.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f},  {0.0f, -3.0f, 0.0f},
                           {0.0f, 0.0f, 3.0f},   {0.0f, 0.0f, -3.0f}, {1.2f, -0.9f, 0.6f}, {-0.4f, 0.4f, -1.5f},
                           {0.05f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
  for (float sg : scales)
    for (float rb : backs)
      for (float rf : fronts) {
        Fx fx;
        EXPECT(ex, fx.set("scale_g", sg));
        EXPECT(ex, fx.set("r_back", rb));
        EXPECT(ex, fx.set("r_front", rf));
        fx.activate();
        char tag[96];
        std::snprintf(tag, sizeof(tag), "bend scale_g=%g r_back=%g r_front=%g", static_cast<double>(sg),
                      static_cast<double>(rb), static_cast<double>(rf));
        int counter = 0;
        bool ok = true;
        for (const gm::Vec3& d : dirs) {
          if (!ok) break;
          ok = hold(ex, fx, dyn_g(-d.x, -d.y, -d.z), 1.0f / 60.0f, 45, tag, counter);  // inertial = d
        }
      }
}

TEST(tunnel_smoothing_and_dt) {
  Expect ex;
  diff::Rand r(804);
  // The spring at both ends of its range, with steps so small it does not
  // move (w * dt < 1e-6), ordinary steps and steps far past the clamp.
  const float hzs[] = {0.3f, 2.5f, 8.0f};
  const float dts[] = {0.0f, 1e-8f, 1e-7f, 5e-7f, 1e-6f, 1e-5f, 1.0f / 240.0f, 1.0f / 60.0f, 1.0f / 15.0f,
                       0.3f, 1.0f,  10.0f};
  for (float hz : hzs) {
    Fx fx;
    EXPECT(ex, fx.set("hz", hz));
    fx.activate();
    char tag[64];
    std::snprintf(tag, sizeof(tag), "hz=%g", static_cast<double>(hz));
    int counter = 0;
    bool ok = true;
    for (int rep = 0; rep < 3 && ok; ++rep)
      for (float dt : dts) {
        const float s = rep == 1 ? -1.0f : 1.0f;
        const gm::GForce g = dyn_g(s * 1.1f, rep == 2 ? 0.7f : -0.4f, -s * 0.8f);
        if (!(ok = hold(ex, fx, g, dt, 20, tag, counter))) break;
        if (!(ok = hold(ex, fx, dyn_g(0.0f, 0.0f, 0.0f), dt, 10, tag, counter))) break;
      }
    for (int i = 0; i < 1500 && ok; ++i) {
      const gm::GForce g = dyn_g(r.frange(-1.5f, 1.5f), r.frange(-0.6f, 0.6f), r.frange(-1.5f, 1.5f));
      ok = hold(ex, fx, g, odd_dt(r), 1, tag, counter);
    }
  }
}

TEST(tunnel_spin_and_pulse_wrap) {
  Expect ex;
  diff::Rand r(805);
  // Spin and depth pulses in both directions, at the extremes of their
  // tunables, so phase wraps above 2 pi and below 0 and the depth phase wraps
  // both ways; long steps make a single update jump several turns.
  const float spins[] = {0.0f, 0.4f, 5.0f};
  const float spin_gs[] = {0.0f, 20.0f};
  const float pulses[] = {0.0f, 3.0f};
  const float pulse_gs[] = {0.0f, 10.0f};
  const float lats[] = {-2.0f, -0.6f, 0.0f, 0.6f, 2.0f};
  const float lons[] = {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f};
  for (float sp : spins)
    for (float sg : spin_gs)
      for (float pu : pulses)
        for (float pg : pulse_gs) {
          Fx fx;
          EXPECT(ex, fx.set("spin", sp));
          EXPECT(ex, fx.set("spin_g", sg));
          EXPECT(ex, fx.set("pulse", pu));
          EXPECT(ex, fx.set("pulse_g", pg));
          fx.activate();
          char tag[128];
          std::snprintf(tag, sizeof(tag), "wrap spin=%g spin_g=%g pulse=%g pulse_g=%g", static_cast<double>(sp),
                        static_cast<double>(sg), static_cast<double>(pu), static_cast<double>(pg));
          int counter = 0;
          bool ok = true;
          for (float lat : lats)
            for (float lon : lons) {
              if (!ok) break;
              // proper.x = lateral g, proper.z = longitudinal g.
              ok = hold(ex, fx, dyn_g(lat, 0.0f, lon), 1.0f / 60.0f, 30, tag, counter) &&
                   hold(ex, fx, dyn_g(lat, 0.0f, lon), r.chance(0.5) ? 0.5f : 2.5f, 3, tag, counter);
            }
          // The depth phase is reduced to one turn after every update (x - floor(x) can round up to 1).
          EXPECT(ex, fx.c->depth_phase >= 0.0f && fx.c->depth_phase <= 1.0f);
          if (sg > 0.0f) CHECK(fx.wraps_up > 0 && fx.wraps_down > 0);  // coverage: both phase wraps happened
        }
}

TEST(tunnel_segments) {
  Expect ex;
  // Every dash count, including halves (rounded to nearest, ties up) and
  // values just either side of a half.
  int counter = 0;
  for (int i = 4; i <= 96; ++i) {
    const float base = 0.25f * static_cast<float>(i);
    const float values[] = {base, std::nextafter(base, 0.0f), std::nextafter(base, 100.0f)};
    for (float v : values) {
      Fx fx;
      EXPECT(ex, fx.set("segments", v));
      fx.activate();
      char tag[64];
      std::snprintf(tag, sizeof(tag), "segments=%.9g", static_cast<double>(v));
      if (!hold(ex, fx, dyn_g(0.7f, 0.1f, -0.5f), 1.0f / 30.0f, 24, tag, counter)) return;
    }
  }
}

TEST(tunnel_nan_inf_g) {
  Expect ex;
  diff::Rand r(807);
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, 1e18f, -1e-30f};
  int poisoned = 0;
  for (int run = 0; run < 40; ++run) {
    Fx fx;
    fx.activate();
    gm::Rng rng(6100u + static_cast<uint32_t>(run));
    const gm::Scenario& sc = gm::Scenario::preset(run % gm::Scenario::presetCount());
    char label[160];
    float t = 0.0f;
    bool stop = false;
    for (int frame = 0; frame < 900 && !stop; ++frame) {
      const float dt = run % 2 ? odd_dt(r) : 1.0f / 60.0f;
      t += dt;
      gm::GForce g = sc.at(std::fmod(t, sc.duration() + 0.5f), &rng);
      if (frame % 150 >= 60 && frame % 150 < 60 + (run % 5) * 6 + 1) {
        // A burst of bad samples: some components, or all of them.
        const int mask = r.irange(1, 7);
        if (mask & 1) g.proper.x = r.pick(bad);
        if (mask & 2) g.proper.y = r.pick(bad);
        if (mask & 4) g.proper.z = r.pick(bad);
      }
      fx.update(g, dt);
      if (std::isnan(fx.c->phase) || std::isnan(fx.c->depth_phase)) ++poisoned;
      // Reset now and then; without it a NaN stays in the spring and phases for good.
      if (r.chance(0.01) || (frame % 150 == 140 && run % 3 == 0)) fx.reset();
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) stop = true;
    }
  }
  CHECK(poisoned > 0);  // coverage: the bad input really reached the state
}

TEST(tunnel_long_run) {
  Expect ex;
  diff::Rand r(808);
  // 10 minutes at 60 fps ...
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7100u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps", false);
    EXPECT(ex, frames >= 600 * 60);
  }
  // ... and an hour at the engine's slowest step, with a strong spin.
  {
    Fx fx;
    EXPECT(ex, fx.set("spin", 5.0f));
    EXPECT(ex, fx.set("pulse", 3.0f));
    fx.activate();
    gm::Rng rng(7101u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 3600 * 15; ++frame) {
      t += 1.0f / 15.0f;
      fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f);
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(tunnel_fuzz) {
  Expect ex;
  diff::Rand r(809);
  gm::TunnelEffect proto;
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
        target = gm::Vec3{r.frange(-1.6f, 1.6f), r.chance(0.2) ? r.frange(-0.8f, 0.8f) : 0.0f, r.frange(-1.6f, 1.6f)};
      dyn = gm::lerp(dyn, target, 0.1f);
      gm::Vec3 d = dyn;
      if (r.chance(0.04)) d.y += r.frange(-1.0f, 1.0f);
      if (r.chance(0.01)) d = gm::Vec3{r.frange(-3, 3), r.frange(-3, 3), r.frange(-3, 3)};
      const int updates = r.chance(0.1) ? r.irange(0, 3) : 1;
      for (int u = 0; u < updates; ++u) fx.update(dyn_g(d.x, d.y, d.z), odd_dt(r));
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
