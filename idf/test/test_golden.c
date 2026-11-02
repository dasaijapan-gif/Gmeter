// Reference-output ("golden") cases: each drives the core the way the
// firmware does and folds every output (frame bytes, burn-in offsets,
// parameter values, estimator outputs, return values) into a digest, which
// main.c compares with golden_<panel height>.txt. A change to the core that
// alters anything the unit shows or prints changes a digest. Inputs come from
// splitmix64 (test.h), never from rand().
//
// Cases:
//   fx_<effect>_<scenario>  each effect through the engine as the standard firmware
//                           runs it, on every demo scenario (app_main's demo source)
//   fx_anim_user_drive      the anim effect with an exported animation (fixtures/)
//   fuzz_<effect>           the effect through its vtable: random finite g (up to
//                           16 g), random dt (0, tiny, odd, up to 2 s), parameters
//                           set in range, at and past the limits, resets and
//                           deactivate/activate cycles at random times
//   nonfinite_<effect>      NaN, infinite and enormous g through the engine
//   engine_*                mode switches (also during a crossfade), auto-cycle,
//                           readout, the not-live-data tag, burn-in, parameters
//                           through the engine; standard and HUD edition
//   estimator_*             raw samples through the estimator with the filter
//                           settings and calibration paths of app_main
//   gstate_*, text_*, frame_ops, params_tables, scenario_presets, demo_assets
//
// Only finite g within +-16 g reaches the effects outside the nonfinite_ cases.
//
// API this file relies on: what idf/main calls, the effect vtables (the
// gm_effect_* wrappers in gm_effect.h), the parameter tables, the init and
// demo functions the firmware uses, raw frame bytes, plus the text and
// driving-state functions as the effects call them:
//   gm_engine_init, gm_engine_add_effect, gm_engine_set_effect, gm_engine_next_effect,
//   gm_engine_find_effect, gm_engine_effect_count, gm_engine_current, gm_engine_current_index,
//   gm_engine_set_auto_cycle, gm_engine_set_readout, gm_engine_set_status_label,
//   gm_engine_step, gm_engine_frame
//   gm_effect_name, gm_effect_params, gm_effect_activate, gm_effect_deactivate,
//   gm_effect_reset, gm_effect_update, gm_effect_render, gm_effect_burn_in_offset,
//   GM_EFFECT_CTX_INIT (only .g and .dt are set)
//   gm_params_count, gm_params_at, gm_params_get, gm_params_set
//   gm_gball_init, gm_slosh_init, gm_particles_init, gm_smoke_init, gm_tunnel_init,
//   gm_sprite_init (sheet, NULL), gm_sprite_demo_sheet, gm_anim_init, gm_anim_demo,
//   gm_anim_force_state, gm_anim_state_name, gm_hud_init, gm_hud_set_orbit_step,
//   gm_atfield_init, gm_sync_init, and the asset helpers gm_sprite_sheet_frame_bytes and
//   gm_anim_asset_total_bytes
//   gm_scenario_preset_count, gm_scenario_preset, gm_scenario_find, gm_scenario_name,
//   gm_scenario_duration, gm_scenario_at (rng NULL)
//   gm_estimator_init, gm_estimator_set_filter, gm_estimator_set_auto_zero,
//   gm_estimator_set_calibration, gm_estimator_set_nominal_forward, gm_estimator_update,
//   gm_estimator_reset, gm_estimator_begin_rest_calibration, gm_estimator_feed_rest_sample,
//   gm_estimator_finish_rest_calibration, gm_estimator_calibrate_forward_from_braking,
//   gm_estimator_forward_trusted, gm_estimator_calibration, gm_calibration_none
//   gm_frame_clear, gm_frame_rect, gm_frame_text, gm_frame_set
//   gm_jp_draw, gm_jp_draw_vertical, gm_jp_measure, gm_jp_small, gm_jp_large
//   gm_gstate_init, gm_gstate_update, gm_gstate_state, gm_gstate_reset (thresholds set
//   through the struct fields)
//   inline helpers of gm_math.h and gm_gforce.h (gm_vec3*, gm_mat3_row, gm_clampf, ...)
#include <float.h>
#include <stdlib.h>

#include "fixtures/user_animation.h"
#include "test.h"

// ---- the unit: static objects as in app_main ------------------------------------------------
#define FX_COUNT 10

typedef struct {
  gm_engine_t engine;
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
} unit_t;

static unit_t s_unit;
static gm_frame_t s_frame;  // for effects driven without the engine, and for drawing

// app_main's registration order.
static const char* const k_fx_names[FX_COUNT] = {"ball",   "slosh", "particles", "smoke",   "tunnel",
                                                 "sprite", "anim",  "hud",       "atfield", "sync"};
static gm_effect_t* const k_fx[FX_COUNT] = {&s_unit.ball.base,   &s_unit.slosh.base,  &s_unit.particles.base,
                                            &s_unit.smoke.base,  &s_unit.tunnel.base, &s_unit.sprite.base,
                                            &s_unit.anim.base,   &s_unit.hud.base,    &s_unit.atfield.base,
                                            &s_unit.sync.base};

static int fx_index(const char* name) {
  for (int i = 0; i < FX_COUNT; ++i)
    if (!strcmp(k_fx_names[i], name)) return i;
  return -1;
}

static void init_effect(int i, const gm_anim_asset_t* art) {
  switch (i) {
    case 0: gm_gball_init(&s_unit.ball); break;
    case 1: gm_slosh_init(&s_unit.slosh); break;
    case 2: gm_particles_init(&s_unit.particles); break;
    case 3: gm_smoke_init(&s_unit.smoke); break;
    case 4: gm_tunnel_init(&s_unit.tunnel); break;
    case 5: gm_sprite_init(&s_unit.sprite, gm_sprite_demo_sheet(), NULL); break;
    case 6: gm_anim_init(&s_unit.anim, art); break;
    case 7: gm_hud_init(&s_unit.hud); break;
    case 8: gm_atfield_init(&s_unit.atfield); break;
    default: gm_sync_init(&s_unit.sync); break;
  }
}

// Deactivates every initialised effect (frees the smoke grids).
static void unit_release(void) {
  for (int i = 0; i < FX_COUNT; ++i)
    if (k_fx[i]->vt) gm_effect_deactivate(k_fx[i]);
}

// Zeroed storage (the device's .bss), then build_objects() and the
// registration of all ten effects.
static void unit_build(const gm_anim_asset_t* art) {
  unit_release();
  memset(&s_unit, 0, sizeof(s_unit));
  gm_engine_init(&s_unit.engine);
  for (int i = 0; i < FX_COUNT; ++i) init_effect(i, art);
  for (int i = 0; i < FX_COUNT; ++i) gm_engine_add_effect(&s_unit.engine, k_fx[i]);
}

// One effect on its own, for driving it through its vtable.
static gm_effect_t* single_build(int i, const gm_anim_asset_t* art) {
  unit_release();
  memset(&s_unit, 0, sizeof(s_unit));
  init_effect(i, art);
  return k_fx[i];
}

