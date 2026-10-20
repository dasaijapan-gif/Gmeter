// Differential tests of the Japanese text renderer and glyph data
// (gm_jp_text.c / gm_jp_glyphs.c vs jp_text.cpp / jp_glyphs.cpp).
#include <cstdio>
#include <string>
#include <vector>

#include "diff_util.hpp"

using diff::Expect;
using diff::FramePair;

namespace {

std::string utf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return s;
}

struct Fonts {
  const gm_jp_font_t* c;
  const gm::JpFont* p;
  const char* name;
};
const Fonts kFonts[] = {{&gm_jp_large, &gm::kJpLarge, "large"}, {&gm_jp_small, &gm::kJpSmall, "small"}};

const float kIntensities[] = {1.0f, 0.5f, 0.25f, 0.9f, 0.0f, -0.0f, -1.0f, -0.5f, 1.5f, diff::kNaN};

// Pieces that random strings are made of.
std::vector<std::string> pieces() {
  std::vector<std::string> v;
  for (const Fonts& f : kFonts)
    for (int i = 0; i < f.p->count; ++i) v.push_back(utf8(f.p->glyphs[i].codepoint));
  for (int c = 1; c < 0x80; ++c) v.push_back(std::string(1, static_cast<char>(c)));
  const uint32_t missing[] = {0x30FC, 0x30FC, 0x30FC, 0x6F22, 0x3042, 0x00E9, 0x20AC, 0x1F600, 0xFFFD, 0x10FFFF, 0x7FF};
  for (uint32_t cp : missing) v.push_back(utf8(cp));
  // Invalid UTF-8: stray continuation bytes, bad leads, truncated sequences.
  const char* invalid[] = {"\x80", "\xBF", "\xC0", "\xFF", "\xF8\x80", "\xE6\x97", "\xE6", "\xF0\x9F\x98", "\xC3("};
  for (const char* s : invalid) v.push_back(s);
  return v;
}

std::string random_string(diff::Rand& r, const std::vector<std::string>& p) {
  std::string s;
  const int n = r.irange(0, 10);
  for (int i = 0; i < n; ++i) s += p[static_cast<size_t>(r.irange(0, static_cast<int>(p.size()) - 1))];
  return s;
}

void start_frame(FramePair& f, diff::Rand& r, float intensity) {
  // Knock-out text needs lit pixels to clear.
  if (intensity < 0.0f || r.chance(0.2)) {
    gm_frame_fill(f.c.get());
    f.p->fill();
  } else {
    gm_frame_clear(f.c.get());
    f.p->clear();
  }
}

}  // namespace

TEST(jp_glyph_tables_identical) {
  Expect ex;
  for (const Fonts& f : kFonts) {
    EXPECT(ex, f.c->height == f.p->height && f.c->count == f.p->count);
    for (int i = 0; i < f.p->count && i < f.c->count; ++i) {
      const gm_jp_glyph_t& a = f.c->glyphs[i];
      const gm::JpGlyph& b = f.p->glyphs[i];
      EXPECT(ex, a.codepoint == b.codepoint && a.width == b.width);
      const size_t bytes = static_cast<size_t>((b.width + 7) / 8) * f.p->height;
      EXPECT(ex, std::memcmp(a.bits, b.bits, bytes) == 0);
      if (i > 0) EXPECT(ex, f.c->glyphs[i - 1].codepoint < a.codepoint);  // sorted for the binary search
    }
  }
  // Every code point of the Basic Multilingual Plane and beyond.
  for (const Fonts& f : kFonts)
    for (uint32_t cp = 0; cp < 0x110000; cp += (cp < 0x10000 ? 1 : 97)) {
      const gm_jp_glyph_t* a = gm_jp_find_glyph(f.c, cp);
      const gm::JpGlyph* b = gm::findGlyph(*f.p, cp);
      if (a == nullptr || b == nullptr)
        EXPECT(ex, a == nullptr && b == nullptr);
      else
        EXPECT(ex, a - f.c->glyphs == b - f.p->glyphs);
    }
  // An empty font finds nothing.
  const gm_jp_font_t empty_c = {14, 0, gm_jp_small.glyphs};
  const gm::JpFont empty_p = {14, 0, gm::kJpSmall.glyphs};
  EXPECT(ex, gm_jp_find_glyph(&empty_c, 0x6A5F) == nullptr && gm::findGlyph(empty_p, 0x6A5F) == nullptr);
}

