// Differential tests of the "sprite" effect (gm_sprite.c vs gm::SpriteEffect):
// the demo sheet (every bit), the default mapping (NULL on the C side), the
// parameter table for default and custom mappings, every scenario preset at
// 60 fps and with irregular dt, every scrub axis, every tunable at its min, max
// and a middle value, set_sheet mid-run, reset/activate/deactivate cycles
// mid-run, NaN/inf g, long runs (the playhead wraps) and random fuzzing. After
// every update the frame index and position must match bit for bit, and every
// update batch is followed by a render into cleared frames that must match
// byte for byte.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

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

gm_sprite_mapping_t to_c(const gm::SpriteMapping& m) {
  gm_sprite_mapping_t r;
  r.px_per_g = diff::to_c(m.pxPerG);
  r.smoothing_hz = m.smoothingHz;
  r.scrub_axis = m.scrubAxis;
  r.frames_per_g = m.framesPerG;
  r.idle_fps = m.idleFps;
  r.fps_per_g = m.fpsPerG;
  r.depth_echo = m.depthEcho;
  r.follow_inertia = m.followInertia;
  return r;
}

gm_sprite_sheet_t to_c(const gm::SpriteSheet& s) {
  gm_sprite_sheet_t r;
  r.width = s.width;
  r.height = s.height;
  r.frames = s.frames;
  r.bits = s.bits;
  return r;
}

bool same_mapping(const gm_sprite_mapping_t& c, const gm::SpriteMapping& p) {
  return diff::same(c.px_per_g, p.pxPerG) && diff::same(c.smoothing_hz, p.smoothingHz) &&
         c.scrub_axis == p.scrubAxis && diff::same(c.frames_per_g, p.framesPerG) &&
         diff::same(c.idle_fps, p.idleFps) && diff::same(c.fps_per_g, p.fpsPerG) &&
         diff::same(c.depth_echo, p.depthEcho) && c.follow_inertia == p.followInertia;
}

// Every field; the artwork is compared byte for byte (not by pointer).
bool same_sheet(const gm_sprite_sheet_t& c, const gm::SpriteSheet& p) {
  bool ok = c.width == p.width && c.height == p.height && c.frames == p.frames;
  ok = ok && (c.bits == nullptr) == (p.bits == nullptr);
  if (ok && c.bits && p.frames > 0 && p.frameBytes() > 0)
    ok = std::memcmp(c.bits, p.bits, static_cast<size_t>(p.frames * p.frameBytes())) == 0;
  return ok;
}

