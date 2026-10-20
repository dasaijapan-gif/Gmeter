// Differential tests of the engine (gm_engine.c vs gm::Engine): effect
// switching with and without crossfade, auto-cycle, the mode name, readout and
// status overlays, set_param and the burn-in offset rules.
//
// The same small test effect is written twice, once as a gm::Effect subclass
// and once as a C gm_effect_t with a vtable. Its picture depends on g, time
// and its parameters, and its burn-in offset can be non-zero. It comes in
// three kinds:
//   full     overrides every hook (activate, deactivate, reset, burn-in offset)
//   plain    reset only; activate falls back to reset, no burn-in offset
//   minimal  only update and render (every optional C vtable slot is NULL)
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "diff_util.hpp"

using diff::Expect;
using diff::same;

namespace {

enum Kind { kFull = 0, kPlain = 1, kMinimal = 2 };

// ---- C++ version ------------------------------------------------------------------
class CppFx : public gm::Effect {
 public:
  float gain = 1.0f, speed = 1.0f, ox = 0.0f, oy = 0.0f, density = 0.5f;
  gm::Vec3 pos;
  float phase = 0.0f, t = 0.0f;
  uint32_t frames = 0;
  int activations = 0, deactivations = 0, resets = 0, updates = 0, renders = 0;

  CppFx(const char* name, Kind kind) : name_(name), kind_(kind) {
    params_.add("gain", &gain, 0.0f, 4.0f, "position gain");
    params_.add("speed", &speed, 0.0f, 10.0f);
    params_.add("ox", &ox, -6.0f, 6.0f, "burn-in x");
    params_.add("oy", &oy, -6.0f, 6.0f, "burn-in y");
    params_.add("density", &density, 0.0f, 1.0f);
  }
  const char* name() const override { return name_; }
  void update(const gm::EffectContext& ctx) override {
    ++updates;
    phase += ctx.dt * speed;
    if (phase > 1.0f) phase -= 1.0f;
    const gm::Vec3 target = ctx.g.inertial() * (gain * 30.0f);
    pos = gm::lerp(pos, target, 0.15f);
    t = ctx.time;
    frames = ctx.frame;
  }
  void render(gm::VoxelFrame& f) override {
    ++renders;
    f.sphere(pos, 7.0f, 1.2f * gm::kLayerPitchPx, density);
    f.line3(gm::Vec3{}, pos);
    const int k = static_cast<int>(phase * 8.0f);
    f.fillRect(k % gm::kD, 5, 5, 5 + static_cast<int>(phase * 40.0f), 20, 0.5f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(frames % 1000u));
    f.text(gm::kD - 1, 60, gm::kH - 12, buf);
    f.circle(static_cast<int>((frames / 7u) % static_cast<uint32_t>(gm::kD)), gm::kW / 2 + static_cast<int>(pos.x),
             gm::kH / 2, 10 + static_cast<int>(phase * 10.0f));
  }

 protected:
  const char* name_;
  Kind kind_;
};

class CppFull : public CppFx {
 public:
  using CppFx::CppFx;
  void activate() override {
    ++activations;
    phase = 0.25f;
  }
  void deactivate() override {
    ++deactivations;
    pos = pos * 0.5f;
  }
  void reset() override {
    ++resets;
    pos = gm::Vec3{};
    phase = 0.0f;
  }
  void burnInOffset(int& dx, int& dy) const override {
    dx = static_cast<int>(ox) + (static_cast<int>(t * speed) & 1);
    dy = static_cast<int>(oy);
  }
};

class CppPlain : public CppFx {
 public:
  using CppFx::CppFx;
  void reset() override {
    ++resets;
    pos = gm::Vec3{};
    phase = 0.0f;
  }
};

class CppMinimal : public CppFx {
 public:
  using CppFx::CppFx;
};

// ---- C version ------------------------------------------------------------------------
struct CFx {
  gm_effect_t base;
  const char* name;
  float gain, speed, ox, oy, density;
  gm_vec3_t pos;
  float phase, t;
  uint32_t frames;
  int activations, deactivations, resets, updates, renders;
};

void cfx_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  CFx* s = reinterpret_cast<CFx*>(self);
  ++s->updates;
  s->phase += ctx->dt * s->speed;
  if (s->phase > 1.0f) s->phase -= 1.0f;
  const gm_vec3_t target = gm_vec3_scale(gm_gforce_inertial(ctx->g), s->gain * 30.0f);
  s->pos = gm_vec3_lerp(s->pos, target, 0.15f);
  s->t = ctx->time;
  s->frames = ctx->frame;
}

