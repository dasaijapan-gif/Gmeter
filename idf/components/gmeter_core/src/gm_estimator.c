// Raw accelerometer samples (sensor frame) -> filtered g-force in the car
// frame: mounting calibration, low-pass filtering and optional auto re-zero.
#include "gm_estimator.h"

#include <math.h>

void gm_estimator_set_filter(gm_estimator_t* e, float cutoff_hz, float sample_hz) {
  e->cutoff_hz = cutoff_hz;
  e->sample_hz = sample_hz;
  gm_lowpass_configure(&e->lp_x, cutoff_hz, sample_hz);
  gm_lowpass_configure(&e->lp_y, cutoff_hz, sample_hz);
  gm_lowpass_configure(&e->lp_z, cutoff_hz, sample_hz);
}

void gm_estimator_init(gm_estimator_t* e) {
  e->cal = gm_calibration_none();
  e->nominal_forward = gm_vec3(1.0f, 0.0f, 0.0f);
  e->rest_accum = gm_vec3_zero();
  e->rest_count = 0;
  e->forward_trusted = true;

  e->cutoff_hz = 4.0f;
  e->sample_hz = 200.0f;
  gm_lowpass_init(&e->lp_x);
  gm_lowpass_init(&e->lp_y);
  gm_lowpass_init(&e->lp_z);
  e->primed = false;

  e->auto_zero = false;
  e->auto_zero_tau = 60.0f;
  e->bias = gm_vec3_zero();
  e->slow_mean = gm_vec3_zero();
  e->motion_level = 1.0f;

  e->raw_car = gm_vec3_zero();
  e->g = gm_gforce_rest();

  gm_estimator_set_filter(e, 4.0f, 200.0f);
}

void gm_estimator_reset(gm_estimator_t* e) {
  e->primed = false;
  e->bias = gm_vec3_zero();
  e->motion_level = 1.0f;
  e->g = gm_gforce_rest();
}

gm_calibration_t gm_estimator_make_calibration(gm_vec3_t up_sensor, gm_vec3_t forward_hint_sensor) {
  gm_calibration_t c = gm_calibration_none();
  const gm_vec3_t up = gm_vec3_normalized(up_sensor);
  const gm_vec3_t hint = gm_vec3_normalized(forward_hint_sensor);
  if (gm_vec3_length2(up) < 0.5f || gm_vec3_length2(hint) < 0.5f) return c;
  // Gram-Schmidt: remove the gravity component from the forward hint. A hint
  // within 30 degrees of vertical carries too little horizontal direction to
  // trust (its remainder is set by mounting tilt), so reject it.
  gm_vec3_t fwd = gm_vec3_sub(hint, gm_vec3_scale(up, gm_vec3_dot(hint, up)));
  if (gm_vec3_length2(fwd) < 0.25f) return c;
  fwd = gm_vec3_normalized(fwd);
  // The car frame (x right, y up, z forward) is left-handed: right = forward x up.
  const gm_vec3_t right = gm_vec3_normalized(gm_vec3_cross(fwd, up));
  c.sensor_to_car = gm_mat3_from_rows(right, up, fwd);
  c.valid = true;
  return c;
}

gm_calibration_t gm_estimator_make_up_only_calibration(gm_vec3_t up_sensor) {
  // Forward unknown: use whichever sensor axis is most horizontal as a
  // placeholder until a braking capture supplies the real direction.
  const gm_vec3_t up = gm_vec3_normalized(up_sensor);
  const gm_vec3_t axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  int best = 0;
  for (int i = 1; i < 3; ++i)
    if (fabsf(gm_vec3_dot(axes[i], up)) < fabsf(gm_vec3_dot(axes[best], up))) best = i;
  return gm_estimator_make_calibration(up_sensor, axes[best]);
}

void gm_estimator_begin_rest_calibration(gm_estimator_t* e) {
  e->rest_accum = gm_vec3_zero();
  e->rest_count = 0;
}