// A sprite sheet owned by the test, with matching C and C++ descriptions.
struct Sheet {
  std::vector<uint8_t> bits;
  gm::SpriteSheet p;
  gm_sprite_sheet_t c;
  Sheet(int w, int h, int frames) {
    p.width = w;
    p.height = h;
    p.frames = frames;
  }
  // Sizes the artwork for the dimensions and fills it at random (density 0..1).
  void fill(diff::Rand& r, float density) {
    const int bytes = p.frames > 0 && p.width > 0 && p.height > 0 ? p.frames * p.frameBytes() : 0;
    bits.assign(static_cast<size_t>(bytes), 0);
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

struct Pair {
  const gm::SpriteSheet* p;
  const gm_sprite_sheet_t* c;
  const char* tag;
};
Pair demo_pair() { return Pair{&gm::demoSprite(), gm_sprite_demo_sheet(), "demo"}; }
Pair sheet_pair(const Sheet& s, const char* tag) { return Pair{&s.p, &s.c, tag}; }

// A larger sheet with many frames (a nontrivial frame wrap) and some empty rows.
std::unique_ptr<Sheet> make_big() {
  std::unique_ptr<Sheet> s(new Sheet(37, 21, 29));
  diff::Rand r(91);
  s->fill(r, 0.25f);
  return s;
}

// A mapping with every feature on: autonomous playback, lateral scrub, deep echo.
gm::SpriteMapping lively_mapping() {
  gm::SpriteMapping m;
  m.pxPerG = gm::Vec3{80.0f, 30.0f, 90.0f};
  m.smoothingHz = 3.5f;
  m.scrubAxis = 0;
  m.framesPerG = -11.0f;
  m.idleFps = 9.0f;
  m.fpsPerG = 30.0f;
  m.depthEcho = 3.0f;
  m.followInertia = false;
  return m;
}

// ---- the effect pair -----------------------------------------------------------------

// One C effect and one C++ effect driven with identical contexts.
struct Fx {
  std::unique_ptr<gm_sprite_t> c{new gm_sprite_t};
  std::unique_ptr<gm::SpriteEffect> p;
  diff::FramePair f;
  gm_effect_ctx_t cctx = GM_EFFECT_CTX_INIT;
  gm::EffectContext pctx;
  // Coverage counters.
  uint64_t frames_seen = 0;  // bit i: frame index i shown (i < 64)
  int wrapped = 0;           // updates where the playhead was wrapped (C side)
  int lit = 0;               // rendered frames with something on the glass
  int layers_seen = 0;       // bit z: something lit on layer z

  // The default mapping: NULL on the C side, the default argument on the C++ side.
  explicit Fx(const Pair& s) : p(new gm::SpriteEffect(*s.p)) { gm_sprite_init(c.get(), s.c, nullptr); }
  Fx(const Pair& s, const gm::SpriteMapping& m) : p(new gm::SpriteEffect(*s.p, m)) {
    const gm_sprite_mapping_t cm = to_c(m);
    gm_sprite_init(c.get(), s.c, &cm);
  }
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
  void set_sheet(const Pair& s) {
    gm_sprite_set_sheet(c.get(), s.c);
    p->setSheet(*s.p);
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
    return gm_sprite_current_frame(c.get()) == p->currentFrame() &&
           diff::same(gm_sprite_position(c.get()), p->position());
  }
  void note_state(const char* label) const {
    const gm_vec3_t a = gm_sprite_position(c.get());
    const gm::Vec3 b = p->position();
    harness::note("%s: C frame %d pos (%.9g, %.9g, %.9g); C++ frame %d pos (%.9g, %.9g, %.9g)", label,
                  gm_sprite_current_frame(c.get()), static_cast<double>(a.x), static_cast<double>(a.y),
                  static_cast<double>(a.z), p->currentFrame(), static_cast<double>(b.x), static_cast<double>(b.y),
                  static_cast<double>(b.z));
  }
  // Advances both; true when the frame index and position match afterwards.
  bool update(const gm::GForce& g, float dt) {
    pctx.g = g;
    pctx.dt = dt;
    pctx.time += dt;
    ++pctx.frame;
    cctx.g = gm_gforce_from_proper(diff::to_c(g.proper));
    cctx.dt = dt;
    cctx.time = pctx.time;
    cctx.frame = pctx.frame;
    const float before = c->playhead;
    gm_effect_update(ce(), &cctx);
    p->update(pctx);
    if (c->playhead < before) ++wrapped;  // otherwise it never goes back (outside reset)
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
    const int i = gm_sprite_current_frame(c.get());
    if (i >= 0 && i < 64) frames_seen |= uint64_t{1} << i;
    if (gm_frame_count_lit(f.c.get()) > 0) ++lit;
    for (int z = 0; z < GM_D; ++z)
      if (gm_frame_count_lit_layer(f.c.get(), z) > 0) layers_seen |= 1 << z;
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

// Plays preset `sc` (looped) for `seconds` into both effects, comparing the
// state after every update and rendering and comparing after every update
// batch. With `events`, resets, deactivate/activate cycles and (when `sheets`
// is given) set_sheet happen mid-run. Stops at the first mismatch. Returns the
// rendered frame count.
int play(Expect& ex, Fx& fx, const gm::Scenario& sc, gm::Rng& rng, diff::Rand& r, Dt mode, float seconds,
         const char* tag, bool events = false, const std::vector<Pair>* sheets = nullptr) {
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
      if (sheets && !sheets->empty() && r.chance(0.006))
        fx.set_sheet((*sheets)[static_cast<size_t>(r.irange(0, static_cast<int>(sheets->size()) - 1))]);
      if (!EXPECT(ex, fx.same_state())) {
        harness::note("%s: state differs after an event", label);
        break;
      }
    }
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
  return frames;
}

// Hard driving for `frames` frames at 60 fps: pinned against every wall in turn.
void hard(Expect& ex, Fx& fx, int frames, const char* tag) {
  char label[160];
  float t = 0.0f;
  for (int frame = 0; frame < frames; ++frame) {
    t += 1.0f / 60.0f;
    const float s = std::sin(t * 1.7f), q = std::cos(t * 0.9f);
    if (!EXPECT(ex, fx.update(dyn_g(2.5f * s, 1.2f * s * q, -2.0f * q), 1.0f / 60.0f))) {
      fx.note_state(tag);
      break;
    }
    std::snprintf(label, sizeof(label), "%s hard frame %d", tag, frame);
    if (!EXPECT(ex, fx.render_same(label))) break;
  }
}

}  // namespace

TEST(sprite_demo_sheet) {
  Expect ex;
  const gm::SpriteSheet& pd = gm::demoSprite();
  const gm_sprite_sheet_t* cd = gm_sprite_demo_sheet();
  EXPECT(ex, cd != nullptr);
  EXPECT(ex, gm_sprite_demo_sheet() == cd);       // built once
  EXPECT(ex, cd->bits && cd->bits != pd.bits);   // its own artwork ...
  EXPECT(ex, same_sheet(*cd, pd));               // ... equal to the C++ demo in every bit
  EXPECT(ex, cd->width == 16 && cd->height == 16 && cd->frames == 12);
  EXPECT(ex, gm_sprite_sheet_row_bytes(cd) == pd.rowBytes() && gm_sprite_sheet_frame_bytes(cd) == pd.frameBytes());
  int lit = 0, diffs = 0;
  for (int fr = -1; fr <= pd.frames; ++fr)
    for (int y = -2; y < pd.height + 2; ++y)
      for (int x = -2; x < pd.width + 2; ++x) {
        const bool a = gm_sprite_sheet_pixel(cd, fr, x, y), b = pd.pixel(fr, x, y);
        diffs += a != b;
        lit += a;
      }
  EXPECT(ex, diffs == 0);
  harness::note("demo: %d lit pixels", lit);
  EXPECT(ex, lit > 300);
  EXPECT(ex, same_sheet(*gm_sprite_demo_sheet(), gm::demoSprite()));  // calling again changes nothing

  // The inline sheet helpers on random artwork, and without artwork.
  diff::Rand r(4101);
  for (int run = 0; run < 60; ++run) {
    Sheet s(r.irange(1, 40), r.irange(1, 20), r.irange(1, 6));
    s.fill(r, r.frange(0.0f, 1.0f));
    EXPECT(ex, gm_sprite_sheet_row_bytes(&s.c) == s.p.rowBytes());
    EXPECT(ex, gm_sprite_sheet_frame_bytes(&s.c) == s.p.frameBytes());
    int d = 0;
    for (int fr = -1; fr <= s.p.frames; ++fr)
      for (int y = -1; y <= s.p.height; ++y)
        for (int x = -1; x <= s.p.width + 8; ++x) d += gm_sprite_sheet_pixel(&s.c, fr, x, y) != s.p.pixel(fr, x, y);
    EXPECT(ex, d == 0);
    s.finish(true);
    EXPECT(ex, !gm_sprite_sheet_pixel(&s.c, 0, 0, 0) && !s.p.pixel(0, 0, 0));
  }
}

TEST(sprite_params) {
  Expect ex;
  // GM_SPRITE_MAPPING_DEFAULT is gm::SpriteMapping{}.
  const gm_sprite_mapping_t md = GM_SPRITE_MAPPING_DEFAULT;
  EXPECT(ex, same_mapping(md, gm::SpriteMapping{}));
  // NULL mapping = the defaults = the C++ default argument.
  Fx fx(demo_pair());
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), fx.p->name()) == 0);
  EXPECT(ex, std::strcmp(gm_effect_name(fx.ce()), "sprite") == 0);
  EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
  Fx explicit_default(demo_pair(), gm::SpriteMapping{});
  EXPECT(ex, same_param_tables(gm_effect_params(explicit_default.ce()), fx.p->params()));
  EXPECT(ex, same_param_tables(gm_effect_params(explicit_default.ce()), explicit_default.p->params()));
  EXPECT(ex, fx.c->playhead == 0.0f && fx.c->frame_index == 0);
  EXPECT(ex, fx.c->pos.x.pos == 0.0f && fx.c->pos.y.pos == 0.0f && fx.c->pos.z.pos == 0.0f);
  EXPECT(ex, fx.c->pos.x.vel == 0.0f && fx.c->pos.y.vel == 0.0f && fx.c->pos.z.vel == 0.0f);
  EXPECT(ex, same_sheet(fx.c->sheet, gm::demoSprite()));
  EXPECT(ex, fx.same_state());
  // The vtable: activate falls back to reset, no burn-in offset.
  EXPECT(ex, fx.ce()->vt->activate == nullptr && fx.ce()->vt->deactivate == nullptr);
  EXPECT(ex, fx.ce()->vt->reset != nullptr && fx.ce()->vt->burn_in_offset == nullptr);
  int dxc = 5, dyc = 5, dxp = 5, dyp = 5;
  gm_effect_burn_in_offset(fx.ce(), &dxc, &dyc);
  fx.p->burnInOffset(dxp, dyp);
  EXPECT(ex, dxc == 0 && dyc == 0 && dxp == 0 && dyp == 0);
  EXPECT(ex, fx.render_same("fresh render"));
  // Each name points at the field the header documents.
  gm_sprite_t* c = fx.c.get();
  const struct {
    const char* name;
    float* field;
  } fields[] = {{"px_g_x", &c->px_gx},       {"px_g_y", &c->px_gy},           {"px_g_z", &c->px_gz},
                {"hz", &c->hz},              {"scrub_axis", &c->scrub_axis},  {"frames_g", &c->frames_per_g},
                {"idle_fps", &c->idle_fps},  {"fps_g", &c->fps_per_g},        {"echo", &c->echo},
                {"inertia", &c->inertia}};
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
  // Custom mappings (not clamped: they are the defaults the table starts from).
  diff::Rand r(4102);
  for (int run = 0; run < 200; ++run) {
    gm::SpriteMapping m;
    m.pxPerG = gm::Vec3{r.frange(-10.0f, 250.0f), r.frange(-10.0f, 120.0f), r.frange(-10.0f, 250.0f)};
    m.smoothingHz = r.frange(0.0f, 10.0f);
    m.scrubAxis = r.irange(-3, 4);
    m.framesPerG = r.frange(-80.0f, 80.0f);
    m.idleFps = r.frange(-5.0f, 70.0f);
    m.fpsPerG = r.frange(-5.0f, 130.0f);
    m.depthEcho = r.frange(-1.0f, 6.0f);
    m.followInertia = r.chance(0.5);
    Fx cm(demo_pair(), m);
    EXPECT(ex, same_param_tables(gm_effect_params(cm.ce()), cm.p->params()));
    EXPECT(ex, cm.c->inertia == (m.followInertia ? 1.0f : 0.0f));
    EXPECT(ex, cm.c->scrub_axis == static_cast<float>(m.scrubAxis));
  }
}