void cfx_render(gm_effect_t* self, gm_frame_t* f) {
  CFx* s = reinterpret_cast<CFx*>(self);
  ++s->renders;
  gm_frame_sphere(f, s->pos, 7.0f, 1.2f * GM_LAYER_PITCH_PX, s->density);
  gm_frame_line3(f, gm_vec3_zero(), s->pos, 1.0f);
  const int k = static_cast<int>(s->phase * 8.0f);
  gm_frame_fill_rect(f, k % GM_D, 5, 5, 5 + static_cast<int>(s->phase * 40.0f), 20, 0.5f);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(s->frames % 1000u));
  gm_frame_text(f, GM_D - 1, 60, GM_H - 12, buf);
  gm_frame_circle(f, static_cast<int>((s->frames / 7u) % static_cast<uint32_t>(GM_D)), GM_W / 2 + static_cast<int>(s->pos.x),
                  GM_H / 2, 10 + static_cast<int>(s->phase * 10.0f));
}

void cfx_activate(gm_effect_t* self) {
  CFx* s = reinterpret_cast<CFx*>(self);
  ++s->activations;
  s->phase = 0.25f;
}

void cfx_deactivate(gm_effect_t* self) {
  CFx* s = reinterpret_cast<CFx*>(self);
  ++s->deactivations;
  s->pos = gm_vec3_scale(s->pos, 0.5f);
}

void cfx_reset(gm_effect_t* self) {
  CFx* s = reinterpret_cast<CFx*>(self);
  ++s->resets;
  s->pos = gm_vec3_zero();
  s->phase = 0.0f;
}

void cfx_burn_in(const gm_effect_t* self, int* dx, int* dy) {
  const CFx* s = reinterpret_cast<const CFx*>(self);
  *dx = static_cast<int>(s->ox) + (static_cast<int>(s->t * s->speed) & 1);
  *dy = static_cast<int>(s->oy);
}

// The vtable holds the name, so each instance gets its own.
struct CFxBox {
  gm_effect_vtbl_t vt;
  CFx fx;
};

std::unique_ptr<CFxBox> make_c(const char* name, Kind kind) {
  std::unique_ptr<CFxBox> b(new CFxBox());
  b->vt.name = name;
  b->vt.update = cfx_update;
  b->vt.render = cfx_render;
  b->vt.activate = kind == kFull ? cfx_activate : nullptr;
  b->vt.deactivate = kind == kFull ? cfx_deactivate : nullptr;
  b->vt.reset = kind == kMinimal ? nullptr : cfx_reset;
  b->vt.burn_in_offset = kind == kFull ? cfx_burn_in : nullptr;
  CFx& s = b->fx;
  s.base.vt = &b->vt;
  s.name = name;
  s.gain = 1.0f;
  s.speed = 1.0f;
  s.ox = 0.0f;
  s.oy = 0.0f;
  s.density = 0.5f;
  s.pos = gm_vec3_zero();
  s.phase = 0.0f;
  s.t = 0.0f;
  s.frames = 0;
  s.activations = s.deactivations = s.resets = s.updates = s.renders = 0;
  gm_params_init(&s.base.params);
  gm_params_add(&s.base.params, "gain", &s.gain, 0.0f, 4.0f, "position gain");
  gm_params_add(&s.base.params, "speed", &s.speed, 0.0f, 10.0f, "");
  gm_params_add(&s.base.params, "ox", &s.ox, -6.0f, 6.0f, "burn-in x");
  gm_params_add(&s.base.params, "oy", &s.oy, -6.0f, 6.0f, "burn-in y");
  gm_params_add(&s.base.params, "density", &s.density, 0.0f, 1.0f, "");
  return b;
}

std::unique_ptr<CppFx> make_cpp(const char* name, Kind kind) {
  switch (kind) {
    case kFull: return std::unique_ptr<CppFx>(new CppFull(name, kind));
    case kPlain: return std::unique_ptr<CppFx>(new CppPlain(name, kind));
    default: return std::unique_ptr<CppFx>(new CppMinimal(name, kind));
  }
}

