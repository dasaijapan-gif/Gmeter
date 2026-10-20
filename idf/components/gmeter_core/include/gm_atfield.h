// Effect "atfield": octagon shockwaves through the stack, after the A.T.
// fields of Neon Genesis Evangelion.
#pragma once

#include <stdbool.h>

#include "gm_effect.h"
#include "gm_gstate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_ATFIELD_MAX_RIPPLES 12

typedef struct {
  gm_vec3_t origin;
  float age;
  float strength;
  bool active;
} gm_atfield_ripple_t;

typedef struct {
  gm_effect_t base;

  float speed;        // wavefront speed px/s
  float life;         // seconds a ripple lives
  float rate_g;       // ripples per second per g above min_g
  float min_g;        // horizontal g below which only the idle pulse runs
  float step_g;       // a strong ripple each time g crosses a multiple
  float idle;         // slow idle pulse from the centre
  float double_ring;  // second, inner octagon (parameter "double")
  float label;        // A.T.フィールド / 全開 captions
  float bump_g;

  gm_atfield_ripple_t ripples[GM_ATFIELD_MAX_RIPPLES];
  gm_gstate_t tracker;
  float emit_accum;
  float idle_timer;
  int last_step;
  float horiz;
  float time;
  float caption_age;  // time since the last strong ripple
} gm_atfield_t;

void gm_atfield_init(gm_atfield_t* a);
int gm_atfield_active_ripples(const gm_atfield_t* a);

#ifdef __cplusplus
}
#endif