TEST(sprite_presets_60fps) {
  Expect ex;
  diff::Rand r(4103);
  std::unique_ptr<Sheet> big = make_big();
  uint64_t seen = 0;
  int layers = 0;
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
      const gm::Scenario& sc = gm::Scenario::preset(i);
      std::unique_ptr<Fx> fx(k ? new Fx(sheet_pair(*big, "big"), lively_mapping()) : new Fx(demo_pair()));
      fx->activate();
      gm::Rng rng(1000u + static_cast<uint32_t>(i));
      const int frames = play(ex, *fx, sc, rng, r, Dt::k60, 22.0f, k ? "big" : "demo");
      EXPECT(ex, frames >= 22 * 60);
      EXPECT(ex, fx->lit == frames);  // something is always on the glass
      if (!k) seen |= fx->frames_seen;
      layers |= fx->layers_seen;
    }
  harness::note("demo frames shown: mask 0x%llx, layers lit 0x%x", static_cast<unsigned long long>(seen), layers);
  CHECK(seen == 0xFFF);  // every one of the 12 frames
  CHECK(layers == (1 << GM_D) - 1);
}

TEST(sprite_presets_irregular_dt) {
  Expect ex;
  diff::Rand r(4104);
  std::unique_ptr<Sheet> big = make_big();
  for (int rep = 0; rep < 2; ++rep)
    for (int k = 0; k < 2; ++k)
      for (int i = 0; i < gm::Scenario::presetCount(); ++i) {
        const gm::Scenario& sc = gm::Scenario::preset(i);
        std::unique_ptr<Fx> fx(k ? new Fx(sheet_pair(*big, "big"), lively_mapping()) : new Fx(demo_pair()));
        if (rep) {  // livelier: quick spring, autonomous playback
          EXPECT(ex, fx->set("hz", 7.0f));
          EXPECT(ex, fx->set("idle_fps", 24.0f));
        }
        fx->activate();
        gm::Rng rng(2000u + static_cast<uint32_t>(i * 7 + rep * 3 + k));
        play(ex, *fx, sc, rng, r, Dt::kIrregular, 25.0f, k ? "big irregular" : "demo irregular");
      }
}

