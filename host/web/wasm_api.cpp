// C API over the gmeter core for the browser (Gmeter Studio). Compiled to
// WebAssembly by tools/build_studio.py; every function here is exported.
//
// Strings are returned as pointers to NUL-terminated UTF-8 in linear memory;
// arrays as pointers to static storage that stays valid until the next call
// of the same function.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "gmeter/gmeter.hpp"

using namespace gm;

#define GM_EXPORT(name) extern "C" __attribute__((export_name(#name)))

namespace {

GBallEffect gBall;
SloshEffect gSlosh;
ParticleEffect gParticles;
SmokeEffect gSmoke;
TunnelEffect gTunnel;
SpriteEffect gSprite(demoSprite());
AnimationEffect gAnim(demoAnimation());
Engine gEngine;
bool gInit = false;

// Optional sensor realism: true g -> rotated noisy accelerometer -> estimator.
bool gSensor = false;
float gNoiseG = 0.02f;
GForceEstimator gEstimator;
Rng gRng(2024u);
constexpr float kSensorHz = 200.0f;
float gSensorAccum = 0.0f;

float gApplied[3];
float gSample[3];
int32_t gInfo[4 + 4 * kAnimStateCount];

// User animation storage (owned here; the asset points into it).
uint8_t* gUserBits = nullptr;
size_t gUserCap = 0;
AnimationAsset gUserAsset;
char gUserName[32] = "studio";

void ensureInit() {
  if (gInit) return;
  gEngine.addEffect(&gBall);
  gEngine.addEffect(&gSlosh);
  gEngine.addEffect(&gParticles);
  gEngine.addEffect(&gSmoke);
  gEngine.addEffect(&gTunnel);
  gEngine.addEffect(&gSprite);
  gEngine.addEffect(&gAnim);
  gEngine.setShowModeName(false);
  gEngine.setCrossfadeSeconds(0.3f);
  gEstimator.setFilter(4.0f, kSensorHz);
  gInit = true;
}

// which < 0: the active effect; which >= 0: effect number `which`.
const ParamTable* table(int which) {
  ensureInit();
  Effect* e = which < 0 ? gEngine.current() : gEngine.effect(which);
  return e ? &e->params() : nullptr;
}

}  // namespace

// ---- geometry ------------------------------------------------------------------------------
GM_EXPORT(gm_init) void gm_init() { ensureInit(); }
GM_EXPORT(gm_width) int gm_width() { return kW; }
GM_EXPORT(gm_height) int gm_height() { return kH; }
GM_EXPORT(gm_layers) int gm_layers() { return kD; }
GM_EXPORT(gm_layer_pitch) float gm_layer_pitch() { return kLayerPitchPx; }
GM_EXPORT(gm_frame_bytes) int gm_frame_bytes() { return kBytesPerFrame; }

// ---- effects -------------------------------------------------------------------------------
GM_EXPORT(gm_effect_count) int gm_effect_count() { ensureInit(); return gEngine.effectCount(); }
GM_EXPORT(gm_effect_name) const char* gm_effect_name(int i) {
  ensureInit();
  Effect* e = gEngine.effect(i);
  return e ? e->name() : "";
}
GM_EXPORT(gm_effect_index) int gm_effect_index() { ensureInit(); return gEngine.currentIndex(); }
GM_EXPORT(gm_set_effect) void gm_set_effect(int i, int crossfade) { ensureInit(); gEngine.setEffect(i, crossfade != 0); }
GM_EXPORT(gm_reset_effect) void gm_reset_effect() {
  ensureInit();
  if (gEngine.current()) gEngine.current()->reset();
}
GM_EXPORT(gm_set_readout) void gm_set_readout(int on) { ensureInit(); gEngine.setReadout(on != 0); }

