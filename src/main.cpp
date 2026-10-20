// Volumetric G-force meter firmware.
//
// Tasks:
//   imuTask     (core 0, 200 Hz)  reads the accelerometer, runs the estimator,
//                                 publishes the filtered g-force; handles calibration
//                                 captures; retries a lost sensor once a second.
//   renderTask  (core 1, 60 fps)  steps the active effect and renders a voxel frame.
//   displayTask (core 0)          streams finished frames to the 8 panels over SPI.
//   loop()      (core 1, idle)    serial CLI, mode button, periodic housekeeping.
//
// Concurrency: the g vector, source and frame-buffer indices are shared under
// the gMux spinlock. The Engine (and its effects) is guarded by engineMtx:
// renderTask holds it while stepping, and the CLI/button hold it while they
// change modes or parameters. The estimator is guarded by estMtx in the same
// way between imuTask and the CLI.
#include <Arduino.h>
#include <esp_timer.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

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

// Single-effect build (the HUD edition): -DGM_FIXED_EFFECT=hud in platformio.ini.
// The firmware then always shows that effect: it ignores the saved mode and
// auto-cycle, and the BOOT button and the mode/cycle/anim commands no longer
// switch away from it.
#ifdef GM_FIXED_EFFECT
#define GM_STR_(x) #x
#define GM_STR(x) GM_STR_(x)
static const char* const kFixedEffect = GM_STR(GM_FIXED_EFFECT);
#else
static const char* const kFixedEffect = nullptr;
#endif

namespace {

// ---- hardware & core objects ------------------------------------------------------------
// These are built at the top of setup(), not during static initialisation.
// Their constructors do floating-point work, and on the ESP32-S3 (Arduino core
// 2.0.x) the FPU is only enabled once FreeRTOS runs: a float instruction in a
// global constructor panics with "Coprocessor exception" before setup() starts.
// Each lives in raw static storage; the names below are references to it.
template <typename T>
class Deferred {
 public:
  template <typename... Args>
  void build(Args&&... args) { new (storage_) T(std::forward<Args>(args)...); }
  T& operator*() { return *reinterpret_cast<T*>(storage_); }

 private:
  alignas(T) unsigned char storage_[sizeof(T)];
};

Deferred<PanelStack> displayBox;
Deferred<Mpu6050> imuBox;
Deferred<GForceEstimator> estimatorBox;
Deferred<Engine> engineBox;
Deferred<GBallEffect> ballBox;
Deferred<SloshEffect> sloshBox;
Deferred<ParticleEffect> particleBox;
Deferred<SmokeEffect> smokeBox;
Deferred<TunnelEffect> tunnelBox;
Deferred<SpriteEffect> spriteBox;
Deferred<AnimationEffect> animBox;
Deferred<HudEffect> hudBox;
Deferred<AtFieldEffect> atFieldBox;
Deferred<SyncEffect> syncBox;

PanelStack& display = *displayBox;
Mpu6050& imu = *imuBox;
GForceEstimator& estimator = *estimatorBox;
Engine& engine = *engineBox;
GBallEffect& ballEffect = *ballBox;
SloshEffect& sloshEffect = *sloshBox;
ParticleEffect& particleEffect = *particleBox;
SmokeEffect& smokeEffect = *smokeBox;
TunnelEffect& tunnelEffect = *tunnelBox;
SpriteEffect& spriteEffect = *spriteBox;
AnimationEffect& animEffect = *animBox;
HudEffect& hudEffect = *hudBox;
AtFieldEffect& atFieldEffect = *atFieldBox;
SyncEffect& syncEffect = *syncBox;

void buildObjects() {
  displayBox.build();
  imuBox.build();
  estimatorBox.build();
  engineBox.build();
  ballBox.build();
  sloshBox.build();
  particleBox.build();
  smokeBox.build();
  tunnelBox.build();
  spriteBox.build(demoSprite());
  animBox.build(GM_USER_ANIMATION);
  hudBox.build();
  atFieldBox.build();
  syncBox.build();
}

Settings settings;
Calibration calibration;

SemaphoreHandle_t engineMtx;  // Engine + effects
SemaphoreHandle_t estMtx;     // GForceEstimator

struct Lock {
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }
  SemaphoreHandle_t m_;
};

