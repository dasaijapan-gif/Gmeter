// Differential tests of the "slosh" effect (gm_slosh.c vs gm::SloshEffect): the
// parameter table and grid geometry, every scenario preset at 60 fps and with
// irregular dt, every tunable at its min, max and a middle value, every style
// and surface thickness, the substep cap, violent g (dry cells), vertical
// jolts (bob and splash), ripples, runtime fill changes,
// reset/activate/deactivate cycles mid-run, NaN/inf g, long runs and random
// fuzzing. After every update all height cells, the total volume, gravity_px
// and the substep count must match bit for bit, and every update batch is
// followed by a render into cleared frames that must match byte for byte.
#include <algorithm>
#include <climits>
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

// The C++ render, restated with rows above the panel skipped (they draw
// nothing) and iround as the x86 build of the C++ computes it (INT_MIN for NaN
// and out-of-range values). Used only where the C++ render itself would loop
// for ~2^31 rows per column (style 1 with a NaN or huge surface height, which
// only absurd g input produces); slosh_nan_inf_g checks it against the C++
// render wherever that one finishes.
void reference_render(const gm_slosh_t* s, gm::VoxelFrame& f) {
  const float bottom = static_cast<float>(gm::kH) - 1.0f;
  const float bob = s->bob.pos;
  const int thick = std::max(1, gm::iround(s->surface_px));
  const int style = gm::iround(s->style);
  for (int z = 0; z < GM_SLOSH_NZ; ++z) {
    for (int px = 0; px < gm::kW; ++px) {
      const float cx = (static_cast<float>(px) + 0.5f) / gm::SloshEffect::DX - 0.5f;
      int i0 = gm::ifloor(cx);
      const float t = cx - static_cast<float>(i0);
      i0 = gm::clampi(i0, 0, GM_SLOSH_NX - 1);
      const int i1 = gm::clampi(i0 + 1, 0, GM_SLOSH_NX - 1);
      const float h = gm::lerpf(s->h[z][i0], s->h[z][i1], t) + bob;
      const float yr = std::floor(bottom - h + 0.5f);
      const int y_surf = yr >= -2147483648.0f && yr < 2147483648.0f ? static_cast<int>(yr) : INT_MIN;
      if (h < 0.5f) continue;
      for (int k = 0; k < thick; ++k) f.set(px, y_surf + k, z);
      if (style == 1) {
        for (int y = std::max(0, y_surf + thick); y <= gm::kH - 1; ++y) f.plot(px, y, z, s->body_intensity);
      } else if (style == 2) {
        f.vline(z, px, y_surf + thick, gm::kH - 1);
      }
    }
  }
}