void gm_estimator_feed_rest_sample(gm_estimator_t* e, gm_vec3_t raw) {
  e->rest_accum = gm_vec3_add(e->rest_accum, raw);
  ++e->rest_count;
}

bool gm_estimator_finish_rest_calibration(gm_estimator_t* e) {
  if (e->rest_count < 8) return false;
  // Validate before committing anything, so a rejected capture leaves the
  // previous calibration untouched.
  const gm_vec3_t mean = gm_vec3_div(e->rest_accum, (float)e->rest_count);
  const float mag = gm_vec3_length(mean);
  if (mag < 0.7f || mag > 1.3f) return false;
  gm_calibration_t c = gm_estimator_make_calibration(mean, e->nominal_forward);
  e->forward_trusted = c.valid;
  if (!c.valid) c = gm_estimator_make_up_only_calibration(mean);  // up is known; forward needs `cal brake`
  if (!c.valid) return false;
  e->cal = c;
  gm_estimator_reset(e);
  return true;
}

bool gm_estimator_calibrate_forward_from_braking(gm_estimator_t* e, gm_vec3_t mean_raw_during_brake) {
  if (!e->cal.valid) return false;
  const gm_vec3_t up = gm_mat3_row(&e->cal.sensor_to_car, 1);
  // The rest vector lies along `up`, so the horizontal part of the raw braking
  // sample is the braking acceleration itself (no stored rest mean needed,
  // which keeps this valid after a reboot).
  gm_vec3_t delta = mean_raw_during_brake;
  delta = gm_vec3_sub(delta, gm_vec3_scale(up, gm_vec3_dot(delta, up)));
  if (gm_vec3_length(delta) < 0.05f) return false;  // needs at least 0.05 g of braking
  // During braking the proper acceleration points backward, so forward is -delta.
  const gm_vec3_t fwd = gm_vec3_neg(delta);
  gm_calibration_t c = gm_estimator_make_calibration(up, fwd);
  if (!c.valid) return false;
  e->cal = c;
  e->forward_trusted = true;
  e->nominal_forward = gm_vec3_normalized(fwd);
  gm_estimator_reset(e);
  return true;
}

gm_gforce_t gm_estimator_update(gm_estimator_t* e, gm_vec3_t raw) {
  e->raw_car = gm_mat3_apply(&e->cal.sensor_to_car, raw);
  if (!e->primed) {
    gm_lowpass_reset(&e->lp_x, e->raw_car.x);
    gm_lowpass_reset(&e->lp_y, e->raw_car.y);
    gm_lowpass_reset(&e->lp_z, e->raw_car.z);
    e->slow_mean = e->raw_car;
    e->primed = true;
  }
  const float fx = gm_lowpass_process(&e->lp_x, e->raw_car.x);
  const float fy = gm_lowpass_process(&e->lp_y, e->raw_car.y);
  const float fz = gm_lowpass_process(&e->lp_z, e->raw_car.z);
  gm_vec3_t f = gm_vec3(fx, fy, fz);

  if (e->auto_zero) {
    // Stillness detector: exponential average of |raw - slow mean|.
    const float dt = 1.0f / e->sample_hz;
    const float a_slow = dt / (dt + 2.0f);  // ~2 s mean
    e->slow_mean = gm_vec3_lerp(e->slow_mean, e->raw_car, a_slow);
    const float dev = gm_vec3_length(gm_vec3_sub(e->raw_car, e->slow_mean));
    e->motion_level = gm_lerpf(e->motion_level, dev, dt / (dt + 1.0f));
    if (e->motion_level < 0.01f) {  // < 0.01 g of jitter: parked
      const float a = dt / (dt + e->auto_zero_tau);
      e->bias.x = gm_lerpf(e->bias.x, f.x, a);
      e->bias.z = gm_lerpf(e->bias.z, f.z, a);
    }
    f.x -= e->bias.x;
    f.z -= e->bias.z;
  }
  e->g.proper = f;
  return e->g;
}
