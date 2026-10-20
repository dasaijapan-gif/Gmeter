// Effect "hud": tactical HUD inspired by the NERV screens in Neon Genesis
// Evangelion. See gm_hud.h for the layout and the anti burn-in orbit.
#include "gm_hud.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "gm_jp_text.h"

// Anti burn-in orbit: right, down, left, up around a 2 x 2 pixel square.
static const int k_orbit_x[4] = {0, 1, 1, 0};
static const int k_orbit_y[4] = {0, 0, 1, 1};

// Rectangle with 45-degree chamfered corners.
static void chamfer_rect(gm_frame_t* f, int z, int x0, int y0, int x1, int y1, int c) {
  gm_frame_hline(f, z, x0 + c, x1 - c, y0);
  gm_frame_hline(f, z, x0 + c, x1 - c, y1);
  gm_frame_vline(f, z, x0, y0 + c, y1 - c);
  gm_frame_vline(f, z, x1, y0 + c, y1 - c);
  gm_frame_line(f, z, x0, y0 + c, x0 + c, y0);
  gm_frame_line(f, z, x1 - c, y0, x1, y0 + c);
  gm_frame_line(f, z, x1, y1 - c, x1 - c, y1);
  gm_frame_line(f, z, x0 + c, y1, x0, y1 - c);
}

static void hud_reset(gm_effect_t* self) {
  gm_hud_t* h = (gm_hud_t*)self;
  gm_gstate_reset(&h->tracker);
  gm_spring3_snap(&h->ball, gm_vec3_zero());
  h->tip = gm_vec3_zero();
  h->time = 0.0f;
  h->title_age = 0.0f;
  h->bump_age = 99.0f;
  h->hazard = 0.0f;
  h->danger = false;
  h->trail_head = h->trail_count = 0;
  h->trail_timer = 0.0f;
  // The burn-in orbit keeps its place across re-activation.
}

int gm_hud_orbit_x(const gm_hud_t* h) { return k_orbit_x[h->orbit_step & 3]; }
int gm_hud_orbit_y(const gm_hud_t* h) { return k_orbit_y[h->orbit_step & 3]; }

static void hud_burn_in_offset(const gm_effect_t* self, int* dx, int* dy) {
  const gm_hud_t* h = (const gm_hud_t*)self;
  *dx = gm_hud_orbit_x(h);
  *dy = gm_hud_orbit_y(h);
}

static gm_vec3_t travel(const gm_hud_t* h) {
  const gm_vec3_t he = gm_half_extent();
  return gm_vec3((he.x - 10.0f) / h->scale_g, (he.y - 8.0f) / h->vscale_g,
                 (he.z - GM_LAYER_PITCH_PX * 0.5f) / h->dscale_g);
}

const char* gm_hud_title_text(const gm_hud_t* h) {
  if (h->bump_age < 1.2f) return "衝撃";
  if (h->danger) return "危険";
  switch (gm_gstate_state(&h->tracker)) {
    case GM_DRIVE_ACCELERATE: return "加速";
    case GM_DRIVE_BRAKE: return "制動";
    case GM_DRIVE_LEFT: return "左旋回";
    case GM_DRIVE_RIGHT: return "右旋回";
    default: return "待機";
  }
}