TEST(sprite_scrub_axes) {
  Expect ex;
  diff::Rand r(4105);
  std::unique_ptr<Sheet> big = make_big();
  // Through the mapping (an int, not clamped) ...
  for (int axis = -3; axis <= 4; ++axis)
    for (int inertia = 0; inertia < 2; ++inertia) {
      gm::SpriteMapping m;
      m.scrubAxis = axis;
      m.followInertia = inertia != 0;
      m.framesPerG = axis % 2 ? 13.0f : -9.0f;
      char tag[64];
      std::snprintf(tag, sizeof(tag), "mapping axis %d inertia %d", axis, inertia);
      Fx fx(axis % 2 ? sheet_pair(*big, "big") : demo_pair(), m);
      fx.activate();
      gm::Rng rng(4200u + static_cast<uint32_t>(axis + 3));
      for (int i = 0; i < gm::Scenario::presetCount(); ++i)
        play(ex, fx, gm::Scenario::preset(i), rng, r, i % 2 ? Dt::kIrregular : Dt::k60, 6.0f, tag);
      hard(ex, fx, 300, tag);
    }
  // ... and through the table (a float, rounded).
  const float axes[] = {-1.0f, -0.6f, -0.5f, 0.0f, 0.4f, 0.5f, 1.0f, 1.49f, 1.5f, 2.0f};
  const float rates[] = {8.0f, -12.5f, 64.0f, -64.0f, 0.0f};
  for (float axis : axes)
    for (float rate : rates) {
      char tag[64];
      std::snprintf(tag, sizeof(tag), "table axis %g x %g", static_cast<double>(axis), static_cast<double>(rate));
      Fx fx(demo_pair());
      EXPECT(ex, fx.set("scrub_axis", axis));
      EXPECT(ex, fx.set("frames_g", rate));
      if (rate == 0.0f) EXPECT(ex, fx.set("idle_fps", 7.0f));
      fx.activate();
      gm::Rng rng(4300u);
      for (int i = 0; i < gm::Scenario::presetCount(); ++i)
        play(ex, fx, gm::Scenario::preset(i), rng, r, i % 2 ? Dt::kIrregular : Dt::k60, 3.0f, tag);
      hard(ex, fx, 300, tag);
    }
}

