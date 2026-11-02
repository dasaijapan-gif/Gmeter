// Minimal harness for the host tests of the core.
//
// Behaviour tests: TEST(name) { CHECK(...); } in any test_*.c file. A failed
// CHECK prints file:line and fails the test; the run goes on.
//
// Reference-output ("golden") cases: a function that drives the core and folds
// every output into a 64-bit FNV-1a digest (gd_* below), registered with
// golden_register. main.c compares each digest with the one recorded in
// golden_<panel height>.txt (see README.md).
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gmeter.h"

// ---- behaviour tests --------------------------------------------------------------------
typedef void (*test_fn_t)(void);
void test_register(const char* name, test_fn_t fn, const char* file, int line);
void test_check(bool ok, const char* what, const char* file, int line);
// Prints a message under the current test.
void test_note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define TEST(name)                                                                                          \
  static void name(void);                                                                                   \
  __attribute__((constructor)) static void name##_register(void) { test_register(#name, name, __FILE__, __LINE__); } \
  static void name(void)
#define CHECK(x) test_check((x), #x, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) \
  test_check(fabsf((float)(a) - (float)(b)) <= (float)(tol), #a " ~= " #b, __FILE__, __LINE__)

// ---- reference-output cases ------------------------------------------------------------
typedef struct {
  uint64_t h;    // running FNV-1a digest of everything folded so far
  long step;     // steps completed (gd_step)
  FILE* dump;    // --dump: one line per step, else NULL
  // --against: the per-step digests of an earlier dump, to find the first step that differs.
  const uint64_t* ref;
  long ref_count;
  long first_diff;  // first step whose digest differs from ref, or -1
} gd_t;

typedef void (*golden_fn_t)(gd_t* g, int arg);
// `name` is copied. Cases run in registration order.
void golden_register(const char* name, golden_fn_t fn, int arg);

static inline void gd_bytes(gd_t* g, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  uint64_t h = g->h;
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 0x100000001B3ull;
  }
  g->h = h;
}
static inline void gd_u32(gd_t* g, uint32_t v) {
  const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  gd_bytes(g, b, 4);
}
static inline void gd_i32(gd_t* g, int v) { gd_u32(g, (uint32_t)v); }
static inline void gd_bool(gd_t* g, bool v) { gd_u32(g, v ? 1u : 0u); }
// The bit pattern, except that every NaN folds the same (which NaN an
// operation returns is not something the firmware relies on).
static inline void gd_f32(gd_t* g, float v) {
  uint32_t u;
  memcpy(&u, &v, sizeof(u));
  gd_u32(g, isnan(v) ? 0x7FC00000u : u);
}
static inline void gd_vec(gd_t* g, gm_vec3_t v) {
  gd_f32(g, v.x);
  gd_f32(g, v.y);
  gd_f32(g, v.z);
}
// NULL folds differently from "".
static inline void gd_str(gd_t* g, const char* s) {
  if (!s) {
    gd_u32(g, 0xFFFFFFFFu);
    return;
  }
  gd_bytes(g, s, strlen(s) + 1);
}
static inline void gd_frame(gd_t* g, const gm_frame_t* f) { gd_bytes(g, f->bits, sizeof(f->bits)); }
// Ends a step: --dump prints the running digest, --against compares it.
void gd_step(gd_t* g, const char* what);
// A line in the --dump output that explains the next step (not folded).
void gd_event(gd_t* g, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- deterministic inputs ------------------------------------------------------------------
// splitmix64, with an explicit mapping to floats, so the inputs are the same
// on every host and compiler.
typedef struct {
  uint64_t s;
} trng_t;

static inline trng_t trng(uint64_t seed) {
  trng_t r = {seed};
  return r;
}
static inline uint64_t trng_next(trng_t* r) {
  uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
// Uniform in [0, 1): the top 24 bits, exactly representable.
static inline float trng_unit(trng_t* r) { return (float)(trng_next(r) >> 40) * 0x1.0p-24f; }
// Uniform in [lo, hi).
static inline float trng_range(trng_t* r, float lo, float hi) { return lo + (hi - lo) * trng_unit(r); }
// Uniform in [lo, hi], inclusive.
static inline int trng_int(trng_t* r, int lo, int hi) {
  return lo + (int)(trng_next(r) % (uint64_t)((int64_t)hi - lo + 1));
}
static inline bool trng_chance(trng_t* r, float p) { return trng_unit(r) < p; }

// ---- frame inspection ----------------------------------------------------------------------
// Straight from the bytes (SSD130x page layout), so the tests need no reader
// functions in the core.
static inline bool px(const gm_frame_t* f, int x, int y, int z) {
  if (x < 0 || x >= GM_W || y < 0 || y >= GM_H || z < 0 || z >= GM_D) return false;
  return (f->bits[z][(y >> 3) * GM_W + x] >> (y & 7)) & 1u;
}
static inline int lit_layer(const gm_frame_t* f, int z) {
  int n = 0;
  for (size_t i = 0; i < sizeof(f->bits[z]); ++i) n += __builtin_popcount(f->bits[z][i]);
  return n;
}
static inline int lit(const gm_frame_t* f) {
  int n = 0;
  for (int z = 0; z < GM_D; ++z) n += lit_layer(f, z);
  return n;
}
