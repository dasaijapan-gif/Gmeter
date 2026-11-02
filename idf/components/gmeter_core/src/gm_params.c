// Tiny parameter registry so every effect's tunables can be listed and set by
// name from the serial command line.
#include "gm_effect.h"

#include <math.h>
#include <string.h>

#include "gm_math.h"

void gm_params_init(gm_param_table_t* t) { t->count = 0; }

void gm_params_add(gm_param_table_t* t, const char* name, float* value, float min, float max, const char* help) {
  if (t->count < GM_PARAMS_MAX) t->items[t->count++] = (gm_param_t){name, value, min, max, help};
}

const gm_param_t* gm_params_find(const gm_param_table_t* t, const char* name) {
  for (int i = 0; i < t->count; ++i)
    if (strcmp(t->items[i].name, name) == 0) return &t->items[i];
  return NULL;
}

bool gm_params_set(gm_param_table_t* t, const char* name, float v) {
  const gm_param_t* p = gm_params_find(t, name);
  if (!p || !isfinite(v)) return false;
  *p->value = gm_clampf(v, p->min, p->max);
  return true;
}

bool gm_params_get(const gm_param_table_t* t, const char* name, float* out) {
  const gm_param_t* p = gm_params_find(t, name);
  if (!p) return false;
  *out = *p->value;
  return true;
}
