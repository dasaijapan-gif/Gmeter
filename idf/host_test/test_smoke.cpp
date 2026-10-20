// Differential tests of the "smoke" effect (gm_smoke.c vs gm::SmokeEffect): the
// parameter table and grid constants, heap allocation on activate and release
// on deactivate / gm_smoke_deinit, every scenario preset at 60 fps and with
// irregular dt, every tunable at its min, max and a middle value, solver rate
// and iteration variations, reset/activate/deactivate cycles mid-run, NaN/inf
// g, a long run and random fuzzing. After every update the allocation state,
// the total density and every density cell of the padded grid must match bit
// for bit, and every update batch is followed by a render into cleared frames
// that must match byte for byte. Run it with the default grid (32 cells across)
// and with the firmware's -DGM_SMOKE_NX=16 -DGM_SMOKE_NY=8.
//
// Both solvers run in every test and their cost grows with the grid: the
// default grid costs about 5x the firmware's 16x8. With a grid bigger than
// 16x8 the tunable, rate, NaN, long and random runs are shortened (the
// per-preset runs keep 20 s or more).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "diff_util.hpp"

using diff::Expect;
using Smoke = gm::SmokeEffect;

namespace {

constexpr bool kSmallGrid = GM_SMOKE_NX * GM_SMOKE_NY <= 16 * 8;

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

struct SmokeDeleter {
  void operator()(gm_smoke_t* s) const {
    gm_smoke_deinit(s);
    delete s;
  }
};

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_smoke_t, SmokeDeleter> c{new gm_smoke_t};
  std::unique_ptr<Smoke> p{new Smoke};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters.
  int steps = 0;         // solver steps
  int drops = 0;         // solver steps that dropped a backlog (a stall)
  int lit_frames = 0;    // rendered frames with something lit
  int solid_cells = 0;   // rendered cells drawn as a full rectangle
  int dither_cells = 0;  // rendered cells drawn dithered

  Fx() { gm_smoke_init(c.get()); }
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
  // Allocation, total density and every cell of the padded grid.
  bool same_state() const {
    const gm_smoke_t* s = c.get();
    if (gm_smoke_allocated(s) != p->allocated()) {
      harness::note("allocated: C %d, C++ %d", gm_smoke_allocated(s) ? 1 : 0, p->allocated() ? 1 : 0);
      return false;
    }
    if (!diff::same(gm_smoke_total_density(s), p->totalDensity())) {
      harness::note("total density: C %.9g, C++ %.9g", static_cast<double>(gm_smoke_total_density(s)),
                    static_cast<double>(p->totalDensity()));
      return false;
    }
    for (int k = 0; k < GM_SMOKE_SZ; ++k)
      for (int j = 0; j < GM_SMOKE_SY; ++j)
        for (int i = 0; i < GM_SMOKE_SX; ++i)
          if (!diff::same(gm_smoke_density(s, i, j, k), p->density(i, j, k))) {
            harness::note("density (%d,%d,%d): C %.9g, C++ %.9g", i, j, k,
                          static_cast<double>(gm_smoke_density(s, i, j, k)), static_cast<double>(p->density(i, j, k)));
            return false;
          }
    return true;
  }
  // Advances both; true when the states match afterwards.
  bool update(const gm::GForce& g, float dt) {
    // Predict the solver schedule from the C state (coverage only).
    if (c->d) {
      const float a = c->accum + dt, period = 1.0f / c->rate_hz;
      if (!(a < period)) {
        ++steps;
        if (a - period > period) ++drops;
      }
    }
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
    if (gm_frame_count_lit(f.c.get()) > 0) ++lit_frames;
    if (c->d)
      for (int k = 1; k <= GM_SMOKE_NZ; ++k)
        for (int j = 1; j <= GM_SMOKE_NY; ++j)
          for (int i = 1; i <= GM_SMOKE_NX; ++i) {
            const float val = gm_smoke_density(c.get(), i, j, k) * c->gain;
            if (val >= 1.0f)
              ++solid_cells;
            else if (val >= 1.0f / 64.0f)
              ++dither_cells;
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

// Plays preset `sc` (looped) for `seconds` into both effects, comparing the
// states after every update and rendering and comparing after every update
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
      harness::note("%s: state differs after an update", label);
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
          default: fx.deactivate(); break;  // the next update allocates again
        }
      }
    }
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

}  // namespace

