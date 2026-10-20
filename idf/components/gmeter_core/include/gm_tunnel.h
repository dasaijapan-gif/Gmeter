// Effect "tunnel": one ring per layer, bending with g.
#pragma once

#include "gm_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  gm_effect_t base;

  float scale_g;      // g for full bend
  float r_front;      // default GM_H / 2 - 2
  float r_back;
  float segments;     // dashes per ring
  float spin;         // rad/s at rest
  float spin_per_g;   // extra rad/s per g lateral
  float pulse_hz;     // pulses/s at rest
  float pulse_per_g;  // pulses/s per g longitudinal
  float hz;           // smoothing

  gm_spring3_t offs;
  float phase;
  float depth_phase;
} gm_tunnel_t;

void gm_tunnel_init(gm_tunnel_t* t);

#ifdef __cplusplus
}
#endif
