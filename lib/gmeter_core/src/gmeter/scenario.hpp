// Scripted g-force profiles: keyframes of dynamic g (car frame: x lateral,
// y vertical, z longitudinal) smoothly interpolated, plus optional vibration
// noise. Used by the host simulator, the unit tests and the firmware's demo
// mode when no IMU is connected.
#pragma once

#include "gforce.hpp"
#include "math.hpp"

namespace gm {

struct GKey {
  float t;        // seconds
  Vec3 dynamic;   // g, car frame
};

class Scenario {
 public:
  constexpr Scenario() = default;
  constexpr Scenario(const char* name, const GKey* keys, int count, float noiseG = 0.015f, float loopAfter = 0.0f)
      : name_(name), keys_(keys), count_(count), noiseG_(noiseG), loopAfter_(loopAfter) {}

  const char* name() const { return name_; }
  float duration() const { return count_ ? keys_[count_ - 1].t : 0.0f; }
  float noiseG() const { return noiseG_; }

  // Smoothly interpolated dynamic g at time t (holds the last key).
  Vec3 dynamicAt(float t) const;
  // Full proper acceleration (rest + dynamic), with vibration noise if rng given.
  GForce at(float t, Rng* rng = nullptr) const;

  static int presetCount();
  static const Scenario& preset(int i);
  static const Scenario* find(const char* name);

 private:
  const char* name_ = "";
  const GKey* keys_ = nullptr;
  int count_ = 0;
  float noiseG_ = 0.0f;
  float loopAfter_ = 0.0f;
};

// Simulates what a sensor mounted with the given car->sensor rotation would
// read for this g (inverse of the calibration rotation), with white noise.
Vec3 simulateRawSensor(const GForce& g, const Mat3& sensorToCar, Rng& rng, float noiseG);

}  // namespace gm