// False when the C++ render would take ~|surface row| iterations per column
// (style 1 with a NaN, infinite or huge surface height).
bool cpp_render_fast(const gm_slosh_t* s) {
  if (gm::iround(s->style) != 1) return true;
  if (!std::isfinite(s->bob.pos)) return false;
  for (int z = 0; z < GM_SLOSH_NZ; ++z)
    for (int x = 0; x < GM_SLOSH_NX; ++x)
      if (!std::isfinite(s->h[z][x]) || !(s->h[z][x] + s->bob.pos < 4096.0f)) return false;
  return true;
}

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_slosh_t> c{new gm_slosh_t};
  std::unique_ptr<gm::SloshEffect> p{new gm::SloshEffect};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters (updates / rendered frames).
  int capped = 0;      // updates where the substep cap bound
  int splashes = 0;    // updates with a splash (vertical jerk > 2 g/s)
  int dry = 0;         // frames with a dry cell (h < 0.05)
  int max_steps = 0;   // most substeps in one update
  int slow = 0;        // frames checked against reference_render instead of the C++ render
  int nonfinite = 0;   // frames with a non-finite height or bob

  Fx() { gm_slosh_init(c.get()); }
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
  // Every height cell, the total volume, gravity_px and the substep count.
  bool same_state(const char* label) const {
    for (int z = 0; z < GM_SLOSH_NZ; ++z)
      for (int x = 0; x < GM_SLOSH_NX; ++x)
        if (!diff::same(gm_slosh_height(c.get(), x, z), p->height(x, z))) {
          harness::note("%s: height (%d, %d) C %.9g, C++ %.9g", label, x, z,
                        static_cast<double>(gm_slosh_height(c.get(), x, z)), static_cast<double>(p->height(x, z)));
          return false;
        }
    if (!diff::same(gm_slosh_total_volume(c.get()), p->totalVolume())) {
      harness::note("%s: total volume C %.9g, C++ %.9g", label, static_cast<double>(gm_slosh_total_volume(c.get())),
                    static_cast<double>(p->totalVolume()));
      return false;
    }
    if (!diff::same(gm_slosh_gravity_px(c.get()), p->gravityPx())) {
      harness::note("%s: gravity_px differs", label);
      return false;
    }
    if (gm_slosh_last_substeps(c.get()) != p->lastSubsteps()) {
      harness::note("%s: substeps C %d, C++ %d", label, gm_slosh_last_substeps(c.get()), p->lastSubsteps());
      return false;
    }
    return true;
  }
  // Advances both; true when the states match afterwards.
  bool update(const gm::GForce& g, float dt, const char* label = "update") {
    // Splash coverage, from the state before the update (as the effect sees it).
    const float vg = g.proper.y - 1.0f;
    const float jerk = dt > 0.0f ? std::fabs(vg - c->last_vert_g) / dt : 0.0f;
    if (jerk > 2.0f && c->splash_gain > 0.0f) ++splashes;
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
    if (c->last_substeps == static_cast<int>(c->max_substeps)) ++capped;
    max_steps = std::max(max_steps, c->last_substeps);
    return same_state(label);
  }
  // Renders both into cleared frames; true when the frames and states match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    bool ok;
    if (cpp_render_fast(c.get())) {
      p->render(*f.p);
      ok = f.same(label);
    } else {
      ++slow;
      reference_render(c.get(), *f.p);
      ok = f.same(label);
    }
    ok = same_state(label) && ok;
    // Coverage.
    bool any_dry = false, finite = std::isfinite(c->bob.pos);
    for (int z = 0; z < GM_SLOSH_NZ; ++z)
      for (int x = 0; x < GM_SLOSH_NX; ++x) {
        if (c->h[z][x] < 0.05f) any_dry = true;
        if (!std::isfinite(c->h[z][x])) finite = false;
      }
    if (any_dry) ++dry;
    if (!finite) ++nonfinite;
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
    std::snprintf(label, sizeof(label), "%s %s frame %d t=%.4f", tag, sc.name(), frames + 1, static_cast<double>(t));
    bool ok = true;
    for (int u = 0; u < updates && ok; ++u) {
      const float dt = mode == Dt::k60 ? 1.0f / 60.0f : odd_dt(r);
      t += dt;
      ok = fx.update(sc.at(std::fmod(t, loop), &rng), dt, label);
    }
    ++frames;
    if (!EXPECT(ex, ok)) break;
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

// A stretch of hand-made g: `kind` 0 violent lateral/longitudinal square
// waves, 1 vertical jolts, 2 both plus random spikes. Stops at the first
// mismatch; false then.
bool violent(Expect& ex, Fx& fx, diff::Rand& r, int kind, int frames, bool irregular, const char* tag) {
  char label[160];
  float t = 0.0f;
  for (int frame = 0; frame < frames; ++frame) {
    const float dt = irregular ? odd_dt(r) : 1.0f / 60.0f;
    t += dt;
    gm::Vec3 d;
    if (kind != 1) {
      const float sq = std::fmod(t, 2.4f) < 1.2f ? 1.0f : -1.0f;
      d.x = 3.0f * sq * std::sin(t * 0.7f);
      d.z = 2.5f * std::cos(t * 1.1f) * sq;
    }
    if (kind != 0)  // short jolts every 0.9 s, a longer dip every 3.1 s
      d.y = std::fmod(t, 0.9f) < 0.06f ? r.frange(-2.5f, 2.5f) : (std::fmod(t, 3.1f) < 0.3f ? -0.9f : 0.0f);
    if (kind == 2 && r.chance(0.03)) d = gm::Vec3{r.frange(-8, 8), r.frange(-8, 8), r.frange(-8, 8)};
    std::snprintf(label, sizeof(label), "%s violent %d frame %d", tag, kind, frame);
    if (!EXPECT(ex, fx.update(dyn_g(d.x, d.y, d.z), dt, label))) return false;
    if (!EXPECT(ex, fx.render_same(label))) return false;
  }
  return true;
}

}  // namespace

