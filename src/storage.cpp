#include "storage.hpp"

#include <Preferences.h>
#include <cstddef>
#include <cstring>

namespace {

constexpr uint32_t kMagic = 0x474D5452u;  // "GMTR"
constexpr uint16_t kVersion = 1;

struct Blob {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  Settings settings;
  float rotation[9];
  uint8_t calValid;
  uint8_t pad[3];
  uint32_t crc;
};

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

}  // namespace

namespace storage {

bool load(Settings& settings, gm::Calibration& calibration) {
  Preferences prefs;
  if (!prefs.begin("gmeter", true)) return false;
  Blob b{};
  const size_t n = prefs.getBytesLength("state");
  bool ok = n == sizeof(b) && prefs.getBytes("state", &b, sizeof(b)) == sizeof(b);
  prefs.end();
  if (!ok || b.magic != kMagic || b.version != kVersion) return false;
  if (crc32(reinterpret_cast<const uint8_t*>(&b), offsetof(Blob, crc)) != b.crc) return false;
  settings = b.settings;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) calibration.sensorToCar.m[r][c] = b.rotation[r * 3 + c];
  calibration.valid = b.calValid != 0;
  return true;
}

bool save(const Settings& settings, const gm::Calibration& calibration) {
  Blob b{};
  b.magic = kMagic;
  b.version = kVersion;
  b.settings = settings;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) b.rotation[r * 3 + c] = calibration.sensorToCar.m[r][c];
  b.calValid = calibration.valid ? 1 : 0;
  b.crc = crc32(reinterpret_cast<const uint8_t*>(&b), offsetof(Blob, crc));
  Preferences prefs;
  if (!prefs.begin("gmeter", false)) return false;
  const bool ok = prefs.putBytes("state", &b, sizeof(b)) == sizeof(b);
  prefs.end();
  return ok;
}

bool clear() {
  Preferences prefs;
  if (!prefs.begin("gmeter", false)) return false;
  const bool ok = prefs.clear();
  prefs.end();
  return ok;
}

gm::Vec3 forwardAxisVector(uint8_t index) {
  switch (index) {
    case 0: return {1, 0, 0};
    case 1: return {-1, 0, 0};
    case 2: return {0, 1, 0};
    case 3: return {0, -1, 0};
    case 5: return {0, 0, -1};
    default: return {0, 0, 1};
  }
}

const char* forwardAxisName(uint8_t index) {
  static const char* names[] = {"+x", "-x", "+y", "-y", "+z", "-z"};
  return index < 6 ? names[index] : "?";
}

int parseForwardAxis(const char* text) {
  if (!text) return -1;
  int sign = 1;
  if (*text == '+') ++text;
  else if (*text == '-') { sign = -1; ++text; }
  switch (*text) {
    case 'x': case 'X': return sign > 0 ? 0 : 1;
    case 'y': case 'Y': return sign > 0 ? 2 : 3;
    case 'z': case 'Z': return sign > 0 ? 4 : 5;
    default: return -1;
  }
}

}  // namespace storage
