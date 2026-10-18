// Volumetric G-force meter firmware.
//
// Tasks:
//   imuTask     (core 0, 200 Hz)  reads the accelerometer, runs the estimator,
//                                 publishes the filtered g-force; handles calibration captures.
//   renderTask  (core 1, 60 fps)  steps the active effect and renders a voxel frame.
//   displayTask (core 0)          streams finished frames to the 8 panels over SPI.
//   loop()      (core 1, idle)    serial CLI, mode button, periodic housekeeping.
#include <Arduino.h>
#include <esp_timer.h>

#include <cstdlib>
#include <cstring>

#include "board_config.hpp"
#include "display_ssd1309.hpp"
#include "gmeter/gmeter.hpp"
#include "imu_mpu6050.hpp"
#include "serial_cli.hpp"
#include "storage.hpp"

// Artwork exported from Gmeter Studio (host/web): save it as
// src/user_animation.hpp and it replaces the built-in demo animation.
#if __has_include("user_animation.hpp")
#include "user_animation.hpp"
#define GM_USER_ANIMATION kUserAnimation
#else
#define GM_USER_ANIMATION gm::demoAnimation()
#endif

using namespace gm;

namespace {

// ---- hardware & core objects ------------------------------------------------------------
PanelStack display;
Mpu6050 imu;
GForceEstimator estimator;
Engine engine;
GBallEffect ballEffect;
SloshEffect sloshEffect;
ParticleEffect particleEffect;
SmokeEffect smokeEffect;
TunnelEffect tunnelEffect;
SpriteEffect spriteEffect(demoSprite());
AnimationEffect animEffect(GM_USER_ANIMATION);

Settings settings;
Calibration calibration;

// ---- shared state (protected by gMux) -----------------------------------------------------------
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
GForce imuG;                 // latest filtered g from the IMU
Vec3 imuRaw;                 // latest raw sensor sample (sensor frame)
float imuTempC = 0.0f;
Vec3 serialDynamicG;         // injected over serial (lat, vert, lon)
enum class Source : uint8_t { Imu, Serial, Demo };
Source source = Source::Demo;
const Scenario* demoScenario = &Scenario::preset(Scenario::presetCount() - 1);  // "drive"

enum class CalState : uint8_t { Idle, Rest, Brake, Done, Failed };
volatile CalState calState = CalState::Idle;
Vec3 calAccum;
int calCount = 0;
uint32_t calDeadlineMs = 0;
const char* calMessage = "";

// ---- frame hand-off: triple buffer ---------------------------------------------------------------
VoxelFrame frames[3];
int frameReady = -1, frameDisplaying = -1;
SemaphoreHandle_t frameSem;
volatile bool wantReinit = false;
volatile int testPanel = -2;  // -2 none, -1 clear test, >= 0 panel index

// ---- stats --------------------------------------------------------------------------------------------
float fpsEma = 0.0f;
uint32_t stepUs = 0, transferUs = 0, droppedFrames = 0;
uint32_t bootMs = 0;

// ---- helpers -------------------------------------------------------------------------------------------
GForce currentG(float t) {
  GForce g;
  portENTER_CRITICAL(&gMux);
  const Source src = source;
  const GForce fromImu = imuG;
  const Vec3 injected = serialDynamicG;
  portEXIT_CRITICAL(&gMux);
  switch (src) {
    case Source::Imu: g = fromImu; break;
    case Source::Serial: g.proper = GForce::kRestProper() + injected; break;
    case Source::Demo: {
      const float dur = demoScenario->duration();
      g = demoScenario->at(dur > 0.0f ? fmodf(t, dur + 1.0f) : 0.0f, nullptr);
      break;
    }
  }
  return g;
}

void applySettings() {
  engine.setReadout(settings.readout != 0);
  engine.setAutoCycle(settings.autoCycleS);
  estimator.setFilter(settings.filterHz, static_cast<float>(board::kImuSampleHz));
  estimator.setNominalForward(storage::forwardAxisVector(settings.forwardAxis));
  estimator.setAutoZero(settings.autoZero != 0);
  if (calibration.valid) estimator.setCalibration(calibration);
}

// ---- tasks ----------------------------------------------------------------------------------------------
void imuTask(void*) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / board::kImuSampleHz);
  for (;;) {
    vTaskDelayUntil(&last, period);
    if (!imu.present()) continue;
    Vec3 raw;
    float temp;
    if (!imu.read(raw, &temp)) {
      if (imu.consecutiveErrors() >= 10) imu.reinit();
      continue;
    }
    const GForce g = estimator.update(raw);
    portENTER_CRITICAL(&gMux);
    imuG = g;
    imuRaw = raw;
    imuTempC = temp;
    portEXIT_CRITICAL(&gMux);

    // Calibration captures run here so they see every sample.
    const CalState cs = calState;
    if (cs == CalState::Rest || cs == CalState::Brake) {
      calAccum += raw;
      ++calCount;
      if (millis() >= calDeadlineMs) {
        bool ok = false;
        if (cs == CalState::Rest) {
          estimator.beginRestCalibration();
          const Vec3 mean = calAccum / static_cast<float>(calCount);
          for (int i = 0; i < 16; ++i) estimator.feedRestSample(mean);
          ok = estimator.finishRestCalibration();
          calMessage = ok ? "rest calibration ok (save to keep)" : "rest calibration failed: not ~1 g or degenerate axes";
        } else {
          ok = estimator.calibrateForwardFromBraking(calAccum / static_cast<float>(calCount));
          calMessage = ok ? "forward axis refined from braking (save to keep)" : "braking capture too weak (< 0.05 g) or no rest calibration";
        }
        if (ok) calibration = estimator.calibration();
        calState = ok ? CalState::Done : CalState::Failed;
      }
    }
  }
}

