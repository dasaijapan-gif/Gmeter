// G-force frame conventions. Everything downstream of the IMU uses this.
//
// CAR FRAME (right-handed), units of g:
//   x  = right   (+)   / left     (-)
//   y  = up      (+)   / down     (-)
//   z  = forward (+)   / backward (-)   == deeper into the display stack
//
// `proper` is the proper acceleration an accelerometer measures, rotated into
// the car frame. A car at rest on level ground reads (0, +1, 0): the sensor
// feels the ground pushing up. Braking (decelerating) adds a -z component,
// accelerating adds +z, a right turn adds +x (centripetal, toward the turn
// centre), a left turn adds -x.
//
// Loose objects, liquids and the occupant are pushed the OPPOSITE way to the
// proper acceleration, so the body force per unit mass acting on anything
// simulated inside the display volume is simply -proper (`effectiveGravity`).
#pragma once

#include "math.hpp"

namespace gm {

struct GForce {
  Vec3 proper{0.0f, 1.0f, 0.0f};

  static constexpr Vec3 kRestProper() { return Vec3{0.0f, 1.0f, 0.0f}; }

  // Acceleration of the car with gravity removed (what a classic g-meter
  // needle shows). At rest (0,0,0).
  Vec3 dynamic() const { return proper - kRestProper(); }

  // Direction and magnitude (in g) loose objects move relative to the car.
  // Braking -> +z... no: braking means proper.z < 0, so inertial.z > 0: objects
  // fly forward toward the windshield (deeper into the stack). Right turn ->
  // inertial.x < 0: objects slide left.
  Vec3 inertial() const { return -dynamic(); }

  // Body force per unit mass on a simulated fluid, in g. At rest (0,-1,0).
  Vec3 effectiveGravity() const { return -proper; }

  // Horizontal (lateral + longitudinal) g, the number drivers care about.
  float horizontalG() const {
    const Vec3 d = dynamic();
    return std::sqrt(d.x * d.x + d.z * d.z);
  }
  float lateralG() const { return dynamic().x; }
  float longitudinalG() const { return dynamic().z; }
  float verticalG() const { return dynamic().y; }
};

}  // namespace gm
