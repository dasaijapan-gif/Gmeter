// Properties of the core on its own, carried over from the assertions of the
// former differential tests that did not compare with another implementation:
// the effect contract (names, parameter tables, burn-in), smoke's grid
// lifecycle, the built-in artwork, the HUD's orbit, the registries.
//
// Core functions used here that the firmware (idf/main) does not call;
// a change that removes one must replace it here:
//   gm_params_init, gm_params_add, gm_params_find, gm_gball_position, gm_slosh_height,
//   gm_slosh_total_volume, gm_slosh_last_substeps, gm_particles_gravity_px,
//   gm_smoke_total_density, gm_smoke_density, gm_sprite_sheet_pixel, gm_anim_asset_pixel,
//   gm_anim_asset_empty, gm_sprite_current_frame, gm_jp_find_glyph, gm_gstate_* (struct
//   fields read directly); smoke's grid pointer s->d
#include <limits.h>

#include "fixtures/user_animation.h"
#include "test.h"

static gm_frame_t s_f;

typedef union {
  gm_gball_t ball;
  gm_slosh_t slosh;
  gm_particles_t particles;
  gm_smoke_t smoke;
  gm_tunnel_t tunnel;
  gm_sprite_t sprite;
  gm_anim_t anim;
  gm_hud_t hud;
  gm_atfield_t atfield;
  gm_sync_t sync;
} any_effect_t;

static any_effect_t s_fx;

#define FX_COUNT 10
static const char* const k_names[FX_COUNT] = {"ball",   "slosh", "particles", "smoke",   "tunnel",
                                              "sprite", "anim",  "hud",       "atfield", "sync"};

// A freshly initialised effect number i (app_main's order).
static gm_effect_t* fresh_effect(int i) {
  memset(&s_fx, 0, sizeof(s_fx));
  switch (i) {
    case 0: gm_gball_init(&s_fx.ball); break;
    case 1: gm_slosh_init(&s_fx.slosh); break;
    case 2: gm_particles_init(&s_fx.particles); break;
    case 3: gm_smoke_init(&s_fx.smoke); break;
    case 4: gm_tunnel_init(&s_fx.tunnel); break;
    case 5: gm_sprite_init(&s_fx.sprite, gm_sprite_demo_sheet(), NULL); break;
    case 6: gm_anim_init(&s_fx.anim, gm_anim_demo()); break;
    case 7: gm_hud_init(&s_fx.hud); break;
    case 8: gm_atfield_init(&s_fx.atfield); break;
    default: gm_sync_init(&s_fx.sync); break;
  }
  return &s_fx.ball.base;  // every effect starts with its gm_effect_t
}

TEST(effects_names_and_burn_in) {
  for (int i = 0; i < FX_COUNT; ++i) {
    gm_effect_t* e = fresh_effect(i);
    CHECK(strcmp(gm_effect_name(e), k_names[i]) == 0);
    CHECK(e->vt->update && e->vt->render && e->vt->reset);
    int dx = 7, dy = 7;
    gm_effect_burn_in_offset(e, &dx, &dy);
    CHECK(dx == 0 && dy == 0);  // only the HUD ever moves, and it starts at step 0
    gm_effect_deactivate(e);
  }
}

// Every parameter: sane range and default, a unique name; setting through the
// table clamps finite values to the range and refuses non-finite ones and
// unknown names.
TEST(effects_parameter_tables) {
  static const float probes[] = {-1e9f, -1.0f, 0.0f, 0.05f, 0.5f, 1.0f, 1.5f, 7.0f, 25.0f, 121.0f, 1e9f,
                                 INFINITY, -INFINITY, NAN};
  for (int i = 0; i < FX_COUNT; ++i) {
    gm_effect_t* e = fresh_effect(i);
    gm_param_table_t* t = gm_effect_params(e);
    CHECK(gm_params_count(t) > 0 && gm_params_count(t) <= GM_PARAMS_MAX);
    for (int k = 0; k < gm_params_count(t); ++k) {
      const gm_param_t* p = gm_params_at(t, k);
      CHECK(p->min <= p->max && *p->value >= p->min && *p->value <= p->max);
      CHECK(p->help != NULL);
      CHECK(gm_params_find(t, p->name) == p);  // names are unique
      for (size_t j = 0; j < sizeof(probes) / sizeof(probes[0]); ++j) {
        const float before = *p->value, v = probes[j];
        const bool ok = gm_params_set(t, p->name, v);
        if (isfinite(v))
          CHECK(ok && *p->value == gm_clampf(v, p->min, p->max));
        else
          CHECK(!ok && *p->value == before);
        float got = 0.0f;
        CHECK(gm_params_get(t, p->name, &got) && got == *p->value);
      }
    }
    float v = 123.0f;
    CHECK(!gm_params_set(t, "nope", 1.0f));
    CHECK(!gm_params_get(t, "nope", &v) && v == 123.0f);
    gm_effect_deactivate(e);
  }
}