TEST(sprite_events) {
  Expect ex;
  diff::Rand r(4106);
  std::unique_ptr<Sheet> big = make_big();
  Sheet one(5, 3, 1), none(16, 16, 0), nobits(16, 16, 12), wide(GM_W + 30, GM_H + 10, 3);
  one.fill(r, 0.6f);
  none.finish();
  nobits.finish(true);
  wide.fill(r, 0.02f);
  const std::vector<Pair> sheets = {demo_pair(), sheet_pair(*big, "big"), sheet_pair(one, "one"),
                                    sheet_pair(none, "none"), sheet_pair(nobits, "nobits"), sheet_pair(wide, "wide")};
  for (int i = 0; i < gm::Scenario::presetCount(); ++i)
    for (int mode = 0; mode < 2; ++mode)
      for (int k = 0; k < 2; ++k) {
        const gm::Scenario& sc = gm::Scenario::preset(i);
        std::unique_ptr<Fx> fx(k ? new Fx(sheet_pair(*big, "big"), lively_mapping()) : new Fx(demo_pair()));
        fx->activate();
        gm::Rng rng(2500u + static_cast<uint32_t>(i * 3 + mode));
        play(ex, *fx, sc, rng, r, mode ? Dt::kIrregular : Dt::k60, 24.0f, mode ? "events irregular" : "events 60fps",
             true, &sheets);
      }
  // Never activated, and deactivated before the first update.
  for (int k = 0; k < 2; ++k) {
    Fx fx(demo_pair());
    if (k) fx.deactivate();
    gm::Rng rng(2600u + static_cast<uint32_t>(k));
    play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 21.0f, k ? "deactivated" : "never activated");
  }
}

