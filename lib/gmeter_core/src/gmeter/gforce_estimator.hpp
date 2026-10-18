// Turns raw accelerometer samples (sensor frame, in g) into a filtered GForce
// in the car frame. Handles arbitrary mounting orientation via a calibration
// rotation, low-pass filtering to remove engine/road vibration, and optional
// slow re-zeroing of the horizontal axes while parked.
#pragma once

#include "gforce.hpp"
#include "math.hpp"

namespace gm {

struct Calibration {
  // Rows: car right, car up, car forward, each expressed in the sensor frame.
  Mat3 sensorToCar = Mat3::identity();
  bool valid = false;
};

class GForceEstimator {
 public:
  GForceEstimator() { setFilter(4.0f, 200.0f); }

  // ---- filtering --------------------------------------------------------------
  // cutoffHz: ~3-5 Hz keeps real g changes (<2 Hz) and drops vibration.
  void setFilter(float cutoffHz, float sampleHz);
  float cutoffHz() const { return cutoffHz_; }
  float sampleHz() const { return sampleHz_; }

  // Feed one raw sample (sensor frame, in g). Returns the filtered car-frame g.
  const GForce& update(const Vec3& rawSensorG);
  const GForce& current() const { return g_; }
  // Unfiltered car-frame proper acceleration from the last sample.
  const Vec3& rawCar() const { return rawCar_; }

  // ---- calibration ----------------------------------------------------------------
  // Nominal sensor axis that points toward the front of the car when the
  // sensor is mounted as intended (default: sensor +X). Only the component
  // perpendicular to gravity is used, so a tilted mount is fine.
  void setNominalForward(const Vec3& sensorAxis) { nominalForward_ = sensorAxis; }
  const Vec3& nominalForward() const { return nominalForward_; }

  // Step 1: average samples while the car is parked on level-ish ground.
  // Succeeds even when the forward hint is vertical (then forwardTrusted() is
  // false and a braking capture is required).
  void beginRestCalibration();
  void feedRestSample(const Vec3& rawSensorG);
  int restSampleCount() const { return restCount_; }
  // Builds the rotation from the averaged rest vector and the nominal forward
  // axis. Returns false if too few samples or the axes are degenerate.
  bool finishRestCalibration();

  // Step 2 (optional): refine the forward axis empirically. Pass the average
  // raw sample collected during a firm brake from a straight line; the
  // horizontal part of (brake - rest) points backward.
  bool calibrateForwardFromBraking(const Vec3& meanRawDuringBrake);

  // Construct directly from known vectors in the sensor frame. Invalid when
  // the forward hint is within 30 degrees of vertical.
  static Calibration makeCalibration(const Vec3& upSensor, const Vec3& forwardHintSensor);
  // Up from the rest vector, forward a placeholder (most horizontal sensor axis).
  static Calibration makeUpOnlyCalibration(const Vec3& upSensor);
  // False after a rest calibration whose forward hint was (nearly) vertical:
  // lateral/longitudinal are then arbitrary until calibrateForwardFromBraking.
  bool forwardTrusted() const { return forwardTrusted_; }
  void setCalibration(const Calibration& c) { cal_ = c; forwardTrusted_ = c.valid; }
  const Calibration& calibration() const { return cal_; }

  // ---- auto re-zero ------------------------------------------------------------------
  // When enabled, the horizontal components drift toward zero with time
  // constant tauSeconds while the car is stationary (very low variance). This
  // compensates for a dashboard mount that shifts, at the cost of slowly
  // hiding a real sustained slope. Disabled by default.
  void setAutoZero(bool enabled, float tauSeconds = 60.0f) { autoZero_ = enabled; autoZeroTau_ = tauSeconds; }

  void reset();

 private:
  Calibration cal_;
  Vec3 nominalForward_{1.0f, 0.0f, 0.0f};
  Vec3 restAccum_;
  int restCount_ = 0;
  bool forwardTrusted_ = true;

  float cutoffHz_ = 4.0f, sampleHz_ = 200.0f;
  LowPassBiquad lpX_, lpY_, lpZ_;
  bool primed_ = false;

  bool autoZero_ = false;
  float autoZeroTau_ = 60.0f;
  Vec3 bias_;        // subtracted from horizontal axes
  Vec3 slowMean_;    // for stillness detection
  float motionLevel_ = 1.0f;

  Vec3 rawCar_;
  GForce g_;
};

}  // namespace gm
