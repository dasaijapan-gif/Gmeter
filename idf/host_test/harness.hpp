// Minimal test harness shared by the host tests of the C core.
//
// Each test_*.cpp file includes this header and defines tests with TEST(name).
// Both cores are visible: the C core through "gmeter.h" (gm_* names) and the
// original C++ core through "gmeter/gmeter.hpp" (namespace gm).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gmeter.h"
#include "gmeter/gmeter.hpp"

namespace harness {

struct Test {
  const char* name;
  std::function<void()> fn;
};
std::vector<Test>& tests();
void check(bool ok, const char* what, const char* file, int line);
// Prints a message under the current test, e.g. where two frames first differ.
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct Reg {
  Reg(const char* n, std::function<void()> f) { tests().push_back({n, std::move(f)}); }
};

// Byte-for-byte frame comparison. On a mismatch it reports the first
// differing voxel (layer, x, y) via note() and returns false.
bool same_frame(const gm_frame_t* c, const gm::VoxelFrame& cpp, const char* label);
// Bit-exact float comparison (also equal for two NaNs with the same bits).
bool same_float(float a, float b);
bool same_vec(gm_vec3_t a, const gm::Vec3& b);

}  // namespace harness

#define GM_CAT2(a, b) a##b
#define GM_CAT(a, b) GM_CAT2(a, b)
#define TEST(name)                                                \
  static void name();                                             \
  static harness::Reg GM_CAT(reg_, name)(#name, name);            \
  static void name()
#define CHECK(x) harness::check((x), #x, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) harness::check(std::fabs((a) - (b)) <= (tol), #a " ~= " #b, __FILE__, __LINE__)
