// Host simulator: drives the exact same core code as the firmware through
// scripted g-force scenarios (via a simulated, rotated, noisy accelerometer and
// the real GForceEstimator), and records the resulting voxel frames.
//
//   gmeter_sim --effect all --scenario drive --fps 30 --out frames.bin
//   gmeter_sim --effect slosh --scenario corner --ascii
//   gmeter_sim --bench
//
// Output format (frames.bin, little-endian):
//   "GMV1" u16 w, u16 h, u16 d, f32 pitch, u16 fps, u16 clipCount
//   per clip: u8 len, effect name, u8 len, scenario name, u32 frames,
//             frames x { f32 lat, f32 vert, f32 lon, u8 frame[w*h/8*d] }
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gmeter/gmeter.hpp"

using namespace gm;

namespace {

struct Options {
  std::vector<std::string> effects{"all"};
  std::vector<std::string> scenarios{"drive"};
  float fps = 30.0f;
  float seconds = -1.0f;  // -1 = scenario duration
  std::string out;
  bool ascii = false;
  bool bench = false;
  std::vector<std::pair<std::string, float>> params;
  float mountYawDeg = 15.0f, mountTiltDeg = 25.0f;
  float sensorHz = 200.0f;
  float cutoffHz = 4.0f;
  float noiseG = 0.02f;
  bool readout = false;
  bool allowFade = false;
};

void usage() {
  std::printf(
      "gmeter_sim [options]\n"
      "  --effect NAME[,NAME]   ball|slosh|particles|smoke|tunnel|sprite|all (default all)\n"
      "  --scenario NAME[,..]   idle|launch|brake|slalom|corner|bump|drive|all (default drive)\n"
      "  --fps N                frame rate (default 30)\n"
      "  --seconds S            override clip length\n"
      "  --out FILE             write frames.bin for the web viewer\n"
      "  --ascii                print an ASCII preview every 0.5 s\n"
      "  --bench                time each effect's update+render per frame\n"
      "  --param key=value      set an effect parameter (applies to every effect that has it)\n"
      "  --mount YAW,TILT       simulated sensor mounting error in degrees (default 15,25)\n"
      "  --noise G              sensor noise amplitude in g (default 0.02)\n"
      "  --cutoff HZ            estimator low-pass cutoff (default 4)\n"
      "  --readout              overlay the numeric g readout on every effect\n");
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t comma = s.find(',', start);
    if (comma == std::string::npos) comma = s.size();
    if (comma > start) out.push_back(s.substr(start, comma - start));
    start = comma + 1;
  }
  return out;
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
      return argv[++i];
    };
    if (a == "--effect") o.effects = split(need("--effect"));
    else if (a == "--scenario") o.scenarios = split(need("--scenario"));
    else if (a == "--fps") o.fps = std::strtof(need("--fps"), nullptr);
    else if (a == "--seconds") o.seconds = std::strtof(need("--seconds"), nullptr);
    else if (a == "--out") o.out = need("--out");
    else if (a == "--ascii") o.ascii = true;
    else if (a == "--bench") o.bench = true;
    else if (a == "--readout") o.readout = true;
    else if (a == "--noise") o.noiseG = std::strtof(need("--noise"), nullptr);
    else if (a == "--cutoff") o.cutoffHz = std::strtof(need("--cutoff"), nullptr);
    else if (a == "--mount") {
      std::vector<std::string> v = split(need("--mount"));
      if (v.size() == 2) { o.mountYawDeg = std::strtof(v[0].c_str(), nullptr); o.mountTiltDeg = std::strtof(v[1].c_str(), nullptr); }
    } else if (a == "--param") {
      std::string kv = need("--param");
      size_t eq = kv.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "bad --param %s\n", kv.c_str()); return false; }
      o.params.emplace_back(kv.substr(0, eq), std::strtof(kv.c_str() + eq + 1, nullptr));
    } else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return false; }
  }
  return true;
}

struct EffectSet {
  GBallEffect ball;
  SloshEffect slosh;
  ParticleEffect particles;
  SmokeEffect smoke;
  TunnelEffect tunnel;
  SpriteEffect sprite{demoSprite()};
  Effect* all[6] = {&ball, &slosh, &particles, &smoke, &tunnel, &sprite};
  Effect* byName(const std::string& n) {
    for (Effect* e : all) if (n == e->name()) return e;
    return nullptr;
  }
};

