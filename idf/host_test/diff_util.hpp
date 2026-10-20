// Helpers for the differential tests of the shared core (C vs C++).
//
// The tests compare thousands of outputs each. EXPECT counts every comparison
// as a check but prints only the first few failures of a test, so a systematic
// mismatch stays readable (the test still fails).
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>

#include "harness.hpp"

namespace diff {

struct Expect {
  int fails = 0;
  bool operator()(bool ok, const char* what, const char* file, int line) {
    if (ok) {
      harness::check(true, what, file, line);
      return true;
    }
    if (++fails <= 10) harness::check(false, what, file, line);
    return false;
  }
};
#define EXPECT(e, x) (e)((x), #x, __FILE__, __LINE__)

inline bool same(float a, float b) { return harness::same_float(a, b); }
inline bool same(gm_vec3_t a, const gm::Vec3& b) { return harness::same_vec(a, b); }
inline bool same(gm_vec3_t a, gm_vec3_t b) { return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z); }
inline bool same(const gm_mat3_t& a, const gm::Mat3& b) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (!same(a.m[i][j], b.m[i][j])) return false;
  return true;
}

inline gm::Vec3 to_cpp(gm_vec3_t v) { return gm::Vec3{v.x, v.y, v.z}; }
inline gm_vec3_t to_c(const gm::Vec3& v) { return gm_vec3(v.x, v.y, v.z); }
inline gm_mat3_t to_c(const gm::Mat3& m) {
  gm_mat3_t r;
  std::memcpy(r.m, m.m, sizeof(r.m));
  return r;
}

// Deterministic random source for the tests.
struct Rand {
  std::mt19937 gen;
  explicit Rand(uint32_t seed) : gen(seed) {}
  int irange(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); }  // inclusive
  float frange(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(gen); }
  bool chance(double p) { return std::bernoulli_distribution(p)(gen); }
  uint32_t u32() { return static_cast<uint32_t>(gen()); }
  template <typename T, size_t N>
  const T& pick(const T (&a)[N]) {
    return a[static_cast<size_t>(irange(0, static_cast<int>(N) - 1))];
  }
};

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// One C frame and one C++ frame, on the heap (each is 7-8 KB), both cleared.
struct FramePair {
  std::unique_ptr<gm_frame_t> c{new gm_frame_t};
  std::unique_ptr<gm::VoxelFrame> p{new gm::VoxelFrame};
  FramePair() { gm_frame_clear(c.get()); }
  bool same(const char* label) const { return harness::same_frame(c.get(), *p, label); }
};

}  // namespace diff