TEST(smoke_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "smoke") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // The vtable: own activate / deactivate / reset, no burn-in offset.
  EXPECT(ex, fx.ce()->vt->activate != nullptr);
  EXPECT(ex, fx.ce()->vt->deactivate != nullptr);
  EXPECT(ex, fx.ce()->vt->reset != nullptr);
  EXPECT(ex, fx.ce()->vt->burn_in_offset == nullptr);
  int dxc = 5, dyc = 5, dxp = 5, dyp = 5;
  gm_effect_burn_in_offset(fx.ce(), &dxc, &dyc);
  fx.p->burnInOffset(dxp, dyp);
  EXPECT(ex, dxc == 0 && dyc == 0 && dxp == 0 && dyp == 0);
  // Grid geometry.
  EXPECT(ex, GM_SMOKE_NX == Smoke::NX && GM_SMOKE_NY == Smoke::NY && GM_SMOKE_NZ == Smoke::NZ);
  EXPECT(ex, GM_SMOKE_SX == Smoke::SX && GM_SMOKE_SY == Smoke::SY && GM_SMOKE_SZ == Smoke::SZ);
  EXPECT(ex, GM_SMOKE_N == Smoke::N && GM_SMOKE_CX == Smoke::CX && GM_SMOKE_CY == Smoke::CY);
  EXPECT(ex, gm_smoke_memory_bytes() == Smoke::memoryBytes());
  for (int k = 0; k < GM_SMOKE_SZ; ++k)
    for (int j = 0; j < GM_SMOKE_SY; ++j)
      for (int i = 0; i < GM_SMOKE_SX; ++i) EXPECT(ex, gm_smoke_idx(i, j, k) == Smoke::idx(i, j, k));
  harness::note("grid %dx%dx%d (%d cells incl. padding), %d bytes while active", GM_SMOKE_NX, GM_SMOKE_NY, GM_SMOKE_NZ,
                GM_SMOKE_N, static_cast<int>(gm_smoke_memory_bytes()));
  // Each name points at the field the header documents.
  gm_smoke_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"wind", &c->wind},       {"buoyancy", &c->buoyancy},     {"emit", &c->emit},
                {"emit_r", &c->emit_radius}, {"emit_d", &c->emit_depth},  {"dissipate", &c->dissipation},
                {"iters", &c->iters},     {"rate", &c->rate_hz},          {"gain", &c->gain},
                {"swirl", &c->swirl}};
  const int nfields = static_cast<int>(sizeof(fields) / sizeof(fields[0]));
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == nfields);
  for (int i = 0; i < nfields; ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));
  }
  // The rest of the state: nothing allocated, schedule at zero, rng seeded.
  EXPECT(ex, c->accum == 0.0f && c->since_step == 0.0f);
  EXPECT(ex, !c->u && !c->v && !c->w && !c->u0 && !c->v0 && !c->w0 && !c->d && !c->d0);
  EXPECT(ex, c->rng.s == 4242u);
  EXPECT(ex, fx.same_state());
  EXPECT(ex, fx.render_same("fresh render"));
  EXPECT(ex, gm_frame_count_lit(fx.f.c.get()) == 0);
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

