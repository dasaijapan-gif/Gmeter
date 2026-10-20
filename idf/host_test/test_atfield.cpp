// Differential tests of the "atfield" effect (gm_atfield.c vs gm::AtFieldEffect):
// the parameter table, every scenario preset at 60 fps and with irregular dt,
// every tunable at its min, max and a middle value, strong-ripple steps, bumps,
// the idle pulse, the captions, reset/activate/deactivate cycles, NaN/inf g and
// long runs. Every update is followed by a render into cleared frames that must
// match byte for byte, and the number of active ripples must match too.
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
  std::unique_ptr<gm_atfield_t> c{new gm_atfield_t};
  std::unique_ptr<gm::AtFieldEffect> p{new gm::AtFieldEffect};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  int max_ripples = 0;  // coverage: most ripples seen at once
  int lit_frames = 0;   // coverage: frames with something drawn

  Fx() { gm_atfield_init(c.get()); }
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
    gm_effect_update(ce(), &cctx);
    p->update(pctx);
  }
  // Renders both into cleared frames; true when the frames and the active
  // ripple counts match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    bool ok = f.same(label);
    const int rc = gm_atfield_active_ripples(c.get()), rp = p->activeRipples();
    if (rc != rp) {
      harness::note("%s: active ripples C %d, C++ %d", label, rc, rp);
      ok = false;
    }
    if (rc > max_ripples) max_ripples = rc;
    if (gm_frame_count_lit(f.c.get()) > 0) ++lit_frames;
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

}  // namespace

TEST(atfield_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "atfield") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  EXPECT(ex, gm_atfield_active_ripples(fx.c.get()) == fx.p->activeRipples());
  // Each name points at the field the header documents.
  gm_atfield_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"speed", &c->speed},   {"life", &c->life},          {"rate_g", &c->rate_g},
                {"min_g", &c->min_g},   {"step_g", &c->step_g},      {"idle", &c->idle},
                {"double", &c->double_ring}, {"label", &c->label}, {"bump_g", &c->bump_g}};
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == static_cast<int>(sizeof(fields) / sizeof(fields[0])));
  for (const auto& fd : fields) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fd.name);
    EXPECT(ex, prm && prm->value == fd.field);
  }
  // The tracker keeps its own defaults (only bump_g is copied in update).
  EXPECT(ex, diff::same(c->tracker.enter_g, 0.25f) && diff::same(c->tracker.hold_s, 0.4f));
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.05f, 0.5f, 1.0f, 7.0f, 1e9f, diff::kInf, -diff::kInf, diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // A fresh effect after all that still has the defaults.
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
}

TEST(atfield_presets_60fps) {
  Expect ex;
  diff::Rand r(901);
  int most = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", true);
    EXPECT(ex, frames >= 22 * 60);
    harness::note("%s: %d of %d frames drawn, up to %d ripples", sc.name(), fx.lit_frames, frames, fx.max_ripples);
    CHECK(fx.lit_frames > frames / 4);  // coverage: ripples (at least the idle pulse) are drawn
    if (fx.max_ripples > most) most = fx.max_ripples;
  }
  CHECK(most >= 4);  // coverage: the runs really spawn ripples
}

TEST(atfield_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(902);
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", true);
    }
}

TEST(atfield_tunables) {
  Expect ex;
  diff::Rand r(903);
  gm::AtFieldEffect proto;
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
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
      // Change it again mid-run, while ripples are alive.
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag, false);
    }
  }
}