// ---- shared state (protected by gMux) -----------------------------------------------------------
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
GForce imuG;                 // latest filtered g from the IMU (rest while the sensor is lost)
Vec3 imuRaw;                 // latest raw sensor sample (sensor frame)
float imuTempC = 0.0f;
Vec3 serialDynamicG;         // injected over serial (lat, vert, lon)
enum class Source : uint8_t { Imu, Serial, Demo };
Source source = Source::Demo;
bool demoBecauseNoImu = false;  // switch to the IMU automatically when it shows up
bool imuLive = false;           // the IMU is delivering samples (false after it is lost)
const Scenario* demoScenario = &Scenario::preset(Scenario::presetCount() - 1);  // "drive"

enum class CalState : uint8_t { Idle, Rest, Brake, Done, Failed };
volatile CalState calState = CalState::Idle;
Vec3 calAccum;
int calCount = 0;
uint32_t calDeadlineMs = 0;
const char* calMessage = "";
volatile bool imuEvent = false;  // imuTask -> loop(): print imuEventText
const char* imuEventText = "";

// ---- frame hand-off: triple buffer ---------------------------------------------------------------
// Allocated in setup() on the heap: the ESP32's static DRAM segment is only
// about 124 KB, while the heap can also use the DRAM above it.
VoxelFrame* frames = nullptr;
int frameReady = -1, frameDisplaying = -1;
SemaphoreHandle_t frameSem;
volatile bool wantReinit = false;   // hard reset (CLI)
volatile bool wantRefresh = false;  // soft re-send (periodic)
volatile int testPanel = -2;        // -2 none, >= 0 panel index

// ---- stats --------------------------------------------------------------------------------------------
float fpsEma = 0.0f;
uint32_t stepUs = 0, transferUs = 0, droppedFrames = 0;
int64_t bootUs = 0;

// ---- helpers -------------------------------------------------------------------------------------------
// `label` (optional) receives the tag shown on the panels while the g is not
// live sensor data: DEMO, SERIAL or NO SENSOR; nullptr when it is live.
GForce currentG(int64_t usSinceBoot, const char** label = nullptr) {
  GForce g;
  portENTER_CRITICAL(&gMux);
  const Source src = source;
  const bool live = imuLive;
  const GForce fromImu = imuG;
  const Vec3 injected = serialDynamicG;
  const Scenario* sc = demoScenario;
  portEXIT_CRITICAL(&gMux);
  if (label) *label = src == Source::Imu ? (live ? nullptr : "NO SENSOR") : (src == Source::Serial ? "SERIAL" : "DEMO");
  switch (src) {
    case Source::Imu: g = fromImu; break;
    case Source::Serial: g.proper = GForce::kRestProper() + injected; break;
    case Source::Demo: {
      // Wrap in integer microseconds so the demo stays smooth after days of uptime.
      const float dur = sc->duration();
      const int64_t period = static_cast<int64_t>((dur + 1.0f) * 1e6f);
      g = sc->at(static_cast<float>(usSinceBoot % period) * 1e-6f, nullptr);
      break;
    }
  }
  return g;
}

void applySettings() {
  engine.setReadout(settings.readout != 0);
  engine.setAutoCycle(settings.autoCycleS);
  estimator.setFilter(settings.filterHz, static_cast<float>(board::kImuSampleHz));
  estimator.setAutoZero(settings.autoZero != 0);
  if (calibration.valid) {
    estimator.setCalibration(calibration);
    // Keep the calibrated forward axis (possibly refined by `cal brake`) as the
    // hint, so a later `cal rest` does not throw that refinement away.
    estimator.setNominalForward(calibration.sensorToCar.row(2));
  } else {
    estimator.setNominalForward(storage::forwardAxisVector(settings.forwardAxis));
  }
}

void publishImu(const GForce& g, const Vec3& raw, float temp, bool live = true) {
  portENTER_CRITICAL(&gMux);
  imuLive = live;
  imuG = g;
  imuRaw = raw;
  imuTempC = temp;
  portEXIT_CRITICAL(&gMux);
}

