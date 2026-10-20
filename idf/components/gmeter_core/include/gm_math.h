// Small float vector math. Single precision only: the ESP32 has a hardware FPU
// for float but emulates double in software, so never use double literals or
// the double versions of the libm functions (sqrt, sin, ...): use sqrtf, sinf.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GM_PI 3.14159265358979f
#define GM_TWO_PI 6.28318530717959f
#define GM_STANDARD_GRAVITY 9.80665f  // m/s^2 per g

// NaN-safe: a NaN input returns lo, so bad input can never poison state.
static inline float gm_clampf(float v, float lo, float hi) {
  if (!(v >= lo)) return lo;
  if (!(v <= hi)) return hi;
  return v;
}
static inline int gm_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float gm_lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float gm_smoothstepf(float e0, float e1, float x) {
  float t = gm_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
static inline float gm_signf(float v) { return v < 0.0f ? -1.0f : 1.0f; }
static inline int gm_ifloor(float v) { return (int)floorf(v); }
static inline int gm_iround(float v) { return (int)floorf(v + 0.5f); }
// Smaller / larger of two values; the first argument wins a tie.
static inline float gm_minf(float a, float b) { return b < a ? b : a; }
static inline float gm_maxf(float a, float b) { return a < b ? b : a; }
static inline int gm_mini(int a, int b) { return b < a ? b : a; }
static inline int gm_maxi(int a, int b) { return a < b ? b : a; }

// ---- 3D vector ------------------------------------------------------------------
typedef struct {
  float x, y, z;
} gm_vec3_t;

static inline gm_vec3_t gm_vec3(float x, float y, float z) {
  gm_vec3_t v = {x, y, z};
  return v;
}
static inline gm_vec3_t gm_vec3_zero(void) { return gm_vec3(0.0f, 0.0f, 0.0f); }
static inline gm_vec3_t gm_vec3_add(gm_vec3_t a, gm_vec3_t b) { return gm_vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline gm_vec3_t gm_vec3_sub(gm_vec3_t a, gm_vec3_t b) { return gm_vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline gm_vec3_t gm_vec3_scale(gm_vec3_t v, float s) { return gm_vec3(v.x * s, v.y * s, v.z * s); }
static inline gm_vec3_t gm_vec3_div(gm_vec3_t v, float s) { return gm_vec3(v.x / s, v.y / s, v.z / s); }
static inline gm_vec3_t gm_vec3_neg(gm_vec3_t v) { return gm_vec3(-v.x, -v.y, -v.z); }
static inline float gm_vec3_dot(gm_vec3_t a, gm_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline gm_vec3_t gm_vec3_cross(gm_vec3_t a, gm_vec3_t b) {
  return gm_vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float gm_vec3_length2(gm_vec3_t v) { return gm_vec3_dot(v, v); }
static inline float gm_vec3_length(gm_vec3_t v) { return sqrtf(gm_vec3_length2(v)); }
static inline gm_vec3_t gm_vec3_normalized(gm_vec3_t v) {
  float l = gm_vec3_length(v);
  return l > 1e-9f ? gm_vec3_div(v, l) : gm_vec3(0.0f, 0.0f, 0.0f);
}
// Component-wise product.
static inline gm_vec3_t gm_vec3_mul(gm_vec3_t a, gm_vec3_t b) { return gm_vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline gm_vec3_t gm_vec3_lerp(gm_vec3_t a, gm_vec3_t b, float t) {
  return gm_vec3_add(a, gm_vec3_scale(gm_vec3_sub(b, a), t));
}

// ---- 3x3 matrix -----------------------------------------------------------------
// Row-major; rows are the car-frame basis vectors expressed in the sensor
// frame, so gm_mat3_apply maps sensor-frame vectors into the car frame.
typedef struct {
  float m[3][3];
} gm_mat3_t;

static inline gm_mat3_t gm_mat3_identity(void) {
  gm_mat3_t r = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
  return r;
}
static inline gm_mat3_t gm_mat3_from_rows(gm_vec3_t r0, gm_vec3_t r1, gm_vec3_t r2) {
  gm_mat3_t r = {{{r0.x, r0.y, r0.z}, {r1.x, r1.y, r1.z}, {r2.x, r2.y, r2.z}}};
  return r;
}
static inline gm_vec3_t gm_mat3_row(const gm_mat3_t* a, int i) { return gm_vec3(a->m[i][0], a->m[i][1], a->m[i][2]); }
static inline gm_vec3_t gm_mat3_apply(const gm_mat3_t* a, gm_vec3_t v) {
  return gm_vec3(gm_vec3_dot(gm_mat3_row(a, 0), v), gm_vec3_dot(gm_mat3_row(a, 1), v),
                 gm_vec3_dot(gm_mat3_row(a, 2), v));
}

// ---- random numbers -------------------------------------------------------------
// Deterministic, cheap PRNG (xorshift32). Same sequence on host and target so
// recorded simulator output is reproducible.
#define GM_RNG_DEFAULT_SEED 0x9E3779B9u

typedef struct {
  uint32_t s;
} gm_rng_t;

static inline void gm_rng_seed(gm_rng_t* r, uint32_t seed) { r->s = seed ? seed : 1u; }
static inline uint32_t gm_rng_next(gm_rng_t* r) {
  uint32_t x = r->s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  r->s = x;
  return x;
}
// Uniform in [0, 1).
static inline float gm_rng_uniform(gm_rng_t* r) { return (float)(gm_rng_next(r) >> 8) * (1.0f / 16777216.0f); }
// Uniform in [lo, hi).
static inline float gm_rng_range(gm_rng_t* r, float lo, float hi) { return lo + (hi - lo) * gm_rng_uniform(r); }
// Uniform in [-1, 1).
static inline float gm_rng_symmetric(gm_rng_t* r) { return gm_rng_uniform(r) * 2.0f - 1.0f; }

// ---- second-order low-pass ----------------------------------------------------------
// Butterworth biquad, direct form I. Used to smooth accelerometer data; see
// gm_estimator. gm_lowpass_init makes a pass-through filter.
typedef struct {
  float b0, b1, b2, a1, a2;
  float x1, x2, y1, y2;
} gm_lowpass_t;

static inline void gm_lowpass_init(gm_lowpass_t* f) {
  f->b0 = 1.0f;
  f->b1 = f->b2 = f->a1 = f->a2 = 0.0f;
  f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}
static inline void gm_lowpass_configure(gm_lowpass_t* f, float cutoff_hz, float sample_hz) {
  if (cutoff_hz <= 0.0f || sample_hz <= 0.0f || cutoff_hz >= sample_hz * 0.49f) {
    // Pass-through.
    f->b0 = 1.0f;
    f->b1 = f->b2 = f->a1 = f->a2 = 0.0f;
    return;
  }
  const float w0 = GM_TWO_PI * cutoff_hz / sample_hz;
  const float cosw = cosf(w0), sinw = sinf(w0);
  const float q = 0.70710678f;  // Butterworth
  const float alpha = sinw / (2.0f * q);
  const float a0 = 1.0f + alpha;
  f->b0 = ((1.0f - cosw) * 0.5f) / a0;
  f->b1 = (1.0f - cosw) / a0;
  f->b2 = f->b0;
  f->a1 = (-2.0f * cosw) / a0;
  f->a2 = (1.0f - alpha) / a0;
}
static inline void gm_lowpass_reset(gm_lowpass_t* f, float value) { f->x1 = f->x2 = f->y1 = f->y2 = value; }
static inline float gm_lowpass_process(gm_lowpass_t* f, float x) {
  float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
  f->x2 = f->x1;
  f->x1 = x;
  f->y2 = f->y1;
  f->y1 = y;
  return y;
}
static inline float gm_lowpass_value(const gm_lowpass_t* f) { return f->y1; }

// ---- damped spring ----------------------------------------------------------------
// Second-order smoother advanced with the exact closed-form solution of
// x'' + 2*zeta*w*x' + w^2*x = 0 over one step (after Ryan Juckett's "Damped
// Springs"), so it is stable for any dt, unlike semi-implicit Euler which
// diverges once w*dt exceeds ~0.85. zeta = 1 gives the classic no-overshoot
// "g-ball" motion; zeta < 1 jiggles. A zeroed struct is a spring at rest at 0.
typedef struct {
  float pos, vel;
} gm_spring_t;

static inline void gm_spring_step(gm_spring_t* s, float target, float dt, float natural_hz, float zeta) {
  const float w = GM_TWO_PI * natural_hz;
  if (w * dt < 1e-6f) return;
  zeta = zeta < 0.01f ? 0.01f : zeta;
  float pos_pos, pos_vel, vel_pos, vel_vel;
  if (zeta > 0.999f && zeta < 1.001f) {
    const float e = expf(-w * dt), te = dt * e, tew = te * w;
    pos_pos = tew + e;
    pos_vel = te;
    vel_pos = -w * tew;
    vel_vel = e - tew;
  } else if (zeta < 1.0f) {
    const float a = w * sqrtf(1.0f - zeta * zeta);
    const float e = expf(-w * zeta * dt);
    const float c = cosf(a * dt), sn = sinf(a * dt), r = w * zeta / a;
    pos_pos = e * (c + r * sn);
    pos_vel = e * sn / a;
    vel_pos = -e * sn * a - w * zeta * e * r * sn;
    vel_vel = e * (c - r * sn);
  } else {
    const float za = -w * zeta, zb = w * sqrtf(zeta * zeta - 1.0f);
    const float z1 = za - zb, z2 = za + zb;
    const float e1 = expf(z1 * dt), e2 = expf(z2 * dt), inv = 1.0f / (z2 - z1);
    pos_pos = (e1 * z2 - e2 * z1) * inv;
    pos_vel = (e2 - e1) * inv;
    vel_pos = (e1 - e2) * z1 * z2 * inv;
    vel_vel = (e2 * z2 - e1 * z1) * inv;
  }
  const float x = s->pos - target;
  s->pos = x * pos_pos + s->vel * pos_vel + target;
  s->vel = x * vel_pos + s->vel * vel_vel;
}
static inline void gm_spring_snap(gm_spring_t* s, float v) {
  s->pos = v;
  s->vel = 0.0f;
}

typedef struct {
  gm_spring_t x, y, z;
} gm_spring3_t;

static inline gm_vec3_t gm_spring3_pos(const gm_spring3_t* s) { return gm_vec3(s->x.pos, s->y.pos, s->z.pos); }
static inline void gm_spring3_step(gm_spring3_t* s, gm_vec3_t target, float dt, float natural_hz, float zeta) {
  gm_spring_step(&s->x, target.x, dt, natural_hz, zeta);
  gm_spring_step(&s->y, target.y, dt, natural_hz, zeta);
  gm_spring_step(&s->z, target.z, dt, natural_hz, zeta);
}
static inline void gm_spring3_snap(gm_spring3_t* s, gm_vec3_t v) {
  gm_spring_snap(&s->x, v.x);
  gm_spring_snap(&s->y, v.y);
  gm_spring_snap(&s->z, v.z);
}

#ifdef __cplusplus
}
#endif