TEST(sprite_set_sheet) {
  Expect ex;
  diff::Rand r(4107);
  std::unique_ptr<Sheet> big = make_big();
  Sheet one(5, 3, 1), none(16, 16, 0), nobits(16, 16, 12), wide(GM_W + 30, GM_H + 10, 3), odd(9, 13, 7);
  one.fill(r, 0.6f);
  none.finish();
  nobits.finish(true);
  wide.fill(r, 0.02f);
  odd.fill(r, 0.5f);
  const Pair pairs[] = {demo_pair(),           sheet_pair(*big, "big"),      sheet_pair(one, "one"),
                        sheet_pair(none, "none"), sheet_pair(nobits, "nobits"), sheet_pair(wide, "wide"),
                        sheet_pair(odd, "odd")};
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  int stale = 0;
  for (const Pair& from : pairs)
    for (const Pair& to : pairs)
      for (int k = 0; k < 2; ++k) {
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s -> %s mapping %d", from.tag, to.tag, k);
        std::unique_ptr<Fx> fx(k ? new Fx(from, lively_mapping()) : new Fx(from));
        fx->activate();
        gm::Rng rng(4400u + static_cast<uint32_t>(k));
        play(ex, *fx, drive, rng, r, Dt::k60, r.frange(1.0f, 6.0f), tag);
        fx->set_sheet(to);
        EXPECT(ex, same_sheet(fx->c->sheet, *to.p));
        EXPECT(ex, fx->same_state());
        // The frame index is kept until the next update (it may be past the new sheet's end).
        stale += gm_sprite_current_frame(fx->c.get()) >= to.p->frames;
        EXPECT(ex, fx->render_same(tag));
        play(ex, *fx, drive, rng, r, Dt::kIrregular, 4.0f, tag);
      }
  harness::note("swaps leaving the frame index past the end: %d", stale);
  CHECK(stale > 10);
}

TEST(sprite_tunables) {
  Expect ex;
  diff::Rand r(4108);
  std::unique_ptr<Sheet> big = make_big();
  gm::SpriteEffect proto(gm::demoSprite());
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  const gm::Scenario& bump = *gm::Scenario::find("bump");
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < proto.params().count(); ++i) {
      const gm::Param& prm = proto.params().at(i);
      const float values[] = {prm.min, prm.max, 0.5f * (prm.min + prm.max)};
      for (float v : values) {
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s %s=%g", k ? "big" : "demo", prm.name, static_cast<double>(v));
        std::unique_ptr<Fx> fx(k ? new Fx(sheet_pair(*big, "big"), lively_mapping()) : new Fx(demo_pair()));
        EXPECT(ex, fx->set(prm.name, v));
        fx->activate();
        EXPECT(ex, same_param_tables(gm_effect_params(fx->ce()), fx->p->params()));
        gm::Rng rng(3000u + static_cast<uint32_t>(i));
        play(ex, *fx, drive, rng, r, Dt::k60, 22.0f, tag);
        play(ex, *fx, bump, rng, r, Dt::kIrregular, 6.0f, tag, true);
        hard(ex, *fx, 600, tag);
        // Change it again mid-run.
        EXPECT(ex, fx->set(prm.name, prm.max + prm.min - v));
        play(ex, *fx, drive, rng, r, Dt::k60, 4.0f, tag);
      }
    }
}

