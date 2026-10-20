// Differential tests of the voxel frame (gm_frame.c / gm_frame.h vs
// gm::VoxelFrame): every primitive with random, out-of-range and negative
// arguments, the 3x5 font, clear_box, shift, crossfade, or and the counters.
// Both frames are compared byte for byte after every call.
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

#include "diff_util.hpp"

using diff::Expect;
using diff::FramePair;
using diff::same;

namespace {

// Integer coordinate: mostly in or near range, sometimes far out. `extreme`
// adds INT_MIN/INT_MAX-like values for the arguments that are clipped before
// any arithmetic (set/get/reset, hline, vline, line, rect, clear_box, fill_rect
// x/y, disc z, shift).
int coord(diff::Rand& r, int n, bool extreme) {
  const int k = r.irange(0, 19);
  if (k < 12) return r.irange(-4, n + 3);
  if (k < 17 || !extreme) return r.irange(-3 * n - 50, 4 * n + 50);
  static const int kExtremes[] = {INT_MIN,   INT_MIN + 1,   -1000000, -(1 << 14) - 1, -(1 << 14),
                                  (1 << 14), (1 << 14) + 1, 1000000,  INT_MAX - 1,    INT_MAX};
  return r.pick(kExtremes);
}

float intensity(diff::Rand& r) {
  static const float kSpecial[] = {0.0f,  -0.0f,        1.0f,         -1.0f,         1.5f,
                                   0.5f,  1.0f / 64.0f, 0.5f / 64.0f, 63.5f / 64.0f, 0.99999994f,
                                   1e-7f, diff::kNaN,   diff::kInf,   -diff::kInf};
  return r.chance(0.3) ? r.pick(kSpecial) : r.frange(-0.2f, 1.2f);
}

gm::Vec3 world(diff::Rand& r, float spread) {
  const gm::Vec3 h = gm::VoxelFrame::halfExtent();
  return gm::Vec3{r.frange(-h.x, h.x) * spread, r.frange(-h.y, h.y) * spread, r.frange(-h.z, h.z) * spread};
}

void randomize(FramePair& f, diff::Rand& r, int density) {
  uint8_t* c = gm_frame_data(f.c.get());
  for (int i = 0; i < GM_BYTES_PER_FRAME; ++i) {
    uint8_t v = 0;
    for (int b = 0; b < 8; ++b)
      if (r.irange(0, 99) < density) v |= static_cast<uint8_t>(1u << b);
    c[i] = v;
  }
  std::memcpy(f.p->data(), c, GM_BYTES_PER_FRAME);
}

std::string random_text(diff::Rand& r) {
  std::string s;
  const int n = r.irange(0, 12);
  for (int i = 0; i < n; ++i) {
    if (r.chance(0.5))
      s.push_back(static_cast<char>(r.irange(32, 126)));
    else
      s.push_back(static_cast<char>(r.irange(1, 255)));
  }
  return s;
}

// One random drawing call on both frames. Returns a short description.
const char* random_op(FramePair& f, diff::Rand& r, Expect& ex) {
  gm_frame_t* c = f.c.get();
  gm::VoxelFrame& p = *f.p;
  const int W = GM_W, H = GM_H, D = GM_D;
  switch (r.irange(0, 18)) {
    case 0: {
      const int x = coord(r, W, true), y = coord(r, H, true), z = coord(r, D, true);
      gm_frame_set(c, x, y, z);
      p.set(x, y, z);
      return "set";
    }
    case 1: {
      const int x = coord(r, W, true), y = coord(r, H, true), z = coord(r, D, true);
      gm_frame_reset(c, x, y, z);
      p.reset(x, y, z);
      return "reset";
    }
    case 2: {
      // plot computes the Bayer threshold before the bounds check, so keep
      // the coordinates where that arithmetic is defined.
      const int x = coord(r, W, false), y = coord(r, H, false), z = coord(r, D, false);
      const float v = intensity(r);
      gm_frame_plot(c, x, y, z, v);
      p.plot(x, y, z, v);
      return "plot";
    }
    case 3: {
      const int z = coord(r, D, true), x0 = coord(r, W, true), x1 = coord(r, W, true), y = coord(r, H, true);
      gm_frame_hline(c, z, x0, x1, y);
      p.hline(z, x0, x1, y);
      return "hline";
    }
    case 4: {
      const int z = coord(r, D, true), x = coord(r, W, true), y0 = coord(r, H, true), y1 = coord(r, H, true);
      gm_frame_vline(c, z, x, y0, y1);
      p.vline(z, x, y0, y1);
      return "vline";
    }
    case 5: {
      const int z = coord(r, D, true), x0 = coord(r, W, true), y0 = coord(r, H, true), x1 = coord(r, W, true),
                y1 = coord(r, H, true);
      gm_frame_line(c, z, x0, y0, x1, y1);
      p.line(z, x0, y0, x1, y1);
      return "line";
    }
    case 6: {
      const int z = coord(r, D, true), x0 = coord(r, W, true), y0 = coord(r, H, true), x1 = coord(r, W, true),
                y1 = coord(r, H, true);
      gm_frame_rect(c, z, x0, y0, x1, y1);
      p.rect(z, x0, y0, x1, y1);
      return "rect";
    }
    case 7: {
      // fill_rect clips x and y but plots on any z, so z stays where plot is defined.
      const int z = coord(r, D, false), x0 = coord(r, W, true), y0 = coord(r, H, true), x1 = coord(r, W, true),
                y1 = coord(r, H, true);
      if (r.chance(0.2)) {
        gm_frame_fill_rect(c, z, x0, y0, x1, y1, 1.0f);
        p.fillRect(z, x0, y0, x1, y1);  // C++ default intensity 1
      } else {
        const float v = intensity(r);
        gm_frame_fill_rect(c, z, x0, y0, x1, y1, v);
        p.fillRect(z, x0, y0, x1, y1, v);
      }
      return "fill_rect";
    }
    case 8: {
      const int z = coord(r, D, true), cx = coord(r, W, false), cy = coord(r, H, false);
      const int rad = r.chance(0.9) ? r.irange(-3, 140) : r.irange(141, 3000);
      gm_frame_circle(c, z, cx, cy, rad);
      p.circle(z, cx, cy, rad);
      return "circle";
    }
    case 9: {
      const int z = coord(r, D, true);
      const float cx = r.frange(-40.0f, W + 40.0f), cy = r.frange(-40.0f, H + 40.0f);
      static const float kRad[] = {0.0f, -1.0f, 1e-6f, 0.5f, 1.0f};
      const float rad = r.chance(0.1) ? r.pick(kRad) : r.frange(0.0f, 70.0f);
      if (r.chance(0.2)) {
        gm_frame_disc(c, z, cx, cy, rad, 1.0f);
        p.disc(z, cx, cy, rad);
      } else {
        const float v = intensity(r);
        gm_frame_disc(c, z, cx, cy, rad, v);
        p.disc(z, cx, cy, rad, v);
      }
      return "disc";
    }
    case 10: {
      const gm::Vec3 w = world(r, 1.4f);
      static const float kR[] = {0.0f, -1.0f, 1e-4f, 0.35f, 0.4f, 0.6f};
      const float rxy = r.chance(0.1) ? r.pick(kR) : r.frange(0.0f, 50.0f);
      const float rz = r.chance(0.15) ? r.pick(kR) * GM_LAYER_PITCH_PX : r.frange(0.0f, 4.0f * GM_LAYER_PITCH_PX);
      if (r.chance(0.2)) {
        gm_frame_sphere(c, diff::to_c(w), rxy, rz, 1.0f);
        p.sphere(w, rxy, rz);
      } else {
        const float v = intensity(r);
        gm_frame_sphere(c, diff::to_c(w), rxy, rz, v);
        p.sphere(w, rxy, rz, v);
      }
      return "sphere";
    }
    case 11: {
      const gm::Vec3 w = world(r, 1.3f);
      const float v = r.chance(0.3) ? 1.0f : intensity(r);
      gm_frame_point(c, diff::to_c(w), v);
      p.point(w, v);
      return "point";
    }
    case 12: {
      const gm::Vec3 a = world(r, 1.5f), b = r.chance(0.1) ? a : world(r, 1.5f);
      if (r.chance(0.2)) {
        gm_frame_line3(c, diff::to_c(a), diff::to_c(b), 1.0f);
        p.line3(a, b);
      } else {
        const float v = intensity(r);
        gm_frame_line3(c, diff::to_c(a), diff::to_c(b), v);
        p.line3(a, b, v);
      }
      return "line3";
    }
    case 13: {
      const std::string s = random_text(r);
      const int z = coord(r, D, false), x = coord(r, W, false), y = coord(r, H, false);
      EXPECT(ex, gm_frame_text(c, z, x, y, s.c_str()) == p.text(z, x, y, s.c_str()));
      return "text";
    }
    case 14: {
      const int x0 = coord(r, W, true), y0 = coord(r, H, true), x1 = coord(r, W, true), y1 = coord(r, H, true);
      gm_frame_clear_box(c, x0, y0, x1, y1);
      p.clearBox(x0, y0, x1, y1);
      return "clear_box";
    }
    case 15: {
      const int dx = r.chance(0.8) ? r.irange(-3, 3) : coord(r, W, true);
      const int dy = r.chance(0.8) ? r.irange(-3, 3) : coord(r, H, true);
      gm_frame_shift(c, dx, dy);
      p.shift(dx, dy);
      return "shift";
    }
    case 16: {
      static FramePair o;
      randomize(o, r, r.irange(0, 30));
      gm_frame_or(c, o.c.get());
      p.orWith(*o.p);
      return "or";
    }
    case 17: {
      if (r.chance(0.5)) {
        gm_frame_clear(c);
        p.clear();
      } else {
        gm_frame_fill(c);
        p.fill();
      }
      return "clear/fill";
    }
    default: {
      for (int i = 0; i < 50; ++i) {
        const int x = coord(r, W, true), y = coord(r, H, true), z = coord(r, D, true);
        EXPECT(ex, gm_frame_get(c, x, y, z) == p.get(x, y, z));
      }
      return "get";
    }
  }
}

}  // namespace

