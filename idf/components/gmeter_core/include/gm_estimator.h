// Turns raw accelerometer samples (sensor frame, in g) into a filtered
// g-force in the car frame. Handles arbitrary mounting orientation via a
// calibration rotation, low-pass filtering to remove engine/road vibration,
// and optional slow re-zeroing of the horizontal axes while parked.
#pragma once

#include <stdbool.h>

#include "gm_gforce.h"
#include "gm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  // Rows: car right, car up, car forward, each expressed in the sensor frame.
  gm_mat3_t sensor_to_car;
  bool valid;
} gm_calibration_t;

// Identity rotation, not valid.
static inline gm_calibration_t gm_calibration_none(void) {
  gm_calibration_t c;
  c.sensor_to_car = gm_mat3_identity();
  c.valid = false;
  return c;
}

typedef struct {
  gm_calibration_t cal;
  gm_vec3_t nominal_forward;
  bool forward_trusted;

  float sample_hz;
  gm_lowpass_t lp_x, lp_y, lp_z;
  bool primed;

  bool auto_zero;
  float auto_zero_tau;
  gm_vec3_t bias;       // subtracted from horizontal axes
  gm_vec3_t slow_mean;  // for stillness detection
  float motion_level;
} gm_estimator_t;

// Defaults: 4 Hz filter at 200 Hz, no calibration, forward hint sensor +X.
void gm_estimator_init(gm_estimator_t* e);

// ---- filtering ------------------------------------------------------------------------
// cutoff_hz: ~3-5 Hz keeps real g changes (<2 Hz) and drops vibration.
void gm_estimator_set_filter(gm_estimator_t* e, float cutoff_hz, float sample_hz);

// Feed one raw sample (sensor frame, in g). Returns the filtered car-frame g.
gm_gforce_t gm_estimator_update(gm_estimator_t* e, gm_vec3_t raw_sensor_g);

// ---- calibration ---------------------------------------------------------------------
// Nominal sensor axis that points toward the front of the car when the sensor
// is mounted as intended (default: sensor +X). Only the component
// perpendicular to gravity is used, so a tilted mount is fine.
static inline void gm_estimator_set_nominal_forward(gm_estimator_t* e, gm_vec3_t sensor_axis) {
  e->nominal_forward = sensor_axis;
}

// Step 1: pass the average raw sample taken while the car is parked on
// level-ish ground. Builds the rotation from that rest vector and the nominal
// forward axis. Returns false, keeping the previous calibration, when the
// average is not about 1 g or the axes are degenerate. Succeeds even when the
// forward hint is vertical (then forward_trusted is false and a braking
// capture is required).
bool gm_estimator_calibrate_rest(gm_estimator_t* e, gm_vec3_t mean_raw_at_rest);

// Step 2 (optional): refine the forward axis empirically. Pass the average raw
// sample collected during a firm brake from a straight line; the horizontal
// part of (brake - rest) points backward.
bool gm_estimator_calibrate_forward_from_braking(gm_estimator_t* e, gm_vec3_t mean_raw_during_brake);

// Construct directly from known vectors in the sensor frame. Invalid when the
// forward hint is within 30 degrees of vertical.
gm_calibration_t gm_estimator_make_calibration(gm_vec3_t up_sensor, gm_vec3_t forward_hint_sensor);
// Up from the rest vector, forward a placeholder (most horizontal sensor axis).
gm_calibration_t gm_estimator_make_up_only_calibration(gm_vec3_t up_sensor);
// False after a rest calibration whose forward hint was (nearly) vertical:
// lateral/longitudinal are then arbitrary until a braking calibration.
static inline bool gm_estimator_forward_trusted(const gm_estimator_t* e) { return e->forward_trusted; }
static inline void gm_estimator_set_calibration(gm_estimator_t* e, gm_calibration_t c) {
  e->cal = c;
  e->forward_trusted = c.valid;
}
static inline gm_calibration_t gm_estimator_calibration(const gm_estimator_t* e) { return e->cal; }

// ---- auto re-zero ----------------------------------------------------------------------
// When enabled, the horizontal components drift toward zero with time
// constant tau_seconds (usually 60) while the car is stationary (very low
// variance). This compensates for a dashboard mount that shifts, at the cost
// of slowly hiding a real sustained slope. Disabled by default.
static inline void gm_estimator_set_auto_zero(gm_estimator_t* e, bool enabled, float tau_seconds) {
  e->auto_zero = enabled;
  e->auto_zero_tau = tau_seconds;
}

// Clears the filter and auto-zero state (keeps the calibration).
void gm_estimator_reset(gm_estimator_t* e);

#ifdef __cplusplus
}
#endif