static void hud_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_hud_t* h = (gm_hud_t*)self;
  h->g = ctx->g;
  // Blink clock: wrapped so it keeps full float precision after days on.
  h->time += ctx->dt;
  if (h->time >= 60.0f) h->time -= 60.0f;
  if (h->orbit_min > 0.0f) {
    h->orbit_timer += ctx->dt;
    if (h->orbit_timer >= h->orbit_min * 60.0f) {
      h->orbit_timer = 0.0f;
      h->orbit_step = (h->orbit_step + 1) & 3;
    }
  } else {
    h->orbit_timer = 0.0f;
    h->orbit_step = 0;
  }
  h->title_age += ctx->dt;
  h->bump_age += ctx->dt;
  h->tracker.enter_g = h->enter_g;
  h->tracker.hold_s = h->hold_s;
  h->tracker.bump_g = h->bump_g;
  bool bumped = false;
  if (gm_gstate_update(&h->tracker, ctx->g, ctx->dt, &bumped)) h->title_age = 0.0f;
  if (bumped) {
    h->bump_age = 0.0f;
    h->title_age = 0.0f;
  }
  const float horiz = gm_gforce_horizontal(ctx->g);
  const bool danger = horiz >= h->danger_g;
  if (danger != h->danger) {
    h->danger = danger;
    h->title_age = 0.0f;
  }

  // Tip of the g-vector line, clamped inside the volume; the ball follows it.
  const gm_vec3_t drive = h->inertia > 0.5f ? gm_gforce_inertial(ctx->g) : gm_gforce_dynamic(ctx->g);
  const gm_vec3_t he = gm_half_extent();
  const gm_vec3_t tr = travel(h);
  h->tip = gm_vec3(gm_clampf(drive.x * tr.x, -(he.x - 6.0f), he.x - 6.0f),
                   gm_clampf(drive.y * tr.y, -(he.y - 5.0f), he.y - 5.0f),
                   gm_clampf(drive.z * tr.z, -(he.z - GM_LAYER_PITCH_PX * 0.5f), he.z - GM_LAYER_PITCH_PX * 0.5f));
  gm_spring3_step(&h->ball, h->tip, ctx->dt, h->hz, 1.0f);

  h->hazard += ctx->dt * (10.0f + 30.0f * horiz);
  if (h->hazard > 1000.0f) h->hazard -= 1000.0f;

  const float interval = gm_maxf(0.02f, h->trail_s / (float)(GM_HUD_TRAIL - 1));
  h->trail_timer += ctx->dt;
  if (h->trail_s > 0.0f && h->trail_timer >= interval) {
    h->trail_timer -= interval;
    if (h->trail_timer > interval) h->trail_timer = 0.0f;
    h->trail[h->trail_head] = gm_spring3_pos(&h->ball);
    h->trail_head = (h->trail_head + 1) % GM_HUD_TRAIL;
    if (h->trail_count < GM_HUD_TRAIL) ++h->trail_count;
  }
}

static void draw_frame(const gm_hud_t* h, gm_frame_t* f, float horiz) {
  const int W = GM_W, H = GM_H;
  // One pixel of margin on the right and bottom keeps the border on the glass
  // while the burn-in orbit moves everything right or down by a pixel.
  chamfer_rect(f, 0, 0, 0, W - 2, H - 2, 6);
  // Corner brackets just inside the frame.
  gm_frame_hline(f, 0, 3, 9, 3);
  gm_frame_vline(f, 0, 3, 3, 7);
  gm_frame_hline(f, 0, W - 10, W - 4, 3);
  gm_frame_vline(f, 0, W - 4, 3, 7);
  gm_frame_hline(f, 0, 3, 9, H - 4);
  gm_frame_vline(f, 0, 3, H - 8, H - 4);
  gm_frame_hline(f, 0, W - 10, W - 4, H - 4);
  gm_frame_vline(f, 0, W - 4, H - 8, H - 4);
  // Readouts: 横G (lateral) top-left, 縦G (longitudinal) top-right.
  char buf[12];
  const gm_vec3_t d = gm_gforce_dynamic(h->g);
  snprintf(buf, sizeof(buf), "%+.2f", (double)d.x);
  int x = 12 + gm_jp_draw(f, &gm_jp_small, 0, 12, 4, "横G", 1, 1.0f);
  gm_frame_text(f, 0, x + 3, 9, buf);
  snprintf(buf, sizeof(buf), "%+.2f", (double)d.z);
  const int wv = gm_jp_measure(&gm_jp_small, "縦G", 1) + 3 + 4 * 5;
  x = W - 12 - wv;
  x += gm_jp_draw(f, &gm_jp_small, 0, x, 4, "縦G", 1, 1.0f);
  gm_frame_text(f, 0, x + 3, 9, buf);
  // Status column (vertical writing) on the right: 正常, or 警告 blinking.
  // It ends on row H-11, a row above the hazard stripes and clear of the
  // bottom 7 rows where the engine puts its DEMO / NO SENSOR tag.
  const bool warn = horiz >= h->warn_g;
  const int col_x = W - 6 - gm_jp_small.glyphs[0].width;
  const int col_y = gm_mini(21, H - 11 - 2 * gm_jp_small.height);  // 2 glyphs + 1 px gap
  if (!warn)
    gm_jp_draw_vertical(f, &gm_jp_small, 0, col_x, col_y, "正常", 1, 1.0f);
  else if (fmodf(h->time, 0.5f) < 0.3f)
    gm_jp_draw_vertical(f, &gm_jp_small, 0, col_x, col_y, "警告", 1, 1.0f);
  // Bottom band: hazard stripes when over warn_g.
  if (warn) {
    // (int)hazard. The hazard clock is NaN, infinite or huge only after absurd
    // g input; the C++ conversion is then undefined (INT_MIN when built for
    // x86), so give INT_MIN without the undefined behaviour. The bound keeps
    // xx + y + off inside int too.
    const int off = h->hazard >= -2147483648.0f && h->hazard < 2147483520.0f ? (int)h->hazard : INT_MIN;
    for (int y = H - 9; y <= H - 6; ++y)
      for (int xx = 12; xx < W - 12; ++xx)
        if (((xx + y + off) / 3) % 2 == 0) gm_frame_set(f, xx, y, 0);
  }
}

