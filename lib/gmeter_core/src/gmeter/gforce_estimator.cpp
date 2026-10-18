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
  if (up.length2() < 0.5f) return c;
  // Gram-Schmidt: remove the gravity component from the forward hint.
  Vec3 fwd = forwardHintSensor - up * forwardHintSensor.dot(up);
  if (fwd.length2() < 1e-6f) {
    // Hint is (anti)parallel to gravity: pick any perpendicular axis.
    const Vec3 helper = std::fabs(up.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    fwd = helper - up * helper.dot(up);
  }
  fwd = fwd.normalized();
  // Right-handed car frame: right = up x forward  (y cross z = x).
  const Vec3 right = up.cross(fwd).normalized();
  c.sensorToCar = Mat3::fromRows(right, up, fwd);
  c.valid = true;
  return c;
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
  restMean_ = restAccum_ / static_cast<float>(restCount_);
  // Sanity: magnitude should be roughly 1 g.
  const float mag = restMean_.length();
  if (mag < 0.7f || mag > 1.3f) return false;
  Calibration c = makeCalibration(restMean_, nominalForward_);
  if (!c.valid) return false;
  cal_ = c;
  reset();
  return true;
}

bool GForceEstimator::calibrateForwardFromBraking(const Vec3& meanRawDuringBrake) {
  if (!cal_.valid) return false;
  const Vec3 up = cal_.sensorToCar.row(1);
  Vec3 delta = meanRawDuringBrake - restMean_;
  delta -= up * delta.dot(up);  // horizontal part
  if (delta.length() < 0.05f) return false;  // needs at least 0.05 g of braking
  // During braking the proper acceleration points backward, so forward is -delta.
  const Vec3 fwd = -delta;
  Calibration c = makeCalibration(up, fwd);
  if (!c.valid) return false;
  cal_ = c;
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
