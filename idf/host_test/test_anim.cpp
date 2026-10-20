// Differential tests of the "anim" effect (gm_anim.c vs gm::AnimationEffect):
// the demo asset (every field and every bit), the empty asset, the parameter
// table, every scenario preset at 60 fps and with irregular dt on several
// assets (the demo, the real Studio export compiled through its C branch, a
// single-slice asset with echo and scrubbing, art larger than the volume),
// every tunable at its min, max and a middle value, every scrub axis, forced
// states, clip resolution with empty clips (mirror and idle fallbacks),
// set_asset with every apply_mapping / reset_motion combination,
// reset/activate/deactivate cycles mid-run, NaN/inf g, long runs and random
// fuzzing. After every update the state, frame, mirror flag and position must
// match bit for bit, and every update batch is followed by a render into
// cleared frames that must match byte for byte.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "diff_util.hpp"

// The Studio export the C++ core is tested with (tests/fixtures), as C++.
namespace cpp_export {
#include "fixtures/studio_export.hpp"
}  // namespace cpp_export

// The same export in the current Studio format (fixtures/user_animation.h
// here), which compiles as C (the firmware's main/user_animation.h) or as C++.
// Its C branch declares the asset as a gm_anim_asset_t. Inside this namespace
// gm::AnimationAsset names that C struct, so the C effect below plays exactly
// the positional initialiser the firmware compiles.
namespace c_export {
namespace gm {
typedef ::gm_anim_asset_t AnimationAsset;
}  // namespace gm
#include "fixtures/user_animation.h"
}  // namespace c_export

using diff::Expect;

namespace {

#if defined(__SANITIZE_ADDRESS__)
// The C++ adds g-scrubbed frames to the clip position without an overflow
// check. With a NaN clip time (after NaN or absurd g) that sum overflows,
// which is undefined; an x86 build wraps around (and so does the C), but the
// sanitizer build of the C++ aborts. Such runs are skipped there.
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

// ---- assets -------------------------------------------------------------------------

gm_anim_clip_t to_c(const gm::AnimClip& c) {
  gm_anim_clip_t r;
  r.start = c.start;
  r.count = c.count;
  r.fps = c.fps;
  r.loop = c.loop;
  return r;
}

// Field by field (the positional order is checked by the export test).
gm_anim_asset_t to_c(const gm::AnimationAsset& a) {
  gm_anim_asset_t r;
  r.name = a.name;
  r.width = a.width;
  r.height = a.height;
  r.depth = a.depth;
  r.frames = a.frames;
  r.bits = a.bits;
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) r.clips[s] = to_c(a.clips[s]);
  const gm::AnimMapping& m = a.mapping;
  r.mapping.px_per_gx = m.pxPerGx;
  r.mapping.px_per_gy = m.pxPerGy;
  r.mapping.px_per_gz = m.pxPerGz;
  r.mapping.spring_hz = m.springHz;
  r.mapping.inertia = m.inertia;
  r.mapping.enter_g = m.enterG;
  r.mapping.hold_s = m.holdS;
  r.mapping.bump_g = m.bumpG;
  r.mapping.fps_per_g = m.fpsPerG;
  r.mapping.scrub_axis = m.scrubAxis;
  r.mapping.frames_per_g = m.framesPerG;
  r.mapping.echo = m.echo;
  r.mapping.mirror = m.mirror;
  return r;
}

bool same_clip(const gm_anim_clip_t& c, const gm::AnimClip& p) {
  return c.start == p.start && c.count == p.count && diff::same(c.fps, p.fps) && c.loop == p.loop;
}

bool same_mapping(const gm_anim_mapping_t& c, const gm::AnimMapping& p) {
  return diff::same(c.px_per_gx, p.pxPerGx) && diff::same(c.px_per_gy, p.pxPerGy) &&
         diff::same(c.px_per_gz, p.pxPerGz) && diff::same(c.spring_hz, p.springHz) &&
         diff::same(c.inertia, p.inertia) && diff::same(c.enter_g, p.enterG) && diff::same(c.hold_s, p.holdS) &&
         diff::same(c.bump_g, p.bumpG) && diff::same(c.fps_per_g, p.fpsPerG) &&
         diff::same(c.scrub_axis, p.scrubAxis) && diff::same(c.frames_per_g, p.framesPerG) &&
         diff::same(c.echo, p.echo) && diff::same(c.mirror, p.mirror);
}

// Every field; the artwork is compared byte for byte (not by pointer).
bool same_asset(const gm_anim_asset_t& c, const gm::AnimationAsset& p) {
  bool ok = c.name && p.name && std::strcmp(c.name, p.name) == 0;
  ok = ok && c.width == p.width && c.height == p.height && c.depth == p.depth && c.frames == p.frames;
  ok = ok && (c.bits == nullptr) == (p.bits == nullptr);
  if (ok && c.bits && p.totalBytes() > 0) ok = std::memcmp(c.bits, p.bits, static_cast<size_t>(p.totalBytes())) == 0;
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) ok = ok && same_clip(c.clips[s], p.clips[s]);
  ok = ok && same_mapping(c.mapping, p.mapping);
  return ok;
}

// Artwork owned by the test, with matching C and C++ asset descriptions.
struct Art {
  std::vector<uint8_t> bits;
  gm::AnimationAsset p;
  gm_anim_asset_t c;
  // Sizes the artwork for p's dimensions and fills it at random (density 0..1).
  void fill(diff::Rand& r, float density) {
    bits.assign(static_cast<size_t>(p.totalBytes() > 0 ? p.totalBytes() : 0), 0);
    for (uint8_t& b : bits)
      for (int k = 0; k < 8; ++k)
        if (r.frange(0.0f, 1.0f) < density) b = static_cast<uint8_t>(b | (1u << k));
    finish();
  }
  void finish(bool null_bits = false) {
    p.bits = null_bits || bits.empty() ? nullptr : bits.data();
    c = to_c(p);
  }
};

// A single-slice sprite with every feature of the mapping on: echo, scrub on
// the longitudinal axis, speed-up with g. The right clip is empty (mirrored
// from left) and the brake clip plays once.
std::unique_ptr<Art> make_flat() {
  std::unique_ptr<Art> a(new Art);
  diff::Rand r(77);
  a->p.name = "flat";
  a->p.width = 24;
  a->p.height = 16;
  a->p.depth = 1;
  a->p.frames = 20;
  a->p.clips[gm::kAnimIdle] = gm::AnimClip{0, 4, 6.0f, 1};
  a->p.clips[gm::kAnimAccelerate] = gm::AnimClip{4, 4, 15.0f, 1};
  a->p.clips[gm::kAnimBrake] = gm::AnimClip{8, 4, 10.0f, 0};
  a->p.clips[gm::kAnimLeft] = gm::AnimClip{12, 4, 9.0f, 1};
  a->p.clips[gm::kAnimRight] = gm::AnimClip{0, 0, 12.0f, 1};
  a->p.clips[gm::kAnimBump] = gm::AnimClip{16, 4, 24.0f, 0};
  a->p.mapping.echo = 2.0f;
  a->p.mapping.scrubAxis = 2.0f;
  a->p.mapping.framesPerG = 6.0f;
  a->p.mapping.fpsPerG = 1.5f;
  a->p.mapping.holdS = 0.2f;
  a->fill(r, 0.3f);
  return a;
}