static void draw_axes(const gm_hud_t* h, gm_frame_t* f) {
  const int W = GM_W, H = GM_H, D = GM_D;
  const int cx = W / 2, cy = H / 2;
  const int axis_z = (D - 1) / 2;
  const gm_vec3_t tr = travel(h);
  // Lateral and vertical axis lines on the middle panel, with ticks.
  gm_frame_hline(f, axis_z, 10, W - 11, cy);
  gm_frame_vline(f, axis_z, cx, 4, H - 5);
  for (int s = -1; s <= 1; s += 2)
    for (int k = 1; k <= 2; ++k) {
      const float gval = 0.5f * (float)k;
      const int tx = cx + s * gm_iround(tr.x * gval);
      const int len = k == 2 ? 3 : 1;
      if (tx > 8 && tx < W - 8) gm_frame_vline(f, axis_z, tx, cy - len, cy + len);
      const int ty = cy - s * gm_iround(tr.y * gval);
      if (ty > 3 && ty < H - 4) gm_frame_hline(f, axis_z, cx - len, cx + len, ty);
    }
  // Depth axis: a dotted line through every panel, ticks at 0.5 g / 1 g.
  for (int z = 0; z < D; ++z) gm_frame_set(f, cx, cy, z);
  for (int k = -2; k <= 2; ++k) {
    if (k == 0) continue;
    const float zw = tr.z * 0.5f * (float)k;
    const int layer = gm_iround(gm_world_to_voxel(gm_vec3(0, 0, zw)).z);
    if (layer >= 0 && layer < D && layer != axis_z)
      gm_frame_hline(f, layer, cx - (abs(k) == 2 ? 3 : 1), cx + (abs(k) == 2 ? 3 : 1), cy);
  }
  // Markers that ride the axes with the ball's components.
  const gm_vec3_t b = gm_world_to_voxel(gm_spring3_pos(&h->ball));
  const int bx = gm_iround(b.x), by = gm_iround(b.y), bz = gm_iround(b.z);
  gm_frame_vline(f, axis_z, bx, cy - 2, cy + 2);
  gm_frame_hline(f, axis_z, cx - 2, cx + 2, by);
  if (bz >= 0 && bz < D) gm_frame_circle(f, bz, cx, cy, 2);
  // Rulers on the front panel: lateral along the bottom, vertical on the left.
  if (h->frame > 0.5f) {
    const int ry = H - 5;
    for (int x = 12; x <= W - 13; x += 4) gm_frame_set(f, x, ry, 0);
    gm_frame_hline(f, 0, bx - 2, bx + 2, ry + 1);  // caret
    gm_frame_set(f, bx, ry, 0);
    gm_frame_set(f, bx, ry - 1, 0);
    const int rx = 5;
    for (int y = 12; y <= H - 12; y += 4) gm_frame_set(f, rx, y, 0);
    gm_frame_vline(f, 0, rx - 1, by - 2, by + 2);
    gm_frame_set(f, rx, by, 0);
    gm_frame_set(f, rx + 1, by, 0);
  }
}

