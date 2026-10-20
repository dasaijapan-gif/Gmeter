// Differential tests of the "hud" effect (gm_hud.c vs gm::HudEffect): the
// parameter table, every scenario preset at 60 fps and with irregular dt,
// every tunable at its min, max and a middle value, g beyond warn_g and
// danger_g, bumps, the anti burn-in orbit (orbit_min, long runs, set_orbit_step
// and the burn_in_offset vtable entry), reset/activate/deactivate cycles,
// NaN/inf g and random fuzzing. Every update is followed by a render into
// cleared frames that must match byte for byte (also after the engine's
// burn-in shift), and the title word, orbit offset, ball and tip positions and
// driving state must match after every update.
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

const char* const kWords[] = {"待機", "加速", "制動", "左旋回", "右旋回", "衝撃", "危険"};
constexpr int kWordCount = 7;

int word_index(const char* w) {
  for (int i = 0; i < kWordCount; ++i)
    if (std::strcmp(w, kWords[i]) == 0) return i;
  return kWordCount;
}

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_hud_t> c{new gm_hud_t};
  std::unique_ptr<gm::HudEffect> p{new gm::HudEffect};
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters (frames).
  int words[kWordCount + 1] = {};
  int warn = 0, flash = 0, shifted = 0;
  int orbit_changes = 0;
  bool orbit_order_ok = true;
  int last_step = -1;

  Fx() { gm_hud_init(c.get()); }
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
  void set_orbit_step(int s) {
    gm_hud_set_orbit_step(c.get(), s);
    p->setOrbitStep(s);
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
  // Title word, orbit offset, ball, tip and driving state.
  bool same_state(const char* label) {
    bool ok = true;
    const char* wc = gm_hud_title_text(c.get());
    const char* wp = p->titleText();
    if (std::strcmp(wc, wp) != 0) {
      harness::note("%s: title C %s, C++ %s", label, wc, wp);
      ok = false;
    }
    int dxc = -9, dyc = -9, dxp = -7, dyp = -7;
    gm_effect_burn_in_offset(ce(), &dxc, &dyc);
    p->burnInOffset(dxp, dyp);
    if (gm_hud_orbit_x(c.get()) != p->orbitX() || gm_hud_orbit_y(c.get()) != p->orbitY() || dxc != dxp ||
        dyc != dyp || dxc != p->orbitX() || dyc != p->orbitY()) {
      harness::note("%s: orbit C (%d,%d) offset (%d,%d), C++ (%d,%d) offset (%d,%d)", label, gm_hud_orbit_x(c.get()),
                    gm_hud_orbit_y(c.get()), dxc, dyc, p->orbitX(), p->orbitY(), dxp, dyp);
      ok = false;
    }
    if (!diff::same(gm_hud_ball_position(c.get()), p->ballPosition()) ||
        !diff::same(gm_hud_tip_position(c.get()), p->tipPosition())) {
      const gm_vec3_t bc = gm_hud_ball_position(c.get()), tc = gm_hud_tip_position(c.get());
      const gm::Vec3 bp = p->ballPosition(), tp = p->tipPosition();
      harness::note("%s: ball C (%.9g %.9g %.9g) C++ (%.9g %.9g %.9g), tip C (%.9g %.9g %.9g) C++ (%.9g %.9g %.9g)",
                    label, double(bc.x), double(bc.y), double(bc.z), double(bp.x), double(bp.y), double(bp.z),
                    double(tc.x), double(tc.y), double(tc.z), double(tp.x), double(tp.y), double(tp.z));
      ok = false;
    }
    if (static_cast<int>(gm_hud_state(c.get())) != static_cast<int>(p->state())) {
      harness::note("%s: state C %d, C++ %d", label, static_cast<int>(gm_hud_state(c.get())),
                    static_cast<int>(p->state()));
      ok = false;
    }
    return ok;
  }
  // Renders both into cleared frames; true when the frames match, before and
  // after the burn-in shift the engine applies, and the state matches.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    bool ok = f.same(label);
    ok = same_state(label) && ok;
    int dx = 0, dy = 0;
    gm_effect_burn_in_offset(ce(), &dx, &dy);
    if (dx || dy) {
      ++shifted;
      gm_frame_shift(f.c.get(), dx, dy);
      f.p->shift(dx, dy);
      char l2[256];
      std::snprintf(l2, sizeof(l2), "%s (shifted %d,%d)", label, dx, dy);
      ok = f.same(l2) && ok;
    }
    // Coverage.
    ++words[word_index(gm_hud_title_text(c.get()))];
    if (gm_gforce_horizontal(c->g) >= c->warn_g) ++warn;
    if (c->title > 0.5f && c->title_age < 0.35f && std::fmod(c->title_age, 0.12f) < 0.08f) ++flash;
    const int step = c->orbit_step;
    if (last_step >= 0 && step != last_step) {
      ++orbit_changes;
      if (step != ((last_step + 1) & 3) && step != 0) orbit_order_ok = false;
    }
    last_step = step;
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
        switch (r.irange(0, 4)) {
          case 0: fx.reset(); break;
          case 1: fx.deactivate(); fx.activate(); break;
          case 2: fx.activate(); break;
          case 3: fx.set_orbit_step(r.irange(-5, 9)); break;
          default: fx.deactivate(); break;  // keeps running after a deactivate, like a crossfade-out
        }
      }
    }
    std::snprintf(label, sizeof(label), "%s %s frame %d t=%.4f", tag, sc.name(), frames, static_cast<double>(t));
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