void renderTask(void*) {
  int64_t lastUs = esp_timer_get_time();
  const int64_t periodUs = 1000000 / GM_TARGET_FPS;
  for (;;) {
    const int64_t now = esp_timer_get_time();
    const float dt = static_cast<float>(now - lastUs) * 1e-6f;
    lastUs = now;

    const GForce g = currentG(static_cast<float>(now - static_cast<int64_t>(bootMs) * 1000) * 1e-6f);
    engine.step(g, dt);

    // Pick a buffer that is neither being displayed nor already queued.
    portENTER_CRITICAL(&gMux);
    int target = 0;
    while (target == frameReady || target == frameDisplaying) ++target;
    portEXIT_CRITICAL(&gMux);
    memcpy(frames[target].data(), engine.frame().data(), VoxelFrame::frameBytes());
    portENTER_CRITICAL(&gMux);
    if (frameReady >= 0) ++droppedFrames;  // display did not keep up; newest frame wins
    frameReady = target;
    portEXIT_CRITICAL(&gMux);
    xSemaphoreGive(frameSem);

    const int64_t spent = esp_timer_get_time() - now;
    stepUs = static_cast<uint32_t>(spent);
    const float inst = 1.0f / (dt > 1e-4f ? dt : 1e-4f);
    fpsEma = fpsEma == 0.0f ? inst : fpsEma + 0.05f * (inst - fpsEma);
    // Always sleep at least one tick: a plain yield would never let the
    // lower-priority loop()/CLI task or the idle task run when rendering is
    // over budget (and the idle task feeds the watchdog).
    const int64_t sleepUs = periodUs - spent;
    const uint32_t sleepMs = sleepUs > 1000 ? static_cast<uint32_t>(sleepUs / 1000) : 1u;
    vTaskDelay(pdMS_TO_TICKS(sleepMs));
  }
}

