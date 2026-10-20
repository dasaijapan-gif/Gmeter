// Tiny parameter registry so every effect's tunables can be listed and set by
// name from the serial command line.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