// Horizontal g swept from 0 to `peak` and back in direction `angle` (0 =
// right turn, pi/2 = accelerating), with a vertical bump every `bump_every`
// seconds when that is > 0.
gm::GForce sweep_g(float t, float period, float peak, float angle, float bump_every, float bump_g) {
  const float u = std::fmod(t, period) / period;
  const float mag = peak * (u < 0.5f ? 2.0f * u : 2.0f - 2.0f * u);
  float y = 0.0f;
  if (bump_every > 0.0f) {
    const float b = std::fmod(t, bump_every);
    if (b < 0.06f) y = bump_g;
    else if (b < 0.12f) y = -0.8f * bump_g;
  }
  return dyn_g(mag * std::cos(angle), y, mag * std::sin(angle));
}

}  // namespace

TEST(hud_params) {
  Expect ex;
  Fx fx;
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "hud") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // The vtable: activate falls back to reset, and the burn-in offset is wired.
  EXPECT(ex, fx.ce()->vt->activate == nullptr);
  EXPECT(ex, fx.ce()->vt->deactivate == nullptr);
  EXPECT(ex, fx.ce()->vt->reset != nullptr);
  EXPECT(ex, fx.ce()->vt->burn_in_offset != nullptr);
  EXPECT(ex, fx.same_state("fresh"));
  // Each name points at the field the header documents.
  gm_hud_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"scale_g", &c->scale_g},     {"vscale_g", &c->vscale_g}, {"dscale_g", &c->dscale_g},
                {"hz", &c->hz},               {"inertia", &c->inertia},   {"warn_g", &c->warn_g},
                {"danger_g", &c->danger_g},   {"ball_r", &c->ball_r},     {"title_z", &c->title_z},
                {"trail_s", &c->trail_s},     {"title", &c->title},       {"axes", &c->axes},
                {"frame", &c->frame},         {"enter_g", &c->enter_g},   {"hold_s", &c->hold_s},
                {"bump_g", &c->bump_g},       {"orbit_min", &c->orbit_min}};
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == static_cast<int>(sizeof(fields) / sizeof(fields[0])));
  for (int i = 0; i < static_cast<int>(sizeof(fields) / sizeof(fields[0])); ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));  // same order as the header lists them
  }
  // The rest of the state starts like the C++ object.
  EXPECT(ex, diff::same(c->g.proper, gm::GForce{}.proper));
  EXPECT(ex, diff::same(c->tracker.enter_g, 0.25f) && diff::same(c->tracker.hold_s, 0.4f) &&
                 diff::same(c->tracker.bump_g, 0.3f));
  EXPECT(ex, c->orbit_step == 0 && c->orbit_timer == 0.0f && c->trail_count == 0 && c->trail_head == 0);
  EXPECT(ex, c->title_age == 0.0f && c->bump_age == 99.0f && !c->danger && c->hazard == 0.0f && c->time == 0.0f);
  // Rendering before any update (the C++ constructor already reset).
  EXPECT(ex, fx.render_same("fresh render"));
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -1.0f, 0.0f, 0.05f, 0.5f, 1.0f, 7.0f, 31.0f, 1e9f,
                          diff::kInf, -diff::kInf, diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // A fresh effect after all that still has the defaults.
  Fx fresh;
  EXPECT(ex, same_param_tables(gm_effect_params(fresh.ce()), fresh.p->params()));
}