bool same_fx(const CFx& c, const CppFx& p) {
  return same(c.pos, p.pos) && same(c.phase, p.phase) && same(c.t, p.t) && c.frames == p.frames &&
         c.activations == p.activations && c.deactivations == p.deactivations && c.resets == p.resets &&
         c.updates == p.updates && c.renders == p.renders && same(c.gain, p.gain) && same(c.speed, p.speed) &&
         same(c.ox, p.ox) && same(c.oy, p.oy) && same(c.density, p.density);
}

// Long names exercise the 19-character limit of the mode-name overlay; the
// duplicate checks that lookups find the first match.
const char* const kNames[] = {"ball",  "slosh", "Smoke2", "averyveryverylongeffectname", "a",  "hud",
                              "sync",  "tunnel", "ball",  "x_y-z.9",                     "AT", "particles",
                              "anim", "sprite"};
const int kPool = 14;

struct World {
  std::vector<std::unique_ptr<CFxBox>> c;
  std::vector<std::unique_ptr<CppFx>> p;
  std::unique_ptr<gm_engine_t> ce{new gm_engine_t};
  std::unique_ptr<gm::Engine> pe{new gm::Engine};
  explicit World(int kind_offset) {
    gm_engine_init(ce.get());
    for (int i = 0; i < kPool; ++i) {
      const Kind kind = static_cast<Kind>((i + kind_offset) % 3);
      c.push_back(make_c(kNames[i], kind));
      p.push_back(make_cpp(kNames[i], kind));
    }
  }
  int index_c(const gm_effect_t* e) const {
    for (int i = 0; i < kPool; ++i)
      if (&c[static_cast<size_t>(i)]->fx.base == e) return i;
    return -1;
  }
  int index_p(const gm::Effect* e) const {
    for (int i = 0; i < kPool; ++i)
      if (p[static_cast<size_t>(i)].get() == e) return i;
    return -1;
  }
  // Every observable output of both engines and every effect.
  void compare(Expect& ex, const char* label) {
    EXPECT(ex, harness::same_frame(gm_engine_frame(ce.get()), pe->frame(), label));
    EXPECT(ex, same(gm_engine_time(ce.get()), pe->time()));
    EXPECT(ex, gm_engine_frame_count(ce.get()) == pe->frameCount());
    EXPECT(ex, gm_engine_effect_count(ce.get()) == pe->effectCount());
    EXPECT(ex, gm_engine_current_index(ce.get()) == pe->currentIndex());
    EXPECT(ex, index_c(gm_engine_current(ce.get())) == index_p(pe->current()));
    for (int i = -1; i <= gm_engine_effect_count(ce.get()); ++i)
      EXPECT(ex, index_c(gm_engine_effect(ce.get(), i)) == index_p(pe->effect(i)));
    for (const char* n : kNames) EXPECT(ex, gm_engine_find_effect(ce.get(), n) == pe->findEffect(n));
    EXPECT(ex, gm_engine_find_effect(ce.get(), "missing") == pe->findEffect("missing"));
    for (int i = 0; i < kPool; ++i) EXPECT(ex, same_fx(c[static_cast<size_t>(i)]->fx, *p[static_cast<size_t>(i)]));
  }
};

}  // namespace

TEST(engine_empty) {
  Expect ex;
  World w(0);
  for (int i = 0; i < 20; ++i) {
    gm_engine_step(w.ce.get(), gm_gforce_rest(), 1.0f / 60.0f);
    w.pe->step(gm::GForce{}, 1.0f / 60.0f);
    w.compare(ex, "empty");
  }
  gm_engine_next_effect(w.ce.get());
  w.pe->nextEffect();
  gm_engine_set_effect(w.ce.get(), 0, true);
  w.pe->setEffect(0);
  EXPECT(ex, gm_engine_set_effect_by_name(w.ce.get(), "ball", true) == w.pe->setEffect("ball"));
  EXPECT(ex, gm_engine_set_param(w.ce.get(), "gain", 2.0f) == w.pe->setParam("gain", 2.0f));
  EXPECT(ex, gm_engine_add_effect(w.ce.get(), nullptr) == w.pe->addEffect(nullptr));
  w.compare(ex, "empty ops");
}