// Art larger than the volume in every direction (more slices than panels).
std::unique_ptr<Art> make_big() {
  std::unique_ptr<Art> a(new Art);
  diff::Rand r(78);
  a->p.name = "big";
  a->p.width = static_cast<uint16_t>(GM_W + 20);
  a->p.height = static_cast<uint16_t>(GM_H + 12);
  a->p.depth = static_cast<uint16_t>(GM_D + 2);
  a->p.frames = 4;
  a->p.clips[gm::kAnimIdle] = gm::AnimClip{0, 2, 5.0f, 1};
  a->p.clips[gm::kAnimBrake] = gm::AnimClip{2, 2, 7.0f, 1};
  a->fill(r, 0.05f);
  return a;
}

// A tiny two-frame asset whose clips run past the end.
std::unique_ptr<Art> make_tiny() {
  std::unique_ptr<Art> a(new Art);
  a->p.name = "tiny";
  a->p.width = 8;
  a->p.height = 8;
  a->p.depth = 1;
  a->p.frames = 2;
  a->p.clips[gm::kAnimBrake] = gm::AnimClip{1, 50, 10.0f, 1};
  a->p.clips[gm::kAnimBump] = gm::AnimClip{2, 4, 10.0f, 0};  // starts one past the last frame
  a->bits = {0xFF, 0, 0, 0, 0, 0, 0, 0x18, 0x81, 0, 0, 0, 0, 0, 0, 0x7E};
  a->finish();
  return a;
}

// Very fast clips, so the clip timer reaches its 1e5-frame bound often.
std::unique_ptr<Art> make_fast() {
  std::unique_ptr<Art> a(new Art);
  diff::Rand r(79);
  a->p.name = "fast";
  a->p.width = 12;
  a->p.height = 10;
  a->p.depth = 2;
  a->p.frames = 37;
  a->p.clips[gm::kAnimIdle] = gm::AnimClip{0, 7, 31500.0f, 1};
  a->p.clips[gm::kAnimAccelerate] = gm::AnimClip{7, 11, 52100.0f, 1};
  a->p.clips[gm::kAnimBrake] = gm::AnimClip{18, 9, 19777.0f, 1};
  a->p.clips[gm::kAnimRight] = gm::AnimClip{27, 10, 43333.0f, 1};
  a->p.clips[gm::kAnimBump] = gm::AnimClip{30, 7, 60.0f, 0};
  a->p.mapping.fpsPerG = 8.0f;
  a->fill(r, 0.4f);
  return a;
}

struct Pair {
  const gm::AnimationAsset* p;
  const gm_anim_asset_t* c;
  const char* tag;
};

Pair demo_pair() { return Pair{&gm::demoAnimation(), gm_anim_demo(), "demo"}; }
Pair export_pair() { return Pair{&cpp_export::kUserAnimation, &c_export::kUserAnimation, "export"}; }
Pair art_pair(const Art& a) { return Pair{&a.p, &a.c, a.p.name}; }