static void draw_title(const gm_hud_t* h, gm_frame_t* f) {
  const char* word = gm_hud_title_text(h);
  // Idle fades after a few seconds so nothing static burns into the panels.
  const bool idle = word[0] && gm_gstate_state(&h->tracker) == GM_DRIVE_IDLE && h->bump_age >= 1.2f && !h->danger;
  if (idle && h->title_age > 3.0f) return;
  const int z = gm_clampi(gm_iround(h->title_z), 0, GM_D - 1);
  const int w = gm_jp_measure(&gm_jp_large, word, 2);
  const int x = (GM_W - w) / 2, y = (GM_H - gm_jp_large.height) / 2;
  // Reverse-video flash for the first moments (an NGE title-card cut).
  const bool flash = h->title_age < 0.35f && fmodf(h->title_age, 0.12f) < 0.08f;
  if (flash) {
    gm_frame_fill_rect(f, z, x - 4, y - 3, x + w + 3, y + gm_jp_large.height + 2, 1.0f);
    gm_jp_draw(f, &gm_jp_large, z, x, y, word, 2, -1.0f);
  } else {
    gm_jp_draw(f, &gm_jp_large, z, x, y, word, 2, 1.0f);
    // Underline bar, NGE caption style.
    gm_frame_hline(f, z, x, x + w - 1, y + gm_jp_large.height + 2);
  }
}

static void hud_render(gm_effect_t* self, gm_frame_t* f) {
  gm_hud_t* h = (gm_hud_t*)self;
  const float horiz = gm_gforce_horizontal(h->g);
  if (h->title > 0.5f) draw_title(h, f);
  if (h->axes > 0.5f) draw_axes(h, f);

  // The g-vector line from the centre to its tip, through the stack.
  gm_frame_line3(f, gm_vec3_zero(), h->tip, 1.0f);
  // Trail of where the ball has been, fading.
  if (h->trail_s > 0.0f) {
    for (int i = 0; i < h->trail_count; ++i) {
      const int idx = (h->trail_head - 1 - i + GM_HUD_TRAIL) % GM_HUD_TRAIL;
      const float fade = 1.0f - (float)i / (float)GM_HUD_TRAIL;
      gm_frame_point(f, h->trail[idx], 0.7f * fade);
    }
  }
  gm_frame_sphere(f, gm_spring3_pos(&h->ball), h->ball_r, 0.8f * GM_LAYER_PITCH_PX, 1.0f);
  // Target-lock bracket on the FRONT panel, tracking the ball behind it. Rear
  // panels are dim through the glass, so the brightest layer carries the
  // lock; the bracket shrinks as the ball recedes (a perspective cue).
  {
    const gm_vec3_t b = gm_world_to_voxel(gm_spring3_pos(&h->ball));
    const float depth = gm_clampf(b.z / (float)(GM_D - 1), 0.0f, 1.0f);
    const int bx = gm_iround(b.x), by = gm_iround(b.y);
    const int r = gm_iround(h->ball_r + 2.0f + 5.0f * (1.0f - depth));
    const int arm = gm_maxi(2, r / 2);
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2) {
        gm_frame_hline(f, 0, bx + sx * r, bx + sx * (r - arm), by + sy * r);
        gm_frame_vline(f, 0, bx + sx * r, by + sy * r, by + sy * (r - arm));
      }
  }

  if (h->frame > 0.5f) {
    draw_frame(h, f, horiz);
    if (h->bump_age < 0.3f) chamfer_rect(f, 1, 2, 2, GM_W - 3, GM_H - 3, 6);  // impact flash
  }
}

