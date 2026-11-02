// An effect turns the current g-force into voxels. Effects are plain C and
// run identically in the firmware and in host tests.
//
// Every effect is a struct whose first member is a gm_effect_t (`base`) and
// whose vtable gives its name and behaviour. Its init function (for example
// gm_hud_init) fills in the vtable, registers the tunables and sets the
// defaults; after that the effect is used through the gm_effect_* calls below
// (or the engine), passing &effect.base.
#pragma once

#include <stdbool.h>

#include "gm_frame.h"
#include "gm_gforce.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- parameters (gm_params.c) ---------------------------------------------------------
// Tiny registry so every effect's tunables can be listed and set by name from
// the serial command line.
#define GM_PARAMS_MAX 24

typedef struct {
  const char* name;
  float* value;
  float min, max;
  const char* help;
} gm_param_t;

typedef struct {
  gm_param_t items[GM_PARAMS_MAX];
  int count;
} gm_param_table_t;

// Empties the table.
void gm_params_init(gm_param_table_t* t);
// Registers a tunable; ignored once the table is full. `help` may be "".
void gm_params_add(gm_param_table_t* t, const char* name, float* value, float min, float max, const char* help);
static inline int gm_params_count(const gm_param_table_t* t) { return t->count; }
static inline const gm_param_t* gm_params_at(const gm_param_table_t* t, int i) { return &t->items[i]; }
// NULL when there is no parameter of that name.
const gm_param_t* gm_params_find(const gm_param_table_t* t, const char* name);
// Clamps to the parameter's range. False for an unknown name or a non-finite value.
bool gm_params_set(gm_param_table_t* t, const char* name, float v);
bool gm_params_get(const gm_param_table_t* t, const char* name, float* out);

// ---- effects --------------------------------------------------------------------------
typedef struct {
  // Filtered g-force, car frame. The engine sanitises it: finite, and each
  // axis within +-GM_ENGINE_MAX_G (16 g).
  gm_gforce_t g;
  float dt;  // seconds since the previous update (clamped by the engine)
} gm_effect_ctx_t;

#define GM_EFFECT_CTX_INIT {GM_GFORCE_REST_INIT, 1.0f / 60.0f}

typedef struct gm_effect gm_effect_t;

typedef struct {
  const char* name;
  // Called when the effect stops being shown (may be NULL). Heavy effects free
  // their grids here and allocate them again in reset, so several big effects
  // can coexist on a small heap.
  void (*deactivate)(gm_effect_t* self);
  // Back to the start state; the engine calls it whenever the effect becomes
  // active (may be NULL).
  void (*reset)(gm_effect_t* self);
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

// For an effect's init function: sets the vtable and empties the parameter
// table, which it returns for gm_params_add.
static inline gm_param_table_t* gm_effect_init(gm_effect_t* e, const gm_effect_vtbl_t* vt) {
  e->vt = vt;
  gm_params_init(&e->params);
  return &e->params;
}
static inline const char* gm_effect_name(const gm_effect_t* e) { return e->vt->name; }
static inline void gm_effect_reset(gm_effect_t* e) {
  if (e->vt->reset) e->vt->reset(e);
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
