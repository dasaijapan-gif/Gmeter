// Effect "sync": sync-graph traces of each g axis, after the synchronisation
// readouts of Neon Genesis Evangelion.
#pragma once

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_SYNC_HIST GM_W

typedef struct {
  gm_effect_t base;

  float rate;    // samples per second (window = GM_W / rate)
  float gain;    // px per g
  float grid;
  float labels;

  float hist[3][GM_SYNC_HIST];
  int head;
  float accum;
  float scroll;
  float sync_pct;
} gm_sync_t;

void gm_sync_init(gm_sync_t* s);

#ifdef __cplusplus
}
#endif
