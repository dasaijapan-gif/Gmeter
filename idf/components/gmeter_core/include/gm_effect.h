// An effect turns the current g-force into voxels. Effects are plain C and
// run identically in the firmware and in host tests.
//
// Every effect is a struct whose first member is a gm_effect_t (`base`) and
// whose vtable gives its name and behaviour. Its init function (for example
// gm_hud_init) fills in the vtable, registers the tunables and sets the
// defaults; after that the effect is used through the gm_effect_* calls below
// (or the engine), passing &effect.base.
#pragma once

#include <stdint.h>

#include "gm_frame.h"
#include "gm_gforce.h"
#include "gm_params.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  gm_gforce_t g;   // filtered g-force, car frame
  float dt;        // seconds since the previous update (clamped by the engine)
  float time;      // seconds since the engine started
  uint32_t frame;  // frame counter
} gm_effect_ctx_t;

#define GM_EFFECT_CTX_INIT {GM_GFORCE_REST_INIT, 1.0f / 60.0f, 0.0f, 0u}

typedef struct gm_effect gm_effect_t;

typedef struct {
  const char* name;
  // Called when the effect becomes active / inactive. Heavy effects allocate
  // their grids in activate and free them in deactivate so several big
  // effects can coexist on a small heap. NULL activate means "call reset".
  void (*activate)(gm_effect_t* self);
  void (*deactivate)(gm_effect_t* self);  // may be NULL
  void (*reset)(gm_effect_t* self);       // may be NULL
  // Advance the simulation. May be called more than once per rendered frame.
  void (*update)(gm_effect_t* self, const gm_effect_ctx_t* ctx);
  // Draw into a cleared frame.
  void (*render)(gm_effect_t* self, gm_frame_t* frame);
  // Offset in pixels for the whole picture, against OLED burn-in. The engine
  // applies it after its own overlays, so everything on the glass moves.
  // NULL means (0, 0).
  void (*burn_in_offset)(const gm_effect_t* self, int* dx, int* dy);
} gm_effect_vtbl_t;

struct gm_effect {
  const gm_effect_vtbl_t* vt;
  gm_param_table_t params;
};

static inline const char* gm_effect_name(const gm_effect_t* e) { return e->vt->name; }
static inline void gm_effect_reset(gm_effect_t* e) {
  if (e->vt->reset) e->vt->reset(e);
}
static inline void gm_effect_activate(gm_effect_t* e) {
  if (e->vt->activate)
    e->vt->activate(e);
  else
    gm_effect_reset(e);
}
static inline void gm_effect_deactivate(gm_effect_t* e) {
  if (e->vt->deactivate) e->vt->deactivate(e);
}
static inline void gm_effect_update(gm_effect_t* e, const gm_effect_ctx_t* ctx) { e->vt->update(e, ctx); }
static inline void gm_effect_render(gm_effect_t* e, gm_frame_t* f) { e->vt->render(e, f); }
static inline void gm_effect_burn_in_offset(const gm_effect_t* e, int* dx, int* dy) {
  if (e->vt->burn_in_offset) {
    e->vt->burn_in_offset(e, dx, dy);
  } else {
    *dx = 0;
    *dy = 0;
  }
}
static inline gm_param_table_t* gm_effect_params(gm_effect_t* e) { return &e->params; }

#ifdef __cplusplus
}
#endif