TEST(slosh_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "slosh") == 0);
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
  // Grid geometry.
  EXPECT(ex, GM_SLOSH_NX == gm::SloshEffect::NX);
  EXPECT(ex, GM_SLOSH_NZ == gm::SloshEffect::NZ);
  EXPECT(ex, harness::same_float(GM_SLOSH_DX, gm::SloshEffect::DX));
  EXPECT(ex, harness::same_float(GM_SLOSH_DZ, gm::SloshEffect::DZ));
  // Each name points at the field the header documents.
  gm_slosh_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"fill", &c->fill_fraction},    {"tank_m", &c->tank_width_m},  {"slope_gain", &c->slope_gain},
                {"damping", &c->damping},       {"viscosity", &c->viscosity},  {"ripple", &c->ripple},
                {"bob", &c->bob_px_per_g},      {"bob_hz", &c->bob_hz},        {"splash", &c->splash_gain},
                {"style", &c->style},           {"body_i", &c->body_intensity}, {"surface_px", &c->surface_px},
                {"substeps", &c->max_substeps}};
  const int nfields = static_cast<int>(sizeof(fields) / sizeof(fields[0]));
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == nfields);
  for (int i = 0; i < nfields; ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));
  }
  // The rest of the state: a level, still tank at the rest depth.
  const float h0 = 0.42f * static_cast<float>(GM_H);
  bool level = true;
  for (int z = 0; z < GM_SLOSH_NZ; ++z) {
    for (int x = 0; x < GM_SLOSH_NX; ++x) level = level && c->h[z][x] == h0;
    for (int x = 0; x <= GM_SLOSH_NX; ++x) level = level && c->u[z][x] == 0.0f;
  }
  for (int z = 0; z <= GM_SLOSH_NZ; ++z)
    for (int x = 0; x < GM_SLOSH_NX; ++x) level = level && c->w[z][x] == 0.0f;
  EXPECT(ex, level);
  EXPECT(ex, c->target_volume == h0 * GM_SLOSH_NX * GM_SLOSH_NZ);
  EXPECT(ex, c->bob.pos == 0.0f && c->bob.vel == 0.0f);
  EXPECT(ex, c->last_vert_g == 0.0f && c->gv_filtered == 1.0f && c->last_substeps == 0);
  EXPECT(ex, c->rng.s == 12345u);
  EXPECT(ex, fx.same_state("fresh"));
  EXPECT(ex, fx.render_same("fresh render"));
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.1f, 0.5f, 1.0f, 1.5f, 2.0f, 5.0f, 32.0f, 121.0f, 1e9f, diff::kInf,
                          -diff::kInf, diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  EXPECT(ex, fx.same_state("after sets"));
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
}

TEST(slosh_presets_60fps) {
  Expect ex;
  diff::Rand r(3201);
  int splashes = 0, dry = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", false);
    EXPECT(ex, frames >= 22 * 60);
    splashes += fx.splashes;
    dry += fx.dry;
  }
  harness::note("updates with a splash: %d, frames with a dry cell: %d", splashes, dry);
  CHECK(splashes > 100);
}

TEST(slosh_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(3202);
  int capped = 0;
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      if (rep) EXPECT(ex, fx.set("ripple", 20.0f));
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", false);
      capped += fx.capped;
    }
  // The long steps (0.5 s and more) need more than the default 12 substeps.
  harness::note("updates with the substep cap binding: %d", capped);
  CHECK(capped > 100);
}