void asciiPreview(const VoxelFrame& f, const GForce& g, float t) {
  // Front view (all layers OR-ed, 2x2 -> 1 char) and top view (x vs layer).
  std::printf("t=%5.2fs  lat %+.2f  vert %+.2f  lon %+.2f   lit=%d\n", t, g.lateralG(), g.verticalG(), g.longitudinalG(), f.countLit());
  for (int y = 0; y < kH; y += 2) {
    std::string row;
    for (int x = 0; x < kW; x += 2) {
      int n = 0;
      for (int z = 0; z < kD; ++z)
        n += f.get(x, y, z) + f.get(x + 1, y, z) + f.get(x, y + 1, z) + f.get(x + 1, y + 1, z);
      row += n == 0 ? ' ' : (n < 3 ? '.' : (n < 8 ? 'o' : '#'));
    }
    std::printf("|%s|\n", row.c_str());
  }
  std::printf("top view (viewer at top, windshield at bottom):\n");
  for (int z = 0; z < kD; ++z) {
    std::string row;
    for (int x = 0; x < kW; x += 2) {
      int n = f.countLitLayer(z) ? 0 : 0;
      for (int y = 0; y < kH; ++y) n += f.get(x, y, z) + f.get(x + 1, y, z);
      row += n == 0 ? ' ' : (n < 3 ? '.' : (n < 10 ? 'o' : '#'));
    }
    std::printf("|%s| L%d\n", row.c_str(), z);
  }
  std::printf("\n");
}