// ---- the effect pair -----------------------------------------------------------------

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_anim_t> c{new gm_anim_t};
  std::unique_ptr<gm::AnimationEffect> p;
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters (rendered frames).
  int seen[GM_ANIM_STATE_COUNT] = {};
  int mirrored = 0;
  int lit = 0;
  int timer_bounded = 0;  // updates where the clip timer was wrapped (C side)

  explicit Fx(const Pair& a) : p(new gm::AnimationEffect(*a.p)) { gm_anim_init(c.get(), a.c); }
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
  void force(int state) {
    gm_anim_force_state(c.get(), state);
    p->forceState(state);
  }
  void set_asset(const Pair& a, bool apply_mapping, bool reset_motion) {
    gm_anim_set_asset(c.get(), a.c, apply_mapping, reset_motion);
    p->setAsset(*a.p, apply_mapping, reset_motion);
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
  bool same_state() const {
    return gm_anim_state(c.get()) == p->state() && gm_anim_current_frame(c.get()) == p->currentFrame() &&
           gm_anim_mirrored(c.get()) == p->mirrored() && diff::same(gm_anim_position(c.get()), p->position());
  }
  void note_state(const char* label) const {
    const gm_vec3_t a = gm_anim_position(c.get());
    const gm::Vec3 b = p->position();
    harness::note("%s: C state %d frame %d mirror %d pos (%.9g, %.9g, %.9g); C++ state %d frame %d mirror %d pos "
                  "(%.9g, %.9g, %.9g)",
                  label, gm_anim_state(c.get()), gm_anim_current_frame(c.get()), gm_anim_mirrored(c.get()) ? 1 : 0,
                  static_cast<double>(a.x), static_cast<double>(a.y), static_cast<double>(a.z), p->state(),
                  p->currentFrame(), p->mirrored() ? 1 : 0, static_cast<double>(b.x), static_cast<double>(b.y),
                  static_cast<double>(b.z));
  }
  // The resolved clip of every state (and of out-of-range states) agrees.
  bool same_clips() const {
    for (int s = -3; s <= GM_ANIM_STATE_COUNT + 2; ++s) {
      bool mc = true, mp = false;
      const gm_anim_clip_t a = gm_anim_resolve_clip(c.get(), s, &mc);
      const gm::AnimClip b = p->resolveClip(s, mp);
      if (!same_clip(a, b) || mc != mp) return false;
    }
    return true;
  }
  // Advances both; true when state, frame, mirror flag and position match afterwards.
  bool update(const gm::GForce& g, float dt) {
    pctx.g = g;
    pctx.dt = dt;
    pctx.time += dt;
    ++pctx.frame;
    cctx.g = gm_gforce_from_proper(diff::to_c(g.proper));
    cctx.dt = dt;
    cctx.time = pctx.time;
    cctx.frame = pctx.frame;
    const float before = c->clip_time;
    gm_effect_update(ce(), &cctx);
    p->update(pctx);
    // The timer only goes back without a state change (state_time > 0) when it is wrapped.
    if (c->clip_time < before && c->state_time > 0.0f) ++timer_bounded;
    return same_state();
  }
  // Renders both into cleared frames; true when the frames (and the state) match.
  bool render_same(const char* label) {
    gm_frame_clear(f.c.get());
    f.p->clear();
    gm_effect_render(ce(), f.c.get());
    p->render(*f.p);
    bool ok = f.same(label);
    if (!same_state()) {
      note_state(label);
      ok = false;
    }
    const int s = gm_anim_state(c.get());
    if (s >= 0 && s < GM_ANIM_STATE_COUNT) ++seen[s];
    if (gm_anim_mirrored(c.get())) ++mirrored;
    if (gm_frame_count_lit(f.c.get()) > 0) ++lit;
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
        !harness::same_float(a->max, b.max) || !diff::same(*a->value, *b.value) || std::strcmp(a->help, b.help) != 0) {
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

// Mid-run events for play().
enum : unsigned {
  kNoEvents = 0,
  kCycles = 1,  // resets and deactivate/activate cycles (some at fixed times, some at random)
  kForce = 2,   // forced states (also out of range) and back to g-driven
  kSwap = 4,    // set_asset to one of `swaps`, every apply_mapping / reset_motion combination
};

// Plays preset `sc` (looped) for `seconds` into both effects, comparing the
// state after every update and rendering and comparing after every update
// batch. Stops at the first mismatch. Returns the rendered frame count.
int play(Expect& ex, Fx& fx, const gm::Scenario& sc, gm::Rng& rng, diff::Rand& r, Dt mode, float seconds,
         const char* tag, unsigned events = kNoEvents, const std::vector<Pair>* swaps = nullptr) {
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
    std::snprintf(label, sizeof(label), "%s %s frame %d t=%.4f", tag, sc.name(), frames, static_cast<double>(t));
    if (!EXPECT(ex, ok)) {
      harness::note("%s: state differs after an update", label);
      fx.note_state(label);
      break;
    }
    if (events & kCycles) {
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
    if ((events & kForce) && r.chance(0.02)) fx.force(r.chance(0.4) ? -1 : r.irange(-2, GM_ANIM_STATE_COUNT + 1));
    if ((events & kSwap) && swaps && !swaps->empty() && r.chance(0.006)) {
      const Pair& a = (*swaps)[static_cast<size_t>(r.irange(0, static_cast<int>(swaps->size()) - 1))];
      fx.set_asset(a, r.chance(0.5), r.chance(0.5));
    }
    if (events && !EXPECT(ex, fx.same_state() && fx.same_clips())) {
      harness::note("%s: state differs after an event", label);
      fx.note_state(label);
      break;
    }
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

// Hard driving for `frames` frames at 60 fps: pinned against every wall in
// turn, quick direction changes and bumps.
void hard(Expect& ex, Fx& fx, int frames, const char* tag) {
  char label[160];
  float t = 0.0f;
  for (int frame = 0; frame < frames; ++frame) {
    t += 1.0f / 60.0f;
    const float s = std::sin(t * 1.7f), q = std::cos(t * 0.9f);
    const float bump = frame % 97 < 3 ? 0.9f : 0.0f;
    if (!EXPECT(ex, fx.update(dyn_g(2.5f * s, 1.2f * s * q + bump, -2.0f * q), 1.0f / 60.0f))) {
      fx.note_state(tag);
      break;
    }
    std::snprintf(label, sizeof(label), "%s hard frame %d", tag, frame);
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
}

}  // namespace

TEST(anim_demo_asset) {
  Expect ex;
  const gm::AnimationAsset& pd = gm::demoAnimation();
  const gm_anim_asset_t* cd = gm_anim_demo();
  EXPECT(ex, cd != nullptr);
  EXPECT(ex, gm_anim_demo() == cd);              // built once
  EXPECT(ex, cd->bits && cd->bits != pd.bits);  // its own artwork ...
  EXPECT(ex, same_asset(*cd, pd));              // ... equal to the C++ demo in every field and every bit
  EXPECT(ex, std::strcmp(cd->name, "demo") == 0);
  EXPECT(ex, gm_anim_asset_valid(cd) && pd.valid());
  EXPECT(ex, gm_anim_asset_row_bytes(cd) == pd.rowBytes() && gm_anim_asset_slice_bytes(cd) == pd.sliceBytes());
  EXPECT(ex, gm_anim_asset_frame_bytes(cd) == pd.frameBytes() && gm_anim_asset_total_bytes(cd) == pd.totalBytes());
  EXPECT(ex, gm_anim_asset_total_bytes(cd) == 32 * 3 * 24 * 4);
  int lit = 0, diffs = 0;
  for (int fr = -1; fr <= pd.frames; ++fr)
    for (int s = -1; s <= pd.depth; ++s)
      for (int y = -2; y < pd.height + 2; ++y)
        for (int x = -2; x < pd.width + 2; ++x) {
          const bool a = gm_anim_asset_pixel(cd, fr, s, x, y), b = pd.pixel(fr, s, x, y);
          diffs += a != b;
          lit += a;
        }
  EXPECT(ex, diffs == 0);
  harness::note("demo: %d lit pixels", lit);
  EXPECT(ex, lit > 1000);
  // Calling again changes nothing.
  EXPECT(ex, same_asset(*gm_anim_demo(), gm::demoAnimation()));

  // The empty asset is the default gm::AnimationAsset.
  const gm_anim_asset_t e = gm_anim_asset_empty();
  const gm::AnimationAsset pe;
  EXPECT(ex, same_asset(e, pe));
  EXPECT(ex, e.name && e.name[0] == '\0' && e.bits == nullptr && e.depth == 1 && e.frames == 0);
  EXPECT(ex, !gm_anim_asset_valid(&e) && !pe.valid());
  EXPECT(ex, !gm_anim_asset_pixel(&e, 0, 0, 0, 0));
  const gm_anim_mapping_t md = GM_ANIM_MAPPING_DEFAULT;
  EXPECT(ex, same_mapping(md, gm::AnimMapping{}));
  const gm_anim_clip_t cl = GM_ANIM_CLIP_DEFAULT;
  EXPECT(ex, same_clip(cl, gm::AnimClip{}));

  // State names, also out of range.
  const int c_states[] = {GM_ANIM_IDLE, GM_ANIM_ACCELERATE, GM_ANIM_BRAKE, GM_ANIM_LEFT, GM_ANIM_RIGHT, GM_ANIM_BUMP,
                          GM_ANIM_STATE_COUNT};
  const int p_states[] = {gm::kAnimIdle, gm::kAnimAccelerate, gm::kAnimBrake,     gm::kAnimLeft,
                          gm::kAnimRight, gm::kAnimBump,      gm::kAnimStateCount};
  for (int i = 0; i < 7; ++i) EXPECT(ex, c_states[i] == p_states[i]);
  for (int s = -3; s <= GM_ANIM_STATE_COUNT + 3; ++s)
    EXPECT(ex, std::strcmp(gm_anim_state_name(s), gm::animStateName(s)) == 0);

  // The inline asset helpers on random artwork.
  diff::Rand r(3101);
  for (int run = 0; run < 60; ++run) {
    Art a;
    a.p.name = "rnd";
    a.p.width = static_cast<uint16_t>(r.irange(1, 40));
    a.p.height = static_cast<uint16_t>(r.irange(1, 20));
    a.p.depth = static_cast<uint16_t>(r.irange(1, 4));
    a.p.frames = static_cast<uint16_t>(r.irange(1, 5));
    a.fill(r, r.frange(0.0f, 1.0f));
    EXPECT(ex, gm_anim_asset_total_bytes(&a.c) == a.p.totalBytes());
    EXPECT(ex, gm_anim_asset_valid(&a.c) == a.p.valid());
    int d = 0;
    for (int fr = -1; fr <= a.p.frames; ++fr)
      for (int s = -1; s <= a.p.depth; ++s)
        for (int y = -1; y <= a.p.height; ++y)
          for (int x = -1; x <= a.p.width + 8; ++x)
            d += gm_anim_asset_pixel(&a.c, fr, s, x, y) != a.p.pixel(fr, s, x, y);
    EXPECT(ex, d == 0);
    a.finish(true);  // no artwork
    EXPECT(ex, !gm_anim_asset_valid(&a.c) && !a.p.valid() && !gm_anim_asset_pixel(&a.c, 0, 0, 0, 0));
  }
}

TEST(anim_params) {
  Expect ex;
  std::unique_ptr<Art> flat = make_flat(), tiny = make_tiny();
  Art empty;
  empty.finish();
  const Pair pairs[] = {demo_pair(), export_pair(), art_pair(*flat), art_pair(*tiny), art_pair(empty)};
  for (const Pair& pr : pairs) {
    Fx fx(pr);
    EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
    EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "anim") == 0);
    EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
    EXPECT(ex, same_asset(*gm_anim_asset(fx.c.get()), fx.p->asset()));
    EXPECT(ex, same_mapping(fx.c->m, pr.p->mapping));
    EXPECT(ex, fx.c->forced == -1 && fx.c->state == GM_ANIM_IDLE && fx.c->state_time == 0.0f &&
                   fx.c->clip_time == 0.0f && fx.c->frame == 0 && !fx.c->mirror && fx.c->prev_vert == 0.0f);
    EXPECT(ex, fx.same_state() && fx.same_clips());
    // The vtable: activate falls back to reset, no burn-in offset.
    EXPECT(ex, fx.ce()->vt->activate == nullptr && fx.ce()->vt->deactivate == nullptr);
    EXPECT(ex, fx.ce()->vt->reset != nullptr && fx.ce()->vt->burn_in_offset == nullptr);
    int dxc = 5, dyc = 5, dxp = 5, dyp = 5;
    gm_effect_burn_in_offset(fx.ce(), &dxc, &dyc);
    fx.p->burnInOffset(dxp, dyp);
    EXPECT(ex, dxc == 0 && dyc == 0 && dxp == 0 && dyp == 0);
    EXPECT(ex, fx.render_same(pr.tag));
  }
  // Each name points at the field the header documents.
  Fx fx(demo_pair());
  gm_anim_mapping_t* m = &fx.c->m;
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"px_g_x", &m->px_per_gx}, {"px_g_y", &m->px_per_gy},     {"px_g_z", &m->px_per_gz},
                {"hz", &m->spring_hz},     {"inertia", &m->inertia},      {"enter_g", &m->enter_g},
                {"hold_s", &m->hold_s},    {"bump_g", &m->bump_g},        {"fps_g", &m->fps_per_g},
                {"scrub_axis", &m->scrub_axis}, {"frames_g", &m->frames_per_g}, {"echo", &m->echo},
                {"mirror", &m->mirror}};
  const int nfields = static_cast<int>(sizeof(fields) / sizeof(fields[0]));
  EXPECT(ex, gm_params_count(gm_effect_params(fx.ce())) == nfields);
  for (int i = 0; i < nfields; ++i) {
    const gm_param_t* prm = gm_params_find(gm_effect_params(fx.ce()), fields[i].name);
    EXPECT(ex, prm && prm->value == fields[i].field);
    EXPECT(ex, prm == gm_params_at(gm_effect_params(fx.ce()), i));
  }
  // Setting through the table: in range, out of range (clamped), non-finite (refused), unknown names.
  const float probes[] = {-1e9f, -65.0f, -1.0f, -0.5f, 0.0f, 0.01f, 0.5f, 1.0f, 2.0f, 2.5f, 8.0f, 64.0f, 121.0f,
                          1e9f,  diff::kInf, -diff::kInf, diff::kNaN};
  for (int i = 0; i < fx.p->params().count(); ++i)
    for (float v : probes) EXPECT(ex, fx.set(fx.p->params().at(i).name, v));
  EXPECT(ex, fx.set("nope", 1.0f));
  EXPECT(ex, !gm_params_set(gm_effect_params(fx.ce()), "nope", 1.0f));
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  // The table edits the effect's tunables, not the asset's mapping.
  EXPECT(ex, same_asset(*gm_anim_asset(fx.c.get()), fx.p->asset()));
  EXPECT(ex, same_mapping(gm_anim_asset(fx.c.get())->mapping, gm::demoAnimation().mapping));
}