TEST(slosh_events) {
  Expect ex;
  diff::Rand r(3203);
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

TEST(slosh_tunables) {
  Expect ex;
  diff::Rand r(3204);
  gm::SloshEffect proto;
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
  int capped = 0, dry = 0;
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
      EXPECT(ex, fx.same_state(tag));
      gm::Rng rng(3000u + static_cast<uint32_t>(i));
      play(ex, fx, drive, rng, r, Dt::k60, 22.0f, tag, false);
      play(ex, fx, bump, rng, r, Dt::kIrregular, 6.0f, tag, true);
      violent(ex, fx, r, 2, 600, false, tag);
      // Change it again mid-run (a fill change eases the volume to the new target).
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag, false);
      capped += fx.capped;
      dry += fx.dry;
    }
  }
  harness::note("updates with the substep cap binding: %d, frames with a dry cell: %d", capped, dry);
  CHECK(capped > 100);
  CHECK(dry > 1000);
}

TEST(slosh_styles) {
  Expect ex;
  diff::Rand r(3205);
  // Every style value (iround: <0.5 surface, <1.5 body, else solid), surface
  // thicknesses around the rounding points, body densities from none to full.
  const float styles[] = {0.0f, 0.3f, 0.49f, 0.5f, 0.75f, 1.0f, 1.49f, 1.5f, 1.8f, 2.0f};
  const float surfaces[] = {1.0f, 1.49f, 1.5f, 2.5f, 3.2f, 4.0f};
  const float bodies[] = {0.0f, 0.01f, 0.12f, 0.5f, 0.99f, 1.0f};
  const gm::Scenario& slalom = *gm::Scenario::find("slalom");
  for (int si = 0; si < 10; ++si)
    for (int ti = 0; ti < 6; ++ti) {
      char tag[96];
      const float body = bodies[(si + ti) % 6];
      std::snprintf(tag, sizeof(tag), "style=%g surface_px=%g body_i=%g", static_cast<double>(styles[si]),
                    static_cast<double>(surfaces[ti]), static_cast<double>(body));
      Fx fx;
      EXPECT(ex, fx.set("style", styles[si]));
      EXPECT(ex, fx.set("surface_px", surfaces[ti]));
      EXPECT(ex, fx.set("body_i", body));
      if (ti % 2) EXPECT(ex, fx.set("fill", ti % 4 == 1 ? 0.8f : 0.1f));
      if (ti % 3 == 2) EXPECT(ex, fx.set("bob", 30.0f));
      fx.activate();
      gm::Rng rng(3500u + static_cast<uint32_t>(si * 6 + ti));
      play(ex, fx, slalom, rng, r, ti % 2 ? Dt::kIrregular : Dt::k60, 5.0f, tag, false);
      violent(ex, fx, r, si % 3, 240, false, tag);
    }
  // Each style for a full 20 s preset run.
  for (int s = 0; s <= 2; ++s) {
    Fx fx;
    EXPECT(ex, fx.set("style", static_cast<float>(s)));
    fx.activate();
    gm::Rng rng(3600u + static_cast<uint32_t>(s));
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 21.0f, "style run", false);
    EXPECT(ex, frames >= 21 * 60);
  }
}