TEST(atfield_strong_ripple_steps) {
  Expect ex;
  const float steps[] = {0.05f, 0.1f, 0.25f, 0.3f, 0.5f, 1.0f};
  int most = 0;
  for (float sg : steps) {
    for (int axis = 0; axis < 4; ++axis) {
      Fx fx;
      EXPECT(ex, fx.set("step_g", sg));
      fx.activate();
      char label[128];
      int frame = 0;
      bool stop = false;
      // Staircase up to 2.5 g in 0.025 g steps (exact multiples of step_g
      // included), held 0.2 s each, then back down, then sharp jumps.
      for (int pass = 0; pass < 3 && !stop; ++pass) {
        for (int k = 0; k <= 100 && !stop; ++k) {
          const int kk = pass == 1 ? 100 - k : k;
          const float a = pass == 2 ? (k % 20 < 10 ? 2.5f : 0.0f) : 0.025f * static_cast<float>(kk);
          const float sign = (k / 7) % 2 ? -1.0f : 1.0f;
          gm::GForce g;
          switch (axis) {
            case 0: g = dyn_g(sign * a, 0.0f, 0.0f); break;   // lateral
            case 1: g = dyn_g(0.0f, 0.0f, sign * a); break;   // longitudinal
            case 2: g = dyn_g(a * 0.6f, 0.0f, -a * 0.8f); break;  // diagonal (3-4-5)
            default: g = dyn_g(sign * a * 0.5f, a * 0.3f, a * 0.5f); break;
          }
          for (int h = 0; h < 12 && !stop; ++h) {
            fx.update(g, 1.0f / 60.0f);
            std::snprintf(label, sizeof(label), "step_g=%g axis %d frame %d", static_cast<double>(sg), axis, ++frame);
            stop = !EXPECT(ex, fx.render_same(label));
          }
        }
      }
      if (fx.max_ripples > most) most = fx.max_ripples;
    }
  }
  CHECK(most == gm::AtFieldEffect::kMaxRipples);  // coverage: the oldest ripple gets reused
}

TEST(atfield_bumps) {
  Expect ex;
  const float bump_gs[] = {0.05f, 0.25f, 0.3f, 0.5f, 2.0f};
  const float amps[] = {0.04f, 0.1f, 0.25f, 0.3f, 0.31f, 0.5f, 1.0f, 2.5f, -0.3f, -0.6f, -2.0f};
  diff::Rand r(904);
  for (float bg : bump_gs) {
    Fx fx;
    EXPECT(ex, fx.set("bump_g", bg));
    fx.activate();
    char label[128];
    int frame = 0;
    bool stop = false;
    for (int rep = 0; rep < 3 && !stop; ++rep)
      for (float amp : amps) {
        // Quiet, spike, rebound, quiet; on top of a little lateral g.
        const float lat = rep == 1 ? 0.4f : 0.0f;
        const gm::GForce seq[] = {dyn_g(lat, 0.0f, 0.0f), dyn_g(lat, amp, 0.0f), dyn_g(lat, amp, 0.0f),
                                  dyn_g(lat, -0.5f * amp, 0.0f), dyn_g(lat, 0.0f, 0.0f)};
        const int holds[] = {30, 2, 1, 3, 25};
        for (int s = 0; s < 5 && !stop; ++s)
          for (int h = 0; h < holds[s]; ++h) {
            fx.update(seq[s], rep == 2 ? odd_dt(r) : 1.0f / 60.0f);
            std::snprintf(label, sizeof(label), "bump_g=%g amp %g frame %d", static_cast<double>(bg),
                          static_cast<double>(amp), ++frame);
            if (!EXPECT(ex, fx.render_same(label))) {
              stop = true;
              break;
            }
          }
      }
    CHECK(fx.max_ripples >= 1);
  }
  // The "bump" preset with noise, at several bump thresholds.
  for (float bg : bump_gs) {
    Fx fx;
    EXPECT(ex, fx.set("bump_g", bg));
    fx.activate();
    gm::Rng rng(4000u);
    play(ex, fx, *gm::Scenario::find("bump"), rng, r, Dt::k60, 20.0f, "bump preset", false);
  }
}