void putU16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void putU32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void putF32(std::vector<uint8_t>& b, float v) { uint32_t u; std::memcpy(&u, &v, 4); putU32(b, u); }
void putStr(std::vector<uint8_t>& b, const std::string& s) { b.push_back(static_cast<uint8_t>(s.size())); b.insert(b.end(), s.begin(), s.end()); }

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) return 2;

  EffectSet effects;
  std::vector<Effect*> chosen;
  for (const std::string& n : opt.effects) {
    if (n == "all") { chosen.assign(std::begin(effects.all), std::end(effects.all)); break; }
    Effect* e = effects.byName(n);
    if (!e) { std::fprintf(stderr, "unknown effect %s\n", n.c_str()); return 2; }
    chosen.push_back(e);
  }
  std::vector<const Scenario*> scenarios;
  for (const std::string& n : opt.scenarios) {
    if (n == "all") { scenarios.clear(); for (int i = 0; i < Scenario::presetCount(); ++i) scenarios.push_back(&Scenario::preset(i)); break; }
    const Scenario* s = Scenario::find(n.c_str());
    if (!s) { std::fprintf(stderr, "unknown scenario %s\n", n.c_str()); return 2; }
    scenarios.push_back(s);
  }
  for (Effect* e : chosen)
    for (auto& kv : opt.params) e->params().set(kv.first.c_str(), kv.second);

  if (opt.bench) {
    std::printf("effect      us/update  us/render  us/frame (host; ESP32 is roughly 20-40x slower)\n");
    const Scenario& drive = *Scenario::find("drive");
    for (Effect* e : chosen) {
      e->activate();
      EffectContext ctx; ctx.dt = 1.0f / 60.0f;
      VoxelFrame f;
      double upd = 0, ren = 0; int n = 0;
      for (float t = 0.0f; t < drive.duration(); t += ctx.dt, ++n) {
        ctx.g = drive.at(t); ctx.time = t; ++ctx.frame;
        auto t0 = std::chrono::steady_clock::now();
        e->update(ctx);
        auto t1 = std::chrono::steady_clock::now();
        f.clear(); e->render(f);
        auto t2 = std::chrono::steady_clock::now();
        upd += std::chrono::duration<double, std::micro>(t1 - t0).count();
        ren += std::chrono::duration<double, std::micro>(t2 - t1).count();
      }
      std::printf("%-10s %9.1f  %9.1f  %9.1f\n", e->name(), upd / n, ren / n, (upd + ren) / n);
      e->deactivate();
    }
    return 0;
  }

  // Simulated sensor mounting (yaw about up, tilt back) and calibration.
  const float yaw = opt.mountYawDeg * kPi / 180.0f, tilt = opt.mountTiltDeg * kPi / 180.0f;
  const Vec3 upS{0.0f, std::cos(tilt), std::sin(tilt)};
  const Vec3 fwdS{std::sin(yaw), 0.0f, std::cos(yaw)};
  const Calibration mount = GForceEstimator::makeCalibration(upS, fwdS);

  std::vector<uint8_t> blob;
  uint16_t clipCount = 0;
  blob.insert(blob.end(), {'G', 'M', 'V', '1'});
  putU16(blob, kW); putU16(blob, kH); putU16(blob, kD); putF32(blob, kLayerPitchPx);
  putU16(blob, static_cast<uint16_t>(opt.fps)); putU16(blob, 0);  // clip count patched later

  for (Effect* e : chosen) {
    for (const Scenario* sc : scenarios) {
      Rng rng(1234u);
      GForceEstimator est;
      est.setNominalForward(Vec3{0, 0, 1});  // sensor +Z nominally forward; the yaw error is calibrated away below
      est.setFilter(opt.cutoffHz, opt.sensorHz);
      est.beginRestCalibration();
      GForce rest;
      for (int i = 0; i < 200; ++i) est.feedRestSample(simulateRawSensor(rest, mount.sensorToCar, rng, opt.noiseG));
      if (!est.finishRestCalibration()) { std::fprintf(stderr, "calibration failed\n"); return 1; }
      // Settle the filter.
      for (int i = 0; i < 400; ++i) est.update(simulateRawSensor(rest, mount.sensorToCar, rng, opt.noiseG));

      Engine eng;
      eng.addEffect(e);
      eng.setShowModeName(false);
      eng.setReadout(opt.readout);
      // Pre-roll so liquids are settled at t = 0.
      for (int i = 0; i < 90; ++i) eng.step(est.current(), 1.0f / opt.fps);

      const float duration = opt.seconds > 0.0f ? opt.seconds : sc->duration();
      const int frames = static_cast<int>(duration * opt.fps);
      const int sensorPerFrame = std::max(1, static_cast<int>(opt.sensorHz / opt.fps));
      std::vector<uint8_t> clip;
      putStr(clip, e->name()); putStr(clip, sc->name()); putU32(clip, static_cast<uint32_t>(frames));
      float nextPreview = 0.0f;
      for (int fi = 0; fi < frames; ++fi) {
        const float t = fi / opt.fps;
        for (int s = 0; s < sensorPerFrame; ++s) {
          const GForce truth = sc->at(t + s / opt.sensorHz, nullptr);
          est.update(simulateRawSensor(truth, mount.sensorToCar, rng, opt.noiseG));
        }
        const GForce g = est.current();
        eng.step(g, 1.0f / opt.fps);
        const VoxelFrame& f = eng.frame();
        putF32(clip, g.lateralG()); putF32(clip, g.verticalG()); putF32(clip, g.longitudinalG());
        clip.insert(clip.end(), f.data(), f.data() + VoxelFrame::frameBytes());
        if (opt.ascii && t >= nextPreview) { std::printf("[%s / %s]\n", e->name(), sc->name()); asciiPreview(f, g, t); nextPreview += 0.5f; }
      }
      blob.insert(blob.end(), clip.begin(), clip.end());
      ++clipCount;
      std::fprintf(stderr, "clip %s/%s: %d frames\n", e->name(), sc->name(), frames);
    }
  }
  blob[4 + 2 + 2 + 2 + 4 + 2] = clipCount & 0xFF;
  blob[4 + 2 + 2 + 2 + 4 + 2 + 1] = clipCount >> 8;

  if (!opt.out.empty()) {
    FILE* fp = std::fopen(opt.out.c_str(), "wb");
    if (!fp) { std::perror("open"); return 1; }
    std::fwrite(blob.data(), 1, blob.size(), fp);
    std::fclose(fp);
    std::fprintf(stderr, "wrote %s (%zu bytes, %u clips)\n", opt.out.c_str(), blob.size(), clipCount);
  }
  return 0;
}