TEST(slosh_substep_cap) {
  Expect ex;
  diff::Rand r(3206);
  // The cap from its minimum to its maximum (including fractional values,
  // truncated), with fast waves (small tank) and long steps so it binds.
  const float caps[] = {2.0f, 2.5f, 3.0f, 7.9f, 8.0f, 12.0f, 20.0f, 31.99f, 32.0f};
  const float tanks[] = {0.1f, 1.0f, 10.0f};
  const float steps[] = {1.0f / 60.0f, 1.0f / 15.0f, 0.25f, 1.0f, 2.5f, 1e-6f, 0.0f};
  int capped = 0, most = 0;
  for (float cap : caps)
    for (float tank : tanks) {
      Fx fx;
      EXPECT(ex, fx.set("substeps", cap));
      EXPECT(ex, fx.set("tank_m", tank));
      if (tank < 0.5f) EXPECT(ex, fx.set("damping", 0.0f));
      fx.activate();
      char label[160];
      float t = 0.0f;
      for (int frame = 0; frame < 500; ++frame) {
        const float dt = frame % 40 < 30 ? steps[(frame / 40) % 7] : r.pick(steps);
        t += dt;
        const float sq = std::fmod(t, 1.1f) < 0.55f ? 1.0f : -1.0f;
        const gm::GForce g = dyn_g(1.8f * sq, std::fmod(t, 0.7f) < 0.05f ? -1.5f : 0.0f, 1.2f * std::sin(t * 2.3f));
        std::snprintf(label, sizeof(label), "cap %g tank %g frame %d", static_cast<double>(cap),
                      static_cast<double>(tank), frame);
        if (!EXPECT(ex, fx.update(g, dt, label))) break;
        EXPECT(ex, fx.c->last_substeps >= 1 && fx.c->last_substeps <= static_cast<int>(cap));
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
      capped += fx.capped;
      most = std::max(most, fx.max_steps);
    }
  harness::note("updates with the cap binding: %d, most substeps: %d", capped, most);
  CHECK(capped > 1000);
  CHECK(most == 32);
}

TEST(slosh_substep_count_boundaries) {
  Expect ex;
  // The substep count is ceil(dt / dt_max): steps of exactly k * dt_max take k
  // substeps, one ulp more takes k + 1. From a fresh tank at rest dt_max does
  // not depend on dt, so compute it as the effect does and probe around its
  // multiples.
  int exact = 0, probes = 0;
  for (float fill : {0.1f, 0.42f, 0.8f})
    for (float tank : {0.1f, 1.0f, 3.0f, 10.0f}) {
      gm::SloshEffect proto;
      proto.params().set("fill", fill);
      proto.params().set("tank_m", tank);
      proto.activate();
      const float gv = 1.0f * proto.gravityPx();
      const float hmax = fill * static_cast<float>(GM_H);
      const float c = 0.0f + std::sqrt(gv * std::max(hmax, 1.0f));
      const float inv_spacing = std::sqrt(1.0f / (GM_SLOSH_DX * GM_SLOSH_DX) + 1.0f / (GM_SLOSH_DZ * GM_SLOSH_DZ));
      const float dt_max = 0.7f / (std::max(c, 1e-3f) * inv_spacing);
      for (int k = 1; k <= 33; ++k) {
        float dt = dt_max * static_cast<float>(k);
        for (int i = 0; i < 3; ++i) dt = std::nextafter(dt, 0.0f);
        for (int i = 0; i < 7; ++i, dt = std::nextafter(dt, 1e9f)) {
          Fx fx;
          EXPECT(ex, fx.set("fill", fill));
          EXPECT(ex, fx.set("tank_m", tank));
          EXPECT(ex, fx.set("substeps", 32.0f));
          fx.activate();
          char label[160];
          std::snprintf(label, sizeof(label), "fill %g tank %g k %d probe %d", static_cast<double>(fill),
                        static_cast<double>(tank), k, i);
          if (!EXPECT(ex, fx.update(gm::GForce(), dt, label))) break;
          if (dt / dt_max == static_cast<float>(k)) ++exact;
          ++probes;
          if (!EXPECT(ex, fx.render_same(label))) break;
          // A second step from the moving state.
          if (!EXPECT(ex, fx.update(dyn_g(0.3f, -0.2f, 0.1f), dt, label))) break;
          if (!EXPECT(ex, fx.render_same(label))) break;
        }
      }
    }
  harness::note("%d probes, %d with dt exactly k * dt_max", probes, exact);
  CHECK(exact > 100);
}

TEST(slosh_extreme_steps) {
  Expect ex;
  diff::Rand r(3211);
  // Slow waves (big tank, low vertical g) make dt_max long, so long frames run
  // long substeps: damping * dt beyond 1 (the damping factor clamps at 0) and
  // viscosity * dt / dx^2 beyond 0.25 (the diffusion clamps).
  struct Cfg {
    float tank, damping, viscosity, cap;
  } cfgs[] = {{10.0f, 10.0f, 60.0f, 32.0f}, {10.0f, 10.0f, 0.0f, 2.0f}, {10.0f, 0.0f, 60.0f, 12.0f},
              {5.0f, 6.0f, 40.0f, 4.0f},    {0.1f, 10.0f, 60.0f, 32.0f}, {10.0f, 3.7f, 25.0f, 7.0f}};
  const float steps[] = {1.0f / 60.0f, 0.05f, 0.1f, 0.115f, 0.3f, 0.35f, 0.6f, 1.0f, 2.5f};
  const float lifts[] = {0.2f, 0.25f, 0.5f, 1.0f, 3.0f, -1.0f, 4.0f};  // proper.y (clamped to 0.2..3)
  const float low_lifts[] = {0.2f, 0.2f, 0.21f, -1.0f, 0.5f};
  int damp_clamped = 0, smooth_clamped = 0;
  for (const Cfg& cfg : cfgs)
    for (int variant = 0; variant < 4; ++variant) {
      Fx fx;
      EXPECT(ex, fx.set("tank_m", cfg.tank));
      EXPECT(ex, fx.set("damping", cfg.damping));
      EXPECT(ex, fx.set("viscosity", cfg.viscosity));
      EXPECT(ex, fx.set("substeps", cfg.cap));
      if (variant == 1 || variant == 3) EXPECT(ex, fx.set("fill", 0.1f));
      if (variant == 2) EXPECT(ex, fx.set("slope_gain", 2.0f));
      fx.activate();
      char label[160];
      float lift = 1.0f;
      for (int frame = 0; frame < (variant == 3 ? 1200 : 400); ++frame) {
        if (frame % 50 == 0) lift = variant == 3 ? r.pick(low_lifts) : r.pick(lifts);
        // Variant 3 draws long steps from a continuum: substeps reach their
        // longest when dt falls just below a multiple of dt_max.
        const float dt = frame % 3 ? (variant == 3 ? r.frange(0.1f, 3.0f) : r.pick(steps)) : 1.0f / 60.0f;
        const float sq = (frame / 25) % 2 ? 1.0f : -1.0f;
        // Variant 3: a shallow, nearly still tank in near free fall (the slowest waves).
        const float push = variant == 3 ? 0.03f : 1.0f;
        gm::GForce g;
        g.proper = gm::Vec3{0.8f * push * sq, lift, 0.5f * push * std::sin(static_cast<float>(frame) * 0.2f)};
        std::snprintf(label, sizeof(label), "tank %g damping %g viscosity %g cap %g variant %d frame %d",
                      static_cast<double>(cfg.tank), static_cast<double>(cfg.damping),
                      static_cast<double>(cfg.viscosity), static_cast<double>(cfg.cap), variant, frame);
        if (!EXPECT(ex, fx.update(g, dt, label))) break;
        // Coverage: below the cap the substeps are exactly dt / n long.
        if (fx.c->last_substeps < static_cast<int>(cfg.cap)) {
          const float sub = dt / static_cast<float>(fx.c->last_substeps);
          if (cfg.damping * sub > 1.0f) ++damp_clamped;
          if (cfg.viscosity * sub / (GM_SLOSH_DX * GM_SLOSH_DX) > 0.25f) ++smooth_clamped;
        }
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
    }
  harness::note("updates with substeps longer than 1/damping: %d, with clamped diffusion: %d", damp_clamped,
                smooth_clamped);
  CHECK(damp_clamped > 100);
  CHECK(smooth_clamped > 100);
}

TEST(slosh_violent_and_jolts) {
  Expect ex;
  diff::Rand r(3207);
  int splashes = 0, dry = 0;
  for (int run = 0; run < 12; ++run) {
    Fx fx;
    if (run % 3 == 1) EXPECT(ex, fx.set("splash", 300.0f));
    if (run % 3 == 2) EXPECT(ex, fx.set("splash", 0.0f));
    if (run % 4 == 1) EXPECT(ex, fx.set("ripple", 50.0f));
    if (run % 4 == 2) EXPECT(ex, fx.set("slope_gain", 2.0f));
    if (run % 5 == 3) EXPECT(ex, fx.set("bob", 30.0f));
    if (run % 6 == 4) EXPECT(ex, fx.set("tank_m", 0.1f));
    fx.activate();
    char tag[64];
    std::snprintf(tag, sizeof(tag), "run %d", run);
    for (int kind = 0; kind < 3; ++kind)
      if (!violent(ex, fx, r, kind, 720, run % 2 == 1, tag)) break;
    // Runtime fill changes while sloshing.
    EXPECT(ex, fx.set("fill", run % 2 ? 0.1f : 0.8f));
    violent(ex, fx, r, run % 3, 300, false, tag);
    EXPECT(ex, fx.set("fill", 0.42f));
    violent(ex, fx, r, 2, 300, true, tag);
    splashes += fx.splashes;
    dry += fx.dry;
  }
  harness::note("updates with a splash: %d, frames with a dry cell: %d", splashes, dry);
  CHECK(splashes > 1000);
  CHECK(dry > 1000);
}

TEST(slosh_nan_inf_g) {
  Expect ex;
  diff::Rand r(3208);
  // The C++ handles all of these without trapping undefined behaviour (its
  // float-to-int conversions of NaN/huge values give INT_MIN on x86, which the
  // C reproduces). A NaN, infinite or huge vertical g turns the bob (and so
  // every surface height) NaN or huge, and in style 1 the C++ render then
  // loops ~2^31 rows per column; those frames are checked against
  // reference_render, which is itself checked against the C++ render on every
  // frame the C++ render can finish.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, -3e38f, 1e6f, -1e6f, 1e-30f};
  int slow = 0, nonfinite = 0, ref_checked = 0;
  std::unique_ptr<gm::VoxelFrame> ref(new gm::VoxelFrame);
  for (int run = 0; run < 45; ++run) {
    Fx fx;
    EXPECT(ex, fx.set("style", static_cast<float>(run % 3)));
    if (run % 4 == 1) EXPECT(ex, fx.set("bob", 0.0f));  // inf * 0
    if (run % 4 == 2) EXPECT(ex, fx.set("ripple", 30.0f));
    if (run % 5 == 3) EXPECT(ex, fx.set("surface_px", 4.0f));
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
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.update(g, dt, label))) break;
      if (r.chance(0.02)) fx.reset();
      if (!EXPECT(ex, fx.render_same(label))) break;
      // The reference against the C++ render wherever the latter finishes.
      if (cpp_render_fast(fx.c.get())) {
        ref->clear();
        reference_render(fx.c.get(), *ref);
        if (!EXPECT(ex, std::memcmp(ref->data(), fx.f.p->data(), GM_BYTES_PER_FRAME) == 0)) break;
        ++ref_checked;
      }
    }
    slow += fx.slow;
    nonfinite += fx.nonfinite;
  }
  harness::note("frames with a non-finite state: %d, checked against the reference: %d (the reference against "
                "the C++: %d)",
                nonfinite, slow, ref_checked);
  CHECK(nonfinite > 1000);
  CHECK(slow > 500);
}