TEST(atfield_idle_pulse) {
  Expect ex;
  diff::Rand r(905);
  const float idles[] = {0.0f, 0.5f, 0.51f, 1.0f};
  for (float idle : idles) {
    Fx fx;
    EXPECT(ex, fx.set("idle", idle));
    fx.activate();
    gm::Rng rng(5000u);
    play(ex, fx, *gm::Scenario::find("idle"), rng, r, Dt::k60, 30.0f, "idle", false);
    play(ex, fx, *gm::Scenario::find("idle"), rng, r, Dt::kIrregular, 30.0f, "idle irregular", false);
    if (idle > 0.5f) CHECK(fx.max_ripples >= 1);
  }
  // Horizontal g right at min_g: below it only the idle pulse runs.
  const float min_gs[] = {0.0f, 0.15f, 0.5f, 1.0f};
  for (float mg : min_gs) {
    Fx fx;
    EXPECT(ex, fx.set("min_g", mg));
    fx.activate();
    char label[128];
    int frame = 0;
    for (int k = 0; k < 2400; ++k) {
      const float lat = mg + (k % 300 < 150 ? -0.01f : (k % 300 < 200 ? 0.0f : 0.01f));
      fx.update(dyn_g(lat, 0.0f, 0.0f), 1.0f / 60.0f);
      std::snprintf(label, sizeof(label), "min_g=%g frame %d", static_cast<double>(mg), ++frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(atfield_captions) {
  Expect ex;
  diff::Rand r(906);
  // Two pairs with identical input, captions on and off; the difference of
  // their frames shows where captions were drawn.
  Fx on, off;
  EXPECT(ex, on.set("label", 1.0f));
  EXPECT(ex, off.set("label", 0.0f));
  on.activate();
  off.activate();
  int bottom = 0, top = 0;
  char label[128];
  float t = 0.0f;
  for (int frame = 0; frame < 60 * 40; ++frame) {
    const float dt = frame < 60 * 25 ? 1.0f / 60.0f : odd_dt(r);
    t += dt;
    // Pulses of 1.0..1.4 g lateral (horizontal > 0.8: the blinking 全開) and
    // gentler stretches where only the A.T.フィールド caption shows.
    const float ph = std::fmod(t, 6.0f);
    const float lat = ph < 2.5f ? 1.0f + 0.1f * std::floor(t / 6.0f) : (ph < 4.0f ? 0.45f : 0.0f);
    const gm::GForce g = dyn_g(frame % 900 < 450 ? lat : -lat, 0.0f, 0.1f);
    on.update(g, dt);
    off.update(g, dt);
    std::snprintf(label, sizeof(label), "label on frame %d", frame);
    if (!EXPECT(ex, on.render_same(label))) break;
    std::snprintf(label, sizeof(label), "label off frame %d", frame);
    if (!EXPECT(ex, off.render_same(label))) break;
    bool b = false, tp = false;
    for (int y = 0; y < GM_H; ++y)
      for (int x = 0; x < GM_W; ++x)
        if (gm_frame_get(on.f.c.get(), x, y, 0) != gm_frame_get(off.f.c.get(), x, y, 0)) {
          if (y >= GM_H / 2) b = true;
          else tp = true;
        }
    bottom += b ? 1 : 0;
    top += tp ? 1 : 0;
  }
  harness::note("caption frames: A.T. %d, 全開 %d", bottom, top);
  CHECK(bottom > 100);  // coverage: A.T.フィールド shown
  CHECK(top > 50);      // coverage: 全開 blinking
}

TEST(atfield_nan_inf_g) {
  Expect ex;
  diff::Rand r(907);
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, 1e18f, -1e-30f};
  for (int run = 0; run < 40; ++run) {
    Fx fx;
    fx.activate();
    gm::Rng rng(6000u + static_cast<uint32_t>(run));
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
      if (r.chance(0.01)) fx.reset();
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) stop = true;
    }
  }
}

TEST(atfield_long_run) {
  Expect ex;
  diff::Rand r(908);
  // 10 minutes at 60 fps (the blink clock wraps every 60 s) ...
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps", false);
    EXPECT(ex, frames >= 600 * 60);
  }
  // ... and an hour at the engine's slowest step.
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7001u);
    const gm::Scenario& sc = *gm::Scenario::find("drive");
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

TEST(atfield_fuzz) {
  Expect ex;
  diff::Rand r(909);
  gm::AtFieldEffect proto;
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