TEST(hud_presets_60fps) {
  Expect ex;
  diff::Rand r(1101);
  int words[kWordCount + 1] = {};
  int warn = 0, flash = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    Fx fx;
    fx.activate();
    gm::Rng rng(1000u + static_cast<uint32_t>(i));
    const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, "60fps", false);
    EXPECT(ex, frames >= 22 * 60);
    for (int w = 0; w <= kWordCount; ++w) words[w] += fx.words[w];
    warn += fx.warn;
    flash += fx.flash;
  }
  harness::note("title frames: 待機 %d 加速 %d 制動 %d 左旋回 %d 右旋回 %d 衝撃 %d 危険 %d; warn %d, flash %d", words[0],
                words[1], words[2], words[3], words[4], words[5], words[6], warn, flash);
  // Coverage: the presets reach every state word except 危険 (no preset goes past 1 g).
  for (int w = 0; w < 6; ++w) CHECK(words[w] > 30);
  CHECK(words[kWordCount] == 0);
  CHECK(warn > 100);
  CHECK(flash > 50);
}

TEST(hud_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(1102);
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx;
      fx.activate();
      gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
      play(ex, fx, sc, rng, r, Dt::kIrregular, 25.0f, "irregular", false);
    }
}

TEST(hud_events) {
  Expect ex;
  diff::Rand r(1103);
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    for (int mode = 0; mode < 2; ++mode) {
      Fx fx;
      EXPECT(ex, fx.set("orbit_min", 0.05f));  // the orbit moves during the run (every 3 s)
      fx.activate();
      gm::Rng rng(2500u + static_cast<uint32_t>(i * 3 + mode));
      play(ex, fx, sc, rng, r, mode ? Dt::kIrregular : Dt::k60, 24.0f, mode ? "events irregular" : "events 60fps",
           true);
      EXPECT(ex, fx.orbit_changes >= 3);
    }
  }
  // An effect that is never activated (the constructor reset it) and one
  // deactivated before it ever runs behave the same on both sides too.
  for (int k = 0; k < 2; ++k) {
    Fx fx;
    if (k) fx.deactivate();
    gm::Rng rng(2600u + static_cast<uint32_t>(k));
    play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 21.0f, k ? "deactivated" : "never activated", false);
  }
}

TEST(hud_tunables) {
  Expect ex;
  diff::Rand r(1104);
  gm::HudEffect proto;
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
      // Hard driving past warn_g and danger_g with bumps.
      char label[160];
      float t = 0.0f;
      for (int frame = 0; frame < 600; ++frame) {
        t += 1.0f / 60.0f;
        fx.update(sweep_g(t, 4.0f, 2.2f, 0.7f * static_cast<float>(frame / 150), 1.3f, 0.6f), 1.0f / 60.0f);
        std::snprintf(label, sizeof(label), "%s hard frame %d", tag, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
      // Change it again mid-run.
      EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
      play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag, false);
    }
  }
}