// An effect can be drawn before its first update, as the engine may do.
TEST(effects_render_fresh) {
  for (int i = 0; i < FX_COUNT; ++i) {
    gm_effect_t* e = fresh_effect(i);
    gm_frame_clear(&s_f);
    gm_effect_render(e, &s_f);
    if (i == 4) CHECK(lit(&s_f) > 0);  // the tunnel is always drawn
    gm_effect_activate(e);
    gm_frame_clear(&s_f);
    gm_effect_render(e, &s_f);
    if (i == 3) CHECK(lit(&s_f) == 0);  // smoke starts empty
    if (i == 4) CHECK(lit(&s_f) > 0);
    gm_effect_deactivate(e);
  }
}

TEST(effects_initial_state) {
  gm_effect_t* e = fresh_effect(0);
  const gm_vec3_t b = gm_gball_position(&s_fx.ball);
  CHECK(b.x == 0.0f && b.y == 0.0f && b.z == 0.0f);
  e = fresh_effect(1);
  // A level, still tank at the rest depth.
  const float h0 = 0.42f * (float)GM_H;
  bool level = true;
  for (int z = 0; z < GM_SLOSH_NZ; ++z)
    for (int x = 0; x < GM_SLOSH_NX; ++x) level = level && gm_slosh_height(&s_fx.slosh, x, z) == h0;
  CHECK(level);
  CHECK(gm_slosh_last_substeps(&s_fx.slosh) == 0);
  const float v0 = h0 * GM_SLOSH_NX * GM_SLOSH_NZ;
  CHECK_NEAR(gm_slosh_total_volume(&s_fx.slosh), v0, v0 * 1e-5f);
  e = fresh_effect(2);
  // 1 g in px/s^2 for the default 1 m virtual tank across 128 px.
  CHECK(gm_particles_gravity_px(&s_fx.particles) == GM_STANDARD_GRAVITY * 128.0f);
  e = fresh_effect(3);
  CHECK(!gm_smoke_allocated(&s_fx.smoke) && gm_smoke_total_density(&s_fx.smoke) == 0.0f);
  gm_effect_deactivate(e);
}

// Activate allocates zeroed grids, deactivate frees them, a second activate
// keeps the grids and clears them, an update while deactivated (the engine
// keeps updating an effect that fades out) allocates again.
TEST(smoke_grid_lifecycle) {
  gm_smoke_t* s = &s_fx.smoke;
  fresh_effect(3);
  CHECK(!gm_smoke_allocated(s));
  CHECK(gm_smoke_total_density(s) == 0.0f && gm_smoke_density(s, 3, 3, 3) == 0.0f);
  gm_effect_activate(&s->base);
  CHECK(gm_smoke_allocated(s));
  CHECK(gm_smoke_total_density(s) == 0.0f);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  for (int i = 0; i < 180; ++i) gm_effect_update(&s->base, &ctx);
  CHECK(gm_smoke_total_density(s) > 0.0f);
  const float* d_before = s->d;
  gm_effect_activate(&s->base);  // already allocated: same grids, cleared
  CHECK(s->d == d_before);
  CHECK(gm_smoke_total_density(s) == 0.0f);
  gm_effect_deactivate(&s->base);
  CHECK(!gm_smoke_allocated(s));
  CHECK(gm_smoke_total_density(s) == 0.0f && gm_smoke_density(s, 3, 3, 3) == 0.0f);
  gm_frame_clear(&s_f);
  gm_effect_render(&s->base, &s_f);
  CHECK(lit(&s_f) == 0);
  gm_effect_deactivate(&s->base);  // twice is harmless
  gm_effect_reset(&s->base);       // reset without grids does nothing
  CHECK(!gm_smoke_allocated(s));
  gm_effect_update(&s->base, &ctx);
  CHECK(gm_smoke_allocated(s));
  gm_effect_deactivate(&s->base);
  // Never activated: the first update allocates.
  fresh_effect(3);
  gm_effect_update(&s->base, &ctx);
  CHECK(gm_smoke_allocated(s));
  gm_effect_deactivate(&s->base);
}

