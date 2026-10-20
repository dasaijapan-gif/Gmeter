// Runs the active effect, handles mode switching with a dithered crossfade,
// and overlays optional HUD text.
#include "gm_engine.h"

#include <stdio.h>
#include <string.h>

void gm_engine_init(gm_engine_t* e) {
  for (int i = 0; i < GM_ENGINE_MAX_EFFECTS; ++i) e->effects[i] = NULL;
  e->count = 0;
  e->current = -1;
  e->outgoing = -1;
  e->crossfade_t = 1.0f;
  e->crossfade_seconds = 0.4f;
  e->auto_cycle = 0.0f;
  e->since_switch = 0.0f;
  e->show_mode_name = true;
  e->readout = false;
  e->status_label = NULL;
  const gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;
  e->ctx = ctx;
  gm_frame_clear(&e->out);
  gm_frame_clear(&e->scratch_a);
  gm_frame_clear(&e->scratch_b);
}

bool gm_engine_add_effect(gm_engine_t* e, gm_effect_t* fx) {
  if (!fx || e->count >= GM_ENGINE_MAX_EFFECTS) return false;
  e->effects[e->count++] = fx;
  if (e->current < 0) {
    e->current = 0;
    gm_effect_activate(fx);
  }
  return true;
}

int gm_engine_find_effect(const gm_engine_t* e, const char* name) {
  for (int i = 0; i < e->count; ++i)
    if (strcmp(gm_effect_name(e->effects[i]), name) == 0) return i;
  return -1;
}

void gm_engine_set_effect(gm_engine_t* e, int index, bool crossfade) {
  if (index < 0 || index >= e->count || index == e->current) return;
  if (e->outgoing >= 0 && e->outgoing != index) gm_effect_deactivate(e->effects[e->outgoing]);
  e->outgoing = crossfade && e->crossfade_seconds > 0.0f ? e->current : -1;
  if (!crossfade && e->current >= 0) gm_effect_deactivate(e->effects[e->current]);
  e->current = index;
  gm_effect_activate(e->effects[e->current]);
  e->crossfade_t = e->outgoing >= 0 ? 0.0f : 1.0f;
  e->since_switch = 0.0f;
}

bool gm_engine_set_effect_by_name(gm_engine_t* e, const char* name, bool crossfade) {
  const int i = gm_engine_find_effect(e, name);
  if (i < 0) return false;
  gm_engine_set_effect(e, i, crossfade);
  return true;
}

void gm_engine_next_effect(gm_engine_t* e) {
  if (e->count) gm_engine_set_effect(e, (e->current + 1) % e->count, true);
}

void gm_engine_step(gm_engine_t* e, gm_gforce_t g, float dt) {
  dt = gm_clampf(dt, 1.0f / 1000.0f, 1.0f / 15.0f);
  e->ctx.g = g;
  e->ctx.dt = dt;
  e->ctx.time += dt;
  ++e->ctx.frame;
  e->since_switch += dt;

  if (e->auto_cycle > 0.0f && e->since_switch >= e->auto_cycle && e->count > 1) gm_engine_next_effect(e);

  gm_effect_t* cur = gm_engine_current(e);
  if (!cur) {
    gm_frame_clear(&e->out);
    return;
  }

  gm_effect_update(cur, &e->ctx);
  int dx = 0, dy = 0;  // burn-in offset, applied to the whole frame at the end
  gm_effect_burn_in_offset(cur, &dx, &dy);
  if (e->outgoing >= 0) {
    gm_effect_t* old = e->effects[e->outgoing];
    gm_effect_update(old, &e->ctx);
    e->crossfade_t += dt / e->crossfade_seconds;
    gm_frame_clear(&e->scratch_a);
    gm_frame_clear(&e->scratch_b);
    gm_effect_render(old, &e->scratch_a);
    // Keep the outgoing effect at its own offset through the dissolve.
    int odx = 0, ody = 0;
    gm_effect_burn_in_offset(old, &odx, &ody);
    if (odx != dx || ody != dy) gm_frame_shift(&e->scratch_a, odx - dx, ody - dy);
    gm_effect_render(cur, &e->scratch_b);
    gm_frame_crossfade(&e->out, &e->scratch_a, &e->scratch_b, e->crossfade_t);
    if (e->crossfade_t >= 1.0f) {
      gm_effect_deactivate(old);
      e->outgoing = -1;
    }
  } else {
    gm_frame_clear(&e->out);
    gm_effect_render(cur, &e->out);
  }

  if (e->show_mode_name && e->since_switch < 1.5f) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%s", gm_effect_name(cur));
    for (char* p = buf; *p; ++p) *p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    gm_frame_clear_box(&e->out, 0, 0, 1 + 4 * (int)strlen(buf), 6);
    gm_frame_text(&e->out, 0, 1, 1, buf);
  }
  if (e->readout) {
    char buf[12];
    const float hg = gm_gforce_horizontal(g);
    snprintf(buf, sizeof(buf), "%d.%02dG", (int)hg, (int)(hg * 100.0f) % 100);
    gm_frame_clear_box(&e->out, 0, GM_H - 7, 1 + 4 * (int)strlen(buf), GM_H - 1);
    gm_frame_text(&e->out, 0, 1, GM_H - 6, buf);
  }
  if (e->status_label && *e->status_label) {
    const int w = 4 * (int)strlen(e->status_label) - 1;
    const int x0 = GM_W - 2 - w;
    gm_frame_clear_box(&e->out, x0 - 1, GM_H - 7, GM_W - 1, GM_H - 1);
    gm_frame_text(&e->out, 0, x0, GM_H - 6, e->status_label);
  }
  // Last, so the overlays move with the picture.
  if (dx || dy) gm_frame_shift(&e->out, dx, dy);
}

bool gm_engine_set_param(gm_engine_t* e, const char* name, float v) {
  gm_effect_t* cur = gm_engine_current(e);
  return cur ? gm_params_set(gm_effect_params(cur), name, v) : false;
}