TEST(frame_random_primitive_sequences) {
  Expect ex;
  diff::Rand r(61);
  FramePair f;
  int n = 0;
  for (int seq = 0; seq < 600; ++seq) {
    const int start = seq % 3;
    if (start == 0) {
      gm_frame_clear(f.c.get());
      f.p->clear();
    } else if (start == 1) {
      gm_frame_fill(f.c.get());
      f.p->fill();
    } else {
      randomize(f, r, r.irange(1, 60));
    }
    EXPECT(ex, f.same("start"));
    for (int k = 0; k < 80; ++k, ++n) {
      const char* what = random_op(f, r, ex);
      char label[64];
      std::snprintf(label, sizeof(label), "op %d (%s)", n, what);
      EXPECT(ex, f.same(label));
      if (k % 8 == 0) {
        EXPECT(ex, gm_frame_count_lit(f.c.get()) == f.p->countLit());
        for (int z = 0; z < GM_D; ++z) EXPECT(ex, gm_frame_count_lit_layer(f.c.get(), z) == f.p->countLitLayer(z));
      }
    }
  }
}

TEST(frame_plot_every_voxel_and_intensity) {
  Expect ex;
  FramePair f;
  // Every voxel at intensities on and between all 64 Bayer levels.
  for (int level = -1; level <= 65; ++level) {
    const float v = static_cast<float>(level) / 64.0f;
    const float vs[] = {v, std::nextafter(v, -1.0f), std::nextafter(v, 2.0f), v + 0.5f / 64.0f};
    for (float it : vs) {
      gm_frame_clear(f.c.get());
      f.p->clear();
      for (int z = 0; z < GM_D; ++z)
        for (int y = 0; y < GM_H; ++y)
          for (int x = 0; x < GM_W; ++x) {
            gm_frame_plot(f.c.get(), x, y, z, it);
            f.p->plot(x, y, z, it);
          }
      EXPECT(ex, f.same("plot level"));
    }
  }
}