// The rest of app_main's start-up for the engine: the saved (or fixed) effect
// without a crossfade, the HUD's orbit start, then apply_settings.
static void unit_boot(int start_effect, int orbit_step, bool readout, float auto_cycle) {
  gm_engine_set_effect(&s_unit.engine, start_effect, false);
  gm_hud_set_orbit_step(&s_unit.hud, orbit_step);
  gm_engine_set_readout(&s_unit.engine, readout);
  gm_engine_set_auto_cycle(&s_unit.engine, auto_cycle);
}

// ---- folding outputs ---------------------------------------------------------------------------
static void fold_params(gd_t* g, const gm_param_table_t* t) {
  for (int i = 0; i < gm_params_count(t); ++i) gd_f32(g, *gm_params_at(t, i)->value);
}

static void fold_effect(gd_t* g, gm_effect_t* e) {
  int dx = 0, dy = 0;
  gm_effect_burn_in_offset(e, &dx, &dy);
  gd_i32(g, dx);
  gd_i32(g, dy);
  fold_params(g, gm_effect_params(e));
}

static void fold_engine(gd_t* g) {
  const gm_engine_t* e = &s_unit.engine;
  gd_frame(g, gm_engine_frame(e));
  gd_i32(g, gm_engine_current_index(e));
  fold_effect(g, gm_engine_current(e));
}

// ---- inputs --------------------------------------------------------------------------------------
static gm_gforce_t dyn_g(float lat, float vert, float lon) {
  gm_gforce_t g = GM_GFORCE_REST_INIT;
  g.proper = gm_vec3_add(gm_gforce_rest_proper(), gm_vec3(lat, vert, lon));
  return g;
}

// current_g() in app_main for the demo source: the scenario time wraps in
// integer microseconds.
static gm_gforce_t demo_g(const gm_scenario_t* sc, int64_t us_since_boot) {
  const float dur = gm_scenario_duration(sc);
  const int64_t period = (int64_t)((dur + 1.0f) * 1e6f);
  return gm_scenario_at(sc, (float)(us_since_boot % period) * 1e-6f, NULL);
}

static const gm_scenario_t* drive_scenario(void) { return gm_scenario_preset(gm_scenario_preset_count() - 1); }

static const gm_scenario_t* random_scenario(trng_t* r) {
  return gm_scenario_preset(trng_int(r, 0, gm_scenario_preset_count() - 1));
}

// A random parameter of the table, set in range, at a limit, past a limit, to
// a round value, refused (non-finite or unknown name).
static void poke_param(gd_t* g, trng_t* r, gm_param_table_t* t) {
  const gm_param_t* p = gm_params_at(t, trng_int(r, 0, gm_params_count(t) - 1));
  const float span = p->max - p->min;
  const char* name = p->name;
  float v;
  switch (trng_int(r, 0, 9)) {
    case 0:
    case 1:
    case 2: v = trng_range(r, p->min, p->max); break;
    case 3: v = p->min; break;
    case 4: v = p->max; break;
    case 5: v = p->min - trng_range(r, 0.0f, span + 1.0f); break;
    case 6: v = p->max + trng_range(r, 0.0f, span + 1.0f); break;
    case 7: v = trng_chance(r, 0.5f) ? -1e9f : 1e9f; break;
    case 8: v = (float)trng_int(r, -2, 4) * 0.5f; break;
    default:
      v = trng_chance(r, 0.5f) ? NAN : INFINITY;
      if (trng_chance(r, 0.3f)) name = "nope";
      break;
  }
  const bool ok = gm_params_set(t, name, v);
  gd_bool(g, ok);
  float got = 0.0f;
  gd_bool(g, gm_params_get(t, name, &got));
  gd_f32(g, got);
  gd_event(g, "set %s %g -> %s %g", name, (double)v, ok ? "ok" : "refused", (double)got);
}

// ---- fx_<effect>_<scenario> --------------------------------------------------------------------
// render_task: dt from the microsecond clock, g from the demo scenario at
// the time since boot, the standard edition's untagged frame.
static void case_fx(gd_t* g, int arg) {
  const int fx = arg & 0xFF;
  const gm_scenario_t* sc = gm_scenario_preset((arg >> 8) & 0xFF);
  unit_build((arg >> 16) & 1 ? &kUserAnimation : gm_anim_demo());
  unit_boot(fx, (arg >> 8) & 3, false, 0.0f);
  int64_t now = 150000, last = now;  // the render task starts after the panels are up
  const int steps = (int)((gm_scenario_duration(sc) + 1.5f) * 60.0f);
  for (int i = 0; i < steps; ++i) {
    now += 16667;
    const float dt = (float)(now - last) * 1e-6f;
    last = now;
    gm_engine_set_status_label(&s_unit.engine, NULL);
    gm_engine_step(&s_unit.engine, demo_g(sc, now), dt);
    fold_engine(g);
    gd_step(g, "engine step");
  }
  unit_release();
}

// ---- fuzz_<effect> --------------------------------------------------------------------------------
typedef struct {
  int mode, left;
  gm_vec3_t dyn, target;
  const gm_scenario_t* sc;
  float t;
} gen_t;

// Finite g, never beyond 16 g on an axis: smooth driving, jumps, held
// extremes, scenarios, rest, free fall or upside down, vertical spikes.
static gm_gforce_t gen_next(gen_t* s, trng_t* r, float dt) {
  if (s->left-- <= 0) {
    s->mode = trng_int(r, 0, 6);
    s->left = trng_int(r, 30, 300);
    s->sc = random_scenario(r);
    s->t = trng_range(r, 0.0f, gm_scenario_duration(s->sc));
    if (s->mode == 2)
      s->target = gm_vec3(trng_range(r, -16.0f, 16.0f), trng_range(r, -16.0f, 16.0f), trng_range(r, -16.0f, 16.0f));
    else if (s->mode == 5)
      s->target = trng_chance(r, 0.5f) ? gm_vec3_zero() : gm_vec3(0.0f, -1.0f, 0.0f);
    else
      s->target = gm_vec3(trng_range(r, -1.5f, 1.5f), trng_range(r, -0.8f, 0.8f), trng_range(r, -1.5f, 1.5f));
  }
  gm_gforce_t g = GM_GFORCE_REST_INIT;
  switch (s->mode) {
    case 0:
      if (trng_chance(r, 0.03f))
        s->target = gm_vec3(trng_range(r, -1.5f, 1.5f), trng_range(r, -0.8f, 0.8f), trng_range(r, -1.5f, 1.5f));
      s->dyn = gm_vec3_lerp(s->dyn, s->target, 0.1f);
      g = dyn_g(s->dyn.x + trng_range(r, -0.02f, 0.02f), s->dyn.y + trng_range(r, -0.02f, 0.02f),
                s->dyn.z + trng_range(r, -0.02f, 0.02f));
      break;
    case 1:
      g.proper = gm_vec3(trng_range(r, -16.0f, 16.0f), trng_range(r, -16.0f, 16.0f), trng_range(r, -16.0f, 16.0f));
      break;
    case 2:
    case 5: g.proper = s->target; break;
    case 3:
      s->t += dt;
      g = gm_scenario_at(s->sc, s->t, NULL);
      break;
    case 4: break;
    default:
      if (trng_chance(r, 0.1f)) g = dyn_g(0.0f, trng_range(r, -3.0f, 3.0f), 0.0f);
      break;
  }
  return g;
}