// ---- tasks ----------------------------------------------------------------------------------------------
void imuTask(void*) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / board::kImuSampleHz);
  TickType_t nextRetry = 0;
  bool wasPresent = imu.present();
  for (;;) {
    vTaskDelayUntil(&last, period);
    if (!imu.present()) {
      if (wasPresent) {
        // Lost: show a neutral g instead of freezing on the last value.
        publishImu(GForce{}, Vec3{}, 0.0f, false);
        imuEventText = "imu: lost contact; showing 0 g and retrying every second";
        imuEvent = true;
        wasPresent = false;
      }
      const TickType_t now = xTaskGetTickCount();
      if (static_cast<int32_t>(now - nextRetry) >= 0) {
        nextRetry = now + pdMS_TO_TICKS(1000);
        if (imu.recover()) {
          {
            Lock l(estMtx);
            estimator.reset();
          }
          portENTER_CRITICAL(&gMux);
          if (demoBecauseNoImu && source == Source::Demo) source = Source::Imu;
          demoBecauseNoImu = false;
          portEXIT_CRITICAL(&gMux);
          imuEventText = "imu: sensor (re)connected";
          imuEvent = true;
          wasPresent = true;
        }
        last = xTaskGetTickCount();  // the retry took time; do not burst afterwards
      }
      continue;
    }
    Vec3 raw;
    float temp;
    if (!imu.read(raw, &temp)) continue;  // read() marks the sensor lost after repeated failures

    GForce g;
    {
      Lock l(estMtx);
      g = estimator.update(raw);

      // Calibration captures run here so they see every sample.
      const CalState cs = calState;
      if (cs == CalState::Rest || cs == CalState::Brake) {
        calAccum += raw;
        ++calCount;
        if (millis() >= calDeadlineMs) {
          bool ok = false;
          const Vec3 mean = calAccum / static_cast<float>(calCount);
          if (cs == CalState::Rest) {
            estimator.beginRestCalibration();
            for (int i = 0; i < 16; ++i) estimator.feedRestSample(mean);
            ok = estimator.finishRestCalibration();
            if (!ok) calMessage = "rest calibration failed: the average was not about 1 g (was the car moving?)";
            else if (!estimator.forwardTrusted())
              calMessage = "rest ok, but the forward axis is unknown (the forward hint points up or down). "
                           "Run cal brake while braking firmly in a straight line, or cal fwd <axis> then cal rest";
            else calMessage = "rest calibration ok (save to keep; cal brake refines forward)";
          } else {
            ok = estimator.calibrateForwardFromBraking(mean);
            calMessage = ok ? "forward axis set from braking (save to keep)"
                            : "braking capture too weak (needs more than 0.05 g): brake harder and try again";
          }
          if (ok) calibration = estimator.calibration();
          calState = ok ? CalState::Done : CalState::Failed;
        }
      }
    }
    publishImu(g, raw, temp);
  }
}