TEST(frame_glyphs_and_text) {
  Expect ex;
  for (int i = 0; i < 256; ++i) {
    const char ch = static_cast<char>(i);
    EXPECT(ex, gm_glyph3x5(ch) == gm::VoxelFrame::glyph3x5(ch));
  }
  diff::Rand r(62);
  FramePair f;
  // Each character alone at many positions, including partly off the panel.
  for (int i = 1; i < 256; ++i) {
    const char s[2] = {static_cast<char>(i), '\0'};
    for (int k = 0; k < 12; ++k) {
      const int z = r.irange(-1, GM_D), x = r.irange(-4, GM_W + 1), y = r.irange(-6, GM_H + 1);
      EXPECT(ex, gm_frame_text(f.c.get(), z, x, y, s) == f.p->text(z, x, y, s));
      EXPECT(ex, f.same("glyph"));
    }
  }
  // All printable characters in one string, wrapping off the right edge.
  std::string all;
  for (int i = 32; i < 127; ++i) all.push_back(static_cast<char>(i));
  for (int y = -5; y <= GM_H; y += 3) {
    EXPECT(ex, gm_frame_text(f.c.get(), y & 7, -2, y, all.c_str()) == f.p->text(y & 7, -2, y, all.c_str()));
    EXPECT(ex, f.same("all chars"));
  }
  EXPECT(ex, gm_frame_text(f.c.get(), 0, 5, 5, "") == f.p->text(0, 5, 5, ""));
}