static float fuzz_dt(trng_t* r) {
  const float u = trng_unit(r);
  if (u < 0.5f) return 1.0f / 60.0f;
  if (u < 0.6f) return 0.0f;
  if (u < 0.7f) return trng_range(r, 1e-7f, 1e-3f);
  if (u < 0.85f) return trng_range(r, 1e-3f, 0.1f);
  if (u < 0.95f) return trng_range(r, 0.1f, 0.5f);
  return trng_range(r, 0.5f, 2.0f);
}

static void case_fuzz(gd_t* g, int arg) {
  const int fx = arg & 0xFF;
  const bool user_art = (arg >> 16) & 1;
  trng_t r = trng(0xF0225EEDull + (uint64_t)arg * 7919u);
  gm_effect_t* e = single_build(fx, user_art ? &kUserAnimation : gm_anim_demo());
  gm_effect_activate(e);
  gen_t gen;
  memset(&gen, 0, sizeof(gen));
  for (int i = 0; i < 1500; ++i) {
    if (trng_chance(&r, 0.03f)) poke_param(g, &r, gm_effect_params(e));
    if (trng_chance(&r, 0.005f)) {
      gd_event(g, "reset");
      gm_effect_reset(e);
    }
    if (trng_chance(&r, 0.005f)) {
      gd_event(g, "deactivate, activate");
      gm_effect_deactivate(e);
      gm_effect_activate(e);
    }
    if (trng_chance(&r, 0.003f)) {
      gd_event(g, "activate again");
      gm_effect_activate(e);  // the engine does this when a fade is reversed
    }
    if (fx == fx_index("hud") && trng_chance(&r, 0.005f)) {
      const int step = trng_int(&r, 0, 7);
      gd_event(g, "orbit step %d", step);
      gm_hud_set_orbit_step(&s_unit.hud, step);
    }
    if (fx == fx_index("anim") && trng_chance(&r, 0.01f)) {
      const int state = trng_int(&r, -1, GM_ANIM_STATE_COUNT - 1);
      gd_event(g, "anim %s", gm_anim_state_name(state));
      gm_anim_force_state(&s_unit.anim, state);
    }
    const float dt = fuzz_dt(&r);
    const gm_gforce_t gin = gen_next(&gen, &r, dt);
    const int updates = 1 + trng_chance(&r, 0.1f) + trng_chance(&r, 0.05f);
    gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
    ctx.g = gin;
    ctx.dt = dt;
    for (int k = 0; k < updates; ++k) gm_effect_update(e, &ctx);
    gm_frame_clear(&s_frame);
    gm_effect_render(e, &s_frame);
    gd_frame(g, &s_frame);
    fold_effect(g, e);
    gd_step(g, "update, render");
  }
  unit_release();
}

// ---- nonfinite_<effect> -----------------------------------------------------------------------------
static void case_nonfinite(gd_t* g, int fx) {
  static const float bad[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 3e9f, -3e9f, 1e6f, -2.5e7f, FLT_MAX, -FLT_MAX};
  const int nbad = (int)(sizeof(bad) / sizeof(bad[0]));
  trng_t r = trng(0xBADF00Dull + (uint64_t)fx);
  unit_build(gm_anim_demo());
  unit_boot(fx, fx & 3, false, 0.0f);
  const gm_scenario_t* sc = drive_scenario();
  int64_t now = 150000, last = now;
  for (int i = 0; i < 900; ++i) {
    if (i % 300 == 0) gm_engine_set_readout(&s_unit.engine, (i / 300) & 1);
    now += 16667;
    const float dt = (float)(now - last) * 1e-6f;
    last = now;
    gm_gforce_t gin = demo_g(sc, now);
    if (i % 150 >= 60 && i % 150 < 60 + (i / 150 + 1) * 5) {
      // A burst of bad samples: some components or all of them.
      const int mask = trng_int(&r, 1, 7);
      if (mask & 1) gin.proper.x = bad[trng_int(&r, 0, nbad - 1)];
      if (mask & 2) gin.proper.y = bad[trng_int(&r, 0, nbad - 1)];
      if (mask & 4) gin.proper.z = bad[trng_int(&r, 0, nbad - 1)];
      gd_event(g, "g (%g, %g, %g)", (double)gin.proper.x, (double)gin.proper.y, (double)gin.proper.z);
    }
    gm_engine_set_status_label(&s_unit.engine, NULL);
    gm_engine_step(&s_unit.engine, gin, dt);
    fold_engine(g);
    gd_step(g, "engine step");
  }
  unit_release();
}

// ---- engine_* ---------------------------------------------------------------------------------------
// The g sources of app_main and the tag the HUD edition shows for each.
typedef enum { SRC_IMU, SRC_LOST, SRC_SERIAL, SRC_DEMO } src_t;

typedef struct {
  src_t src;
  const gm_scenario_t* sc;
  gm_vec3_t injected;
  gen_t imu;  // a live sensor: smooth driving
  int64_t now, last;
} app_t;

static gm_gforce_t app_g(app_t* a, trng_t* r, const char** label) {
  gm_gforce_t g = GM_GFORCE_REST_INIT;
  switch (a->src) {
    case SRC_IMU:
      *label = NULL;
      a->imu.mode = 0;
      a->imu.left = 1000;
      g = gen_next(&a->imu, r, 1.0f / 60.0f);
      break;
    case SRC_LOST: *label = "NO SENSOR"; break;
    case SRC_SERIAL:
      *label = "SERIAL";
      g.proper = gm_vec3_add(gm_gforce_rest_proper(), a->injected);
      break;
    default:
      *label = "DEMO";
      g = demo_g(a->sc, a->now);
      break;
  }
  return g;
}

// The render task's clock: about 60 Hz with jitter, stalls and short frames.
static float app_tick(app_t* a, trng_t* r) {
  const float u = trng_unit(r);
  if (u < 0.02f)
    a->now += trng_int(r, 30000, 150000);  // a stall
  else if (u < 0.03f)
    a->now += trng_int(r, 100, 900);  // a short frame
  else
    a->now += 16667 + trng_int(r, -3000, 3000);
  const float dt = (float)(a->now - a->last) * 1e-6f;
  a->last = a->now;
  return dt;
}

// `src imu|serial|demo [scenario]`, `g <lat> <vert> <lon>`, or the sensor dropping out.
static void app_source(gd_t* g, app_t* a, trng_t* r) {
  switch (trng_int(r, 0, 4)) {
    case 0: a->src = SRC_IMU; break;
    case 1: a->src = SRC_LOST; break;
    case 2:
    case 3: {
      // app_inject_g clamps each axis to +-8 g.
      const float k = trng_chance(r, 0.8f) ? 1.5f : 12.0f;
      a->injected = gm_vec3(gm_clampf(trng_range(r, -k, k), -8.0f, 8.0f), gm_clampf(trng_range(r, -k, k), -8.0f, 8.0f),
                            gm_clampf(trng_range(r, -k, k), -8.0f, 8.0f));
      a->src = SRC_SERIAL;
      break;
    }
    default:
      a->src = SRC_DEMO;
      if (trng_chance(r, 0.7f)) a->sc = random_scenario(r);
      break;
  }
  gd_event(g, "source %d", (int)a->src);
}