TEST(smoke_allocation) {
  Expect ex;
  diff::Rand r(3101);
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  // Activate allocates (zeroed), deactivate frees, a second activate keeps the
  // buffers and clears them.
  {
    Fx fx;
    gm_smoke_t* c = fx.c.get();
    EXPECT(ex, !gm_smoke_allocated(c) && !fx.p->allocated());
    EXPECT(ex, gm_smoke_total_density(c) == 0.0f && gm_smoke_density(c, 3, 3, 3) == 0.0f);
    fx.activate();
    EXPECT(ex, gm_smoke_allocated(c) && fx.p->allocated());
    EXPECT(ex, c->u && c->v && c->w && c->u0 && c->v0 && c->w0 && c->d && c->d0);
    bool zero = true;
    float* bufs[] = {c->u, c->v, c->w, c->u0, c->v0, c->w0, c->d, c->d0};
    for (float* b : bufs)
      for (int n = 0; n < GM_SMOKE_N; ++n) zero = zero && b[n] == 0.0f;
    EXPECT(ex, zero);
    EXPECT(ex, fx.same_state());
    gm::Rng rng(11u);
    play(ex, fx, drive, rng, r, Dt::k60, 3.0f, "alloc", false);
    EXPECT(ex, gm_smoke_total_density(c) > 0.0f);
    const float* d_before = c->d;
    fx.activate();  // already allocated: same buffers, cleared
    EXPECT(ex, c->d == d_before);
    EXPECT(ex, gm_smoke_total_density(c) == 0.0f && fx.p->totalDensity() == 0.0f);
    EXPECT(ex, c->accum == 0.0f && c->since_step == 0.0f);
    EXPECT(ex, fx.same_state());
    fx.deactivate();
    EXPECT(ex, !gm_smoke_allocated(c) && !fx.p->allocated());
    EXPECT(ex, !c->u && !c->v && !c->w && !c->u0 && !c->v0 && !c->w0 && !c->d && !c->d0);
    EXPECT(ex, gm_smoke_total_density(c) == 0.0f && gm_smoke_density(c, 3, 3, 3) == 0.0f);
    EXPECT(ex, fx.render_same("deactivated render"));
    EXPECT(ex, gm_frame_count_lit(fx.f.c.get()) == 0);
    fx.deactivate();  // twice is harmless
    fx.reset();       // reset without buffers is a no-op
    EXPECT(ex, fx.same_state());
    // An update while deactivated allocates again (the engine keeps updating an
    // effect that is fading out).
    EXPECT(ex, fx.update(drive.at(1.0f, &rng), 1.0f / 60.0f));
    EXPECT(ex, gm_smoke_allocated(c) && fx.p->allocated());
    play(ex, fx, drive, rng, r, Dt::k60, 3.0f, "realloc", false);
  }
  // Never activated: the first update allocates.
  {
    Fx fx;
    gm::Rng rng(12u);
    EXPECT(ex, fx.render_same("never activated render"));
    play(ex, fx, drive, rng, r, Dt::kIrregular, 5.0f, "never activated", false);
    EXPECT(ex, gm_smoke_allocated(fx.c.get()));
  }
  // gm_smoke_deinit frees the buffers like the C++ destructor (the sanitizer
  // build reports a leak or a double free otherwise); it is safe twice and on
  // an effect that never allocated, and the effect can be initialised again.
  {
    gm_smoke_t* s = new gm_smoke_t;
    gm_smoke_init(s);
    gm_smoke_deinit(s);
    gm_effect_activate(&s->base);
    EXPECT(ex, gm_smoke_allocated(s));
    gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
    for (int i = 0; i < 30; ++i) gm_effect_update(&s->base, &ctx);
    gm_smoke_deinit(s);
    EXPECT(ex, !gm_smoke_allocated(s));
    EXPECT(ex, !s->u && !s->v && !s->w && !s->u0 && !s->v0 && !s->w0 && !s->d && !s->d0);
    gm_smoke_deinit(s);
    gm_smoke_init(s);
    gm_effect_update(&s->base, &ctx);
    EXPECT(ex, gm_smoke_allocated(s));
    gm_smoke_deinit(s);
    delete s;
  }
  // Many activate/deactivate cycles (heap churn under the sanitizers).
  {
    Fx fx;
    gm::Rng rng(13u);
    for (int cycle = 0; cycle < 200; ++cycle) {
      fx.activate();
      for (int i = 0; i < 3; ++i)
        if (!EXPECT(ex, fx.update(drive.at(0.1f * static_cast<float>(cycle + i), &rng), 1.0f / 30.0f))) break;
      if (!EXPECT(ex, fx.render_same("cycle"))) break;
      fx.deactivate();
    }
  }
}

TEST(smoke_presets_60fps) {
  Expect ex;
  diff::Rand r(3102);
  int steps = 0, lit = 0, solid = 0, dither = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", false);
    EXPECT(ex, frames >= 22 * 60);
    steps += fx.steps;
    lit += fx.lit_frames;
    solid += fx.solid_cells;
    dither += fx.dither_cells;
  }
  harness::note("solver steps %d, lit frames %d, solid cells %d, dithered cells %d", steps, lit, solid, dither);
  CHECK(steps > gm::Scenario::presetCount() * 22 * 29);
  CHECK(lit > gm::Scenario::presetCount() * 22 * 55);
  CHECK(solid > 1000);
  CHECK(dither > 10000);
}

TEST(smoke_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(3103);
  int steps = 0, drops = 0;
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      if (rep) EXPECT(ex, fx.set("rate", 57.0f));  // also a rate that is not a divisor of the frame rate
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", false);
      steps += fx.steps;
      drops += fx.drops;
    }
  harness::note("solver steps %d, backlog drops %d", steps, drops);
  CHECK(drops > 50);
}

TEST(smoke_events) {
  Expect ex;
  diff::Rand r(3104);
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

TEST(smoke_tunables) {
  Expect ex;
  diff::Rand r(3105);
  Smoke proto;
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
      play(ex, fx, drive, rng, r, Dt::k60, kSmallGrid ? 12.0f : 6.0f, tag, false);
      play(ex, fx, bump, rng, r, Dt::kIrregular, kSmallGrid ? 6.0f : 4.0f, tag, true);
      // Hard driving: the plume thrown against every wall in turn.
      char label[160];
      float t = 0.0f;
      for (int frame = 0; frame < (kSmallGrid ? 300 : 180); ++frame) {
        t += 1.0f / 60.0f;
        const float s = std::sin(t * 1.7f), q = std::cos(t * 0.9f);
        if (!EXPECT(ex, fx.update(dyn_g(2.5f * s, 1.2f * s * q, -2.0f * q), 1.0f / 60.0f))) break;
        std::snprintf(label, sizeof(label), "%s hard frame %d", tag, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
      // Change it again mid-run.
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, kSmallGrid ? 3.0f : 2.0f, tag, false);
    }
  }
}