TEST(anim_studio_export) {
  Expect ex;
  // The dual C/C++ copy holds exactly the C++ export: same artwork bytes, same
  // fields in the same positional order.
  static_assert(sizeof(c_export::kUserAnimationBits) == sizeof(cpp_export::kUserAnimationBits), "bits size");
  EXPECT(ex, std::memcmp(c_export::kUserAnimationBits, cpp_export::kUserAnimationBits,
                         sizeof(cpp_export::kUserAnimationBits)) == 0);
  const gm_anim_asset_t& ca = c_export::kUserAnimation;
  const gm::AnimationAsset& pa = cpp_export::kUserAnimation;
  EXPECT(ex, ca.bits == c_export::kUserAnimationBits);
  EXPECT(ex, same_asset(ca, pa));
  EXPECT(ex, std::strcmp(ca.name, "my_animation") == 0 && ca.width == 32 && ca.height == 24 && ca.depth == 3 &&
                 ca.frames == 33);
  EXPECT(ex, gm_anim_asset_valid(&ca));
  EXPECT(ex, gm_anim_asset_total_bytes(&ca) == static_cast<int>(sizeof(c_export::kUserAnimationBits)));
  EXPECT(ex, ca.clips[GM_ANIM_BUMP].start == 26 && ca.clips[GM_ANIM_BUMP].count == 6 &&
                 ca.clips[GM_ANIM_BUMP].fps == 18.0f && ca.clips[GM_ANIM_BUMP].loop == 0);
  EXPECT(ex, ca.mapping.enter_g == 0.25f && ca.mapping.scrub_axis == -1.0f && ca.mapping.mirror == 1.0f);

  // Play it in both, every preset, 60 fps and irregular dt.
  diff::Rand r(3102);
  Fx probe(export_pair());
  EXPECT(ex, same_param_tables(gm_effect_params(probe.ce()), probe.p->params()));
  float v = 0.0f;
  EXPECT(ex, gm_params_get(gm_effect_params(probe.ce()), "enter_g", &v) && v == 0.25f);
  int seen[GM_ANIM_STATE_COUNT] = {}, mirrored = 0;
  for (int i = 0; i < gm::Scenario::presetCount(); ++i)
    for (int mode = 0; mode < 2; ++mode) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx(export_pair());
      fx.activate();
      gm::Rng rng(3100u + static_cast<uint32_t>(i * 2 + mode));
      const int frames = play(ex, fx, sc, rng, r, mode ? Dt::kIrregular : Dt::k60, 22.0f, "export");
      if (!mode) EXPECT(ex, frames >= 22 * 60);
      for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) seen[s] += fx.seen[s];
      mirrored += fx.mirrored;
    }
  harness::note("export: frames per state %d %d %d %d %d %d, mirrored %d", seen[0], seen[1], seen[2], seen[3], seen[4],
                seen[5], mirrored);
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) CHECK(seen[s] > 20);
  CHECK(mirrored > 20);
}