TEST(engine_defaults_single_effect) {
  Expect ex;
  for (int kind = 0; kind < 3; ++kind) {
    World w(kind);
    EXPECT(ex, gm_engine_add_effect(w.ce.get(), &w.c[0]->fx.base) == w.pe->addEffect(w.p[0].get()));
    w.compare(ex, "added");
    gm::GForce g;
    for (int i = 0; i < 400; ++i) {
      g.proper = gm::Vec3{0.6f * std::sin(static_cast<float>(i) * 0.05f), 1.0f, -0.4f * std::cos(i * 0.03f)};
      gm_engine_step(w.ce.get(), gm_gforce_from_proper(diff::to_c(g.proper)), 1.0f / 60.0f);
      w.pe->step(g, 1.0f / 60.0f);
      char label[32];
      std::snprintf(label, sizeof(label), "kind %d step %d", kind, i);
      w.compare(ex, label);
    }
  }
}

TEST(engine_random_operation_sequences) {
  Expect ex;
  diff::Rand r(81);
  const char* labels[] = {nullptr, "", "DEMO", "SIM", "LONGLABELTEXTXX", "x"};
  const char* params[] = {"gain", "speed", "ox", "oy", "density", "nope", ""};
  const float autos[] = {0.0f, 0.3f, 1.0f, 2.5f, -1.0f};
  const float fades[] = {0.4f, 0.0f, 0.05f, 1.0f, -0.5f};
  const float odd_dt[] = {diff::kNaN, 0.0f, -1.0f, 1e-6f, 1.0f, diff::kInf, 1.0f / 15.0f, 1.0f / 1000.0f};
  // How often each interesting path was reached, so the sequences provably cover them.
  int n_fade = 0, n_fade_offsets = 0, n_offset = 0, n_auto = 0, n_name = 0, n_readout = 0, n_label = 0;
  for (int run = 0; run < 60; ++run) {
    World w(run);
    gm_vec3_t dyn = gm_vec3_zero(), target = gm_vec3_zero();
    const int n_ops = 700;
    for (int op = 0; op < n_ops; ++op) {
      const int k = r.irange(0, 99);
      const char* what = "step";
      if (k < 6 || (op < 3 && k < 50)) {
        what = "add";
        const int i = r.irange(0, kPool - 1);
        if (r.chance(0.05)) {
          EXPECT(ex, gm_engine_add_effect(w.ce.get(), nullptr) == w.pe->addEffect(nullptr));
        } else {
          EXPECT(ex, gm_engine_add_effect(w.ce.get(), &w.c[static_cast<size_t>(i)]->fx.base) ==
                         w.pe->addEffect(w.p[static_cast<size_t>(i)].get()));
        }
      } else if (k < 12) {
        what = "set_effect";
        const int i = r.irange(-2, gm_engine_effect_count(w.ce.get()) + 1);
        const bool fade = r.chance(0.6);
        if (fade && r.chance(0.3)) {
          gm_engine_set_effect(w.ce.get(), i, true);
          w.pe->setEffect(i);  // C++ default crossfade = true
        } else {
          gm_engine_set_effect(w.ce.get(), i, fade);
          w.pe->setEffect(i, fade);
        }
      } else if (k < 16) {
        what = "set_effect_by_name";
        const char* n = r.chance(0.9) ? kNames[r.irange(0, kPool - 1)] : "missing";
        const bool fade = r.chance(0.5);
        EXPECT(ex, gm_engine_set_effect_by_name(w.ce.get(), n, fade) == w.pe->setEffect(n, fade));
      } else if (k < 19) {
        what = "next";
        gm_engine_next_effect(w.ce.get());
        w.pe->nextEffect();
      } else if (k < 21) {
        what = "auto_cycle";
        const float a = r.pick(autos);
        gm_engine_set_auto_cycle(w.ce.get(), a);
        w.pe->setAutoCycle(a);
      } else if (k < 23) {
        what = "crossfade_seconds";
        const float s = r.pick(fades);
        gm_engine_set_crossfade_seconds(w.ce.get(), s);
        w.pe->setCrossfadeSeconds(s);
      } else if (k < 25) {
        what = "show_mode_name";
        const bool on = r.chance(0.6);
        gm_engine_set_show_mode_name(w.ce.get(), on);
        w.pe->setShowModeName(on);
      } else if (k < 27) {
        what = "readout";
        const bool on = r.chance(0.5);
        gm_engine_set_readout(w.ce.get(), on);
        w.pe->setReadout(on);
      } else if (k < 29) {
        what = "status_label";
        const char* l = r.pick(labels);
        gm_engine_set_status_label(w.ce.get(), l);
        w.pe->setStatusLabel(l);
      } else if (k < 35) {
        what = "set_param";
        const char* n = r.pick(params);
        static const float kOdd[] = {diff::kNaN, diff::kInf, -diff::kInf, 100.0f, -100.0f, 0.0f};
        const float v = r.chance(0.2) ? r.pick(kOdd) : r.frange(-7.0f, 7.0f);
        EXPECT(ex, gm_engine_set_param(w.ce.get(), n, v) == w.pe->setParam(n, v));
      } else {
        if (r.chance(0.03))
          target = gm_vec3(r.frange(-1.5f, 1.5f), r.frange(-1.0f, 1.0f), r.frange(-1.5f, 1.5f));
        dyn = gm_vec3_lerp(dyn, target, 0.1f);
        const gm_gforce_t g = gm_gforce_from_proper(gm_vec3_add(gm_gforce_rest_proper(), dyn));
        gm::GForce pg;
        pg.proper = diff::to_cpp(g.proper);
        const float dt = r.chance(0.05) ? r.pick(odd_dt) : r.frange(0.005f, 0.05f);
        const gm_engine_t* e = w.ce.get();
        const int before = e->current;
        gm_engine_step(w.ce.get(), g, dt);
        w.pe->step(pg, dt);
        if (e->current != before && before >= 0) ++n_auto;
        if (e->current >= 0) {
          int dx = 0, dy = 0;
          gm_effect_burn_in_offset(gm_engine_current(e), &dx, &dy);
          if (dx || dy) ++n_offset;
          if (e->outgoing >= 0) {
            ++n_fade;
            int odx = 0, ody = 0;
            gm_effect_burn_in_offset(e->effects[e->outgoing], &odx, &ody);
            if (odx != dx || ody != dy) ++n_fade_offsets;
          }
          if (e->show_mode_name && e->since_switch < 1.5f) ++n_name;
          if (e->readout) ++n_readout;
          if (e->status_label && *e->status_label) ++n_label;
        }
      }
      char label[64];
      std::snprintf(label, sizeof(label), "run %d op %d (%s)", run, op, what);
      w.compare(ex, label);
    }
  }
  harness::note("steps: %d crossfading (%d with differing offsets), %d offset, %d auto-cycle switches, "
                "%d mode name, %d readout, %d status label",
                n_fade, n_fade_offsets, n_offset, n_auto, n_name, n_readout, n_label);
  CHECK(n_fade > 100 && n_fade_offsets > 20 && n_offset > 100 && n_auto > 5 && n_name > 100 && n_readout > 100 &&
        n_label > 100);
}