TEST(slosh_long_run) {
  Expect ex;
  diff::Rand r(3209);
  // 5 minutes at 60 fps (renormalisation keeps the volume; the rng runs on) ...
  {
    Fx fx;
    EXPECT(ex, fx.set("ripple", 5.0f));
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 300.0f, "long 60fps", false);
    EXPECT(ex, frames >= 300 * 60);
  }
  // ... and 10 minutes at the engine's slowest step with fast waves and no damping.
  {
    Fx fx;
    EXPECT(ex, fx.set("tank_m", 0.1f));
    EXPECT(ex, fx.set("damping", 0.0f));
    EXPECT(ex, fx.set("substeps", 4.0f));
    fx.activate();
    gm::Rng rng(7001u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 600 * 15; ++frame) {
      t += 1.0f / 15.0f;
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f, label))) break;
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(slosh_fuzz) {
  Expect ex;
  diff::Rand r(3210);
  gm::SloshEffect proto;
  for (int run = 0; run < 60; ++run) {
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
      std::snprintf(label, sizeof(label), "fuzz run %d frame %d", run, frame);
      const int updates = r.chance(0.1) ? r.irange(0, 3) : 1;
      bool ok = true;
      for (int u = 0; u < updates && ok; ++u) ok = fx.update(dyn_g(d.x, d.y, d.z), odd_dt(r), label);
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
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}