// ---- stepping ------------------------------------------------------------------------------
// lat/vert/lon: true dynamic g in the car frame (x right, y up, z forward).
// Returns a pointer to the rendered frame (gm_frame_bytes() bytes, SSD130x page layout per layer).
GM_EXPORT(gm_step) const uint8_t* gm_step(float lat, float vert, float lon, float dt) {
  ensureInit();
  GForce truth;
  truth.proper = GForce::kRestProper() + Vec3{lat, vert, lon};
  GForce g = truth;
  if (gSensor) {
    gSensorAccum += dt;
    const float period = 1.0f / kSensorHz;
    int n = 0;
    while (gSensorAccum >= period && n < 64) {
      gSensorAccum -= period;
      gEstimator.update(simulateRawSensor(truth, Mat3::identity(), gRng, gNoiseG));
      ++n;
    }
    g = gEstimator.current();
  }
  gApplied[0] = g.lateralG();
  gApplied[1] = g.verticalG();
  gApplied[2] = g.longitudinalG();
  gEngine.step(g, dt);
  return gEngine.frame().data();
}
GM_EXPORT(gm_frame) const uint8_t* gm_frame() { ensureInit(); return gEngine.frame().data(); }
// The g actually fed to the effect (after the simulated sensor and filter): lat, vert, lon.
GM_EXPORT(gm_applied_g) const float* gm_applied_g() { return gApplied; }
GM_EXPORT(gm_set_sensor) void gm_set_sensor(int enabled, float noiseG, float cutoffHz) {
  ensureInit();
  const bool was = gSensor;
  gSensor = enabled != 0;
  gNoiseG = clampf(noiseG, 0.0f, 1.0f);
  gEstimator.setFilter(clampf(cutoffHz, 0.5f, 40.0f), kSensorHz);
  if (gSensor && !was) { gEstimator.reset(); gSensorAccum = 0.0f; }
}

// ---- effect parameters ------------------------------------------------------------------------
// `which` selects the table: -1 = active effect, otherwise the effect index.
GM_EXPORT(gm_param_count) int gm_param_count(int which) { const ParamTable* p = table(which); return p ? p->count() : 0; }
GM_EXPORT(gm_param_name) const char* gm_param_name(int which, int i) { const ParamTable* p = table(which); return p && i >= 0 && i < p->count() ? p->at(i).name : ""; }
GM_EXPORT(gm_param_help) const char* gm_param_help(int which, int i) { const ParamTable* p = table(which); return p && i >= 0 && i < p->count() ? p->at(i).help : ""; }
GM_EXPORT(gm_param_min) float gm_param_min(int which, int i) { const ParamTable* p = table(which); return p && i >= 0 && i < p->count() ? p->at(i).min : 0.0f; }
GM_EXPORT(gm_param_max) float gm_param_max(int which, int i) { const ParamTable* p = table(which); return p && i >= 0 && i < p->count() ? p->at(i).max : 0.0f; }
GM_EXPORT(gm_param_get) float gm_param_get(int which, int i) { const ParamTable* p = table(which); return p && i >= 0 && i < p->count() ? *p->at(i).value : 0.0f; }
GM_EXPORT(gm_param_set) float gm_param_set(int which, int i, float v) {
  const ParamTable* p = table(which);
  if (!p || i < 0 || i >= p->count()) return 0.0f;
  const Param& q = p->at(i);
  *q.value = clampf(v, q.min, q.max);
  return *q.value;
}

// ---- scenarios --------------------------------------------------------------------------------
GM_EXPORT(gm_scenario_count) int gm_scenario_count() { return Scenario::presetCount(); }
GM_EXPORT(gm_scenario_name) const char* gm_scenario_name(int i) { return Scenario::preset(i).name(); }
GM_EXPORT(gm_scenario_duration) float gm_scenario_duration(int i) { return Scenario::preset(i).duration(); }
// Dynamic g (lat, vert, lon) of preset i at time t, without noise.
GM_EXPORT(gm_scenario_sample) const float* gm_scenario_sample(int i, float t) {
  const Vec3 d = Scenario::preset(i).dynamicAt(t);
  gSample[0] = d.x; gSample[1] = d.y; gSample[2] = d.z;
  return gSample;
}

