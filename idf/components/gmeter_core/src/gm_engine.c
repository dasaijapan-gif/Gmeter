// Runs the active effect, handles mode switching with a dithered crossfade,
// and overlays optional HUD text.
#include "gm_engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void gm_engine_init(gm_engine_t* e) {
  memset(e, 0, sizeof *e);
  e->current = -1;
  e->outgoing = -1;
  e->crossfade_t = 1.0f;
  const gm_effect_ctx_t ctx = GM_EFFECT_CTX_INIT;  // at rest until the first step
  e->ctx = ctx;
}

bool gm_engine_add_effect(gm_engine_t* e, gm_effect_t* fx) {
  if (!fx || e->count >= GM_ENGINE_MAX_EFFECTS) return false;
  e->effects[e->count++] = fx;
  if (e->current < 0) {
    e->current = 0;
    gm_effect_reset(fx);  // activation
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
  e->outgoing = crossfade ? e->current : -1;
  if (!crossfade && e->current >= 0) gm_effect_deactivate(e->effects[e->current]);
  e->current = index;
  gm_effect_reset(e->effects[e->current]);  // activation
  e->crossfade_t = e->outgoing >= 0 ? 0.0f : 1.0f;
  e->since_switch = 0.0f;
}

void gm_engine_next_effect(gm_engine_t* e) {
  if (e->count) gm_engine_set_effect(e, (e->current + 1) % e->count, true);
}

// The g the effects see (see gm_engine_step in gm_engine.h).
static gm_gforce_t sanitize(gm_gforce_t g, gm_gforce_t previous) {
  const gm_vec3_t p = g.proper;
  if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) return previous;
  const float m = GM_ENGINE_MAX_G;
  return gm_gforce_from_proper(gm_vec3(gm_clampf(p.x, -m, m), gm_clampf(p.y, -m, m), gm_clampf(p.z, -m, m)));
}

void gm_engine_step(gm_engine_t* e, gm_gforce_t g, float dt) {
  dt = gm_clampf(dt, 1.0f / 1000.0f, 1.0f / 15.0f);
  g = sanitize(g, e->ctx.g);
  e->ctx.g = g;
  e->ctx.dt = dt;
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
  gm_effect_t* old = e->outgoing >= 0 ? e->effects[e->outgoing] : NULL;
  if (old) {
    gm_effect_update(old, &e->ctx);
    e->crossfade_t += dt / GM_ENGINE_CROSSFADE_S;
    gm_frame_clear(&e->scratch);
    gm_effect_render(old, &e->scratch);
    // Keep the outgoing effect at its own offset through the dissolve.
    int odx = 0, ody = 0;
    gm_effect_burn_in_offset(old, &odx, &ody);
    if (odx != dx || ody != dy) gm_frame_shift(&e->scratch, odx - dx, ody - dy);
  }
  gm_frame_clear(&e->out);
  gm_effect_render(cur, &e->out);
  if (old) {
    // Dissolve in place: the outgoing picture in scratch, the incoming one in out.
    gm_frame_crossfade(&e->out, &e->scratch, &e->out, e->crossfade_t);
    if (e->crossfade_t >= 1.0f) {
      gm_effect_deactivate(old);
      e->outgoing = -1;
    }
  }

  if (e->since_switch < 1.5f) {  // the effect's name, briefly after a switch
    char buf[20];
    snprintf(buf, sizeof(buf), "%s", gm_effect_name(cur));
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