TEST(sprite_demo_sheet) {
  const gm_sprite_sheet_t* d = gm_sprite_demo_sheet();
  CHECK(d != NULL && gm_sprite_demo_sheet() == d);  // built once
  CHECK(d->bits && d->width == 16 && d->height == 16 && d->frames == 12);
  CHECK(gm_sprite_sheet_row_bytes(d) == 2 && gm_sprite_sheet_frame_bytes(d) == 32);
  int n = 0;
  for (int fr = -1; fr <= d->frames; ++fr)
    for (int y = -2; y < d->height + 2; ++y)
      for (int x = -2; x < d->width + 2; ++x) n += gm_sprite_sheet_pixel(d, fr, x, y);
  CHECK(n > 300);
}

TEST(anim_demo_asset) {
  const gm_anim_asset_t* d = gm_anim_demo();
  CHECK(d != NULL && gm_anim_demo() == d);  // built once
  CHECK(d->bits && strcmp(d->name, "demo") == 0 && gm_anim_asset_valid(d));
  CHECK(gm_anim_asset_total_bytes(d) == 32 * 3 * 24 * 4);
  int n = 0;
  for (int fr = -1; fr <= d->frames; ++fr)
    for (int s = -1; s <= d->depth; ++s)
      for (int y = -2; y < d->height + 2; ++y)
        for (int x = -2; x < d->width + 2; ++x) n += gm_anim_asset_pixel(d, fr, s, x, y);
  CHECK(n > 1000);
}

TEST(anim_empty_asset_and_state_names) {
  const gm_anim_asset_t e = gm_anim_asset_empty();
  CHECK(e.name && e.name[0] == '\0' && e.bits == NULL && e.depth == 1 && e.frames == 0);
  CHECK(!gm_anim_asset_valid(&e) && !gm_anim_asset_pixel(&e, 0, 0, 0, 0));
  const gm_anim_mapping_t m = GM_ANIM_MAPPING_DEFAULT;
  CHECK(memcmp(&e.mapping, &m, sizeof(m)) == 0);
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s)
    CHECK(e.clips[s].start == 0 && e.clips[s].count == 0 && e.clips[s].fps == 12.0f && e.clips[s].loop == 1);
  static const char* const names[] = {"idle", "accelerate", "brake", "left", "right", "bump"};
  for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) CHECK(strcmp(gm_anim_state_name(s), names[s]) == 0);
  CHECK(strcmp(gm_anim_state_name(-1), "?") == 0 && strcmp(gm_anim_state_name(GM_ANIM_STATE_COUNT), "?") == 0);
}

TEST(anim_user_animation_fixture) {
  const gm_anim_asset_t* a = &kUserAnimation;
  CHECK(strcmp(a->name, "my_animation") == 0 && a->width == 32 && a->height == 24 && a->depth == 3 && a->frames == 33);
  CHECK(gm_anim_asset_valid(a) && a->bits == kUserAnimationBits);
  CHECK(gm_anim_asset_total_bytes(a) == (int)sizeof(kUserAnimationBits));
  CHECK(a->clips[GM_ANIM_BUMP].start == 26 && a->clips[GM_ANIM_BUMP].count == 6 && a->clips[GM_ANIM_BUMP].loop == 0);
  CHECK(a->mapping.enter_g == 0.25f && a->mapping.scrub_axis == -1.0f && a->mapping.mirror == 1.0f);
  static gm_anim_t fx;
  gm_anim_init(&fx, a);
  float v = 0.0f;
  CHECK(gm_params_get(gm_effect_params(&fx.base), "enter_g", &v) && v == 0.25f);
}

// gm_hud_set_orbit_step picks the start (step & 3), activation keeps it, and
// the steps go right, down, left, up.
TEST(hud_orbit_steps) {
  static const int kx[4] = {0, 1, 1, 0}, ky[4] = {0, 0, 1, 1};
  static const int steps[] = {0, 1, 2, 3, 4, 5, 7, 100, -1, -2, -3, -4, -5, INT_MAX, INT_MIN};
  gm_hud_t* h = &s_fx.hud;
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
    fresh_effect(7);
    gm_hud_set_orbit_step(h, steps[i]);
    int dx = -1, dy = -1;
    gm_effect_burn_in_offset(&h->base, &dx, &dy);
    CHECK(dx == kx[steps[i] & 3] && dy == ky[steps[i] & 3]);
    gm_effect_activate(&h->base);
    gm_effect_reset(&h->base);
    gm_effect_burn_in_offset(&h->base, &dx, &dy);
    CHECK(dx == kx[steps[i] & 3] && dy == ky[steps[i] & 3]);
  }
  // 0.01 min = 0.6 s per step: in order, about every 36 frames.
  fresh_effect(7);
  gm_effect_activate(&h->base);
  gm_params_set(gm_effect_params(&h->base), "orbit_min", 0.01f);
  gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  int step = 0, changes = 0;
  bool in_order = true;
  for (int i = 0; i < 60 * 30; ++i) {
    gm_effect_update(&h->base, &ctx);
    int dx = 0, dy = 0;
    gm_effect_burn_in_offset(&h->base, &dx, &dy);
    if (dx != kx[step] || dy != ky[step]) {
      step = (step + 1) & 3;
      ++changes;
      in_order = in_order && dx == kx[step] && dy == ky[step];
    }
  }
  CHECK(in_order && changes >= 45 && changes <= 55);
  // Off: never moves.
  gm_params_set(gm_effect_params(&h->base), "orbit_min", 0.0f);
  int moved = 0;
  for (int i = 0; i < 60 * 60; ++i) {
    gm_effect_update(&h->base, &ctx);
    int dx = 0, dy = 0;
    gm_effect_burn_in_offset(&h->base, &dx, &dy);
    moved += dx != 0 || dy != 0;
  }
  CHECK(moved == 0);
}