// ---- animation -----------------------------------------------------------------------------------
GM_EXPORT(gm_anim_state_count) int gm_anim_state_count() { return kAnimStateCount; }
GM_EXPORT(gm_anim_state_name) const char* gm_anim_state_name(int s) { return animStateName(s); }
GM_EXPORT(gm_anim_state) int gm_anim_state() { return gAnim.state(); }
GM_EXPORT(gm_anim_frame) int gm_anim_frame() { return gAnim.currentFrame(); }
GM_EXPORT(gm_anim_mirrored) int gm_anim_mirrored() { return gAnim.mirrored() ? 1 : 0; }
GM_EXPORT(gm_anim_force_state) void gm_anim_force_state(int s) { gAnim.forceState(s); }
GM_EXPORT(gm_anim_effect_index) int gm_anim_effect_index() { ensureInit(); return gEngine.findEffect("anim"); }

// Describe an asset: [w, h, depth, frames, then per state: start, count, fps*1000, loop].
static const int32_t* describe(const AnimationAsset& a) {
  gInfo[0] = a.width; gInfo[1] = a.height; gInfo[2] = a.depth; gInfo[3] = a.frames;
  for (int s = 0; s < kAnimStateCount; ++s) {
    gInfo[4 + s * 4 + 0] = a.clips[s].start;
    gInfo[4 + s * 4 + 1] = a.clips[s].count;
    gInfo[4 + s * 4 + 2] = static_cast<int32_t>(a.clips[s].fps * 1000.0f + 0.5f);
    gInfo[4 + s * 4 + 3] = a.clips[s].loop;
  }
  return gInfo;
}
GM_EXPORT(gm_demo_anim_info) const int32_t* gm_demo_anim_info() { return describe(demoAnimation()); }
GM_EXPORT(gm_demo_anim_bits) const uint8_t* gm_demo_anim_bits() { return demoAnimation().bits; }

// Reserve space for user artwork; JS writes packed bits there, then calls gm_anim_load.
GM_EXPORT(gm_anim_alloc) uint8_t* gm_anim_alloc(int bytes) {
  if (bytes <= 0) return nullptr;
  if (static_cast<size_t>(bytes) > gUserCap) {
    // The effect may still point at the old buffer; detach it before freeing.
    gAnim.setAsset(demoAnimation(), false, false);
    std::free(gUserBits);
    gUserBits = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(bytes)));
    gUserCap = gUserBits ? static_cast<size_t>(bytes) : 0;
  }
  return gUserBits;
}
// Clip fields are set with gm_anim_set_clip before or after; mapping comes from params.
GM_EXPORT(gm_anim_set_clip) void gm_anim_set_clip(int state, int start, int count, float fps, int loop) {
  if (state < 0 || state >= kAnimStateCount) return;
  AnimClip& c = gUserAsset.clips[state];
  c.start = static_cast<uint16_t>(clampi(start, 0, 65535));
  c.count = static_cast<uint16_t>(clampi(count, 0, 65535));
  c.fps = clampf(fps, 0.5f, 120.0f);
  c.loop = loop ? 1 : 0;
}
// Returns 1 on success. Keeps the current tunables (mapping) so the UI owns them.
GM_EXPORT(gm_anim_load) int gm_anim_load(int w, int h, int depth, int frames) {
  ensureInit();
  if (w <= 0 || h <= 0 || depth <= 0 || frames <= 0 || w > kW || h > kH || depth > kD) return 0;
  const size_t need = static_cast<size_t>((w + 7) / 8) * h * depth * frames;
  if (!gUserBits || need > gUserCap) return 0;
  gUserAsset.name = gUserName;
  gUserAsset.width = static_cast<uint16_t>(w);
  gUserAsset.height = static_cast<uint16_t>(h);
  gUserAsset.depth = static_cast<uint16_t>(depth);
  gUserAsset.frames = static_cast<uint16_t>(frames);
  gUserAsset.bits = gUserBits;
  gAnim.setAsset(gUserAsset, false, false);
  return 1;
}
GM_EXPORT(gm_anim_use_demo) void gm_anim_use_demo() { gAnim.setAsset(demoAnimation(), true); }

// ---- memory helpers for JS ---------------------------------------------------------------------
GM_EXPORT(gm_malloc) void* gm_malloc(int bytes) { return bytes > 0 ? std::malloc(static_cast<size_t>(bytes)) : nullptr; }
GM_EXPORT(gm_free) void gm_free(void* p) { std::free(p); }