// No activate / deactivate hooks: activating the HUD just resets it.
static const gm_effect_vtbl_t k_hud_vtbl = {
    .name = "hud",
    .activate = NULL,
    .deactivate = NULL,
    .reset = hud_reset,
    .update = hud_update,
    .render = hud_render,
    .burn_in_offset = hud_burn_in_offset,
};

void gm_hud_init(gm_hud_t* h) {
  h->base.vt = &k_hud_vtbl;
  h->scale_g = 1.0f;
  h->vscale_g = 0.6f;
  h->dscale_g = 1.0f;
  h->hz = 2.5f;
  h->inertia = 1.0f;
  h->warn_g = 0.7f;
  h->danger_g = 1.0f;
  h->ball_r = 4.5f;
  h->title_z = 2.0f;
  h->trail_s = 0.8f;
  h->title = 1.0f;
  h->axes = 1.0f;
  h->frame = 1.0f;
  h->enter_g = 0.25f;
  h->hold_s = 0.4f;
  h->bump_g = 0.3f;
  h->orbit_min = 2.0f;

  gm_gstate_init(&h->tracker);
  gm_spring3_snap(&h->ball, gm_vec3_zero());
  h->tip = gm_vec3_zero();
  h->g = gm_gforce_rest();
  h->time = 0.0f;
  h->orbit_timer = 0.0f;
  h->orbit_step = 0;
  h->title_age = 99.0f;
  h->bump_age = 99.0f;
  h->hazard = 0.0f;
  h->danger = false;
  for (int i = 0; i < GM_HUD_TRAIL; ++i) h->trail[i] = gm_vec3_zero();
  h->trail_head = h->trail_count = 0;
  h->trail_timer = 0.0f;

  gm_param_table_t* p = &h->base.params;
  gm_params_init(p);
  gm_params_add(p, "scale_g", &h->scale_g, 0.2f, 3.0f, "lateral g at full travel");
  gm_params_add(p, "vscale_g", &h->vscale_g, 0.1f, 3.0f, "vertical g at full travel");
  gm_params_add(p, "dscale_g", &h->dscale_g, 0.2f, 3.0f, "longitudinal g at full depth");
  gm_params_add(p, "hz", &h->hz, 0.3f, 8.0f, "ball follow speed");
  gm_params_add(p, "inertia", &h->inertia, 0.0f, 1.0f, "1 = ball goes where a loose object would");
  gm_params_add(p, "warn_g", &h->warn_g, 0.1f, 3.0f, "hazard stripes and 警告 above this g");
  gm_params_add(p, "danger_g", &h->danger_g, 0.2f, 4.0f, "title shows 危険 above this g");
  gm_params_add(p, "ball_r", &h->ball_r, 1.5f, 8.0f, "ball radius px");
  gm_params_add(p, "title_z", &h->title_z, 0.0f, (float)(GM_D - 1), "panel for the state kanji (0 = front)");
  gm_params_add(p, "trail_s", &h->trail_s, 0.0f, 3.0f, "trail length seconds");
  gm_params_add(p, "title", &h->title, 0.0f, 1.0f, "big state kanji (on the panel set by title_z)");
  gm_params_add(p, "axes", &h->axes, 0.0f, 1.0f, "axis lines and rulers");
  gm_params_add(p, "frame", &h->frame, 0.0f, 1.0f, "border, readouts and status");
  gm_params_add(p, "enter_g", &h->enter_g, 0.05f, 1.5f, "g that changes the state word");
  gm_params_add(p, "hold_s", &h->hold_s, 0.0f, 3.0f, "minimum seconds per state word");
  gm_params_add(p, "bump_g", &h->bump_g, 0.05f, 2.0f, "vertical spike that shows 衝撃");
  gm_params_add(p, "orbit_min", &h->orbit_min, 0.0f, 30.0f, "minutes between 1-px shifts against burn-in (0 = off)");
  hud_reset(&h->base);
}
