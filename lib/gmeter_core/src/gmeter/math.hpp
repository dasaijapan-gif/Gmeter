// Small float vector math. Single precision only: the ESP32 has a hardware FPU
// for float but emulates double in software, so never use double literals.
#pragma once

#include <cmath>
#include <cstdint>

namespace gm {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;
constexpr float kStandardGravity = 9.80665f;  // m/s^2 per g

// NaN-safe: a NaN input returns lo, so bad input can never poison state.
inline float clampf(float v, float lo, float hi) {
  if (!(v >= lo)) return lo;
  if (!(v <= hi)) return hi;
  return v;
}
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstepf(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
inline float signf(float v) { return v < 0.0f ? -1.0f : 1.0f; }
inline int ifloor(float v) { return static_cast<int>(std::floor(v)); }
inline int iround(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;

  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

  float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 cross(const Vec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
  float length2() const { return dot(*this); }
  float length() const { return std::sqrt(length2()); }
  Vec3 normalized() const {
    float l = length();
    return l > 1e-9f ? (*this) / l : Vec3{0.0f, 0.0f, 0.0f};
  }
  // Component-wise product.
  Vec3 scaled(const Vec3& s) const { return {x * s.x, y * s.y, z * s.z}; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// Row-major 3x3 matrix; rows are the car-frame basis vectors expressed in the
// sensor frame, so `apply` maps sensor-frame vectors into the car frame.
struct Mat3 {
  float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  static Mat3 identity() { return Mat3{}; }
  static Mat3 fromRows(const Vec3& r0, const Vec3& r1, const Vec3& r2) {
    Mat3 r;
    r.m[0][0] = r0.x; r.m[0][1] = r0.y; r.m[0][2] = r0.z;
    r.m[1][0] = r1.x; r.m[1][1] = r1.y; r.m[1][2] = r1.z;
    r.m[2][0] = r2.x; r.m[2][1] = r2.y; r.m[2][2] = r2.z;
    return r;
  }
  Vec3 row(int i) const { return {m[i][0], m[i][1], m[i][2]}; }
  Vec3 apply(const Vec3& v) const {
    return {row(0).dot(v), row(1).dot(v), row(2).dot(v)};
  }
};

// Deterministic, cheap PRNG (xorshift32). Same sequence on host and target so
// recorded simulator output is reproducible.
class Rng {
 public:
  explicit Rng(uint32_t seed = 0x9E3779B9u) : s_(seed ? seed : 1u) {}
  uint32_t next() {
    uint32_t x = s_;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_ = x;
    return x;
  }
  // Uniform in [0, 1).
  float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  // Uniform in [lo, hi).
  float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
  // Uniform in [-1, 1).
  float symmetric() { return uniform() * 2.0f - 1.0f; }
  void seed(uint32_t s) { s_ = s ? s : 1u; }

 private:
  uint32_t s_;
};

// Second-order low-pass (Butterworth) biquad, direct form I. Used to smooth
// accelerometer data; see GForceEstimator.
class LowPassBiquad {
 public:
  void configure(float cutoffHz, float sampleHz) {
    if (cutoffHz <= 0.0f || sampleHz <= 0.0f || cutoffHz >= sampleHz * 0.49f) {
      // Pass-through.
      b0_ = 1.0f; b1_ = b2_ = a1_ = a2_ = 0.0f;
      return;
    }
    const float w0 = kTwoPi * cutoffHz / sampleHz;
    const float cosw = std::cos(w0), sinw = std::sin(w0);
    const float q = 0.70710678f;  // Butterworth
    const float alpha = sinw / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0_ = ((1.0f - cosw) * 0.5f) / a0;
    b1_ = (1.0f - cosw) / a0;
    b2_ = b0_;
    a1_ = (-2.0f * cosw) / a0;
    a2_ = (1.0f - alpha) / a0;
  }
  void reset(float value = 0.0f) { x1_ = x2_ = y1_ = y2_ = value; }
  float process(float x) {
    float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_; x1_ = x;
    y2_ = y1_; y1_ = y;
    return y;
  }
  float value() const { return y1_; }

 private:
  float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
  float x1_ = 0.0f, x2_ = 0.0f, y1_ = 0.0f, y2_ = 0.0f;
};

// Damped spring (second-order smoother) advanced with the exact closed-form
// solution of x'' + 2*zeta*w*x' + w^2*x = 0 over one step (after Ryan
// Juckett's "Damped Springs"), so it is stable for any dt, unlike semi-implicit
// Euler which diverges once w*dt exceeds ~0.85. zeta = 1 gives the classic
// no-overshoot "g-ball" motion; zeta < 1 jiggles.
struct Spring {
  float pos = 0.0f, vel = 0.0f;
  void step(float target, float dt, float naturalHz, float zeta = 1.0f) {
    const float w = kTwoPi * naturalHz;
    if (w * dt < 1e-6f) return;
    zeta = zeta < 0.01f ? 0.01f : zeta;
    float posPos, posVel, velPos, velVel;
    if (zeta > 0.999f && zeta < 1.001f) {
      const float e = std::exp(-w * dt), te = dt * e, tew = te * w;
      posPos = tew + e; posVel = te; velPos = -w * tew; velVel = e - tew;
    } else if (zeta < 1.0f) {
      const float a = w * std::sqrt(1.0f - zeta * zeta);
      const float e = std::exp(-w * zeta * dt);
      const float c = std::cos(a * dt), sn = std::sin(a * dt), r = w * zeta / a;
      posPos = e * (c + r * sn); posVel = e * sn / a;
      velPos = -e * sn * a - w * zeta * e * r * sn; velVel = e * (c - r * sn);
    } else {
      const float za = -w * zeta, zb = w * std::sqrt(zeta * zeta - 1.0f);
      const float z1 = za - zb, z2 = za + zb;
      const float e1 = std::exp(z1 * dt), e2 = std::exp(z2 * dt), inv = 1.0f / (z2 - z1);
      posPos = (e1 * z2 - e2 * z1) * inv; posVel = (e2 - e1) * inv;
      velPos = (e1 - e2) * z1 * z2 * inv; velVel = (e2 * z2 - e1 * z1) * inv;
    }
    const float x = pos - target;
    pos = x * posPos + vel * posVel + target;
    vel = x * velPos + vel * velVel;
  }
  void snap(float v) { pos = v; vel = 0.0f; }
};

struct Spring3 {
  Spring x, y, z;
  Vec3 pos() const { return {x.pos, y.pos, z.pos}; }
  void step(const Vec3& target, float dt, float naturalHz, float zeta = 1.0f) {
    x.step(target.x, dt, naturalHz, zeta);
    y.step(target.y, dt, naturalHz, zeta);
    z.step(target.z, dt, naturalHz, zeta);
  }
  void snap(const Vec3& v) { x.snap(v.x); y.snap(v.y); z.snap(v.z); }
};

}  // namespace gm
