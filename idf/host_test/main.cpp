// Runs every TEST registered by the test_*.cpp files. Optional argument: a
// substring; only tests whose name contains it run.
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "harness.hpp"

namespace harness {

namespace {
int g_checks = 0, g_failures = 0, g_notes = 0;
const char* g_current = "";
}  // namespace

std::vector<Test>& tests() {
  static std::vector<Test> t;
  return t;
}

void check(bool ok, const char* what, const char* file, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL [%s] %s:%d: %s\n", g_current, file, line, what);
  }
}

void note(const char* fmt, ...) {
  if (++g_notes > 200) return;  // keep a broken run readable
  std::printf("  note [%s] ", g_current);
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::printf("\n");
}

bool same_frame(const gm_frame_t* c, const gm::VoxelFrame& cpp, const char* label) {
  static_assert(sizeof(c->bits) == gm::VoxelFrame::frameBytes(), "frame sizes differ");
  if (std::memcmp(gm_frame_cdata(c), cpp.data(), sizeof(c->bits)) == 0) return true;
  for (int z = 0; z < GM_D; ++z)
    for (int y = 0; y < GM_H; ++y)
      for (int x = 0; x < GM_W; ++x)
        if (gm_frame_get(c, x, y, z) != cpp.get(x, y, z)) {
          note("%s: frames differ first at layer %d x %d y %d (C %d, C++ %d)", label, z, x, y,
               gm_frame_get(c, x, y, z) ? 1 : 0, cpp.get(x, y, z) ? 1 : 0);
          return false;
        }
  return false;
}

bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

bool same_vec(gm_vec3_t a, const gm::Vec3& b) { return same_float(a.x, b.x) && same_float(a.y, b.y) && same_float(a.z, b.z); }

}  // namespace harness

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const harness::Test& t : harness::tests()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    harness::g_current = t.name;
    const int before = harness::g_failures;
    t.fn();
    ++run;
    std::printf("%s %s\n", harness::g_failures == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d tests, %d checks, %d failures (GM_PANEL_H=%d)\n", run, harness::g_checks, harness::g_failures,
              GM_PANEL_H);
  return harness::g_failures ? 1 : 0;
}
