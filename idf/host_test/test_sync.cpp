// Differential tests of the "sync" effect (gm_sync.c vs gm::SyncEffect): the
// parameter table, every scenario preset at 60 fps and with irregular dt,
// every tunable at its min, max and a middle value, sample-rate extremes (one
// update far longer than the history, many updates shorter than a sample),
// traces beyond the band, reset/activate/deactivate cycles, NaN/inf g, long
// runs and random fuzzing. Every update is followed by a render into cleared
// frames that must match byte for byte.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "diff_util.hpp"

using diff::Expect;

namespace {

#if defined(__SANITIZE_ADDRESS__)
// GM_SANITIZE turns on ASan and UBSan together; GCC only announces the first.
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

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
  std::unique_ptr<gm_sync_t> c{new gm_sync_t};
  std::unique_ptr<gm::SyncEffect> p{new gm::SyncEffect};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters (frames).
  int big_pct = 0;   // 同期率 of 100 or more (three digits)
  int clipped = 0;   // a trace sample beyond its band

  Fx() { gm_sync_init(c.get()); }
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
  // Renders both into cleared frames; true when the frames match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    const bool ok = f.same(label);
    // Coverage.
    if (c->sync_pct + 0.5f >= 100.0f) ++big_pct;
    const float half = static_cast<float>(GM_H / 3 / 2 - 1);
    for (int b = 0; b < 3; ++b)
      if (std::fabs(c->hist[b][(c->head + GM_SYNC_HIST - 1) % GM_SYNC_HIST] * c->gain) > half + 0.5f) {
        ++clipped;
        break;
      }
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
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

}  // namespace

TEST(sync_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "sync") == 0);
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
  EXPECT(ex, GM_SYNC_HIST == gm::SyncEffect::kHist);
  // Each name points at the field the header documents.
  gm_sync_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"rate", &c->rate}, {"gain", &c->gain}, {"grid", &c->grid}, {"labels", &c->labels}};
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == static_cast<int>(sizeof(fields) / sizeof(fields[0])));
  for (int i = 0; i < static_cast<int>(sizeof(fields) / sizeof(fields[0])); ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));
  }
  // The rest of the state starts cleared.
  bool zero = c->head == 0 && c->accum == 0.0f && c->scroll == 0.0f && c->sync_pct == 0.0f;
  for (int b = 0; b < 3; ++b)
    for (int i = 0; i < GM_SYNC_HIST; ++i) zero = zero && c->hist[b][i] == 0.0f;
  EXPECT(ex, zero);
  EXPECT(ex, fx.render_same("fresh render"));
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, 5.0f, 30.0f, 121.0f, 1e9f, diff::kInf, -diff::kInf,
                          diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
}

TEST(sync_presets_60fps) {
  Expect ex;
  diff::Rand r(1201);
  int clipped = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", false);
    EXPECT(ex, frames >= 22 * 60);
    clipped += fx.clipped;
  }
  // The presets stay under 0.9 g, inside the bands at the default gain
  // (sync_rate_extremes and sync_tunables cover clipped traces).
  harness::note("frames with a clipped newest sample: %d", clipped);
}

TEST(sync_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(1202);
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", false);
    }
}

TEST(sync_events) {
  Expect ex;
  diff::Rand r(1203);
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

TEST(sync_tunables) {
  Expect ex;
  diff::Rand r(1204);
  gm::SyncEffect proto;
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
      // Hard driving: traces far past their bands, 同期率 over 100.
      char label[160];
      float t = 0.0f;
      for (int frame = 0; frame < 600; ++frame) {
        t += 1.0f / 60.0f;
        const float s = std::sin(t * 1.7f), q = std::cos(t * 0.9f);
        fx.update(dyn_g(2.5f * s, 1.2f * s * q, -2.0f * q), 1.0f / 60.0f);
        std::snprintf(label, sizeof(label), "%s hard frame %d", tag, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
      // Change it again mid-run.
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag, false);
    }
  }
}