TEST(frame_shift_every_offset) {
  Expect ex;
  diff::Rand r(63);
  FramePair src, f;
  randomize(src, r, 45);
  int n = 0;
  for (int dy = -GM_H - 2; dy <= GM_H + 2; ++dy)
    for (int dx = -GM_W - 2; dx <= GM_W + 2; ++dx, ++n) {
      std::memcpy(f.c->bits, src.c->bits, sizeof(f.c->bits));
      std::memcpy(f.p->data(), src.p->data(), GM_BYTES_PER_FRAME);
      gm_frame_shift(f.c.get(), dx, dy);
      f.p->shift(dx, dy);
      char label[48];
      std::snprintf(label, sizeof(label), "shift %d,%d", dx, dy);
      EXPECT(ex, f.same(label));
    }
  const int extremes[] = {INT_MIN, INT_MIN + 1, -1000, 1000, INT_MAX};
  for (int dx : extremes)
    for (int dy : {0, 1, -1, INT_MIN, INT_MAX}) {
      std::memcpy(f.c->bits, src.c->bits, sizeof(f.c->bits));
      std::memcpy(f.p->data(), src.p->data(), GM_BYTES_PER_FRAME);
      gm_frame_shift(f.c.get(), dx, dy);
      f.p->shift(dx, dy);
      EXPECT(ex, f.same("extreme shift"));
      gm_frame_shift(f.c.get(), dy, dx);
      f.p->shift(dy, dx);
      EXPECT(ex, f.same("extreme shift"));
    }
  // Chains of small shifts on the same frame (the HUD orbit).
  randomize(f, r, 30);
  for (int i = 0; i < 2000; ++i) {
    const int dx = r.irange(-2, 2), dy = r.irange(-2, 2);
    gm_frame_shift(f.c.get(), dx, dy);
    f.p->shift(dx, dy);
    EXPECT(ex, f.same("shift chain"));
    if (i % 100 == 0) randomize(f, r, 30);
  }
}

TEST(frame_crossfade) {
  Expect ex;
  diff::Rand r(64);
  FramePair a, b, f;
  std::vector<float> ts = {-1.0f,      -0.0f,      0.0f,        1e-9f, 1.0f,       2.0f,
                           diff::kNaN, diff::kInf, -diff::kInf, 0.5f,  0.99999994f};
  for (int k = 0; k <= 64; ++k) {
    const float t = (static_cast<float>(k) + 0.5f) / 64.0f;
    ts.push_back(t);
    ts.push_back(std::nextafter(t, -1.0f));
    ts.push_back(std::nextafter(t, 2.0f));
    ts.push_back(static_cast<float>(k) / 64.0f);
  }
  for (int i = 0; i < 400; ++i) ts.push_back(r.frange(-0.1f, 1.1f));
  for (size_t i = 0; i < ts.size(); ++i) {
    if (i % 16 == 0) {
      randomize(a, r, r.irange(0, 100));
      randomize(b, r, r.irange(0, 100));
    }
    const float t = ts[i];
    randomize(f, r, 50);  // the old content must not leak through
    gm_frame_crossfade(f.c.get(), a.c.get(), b.c.get(), t);
    f.p->crossfade(*a.p, *b.p, t);
    char label[48];
    std::snprintf(label, sizeof(label), "crossfade t=%.9g", static_cast<double>(t));
    EXPECT(ex, f.same(label));
    // In place: the output is one of the inputs.
    FramePair a2, b2;
    std::memcpy(a2.c->bits, a.c->bits, sizeof(a2.c->bits));
    std::memcpy(a2.p->data(), a.p->data(), GM_BYTES_PER_FRAME);
    std::memcpy(b2.c->bits, b.c->bits, sizeof(b2.c->bits));
    std::memcpy(b2.p->data(), b.p->data(), GM_BYTES_PER_FRAME);
    gm_frame_crossfade(a2.c.get(), a2.c.get(), b2.c.get(), t);
    a2.p->crossfade(*a2.p, *b2.p, t);
    EXPECT(ex, a2.same("crossfade into a"));
    gm_frame_crossfade(b2.c.get(), a.c.get(), b2.c.get(), t);
    b2.p->crossfade(*a.p, *b2.p, t);
    EXPECT(ex, b2.same("crossfade into b"));
  }
}