TEST(anim_presets_60fps) {
  Expect ex;
  diff::Rand r(3103);
  std::unique_ptr<Art> flat = make_flat();
  const Pair pairs[] = {demo_pair(), art_pair(*flat)};
  int seen[GM_ANIM_STATE_COUNT] = {}, mirrored = 0;
  for (const Pair& pr : pairs)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      Fx fx(pr);
      fx.activate();
      gm::Rng rng(1000u + static_cast<uint32_t>(i));
      const int frames = play(ex, fx, sc, rng, r, Dt::k60, 22.0f, pr.tag);
      EXPECT(ex, frames >= 22 * 60);
      EXPECT(ex, fx.lit == frames);  // something is always on the glass
      for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) seen[s] += fx.seen[s];
      mirrored += fx.mirrored;
    }
  harness::note("frames per state %d %d %d %d %d %d, mirrored %d", seen[0], seen[1], seen[2], seen[3], seen[4],
                seen[5], mirrored);
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) CHECK(seen[s] > 50);
  CHECK(mirrored > 200);
}

TEST(anim_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(3104);
  std::unique_ptr<Art> flat = make_flat(), big = make_big();
  const Pair pairs[] = {demo_pair(), art_pair(*flat), art_pair(*big)};
  for (int rep = 0; rep < 2; ++rep)
    for (const Pair& pr : pairs)
      for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
        const gm::Scenario& sc = gm::Scenario::preset(i);
        Fx fx(pr);
        if (rep) {  // livelier: no hold time, quick spring, playback speed-up
          EXPECT(ex, fx.set("hold_s", 0.0f));
          EXPECT(ex, fx.set("hz", 7.0f));
          EXPECT(ex, fx.set("fps_g", 4.0f));
        }
        fx.activate();
        gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep));
        play(ex, fx, sc, rng, r, Dt::kIrregular, pr.p == &big->p ? 20.0f : 25.0f, pr.tag);
      }
}

TEST(anim_events) {
  Expect ex;
  diff::Rand r(3105);
  std::unique_ptr<Art> flat = make_flat(), big = make_big(), tiny = make_tiny(), fast = make_fast();
  Art empty, nobits;
  empty.finish();
  nobits.p = flat->p;
  nobits.p.name = "nobits";
  nobits.finish(true);
  const std::vector<Pair> swaps = {demo_pair(),      export_pair(),     art_pair(*flat),  art_pair(*big),
                                   art_pair(*tiny),  art_pair(*fast),   art_pair(empty), art_pair(nobits)};
  for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
    const gm::Scenario& sc = gm::Scenario::preset(i);
    for (int mode = 0; mode < 2; ++mode) {
      for (const Pair& start : {demo_pair(), art_pair(*flat)}) {
        Fx fx(start);
        fx.activate();
        gm::Rng rng(2500u + static_cast<uint32_t>(i * 3 + mode));
        play(ex, fx, sc, rng, r, mode ? Dt::kIrregular : Dt::k60, 24.0f, mode ? "events irregular" : "events 60fps",
             kCycles | kForce | kSwap, &swaps);
      }
    }
  }
  // Never activated, and deactivated before the first update.
  for (int k = 0; k < 2; ++k) {
    Fx fx(demo_pair());
    if (k) fx.deactivate();
    gm::Rng rng(2600u + static_cast<uint32_t>(k));
    play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 21.0f, k ? "deactivated" : "never activated");
  }
}

TEST(anim_tunables) {
  Expect ex;
  diff::Rand r(3106);
  std::unique_ptr<Art> flat = make_flat();
  gm::AnimationEffect proto(gm::demoAnimation());
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
  for (const Pair& pr : {demo_pair(), art_pair(*flat)})
    for (int i = 0; i < proto.params().count(); ++i) {
      const gm::Param& prm = proto.params().at(i);
      const float values[] = {prm.min, prm.max, 0.5f * (prm.min + prm.max)};
      for (float v : values) {
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s %s=%g", pr.tag, prm.name, static_cast<double>(v));
        Fx fx(pr);
        EXPECT(ex, fx.set(prm.name, v));
        // Scrubbing needs both an axis and a rate.
        if (std::strcmp(prm.name, "scrub_axis") == 0) EXPECT(ex, fx.set("frames_g", 12.0f));
        if (std::strcmp(prm.name, "frames_g") == 0) EXPECT(ex, fx.set("scrub_axis", 2.0f));
        fx.activate();
        EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
        EXPECT(ex, fx.same_clips());
        gm::Rng rng(3000u + static_cast<uint32_t>(i));
        play(ex, fx, drive, rng, r, Dt::k60, 22.0f, tag);
        play(ex, fx, bump, rng, r, Dt::kIrregular, 6.0f, tag, kCycles | kForce);
        hard(ex, fx, 600, tag);
        // Change it again mid-run.
        EXPECT(ex, fx.set(prm.name, prm.max + prm.min - v));
        EXPECT(ex, fx.same_clips());
        play(ex, fx, drive, rng, r, Dt::k60, 4.0f, tag);
      }
    }
}

TEST(anim_scrub_axes) {
  Expect ex;
  diff::Rand r(3107);
  std::unique_ptr<Art> flat = make_flat();
  const float axes[] = {-1.0f, -0.6f, -0.5f, 0.0f, 0.4f, 0.5f, 1.0f, 1.49f, 1.5f, 2.0f};
  const float rates[] = {12.0f, -7.5f, 64.0f, -64.0f, 0.0f};
  int changed = 0;
  for (const Pair& pr : {demo_pair(), art_pair(*flat), export_pair()})
    for (float axis : axes)
      for (float rate : rates) {
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s scrub %g x %g", pr.tag, static_cast<double>(axis),
                      static_cast<double>(rate));
        Fx fx(pr);
        EXPECT(ex, fx.set("scrub_axis", axis));
        EXPECT(ex, fx.set("frames_g", rate));
        fx.activate();
        gm::Rng rng(3200u);
        for (int i = 0; i < gm::Scenario::presetCount(); ++i)
          play(ex, fx, gm::Scenario::preset(i), rng, r, i % 2 ? Dt::kIrregular : Dt::k60, 3.0f, tag);
        hard(ex, fx, 300, tag);
        ++changed;
      }
  CHECK(changed == 3 * 10 * 5);
}