TEST(hud_warn_danger_bumps) {
  Expect ex;
  diff::Rand r(1105);
  int words[kWordCount + 1] = {};
  int warn = 0, flash = 0;
  const float warns[] = {0.1f, 0.7f, 1.3f, 3.0f};
  const float dangers[] = {0.2f, 1.0f, 2.0f, 4.0f};
  const float bumps[] = {0.05f, 0.3f, 0.9f, 2.0f};
  for (int k = 0; k < 16; ++k) {
    Fx fx;
    EXPECT(ex, fx.set("warn_g", warns[k % 4]));
    EXPECT(ex, fx.set("danger_g", dangers[(k / 4) % 4]));
    EXPECT(ex, fx.set("bump_g", bumps[(k + k / 4) % 4]));
    EXPECT(ex, fx.set("inertia", static_cast<float>(k % 2)));
    if (k % 3 == 0) EXPECT(ex, fx.set("orbit_min", 0.02f));
    fx.activate();
    char label[160];
    float t = 0.0f;
    const bool irregular = k >= 8;
    for (int frame = 0; frame < 2400; ++frame) {
      const float dt = irregular ? odd_dt(r) : 1.0f / 60.0f;
      t += dt;
      // Sweeps up to 4.5 g in eight directions, with bumps of growing size.
      const float angle = 0.785398f * static_cast<float>((frame / 300) % 8);
      const float bump_g = 0.1f + 0.4f * static_cast<float>((frame / 200) % 6);
      fx.update(sweep_g(t, 3.0f + static_cast<float>(k % 3), 4.5f, angle, 0.9f, bump_g), dt);
      std::snprintf(label, sizeof(label), "warn %g danger %g run %d frame %d", double(fx.c->warn_g),
                    double(fx.c->danger_g), k, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    for (int w = 0; w <= kWordCount; ++w) words[w] += fx.words[w];
    warn += fx.warn;
    flash += fx.flash;
  }
  harness::note("title frames: 待機 %d 加速 %d 制動 %d 左旋回 %d 右旋回 %d 衝撃 %d 危険 %d; warn %d, flash %d", words[0],
                words[1], words[2], words[3], words[4], words[5], words[6], warn, flash);
  for (int w = 0; w < kWordCount; ++w) CHECK(words[w] > 100);  // coverage: every word, 危険 and 衝撃 included
  CHECK(warn > 5000);
  CHECK(flash > 200);
}

TEST(hud_timing_edges) {
  Expect ex;
  diff::Rand r(1111);
  // Steps that are exact binary fractions or sums of them land the clocks
  // exactly on their thresholds: the 0.3 s of the 0.5 s 警告 blink, the 0.35 s
  // flash and its 0.12 / 0.08 s phases, the 0.3 s impact flash, the 1.2 s 衝撃
  // and 3 s idle fade, and the hazard clock's wrap at exactly 1000.
  const float steps[] = {0.1f,   0.15f, 0.05f, 0.3f,  0.12f, 0.04f,  0.06f, 0.35f, 0.08f,
                         0.02f,  0.01f, 0.25f, 0.5f,  0.125f, 0.0625f, 0.4f, 1.0f / 60.0f};
  for (float dt : steps) {
    for (int k = 0; k < 4; ++k) {
      Fx fx;
      if (k & 1) EXPECT(ex, fx.set("warn_g", 1.0f));
      if (k & 2) EXPECT(ex, fx.set("hold_s", 0.0f));
      fx.activate();
      char label[128];
      float t = 0.0f;
      for (int frame = 0; frame < 700; ++frame) {
        t += dt;
        // Exactly 1 g lateral (horiz = 1, hazard += dt * 40), alternating sides
        // with idle gaps, and exact vertical spikes.
        const int phase = (frame / 40) % 6;
        const float x = phase == 1 ? 1.0f : phase == 3 ? -1.0f : phase == 5 ? 0.5f : 0.0f;
        const float y = frame % 37 == 0 ? 0.5f : 0.0f;
        fx.update(dyn_g(x, y, phase == 4 ? -1.0f : 0.0f), dt);
        if (r.chance(0.01)) fx.reset();
        std::snprintf(label, sizeof(label), "edges dt %g run %d frame %d", double(dt), k, frame);
        if (!EXPECT(ex, fx.render_same(label))) break;
      }
    }
  }
  // The hazard clock hits 1000 exactly: 1 g at dt 0.25 adds 10 per update.
  Fx fx;
  fx.activate();
  int at_wrap = 0;
  for (int frame = 0; frame < 400; ++frame) {
    fx.update(dyn_g(1.0f, 0.0f, 0.0f), 0.25f);
    if (fx.c->hazard == 1000.0f) ++at_wrap;
    char label[64];
    std::snprintf(label, sizeof(label), "hazard wrap frame %d", frame);
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  EXPECT(ex, at_wrap >= 3);
}

TEST(hud_orbit_min) {
  Expect ex;
  diff::Rand r(1106);
  // orbit_min (minutes per step), simulated seconds, dt, minimum step count.
  const struct {
    float orbit_min;
    float seconds;
    float dt;
    int min_steps;
  } runs[] = {
      {0.0f, 300.0f, 1.0f / 60.0f, 0},     // off
      {-3.0f, 120.0f, 1.0f / 60.0f, 0},    // clamped to 0 = off
      {0.01f, 60.0f, 1.0f / 60.0f, 90},    // every 0.6 s
      {0.05f, 120.0f, 1.0f / 30.0f, 35},   // every 3 s
      {0.25f, 180.0f, 1.0f / 15.0f, 11},   // every 15 s at the engine's slowest step
      {1.0f, 400.0f, 1.0f / 60.0f, 6},     // every minute
      {2.0f, 600.0f, 1.0f / 60.0f, 4},     // default, 10 minutes at 60 fps
      {7.5f, 1900.0f, 1.0f / 15.0f, 4},    // half an hour
      {30.0f, 3700.0f, 1.0f / 15.0f, 2},   // max: an hour
      {45.0f, 1900.0f, 1.0f / 15.0f, 1},   // clamped to 30
  };
  for (const auto& run : runs) {
    Fx fx;
    EXPECT(ex, fx.set("orbit_min", run.orbit_min));
    fx.activate();
    gm::Rng rng(4000u);
    const gm::Scenario& sc = *gm::Scenario::find("drive");
    char label[160];
    float t = 0.0f;
    bool ok = true;
    for (int frame = 0; t < run.seconds && ok; ++frame) {
      t += run.dt;
      fx.update(sc.at(std::fmod(t, sc.duration() + 2.0f), &rng), run.dt);
      std::snprintf(label, sizeof(label), "orbit_min %g frame %d t=%.3f", double(run.orbit_min), frame, double(t));
      ok = EXPECT(ex, fx.render_same(label));
    }
    harness::note("orbit_min %g: %d steps in %g s", double(run.orbit_min), fx.orbit_changes, double(run.seconds));
    EXPECT(ex, fx.orbit_changes >= run.min_steps);
    if (run.min_steps == 0) EXPECT(ex, fx.orbit_changes == 0);
    EXPECT(ex, fx.orbit_order_ok);
  }
  // Irregular dt, with orbit_min changed mid-run (0 snaps the orbit home).
  for (int k = 0; k < 6; ++k) {
    Fx fx;
    EXPECT(ex, fx.set("orbit_min", 0.02f * static_cast<float>(k + 1)));
    fx.activate();
    gm::Rng rng(4100u + static_cast<uint32_t>(k));
    const gm::Scenario& sc = gm::Scenario::preset(k % gm::Scenario::presetCount());
    char label[160];
    float t = 0.0f;
    for (int frame = 0; frame < 6000; ++frame) {
      const int updates = r.chance(0.15) ? r.irange(0, 3) : 1;
      for (int u = 0; u < updates; ++u) {
        const float dt = odd_dt(r);
        t += dt;
        fx.update(sc.at(std::fmod(t, sc.duration() + 0.5f), &rng), dt);
      }
      if (frame % 1500 == 1000) EXPECT(ex, fx.set("orbit_min", 0.0f));
      if (frame % 1500 == 1100) EXPECT(ex, fx.set("orbit_min", r.frange(0.005f, 0.05f)));
      if (r.chance(0.003)) fx.reset();  // keeps the orbit
      if (r.chance(0.003)) {
        fx.deactivate();
        fx.activate();
      }
      std::snprintf(label, sizeof(label), "orbit irregular run %d frame %d", k, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    EXPECT(ex, fx.orbit_changes >= 8);
  }
}

TEST(hud_orbit_survives_reset) {
  Expect ex;
  Fx fx;
  EXPECT(ex, fx.set("orbit_min", 0.01f));
  fx.activate();
  const gm::Scenario& sc = *gm::Scenario::find("slalom");
  gm::Rng rng(4200u);
  char label[128];
  float t = 0.0f;
  int checked = 0;
  for (int frame = 0; frame < 3000; ++frame) {
    t += 1.0f / 60.0f;
    fx.update(sc.at(std::fmod(t, sc.duration()), &rng), 1.0f / 60.0f);
    if (frame % 97 == 50) {
      const int before = fx.c->orbit_step;
      const float timer = fx.c->orbit_timer;
      switch ((frame / 97) % 3) {
        case 0: fx.reset(); break;
        case 1: fx.activate(); break;
        default: fx.deactivate(); fx.activate(); break;
      }
      EXPECT(ex, fx.c->orbit_step == before && fx.c->orbit_timer == timer);
      ++checked;
    }
    std::snprintf(label, sizeof(label), "orbit across resets frame %d", frame);
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  EXPECT(ex, checked > 20);
  EXPECT(ex, fx.orbit_changes >= 70);
  EXPECT(ex, fx.orbit_order_ok);
}

TEST(hud_set_orbit_step) {
  Expect ex;
  const int steps[] = {0, 1, 2, 3, 4, 5, 7, 100, -1, -2, -3, -4, -5, INT_MAX, INT_MIN};
  for (int s : steps) {
    Fx fx;
    fx.set_orbit_step(s);
    EXPECT(ex, fx.c->orbit_step == (s & 3) && fx.c->orbit_timer == 0.0f);
    char label[96];
    std::snprintf(label, sizeof(label), "set_orbit_step %d", s);
    EXPECT(ex, fx.same_state(label));
    int dx = -1, dy = -1;
    gm_effect_burn_in_offset(fx.ce(), &dx, &dy);
    const int kx[4] = {0, 1, 1, 0}, ky[4] = {0, 0, 1, 1};
    EXPECT(ex, dx == kx[s & 3] && dy == ky[s & 3]);
    // Activating keeps the chosen start (the firmware picks one at power-on, then activates).
    fx.activate();
    EXPECT(ex, fx.c->orbit_step == (s & 3));
    EXPECT(ex, fx.render_same(label));
  }
  // Mid-run: the timer restarts, so both step at the same frame afterwards.
  diff::Rand r(1107);
  for (int k = 0; k < 8; ++k) {
    Fx fx;
    EXPECT(ex, fx.set("orbit_min", 0.02f + 0.01f * static_cast<float>(k)));
    fx.activate();
    gm::Rng rng(4300u + static_cast<uint32_t>(k));
    const gm::Scenario& sc = gm::Scenario::preset(k % gm::Scenario::presetCount());
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 4000; ++frame) {
      const float dt = k % 2 ? odd_dt(r) : 1.0f / 60.0f;
      t += dt;
      fx.update(sc.at(std::fmod(t, sc.duration() + 0.5f), &rng), dt);
      if (r.chance(0.01)) fx.set_orbit_step(r.irange(-8, 8));
      std::snprintf(label, sizeof(label), "set_orbit_step run %d frame %d", k, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
  // With orbit_min = 0 the next update snaps the orbit back to step 0.
  Fx off;
  EXPECT(ex, off.set("orbit_min", 0.0f));
  off.activate();
  off.set_orbit_step(2);
  EXPECT(ex, off.render_same("orbit off, step 2 before update"));
  EXPECT(ex, gm_hud_orbit_x(off.c.get()) == 1 && gm_hud_orbit_y(off.c.get()) == 1);
  off.update(gm::GForce{}, 1.0f / 60.0f);
  EXPECT(ex, off.render_same("orbit off, after update"));
  EXPECT(ex, gm_hud_orbit_x(off.c.get()) == 0 && gm_hud_orbit_y(off.c.get()) == 0);
}

TEST(hud_nan_inf_g) {
  Expect ex;
  diff::Rand r(1108);
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, 1e18f, -1e-30f, 2e9f, -2e9f};
  int warn = 0;
  for (int run = 0; run < 40; ++run) {
    Fx fx;
    if (run % 4 == 1) EXPECT(ex, fx.set("inertia", 0.0f));
    if (run % 4 == 2) EXPECT(ex, fx.set("orbit_min", 0.01f));
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
      // The ball and the tip never leave the volume.
      const gm_vec3_t b = gm_hud_ball_position(fx.c.get());
      if (!EXPECT(ex, std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z))) stop = true;
    }
    warn += fx.warn;
  }
  CHECK(warn > 100);  // coverage: the hazard stripes ran with a non-finite or huge hazard clock
}

TEST(hud_long_run) {
  Expect ex;
  diff::Rand r(1109);
  // 10 minutes at 60 fps (the blink clock wraps every 60 s) ...
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps", false);
    EXPECT(ex, frames >= 600 * 60);
    EXPECT(ex, fx.orbit_changes >= 4 && fx.orbit_order_ok);
  }
  // ... and an hour and a half at the engine's slowest step: 45 orbit steps.
  {
    Fx fx;
    fx.activate();
    gm::Rng rng(7001u);
    const gm::Scenario& sc = *gm::Scenario::find("drive");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 5400 * 15; ++frame) {
      t += 1.0f / 15.0f;
      fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f);
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    harness::note("90 min at 15 fps: %d orbit steps", fx.orbit_changes);
    EXPECT(ex, fx.orbit_changes >= 44 && fx.orbit_order_ok);
  }
}

TEST(hud_fuzz) {
  Expect ex;
  diff::Rand r(1110);
  gm::HudEffect proto;
  for (int run = 0; run < 120; ++run) {
    Fx fx;
    for (int i = 0; i < proto.params().count(); ++i) {
      const gm::Param& prm = proto.params().at(i);
      if (r.chance(0.6)) EXPECT(ex, fx.set(prm.name, r.frange(prm.min, prm.max)));
    }
    if (r.chance(0.5)) EXPECT(ex, fx.set("orbit_min", r.frange(0.0f, 0.03f)));
    if (r.chance(0.3)) fx.set_orbit_step(r.irange(0, 3));
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
      if (r.chance(0.005)) fx.set_orbit_step(r.irange(-4, 4));
      std::snprintf(label, sizeof(label), "fuzz run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}
