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
  gm_vec3_t rest_accum;
  int rest_count;
  bool forward_trusted;

  float cutoff_hz, sample_hz;
  gm_lowpass_t lp_x, lp_y, lp_z;
  bool primed;

  bool auto_zero;
  float auto_zero_tau;
  gm_vec3_t bias;       // subtracted from horizontal axes
  gm_vec3_t slow_mean;  // for stillness detection
  float motion_level;

  gm_vec3_t raw_car;
  gm_gforce_t g;
} gm_estimator_t;

// Defaults: 4 Hz filter at 200 Hz, no calibration, forward hint sensor +X.
void gm_estimator_init(gm_estimator_t* e);

// ---- filtering ------------------------------------------------------------------------
// cutoff_hz: ~3-5 Hz keeps real g changes (<2 Hz) and drops vibration.
void gm_estimator_set_filter(gm_estimator_t* e, float cutoff_hz, float sample_hz);
static inline float gm_estimator_cutoff_hz(const gm_estimator_t* e) { return e->cutoff_hz; }
static inline float gm_estimator_sample_hz(const gm_estimator_t* e) { return e->sample_hz; }

// Feed one raw sample (sensor frame, in g). Returns the filtered car-frame g.
gm_gforce_t gm_estimator_update(gm_estimator_t* e, gm_vec3_t raw_sensor_g);
static inline gm_gforce_t gm_estimator_current(const gm_estimator_t* e) { return e->g; }
// Unfiltered car-frame proper acceleration from the last sample.
static inline gm_vec3_t gm_estimator_raw_car(const gm_estimator_t* e) { return e->raw_car; }

// ---- calibration ---------------------------------------------------------------------
// Nominal sensor axis that points toward the front of the car when the sensor
// is mounted as intended (default: sensor +X). Only the component
// perpendicular to gravity is used, so a tilted mount is fine.
static inline void gm_estimator_set_nominal_forward(gm_estimator_t* e, gm_vec3_t sensor_axis) {
  e->nominal_forward = sensor_axis;
}
static inline gm_vec3_t gm_estimator_nominal_forward(const gm_estimator_t* e) { return e->nominal_forward; }

// Step 1: average samples while the car is parked on level-ish ground.
// Succeeds even when the forward hint is vertical (then forward_trusted is
// false and a braking capture is required).
void gm_estimator_begin_rest_calibration(gm_estimator_t* e);
void gm_estimator_feed_rest_sample(gm_estimator_t* e, gm_vec3_t raw_sensor_g);
static inline int gm_estimator_rest_sample_count(const gm_estimator_t* e) { return e->rest_count; }
// Builds the rotation from the averaged rest vector and the nominal forward
// axis. Returns false if too few samples or the axes are degenerate.
bool gm_estimator_finish_rest_calibration(gm_estimator_t* e);

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
