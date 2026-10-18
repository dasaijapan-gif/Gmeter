#include "gforce_estimator.hpp"

namespace gm {

void GForceEstimator::setFilter(float cutoffHz, float sampleHz) {
  cutoffHz_ = cutoffHz;
  sampleHz_ = sampleHz;
  lpX_.configure(cutoffHz, sampleHz);
  lpY_.configure(cutoffHz, sampleHz);
  lpZ_.configure(cutoffHz, sampleHz);
}

void GForceEstimator::reset() {
  primed_ = false;
  bias_ = Vec3{};
  motionLevel_ = 1.0f;
  g_ = GForce{};
}

Calibration GForceEstimator::makeCalibration(const Vec3& upSensor, const Vec3& forwardHintSensor) {
  Calibration c;
  const Vec3 up = upSensor.normalized();
  const Vec3 hint = forwardHintSensor.normalized();
  if (up.length2() < 0.5f || hint.length2() < 0.5f) return c;
  // Gram-Schmidt: remove the gravity component from the forward hint. A hint
  // within 30 degrees of vertical carries too little horizontal direction to
  // trust (its remainder is set by mounting tilt), so reject it.
  Vec3 fwd = hint - up * hint.dot(up);
  if (fwd.length2() < 0.25f) return c;
  fwd = fwd.normalized();
  // The car frame (x right, y up, z forward) is left-handed: right = forward x up.
  const Vec3 right = fwd.cross(up).normalized();
  c.sensorToCar = Mat3::fromRows(right, up, fwd);
  c.valid = true;
  return c;
}

Calibration GForceEstimator::makeUpOnlyCalibration(const Vec3& upSensor) {
  // Forward unknown: use whichever sensor axis is most horizontal as a
  // placeholder until a braking capture supplies the real direction.
  const Vec3 up = upSensor.normalized();
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  int best = 0;
  for (int i = 1; i < 3; ++i)
    if (std::fabs(axes[i].dot(up)) < std::fabs(axes[best].dot(up))) best = i;
  return makeCalibration(upSensor, axes[best]);
}

void GForceEstimator::beginRestCalibration() {
  restAccum_ = Vec3{};
  restCount_ = 0;
}

void GForceEstimator::feedRestSample(const Vec3& raw) {
  restAccum_ += raw;
  ++restCount_;
}

bool GForceEstimator::finishRestCalibration() {
  if (restCount_ < 8) return false;
  // Validate before committing anything, so a rejected capture leaves the
  // previous calibration untouched.
  const Vec3 mean = restAccum_ / static_cast<float>(restCount_);
  const float mag = mean.length();
  if (mag < 0.7f || mag > 1.3f) return false;
  Calibration c = makeCalibration(mean, nominalForward_);
  forwardTrusted_ = c.valid;
  if (!c.valid) c = makeUpOnlyCalibration(mean);  // up is known; forward needs `cal brake`
  if (!c.valid) return false;
  cal_ = c;
  reset();
  return true;
}

bool GForceEstimator::calibrateForwardFromBraking(const Vec3& meanRawDuringBrake) {
  if (!cal_.valid) return false;
  const Vec3 up = cal_.sensorToCar.row(1);
  // The rest vector lies along `up`, so the horizontal part of the raw braking
  // sample is the braking acceleration itself (no stored rest mean needed,
  // which keeps this valid after a reboot).
  Vec3 delta = meanRawDuringBrake;
  delta -= up * delta.dot(up);
  if (delta.length() < 0.05f) return false;  // needs at least 0.05 g of braking
  // During braking the proper acceleration points backward, so forward is -delta.
  const Vec3 fwd = -delta;
  Calibration c = makeCalibration(up, fwd);
  if (!c.valid) return false;
  cal_ = c;
  forwardTrusted_ = true;
  nominalForward_ = fwd.normalized();
  reset();
  return true;
}

const GForce& GForceEstimator::update(const Vec3& raw) {
  rawCar_ = cal_.sensorToCar.apply(raw);
  if (!primed_) {
    lpX_.reset(rawCar_.x); lpY_.reset(rawCar_.y); lpZ_.reset(rawCar_.z);
    slowMean_ = rawCar_;
    primed_ = true;
  }
  Vec3 f{lpX_.process(rawCar_.x), lpY_.process(rawCar_.y), lpZ_.process(rawCar_.z)};

  if (autoZero_) {
    // Stillness detector: exponential average of |raw - slow mean|.
    const float dt = 1.0f / sampleHz_;
    const float aSlow = dt / (dt + 2.0f);  // ~2 s mean
    slowMean_ = lerp(slowMean_, rawCar_, aSlow);
    const float dev = (rawCar_ - slowMean_).length();
    motionLevel_ = lerpf(motionLevel_, dev, dt / (dt + 1.0f));
    if (motionLevel_ < 0.01f) {  // < 0.01 g of jitter: parked
      const float a = dt / (dt + autoZeroTau_);
      bias_.x = lerpf(bias_.x, f.x, a);
      bias_.z = lerpf(bias_.z, f.z, a);
    }
    f.x -= bias_.x;
    f.z -= bias_.z;
  }
  g_.proper = f;
  return g_;
}

}  // namespace gm