void renderTask(void*) {
  int64_t lastUs = esp_timer_get_time();
  const int64_t periodUs = 1000000 / GM_TARGET_FPS;
  for (;;) {
    const int64_t now = esp_timer_get_time();
    const float dt = static_cast<float>(now - lastUs) * 1e-6f;
    lastUs = now;

    const char* label = nullptr;
    const GForce g = currentG(now - bootUs, &label);

    // Pick a buffer that is neither being displayed nor already queued.
    portENTER_CRITICAL(&gMux);
    int target = 0;
    while (target == frameReady || target == frameDisplaying) ++target;
    portEXIT_CRITICAL(&gMux);
    {
      Lock l(engineMtx);
      engine.setStatusLabel(label);
      engine.step(g, dt);
      memcpy(frames[target].data(), engine.frame().data(), VoxelFrame::frameBytes());
    }
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
    if (wantReinit) { wantReinit = false; display.reinit(); }
    if (wantRefresh) { wantRefresh = false; display.refresh(); }
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
  void lockEngine() override { xSemaphoreTake(engineMtx, portMAX_DELAY); }
  void unlockEngine() override { xSemaphoreGive(engineMtx); }
  Engine& engine() override { return ::engine; }

  void printStatus(Print& out) override {
    portENTER_CRITICAL(&gMux);
    const GForce g = imuG;
    const Vec3 raw = imuRaw;
    const float temp = imuTempC;
    const Source src = source;
    const Scenario* sc = demoScenario;
    portEXIT_CRITICAL(&gMux);
    const int64_t up = esp_timer_get_time() - bootUs;
    const GForce shown = currentG(up);
    out.printf("uptime %lu s  mode %s  source %s%s\n", static_cast<unsigned long>(up / 1000000),
               ::engine.current() ? ::engine.current()->name() : "-",
               src == Source::Imu ? "imu" : (src == Source::Serial ? "serial" : "demo:"),
               src == Source::Demo ? sc->name() : "");
    out.printf("render %.1f fps  step %lu us  spi %lu us  dropped %lu  heap %lu  smoke %s\n",
               static_cast<double>(fpsEma), static_cast<unsigned long>(stepUs), static_cast<unsigned long>(transferUs),
               static_cast<unsigned long>(droppedFrames), static_cast<unsigned long>(ESP.getFreeHeap()),
               smokeEffect.allocated() ? "allocated" : "freed");
    out.printf("shown g: lat %+.3f vert %+.3f lon %+.3f (|h| %.3f)\n", static_cast<double>(shown.lateralG()),
               static_cast<double>(shown.verticalG()), static_cast<double>(shown.longitudinalG()),
               static_cast<double>(shown.horizontalG()));
    if (imu.present()) {
      out.printf("imu: ok, who 0x%02X%s  raw (%+.3f %+.3f %+.3f) g, |a| %.3f  %.1f C  errors %lu  reconnects %lu\n",
                 imu.whoAmI(), imu.clone() ? " (clone)" : "", static_cast<double>(raw.x), static_cast<double>(raw.y),
                 static_cast<double>(raw.z), static_cast<double>(raw.length()), static_cast<double>(temp),
                 static_cast<unsigned long>(imu.errorCount()), static_cast<unsigned long>(imu.recoveries()));
      out.printf("imu car frame: lat %+.3f vert %+.3f lon %+.3f  filter %.1f Hz  autozero %s\n",
                 static_cast<double>(g.lateralG()), static_cast<double>(g.verticalG()),
                 static_cast<double>(g.longitudinalG()), static_cast<double>(settings.filterHz),
                 settings.autoZero ? "on" : "off");
    } else if (imu.detected()) {
      out.println("imu: LOST (showing 0 g, retrying every second; check wiring and power)");
    } else {
      out.println("imu: not detected (retrying every second; check wiring and address)");
    }
    bool trusted;
    {
      Lock l(estMtx);
      trusted = estimator.forwardTrusted();
    }
    out.printf("calibration: %s, forward axis hint %s",
               calibration.valid ? (trusted ? "valid" : "up only (forward unknown: run cal brake)") : "NONE (cal fwd <axis>, then cal rest)",
               storage::forwardAxisName(settings.forwardAxis));
    if (calState == CalState::Rest || calState == CalState::Brake) out.print("  [capturing]");
    out.println();
    if (calMessage[0]) out.printf("last calibration: %s\n", calMessage);
    if (::engine.current() == &animEffect) {
      const AnimationAsset& a = animEffect.asset();
      out.printf("animation '%s' %ux%u x%u slices, %u frames: state %s, frame %d%s\n", a.name, a.width, a.height, a.depth,
                 a.frames, animStateName(animEffect.state()), animEffect.currentFrame(), animEffect.mirrored() ? " (mirrored)" : "");
    }
    out.printf("contrast ramp %u..%u  cycle %.0f s  readout %s  panel refresh every %u min\n", settings.contrastFront,
               settings.contrastBack, static_cast<double>(settings.autoCycleS), settings.readout ? "on" : "off",
               settings.reinitMinutes);
  }

  void printScenarios(Print& out) override {
    out.println("demo scenarios:");
    for (int i = 0; i < Scenario::presetCount(); ++i)
      out.printf("  %s (%.0f s)\n", Scenario::preset(i).name(), static_cast<double>(Scenario::preset(i).duration()));
  }

  // (engine) methods: callers hold engineMtx.
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
    demoBecauseNoImu = false;  // an explicit choice sticks
    portEXIT_CRITICAL(&gMux);
    return true;
  }

  void injectG(float lat, float vert, float lon) override {
    const Vec3 v{clampf(lat, -8.0f, 8.0f), clampf(vert, -8.0f, 8.0f), clampf(lon, -8.0f, 8.0f)};
    portENTER_CRITICAL(&gMux);
    serialDynamicG = v;
    source = Source::Serial;
    demoBecauseNoImu = false;
    portEXIT_CRITICAL(&gMux);
  }

  const char* startCalibration(const char* which) override {
    if (!imu.present()) return "no imu detected";
    if (calState == CalState::Rest || calState == CalState::Brake) return "a capture is already running";
    const bool rest = !strcmp(which, "rest");
    if (!rest && strcmp(which, "brake")) return "unknown capture";
    if (!rest && !calibration.valid) return "run cal rest first";
    {
      Lock l(estMtx);
      calAccum = Vec3{};
      calCount = 0;
      calDeadlineMs = millis() + (rest ? 2000 : 1500);
      calState = rest ? CalState::Rest : CalState::Brake;
    }
    return nullptr;
  }

  bool setForwardAxis(const char* axis) override {
    const int idx = storage::parseForwardAxis(axis);
    if (idx < 0) return false;
    settings.forwardAxis = static_cast<uint8_t>(idx);
    Lock l(estMtx);
    estimator.setNominalForward(storage::forwardAxisVector(settings.forwardAxis));
    return true;
  }

  bool saveState() override {
    {
      // Auto-cycle changes the effect without going through setMode(): save
      // the one on screen. (The CLI calls this without holding the lock.)
      Lock l(engineMtx);
      settings.mode = static_cast<uint8_t>(::engine.currentIndex());
    }
    Calibration c;
    {
      Lock l(estMtx);  // imuTask writes `calibration` under this lock
      c = calibration;
    }
    return storage::save(settings, c);
  }
  bool clearState() override { return storage::clear(); }
  float setFilterHz(float hz) override {
    settings.filterHz = clampf(hz, 0.5f, 30.0f);
    Lock l(estMtx);
    estimator.setFilter(settings.filterHz, static_cast<float>(board::kImuSampleHz));
    return settings.filterHz;
  }
  int setContrast(int panel, int value) override {
    const uint8_t v = static_cast<uint8_t>(clampi(value, 0, 255));
    if (panel < 0) { settings.contrastFront = settings.contrastBack = v; display.setContrastRamp(v, v); }
    else display.setContrast(panel, v);
    return v;
  }
  void setContrastRamp(int front, int back) override {
    settings.contrastFront = static_cast<uint8_t>(clampi(front, 0, 255));
    settings.contrastBack = static_cast<uint8_t>(clampi(back, 0, 255));
    display.setContrastRamp(settings.contrastFront, settings.contrastBack);
  }
  float setAutoCycle(float s) override {
    settings.autoCycleS = clampf(s, 0.0f, 3600.0f);
    ::engine.setAutoCycle(settings.autoCycleS);
    return settings.autoCycleS;
  }
  void setReadout(bool on) override { settings.readout = on; ::engine.setReadout(on); }
  void setInvert(bool on) override { settings.invert = on; display.setInverted(on); }
  void setAutoZero(bool on) override {
    settings.autoZero = on;
    Lock l(estMtx);
    estimator.setAutoZero(on);
  }
  int testPattern(int panel) override {
    testPanel = panel < 0 ? -2 : clampi(panel, 0, PanelStack::kPanels - 1);
    return panel < 0 ? -1 : testPanel;
  }
  void reinitPanels() override { wantReinit = true; }
  void setRefreshMinutes(int minutes) override { settings.reinitMinutes = static_cast<uint8_t>(clampi(minutes, 0, 255)); }
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
  const char* fixedEffect() const override { return kFixedEffect; }
};

