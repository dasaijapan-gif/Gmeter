// Runs the active effect, handles mode switching with a dithered crossfade,
// and overlays optional HUD text. Owns no hardware: feed it a g-force and a
// dt, read back the frame. A gm_engine_t holds three frames (about 21-24 KB):
// keep it in static or heap memory.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_ENGINE_MAX_EFFECTS 12

typedef struct {
  gm_effect_t* effects[GM_ENGINE_MAX_EFFECTS];
  int count;
  int current;
  int outgoing;
  float crossfade_t;
  float crossfade_seconds;
  float auto_cycle;
  float since_switch;
  bool show_mode_name;
  bool readout;
  const char* status_label;
  gm_effect_ctx_t ctx;
  gm_frame_t out, scratch_a, scratch_b;
} gm_engine_t;

void gm_engine_init(gm_engine_t* e);

// Effects are not owned; keep them alive for the engine's lifetime. The first
// effect added becomes current (and is activated).
bool gm_engine_add_effect(gm_engine_t* e, gm_effect_t* fx);
static inline int gm_engine_effect_count(const gm_engine_t* e) { return e->count; }
static inline gm_effect_t* gm_engine_effect(const gm_engine_t* e, int i) {
  return (i >= 0 && i < e->count) ? e->effects[i] : 0;
}
static inline gm_effect_t* gm_engine_current(const gm_engine_t* e) { return gm_engine_effect(e, e->current); }
static inline int gm_engine_current_index(const gm_engine_t* e) { return e->current; }
// Index of the effect with this name, or -1.
int gm_engine_find_effect(const gm_engine_t* e, const char* name);

void gm_engine_set_effect(gm_engine_t* e, int index, bool crossfade);
bool gm_engine_set_effect_by_name(gm_engine_t* e, const char* name, bool crossfade);
// Switches to the next effect with a crossfade.
void gm_engine_next_effect(gm_engine_t* e);

// Seconds between automatic mode changes; 0 disables.
static inline void gm_engine_set_auto_cycle(gm_engine_t* e, float seconds) { e->auto_cycle = seconds; }
static inline void gm_engine_set_crossfade_seconds(gm_engine_t* e, float s) { e->crossfade_seconds = s; }
// Shows the effect name on the front layer briefly after a switch.
static inline void gm_engine_set_show_mode_name(gm_engine_t* e, bool on) { e->show_mode_name = on; }
// Draws the horizontal g readout on the front layer for every effect.
static inline void gm_engine_set_readout(gm_engine_t* e, bool on) { e->readout = on; }
// Short tag drawn bottom-right on the front layer (e.g. "DEMO" while the g is
// not live sensor data); NULL or "" hides it. Keep the string alive.
static inline void gm_engine_set_status_label(gm_engine_t* e, const char* text) { e->status_label = text; }

// Advance by dt seconds with the given (already filtered) g and render.
void gm_engine_step(gm_engine_t* e, gm_gforce_t g, float dt);
static inline const gm_frame_t* gm_engine_frame(const gm_engine_t* e) { return &e->out; }
static inline float gm_engine_time(const gm_engine_t* e) { return e->ctx.time; }
static inline uint32_t gm_engine_frame_count(const gm_engine_t* e) { return e->ctx.frame; }

// Parameter access on the current effect.
bool gm_engine_set_param(gm_engine_t* e, const char* name, float v);

#ifdef __cplusplus
}
#endif