TEST(engine_crossfade_with_offsets) {
  Expect ex;
  // Two full effects with different burn-in offsets dissolving into each
  // other, every frame of every crossfade compared, for many offset pairs.
  for (int ox = -3; ox <= 3; ++ox)
    for (int oy = -2; oy <= 2; ++oy) {
      World w(0);  // index 0 and 3 are "full"
      gm_engine_add_effect(w.ce.get(), &w.c[0]->fx.base);
      w.pe->addEffect(w.p[0].get());
      gm_engine_add_effect(w.ce.get(), &w.c[3]->fx.base);
      w.pe->addEffect(w.p[3].get());
      gm_engine_set_readout(w.ce.get(), true);
      w.pe->setReadout(true);
      gm_engine_set_status_label(w.ce.get(), "DEMO");
      w.pe->setStatusLabel("DEMO");
      gm_params_set(&w.c[3]->fx.base.params, "ox", static_cast<float>(ox));
      w.p[3]->params().set("ox", static_cast<float>(ox));
      gm_params_set(&w.c[3]->fx.base.params, "oy", static_cast<float>(oy));
      w.p[3]->params().set("oy", static_cast<float>(oy));
      gm::GForce g;
      for (int i = 0; i < 240; ++i) {
        if (i % 40 == 0) {
          gm_engine_next_effect(w.ce.get());
          w.pe->nextEffect();
        }
        g.proper = gm::Vec3{0.5f * std::sin(i * 0.07f), 1.0f + 0.1f * std::sin(i * 0.3f), 0.7f * std::cos(i * 0.05f)};
        gm_engine_step(w.ce.get(), gm_gforce_from_proper(diff::to_c(g.proper)), 1.0f / 50.0f);
        w.pe->step(g, 1.0f / 50.0f);
        char label[48];
        std::snprintf(label, sizeof(label), "offset %d,%d step %d", ox, oy, i);
        w.compare(ex, label);
      }
    }
}