TEST(sprite_nan_inf_g) {
  Expect ex;
  diff::Rand r(4109);
  std::unique_ptr<Sheet> big = make_big();
  // The sprite stays finite because its target is clamped NaN-safely; the
  // playhead can become NaN (it then shows frame 0, as the x86 C++ does)
  // until a reset.
  const float bad[] = {diff::kNaN, diff::kInf, -diff::kInf, 1e30f, -1e30f, 3e38f, -3e38f, 1e6f, -1e6f, 1e-30f};
  int nan_playhead = 0;
  for (int run = 0; run < 48; ++run) {
    std::unique_ptr<Fx> fx(run % 3 == 2 ? new Fx(sheet_pair(*big, "big"), lively_mapping()) : new Fx(demo_pair()));
    if (run % 4 == 1) EXPECT(ex, fx->set("inertia", 0.0f));
    if (run % 5 == 3) EXPECT(ex, fx->set("idle_fps", 30.0f));
    if (run % 7 == 4) EXPECT(ex, fx->set("scrub_axis", 1.0f));
    fx->activate();
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
      const gm_vec3_t pos = gm_sprite_position(fx->c.get());
      EXPECT(ex, std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z));
      if (std::isnan(fx->c->playhead)) ++nan_playhead;
      if (r.chance(0.01)) fx->reset();
      std::snprintf(label, sizeof(label), "bad g run %d frame %d", run, frame);
      if (!EXPECT(ex, fx->render_same(label))) break;
    }
  }
  harness::note("updates with a NaN playhead: %d", nan_playhead);
  CHECK(nan_playhead > 1000);
}