// app_set_mode: a name, else a number.
static void app_mode(gd_t* g, const char* arg) {
  gm_engine_t* e = &s_unit.engine;
  int idx = gm_engine_find_effect(e, arg);
  if (idx < 0 && arg[0] >= '0' && arg[0] <= '9') idx = atoi(arg);
  const bool ok = idx >= 0 && idx < gm_engine_effect_count(e);
  if (ok) gm_engine_set_effect(e, idx, true);
  gd_i32(g, idx);
  gd_bool(g, ok);
  gd_event(g, "mode %s -> %s", arg, ok ? gm_effect_name(gm_engine_current(e)) : "unknown");
}

static void app_random_mode(gd_t* g, trng_t* r) {
  static const char* const k_args[] = {"0", "3", "7", "9", "10", "12", "nope", "HUD", ""};
  const int kind = trng_int(r, 0, 9);
  if (kind < 4) {
    app_mode(g, k_fx_names[trng_int(r, 0, FX_COUNT - 1)]);
  } else if (kind < 6) {
    app_mode(g, k_args[trng_int(r, 0, (int)(sizeof(k_args) / sizeof(k_args[0])) - 1)]);
  } else {
    gd_event(g, "mode next");  // `mode next` or the BOOT button
    gm_engine_next_effect(&s_unit.engine);
  }
}

// `set <name> <value>` (finite values only; the CLI parser refuses others)
// and `get <name>`, on the effect on screen.
static void app_set_get(gd_t* g, trng_t* r) {
  gm_engine_t* e = &s_unit.engine;
  gm_param_table_t* t = gm_effect_params(gm_engine_current(e));
  const gm_param_t* p = gm_params_at(t, trng_int(r, 0, gm_params_count(t) - 1));
  const char* name = trng_chance(r, 0.1f) ? "nope" : p->name;
  float v;
  switch (trng_int(r, 0, 4)) {
    case 0: v = p->min; break;
    case 1: v = p->max; break;
    case 2: v = p->max + trng_range(r, 0.0f, 100.0f); break;
    default: v = trng_range(r, p->min, p->max); break;
  }
  if (trng_chance(r, 0.25f)) {
    float got = 0.0f;
    const bool ok = gm_params_get(t, name, &got);
    gd_bool(g, ok);
    gd_f32(g, got);
    gd_event(g, "get %s -> %g", name, (double)got);
    return;
  }
  const bool ok = engine_set_param(e, name, v);
  float got = v;
  if (ok) gm_params_get(gm_effect_params(gm_engine_current(e)), name, &got);
  gd_bool(g, ok);
  gd_f32(g, got);
  gd_event(g, "set %s %g -> %s %g", name, (double)v, ok ? "ok" : "unknown", (double)got);
}

static void app_start(app_t* a, trng_t* r) {
  memset(a, 0, sizeof(*a));
  a->src = SRC_DEMO;
  a->sc = drive_scenario();
  a->now = a->last = 150000 + trng_int(r, 0, 50000);
}

static void engine_run(gd_t* g, app_t* a, trng_t* r, const char* fixed, int steps) {
  int burst = 0;
  for (int i = 0; i < steps; ++i) {
    if (!fixed) {
      // Switches come in bursts now and then, so some land during a crossfade.
      if (burst > 0 ? --burst == 0 : trng_chance(r, 0.008f)) {
        app_random_mode(g, r);
        if (trng_chance(r, 0.5f)) burst = trng_int(r, 1, 30);
      }
      if (trng_chance(r, 0.003f)) {
        static const float k_cycle[] = {0.0f, 0.0f, 0.3f, 1.0f, 2.5f, 7.0f, -4.0f, 5000.0f};
        const float s = gm_clampf(k_cycle[trng_int(r, 0, 7)], 0.0f, 3600.0f);  // app_set_auto_cycle
        gd_event(g, "cycle %g", (double)s);
        gm_engine_set_auto_cycle(&s_unit.engine, s);
      }
      if (trng_chance(r, 0.003f)) {
        // `anim <state|auto>`: pins the clip, then shows the anim effect.
        const int state = trng_int(r, -1, GM_ANIM_STATE_COUNT - 1);
        gm_anim_force_state(&s_unit.anim, state);
        if (state >= 0) app_mode(g, "anim");
      }
    }
    if (trng_chance(r, 0.004f)) {
      const bool on = trng_chance(r, 0.5f);
      gd_event(g, "readout %d", on);
      gm_engine_set_readout(&s_unit.engine, on);
    }
    if (trng_chance(r, 0.012f)) app_set_get(g, r);
    if (trng_chance(r, 0.006f)) app_source(g, a, r);
    const float dt = app_tick(a, r);
    const char* label = NULL;
    const gm_gforce_t gin = app_g(a, r, &label);
    // Only the single-effect (HUD) edition tags data that is not live.
    gm_engine_set_status_label(&s_unit.engine, fixed ? label : NULL);
    gm_engine_step(&s_unit.engine, gin, dt);
    fold_engine(g);
    gd_step(g, "engine step");
  }
}

static void case_engine_standard(gd_t* g, int arg) {
  trng_t r = trng(0xE591E5ull + (uint64_t)arg);
  unit_build(gm_anim_demo());
  app_t a;
  app_start(&a, &r);
  unit_boot(trng_int(&r, 0, FX_COUNT - 1), trng_int(&r, 0, 3), trng_chance(&r, 0.5f), 0.0f);
  engine_run(g, &a, &r, NULL, 4000);
  unit_release();
}

// Auto-cycle through every effect twice with the readout on.
static void case_engine_cycle(gd_t* g, int arg) {
  (void)arg;
  unit_build(gm_anim_demo());
  unit_boot(0, 1, true, 1.2f);
  const gm_scenario_t* sc = gm_scenario_find("slalom");
  int64_t now = 150000, last = now;
  for (int i = 0; i < 1500; ++i) {
    now += 16667;
    const float dt = (float)(now - last) * 1e-6f;
    last = now;
    gm_engine_set_status_label(&s_unit.engine, NULL);
    gm_engine_step(&s_unit.engine, demo_g(sc, now), dt);
    fold_engine(g);
    gd_step(g, "engine step");
  }
  unit_release();
}

// The HUD edition: always the hud effect, tagged while the g is not live.
static void case_engine_hud(gd_t* g, int arg) {
  trng_t r = trng(0x4D0Dull + (uint64_t)arg);
  unit_build(gm_anim_demo());
  app_t a;
  app_start(&a, &r);
  unit_boot(gm_engine_find_effect(&s_unit.engine, "hud"), trng_int(&r, 0, 3), trng_chance(&r, 0.5f), 0.0f);
  // Low thresholds and a fast orbit so the run sees warnings, 危険 and orbit steps.
  engine_set_param(&s_unit.engine, "orbit_min", 0.02f);
  engine_set_param(&s_unit.engine, "warn_g", 0.4f);
  engine_set_param(&s_unit.engine, "danger_g", 0.9f);
  engine_run(g, &a, &r, "hud", 4000);
  unit_release();
}