TEST(jp_every_glyph_drawn) {
  Expect ex;
  diff::Rand r(71);
  FramePair f;
  for (const Fonts& fo : kFonts)
    for (const Fonts& src : kFonts)  // also glyphs the other font lacks
      for (int i = 0; i < src.p->count; ++i) {
        const std::string s = utf8(src.p->glyphs[i].codepoint);
        for (int k = 0; k < 6; ++k) {
          const float it = k == 0 ? 1.0f : r.pick(kIntensities);
          const int z = r.irange(-1, GM_D), x = r.irange(-25, GM_W + 2), y = r.irange(-25, GM_H + 2);
          const int sp = r.irange(-2, 3);
          start_frame(f, r, it);
          if (k == 1) {  // the C++ defaults: spacing 1, intensity 1
            EXPECT(ex, gm_jp_draw(f.c.get(), fo.c, z, x, y, s.c_str(), 1, 1.0f) ==
                           gm::drawJp(*f.p, *fo.p, z, x, y, s.c_str()));
          } else {
            EXPECT(ex, gm_jp_draw(f.c.get(), fo.c, z, x, y, s.c_str(), sp, it) ==
                           gm::drawJp(*f.p, *fo.p, z, x, y, s.c_str(), sp, it));
          }
          EXPECT(ex, f.same("glyph"));
          start_frame(f, r, it);
          EXPECT(ex, gm_jp_draw_vertical(f.c.get(), fo.c, z, x, y, s.c_str(), sp, it) ==
                         gm::drawJpVertical(*f.p, *fo.p, z, x, y, s.c_str(), sp, it));
          EXPECT(ex, f.same("glyph vertical"));
          EXPECT(ex, gm_jp_measure(fo.c, s.c_str(), sp) == gm::measureJp(*fo.p, s.c_str(), sp));
        }
      }
}

TEST(jp_ascii_fallback_every_char) {
  Expect ex;
  diff::Rand r(72);
  FramePair f;
  for (const Fonts& fo : kFonts)
    for (int c = 1; c < 0x80; ++c) {
      const char s[2] = {static_cast<char>(c), '\0'};
      for (float it : kIntensities) {
        const int z = r.irange(0, GM_D - 1), x = r.irange(-3, GM_W), y = r.irange(-10, GM_H);
        start_frame(f, r, it);
        EXPECT(ex, gm_jp_draw(f.c.get(), fo.c, z, x, y, s, 1, it) == gm::drawJp(*f.p, *fo.p, z, x, y, s, 1, it));
        EXPECT(ex, f.same("ascii"));
        start_frame(f, r, it);
        EXPECT(ex, gm_jp_draw_vertical(f.c.get(), fo.c, z, x, y, s, 1, it) ==
                       gm::drawJpVertical(*f.p, *fo.p, z, x, y, s, 1, it));
        EXPECT(ex, f.same("ascii vertical"));
      }
    }
}

TEST(jp_mixed_strings) {
  Expect ex;
  diff::Rand r(73);
  FramePair f;
  const std::vector<std::string> p = pieces();
  // The strings the effects actually draw, plus random mixes.
  std::vector<std::string> strings = {"待機",
                                      "加速",
                                      "制動",
                                      "左旋回",
                                      "右旋回",
                                      "衝撃",
                                      "危険",
                                      "警告",
                                      "横G",
                                      "縦G",
                                      "パターン青",
                                      "フィールド",
                                      "第3号機",
                                      "ーー",
                                      "同期率 99.9%",
                                      "",
                                      " ",
                                      "使徒接近",
                                      "限界",
                                      "ATフィールド全開",
                                      "\xE6\x97",
                                      "a\x80"
                                      "b"};
  for (int i = 0; i < 4000; ++i) strings.push_back(random_string(r, p));
  int n = 0;
  for (const std::string& s : strings)
    for (const Fonts& fo : kFonts) {
      ++n;
      const float it = r.pick(kIntensities);
      const int sp = r.irange(-3, 4);
      const int z = r.irange(-1, GM_D), x = r.irange(-60, GM_W + 2), y = r.irange(-40, GM_H + 2);
      char label[48];
      std::snprintf(label, sizeof(label), "string %d (%s)", n, fo.name);
      start_frame(f, r, it);
      EXPECT(ex, gm_jp_draw(f.c.get(), fo.c, z, x, y, s.c_str(), sp, it) ==
                     gm::drawJp(*f.p, *fo.p, z, x, y, s.c_str(), sp, it));
      EXPECT(ex, f.same(label));
      start_frame(f, r, it);
      EXPECT(ex, gm_jp_draw_vertical(f.c.get(), fo.c, z, x, y, s.c_str(), sp, it) ==
                     gm::drawJpVertical(*f.p, *fo.p, z, x, y, s.c_str(), sp, it));
      EXPECT(ex, f.same(label));
      EXPECT(ex, gm_jp_measure(fo.c, s.c_str(), sp) == gm::measureJp(*fo.p, s.c_str(), sp));
      EXPECT(ex, gm_jp_measure(fo.c, s.c_str(), 1) == gm::measureJp(*fo.p, s.c_str()));
    }
  // Several strings layered on one frame without clearing.
  gm_frame_clear(f.c.get());
  f.p->clear();
  for (int i = 0; i < 300; ++i) {
    const std::string& s = strings[static_cast<size_t>(r.irange(0, static_cast<int>(strings.size()) - 1))];
    const Fonts& fo = kFonts[i & 1];
    const float it = r.pick(kIntensities);
    const int z = r.irange(0, GM_D - 1), x = r.irange(-20, GM_W), y = r.irange(-20, GM_H);
    if (i % 3 == 0) {
      gm_jp_draw_vertical(f.c.get(), fo.c, z, x, y, s.c_str(), 1, it);
      gm::drawJpVertical(*f.p, *fo.p, z, x, y, s.c_str(), 1, it);
    } else {
      gm_jp_draw(f.c.get(), fo.c, z, x, y, s.c_str(), 1, it);
      gm::drawJp(*f.p, *fo.p, z, x, y, s.c_str(), 1, it);
    }
    EXPECT(ex, f.same("layered"));
  }
}
