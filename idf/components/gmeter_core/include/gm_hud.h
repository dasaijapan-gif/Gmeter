// Effect "hud": tactical HUD inspired by the NERV screens in Neon Genesis
// Evangelion.
//
//   front panel   chamfered frame, corner brackets, 横G / 縦G readouts, a
//                 lateral ruler along the bottom and a vertical ruler on the
//                 left with carets that ride the ball's position, a vertical
//                 正常 / 警告 status column, hazard stripes above warn_g
//   middle panel  axis lines (lateral and vertical) with ticks every 0.5 g;
//                 a dotted depth axis runs through every panel
//   through the   a g-vector line from the centre to the current g, and a
//   stack         ball that follows the line out to its tip with a trail;
//                 small markers ride each axis with the ball's components
//   third panel   the driving state as large kanji (待機 加速 制動 左旋回
//                 右旋回 衝撃 危険) that flash in reverse video when it changes
//                 (title_z moves it; rear panels are dim through the glass)
//   front panel   a target-lock bracket tracks the ball from the brightest
//                 layer and shrinks as the ball recedes into the stack
//
// Built to stay on for hours: every orbit_min minutes the whole picture,
// engine overlays included, moves one pixel around a 2 x 2 square (right,
// down, left, up). That spreads the wear of the frame, rulers and character
// edges, which never move otherwise, over neighbouring OLED pixels; the
// inside of strokes wider than 1 px still stays lit. The orbit keeps its
// place when the effect is re-activated; gm_hud_set_orbit_step picks the start.
#pragma once

#include <stdbool.h>

#include "gm_effect.h"
#include "gm_gstate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_HUD_TRAIL 40

typedef struct {
  gm_effect_t base;

  float scale_g;    // lateral g at full travel
  float vscale_g;   // vertical g at full travel
  float dscale_g;   // longitudinal g at full depth
  float hz;         // ball spring
  float inertia;    // 1: ball goes where a loose object would
  float warn_g;     // hazard stripes and 警告 above this
  float danger_g;   // title switches to 危険 above this
  float ball_r;     // ball radius px
  float title_z;    // panel for the state kanji (rear panels are dim through the stack)
  float trail_s;    // trail length seconds
  float title;      // big kanji on the back panel
  float axes;       // axis lines and rulers
  float frame;      // border, readouts, status column
  float enter_g, hold_s, bump_g;
  float orbit_min;  // minutes per 1-px orbit step (0 = off)

  gm_gstate_t tracker;
  gm_spring3_t ball;
  gm_vec3_t tip;
  gm_gforce_t g;
  float time;  // wraps every 60 s (blink timing only)
  float orbit_timer;
  int orbit_step;
  float title_age;
  float bump_age;
  float hazard;
  bool danger;
  gm_vec3_t trail[GM_HUD_TRAIL];
  int trail_head, trail_count;
  float trail_timer;
} gm_hud_t;

void gm_hud_init(gm_hud_t* h);

static inline gm_vec3_t gm_hud_ball_position(const gm_hud_t* h) { return gm_spring3_pos(&h->ball); }
static inline gm_vec3_t gm_hud_tip_position(const gm_hud_t* h) { return h->tip; }
static inline gm_drive_state_t gm_hud_state(const gm_hud_t* h) { return gm_gstate_state(&h->tracker); }
const char* gm_hud_title_text(const gm_hud_t* h);
// Current anti burn-in offset in pixels (0 or 1 on each axis).
int gm_hud_orbit_x(const gm_hud_t* h);
int gm_hud_orbit_y(const gm_hud_t* h);
// Start the orbit at step 0..3 (e.g. a random one at power-on, so short trips
// do not always use the same position).
static inline void gm_hud_set_orbit_step(gm_hud_t* h, int step) {
  h->orbit_step = step & 3;
  h->orbit_timer = 0.0f;
}

#ifdef __cplusplus
}
#endif
