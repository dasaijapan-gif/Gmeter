// Effect "sync": sync-graph oscilloscope, after the plug-suit sync displays in
// Neon Genesis Evangelion. Lateral, longitudinal and vertical g scroll right to
// left as three traces, each in its own band and on its own panel (panels 2-4,
// so they separate in depth as well), labelled 横 / 縦 / 上下, with a 同期率
// readout of the combined horizontal g as a percentage of 1 g.
#include "gm_sync.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gm_jp_text.h"

// Row of a trace sample: cy - gm_iround(v * gain), clamped to the band. The
// C++ writes exactly that, and its int conversion and subtraction overflow
// (undefined behaviour) when v * gain is NaN, infinite or beyond the int
// range, which only absurd g input produces; built for x86 such samples land
// on the top edge of the band. This gives the rows of such a build for every
// input, without the undefined behaviour.
static int trace_row(float v, float gain, int cy, int half) {
  const float r = floorf(v * gain + 0.5f);  // gm_iround(v * gain) before the conversion to int
  if (!(r > -2147483648.0f && r < 2147483648.0f)) return cy - half;
  return gm_clampi(cy - (int)r, cy - half, cy + half);
}

// (int)v with the result an x86 build of the C++ gets for NaN, infinite and
// out-of-range values (INT_MIN), without the undefined behaviour.
static int to_int(float v) { return v >= -2147483648.0f && v < 2147483648.0f ? (int)v : INT_MIN; }

static void sync_reset(gm_effect_t* self) {
  gm_sync_t* s = (gm_sync_t*)self;
  memset(s->hist, 0, sizeof(s->hist));
  s->head = 0;
  s->accum = 0.0f;
  s->scroll = 0.0f;
  s->sync_pct = 0.0f;
}

static void sync_update(gm_effect_t* self, const gm_effect_ctx_t* ctx) {
  gm_sync_t* s = (gm_sync_t*)self;
  const gm_vec3_t d = gm_gforce_dynamic(ctx->g);
  s->accum += ctx->dt * s->rate;
  int n = 0;
  while (s->accum >= 1.0f && n < 16) {
    s->accum -= 1.0f;
    ++n;
    s->hist[0][s->head] = d.x;  // lateral
    s->hist[1][s->head] = d.z;  // longitudinal
    s->hist[2][s->head] = d.y;  // vertical
    s->head = (s->head + 1) % GM_SYNC_HIST;
    s->scroll += 1.0f;
    if (s->scroll >= 16.0f) s->scroll -= 16.0f;
  }
  if (s->accum > 1.0f) s->accum = 0.0f;
  s->sync_pct = gm_lerpf(s->sync_pct, 100.0f * gm_gforce_horizontal(ctx->g), gm_minf(1.0f, ctx->dt * 6.0f));
}

static void sync_render(gm_effect_t* self, gm_frame_t* f) {
  gm_sync_t* s = (gm_sync_t*)self;
  const int W = GM_W, H = GM_H, D = GM_D;
  const int band_h = H / 3;
  const int x0 = s->labels > 0.5f ? 18 : 2;
  const int x1 = s->labels > 0.5f ? W - 16 : W - 3;
  const char* names[3] = {"横", "縦", "上下"};
  for (int b = 0; b < 3; ++b) {
    const int z = gm_mini(D - 1, 1 + b);  // panels 2-4: separated in depth, still bright
    const int top = b * band_h, bottom = top + band_h - 1;
    const int cy = top + band_h / 2;
    const int half = band_h / 2 - 1;
    if (s->grid > 0.5f) {
      for (int x = x0; x <= x1; x += 3) gm_frame_set(f, x, cy, z);  // dotted zero line
      // Vertical divisions scroll with the trace.
      const int sc = (int)s->scroll;
      for (int x = x1 - sc; x >= x0; x -= 16)
        for (int y = top + 1; y < bottom; y += 2) gm_frame_set(f, x, y, D - 1);
    }
    // Trace: newest sample at the right edge, joined by line segments.
    int prev_x = -1, prev_y = 0;
    for (int x = x1; x >= x0; --x) {
      const int age = x1 - x;
      if (age >= GM_SYNC_HIST) break;
      const float v = s->hist[b][(s->head - 1 - age + 2 * GM_SYNC_HIST) % GM_SYNC_HIST];
      const int y = trace_row(v, s->gain, cy, half);
      if (prev_x >= 0)
        gm_frame_line(f, z, prev_x, prev_y, x, y);
      else
        gm_frame_set(f, x, y, z);
      prev_x = x;
      prev_y = y;
    }
    if (s->labels > 0.5f) gm_jp_draw(f, &gm_jp_small, 0, 1, cy - gm_jp_small.height / 2, names[b], 0, 1.0f);
    if (b > 0)
      for (int x = x0; x <= x1; x += 2) gm_frame_set(f, x, top, 0);  // band divider
  }
  if (s->labels > 0.5f) {
    // 同期率 written vertically on the right, percentage underneath.
    const int col_x = W - 1 - gm_jp_small.glyphs[0].width;
    const int used = gm_jp_draw_vertical(f, &gm_jp_small, 0, col_x, 0, "同期率", 0, 1.0f);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", to_int(s->sync_pct + 0.5f));
    gm_frame_text(f, 0, W - 4 * (int)strlen(buf), gm_mini(H - 6, used + 2), buf);
  }
}

// No activate / deactivate hooks (activating resets) and no burn-in offset.
static const gm_effect_vtbl_t k_sync_vtbl = {
    .name = "sync",
    .activate = NULL,
    .deactivate = NULL,
    .reset = sync_reset,
    .update = sync_update,
    .render = sync_render,
    .burn_in_offset = NULL,
};

void gm_sync_init(gm_sync_t* s) {
  s->base.vt = &k_sync_vtbl;
  s->rate = 30.0f;
  s->gain = 9.0f;
  s->grid = 1.0f;
  s->labels = 1.0f;
  s->head = 0;
  s->accum = 0.0f;
  s->scroll = 0.0f;
  s->sync_pct = 0.0f;

  gm_param_table_t* p = &s->base.params;
  gm_params_init(p);
  gm_params_add(p, "rate", &s->rate, 5.0f, 120.0f, "samples per second (window = 128 / rate)");
  gm_params_add(p, "gain", &s->gain, 2.0f, 30.0f, "trace px per g");
  gm_params_add(p, "grid", &s->grid, 0.0f, 1.0f, "grid lines");
  gm_params_add(p, "labels", &s->labels, 0.0f, 1.0f, "横 / 縦 / 上下 labels and 同期率");
  sync_reset(&s->base);
}
