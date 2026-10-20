// G-force frame conventions. Everything downstream of the IMU uses this.
//
// CAR FRAME, units of g:
//   x  = right   (+)   / left     (-)
//   y  = up      (+)   / down     (-)
//   z  = forward (+)   / backward (-)   == deeper into the display stack
//
// Note this frame is LEFT-handed (x right, y up, z away from the viewer, as on
// a screen): right = forward x up, not up x forward. The accelerometer is
// right-handed, so the sensor-to-car matrix built by gm_estimator has
// determinant -1. gm_vec3_cross is the ordinary right-handed cross product.
//
// `proper` is the proper acceleration an accelerometer measures, rotated into
// the car frame. A car at rest on level ground reads (0, +1, 0): the sensor
// feels the ground pushing up. Braking (decelerating) adds a -z component,
// accelerating adds +z, a right turn adds +x (centripetal, toward the turn
// centre), a left turn adds -x.
//
// Loose objects, liquids and the occupant are pushed the OPPOSITE way to the
// proper acceleration, so the body force per unit mass acting on anything
// simulated inside the display volume is simply -proper (effective gravity).
#pragma once

#include "gm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  gm_vec3_t proper;
} gm_gforce_t;

// A car at rest on level ground. Use it wherever a g-force starts out, since
// a zeroed gm_gforce_t means free fall, not rest.
#define GM_GFORCE_REST_INIT {{0.0f, 1.0f, 0.0f}}

static inline gm_vec3_t gm_gforce_rest_proper(void) { return gm_vec3(0.0f, 1.0f, 0.0f); }
static inline gm_gforce_t gm_gforce_rest(void) {
  gm_gforce_t g = GM_GFORCE_REST_INIT;
  return g;
}
static inline gm_gforce_t gm_gforce_from_proper(gm_vec3_t proper) {
  gm_gforce_t g;
  g.proper = proper;
  return g;
}

// Acceleration of the car with gravity removed (what a classic g-meter needle
// shows). At rest (0,0,0).
static inline gm_vec3_t gm_gforce_dynamic(gm_gforce_t g) { return gm_vec3_sub(g.proper, gm_gforce_rest_proper()); }

// Direction and magnitude (in g) loose objects move relative to the car.
// Braking means proper.z < 0, so inertial.z > 0: objects fly forward toward
// the windshield (deeper into the stack). Right turn -> inertial.x < 0:
// objects slide left.
static inline gm_vec3_t gm_gforce_inertial(gm_gforce_t g) { return gm_vec3_neg(gm_gforce_dynamic(g)); }

// Body force per unit mass on a simulated fluid, in g. At rest (0,-1,0).
static inline gm_vec3_t gm_gforce_effective_gravity(gm_gforce_t g) { return gm_vec3_neg(g.proper); }

// Horizontal (lateral + longitudinal) g, the number drivers care about.
static inline float gm_gforce_horizontal(gm_gforce_t g) {
  const gm_vec3_t d = gm_gforce_dynamic(g);
  return sqrtf(d.x * d.x + d.z * d.z);
}
static inline float gm_gforce_lateral(gm_gforce_t g) { return gm_gforce_dynamic(g).x; }
static inline float gm_gforce_longitudinal(gm_gforce_t g) { return gm_gforce_dynamic(g).z; }
static inline float gm_gforce_vertical(gm_gforce_t g) { return gm_gforce_dynamic(g).y; }

#ifdef __cplusplus
}
#endif