TEST(frame_or_and_counts) {
  Expect ex;
  diff::Rand r(65);
  FramePair f, o;
  for (int i = 0; i < 300; ++i) {
    randomize(f, r, r.irange(0, 100));
    randomize(o, r, r.irange(0, 100));
    EXPECT(ex, gm_frame_count_lit(f.c.get()) == f.p->countLit());
    gm_frame_or(f.c.get(), o.c.get());
    f.p->orWith(*o.p);
    EXPECT(ex, f.same("or"));
    EXPECT(ex, gm_frame_count_lit(f.c.get()) == f.p->countLit());
    for (int z = 0; z < GM_D; ++z) EXPECT(ex, gm_frame_count_lit_layer(f.c.get(), z) == f.p->countLitLayer(z));
  }
  gm_frame_fill(f.c.get());
  f.p->fill();
  EXPECT(ex, f.same("fill") && gm_frame_count_lit(f.c.get()) == GM_W * GM_H * GM_D);
  gm_frame_clear(f.c.get());
  f.p->clear();
  EXPECT(ex, f.same("clear") && gm_frame_count_lit(f.c.get()) == 0);
  // Raw access points at the same layout.
  EXPECT(ex, gm_frame_layer(f.c.get(), 3) - gm_frame_data(f.c.get()) == f.p->layer(3) - f.p->data());
}

TEST(frame_exact_boundaries) {
  Expect ex;
  FramePair f;
  // Discs whose rim passes exactly through pixel centres (integer and
  // half-integer centres, radii from Pythagorean triples), so `<= r*r` is
  // decided on equality.
  const float radii[] = {1.0f, 2.0f, 2.5f, 5.0f, 10.0f, 13.0f, 25.0f, 0.5f, 1.5f};
  for (float rad : radii)
    for (float cx = 10.0f; cx <= 11.0f; cx += 0.5f)
      for (float cy = 20.0f; cy <= 21.0f; cy += 0.5f)
        for (float it : {1.0f, 0.5f}) {
          gm_frame_clear(f.c.get());
          f.p->clear();
          for (int z = 0; z < GM_D; ++z) {
            gm_frame_disc(f.c.get(), z, cx + 8.0f * static_cast<float>(z), cy, rad, it);
            f.p->disc(z, cx + 8.0f * static_cast<float>(z), cy, rad, it);
          }
          EXPECT(ex, f.same("exact disc"));
        }
  // Spheres centred exactly on a layer plane or between two, with rz exact
  // multiples of the pitch, so dz hits +-1 and the radius test exactly.
  for (int k = 0; k < GM_D; ++k)
    for (float half : {0.0f, 0.5f})
      for (float m : {0.6f, 1.0f, 1.5f, 2.0f, 3.0f}) {
        const gm::Vec3 c{-20.0f + 5.0f * static_cast<float>(k), 3.0f,
                         (static_cast<float>(k) + half - (GM_D - 1) * 0.5f) * GM_LAYER_PITCH_PX};
        for (float rxy : {0.35f, 1.0f, 6.0f}) {
          gm_frame_clear(f.c.get());
          f.p->clear();
          gm_frame_sphere(f.c.get(), diff::to_c(c), rxy, m * GM_LAYER_PITCH_PX, 1.0f);
          f.p->sphere(c, rxy, m * GM_LAYER_PITCH_PX);
          EXPECT(ex, f.same("exact sphere"));
        }
      }
  // Points and 3D lines on exact half-voxel positions (round-half-up) and
  // with integer spans (ceil of an exact integer).
  diff::Rand r(66);
  gm_frame_clear(f.c.get());
  f.p->clear();
  for (int i = 0; i < 3000; ++i) {
    const gm_vec3_t v = gm_voxel_to_world(0.5f * static_cast<float>(r.irange(-4, 2 * GM_W + 4)),
                                          0.5f * static_cast<float>(r.irange(-4, 2 * GM_H + 4)),
                                          0.5f * static_cast<float>(r.irange(-2, 2 * GM_D + 2)));
    gm_frame_point(f.c.get(), v, 1.0f);
    f.p->point(diff::to_cpp(v));
    const gm_vec3_t w =
        gm_voxel_to_world(static_cast<float>(r.irange(-2, GM_W + 2)), static_cast<float>(r.irange(-2, GM_H + 2)),
                          static_cast<float>(r.irange(0, GM_D - 1)));
    gm_frame_line3(f.c.get(), v, w, 0.75f);
    f.p->line3(diff::to_cpp(v), diff::to_cpp(w), 0.75f);
    if (i % 100 == 0) EXPECT(ex, f.same("exact points/lines"));
  }
  EXPECT(ex, f.same("exact points/lines"));
}