TEST(sync_rate_extremes) {
  Expect ex;
  diff::Rand r(1205);
  int big = 0, clipped = 0;
  const float rates[] = {5.0f, 7.3f, 30.0f, 60.0f, 61.0f, 119.9f, 120.0f};
  const float long_steps[] = {0.2f, 1.0f, 2.5f, 10.0f};
  const float short_steps[] = {0.0f, 1e-6f, 1e-4f, 1e-3f, 0.004f};
  // dt patterns: steady, one huge step (more than 16 samples and more than the
  // whole history), many steps shorter than one sample, alternating.
  for (float rate : rates) {
    for (int pattern = 0; pattern < 4; ++pattern) {
      Fx fx;
      EXPECT(ex, fx.set("rate", rate));
      EXPECT(ex, fx.set("gain", r.frange(2.0f, 30.0f)));
      fx.activate();
      char label[160];
      float t = 0.0f;
      for (int frame = 0; frame < 1500; ++frame) {
        float dt = 1.0f / 60.0f;
        switch (pattern) {
          case 1: dt = frame % 200 == 100 ? r.pick(long_steps) : 1.0f / 60.0f; break;
          case 2: dt = r.pick(short_steps); break;
          case 3: dt = frame % 2 ? 1.0f / 15.0f : 1.0f / 1000.0f; break;
          default: break;
        }
        t += dt;
        const float x = 1.6f * std::sin(t * 2.1f), z = 1.4f * std::cos(t * 1.3f);
        const float y = std::fmod(t, 1.5f) < 0.08f ? 0.9f : 0.0f;
        fx.update(dyn_g(x, y, z), dt);
        std::snprintf(label, sizeof(label), "rate %g pattern %d frame %d", double(rate), pattern, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
      big += fx.big_pct;
      clipped += fx.clipped;
    }
  }
  harness::note("frames with 同期率 >= 100: %d, with a clipped sample: %d", big, clipped);
  CHECK(big > 1000);
  CHECK(clipped > 1000);
}

TEST(sync_accum_edges) {
  Expect ex;
  diff::Rand r(1209);
  // Rates and steps whose products are exact, so the sample accumulator lands
  // exactly on 1 after the 16-sample cap (and on whole numbers in general).
  const float rates[] = {5.0f, 8.0f, 10.0f, 16.0f, 17.0f, 20.0f, 32.0f, 34.0f, 64.0f, 120.0f};
  const float steps[] = {0.5f, 1.0f, 0.25f, 0.125f, 0.0625f, 0.75f, 2.0f, 0.001f, 1.0f / 64.0f, 1.0f / 60.0f, 0.0f};
  int exact_one = 0;
  for (float rate : rates) {
    for (int run = 0; run < 3; ++run) {
      Fx fx;
      EXPECT(ex, fx.set("rate", rate));
      fx.activate();
      char label[128];
      float t = 0.0f;
      for (int frame = 0; frame < 800; ++frame) {
        const float dt = run == 0 ? steps[frame % 11] : r.pick(steps);
        t += dt;
        fx.update(dyn_g(std::sin(t), 0.3f * std::cos(3.0f * t), std::cos(t * 0.7f)), dt);
        if (fx.c->accum == 1.0f) ++exact_one;
        std::snprintf(label, sizeof(label), "accum rate %g run %d frame %d", double(rate), run, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
    }
  }
  harness::note("updates leaving the accumulator at exactly 1: %d", exact_one);
  CHECK(exact_one > 20);
}

TEST(sync_nan_inf_g) {
  Expect ex;
  diff::Rand r(1206);
  // Samples the C++ handles without undefined behaviour ...
  const float huge[] = {1e6f, -1e6f, 3e7f, -3e7f, 1e-30f, -1e-30f, 7e6f};
  // ... and samples whose trace row overflows int in the C++ (its result is
  // undefined; on x86 they land on the top edge of the band). The C gives the
  // same rows without undefined behaviour. Under the sanitizers the C++ traps
  // on them, so there only the C effect sees them (and must stay clean) while
  // the pair compares the run with them replaced by huge finite values.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, 1e18f, -2.2e8f, 2.5e9f};
  if (kSanitized) harness::note("sanitized build: NaN/inf/1e30 g go to a C-only effect");
  for (int run = 0; run < 40; ++run) {
    Fx fx;
    std::unique_ptr<gm_sync_t> solo(new gm_sync_t);
    std::unique_ptr<gm_frame_t> solo_frame(new gm_frame_t);
    gm_sync_init(solo.get());
    gm_effect_ctx_t sctx = GM_EFFECT_CTX_INIT;
    if (run % 3 == 1) {
      EXPECT(ex, fx.set("gain", 30.0f));
      EXPECT(ex, gm_params_set(gm_effect_params(&solo->base), "gain", 30.0f));
    }
    if (run % 3 == 2) {
      EXPECT(ex, fx.set("rate", 120.0f));
      EXPECT(ex, gm_params_set(gm_effect_params(&solo->base), "rate", 120.0f));
    }
    fx.activate();
    gm_effect_activate(&solo->base);
    gm::Rng rng(6000u + static_cast<uint32_t>(run));
    const gm::Scenario& sc = gm::Scenario::preset(run % gm::Scenario::presetCount());
    char label[160];
    float t = 0.0f;
    bool stop = false;
    for (int frame = 0; frame < 900 && !stop; ++frame) {
      const float dt = run % 2 ? odd_dt(r) : 1.0f / 60.0f;
      t += dt;
      gm::GForce g = sc.at(std::fmod(t, sc.duration() + 0.5f), &rng);
      gm::GForce gsafe = g;
      if (frame % 150 >= 60 && frame % 150 < 60 + (run % 5) * 6 + 1) {
        // A burst of bad samples: some components, or all of them.
        const int mask = r.irange(1, 7);
        const bool undefined = r.chance(0.6);
        float* comp[3] = {&g.proper.x, &g.proper.y, &g.proper.z};
        float* safe[3] = {&gsafe.proper.x, &gsafe.proper.y, &gsafe.proper.z};
        for (int k = 0; k < 3; ++k)
          if (mask & (1 << k)) {
            *comp[k] = undefined ? r.pick(bad) : r.pick(huge);
            *safe[k] = undefined ? (r.chance(0.5) ? 1e6f : -1e6f) : *comp[k];
          }
      }
      fx.update(kSanitized ? gsafe : g, dt);
      sctx.g = gm_gforce_from_proper(diff::to_c(g.proper));
      sctx.dt = dt;
      gm_effect_update(&solo->base, &sctx);
      if (r.chance(0.01)) {
        fx.reset();
        gm_effect_reset(&solo->base);
      }
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) stop = true;
      gm_frame_clear(solo_frame.get());
      gm_effect_render(&solo->base, solo_frame.get());
      // Without the sanitizers both saw the same samples.
      if (!kSanitized && !EXPECT(ex, std::memcmp(solo_frame->bits, fx.f.c->bits, sizeof(solo_frame->bits)) == 0))
        stop = true;
    }
  }
}

TEST(sync_long_run) {
  Expect ex;
  diff::Rand r(1207);
  // 10 minutes at 60 fps (the history and the scroll phase wrap many times) ...
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps", false);
    EXPECT(ex, frames >= 600 * 60);
  }
  // ... and an hour at the engine's slowest step, at the highest rate.
  {
    Fx fx;
    EXPECT(ex, fx.set("rate", 120.0f));
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

TEST(sync_fuzz) {
  Expect ex;
  diff::Rand r(1208);
  gm::SyncEffect proto;
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