App app;
SerialCli serialCli(app);

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
    const char* why = app.startCalibration("rest");
    if (why) Serial.printf("button: cannot start rest calibration: %s\n", why);
    else Serial.println("button: rest calibration started (hold still 2 s)");
  }
  if (!down && wasDown && !longFired && now - downSince > 40 && kFixedEffect) {
    Serial.printf("button: this firmware always shows %s\n", kFixedEffect);
  } else if (!down && wasDown && !longFired && now - downSince > 40) {
    const char* name;
    {
      Lock l(engineMtx);
      app.nextMode();
      name = engine.current()->name();
    }
    Serial.printf("button: mode %s\n", name);
  }
  wasDown = down;
}

}  // namespace

void setup() {
  buildObjects();
  // A transmit buffer so long replies (get, list, status) do not block the
  // CLI, and with it the engine lock, until the UART has sent every byte.
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  delay(200);
  bootUs = esp_timer_get_time();
  Serial.println();
  Serial.printf("gmeter: %dx%dx%d voxels, panel model %d, spi %d Hz, target %d fps\n", kW, kH, kD, GM_PANEL_MODEL,
                GM_SPI_HZ, GM_TARGET_FPS);

  engineMtx = xSemaphoreCreateMutex();
  estMtx = xSemaphoreCreateMutex();
  frameSem = xSemaphoreCreateBinary();
  frames = new (std::nothrow) VoxelFrame[3];
  if (!frames) {
    Serial.println("fatal: not enough memory for the frame buffers");
    for (;;) delay(1000);
  }

  bool migrated = false;
  const bool restored = storage::load(settings, calibration, &migrated);
  Serial.println(restored ? "settings restored from NVS" : "no saved settings; using defaults");
  if (migrated) Serial.println("calibration from an older firmware had its left/right axis mirrored; corrected (type save to keep)");

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
  engine.addEffect(&hudEffect);
  engine.addEffect(&atFieldEffect);
  engine.addEffect(&syncEffect);
  int startEffect = settings.mode < engine.effectCount() ? settings.mode : 0;
  if (kFixedEffect) {
    const int i = engine.findEffect(kFixedEffect);
    if (i >= 0) startEffect = i;
    else Serial.printf("error: fixed effect '%s' does not exist; showing '%s'\n", kFixedEffect, engine.effect(startEffect)->name());
    settings.autoCycleS = 0.0f;
    Serial.printf("single-effect build: always shows %s\n", engine.effect(startEffect)->name());
  }
  settings.mode = static_cast<uint8_t>(startEffect);
  engine.setEffect(startEffect, false);
  applySettings();

  if (imu.begin(board::kI2cSda, board::kI2cScl, board::kImuAddress)) {
    Serial.printf("imu: MPU-6050 found (WHO_AM_I 0x%02X%s)\n", imu.whoAmI(), imu.clone() ? ", clone" : "");
    source = Source::Imu;
    imuLive = true;
    if (!calibration.valid)
      Serial.println("imu: not calibrated. Tell it which sensor axis faces the windshield (cal fwd +y, for example), "
                     "park level, then: cal rest, then save");
  } else {
    Serial.printf("imu: not found (WHO_AM_I 0x%02X); showing demo scenario '%s' and retrying. Type: help\n",
                  imu.whoAmI(), demoScenario->name());
    source = Source::Demo;
    demoBecauseNoImu = true;
  }

  xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, nullptr, 1);
  Serial.print("> ");
}

void loop() {
  serialCli.poll(Serial);
  pollButton();

  static uint32_t lastRefreshMs = millis();
  if (settings.reinitMinutes > 0 && millis() - lastRefreshMs > settings.reinitMinutes * 60000UL) {
    lastRefreshMs = millis();
    wantRefresh = true;  // soft: re-send registers without blanking the panels
  }
  const CalState cs = calState;
  if (cs == CalState::Done || cs == CalState::Failed) {
    Serial.printf("\r\n%s\r\n> ", calMessage);
    calState = CalState::Idle;
  }
  if (imuEvent) {
    imuEvent = false;
    Serial.printf("\r\n%s\r\n> ", imuEventText);
  }
  delay(5);
}