TEST(sprite_long_run) {
  Expect ex;
  diff::Rand r(4110);
  std::unique_ptr<Sheet> big = make_big();
  // Fast playback with long steps: the playhead passes 1e6 frames again and
  // again and is wrapped.
  {
    Fx fx(sheet_pair(*big, "big"), lively_mapping());
    EXPECT(ex, fx.set("idle_fps", 60.0f));
    EXPECT(ex, fx.set("fps_g", 120.0f));
    fx.activate();
    char label[128];
    float t = 0.0f;
    for (int frame = 0; frame < 40000; ++frame) {
      const float dt = frame % 5 ? 1.0f : odd_dt(r);
      t += dt;
      const float s = std::sin(t * 0.37f), q = std::cos(t * 0.21f);
      if (!EXPECT(ex, fx.update(dyn_g(2.5f * s, 0.3f * q, 2.0f * q), dt))) {
        fx.note_state("long fast");
        break;
      }
      std::snprintf(label, sizeof(label), "long fast frame %d", frame);
      if (!EXPECT(ex, fx.render_same(label))) break;
    }
    harness::note("playhead wrapped %d times", fx.wrapped);
    CHECK(fx.wrapped >= 5);
  }
  // 10 minutes at 60 fps ...
  {
    Fx fx(demo_pair());
    EXPECT(ex, fx.set("idle_fps", 12.0f));
    fx.activate();
    gm::Rng rng(7000u);
    const int frames = play(ex, fx, *gm::Scenario::find("drive"), rng, r, Dt::k60, 600.0f, "long 60fps");
    EXPECT(ex, frames >= 600 * 60);
  }
  // ... and an hour at the engine's slowest step.
  {
    Fx fx(sheet_pair(*big, "big"), lively_mapping());
    EXPECT(ex, fx.set("hz", 8.0f));
    fx.activate();
    gm::Rng rng(7001u);
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

TEST(sprite_fuzz) {
  Expect ex;
  diff::Rand r(4111);
  gm::SpriteEffect proto(gm::demoSprite());
  for (int run = 0; run < 160; ++run) {
    const bool large = r.chance(0.1);
    Sheet s(r.irange(large ? 0 : 1, large ? GM_W + 30 : 40), r.irange(large ? 0 : 1, large ? GM_H + 20 : 30),
            r.irange(r.chance(0.05) ? 0 : 1, 40));
    s.fill(r, r.frange(0.0f, 0.6f));
    if (r.chance(0.03)) s.finish(true);
    gm::SpriteMapping m;
    m.pxPerG = gm::Vec3{r.frange(0.0f, 200.0f), r.frange(0.0f, 100.0f), r.frange(0.0f, 200.0f)};
    m.smoothingHz = r.frange(0.3f, 8.0f);
    m.scrubAxis = r.irange(-2, 3);
    m.framesPerG = r.chance(0.3) ? 0.0f : r.frange(-64.0f, 64.0f);
    m.idleFps = r.chance(0.5) ? 0.0f : r.frange(0.0f, 60.0f);
    m.fpsPerG = r.frange(0.0f, 120.0f);
    m.depthEcho = r.chance(0.3) ? 0.0f : r.frange(-1.0f, 6.0f);
    m.followInertia = r.chance(0.7);
    std::unique_ptr<Fx> fx(r.chance(0.2) ? new Fx(sheet_pair(s, "fuzz")) : new Fx(sheet_pair(s, "fuzz"), m));
    EXPECT(ex, same_param_tables(gm_effect_params(fx->ce()), fx->p->params()));
    fx->activate();
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
      for (int u = 0; u < updates; ++u) ok = fx->update(dyn_g(d.x, d.y, d.z), odd_dt(r)) && ok;
      if (!EXPECT(ex, ok)) {
        fx->note_state("fuzz");
        break;
      }
      if (r.chance(0.01)) {
        const gm::Param& prm = proto.params().at(r.irange(0, proto.params().count() - 1));
        EXPECT(ex, fx->set(prm.name, r.frange(prm.min - 1.0f, prm.max + 1.0f)));
      }
      if (r.chance(0.005)) fx->reset();
      if (r.chance(0.005)) {
        fx->deactivate();
        fx->activate();
      }
      std::snprintf(label, sizeof(label), "fuzz run %d frame %d", run, frame);
      if (!EXPECT(ex, fx->render_same(label))) break;
    }
  }
}

TEST(sprite_odd_mappings) {
  Expect ex;
  diff::Rand r(4112);
  std::unique_ptr<Sheet> big = make_big();
  // A mapping is not clamped (only values set through the table are):
  // non-finite and huge values, field by field. A NaN, infinite or
  // out-of-int-range echo makes the C++ negate INT_MIN (undefined; an x86 build
  // draws nothing, and so does the C), so those runs are skipped in the
  // sanitizer build.
#if defined(__SANITIZE_ADDRESS__)
  const bool sanitized = true;
#else
  const bool sanitized = false;
#endif
  const float odd[] = {diff::kNaN, diff::kInf, -diff::kInf, 3e9f, -3e9f, 1e30f, -1e30f, 1e5f, -1e5f, -0.0f};
  const gm::Scenario& drive = *gm::Scenario::find("drive");
  int runs = 0;
  for (int field = 0; field < 8; ++field)
    for (float v : odd)
      for (int k = 0; k < 2; ++k) {
        const bool int_min_echo = field == 7 && !(v >= -2147483648.0f && v < 2147483648.0f);
        if (sanitized && int_min_echo) continue;
        gm::SpriteMapping m = k ? lively_mapping() : gm::SpriteMapping{};
        float* fields[] = {&m.pxPerG.x, &m.pxPerG.y, &m.pxPerG.z, &m.smoothingHz,
                           &m.framesPerG, &m.idleFps, &m.fpsPerG, &m.depthEcho};
        *fields[field] = v;
        char tag[96];
        std::snprintf(tag, sizeof(tag), "%s mapping field %d = %g", k ? "big" : "demo", field, static_cast<double>(v));
        Fx fx(k ? sheet_pair(*big, "big") : demo_pair(), m);
        EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
        fx.activate();
        gm::Rng rng(4600u + static_cast<uint32_t>(field));
        play(ex, fx, drive, rng, r, Dt::k60, 8.0f, tag);
        play(ex, fx, drive, rng, r, Dt::kIrregular, 4.0f, tag, true);
        ++runs;
      }
  // The scrub axis is an int in the mapping: also its extremes.
  const int axes[] = {INT32_MIN, -1000, 1000, INT32_MAX};
  for (int axis : axes) {
    gm::SpriteMapping m;
    m.scrubAxis = axis;
    Fx fx(demo_pair(), m);
    EXPECT(ex, same_param_tables(gm_effect_params(fx.ce()), fx.p->params()));
    fx.activate();
    gm::Rng rng(4700u);
    play(ex, fx, drive, rng, r, Dt::k60, 6.0f, "scrub axis extreme");
    ++runs;
  }
  harness::note("%d runs", runs);
  CHECK(runs == (sanitized ? 2 * 7 * 10 + 2 * 3 + 4 : 2 * 8 * 10 + 4));
}