TEST(anim_force_state) {
  Expect ex;
  diff::Rand r(3108);
  std::unique_ptr<Art> flat = make_flat(), tiny = make_tiny();
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  for (const Pair& pr : {demo_pair(), export_pair(), art_pair(*flat), art_pair(*tiny)}) {
    // Hold each state (and out-of-range values, which release) through a whole drive.
    for (int s = -3; s <= GM_ANIM_STATE_COUNT + 1; ++s)
      for (int mode = 0; mode < 2; ++mode) {
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s forced %d", pr.tag, s);
        Fx fx(pr);
        fx.activate();
        fx.force(s);
        EXPECT(ex, fx.same_state());
        if (s >= 0 && s < GM_ANIM_STATE_COUNT) EXPECT(ex, gm_anim_state(fx.c.get()) == s && fx.c->forced == s);
        if (s < 0 || s >= GM_ANIM_STATE_COUNT) EXPECT(ex, fx.c->forced == -1);
        gm::Rng rng(3300u + static_cast<uint32_t>(s + 3));
        play(ex, fx, drive, rng, r, mode ? Dt::kIrregular : Dt::k60, 21.0f, tag);
        if (s >= 0 && s < GM_ANIM_STATE_COUNT) EXPECT(ex, gm_anim_state(fx.c.get()) == s);
        // Release mid-run.
        fx.force(-1);
        play(ex, fx, drive, rng, r, Dt::k60, 8.0f, tag);
      }
    // Every state forced in turn while driving, also forced again while held,
    // also resets in between (which keep the forced state).
    Fx fx(pr);
    fx.activate();
    gm::Rng rng(3399u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 3000; ++frame) {
      if (frame % 25 == 0) fx.force(r.irange(-2, GM_ANIM_STATE_COUNT));
      if (frame % 25 == 12 && r.chance(0.3)) fx.force(gm_anim_state(fx.c.get()));
      if (frame % 50 == 37 && r.chance(0.2)) fx.reset();
      const float dt = frame % 3 ? 1.0f / 60.0f : odd_dt(r);
      t += dt;
      if (!EXPECT(ex, fx.update(sc.at(std::fmod(t, sc.duration()), &rng), dt))) {
        fx.note_state(pr.tag);
        break;
      }
      std::snprintf(label, sizeof(label), "%s force cycle frame %d", pr.tag, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(anim_resolve_clip) {
  Expect ex;
  diff::Rand r(3109);
  // Every combination of empty clips (bit s of `empty` = clip s has count 0),
  // with and without mirroring, on assets of 1, 3 and 8 frames, with the
  // non-empty clips in several shapes: plain, starting past the end (unusable),
  // running past the end (clamped), odd rates and loop values.
  static uint8_t bits[8 * 8 * 2];
  for (size_t i = 0; i < sizeof(bits); ++i) bits[i] = static_cast<uint8_t>(i * 37u + 11u);
  const int frame_counts[] = {1, 3, 8};
  int assets = 0, mirrored = 0, all_frames = 0;
  for (int frames : frame_counts)
    for (int empty = 0; empty < 64; ++empty)
      for (int mirror = 0; mirror < 2; ++mirror)
        for (int shape = 0; shape < 6; ++shape) {
          gm::AnimationAsset pa;
          pa.name = "clips";
          pa.width = 8;
          pa.height = 8;
          pa.depth = 2;
          pa.frames = static_cast<uint16_t>(frames);
          pa.bits = bits;
          pa.mapping.mirror = mirror ? 1.0f : 0.0f;
          pa.mapping.holdS = 0.1f;
          for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) {
            gm::AnimClip& c = pa.clips[s];
            if (empty & (1 << s)) {
              c.count = 0;
              c.start = static_cast<uint16_t>(r.irange(0, frames));
              c.fps = r.chance(0.3) ? 0.0f : r.frange(1.0f, 30.0f);
              continue;
            }
            c.start = static_cast<uint16_t>(r.irange(0, frames - 1));
            c.count = static_cast<uint16_t>(r.irange(1, frames - c.start));
            c.fps = r.frange(2.0f, 40.0f);
            c.loop = static_cast<uint8_t>(r.irange(0, 1));
            switch (shape) {
              case 1: c.start = static_cast<uint16_t>(frames + r.irange(0, 2)); break;    // unusable
              case 2: c.count = static_cast<uint16_t>(frames + r.irange(0, 40)); break;  // clamped
              case 3: {
                const float odd[] = {0.0f, -3.0f, 1e-3f, 0.5f, 240.0f, 1e6f, diff::kInf, diff::kNaN};
                c.fps = r.pick(odd);
                break;
              }
              case 4: c.loop = static_cast<uint8_t>(r.irange(2, 255)); break;
              case 5: c.count = 65535; c.start = static_cast<uint16_t>(r.irange(0, frames - 1)); break;
              default: break;
            }
          }
          const gm_anim_asset_t ca = to_c(pa);
          const Pair pr{&pa, &ca, "clips"};
          Fx fx(pr);
          ++assets;
          for (int s = -3; s <= GM_ANIM_STATE_COUNT + 2; ++s) {
            bool mc = false, mp = false;
            const gm_anim_clip_t a = gm_anim_resolve_clip(fx.c.get(), s, &mc);
            const gm::AnimClip b = fx.p->resolveClip(s, mp);
            if (!EXPECT(ex, same_clip(a, b) && mc == mp))
              harness::note("frames %d empty %d mirror %d shape %d state %d: C %u+%u @%g loop %u m%d, C++ %u+%u @%g "
                            "loop %u m%d",
                            frames, empty, mirror, shape, s, a.start, a.count, static_cast<double>(a.fps), a.loop,
                            mc ? 1 : 0, b.start, b.count, static_cast<double>(b.fps), b.loop, mp ? 1 : 0);
            mirrored += mc;
            all_frames += a.start == 0 && a.count == frames && (empty & 1);
          }
          // And play it briefly: every state through g, bumps, forced states.
          fx.activate();
          char label[128];
          for (int frame = 0; frame < 90; ++frame) {
            const float phase = static_cast<float>(frame % 30) / 30.0f;
            const int dir = (frame / 30 + shape + empty) % 5;
            const float g = 0.6f * std::sin(phase * 3.14159f);
            const gm::GForce in = dyn_g(dir == 1 ? g : (dir == 2 ? -g : 0.0f), frame % 31 == 5 ? 0.8f : 0.0f,
                                        dir == 3 ? g : (dir == 4 ? -g : 0.0f));
            if (frame == 60 && (empty & 2)) fx.force(r.irange(0, GM_ANIM_STATE_COUNT - 1));
            if (!EXPECT(ex, fx.update(in, frame % 4 ? 1.0f / 60.0f : odd_dt(r)))) {
              fx.note_state("clips");
              break;
            }
            std::snprintf(label, sizeof(label), "clips frames %d empty %d mirror %d shape %d frame %d", frames, empty,
                          mirror, shape, frame);
            if (!EXPECT(ex, fx.render_same(label))) break;
          }
        }
  harness::note("%d assets, %d mirrored resolutions, %d all-frames fallbacks", assets, mirrored, all_frames);
  CHECK(mirrored > 300);
  CHECK(all_frames > 500);
}

TEST(anim_set_asset) {
  Expect ex;
  diff::Rand r(3110);
  std::unique_ptr<Art> flat = make_flat(), big = make_big(), tiny = make_tiny(), fast = make_fast();
  Art empty, nobits;
  empty.finish();
  nobits.p = flat->p;
  nobits.p.name = "nobits";
  nobits.finish(true);
  const Pair pairs[] = {demo_pair(),     export_pair(),    art_pair(*flat),  art_pair(*big),
                        art_pair(*tiny), art_pair(*fast),  art_pair(empty), art_pair(nobits)};
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& slalom = *gm::Scenario::find("slalom");
  int clamped = 0;
  for (const Pair& from : pairs)
    for (const Pair& to : pairs)
      for (int combo = 0; combo < 4; ++combo) {
        const bool apply = combo & 1, reset = combo & 2;
        char tag[128];
        std::snprintf(tag, sizeof(tag), "%s -> %s apply %d reset %d", from.tag, to.tag, apply ? 1 : 0, reset ? 1 : 0);
        Fx fx(from);
        fx.activate();
        if (r.chance(0.5)) EXPECT(ex, fx.set("hz", r.frange(0.3f, 8.0f)));  // a tunable the swap may overwrite
        gm::Rng rng(3500u + static_cast<uint32_t>(combo));
        play(ex, fx, r.chance(0.5) ? drive : slalom, rng, r, Dt::k60, r.frange(1.0f, 6.0f), tag);
        if (r.chance(0.3)) fx.force(r.irange(0, GM_ANIM_STATE_COUNT - 1));
        const int before = gm_anim_current_frame(fx.c.get());
        fx.set_asset(to, apply, reset);
        if (!EXPECT(ex, fx.same_state() && fx.same_clips())) fx.note_state(tag);
        if (!reset) clamped += gm_anim_current_frame(fx.c.get()) < before;
        EXPECT(ex, same_asset(*gm_anim_asset(fx.c.get()), fx.p->asset()));
        EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
        if (apply) EXPECT(ex, same_mapping(fx.c->m, to.p->mapping));
        EXPECT(ex, fx.render_same(tag));
        play(ex, fx, drive, rng, r, Dt::kIrregular, 3.0f, tag);
        // Re-applying the effect's own asset.
        gm_anim_set_asset(fx.c.get(), gm_anim_asset(fx.c.get()), !apply, !reset);
        fx.p->setAsset(fx.p->asset(), !apply, !reset);
        EXPECT(ex, fx.same_state() && fx.same_clips());
        EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
        play(ex, fx, slalom, rng, r, Dt::k60, 2.0f, tag);
      }
  harness::note("swaps without a reset that clamped the frame: %d", clamped);
  CHECK(clamped > 20);
}

TEST(anim_nan_inf_g) {
  Expect ex;
  diff::Rand r(3111);
  std::unique_ptr<Art> flat = make_flat();
  // The artwork stays finite because its target is clamped NaN-safely;
  // the clip time can become NaN (it then shows the clip's first frame, as the
  // x86 C++ does) until a reset or a state change restarts it.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, -3e38f, 1e6f, -1e6f, 1e-30f};
  int nan_time = 0, runs = 0;
  for (int run = 0; run < 60; ++run) {
    const int variant = run % 6;
    // Variants 4 and 5 scrub frames with g: not in the sanitizer build (see kSanitized).
    if (kSanitized && variant >= 4) continue;
    std::unique_ptr<Fx> fx(new Fx(variant == 5 ? art_pair(*flat) : (run % 2 ? export_pair() : demo_pair())));
    if (variant == 1) EXPECT(ex, fx->set("inertia", 0.0f));
    if (variant == 2) EXPECT(ex, fx->set("fps_g", 3.0f));
    if (variant == 3) EXPECT(ex, fx->set("hold_s", 0.0f));
    if (variant == 4) {
      EXPECT(ex, fx->set("scrub_axis", static_cast<float>(run % 3)));
      EXPECT(ex, fx->set("frames_g", -9.0f));
    }
    fx->activate();
    ++runs;
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
      if (!EXPECT(ex, fx->update(g, dt))) {
        fx->note_state("bad g");
        break;
      }
      const gm_vec3_t pos = gm_anim_position(fx->c.get());
      EXPECT(ex, std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z));
      if (std::isnan(fx->c->clip_time)) ++nan_time;
      if (r.chance(0.01)) fx->reset();
      if (r.chance(0.01)) fx->force(r.chance(0.5) ? -1 : r.irange(0, GM_ANIM_STATE_COUNT - 1));
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx->render_same(label))) break;
    }
  }
  harness::note("%d runs, updates with a NaN clip time: %d", runs, nan_time);
  CHECK(nan_time > 1000);
}

