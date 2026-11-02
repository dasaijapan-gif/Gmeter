// Scripted g-force profiles for the demo mode.
#include "gm_scenario.h"

#include <string.h>

gm_vec3_t gm_scenario_dynamic_at(const gm_scenario_t* s, float t) {
  if (s->count == 0) return gm_vec3_zero();
  if (t <= s->keys[0].t) return s->keys[0].dynamic;
  if (t >= s->keys[s->count - 1].t) return s->keys[s->count - 1].dynamic;
  int i = 0;
  while (i + 1 < s->count && s->keys[i + 1].t < t) ++i;
  const gm_gkey_t* a = &s->keys[i];
  const gm_gkey_t* b = &s->keys[i + 1];
  const float span = b->t - a->t;
  const float u = span > 1e-6f ? gm_smoothstepf(0.0f, 1.0f, (t - a->t) / span) : 1.0f;
  return gm_vec3_lerp(a->dynamic, b->dynamic, u);
}

gm_gforce_t gm_scenario_at(const gm_scenario_t* s, float t) {
  return gm_gforce_from_proper(gm_vec3_add(gm_gforce_rest_proper(), gm_scenario_dynamic_at(s, t)));
}

// ---- presets ---------------------------------------------------------------------------
// Dynamic g: x lateral (+ = right turn), y vertical (+ = pushed up), z longitudinal
// (+ = accelerating forward).
static const gm_gkey_t k_idle[] = {{0.0f, {0, 0, 0}}, {6.0f, {0, 0, 0}}};
static const gm_gkey_t k_launch[] = {{0.0f, {0, 0, 0}},     {0.5f, {0, 0, 0}},     {0.9f, {0, 0, 0.55f}},
                                     {2.2f, {0, 0, 0.5f}},  {2.5f, {0, 0, 0.25f}}, {2.8f, {0, 0, 0.45f}},
                                     {4.2f, {0, 0, 0.3f}},  {5.0f, {0, 0, 0}},     {6.5f, {0, 0, 0}}};
static const gm_gkey_t k_brake[] = {{0.0f, {0, 0, 0}},       {0.6f, {0, 0, 0}},          {0.9f, {0, 0, -0.75f}},
                                    {2.6f, {0, 0, -0.8f}},   {3.1f, {0, 0, -0.15f}},     {3.35f, {0, 0.05f, 0.08f}},
                                    {3.8f, {0, 0, 0}},       {6.0f, {0, 0, 0}}};
static const gm_gkey_t k_slalom[] = {{0.0f, {0, 0, 0}},     {0.8f, {0.65f, 0, 0}},  {1.8f, {-0.65f, 0, 0}},
                                     {2.8f, {0.65f, 0, 0}}, {3.8f, {-0.65f, 0, 0}}, {4.8f, {0.65f, 0, 0}},
                                     {5.8f, {-0.65f, 0, 0}}, {6.8f, {0.6f, 0, 0}},  {7.8f, {-0.55f, 0, 0}},
                                     {8.8f, {0, 0, 0}},     {10.0f, {0, 0, 0}}};
static const gm_gkey_t k_corner[] = {{0.0f, {0, 0, 0}},          {0.6f, {0, 0, 0}},        {1.6f, {0.6f, 0, -0.1f}},
                                     {4.0f, {0.6f, 0, 0.05f}},   {5.0f, {0, 0, 0.1f}},     {6.0f, {-0.55f, 0, 0}},
                                     {8.0f, {-0.55f, 0, 0.05f}}, {9.0f, {0, 0, 0}},        {10.0f, {0, 0, 0}}};
static const gm_gkey_t k_bump[] = {{0.0f, {0, 0, 0}},      {1.0f, {0, 0, 0}},      {1.08f, {0, 0.6f, 0}},
                                   {1.16f, {0, -0.5f, 0}}, {1.24f, {0, 0.25f, 0}}, {1.34f, {0, -0.1f, 0}},
                                   {1.45f, {0, 0, 0}},     {2.6f, {0, 0, 0}},      {2.68f, {0, 0.4f, 0}},
                                   {2.76f, {0, -0.35f, 0}}, {2.9f, {0, 0, 0}},     {4.5f, {0, 0, 0}}};
static const gm_gkey_t k_drive[] = {
    {0.0f, {0, 0, 0}},          {0.8f, {0, 0, 0}},           {1.3f, {0, 0, 0.5f}},       {3.0f, {0, 0, 0.35f}},
    {3.6f, {0, 0, 0.05f}},      {5.0f, {0, 0, 0}},           {5.5f, {0, 0, -0.6f}},      {6.8f, {0.2f, 0, -0.5f}},
    {7.6f, {0.7f, 0, -0.1f}},   {9.5f, {0.7f, 0, 0.1f}},     {10.2f, {0.2f, 0, 0.2f}},   {10.5f, {0, 0, 0.2f}},
    {10.58f, {0, 0.55f, 0.2f}}, {10.66f, {0, -0.45f, 0.2f}}, {10.8f, {0, 0, 0.2f}},      {12.0f, {-0.1f, 0, 0.1f}},
    {12.8f, {-0.6f, 0, 0}},     {14.2f, {-0.6f, 0, 0.1f}},   {15.0f, {-0.1f, 0, 0.5f}},  {16.3f, {0, 0, 0.45f}},
    {17.0f, {0, 0, 0}},         {17.6f, {0, 0, -0.8f}},      {18.9f, {0, 0, -0.85f}},    {19.3f, {0, 0, -0.2f}},
    {19.5f, {0, 0.05f, 0.1f}},  {20.0f, {0, 0, 0}},          {21.0f, {0, 0, 0}}};

#define GM_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const gm_scenario_t k_presets[] = {
    {"idle", k_idle, GM_COUNT(k_idle)},       {"launch", k_launch, GM_COUNT(k_launch)},
    {"brake", k_brake, GM_COUNT(k_brake)},    {"slalom", k_slalom, GM_COUNT(k_slalom)},
    {"corner", k_corner, GM_COUNT(k_corner)}, {"bump", k_bump, GM_COUNT(k_bump)},
    {"drive", k_drive, GM_COUNT(k_drive)},
};

int gm_scenario_preset_count(void) { return GM_COUNT(k_presets); }

const gm_scenario_t* gm_scenario_preset(int i) { return &k_presets[gm_clampi(i, 0, gm_scenario_preset_count() - 1)]; }

const gm_scenario_t* gm_scenario_find(const char* name) {
  for (int i = 0; i < gm_scenario_preset_count(); ++i)
    if (strcmp(k_presets[i].name, name) == 0) return &k_presets[i];
  return NULL;
}