TEST(smoke_rate_and_iters) {
  Expect ex;
  diff::Rand r(3106);
  const float rates[] = {20.0f, 24.5f, 41.0f, 61.0f, 120.0f};
  const float iters[] = {1.0f, 1.49f, 6.0f, 13.5f, 30.0f};
  int run = 0;
  for (float rate : rates)
    for (float it : iters) {
      char tag[96];
      std::snprintf(tag, sizeof(tag), "rate=%g iters=%g", static_cast<double>(rate), static_cast<double>(it));
      Fx fx;
      EXPECT(ex, fx.set("rate", rate));
      EXPECT(ex, fx.set("iters", it));
      fx.activate();
      gm::Rng rng(3500u + static_cast<uint32_t>(run));
      const gm::Scenario& sc = gm::Scenario::preset(run % gm::Scenario::presetCount());
      play(ex, fx, sc, rng, r, run % 2 ? Dt::kIrregular : Dt::k60, kSmallGrid ? 10.0f : 4.0f, tag, run % 3 == 0);
      // Change both mid-run.
      EXPECT(ex, fx.set("rate", r.frange(20.0f, 120.0f)));
      EXPECT(ex, fx.set("iters", r.frange(1.0f, 30.0f)));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 2.0f, tag, false);
      ++run;
    }
}

TEST(smoke_nan_inf_g) {
  Expect ex;
  diff::Rand r(3107);
  // NaN or infinite g poisons the velocity field (as in the C++) until the
  // next reset; the density stays finite because advection clamps its source
  // position NaN-safely, so the frames keep matching.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, -3e38f, 1e6f, -1e6f, 1e-30f};
  for (int run = 0; run < (kSmallGrid ? 20 : 8); ++run) {
    Fx fx;
    if (run % 3 == 1) EXPECT(ex, fx.set("rate", 120.0f));
    if (run % 4 == 2) EXPECT(ex, fx.set("wind", 0.0f));
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
        harness::note("bad g run %d frame %d: state differs", run, frame);
        break;
      }
      EXPECT(ex, std::isfinite(gm_smoke_total_density(fx.c.get())));
      if (r.chance(0.01)) fx.reset();
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
  // All-NaN from the very first update.
  {
    Fx fx;
    fx.activate();
    gm::GForce g;
    g.proper = gm::Vec3{diff::kNaN, diff::kNaN, diff::kNaN};
    for (int frame = 0; frame < 300; ++frame) {
      if (!EXPECT(ex, fx.update(g, 1.0f / 60.0f))) break;
      if (!EXPECT(ex, fx.render_same("all NaN"))) break;
    }
    EXPECT(ex, std::isfinite(gm_smoke_total_density(fx.c.get())));
  }
}

TEST(smoke_long_run) {
  Expect ex;
  diff::Rand r(3108);
  // 3 minutes at 60 fps ...
  const float minutes = kSmallGrid ? 3.0f : 1.0f;
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7000u);
    const int frames =
        play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, minutes * 60.0f, "long 60fps", false);
    EXPECT(ex, frames >= static_cast<int>(minutes) * 60 * 60);
  }
  // ... and 10 minutes at the engine's slowest step, heavy emitter, no decay.
  {
    Fx fx;
    EXPECT(ex, fx.set("emit", 200.0f));
    EXPECT(ex, fx.set("dissipate", 0.0f));
    fx.activate();
    gm::Rng rng(7001u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    const int frames = (kSmallGrid ? 600 : 180) * 15;
    for (int frame = 0; frame < frames; ++frame) {
      t += 1.0f / 15.0f;
      if (!EXPECT(ex, fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f))) break;
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    harness::note("total density after %d min without decay: %g", frames / 900,
                  static_cast<double>(gm_smoke_total_density(fx.c.get())));
  }
}

TEST(smoke_fuzz) {
  Expect ex;
  diff::Rand r(3109);
  Smoke proto;
  for (int run = 0; run < (kSmallGrid ? 30 : 10); ++run) {
    Fx fx;
    for (int i = 0; i < proto.params().count(); ++i) {
      const gm::Param& prm = proto.params().at(i);
      if (r.chance(0.6)) EXPECT(ex, fx.set(prm.name, r.frange(prm.min, prm.max)));
    }
    if (r.chance(0.8)) fx.activate();
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
        harness::note("%s: state differs after an update", label);
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
      if (r.chance(0.003)) fx.deactivate();
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}