// The demo source plays these by name (`src demo <name>`); the last is "drive".
TEST(scenario_lookup) {
  const int n = gm_scenario_preset_count();
  CHECK(n == 7);
  CHECK(strcmp(gm_scenario_name(gm_scenario_preset(n - 1)), "drive") == 0);
  for (int i = 0; i < n; ++i) {
    const gm_scenario_t* s = gm_scenario_preset(i);
    CHECK(gm_scenario_find(gm_scenario_name(s)) == s);
    CHECK(gm_scenario_duration(s) > 4.0f);
  }
  CHECK(gm_scenario_preset(-1) == gm_scenario_preset(0) && gm_scenario_preset(n) == gm_scenario_preset(n - 1));
  CHECK(gm_scenario_find("nope") == NULL && gm_scenario_find("") == NULL && gm_scenario_find("Drive") == NULL);
}

TEST(params_registry) {
  static float v[GM_PARAMS_MAX + 4];
  gm_param_table_t t;
  gm_params_init(&t);
  CHECK(gm_params_count(&t) == 0);
  static const char* const names[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
                                      "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "aa", "bb"};
  for (int i = 0; i < GM_PARAMS_MAX + 4; ++i) gm_params_add(&t, names[i], &v[i], -1.0f, 1.0f, "");
  CHECK(gm_params_count(&t) == GM_PARAMS_MAX);  // the rest are ignored
  CHECK(gm_params_find(&t, names[GM_PARAMS_MAX]) == NULL);
  // Duplicate names: the first one wins.
  static float dup[3] = {1, 2, 3};
  gm_params_init(&t);
  for (int i = 0; i < 3; ++i) gm_params_add(&t, "dup", &dup[i], 0.0f, 10.0f, "");
  CHECK(gm_params_set(&t, "dup", 7.5f));
  CHECK(dup[0] == 7.5f && dup[1] == 2.0f && dup[2] == 3.0f);
  CHECK(gm_params_find(&t, "dup") == gm_params_at(&t, 0));
  CHECK(!gm_params_set(&t, "", 1.0f) && !gm_params_set(&t, "dupx", 1.0f));
}

TEST(engine_registry) {
  static gm_engine_t e;
  static gm_tunnel_t fx[GM_ENGINE_MAX_EFFECTS + 1];
  gm_engine_init(&e);
  CHECK(gm_engine_current(&e) == NULL && gm_engine_current_index(&e) == -1);
  CHECK(!gm_engine_set_param(&e, "hz", 1.0f));
  gm_engine_step(&e, gm_gforce_rest(), 1.0f / 60.0f);  // nothing to run: a blank frame
  CHECK(lit(gm_engine_frame(&e)) == 0);
  gm_engine_next_effect(&e);
  CHECK(!gm_engine_add_effect(&e, NULL));
  for (int i = 0; i <= GM_ENGINE_MAX_EFFECTS; ++i) {
    gm_tunnel_init(&fx[i]);
    CHECK(gm_engine_add_effect(&e, &fx[i].base) == (i < GM_ENGINE_MAX_EFFECTS));
  }
  CHECK(gm_engine_effect_count(&e) == GM_ENGINE_MAX_EFFECTS && gm_engine_current_index(&e) == 0);
  CHECK(gm_engine_find_effect(&e, "tunnel") == 0 && gm_engine_find_effect(&e, "missing") == -1);
  gm_engine_set_effect(&e, -1, true);  // out of range: ignored
  gm_engine_set_effect(&e, GM_ENGINE_MAX_EFFECTS, true);
  CHECK(gm_engine_current_index(&e) == 0);
}

TEST(gstate_defaults) {
  gm_gstate_t s;
  gm_gstate_init(&s);
  CHECK(gm_gstate_state(&s) == GM_DRIVE_IDLE);
  CHECK(s.enter_g == 0.25f && s.hold_s == 0.4f && s.bump_g == 0.3f);
  // The anim effect's states share the driving states' values.
  CHECK((int)GM_DRIVE_ACCELERATE == GM_ANIM_ACCELERATE && (int)GM_DRIVE_BRAKE == GM_ANIM_BRAKE &&
        (int)GM_DRIVE_LEFT == GM_ANIM_LEFT && (int)GM_DRIVE_RIGHT == GM_ANIM_RIGHT);
}

TEST(jp_glyph_tables) {
  const gm_jp_font_t* fonts[2] = {&gm_jp_small, &gm_jp_large};
  for (int k = 0; k < 2; ++k) {
    const gm_jp_font_t* f = fonts[k];
    for (int i = 0; i < f->count; ++i) {
      if (i > 0) CHECK(f->glyphs[i - 1].codepoint < f->glyphs[i].codepoint);  // sorted for the search
      CHECK(gm_jp_find_glyph(f, f->glyphs[i].codepoint) == &f->glyphs[i]);
      CHECK(f->glyphs[i].width > 0 && f->glyphs[i].bits != NULL);
    }
    CHECK(gm_jp_find_glyph(f, 'A') == NULL && gm_jp_find_glyph(f, 0x10FFFF) == NULL);
  }
  const gm_jp_font_t empty = {14, 0, NULL};
  CHECK(gm_jp_find_glyph(&empty, 0x6A5F) == NULL);
}

// Whatever the step, the slosh solver runs between 1 and `substeps` substeps.
TEST(slosh_substeps_stay_within_the_cap) {
  static const float caps[] = {2.0f, 12.0f, 32.0f};
  static const float steps[] = {0.0f, 1e-4f, 1.0f / 60.0f, 1.0f / 15.0f, 0.25f, 0.5f, 1.0f};
  gm_slosh_t* s = &s_fx.slosh;
  int most = 0;
  for (int c = 0; c < 3; ++c) {
    fresh_effect(1);
    gm_params_set(gm_effect_params(&s->base), "substeps", caps[c]);
    gm_params_set(gm_effect_params(&s->base), "tank_m", 0.2f);
    gm_effect_activate(&s->base);
    gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
    bool ok = true;
    for (int i = 0; i < 300; ++i) {
      ctx.dt = steps[i % 7];
      ctx.g.proper = gm_vec3((i / 20) % 2 ? 1.8f : -1.8f, 1.0f, (i / 13) % 2 ? 1.2f : -1.2f);
      gm_effect_update(&s->base, &ctx);
      ok = ok && gm_slosh_last_substeps(s) >= 1 && gm_slosh_last_substeps(s) <= (int)caps[c];
      most = gm_maxi(most, gm_slosh_last_substeps(s));
    }
    CHECK(ok);
  }
  CHECK(most == 32);
}

// On every demo scenario the tunnel, the sprite and the anim demo always put
// something on the glass; the sprite shows all 12 frames and reaches every layer.
TEST(effects_always_drawn_on_the_demo_scenarios) {
  static const int which[] = {4, 5, 6};
  unsigned frames_seen = 0, layers_seen = 0;
  for (int w = 0; w < 3; ++w)
    for (int i = 0; i < gm_scenario_preset_count(); ++i) {
      const gm_scenario_t* sc = gm_scenario_preset(i);
      gm_effect_t* e = fresh_effect(which[w]);
      gm_effect_activate(e);
      gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
      int blank = 0;
      for (int f = 0; f < (int)(gm_scenario_duration(sc) * 60.0f); ++f) {
        ctx.g = gm_scenario_at(sc, (float)f / 60.0f, NULL);
        gm_effect_update(e, &ctx);
        gm_frame_clear(&s_f);
        gm_effect_render(e, &s_f);
        blank += lit(&s_f) == 0;
        if (which[w] == 5) {
          frames_seen |= 1u << gm_sprite_current_frame(&s_fx.sprite);
          for (int z = 0; z < GM_D; ++z)
            if (lit_layer(&s_f, z)) layers_seen |= 1u << z;
        }
      }
      CHECK(blank == 0);
    }
  CHECK(frames_seen == 0xFFFu);
  CHECK(layers_seen == (1u << GM_D) - 1);
}