void displayTask(void*) {
  for (;;) {
    if (xSemaphoreTake(frameSem, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    if (wantReinit) { display.reinit(); wantReinit = false; }
    const int tp = testPanel;
    if (tp >= 0) {
      display.testPattern(tp);
      // Hold the test pattern; drop frames until the test is cleared.
      portENTER_CRITICAL(&gMux);
      frameReady = -1;
      portEXIT_CRITICAL(&gMux);
      continue;
    }
    portENTER_CRITICAL(&gMux);
    frameDisplaying = frameReady;
    frameReady = -1;
    portEXIT_CRITICAL(&gMux);
    if (frameDisplaying < 0) continue;
    display.writeFrame(frames[frameDisplaying]);
    transferUs = display.lastTransferUs();
    portENTER_CRITICAL(&gMux);
    frameDisplaying = -1;
    portEXIT_CRITICAL(&gMux);
  }
}

// ---- CLI binding ----------------------------------------------------------------------------------------
class App : public AppControl {
 public:
  Engine& engine() override { return ::engine; }

  void printStatus(Print& out) override {
    portENTER_CRITICAL(&gMux);
    const GForce g = imuG;
    const Vec3 raw = imuRaw;
    const float temp = imuTempC;
    const Source src = source;
    portEXIT_CRITICAL(&gMux);
    const GForce shown = currentG(static_cast<float>(millis() - bootMs) * 1e-3f);
    out.printf("uptime %lu s  mode %s  source %s%s\n", static_cast<unsigned long>((millis() - bootMs) / 1000),
               ::engine.current() ? ::engine.current()->name() : "-",
               src == Source::Imu ? "imu" : (src == Source::Serial ? "serial" : "demo:"),
               src == Source::Demo ? demoScenario->name() : "");
    out.printf("render %.1f fps  step %lu us  spi %lu us  dropped %lu  heap %lu  smoke %s\n",
               static_cast<double>(fpsEma), static_cast<unsigned long>(stepUs), static_cast<unsigned long>(transferUs),
               static_cast<unsigned long>(droppedFrames), static_cast<unsigned long>(ESP.getFreeHeap()),
               smokeEffect.allocated() ? "allocated" : "freed");
    out.printf("shown g: lat %+.3f vert %+.3f lon %+.3f (|h| %.3f)\n", static_cast<double>(shown.lateralG()),
               static_cast<double>(shown.verticalG()), static_cast<double>(shown.longitudinalG()),
               static_cast<double>(shown.horizontalG()));
    if (imu.present()) {
      out.printf("imu: who 0x%02X%s  raw (%+.3f %+.3f %+.3f) g  %.1f C  errors %lu\n", imu.whoAmI(),
                 imu.clone() ? " (clone)" : "", static_cast<double>(raw.x), static_cast<double>(raw.y),
                 static_cast<double>(raw.z), static_cast<double>(temp), static_cast<unsigned long>(imu.errorCount()));
      out.printf("imu car frame: lat %+.3f vert %+.3f lon %+.3f  filter %.1f Hz  autozero %s\n",
                 static_cast<double>(g.lateralG()), static_cast<double>(g.verticalG()),
                 static_cast<double>(g.longitudinalG()), static_cast<double>(settings.filterHz),
                 settings.autoZero ? "on" : "off");
    } else {
      out.println("imu: not detected (check wiring / address); running demo source");
    }
    out.printf("calibration: %s, forward axis hint %s", calibration.valid ? "valid" : "NONE (run: cal rest)",
               storage::forwardAxisName(settings.forwardAxis));
    if (calState == CalState::Rest || calState == CalState::Brake) out.print("  [capturing]");
    out.println();
    if (calMessage[0]) out.printf("last calibration: %s\n", calMessage);
    if (::engine.current() == &animEffect) {
      const AnimationAsset& a = animEffect.asset();
      out.printf("animation '%s' %ux%u x%u slices, %u frames: state %s, frame %d%s\n", a.name, a.width, a.height, a.depth,
                 a.frames, animStateName(animEffect.state()), animEffect.currentFrame(), animEffect.mirrored() ? " (mirrored)" : "");
    }
    out.printf("contrast ramp %u..%u  cycle %.0f s  readout %s  panel reinit every %u min\n", settings.contrastFront,
               settings.contrastBack, static_cast<double>(settings.autoCycleS), settings.readout ? "on" : "off",
               settings.reinitMinutes);
  }

  void printScenarios(Print& out) override {
    out.println("demo scenarios:");
    for (int i = 0; i < Scenario::presetCount(); ++i)
      out.printf("  %s (%.0f s)\n", Scenario::preset(i).name(), static_cast<double>(Scenario::preset(i).duration()));
  }

  bool setMode(const char* nameOrIndex) override {
    if (!nameOrIndex) return false;
    int idx = ::engine.findEffect(nameOrIndex);
    if (idx < 0 && nameOrIndex[0] >= '0' && nameOrIndex[0] <= '9') idx = atoi(nameOrIndex);
    if (idx < 0 || idx >= ::engine.effectCount()) return false;
    ::engine.setEffect(idx);
    settings.mode = static_cast<uint8_t>(idx);
    return true;
  }
  void nextMode() override { ::engine.nextEffect(); settings.mode = static_cast<uint8_t>(::engine.currentIndex()); }

  bool setSource(const char* name, const char* arg) override {
    Source s;
    const Scenario* sc = demoScenario;
    if (!strcmp(name, "imu")) { if (!imu.present()) { Serial.println("no imu detected"); return false; } s = Source::Imu; }
    else if (!strcmp(name, "serial")) s = Source::Serial;
    else if (!strcmp(name, "demo")) {
      s = Source::Demo;
      if (arg) { sc = Scenario::find(arg); if (!sc) { Serial.println("unknown scenario (list)"); return false; } }
    } else return false;
    portENTER_CRITICAL(&gMux);
    source = s;
    demoScenario = sc;
    serialDynamicG = Vec3{};
    portEXIT_CRITICAL(&gMux);
    return true;
  }

  void injectG(float lat, float vert, float lon) override {
    portENTER_CRITICAL(&gMux);
    serialDynamicG = Vec3{lat, vert, lon};
    if (source != Source::Serial) source = Source::Serial;
    portEXIT_CRITICAL(&gMux);
  }

  bool startCalibration(const char* which) override {
    if (!imu.present()) return false;
    if (calState == CalState::Rest || calState == CalState::Brake) return false;
    calAccum = Vec3{};
    calCount = 0;
    if (!strcmp(which, "rest")) { calDeadlineMs = millis() + 2000; calState = CalState::Rest; return true; }
    if (!strcmp(which, "brake")) {
      if (!calibration.valid) { calMessage = "run cal rest first"; return false; }
      calDeadlineMs = millis() + 1500; calState = CalState::Brake; return true;
    }
    return false;
  }

  bool setForwardAxis(const char* axis) override {
    const int idx = storage::parseForwardAxis(axis);
    if (idx < 0) return false;
    settings.forwardAxis = static_cast<uint8_t>(idx);
    estimator.setNominalForward(storage::forwardAxisVector(settings.forwardAxis));
    return true;
  }

  bool saveState() override { return storage::save(settings, calibration); }
  bool clearState() override { return storage::clear(); }
  void setFilterHz(float hz) override {
    settings.filterHz = clampf(hz, 0.5f, 30.0f);
    estimator.setFilter(settings.filterHz, static_cast<float>(board::kImuSampleHz));
  }
  void setContrast(int panel, int value) override {
    const uint8_t v = static_cast<uint8_t>(clampi(value, 0, 255));
    if (panel < 0) { settings.contrastFront = settings.contrastBack = v; display.setContrastRamp(v, v); }
    else display.setContrast(panel, v);
  }
  void setContrastRamp(int front, int back) override {
    settings.contrastFront = static_cast<uint8_t>(clampi(front, 0, 255));
    settings.contrastBack = static_cast<uint8_t>(clampi(back, 0, 255));
    display.setContrastRamp(settings.contrastFront, settings.contrastBack);
  }
  void setAutoCycle(float s) override { settings.autoCycleS = clampf(s, 0.0f, 3600.0f); ::engine.setAutoCycle(settings.autoCycleS); }
  void setReadout(bool on) override { settings.readout = on; ::engine.setReadout(on); }
  void setInvert(bool on) override { settings.invert = on; display.setInverted(on); }
  void setAutoZero(bool on) override { settings.autoZero = on; estimator.setAutoZero(on); }
  void testPattern(int panel) override { testPanel = panel < 0 ? -2 : panel; if (panel < 0) wantReinit = false; }
  void reinitPanels() override { wantReinit = true; }
  bool pinAnimState(const char* name) override {
    if (!name || !strcmp(name, "auto")) { animEffect.forceState(-1); return true; }
    for (int i = 0; i < kAnimStateCount; ++i)
      if (!strcmp(name, animStateName(i))) {
        animEffect.forceState(i);
        setMode("anim");
        return true;
      }
    return false;
  }
  void reboot() override { ESP.restart(); }
};

App app;
SerialCli cli(app);

// ---- button -----------------------------------------------------------------------------------------------------
void pollButton() {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  static bool longFired = false;
  const bool down = digitalRead(board::kButton) == LOW;
  const uint32_t now = millis();
  if (down && !wasDown) { downSince = now; longFired = false; }
  if (down && !longFired && now - downSince > 2000) {
    longFired = true;
    if (app.startCalibration("rest")) Serial.println("button: rest calibration started (hold still 2 s)");
  }
  if (!down && wasDown && !longFired && now - downSince > 40) {
    app.nextMode();
    Serial.printf("button: mode %s\n", engine.current()->name());
  }
  wasDown = down;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  bootMs = millis();
  Serial.println();
  Serial.printf("gmeter: %dx%dx%d voxels, panel model %d, spi %d Hz, target %d fps\n", kW, kH, kD, GM_PANEL_MODEL,
                GM_SPI_HZ, GM_TARGET_FPS);

  const bool restored = storage::load(settings, calibration);
  Serial.println(restored ? "settings restored from NVS" : "no saved settings; using defaults");

  pinMode(board::kButton, INPUT_PULLUP);

  display.begin();
  display.setContrastRamp(settings.contrastFront, settings.contrastBack);
  if (settings.invert) display.setInverted(true);

  engine.addEffect(&ballEffect);
  engine.addEffect(&sloshEffect);
  engine.addEffect(&particleEffect);
  engine.addEffect(&smokeEffect);
  engine.addEffect(&tunnelEffect);
  engine.addEffect(&spriteEffect);
  engine.addEffect(&animEffect);
  engine.setEffect(settings.mode < engine.effectCount() ? settings.mode : 0, false);
  applySettings();

  if (imu.begin(board::kI2cSda, board::kI2cScl, board::kImuAddress)) {
    Serial.printf("imu: MPU-6050 found (WHO_AM_I 0x%02X%s)\n", imu.whoAmI(), imu.clone() ? ", clone" : "");
    source = Source::Imu;
    if (!calibration.valid) Serial.println("imu: no calibration yet. Park level, then type: cal rest  (then: save)");
  } else {
    Serial.printf("imu: not found (WHO_AM_I 0x%02X); showing demo scenario '%s'. Type: help\n", imu.whoAmI(),
                  demoScenario->name());
    source = Source::Demo;
  }

  frameSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, nullptr, 1);
  Serial.print("> ");
}

void loop() {
  cli.poll(Serial);
  pollButton();

  static uint32_t lastReinitMs = millis();
  if (settings.reinitMinutes > 0 && millis() - lastReinitMs > settings.reinitMinutes * 60000UL) {
    lastReinitMs = millis();
    wantReinit = true;
  }
  const CalState cs = calState;
  if (cs == CalState::Done || cs == CalState::Failed) {
    Serial.println(calMessage);
    calState = CalState::Idle;
  }
  delay(5);
}