// The HUD edition at its defaults, rendering slowly (dt clamps at 1/15 s), for
// long enough that the burn-in orbit goes once round.
static void case_engine_hud_orbit(gd_t* g, int arg) {
  (void)arg;
  unit_build(gm_anim_demo());
  unit_boot(gm_engine_find_effect(&s_unit.engine, "hud"), 3, false, 0.0f);
  const gm_scenario_t* sc = drive_scenario();
  int64_t now = 150000, last = now;
  for (int i = 0; i < 7600; ++i) {
    now += 66667;
    const float dt = (float)(now - last) * 1e-6f;
    last = now;
    gm_engine_set_status_label(&s_unit.engine, "DEMO");
    gm_engine_step(&s_unit.engine, demo_g(sc, now), dt);
    fold_engine(g);
    gd_step(g, "engine step");
  }
  unit_release();
}

// ---- estimator_* ------------------------------------------------------------------------------------
#define IMU_HZ 200  // BOARD_IMU_SAMPLE_HZ

typedef struct {
  gm_estimator_t est;
  gm_calibration_t cal;  // s_calibration
} imu_t;

static imu_t s_imu;

// storage_forward_axis_vector: +x -x +y -y +z -z.
static gm_vec3_t axis_vector(int i) {
  static const float k[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  return gm_vec3(k[i][0], k[i][1], k[i][2]);
}

// build_objects() and apply_settings() for the estimator, with the saved
// calibration (NULL: none).
static void imu_boot(float filter_hz, bool auto_zero, int forward_axis, const gm_calibration_t* saved) {
  gm_estimator_init(&s_imu.est);
  s_imu.cal = saved ? *saved : gm_calibration_none();
  gm_estimator_set_filter(&s_imu.est, filter_hz, (float)IMU_HZ);
  gm_estimator_set_auto_zero(&s_imu.est, auto_zero, 60.0f);
  if (s_imu.cal.valid) {
    gm_estimator_set_calibration(&s_imu.est, s_imu.cal);
    gm_estimator_set_nominal_forward(&s_imu.est, gm_mat3_row(&s_imu.cal.sensor_to_car, 2));
  } else {
    gm_estimator_set_nominal_forward(&s_imu.est, axis_vector(forward_axis));
  }
}

// Sensor mounts: rows are car right, up, forward in the sensor frame
// (left-handed car frame, so the determinant is -1).
static const gm_mat3_t k_mount_flat = {{{1, 0, 0}, {0, 0, 1}, {0, 1, 0}}};       // chip +Z up, +Y forward
static const gm_mat3_t k_mount_upright = {{{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};   // chip +Y up, +Z forward
static const gm_mat3_t k_mount_flipped = {{{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};  // upside down, +Y forward
static const gm_mat3_t k_mount_tilted = {
    {{-1, 0, 0}, {0, 0.9396926f, 0.34202015f}, {0, -0.34202015f, 0.9396926f}}};  // upright, tilted 20 degrees

typedef struct {
  const gm_mat3_t* mount;
  trng_t r;
  float noise;     // white noise, g
  float vib;       // engine vibration (a square wave), g
  gm_vec3_t bias;  // mount offset, car frame
  long n;
} sensor_t;

// What mpu6050_read returns for a car-frame proper acceleration: rotated
// into the sensor frame, with noise, in counts of 1/8192 g (+-4 g range).
static gm_vec3_t sensor_read(sensor_t* s, gm_vec3_t proper) {
  const gm_vec3_t c = gm_vec3_add(proper, s->bias);
  const float(*m)[3] = s->mount->m;
  float v[3] = {m[0][0] * c.x + m[1][0] * c.y + m[2][0] * c.z, m[0][1] * c.x + m[1][1] * c.y + m[2][1] * c.z,
                m[0][2] * c.x + m[1][2] * c.y + m[2][2] * c.z};
  const float wave = (s->n++ % 5) < 2 ? s->vib : -s->vib;
  for (int k = 0; k < 3; ++k) {
    float x = v[k] + trng_range(&s->r, -s->noise, s->noise) + (k == 2 ? wave : 0.0f);
    x = gm_clampf(x * 8192.0f, -32768.0f, 32767.0f);
    v[k] = (float)(int16_t)(int)floorf(x + 0.5f) / 8192.0f;
  }
  return gm_vec3(v[0], v[1], v[2]);
}

static void imu_sample(gd_t* g, sensor_t* s, gm_vec3_t proper, gm_vec3_t* accum, int* count) {
  const gm_vec3_t raw = sensor_read(s, proper);
  const gm_gforce_t out = gm_estimator_update(&s_imu.est, raw);
  gd_vec(g, out.proper);
  if (accum) {
    *accum = gm_vec3_add(*accum, raw);
    ++*count;
  }
}

// `seconds` of a scenario, folding every output.
static void imu_drive(gd_t* g, sensor_t* s, const gm_scenario_t* sc, float seconds) {
  const int n = (int)(seconds * IMU_HZ);
  for (int i = 0; i < n; ++i) {
    imu_sample(g, s, gm_scenario_at(sc, (float)i / (float)IMU_HZ, NULL).proper, NULL, NULL);
    if (i % 20 == 19) gd_step(g, "100 ms of samples");
  }
}

static void fold_calibration(gd_t* g) {
  const gm_calibration_t c = gm_estimator_calibration(&s_imu.est);
  for (int i = 0; i < 3; ++i) gd_vec(g, gm_mat3_row(&c.sensor_to_car, i));
  gd_bool(g, c.valid);
  gd_bool(g, gm_estimator_forward_trusted(&s_imu.est));
}

// imu_task's capture: every sample goes through the estimator and into the
// average; at the deadline, `cal rest` or `cal brake` uses the mean.
static bool imu_capture(gd_t* g, sensor_t* s, gm_vec3_t proper, bool rest) {
  gm_vec3_t accum = gm_vec3_zero();
  int count = 0;
  const int n = rest ? 400 : 300;  // 2 s or 1.5 s
  for (int i = 0; i < n; ++i) imu_sample(g, s, proper, &accum, &count);
  const gm_vec3_t mean = gm_vec3_div(accum, (float)count);
  bool ok;
  if (rest) {
    gm_estimator_begin_rest_calibration(&s_imu.est);
    for (int i = 0; i < 16; ++i) gm_estimator_feed_rest_sample(&s_imu.est, mean);
    ok = gm_estimator_finish_rest_calibration(&s_imu.est);
  } else {
    ok = gm_estimator_calibrate_forward_from_braking(&s_imu.est, mean);
  }
  if (ok) s_imu.cal = gm_estimator_calibration(&s_imu.est);
  gd_bool(g, ok);
  fold_calibration(g);
  gd_event(g, "cal %s (%g %g %g) -> %s", rest ? "rest" : "brake", (double)proper.x, (double)proper.y,
           (double)proper.z, ok ? "ok" : "failed");
  gd_step(g, "capture");
  return ok;
}

// Filtering with a calibration from NVS: filter changes, auto zero on and
// off, the sensor dropping out (reset), the forward hint, saturation; then
// `cal clear` and a reboot (no calibration), then another saved mount.
static void case_estimator_filter(gd_t* g, int arg) {
  (void)arg;
  trng_t r = trng(0xE57F11ull);
  sensor_t s = {&k_mount_flat, trng(11), 0.01f, 0.02f, {0, 0, 0}, 0};
  const gm_calibration_t saved = {k_mount_flat, true};
  imu_boot(4.0f, false, 2, &saved);
  const gm_scenario_t* sc = drive_scenario();
  for (int i = 0; i < 40 * IMU_HZ; ++i) {
    if (trng_chance(&r, 0.002f)) {
      const float hz = gm_clampf(trng_range(&r, -2.0f, 40.0f), 0.5f, 30.0f);  // app_set_filter_hz
      gd_event(g, "filter %g", (double)hz);
      gm_estimator_set_filter(&s_imu.est, hz, (float)IMU_HZ);
    }
    if (trng_chance(&r, 0.001f)) gm_estimator_set_auto_zero(&s_imu.est, trng_chance(&r, 0.5f), 60.0f);
    if (trng_chance(&r, 0.0005f)) {
      gd_event(g, "sensor reconnected");
      gm_estimator_reset(&s_imu.est);
    }
    if (trng_chance(&r, 0.0005f)) gm_estimator_set_nominal_forward(&s_imu.est, axis_vector(trng_int(&r, 0, 5)));
    gm_vec3_t proper = gm_scenario_at(sc, (float)i / (float)IMU_HZ, NULL).proper;
    if (i % 1000 < 6) proper = gm_vec3(5.0f, -6.0f, 4.5f);  // a knock that saturates the sensor
    imu_sample(g, &s, proper, NULL, NULL);
    if (i % 20 == 19) gd_step(g, "100 ms of samples");
  }
  imu_boot(4.0f, false, 4, NULL);
  imu_drive(g, &s, gm_scenario_find("slalom"), 10.0f);
  const gm_calibration_t tilted = {k_mount_tilted, true};
  s.mount = &k_mount_tilted;
  imu_boot(12.0f, false, 4, &tilted);
  imu_drive(g, &s, gm_scenario_find("brake"), 6.0f);
}

// Auto zero on a mount that has shifted: long parked stretches between drives.
static void case_estimator_autozero(gd_t* g, int arg) {
  (void)arg;
  sensor_t s = {&k_mount_upright, trng(12), 0.003f, 0.0f, {0.05f, 0.0f, -0.03f}, 0};
  const gm_calibration_t saved = {k_mount_upright, true};
  imu_boot(4.0f, true, 4, &saved);
  static const struct {
    float seconds;
    int what;  // 0 parked, 1 drive, 2 reset, 3 auto zero off, 4 auto zero on
  } k_plan[] = {{70, 0}, {10, 1}, {70, 0}, {0, 2}, {10, 0}, {0, 3}, {5, 0}, {0, 4}, {20, 0}};
  for (size_t p = 0; p < sizeof(k_plan) / sizeof(k_plan[0]); ++p) {
    switch (k_plan[p].what) {
      case 1: imu_drive(g, &s, drive_scenario(), k_plan[p].seconds); break;
      case 2: gm_estimator_reset(&s_imu.est); break;
      case 3: gm_estimator_set_auto_zero(&s_imu.est, false, 60.0f); break;
      case 4: gm_estimator_set_auto_zero(&s_imu.est, true, 60.0f); break;
      default: {
        const int n = (int)(k_plan[p].seconds * IMU_HZ);
        for (int i = 0; i < n; ++i) {
          imu_sample(g, &s, gm_gforce_rest_proper(), NULL, NULL);
          if (i % 50 == 49) gd_step(g, "250 ms parked");
        }
      }
    }
  }
}

// `cal fwd <axis>` then `cal rest` on four mounts and every hint, failed
// captures, then `save` and a reboot with the stored calibration.
static void case_estimator_rest_cal(gd_t* g, int arg) {
  (void)arg;
  static const gm_mat3_t* const k_mounts[] = {&k_mount_flat, &k_mount_upright, &k_mount_flipped, &k_mount_tilted};
  for (int m = 0; m < 4; ++m) {
    for (int axis = 0; axis < 6; ++axis) {
      sensor_t s = {k_mounts[m], trng(100u + (uint64_t)(m * 6 + axis)), 0.004f, 0.01f, {0, 0, 0}, 0};
      imu_boot(4.0f, false, axis, NULL);
      imu_capture(g, &s, gm_gforce_rest_proper(), true);
      imu_drive(g, &s, gm_scenario_find("corner"), 2.0f);
      // A capture while the car moves or bounces is refused and keeps the calibration.
      imu_capture(g, &s, gm_vec3(0.6f, 1.5f, 0.2f), true);
      imu_capture(g, &s, gm_vec3(0.0f, 0.3f, 0.0f), true);
      const gm_calibration_t saved = s_imu.cal;
      imu_boot(4.0f, false, axis, &saved);
      imu_drive(g, &s, gm_scenario_find("launch"), 2.0f);
    }
  }
}

// `cal brake` after `cal rest`: too weak, firm, with some cornering mixed in,
// on a flat board whose forward hint points up (forward unknown), and as a
// refinement of a trusted calibration; then a reboot and another `cal rest`.
static void case_estimator_brake_cal(gd_t* g, int arg) {
  (void)arg;
  sensor_t s = {&k_mount_flat, trng(21), 0.004f, 0.01f, {0, 0, 0}, 0};
  imu_boot(4.0f, false, 4, NULL);
  gd_bool(g, gm_estimator_calibrate_forward_from_braking(&s_imu.est, gm_vec3(0, -0.5f, 1)));  // nothing to refine
  imu_capture(g, &s, gm_gforce_rest_proper(), true);
  static const float k_brakes[][2] = {{0.02f, 0.0f}, {0.3f, 0.0f}, {0.6f, 0.1f}, {0.45f, -0.2f}};
  for (int i = 0; i < 4; ++i) {
    imu_capture(g, &s, gm_vec3(k_brakes[i][1], 1.0f, -k_brakes[i][0]), false);
    imu_drive(g, &s, gm_scenario_find("corner"), 2.0f);
  }
  s.mount = &k_mount_tilted;
  imu_boot(4.0f, false, 4, NULL);
  imu_capture(g, &s, gm_gforce_rest_proper(), true);
  imu_capture(g, &s, gm_vec3(0.05f, 1.0f, -0.5f), false);
  imu_drive(g, &s, drive_scenario(), 4.0f);
  const gm_calibration_t saved = s_imu.cal;
  imu_boot(4.0f, false, 4, &saved);
  imu_capture(g, &s, gm_gforce_rest_proper(), true);
  imu_drive(g, &s, drive_scenario(), 4.0f);
}

// ---- gstate_* ----------------------------------------------------------------------------------------
static void gstate_fold(gd_t* g, gm_gstate_t* s, gm_gforce_t in, float dt) {
  bool bumped = false;
  gd_bool(g, gm_gstate_update(s, in, dt, &bumped));
  gd_bool(g, bumped);
  gd_i32(g, (int)gm_gstate_state(s));
}

// Held inputs at 60 Hz with the default thresholds and the ones the HUD,
// atfield and anim tables allow.
static void case_gstate_scripted(gd_t* g, int arg) {
  (void)arg;
  static const float k_thresholds[][3] = {{0.25f, 0.4f, 0.3f}, {0.5f, 0.0f, 0.3f}, {0.05f, 3.0f, 0.05f},
                                          {1.5f, 0.3f, 2.0f}};
  static const struct {
    float lat, vert, lon, seconds;
  } k_script[] = {{0, 0, 0, 1},         {0, 0, 0.3f, 1},      {0, 0, 0.18f, 0.5f},  {0, 0, 0.1f, 0.5f},
                  {0, 0, -0.6f, 1},     {0.45f, 0, -0.35f, 1}, {0.6f, 0, -0.35f, 1}, {-0.5f, 0, 0, 1},
                  {-0.18f, 0, 0, 0.5f}, {0, 0.6f, 0, 0.02f},  {0, -0.5f, 0, 0.05f}, {0, 0, 0, 1},
                  {1.6f, 0, 1.6f, 1},   {0, 2.5f, 0, 0.1f},   {0.3f, 0, 0.3f, 2},   {0, 0, 0, 1}};
  for (int t = 0; t < 4; ++t) {
    gm_gstate_t s;
    gm_gstate_init(&s);
    s.enter_g = k_thresholds[t][0];
    s.hold_s = k_thresholds[t][1];
    s.bump_g = k_thresholds[t][2];
    for (size_t i = 0; i < sizeof(k_script) / sizeof(k_script[0]); ++i) {
      const int n = (int)(k_script[i].seconds * 60.0f + 0.5f);
      for (int k = 0; k < n; ++k)
        gstate_fold(g, &s, dyn_g(k_script[i].lat, k_script[i].vert, k_script[i].lon), 1.0f / 60.0f);
      gd_step(g, "script line");
    }
    gm_gstate_reset(&s);
    gstate_fold(g, &s, dyn_g(0.5f, 0, 0), 1.0f / 60.0f);
    gd_step(g, "after reset");
  }
}

static void case_gstate_random(gd_t* g, int arg) {
  trng_t r = trng(0x65747A7Eull + (uint64_t)arg);
  gm_gstate_t s;
  gm_gstate_init(&s);
  gen_t gen;
  memset(&gen, 0, sizeof(gen));
  for (int i = 0; i < 8000; ++i) {
    if (trng_chance(&r, 0.002f)) {
      s.enter_g = trng_range(&r, 0.02f, 1.5f);
      s.hold_s = trng_chance(&r, 0.3f) ? 0.0f : trng_range(&r, 0.0f, 3.0f);
      s.bump_g = trng_range(&r, 0.05f, 2.0f);
    }
    if (trng_chance(&r, 0.002f)) gm_gstate_reset(&s);
    const float dt = trng_chance(&r, 0.7f) ? 1.0f / 60.0f : trng_range(&r, 0.0f, 0.3f);
    gstate_fold(g, &s, gen_next(&gen, &r, dt), dt);
    if (i % 10 == 9) gd_step(g, "10 updates");
  }
}

// ---- text_*, frame_ops -----------------------------------------------------------------------------
typedef struct {
  const gm_jp_font_t* font;
  const char* text;
  int spacing;
  bool vertical;
} jp_text_t;

// Every string the effects draw with gm_jp_*, in the font and spacing they use.
static const jp_text_t k_jp_texts[] = {
    {&gm_jp_small, "A.T.フィールド", 1, false}, {&gm_jp_small, "全開", 1, false},    // atfield
    {&gm_jp_small, "横G", 1, false},           {&gm_jp_small, "縦G", 1, false},      // hud
    {&gm_jp_small, "正常", 1, true},           {&gm_jp_small, "警告", 1, true},      // hud
    {&gm_jp_small, "横", 0, false},            {&gm_jp_small, "縦", 0, false},       // sync
    {&gm_jp_small, "上下", 0, false},          {&gm_jp_small, "同期率", 0, true},    // sync
    {&gm_jp_large, "待機", 2, false},          {&gm_jp_large, "加速", 2, false},     // hud titles
    {&gm_jp_large, "制動", 2, false},          {&gm_jp_large, "左旋回", 2, false},
    {&gm_jp_large, "右旋回", 2, false},        {&gm_jp_large, "衝撃", 2, false},
    {&gm_jp_large, "危険", 2, false},
};

static const int k_text_pos[][2] = {{4, 4},  {0, 0},       {-5, 3},      {GM_W - 10, GM_H - 8}, {GM_W + 3, 0},
                                    {-200, 0}, {10, -20},  {10, GM_H - 3}, {GM_W / 2, GM_H / 2}};

static void case_text_jp(gd_t* g, int arg) {
  (void)arg;
  static const int k_layers[] = {0, 2, GM_D - 1, -1, GM_D};
  const int npos = (int)(sizeof(k_text_pos) / sizeof(k_text_pos[0]));
  for (size_t t = 0; t < sizeof(k_jp_texts) / sizeof(k_jp_texts[0]); ++t) {
    const jp_text_t* s = &k_jp_texts[t];
    for (int sp = 0; sp <= 2; ++sp) gd_i32(g, gm_jp_measure(s->font, s->text, sp));
    for (int p = 0; p < npos; ++p)
      for (int l = 0; l < 5; ++l)
        for (int knock = 0; knock < 2; ++knock) {
          const int z = k_layers[l], x = k_text_pos[p][0], y = k_text_pos[p][1];
          gm_frame_clear(&s_frame);
          if (knock)  // reverse video, as the HUD title flashes: clear the glyphs out of a lit block
            for (int yy = y - 2; yy < y + 30; ++yy)
              for (int xx = x - 2; xx < x + 90; ++xx) gm_frame_set(&s_frame, xx, yy, z);
          const float intensity = knock ? -1.0f : 1.0f;
          const int adv = s->vertical ? gm_jp_draw_vertical(&s_frame, s->font, z, x, y, s->text, s->spacing, intensity)
                                      : gm_jp_draw(&s_frame, s->font, z, x, y, s->text, s->spacing, intensity);
          gd_i32(g, adv);
          gd_frame(g, &s_frame);
        }
    gd_step(g, s->text);
  }
}

// The 3x5 font: every printable character, and the strings the engine, the
// effects and the panel test pattern draw.
static void case_text_ascii(gd_t* g, int arg) {
  (void)arg;
  static char printable[96];
  for (int c = 32; c < 127; ++c) printable[c - 32] = (char)c;
  static const char* const k_strings[] = {printable, "BALL", "PARTICLES", "ATFIELD", "0.00G", "1.23G",
                                          "-2147483648.-48G", "P0.45", "+0.25", "-1.50", "DEMO", "SERIAL",
                                          "NO SENSOR", "PANEL 7", "L0 FRONT", "L7 BACK", "100", ""};
  const int npos = (int)(sizeof(k_text_pos) / sizeof(k_text_pos[0]));
  for (size_t i = 0; i < sizeof(k_strings) / sizeof(k_strings[0]); ++i) {
    for (int p = 0; p < npos; ++p)
      for (int z = -1; z <= GM_D; z += 3) {
        gm_frame_clear(&s_frame);
        gd_i32(g, gm_frame_text(&s_frame, z, k_text_pos[p][0], k_text_pos[p][1], k_strings[i]));
        gd_frame(g, &s_frame);
      }
    gd_step(g, k_strings[i]);
  }
}

// The firmware's own drawing: panel_stack_test_pattern for every panel, in
// both stack orientations, and rectangles that run off the glass.
static void case_frame_ops(gd_t* g, int arg) {
  (void)arg;
  for (int reversed = 0; reversed < 2; ++reversed)
    for (int panel = -1; panel <= GM_D; ++panel) {
      gm_frame_clear(&s_frame);
      if (panel >= 0 && panel < GM_D) {
        const int layer = reversed ? (GM_D - 1 - panel) : panel;
        gm_frame_rect(&s_frame, layer, 0, 0, GM_W - 1, GM_H - 1);
        gm_frame_rect(&s_frame, layer, 2, 2, GM_W - 3, GM_H - 3);
        char label[16];
        snprintf(label, sizeof(label), "PANEL %d", panel);
        gm_frame_text(&s_frame, layer, GM_W / 2 - 14, GM_H / 2 - 2, label);
        char where[16];
        snprintf(where, sizeof(where), "L%d%s", layer, layer == 0 ? " FRONT" : (layer == GM_D - 1 ? " BACK" : ""));
        gm_frame_text(&s_frame, layer, 4, 4, where);
      }
      gd_frame(g, &s_frame);
      gd_step(g, "test pattern");
    }
  trng_t r = trng(0xF4A3Eull);
  for (int i = 0; i < 300; ++i) {
    gm_frame_clear(&s_frame);
    for (int k = 0; k < 4; ++k)
      gm_frame_rect(&s_frame, trng_int(&r, -1, GM_D), trng_int(&r, -40, GM_W + 40), trng_int(&r, -40, GM_H + 40),
                    trng_int(&r, -40, GM_W + 40), trng_int(&r, -40, GM_H + 40));
    gd_frame(g, &s_frame);
    gd_step(g, "rectangles");
  }
}

// ---- tables: what `list`, `get` and `status` print -------------------------------------------------
static void case_params_tables(gd_t* g, int arg) {
  (void)arg;
  unit_build(gm_anim_demo());
  gd_i32(g, gm_engine_effect_count(&s_unit.engine));
  for (int i = 0; i < FX_COUNT; ++i) {
    gm_effect_t* e = k_fx[i];
    gd_str(g, gm_effect_name(e));
    gd_i32(g, gm_engine_find_effect(&s_unit.engine, k_fx_names[i]));
    const gm_param_table_t* t = gm_effect_params(e);
    gd_i32(g, gm_params_count(t));
    for (int k = 0; k < gm_params_count(t); ++k) {
      const gm_param_t* p = gm_params_at(t, k);
      gd_str(g, p->name);
      gd_str(g, p->help);
      gd_f32(g, p->min);
      gd_f32(g, p->max);
      gd_f32(g, *p->value);
    }
    gd_step(g, k_fx_names[i]);
  }
  for (int s = -1; s <= GM_ANIM_STATE_COUNT; ++s) gd_str(g, gm_anim_state_name(s));
  gd_bool(g, gm_smoke_allocated(&s_unit.smoke));
  gd_step(g, "anim states");
  unit_release();
}

static void case_scenario_presets(gd_t* g, int arg) {
  (void)arg;
  gd_i32(g, gm_scenario_preset_count());
  for (int i = -1; i <= gm_scenario_preset_count(); ++i) {
    const gm_scenario_t* sc = gm_scenario_preset(i);
    gd_str(g, gm_scenario_name(sc));
    gd_f32(g, gm_scenario_duration(sc));
    gd_bool(g, gm_scenario_find(gm_scenario_name(sc)) == sc);
    // What the demo source plays, at 100 Hz through one wrap.
    for (int64_t us = 0; us < (int64_t)((gm_scenario_duration(sc) + 1.5f) * 1e6f); us += 10000)
      gd_vec(g, demo_g(sc, us).proper);
    gd_step(g, gm_scenario_name(sc));
  }
  gd_bool(g, gm_scenario_find("nope") == NULL);
  gd_bool(g, gm_scenario_find("") == NULL);
  gd_step(g, "find");
}

static void case_demo_assets(gd_t* g, int arg) {
  (void)arg;
  const gm_sprite_sheet_t* sh = gm_sprite_demo_sheet();
  gd_i32(g, sh->width);
  gd_i32(g, sh->height);
  gd_i32(g, sh->frames);
  gd_bytes(g, sh->bits, (size_t)(gm_sprite_sheet_frame_bytes(sh) * sh->frames));
  gd_step(g, "sprite sheet");
  const gm_anim_asset_t* assets[2] = {gm_anim_demo(), &kUserAnimation};
  for (int k = 0; k < 2; ++k) {
    const gm_anim_asset_t* a = assets[k];
    gd_str(g, a->name);
    gd_i32(g, a->width);
    gd_i32(g, a->height);
    gd_i32(g, a->depth);
    gd_i32(g, a->frames);
    gd_bytes(g, a->bits, (size_t)gm_anim_asset_total_bytes(a));
    for (int s = 0; s < GM_ANIM_STATE_COUNT; ++s) {
      gd_i32(g, a->clips[s].start);
      gd_i32(g, a->clips[s].count);
      gd_f32(g, a->clips[s].fps);
      gd_i32(g, a->clips[s].loop);
    }
    float m[sizeof(a->mapping) / sizeof(float)];  // the mapping is 13 floats
    memcpy(m, &a->mapping, sizeof(m));
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); ++i) gd_f32(g, m[i]);
    gd_step(g, a->name);
  }
}

// ---- registration -------------------------------------------------------------------------------------
__attribute__((constructor)) static void register_cases(void) {
  char name[64];
  for (int fx = 0; fx < FX_COUNT; ++fx)
    for (int sc = 0; sc < gm_scenario_preset_count(); ++sc) {
      snprintf(name, sizeof(name), "fx_%s_%s", k_fx_names[fx], gm_scenario_name(gm_scenario_preset(sc)));
      golden_register(name, case_fx, fx | sc << 8);
    }
  golden_register("fx_anim_user_drive", case_fx, fx_index("anim") | (gm_scenario_preset_count() - 1) << 8 | 1 << 16);
  for (int fx = 0; fx < FX_COUNT; ++fx) {
    snprintf(name, sizeof(name), "fuzz_%s", k_fx_names[fx]);
    golden_register(name, case_fuzz, fx);
  }
  golden_register("fuzz_anim_user", case_fuzz, fx_index("anim") | 1 << 16);
  for (int fx = 0; fx < FX_COUNT; ++fx) {
    snprintf(name, sizeof(name), "nonfinite_%s", k_fx_names[fx]);
    golden_register(name, case_nonfinite, fx);
  }
  golden_register("engine_standard_a", case_engine_standard, 1);
  golden_register("engine_standard_b", case_engine_standard, 2);
  golden_register("engine_standard_cycle", case_engine_cycle, 0);
  golden_register("engine_hud_a", case_engine_hud, 1);
  golden_register("engine_hud_b", case_engine_hud, 2);
  golden_register("engine_hud_orbit", case_engine_hud_orbit, 0);
  golden_register("estimator_filter", case_estimator_filter, 0);
  golden_register("estimator_autozero", case_estimator_autozero, 0);
  golden_register("estimator_rest_cal", case_estimator_rest_cal, 0);
  golden_register("estimator_brake_cal", case_estimator_brake_cal, 0);
  golden_register("gstate_scripted", case_gstate_scripted, 0);
  golden_register("gstate_random", case_gstate_random, 0);
  golden_register("text_jp", case_text_jp, 0);
  golden_register("text_ascii", case_text_ascii, 0);
  golden_register("frame_ops", case_frame_ops, 0);
  golden_register("params_tables", case_params_tables, 0);
  golden_register("scenario_presets", case_scenario_presets, 0);
  golden_register("demo_assets", case_demo_assets, 0);
}