TEST(anim_long_run) {
  Expect ex;
  diff::Rand r(3112);
  std::unique_ptr<Art> fast = make_fast();
  // 10 minutes at 60 fps of very fast clips (the clip timer is wrapped again
  // and again) ...
  {
    Fx fx(art_pair(*fast));
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long fast");
    EXPECT(ex, frames >= 600 * 60);
    harness::note("clip timer wrapped %d times", fx.timer_bounded);
    CHECK(fx.timer_bounded > 100);
  }
  // ... 10 minutes of the demo ...
  {
    Fx fx(demo_pair());
    EXPECT(ex, fx.set("fps_g", 8.0f));
    fx.activate();
    gm::Rng rng(7001u);
    play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long demo");
  }
  // ... and an hour at the engine's slowest step.
  {
    Fx fx(export_pair());
    EXPECT(ex, fx.set("hz", 8.0f));
    EXPECT(ex, fx.set("hold_s", 0.0f));
    fx.activate();
    gm::Rng rng(7002u);
    const gm::Scenario& sc = *gm::Scenario::find("slalom");
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 3600 * 15; ++frame) {
      t += 1.0f / 15.0f;
      if (!EXPECT(ex, fx.update(sc.at(std::fmod(t, sc.duration() + 3.0f), &rng), 1.0f / 15.0f))) {
        fx.note_state("long 15fps");
        break;
      }
      std::snprintf(label, sizeof(label), "long 15fps frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
}

TEST(anim_fuzz) {
  Expect ex;
  diff::Rand r(3113);
  gm::AnimationEffect proto(gm::demoAnimation());
  int invalid = 0;
  for (int run = 0; run < 160; ++run) {
    Art a;
    a.p.name = "fuzz";
    const bool large = r.chance(0.1);
    a.p.width = static_cast<uint16_t>(r.irange(large ? 0 : 1, large ? GM_W + 30 : 40));
    a.p.height = static_cast<uint16_t>(r.irange(large ? 0 : 1, large ? GM_H + 20 : 30));
    a.p.depth = static_cast<uint16_t>(r.chance(0.6) ? 1 : r.irange(0, large ? 12 : 9));
    a.p.frames = static_cast<uint16_t>(r.irange(r.chance(0.05) ? 0 : 1, 40));
    for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) {
      gm::AnimClip& c = a.p.clips[s];
      if (r.chance(0.3)) continue;  // default: empty
      c.start = static_cast<uint16_t>(r.irange(0, a.p.frames + 3));
      c.count = static_cast<uint16_t>(r.irange(0, a.p.frames + 5));
      const float fps[] = {0.0f, -1.0f, 0.5f, 6.0f, 12.0f, 24.0f, 60.0f, 240.0f};
      c.fps = r.chance(0.5) ? r.pick(fps) : r.frange(1.0f, 60.0f);
      c.loop = static_cast<uint8_t>(r.irange(0, 1));
    }
    gm::AnimMapping& m = a.p.mapping;
    // Within the parameter ranges, sometimes a little outside (an asset's
    // mapping is not clamped).
    m.pxPerGx = r.frange(0.0f, 220.0f);
    m.pxPerGy = r.frange(0.0f, 110.0f);
    m.pxPerGz = r.frange(0.0f, 220.0f);
    m.springHz = r.frange(0.3f, 8.0f);
    m.inertia = r.frange(0.0f, 1.0f);
    m.enterG = r.frange(0.02f, 1.5f);
    m.holdS = r.chance(0.3) ? 0.0f : r.frange(0.0f, 3.0f);
    m.bumpG = r.frange(0.05f, 2.0f);
    m.fpsPerG = r.chance(0.5) ? 0.0f : r.frange(0.0f, 8.0f);
    m.scrubAxis = r.chance(0.4) ? -1.0f : r.frange(-2.0f, 3.0f);
    m.framesPerG = r.chance(0.3) ? 0.0f : r.frange(-64.0f, 64.0f);
    m.echo = r.chance(0.5) ? 0.0f : r.frange(-1.0f, 6.0f);
    m.mirror = r.chance(0.5) ? 1.0f : r.frange(0.0f, 1.0f);
    a.fill(r, r.frange(0.0f, 0.6f));
    if (r.chance(0.03)) a.finish(true);
    if (!a.p.valid()) ++invalid;
    Fx fx(art_pair(a));
    EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
    EXPECT(ex, fx.same_clips());
    fx.activate();
    gm::Vec3 dyn, target;
    char label[128];
    for (int frame = 0; frame < 400; ++frame) {
      if (r.chance(0.03))
        target = gm::Vec3{r.frange(-2.5f, 2.5f), r.chance(0.2) ? r.frange(-0.8f, 0.8f) : 0.0f, r.frange(-2.5f, 2.5f)};
      dyn = gm::lerp(dyn, target, 0.1f);
      gm::Vec3 d = dyn;
      if (r.chance(0.04)) d.y += r.frange(-1.5f, 1.5f);
      if (r.chance(0.01)) d = gm::Vec3{r.frange(-6, 6), r.frange(-6, 6), r.frange(-6, 6)};
      const int updates = r.chance(0.1) ? r.irange(0, 3) : 1;
      bool ok = true;
      for (int u = 0; u < updates; ++u) ok = fx.update(dyn_g(d.x, d.y, d.z), odd_dt(r)) && ok;
      if (!EXPECT(ex, ok)) {
        fx.note_state("fuzz");
        break;
      }
      if (r.chance(0.01)) {
        const gm::Param& prm = proto.params().at(r.irange(0, proto.params().count() - 1));
        EXPECT(ex, fx.set(prm.name, r.frange(prm.min - 1.0f, prm.max + 1.0f)));
      }
      if (r.chance(0.01)) fx.force(r.chance(0.5) ? -1 : r.irange(-1, GM_ANIM_STATE_COUNT));
      if (r.chance(0.005)) fx.reset();
      if (r.chance(0.005)) {
        fx.deactivate();
        fx.activate();
      }
      std::snprintf(label, sizeof(label), "fuzz run %d frame %d", run, frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
  }
  harness::note("fuzz runs with an invalid asset: %d", invalid);
  CHECK(invalid > 3);
}

TEST(anim_odd_mappings) {
  Expect ex;
  diff::Rand r(3114);
  std::unique_ptr<Art> flat = make_flat();
  // An asset's mapping is not clamped (only values set through the table are):
  // non-finite and huge values in a hand-written asset, field by field. A NaN,
  // infinite or out-of-int-range echo makes the C++ negate INT_MIN (undefined;
  // an x86 build draws nothing, and so does the C), so those runs are skipped
  // in the sanitizer build, as are the scrubbing runs with a NaN clip time.
  const float odd[] = {diff::kNaN, diff::kInf, -diff::kInf, 3e9f, -3e9f, 1e30f, -1e30f, 1e5f, -1e5f, -0.0f};
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
  int runs = 0;
  for (int field = 0; field < 13; ++field)
    for (float v : odd)
      for (int k = 0; k < 2; ++k) {
        const bool int_min_echo = field == 11 && !(v >= -2147483648.0f && v < 2147483648.0f);
        // The flat asset scrubs frames with g: a non-finite or huge fps_g makes the
        // clip time NaN or huge there, the overflow described at kSanitized.
        const bool wild_clip_time = k == 1 && field == 8 && !(std::fabs(v) <= 1e5f);
        if (kSanitized && (int_min_echo || wild_clip_time)) continue;
        gm::AnimationAsset pa = k ? flat->p : gm::demoAnimation();
        float* fields[] = {&pa.mapping.pxPerGx, &pa.mapping.pxPerGy,  &pa.mapping.pxPerGz,    &pa.mapping.springHz,
                           &pa.mapping.inertia, &pa.mapping.enterG,   &pa.mapping.holdS,      &pa.mapping.bumpG,
                           &pa.mapping.fpsPerG, &pa.mapping.scrubAxis, &pa.mapping.framesPerG, &pa.mapping.echo,
                           &pa.mapping.mirror};
        *fields[field] = v;
        if (field == 9) pa.mapping.framesPerG = 6.0f;  // an axis that matters
        if (field == 10) pa.mapping.scrubAxis = 2.0f;
        const gm_anim_asset_t ca = to_c(pa);
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s mapping field %d = %g", k ? "flat" : "demo", field, static_cast<double>(v));
        Fx fx(Pair{&pa, &ca, tag});
        EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
        EXPECT(ex, fx.same_clips());
        fx.activate();
        gm::Rng rng(3600u + static_cast<uint32_t>(field));
        play(ex, fx, drive, rng, r, Dt::k60, 8.0f, tag);
        play(ex, fx, bump, rng, r, Dt::kIrregular, 4.0f, tag, kForce);
        // Also swapped in mid-run without a reset.
        Fx sw(k ? art_pair(*flat) : demo_pair());
        sw.activate();
        play(ex, sw, drive, rng, r, Dt::k60, 3.0f, tag);
        sw.set_asset(Pair{&pa, &ca, tag}, true, false);
        play(ex, sw, drive, rng, r, Dt::kIrregular, 4.0f, tag);
        ++runs;
      }
  harness::note("%d runs", runs);
  CHECK(runs == (kSanitized ? 2 * 12 * 10 + 2 * 3 - 7 : 2 * 13 * 10));
}
