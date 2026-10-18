#include "scenario.hpp"

#include <cstring>

namespace gm {

Vec3 Scenario::dynamicAt(float t) const {
  if (count_ == 0) return Vec3{};
  if (loopAfter_ > 0.0f && t > loopAfter_) t = std::fmod(t, loopAfter_);
  if (t <= keys_[0].t) return keys_[0].dynamic;
  if (t >= keys_[count_ - 1].t) return keys_[count_ - 1].dynamic;
  int i = 0;
  while (i + 1 < count_ && keys_[i + 1].t < t) ++i;
  const GKey& a = keys_[i];
  const GKey& b = keys_[i + 1];
  const float span = b.t - a.t;
  const float u = span > 1e-6f ? smoothstepf(0.0f, 1.0f, (t - a.t) / span) : 1.0f;
  return lerp(a.dynamic, b.dynamic, u);
}

GForce Scenario::at(float t, Rng* rng) const {
  GForce g;
  g.proper = GForce::kRestProper() + dynamicAt(t);
  if (rng && noiseG_ > 0.0f)
    g.proper += Vec3{rng->symmetric(), rng->symmetric(), rng->symmetric()} * noiseG_;
  return g;
}

Vec3 simulateRawSensor(const GForce& g, const Mat3& sensorToCar, Rng& rng, float noiseG) {
  // sensorToCar is orthonormal, so its transpose maps car -> sensor.
  const Vec3 c = g.proper;
  Vec3 s{sensorToCar.m[0][0] * c.x + sensorToCar.m[1][0] * c.y + sensorToCar.m[2][0] * c.z,
         sensorToCar.m[0][1] * c.x + sensorToCar.m[1][1] * c.y + sensorToCar.m[2][1] * c.z,
         sensorToCar.m[0][2] * c.x + sensorToCar.m[1][2] * c.y + sensorToCar.m[2][2] * c.z};
  if (noiseG > 0.0f) s += Vec3{rng.symmetric(), rng.symmetric(), rng.symmetric()} * noiseG;
  return s;
}

// ---- presets ---------------------------------------------------------------------------
// Dynamic g: x lateral (+ = right turn), y vertical (+ = pushed up), z longitudinal
// (+ = accelerating forward).
namespace {
const GKey kIdle[] = {{0.0f, {0, 0, 0}}, {6.0f, {0, 0, 0}}};
const GKey kLaunch[] = {{0.0f, {0, 0, 0}}, {0.5f, {0, 0, 0}}, {0.9f, {0, 0, 0.55f}}, {2.2f, {0, 0, 0.5f}},
                        {2.5f, {0, 0, 0.25f}}, {2.8f, {0, 0, 0.45f}}, {4.2f, {0, 0, 0.3f}}, {5.0f, {0, 0, 0}}, {6.5f, {0, 0, 0}}};
const GKey kBrake[] = {{0.0f, {0, 0, 0}}, {0.6f, {0, 0, 0}}, {0.9f, {0, 0, -0.75f}}, {2.6f, {0, 0, -0.8f}},
                       {3.1f, {0, 0, -0.15f}}, {3.35f, {0, 0.05f, 0.08f}}, {3.8f, {0, 0, 0}}, {6.0f, {0, 0, 0}}};
const GKey kSlalom[] = {{0.0f, {0, 0, 0}}, {0.8f, {0.65f, 0, 0}}, {1.8f, {-0.65f, 0, 0}}, {2.8f, {0.65f, 0, 0}},
                        {3.8f, {-0.65f, 0, 0}}, {4.8f, {0.65f, 0, 0}}, {5.8f, {-0.65f, 0, 0}}, {6.8f, {0.6f, 0, 0}},
                        {7.8f, {-0.55f, 0, 0}}, {8.8f, {0, 0, 0}}, {10.0f, {0, 0, 0}}};
const GKey kCorner[] = {{0.0f, {0, 0, 0}}, {0.6f, {0, 0, 0}}, {1.6f, {0.6f, 0, -0.1f}}, {4.0f, {0.6f, 0, 0.05f}},
                        {5.0f, {0, 0, 0.1f}}, {6.0f, {-0.55f, 0, 0}}, {8.0f, {-0.55f, 0, 0.05f}}, {9.0f, {0, 0, 0}}, {10.0f, {0, 0, 0}}};
const GKey kBump[] = {{0.0f, {0, 0, 0}}, {1.0f, {0, 0, 0}}, {1.08f, {0, 0.6f, 0}}, {1.16f, {0, -0.5f, 0}},
                      {1.24f, {0, 0.25f, 0}}, {1.34f, {0, -0.1f, 0}}, {1.45f, {0, 0, 0}}, {2.6f, {0, 0, 0}},
                      {2.68f, {0, 0.4f, 0}}, {2.76f, {0, -0.35f, 0}}, {2.9f, {0, 0, 0}}, {4.5f, {0, 0, 0}}};
const GKey kDrive[] = {
    {0.0f, {0, 0, 0}},        {0.8f, {0, 0, 0}},           {1.3f, {0, 0, 0.5f}},        {3.0f, {0, 0, 0.35f}},
    {3.6f, {0, 0, 0.05f}},    {5.0f, {0, 0, 0}},           {5.5f, {0, 0, -0.6f}},       {6.8f, {0.2f, 0, -0.5f}},
    {7.6f, {0.7f, 0, -0.1f}}, {9.5f, {0.7f, 0, 0.1f}},     {10.2f, {0.2f, 0, 0.2f}},    {10.5f, {0, 0, 0.2f}},
    {10.58f, {0, 0.55f, 0.2f}}, {10.66f, {0, -0.45f, 0.2f}}, {10.8f, {0, 0, 0.2f}},     {12.0f, {-0.1f, 0, 0.1f}},
    {12.8f, {-0.6f, 0, 0}},   {14.2f, {-0.6f, 0, 0.1f}},   {15.0f, {-0.1f, 0, 0.5f}},   {16.3f, {0, 0, 0.45f}},
    {17.0f, {0, 0, 0}},       {17.6f, {0, 0, -0.8f}},      {18.9f, {0, 0, -0.85f}},     {19.3f, {0, 0, -0.2f}},
    {19.5f, {0, 0.05f, 0.1f}}, {20.0f, {0, 0, 0}},         {21.0f, {0, 0, 0}}};
constexpr Scenario kPresets[] = {
    Scenario("idle", kIdle, sizeof(kIdle) / sizeof(kIdle[0]), 0.01f),
    Scenario("launch", kLaunch, sizeof(kLaunch) / sizeof(kLaunch[0])),
    Scenario("brake", kBrake, sizeof(kBrake) / sizeof(kBrake[0])),
    Scenario("slalom", kSlalom, sizeof(kSlalom) / sizeof(kSlalom[0])),
    Scenario("corner", kCorner, sizeof(kCorner) / sizeof(kCorner[0])),
    Scenario("bump", kBump, sizeof(kBump) / sizeof(kBump[0]), 0.02f),
    Scenario("drive", kDrive, sizeof(kDrive) / sizeof(kDrive[0])),
};
}  // namespace

int Scenario::presetCount() { return static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])); }
const Scenario& Scenario::preset(int i) { return kPresets[clampi(i, 0, presetCount() - 1)]; }
const Scenario* Scenario::find(const char* name) {
  for (int i = 0; i < presetCount(); ++i)
    if (std::strcmp(kPresets[i].name(), name) == 0) return &kPresets[i];
  return nullptr;
}

}  // namespace gm
