// Volumetric G-force meter firmware.
//
// Tasks:
//   imu_task     (core 0, 200 Hz)  reads the accelerometer, runs the estimator,
//                                  publishes the filtered g-force; handles calibration
//                                  captures; retries a lost sensor once a second.
//   render_task  (core 1, 60 fps)  steps the active effect and renders a voxel frame.
//   display_task (core 0)          streams finished frames to the 8 panels over SPI.
//   app_main     (core 1, idle)    serial CLI, mode button, periodic housekeeping
//                                  (the main task: priority 1 on core 1, set in
//                                  sdkconfig.defaults, like Arduino's loop task).
//
// Concurrency: the g vector, source and frame-buffer indices are shared under
// the s_mux spinlock. The engine (and its effects) is guarded by s_engine_mtx:
// render_task holds it while stepping, and the CLI/button hold it while they
// change modes or parameters. The estimator is guarded by s_est_mtx in the
// same way between imu_task and the CLI.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "console_io.h"
#include "display_ssd1309.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gmeter.h"
#include "imu_mpu6050.h"
#include "serial_cli.h"
#include "storage.h"

// Artwork exported from Gmeter Studio (host/web): save it as
// main/user_animation.h and it replaces the built-in demo animation.
// main/CMakeLists.txt defines GM_HAVE_USER_ANIMATION when the file exists.
#if defined(GM_HAVE_USER_ANIMATION)
#include "user_animation.h"
#define GM_USER_ANIMATION (&kUserAnimation)
#else
#define GM_USER_ANIMATION gm_anim_demo()
#endif

// Single-effect build (the HUD edition): menuconfig > Gmeter > Edition, or
// configs/hud.defaults. The firmware then always shows that effect: it ignores
// the saved mode and auto-cycle, and the BOOT button and the mode/cycle/anim
// commands no longer switch away from it.
#if CONFIG_GMETER_EDITION_HUD
static const char* const k_fixed_effect = "hud";
#else
static const char* const k_fixed_effect = NULL;
#endif

// ---- hardware & core objects ------------------------------------------------------------
// Static storage, set up at the top of app_main by their init functions.
static panel_stack_t s_display;
static mpu6050_t s_imu;
static gm_estimator_t s_estimator;
static gm_engine_t s_engine;
static gm_gball_t s_ball;
static gm_slosh_t s_slosh;
static gm_particles_t s_particles;
static gm_smoke_t s_smoke;
static gm_tunnel_t s_tunnel;
static gm_sprite_t s_sprite;
static gm_anim_t s_anim;
static gm_hud_t s_hud;
static gm_atfield_t s_atfield;
static gm_sync_t s_sync;

static void build_objects(void) {
  mpu6050_init(&s_imu);
  gm_estimator_init(&s_estimator);
  gm_engine_init(&s_engine);
  gm_gball_init(&s_ball);
  gm_slosh_init(&s_slosh);
  gm_particles_init(&s_particles);
  gm_smoke_init(&s_smoke);
  gm_tunnel_init(&s_tunnel);
  gm_sprite_init(&s_sprite, gm_sprite_demo_sheet(), NULL);
  gm_anim_init(&s_anim, GM_USER_ANIMATION);
  gm_hud_init(&s_hud);
  gm_atfield_init(&s_atfield);
  gm_sync_init(&s_sync);
}

static settings_t s_settings;
static gm_calibration_t s_calibration;

static SemaphoreHandle_t s_engine_mtx;  // engine + effects
static SemaphoreHandle_t s_est_mtx;     // estimator

static inline void lock(SemaphoreHandle_t m) { xSemaphoreTake(m, portMAX_DELAY); }
static inline void unlock(SemaphoreHandle_t m) { xSemaphoreGive(m); }

static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- shared state (protected by s_mux) -----------------------------------------------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gm_gforce_t s_imu_g = GM_GFORCE_REST_INIT;  // latest filtered g from the IMU (rest while the sensor is lost)
static gm_vec3_t s_imu_raw;                        // latest raw sensor sample (sensor frame)
static float s_imu_temp_c = 0.0f;
static gm_vec3_t s_serial_dynamic_g;               // injected over serial (lat, vert, lon)
typedef enum { SOURCE_IMU, SOURCE_SERIAL, SOURCE_DEMO } source_t;
static source_t s_source = SOURCE_DEMO;
static bool s_demo_because_no_imu = false;  // switch to the IMU automatically when it shows up
static bool s_imu_live = false;             // the IMU is delivering samples (false after it is lost)
static const gm_scenario_t* s_demo_scenario;  // "drive", the last preset (set in app_main)

typedef enum { CAL_IDLE, CAL_REST, CAL_BRAKE, CAL_DONE, CAL_FAILED } cal_state_t;
static volatile cal_state_t s_cal_state = CAL_IDLE;
static gm_vec3_t s_cal_accum;
static int s_cal_count = 0;
static uint32_t s_cal_deadline_ms = 0;
static const char* volatile s_cal_message = "";
static volatile bool s_imu_event = false;  // imu_task -> main loop: print s_imu_event_text
static const char* volatile s_imu_event_text = "";

// ---- frame hand-off: triple buffer ---------------------------------------------------------------
// Allocated in app_main on the heap (DMA-capable, so the SPI driver sends
// straight from it): the ESP32's static DRAM segment is small, while the heap
// can also use the DRAM above it.
static gm_frame_t* s_frames = NULL;
static int s_frame_ready = -1, s_frame_displaying = -1;
static SemaphoreHandle_t s_frame_sem;
static volatile bool s_want_reinit = false;   // hard reset (CLI)
static volatile bool s_want_refresh = false;  // soft re-send (periodic)
static volatile int s_test_panel = -2;        // -2 none, >= 0 panel index

// ---- stats --------------------------------------------------------------------------------------------
static float s_fps_ema = 0.0f;
static uint32_t s_step_us = 0, s_transfer_us = 0, s_dropped_frames = 0;
static int64_t s_boot_us = 0;

// ---- helpers -------------------------------------------------------------------------------------------
// `label` (may be NULL) receives the tag shown on the panels while the g is
// not live sensor data: DEMO, SERIAL or NO SENSOR; NULL when it is live.
static gm_gforce_t current_g(int64_t us_since_boot, const char** label) {
  gm_gforce_t g = GM_GFORCE_REST_INIT;
  portENTER_CRITICAL(&s_mux);
  const source_t src = s_source;
  const bool live = s_imu_live;
  const gm_gforce_t from_imu = s_imu_g;
  const gm_vec3_t injected = s_serial_dynamic_g;
  const gm_scenario_t* sc = s_demo_scenario;
  portEXIT_CRITICAL(&s_mux);
  if (label) *label = src == SOURCE_IMU ? (live ? NULL : "NO SENSOR") : (src == SOURCE_SERIAL ? "SERIAL" : "DEMO");
  switch (src) {
    case SOURCE_IMU:
      g = from_imu;
      break;
    case SOURCE_SERIAL:
      g.proper = gm_vec3_add(gm_gforce_rest_proper(), injected);
      break;
    case SOURCE_DEMO: {
      // Wrap in integer microseconds so the demo stays smooth after days of uptime.
      const float dur = gm_scenario_duration(sc);
      const int64_t period = (int64_t)((dur + 1.0f) * 1e6f);
      g = gm_scenario_at(sc, (float)(us_since_boot % period) * 1e-6f, NULL);
      break;
    }
  }
  return g;
}

static void apply_settings(void) {
  gm_engine_set_readout(&s_engine, s_settings.readout != 0);
  gm_engine_set_auto_cycle(&s_engine, k_fixed_effect ? 0.0f : s_settings.auto_cycle_s);
  gm_estimator_set_filter(&s_estimator, s_settings.filter_hz, (float)BOARD_IMU_SAMPLE_HZ);
  gm_estimator_set_auto_zero(&s_estimator, s_settings.auto_zero != 0, 60.0f);
  if (s_calibration.valid) {
    gm_estimator_set_calibration(&s_estimator, s_calibration);
    // Keep the calibrated forward axis (possibly refined by `cal brake`) as the
    // hint, so a later `cal rest` does not throw that refinement away.
    gm_estimator_set_nominal_forward(&s_estimator, gm_mat3_row(&s_calibration.sensor_to_car, 2));
  } else {
    gm_estimator_set_nominal_forward(&s_estimator, storage_forward_axis_vector(s_settings.forward_axis));
  }
}

static void publish_imu(gm_gforce_t g, gm_vec3_t raw, float temp, bool live) {
  portENTER_CRITICAL(&s_mux);
  s_imu_live = live;
  s_imu_g = g;
  s_imu_raw = raw;
  s_imu_temp_c = temp;
  portEXIT_CRITICAL(&s_mux);
}

// ---- tasks ----------------------------------------------------------------------------------------------
static void imu_task(void* arg) {
  (void)arg;
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / BOARD_IMU_SAMPLE_HZ);
  TickType_t next_retry = 0;
  bool was_present = mpu6050_present(&s_imu);
  for (;;) {
    xTaskDelayUntil(&last, period);
    if (!mpu6050_present(&s_imu)) {
      if (was_present) {
        // Lost: show a neutral g instead of freezing on the last value.
        publish_imu(gm_gforce_rest(), gm_vec3_zero(), 0.0f, false);
        s_imu_event_text = "imu: lost contact; showing 0 g and retrying every second";
        s_imu_event = true;
        was_present = false;
      }
      const TickType_t now = xTaskGetTickCount();
      if ((int32_t)(now - next_retry) >= 0) {
        next_retry = now + pdMS_TO_TICKS(1000);
        if (mpu6050_recover(&s_imu)) {
          lock(s_est_mtx);
          gm_estimator_reset(&s_estimator);
          unlock(s_est_mtx);
          portENTER_CRITICAL(&s_mux);
          if (s_demo_because_no_imu && s_source == SOURCE_DEMO) s_source = SOURCE_IMU;
          s_demo_because_no_imu = false;
          portEXIT_CRITICAL(&s_mux);
          s_imu_event_text = "imu: sensor (re)connected";
          s_imu_event = true;
          was_present = true;
        }
        last = xTaskGetTickCount();  // the retry took time; do not burst afterwards
      }
      continue;
    }
    gm_vec3_t raw;
    float temp;
    if (!mpu6050_read(&s_imu, &raw, &temp)) continue;  // read marks the sensor lost after repeated failures

    gm_gforce_t g;
    lock(s_est_mtx);
    g = gm_estimator_update(&s_estimator, raw);

    // Calibration captures run here so they see every sample.
    const cal_state_t cs = s_cal_state;
    if (cs == CAL_REST || cs == CAL_BRAKE) {
      s_cal_accum = gm_vec3_add(s_cal_accum, raw);
      ++s_cal_count;
      if (millis() >= s_cal_deadline_ms) {
        bool ok = false;
        const gm_vec3_t mean = gm_vec3_div(s_cal_accum, (float)s_cal_count);
        if (cs == CAL_REST) {
          gm_estimator_begin_rest_calibration(&s_estimator);
          for (int i = 0; i < 16; ++i) gm_estimator_feed_rest_sample(&s_estimator, mean);
          ok = gm_estimator_finish_rest_calibration(&s_estimator);
          if (!ok)
            s_cal_message = "rest calibration failed: the average was not about 1 g (was the car moving?)";
          else if (!gm_estimator_forward_trusted(&s_estimator))
            s_cal_message =
                "rest ok, but the forward axis is unknown (the forward hint points up or down). "
                "Run cal brake while braking firmly in a straight line, or cal fwd <axis> then cal rest";
          else
            s_cal_message = "rest calibration ok (save to keep; cal brake refines forward)";
        } else {
          ok = gm_estimator_calibrate_forward_from_braking(&s_estimator, mean);
          s_cal_message = ok ? "forward axis set from braking (save to keep)"
                             : "braking capture too weak (needs more than 0.05 g): brake harder and try again";
        }
        if (ok) s_calibration = gm_estimator_calibration(&s_estimator);
        s_cal_state = ok ? CAL_DONE : CAL_FAILED;
      }
    }
    unlock(s_est_mtx);
    publish_imu(g, raw, temp, true);
  }
}

static void render_task(void* arg) {
  (void)arg;
  int64_t last_us = esp_timer_get_time();
  const int64_t period_us = 1000000 / GM_TARGET_FPS;
  for (;;) {
    const int64_t now = esp_timer_get_time();
    const float dt = (float)(now - last_us) * 1e-6f;
    last_us = now;

    const char* label = NULL;
    const gm_gforce_t g = current_g(now - s_boot_us, &label);

    // Pick a buffer that is neither being displayed nor already queued.
    portENTER_CRITICAL(&s_mux);
    int target = 0;
    while (target == s_frame_ready || target == s_frame_displaying) ++target;
    portEXIT_CRITICAL(&s_mux);
    lock(s_engine_mtx);
    // Only the single-effect (HUD) edition tags data that is not live: in the
    // standard edition that corner holds the ball effect's peak readout.
    gm_engine_set_status_label(&s_engine, k_fixed_effect ? label : NULL);
    gm_engine_step(&s_engine, g, dt);
    memcpy(&s_frames[target], gm_engine_frame(&s_engine), sizeof(gm_frame_t));
    unlock(s_engine_mtx);
    portENTER_CRITICAL(&s_mux);
    if (s_frame_ready >= 0) ++s_dropped_frames;  // display did not keep up; newest frame wins
    s_frame_ready = target;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_frame_sem);

    const int64_t spent = esp_timer_get_time() - now;
    s_step_us = (uint32_t)spent;
    const float inst = 1.0f / (dt > 1e-4f ? dt : 1e-4f);
    s_fps_ema = s_fps_ema == 0.0f ? inst : s_fps_ema + 0.05f * (inst - s_fps_ema);
    // Always sleep at least one tick: a plain yield would never let the
    // lower-priority main/CLI task or the idle task run when rendering is
    // over budget (and the idle task feeds the watchdog).
    const int64_t sleep_us = period_us - spent;
    const uint32_t sleep_ms = sleep_us > 1000 ? (uint32_t)(sleep_us / 1000) : 1u;
    vTaskDelay(pdMS_TO_TICKS(sleep_ms));
  }
}

static void display_task(void* arg) {
  (void)arg;
  for (;;) {
    if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    if (s_want_reinit) {
      s_want_reinit = false;
      panel_stack_reinit(&s_display);
    }
    if (s_want_refresh) {
      s_want_refresh = false;
      panel_stack_refresh(&s_display);
    }
    const int tp = s_test_panel;
    if (tp >= 0) {
      panel_stack_test_pattern(&s_display, tp);
      // Hold the test pattern; drop frames until the test is cleared.
      portENTER_CRITICAL(&s_mux);
      s_frame_ready = -1;
      portEXIT_CRITICAL(&s_mux);
      continue;
    }
    portENTER_CRITICAL(&s_mux);
    s_frame_displaying = s_frame_ready;
    s_frame_ready = -1;
    portEXIT_CRITICAL(&s_mux);
    if (s_frame_displaying < 0) continue;
    panel_stack_write_frame(&s_display, &s_frames[s_frame_displaying]);
    s_transfer_us = panel_stack_last_transfer_us(&s_display);
    portENTER_CRITICAL(&s_mux);
    s_frame_displaying = -1;
    portEXIT_CRITICAL(&s_mux);
    // The next frame is already waiting: the bus cannot keep up (a slow SPI
    // clock or a high frame rate). The transfer polls, so sleep one tick or
    // the idle task on this core never runs and the task watchdog fires.
    if (uxSemaphoreGetCount(s_frame_sem) > 0) vTaskDelay(1);
  }
}

// ---- CLI binding ----------------------------------------------------------------------------------------
static void app_lock_engine(void) { lock(s_engine_mtx); }
static void app_unlock_engine(void) { unlock(s_engine_mtx); }
static gm_engine_t* app_engine(void) { return &s_engine; }

static void app_print_status(print_t* out) {
  portENTER_CRITICAL(&s_mux);
  const gm_gforce_t g = s_imu_g;
  const gm_vec3_t raw = s_imu_raw;
  const float temp = s_imu_temp_c;
  const source_t src = s_source;
  const gm_scenario_t* sc = s_demo_scenario;
  portEXIT_CRITICAL(&s_mux);
  const int64_t up = esp_timer_get_time() - s_boot_us;
  const gm_gforce_t shown = current_g(up, NULL);
  gm_effect_t* cur = gm_engine_current(&s_engine);
  print_fmt(out, "uptime %lu s  mode %s  source %s%s\n", (unsigned long)(up / 1000000), cur ? gm_effect_name(cur) : "-",
            src == SOURCE_IMU ? "imu" : (src == SOURCE_SERIAL ? "serial" : "demo:"),
            src == SOURCE_DEMO ? gm_scenario_name(sc) : "");
  print_fmt(out, "render %.1f fps  step %lu us  spi %lu us  dropped %lu  heap %lu  smoke %s\n", (double)s_fps_ema,
            (unsigned long)s_step_us, (unsigned long)s_transfer_us, (unsigned long)s_dropped_frames,
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            gm_smoke_allocated(&s_smoke) ? "allocated" : "freed");
  print_fmt(out, "shown g: lat %+.3f vert %+.3f lon %+.3f (|h| %.3f)\n", (double)gm_gforce_lateral(shown),
            (double)gm_gforce_vertical(shown), (double)gm_gforce_longitudinal(shown),
            (double)gm_gforce_horizontal(shown));
  if (mpu6050_present(&s_imu)) {
    print_fmt(out, "imu: ok, who 0x%02X%s  raw (%+.3f %+.3f %+.3f) g, |a| %.3f  %.1f C  errors %lu  reconnects %lu\n",
              mpu6050_who_am_i(&s_imu), mpu6050_clone(&s_imu) ? " (clone)" : "", (double)raw.x, (double)raw.y,
              (double)raw.z, (double)gm_vec3_length(raw), (double)temp,
              (unsigned long)mpu6050_error_count(&s_imu), (unsigned long)mpu6050_recoveries(&s_imu));
    print_fmt(out, "imu car frame: lat %+.3f vert %+.3f lon %+.3f  filter %.1f Hz  autozero %s\n",
              (double)gm_gforce_lateral(g), (double)gm_gforce_vertical(g), (double)gm_gforce_longitudinal(g),
              (double)s_settings.filter_hz, s_settings.auto_zero ? "on" : "off");
  } else if (mpu6050_detected(&s_imu)) {
    print_ln(out, "imu: LOST (showing 0 g, retrying every second; check wiring and power)");
  } else {
    print_ln(out, "imu: not detected (retrying every second; check wiring and address)");
  }
  lock(s_est_mtx);
  const bool trusted = gm_estimator_forward_trusted(&s_estimator);
  unlock(s_est_mtx);
  print_fmt(out, "calibration: %s, forward axis hint %s",
            s_calibration.valid ? (trusted ? "valid" : "up only (forward unknown: run cal brake)")
                                : "NONE (cal fwd <axis>, then cal rest)",
            storage_forward_axis_name(s_settings.forward_axis));
  if (s_cal_state == CAL_REST || s_cal_state == CAL_BRAKE) print_str(out, "  [capturing]");
  print_ln(out, "");
  if (s_cal_message[0]) print_fmt(out, "last calibration: %s\n", s_cal_message);
  if (gm_engine_current(&s_engine) == &s_anim.base) {
    const gm_anim_asset_t* a = gm_anim_asset(&s_anim);
    print_fmt(out, "animation '%s' %ux%u x%u slices, %u frames: state %s, frame %d%s\n", a->name, (unsigned)a->width,
              (unsigned)a->height, (unsigned)a->depth, (unsigned)a->frames, gm_anim_state_name(gm_anim_state(&s_anim)),
              gm_anim_current_frame(&s_anim), gm_anim_mirrored(&s_anim) ? " (mirrored)" : "");
  }
  print_fmt(out, "contrast ramp %u..%u  cycle %.0f s  readout %s  panel refresh every %u min\n",
            (unsigned)s_settings.contrast_front, (unsigned)s_settings.contrast_back,
            (double)(k_fixed_effect ? 0.0f : s_settings.auto_cycle_s), s_settings.readout ? "on" : "off",
            (unsigned)s_settings.reinit_minutes);
}

static void app_print_scenarios(print_t* out) {
  print_ln(out, "demo scenarios:");
  for (int i = 0; i < gm_scenario_preset_count(); ++i)
    print_fmt(out, "  %s (%.0f s)\n", gm_scenario_name(gm_scenario_preset(i)),
              (double)gm_scenario_duration(gm_scenario_preset(i)));
}

// (engine) functions: callers hold s_engine_mtx.
static bool app_set_mode(const char* name_or_index) {
  if (!name_or_index) return false;
  int idx = gm_engine_find_effect(&s_engine, name_or_index);
  if (idx < 0 && name_or_index[0] >= '0' && name_or_index[0] <= '9') idx = atoi(name_or_index);
  if (idx < 0 || idx >= gm_engine_effect_count(&s_engine)) return false;
  gm_engine_set_effect(&s_engine, idx, true);
  s_settings.mode = (uint8_t)idx;
  return true;
}

static void app_next_mode(void) {
  gm_engine_next_effect(&s_engine);
  s_settings.mode = (uint8_t)gm_engine_current_index(&s_engine);
}

static bool app_set_source(const char* name, const char* arg) {
  source_t s;
  const gm_scenario_t* sc = s_demo_scenario;
  if (!strcmp(name, "imu")) {
    if (!mpu6050_present(&s_imu)) {
      print_ln(conio_out(), "no imu detected");
      return false;
    }
    s = SOURCE_IMU;
  } else if (!strcmp(name, "serial")) {
    s = SOURCE_SERIAL;
  } else if (!strcmp(name, "demo")) {
    s = SOURCE_DEMO;
    if (arg) {
      sc = gm_scenario_find(arg);
      if (!sc) {
        print_ln(conio_out(), "unknown scenario (list)");
        return false;
      }
    }
  } else {
    return false;
  }
  portENTER_CRITICAL(&s_mux);
  s_source = s;
  s_demo_scenario = sc;
  s_serial_dynamic_g = gm_vec3_zero();
  s_demo_because_no_imu = false;  // an explicit choice sticks
  portEXIT_CRITICAL(&s_mux);
  return true;
}

static void app_inject_g(float lat, float vert, float lon) {
  const gm_vec3_t v =
      gm_vec3(gm_clampf(lat, -8.0f, 8.0f), gm_clampf(vert, -8.0f, 8.0f), gm_clampf(lon, -8.0f, 8.0f));
  portENTER_CRITICAL(&s_mux);
  s_serial_dynamic_g = v;
  s_source = SOURCE_SERIAL;
  s_demo_because_no_imu = false;
  portEXIT_CRITICAL(&s_mux);
}

static const char* app_start_calibration(const char* which) {
  if (!mpu6050_present(&s_imu)) return "no imu detected";
  if (s_cal_state == CAL_REST || s_cal_state == CAL_BRAKE) return "a capture is already running";
  const bool rest = !strcmp(which, "rest");
  if (!rest && strcmp(which, "brake")) return "unknown capture";
  if (!rest && !s_calibration.valid) return "run cal rest first";
  lock(s_est_mtx);
  s_cal_accum = gm_vec3_zero();
  s_cal_count = 0;
  s_cal_deadline_ms = millis() + (rest ? 2000 : 1500);
  s_cal_state = rest ? CAL_REST : CAL_BRAKE;
  unlock(s_est_mtx);
  return NULL;
}

static bool app_set_forward_axis(const char* axis) {
  const int idx = storage_parse_forward_axis(axis);
  if (idx < 0) return false;
  s_settings.forward_axis = (uint8_t)idx;
  lock(s_est_mtx);
  gm_estimator_set_nominal_forward(&s_estimator, storage_forward_axis_vector(s_settings.forward_axis));
  unlock(s_est_mtx);
  return true;
}

static bool app_save_state(void) {
  // Auto-cycle changes the effect without going through set_mode: save the
  // one on screen. (The CLI calls this without holding the lock.) A
  // single-effect build keeps the stored choice for the standard firmware.
  lock(s_engine_mtx);
  if (!k_fixed_effect) s_settings.mode = (uint8_t)gm_engine_current_index(&s_engine);
  unlock(s_engine_mtx);
  lock(s_est_mtx);  // imu_task writes s_calibration under this lock
  const gm_calibration_t c = s_calibration;
  unlock(s_est_mtx);
  return storage_save(&s_settings, &c);
}

static bool app_clear_state(void) { return storage_clear(); }

static float app_set_filter_hz(float hz) {
  s_settings.filter_hz = gm_clampf(hz, 0.5f, 30.0f);
  lock(s_est_mtx);
  gm_estimator_set_filter(&s_estimator, s_settings.filter_hz, (float)BOARD_IMU_SAMPLE_HZ);
  unlock(s_est_mtx);
  return s_settings.filter_hz;
}

static int app_set_contrast(int panel, int value) {
  const uint8_t v = (uint8_t)gm_clampi(value, 0, 255);
  if (panel < 0) {
    s_settings.contrast_front = s_settings.contrast_back = v;
    panel_stack_set_contrast_ramp(&s_display, v, v);
  } else {
    panel_stack_set_contrast(&s_display, panel, v);
  }
  return v;
}

static void app_set_contrast_ramp(int front, int back) {
  s_settings.contrast_front = (uint8_t)gm_clampi(front, 0, 255);
  s_settings.contrast_back = (uint8_t)gm_clampi(back, 0, 255);
  panel_stack_set_contrast_ramp(&s_display, s_settings.contrast_front, s_settings.contrast_back);
}

static float app_set_auto_cycle(float s) {
  s_settings.auto_cycle_s = gm_clampf(s, 0.0f, 3600.0f);
  gm_engine_set_auto_cycle(&s_engine, s_settings.auto_cycle_s);
  return s_settings.auto_cycle_s;
}

static void app_set_readout(bool on) {
  s_settings.readout = on;
  gm_engine_set_readout(&s_engine, on);
}

static void app_set_invert(bool on) {
  s_settings.invert = on;
  panel_stack_set_inverted(&s_display, on);
}

static void app_set_auto_zero(bool on) {
  s_settings.auto_zero = on;
  lock(s_est_mtx);
  gm_estimator_set_auto_zero(&s_estimator, on, 60.0f);
  unlock(s_est_mtx);
}

static int app_test_pattern(int panel) {
  s_test_panel = panel < 0 ? -2 : gm_clampi(panel, 0, PANEL_COUNT - 1);
  return panel < 0 ? -1 : s_test_panel;
}

static void app_reinit_panels(void) { s_want_reinit = true; }

static void app_set_refresh_minutes(int minutes) { s_settings.reinit_minutes = (uint8_t)gm_clampi(minutes, 0, 255); }

static bool app_pin_anim_state(const char* name) {
  if (!name || !strcmp(name, "auto")) {
    gm_anim_force_state(&s_anim, -1);
    return true;
  }
  for (int i = 0; i < GM_ANIM_STATE_COUNT; ++i)
    if (!strcmp(name, gm_anim_state_name(i))) {
      gm_anim_force_state(&s_anim, i);
      app_set_mode("anim");
      return true;
    }
  return false;
}

static void app_reboot(void) {
  conio_flush(100);
  esp_restart();
}

static const char* app_fixed_effect(void) { return k_fixed_effect; }

static const app_control_t k_app = {
    .lock_engine = app_lock_engine,
    .unlock_engine = app_unlock_engine,
    .engine = app_engine,
    .print_status = app_print_status,
    .print_scenarios = app_print_scenarios,
    .set_mode = app_set_mode,
    .next_mode = app_next_mode,
    .set_source = app_set_source,
    .inject_g = app_inject_g,
    .start_calibration = app_start_calibration,
    .set_forward_axis = app_set_forward_axis,
    .save_state = app_save_state,
    .clear_state = app_clear_state,
    .set_filter_hz = app_set_filter_hz,
    .set_contrast = app_set_contrast,
    .set_contrast_ramp = app_set_contrast_ramp,
    .set_auto_cycle = app_set_auto_cycle,
    .set_readout = app_set_readout,
    .set_invert = app_set_invert,
    .set_auto_zero = app_set_auto_zero,
    .test_pattern = app_test_pattern,
    .reinit_panels = app_reinit_panels,
    .set_refresh_minutes = app_set_refresh_minutes,
    .pin_anim_state = app_pin_anim_state,
    .reboot = app_reboot,
    .fixed_effect = app_fixed_effect,
};

static serial_cli_t s_cli;

// ---- button -----------------------------------------------------------------------------------------------------
static void poll_button(void) {
  static bool was_down = false;
  static uint32_t down_since = 0;
  static bool long_fired = false;
  print_t* con = conio_out();
  const bool down = gpio_get_level((gpio_num_t)BOARD_BUTTON) == 0;
  const uint32_t now = millis();
  if (down && !was_down) {
    down_since = now;
    long_fired = false;
  }
  if (down && !long_fired && now - down_since > 2000) {
    long_fired = true;
    const char* why = app_start_calibration("rest");
    if (why) print_fmt(con, "button: cannot start rest calibration: %s\n", why);
    else print_ln(con, "button: rest calibration started (hold still 2 s)");
  }
  if (!down && was_down && !long_fired && now - down_since > 40 && k_fixed_effect) {
    print_fmt(con, "button: this firmware always shows %s\n", k_fixed_effect);
  } else if (!down && was_down && !long_fired && now - down_since > 40) {
    const char* name;
    lock(s_engine_mtx);
    app_next_mode();
    name = gm_effect_name(gm_engine_current(&s_engine));
    unlock(s_engine_mtx);
    print_fmt(con, "button: mode %s\n", name);
  }
  was_down = down;
}

// ---- start-up and the main loop ---------------------------------------------------------------------------------
void app_main(void) {
  build_objects();
  settings_defaults(&s_settings);
  s_calibration = gm_calibration_none();
  s_demo_scenario = gm_scenario_preset(gm_scenario_preset_count() - 1);  // "drive"
  // A transmit buffer so long replies (get, list, status) do not block the
  // CLI, and with it the engine lock, until the UART has sent every byte.
  conio_init();
  print_t* con = conio_out();
  vTaskDelay(pdMS_TO_TICKS(200));
  s_boot_us = esp_timer_get_time();
  print_ln(con, "");
  print_fmt(con, "gmeter: %dx%dx%d voxels, panel model %d, spi %d Hz, target %d fps\n", GM_W, GM_H, GM_D,
            GM_PANEL_MODEL, GM_SPI_HZ, GM_TARGET_FPS);

  s_engine_mtx = xSemaphoreCreateMutex();
  s_est_mtx = xSemaphoreCreateMutex();
  s_frame_sem = xSemaphoreCreateBinary();
  s_frames = (gm_frame_t*)heap_caps_calloc(3, sizeof(gm_frame_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_frames) {
    print_ln(con, "fatal: not enough memory for the frame buffers");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  storage_init();
  bool migrated = false;
  const bool restored = storage_load(&s_settings, &s_calibration, &migrated);
  print_ln(con, restored ? "settings restored from NVS" : "no saved settings; using defaults");
  if (migrated)
    print_ln(con, "calibration from an older firmware had its left/right axis mirrored; corrected (type save to keep)");

  gpio_config_t button;
  memset(&button, 0, sizeof(button));
  button.pin_bit_mask = 1ull << BOARD_BUTTON;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&button);

  panel_stack_begin(&s_display);
  panel_stack_set_contrast_ramp(&s_display, s_settings.contrast_front, s_settings.contrast_back);
  if (s_settings.invert) panel_stack_set_inverted(&s_display, true);

  gm_engine_add_effect(&s_engine, &s_ball.base);
  gm_engine_add_effect(&s_engine, &s_slosh.base);
  gm_engine_add_effect(&s_engine, &s_particles.base);
  gm_engine_add_effect(&s_engine, &s_smoke.base);
  gm_engine_add_effect(&s_engine, &s_tunnel.base);
  gm_engine_add_effect(&s_engine, &s_sprite.base);
  gm_engine_add_effect(&s_engine, &s_anim.base);
  gm_engine_add_effect(&s_engine, &s_hud.base);
  gm_engine_add_effect(&s_engine, &s_atfield.base);
  gm_engine_add_effect(&s_engine, &s_sync.base);
  int start_effect = s_settings.mode < gm_engine_effect_count(&s_engine) ? s_settings.mode : 0;
  if (k_fixed_effect) {
    // Overrides the stored effect and auto-cycle (apply_settings) without
    // changing them, so `save` here leaves the standard firmware's choice.
    const int i = gm_engine_find_effect(&s_engine, k_fixed_effect);
    if (i >= 0) start_effect = i;
    else
      print_fmt(con, "error: fixed effect '%s' does not exist; showing '%s'\n", k_fixed_effect,
                gm_effect_name(gm_engine_effect(&s_engine, start_effect)));
    print_fmt(con, "single-effect build: always shows %s\n", gm_effect_name(gm_engine_effect(&s_engine, start_effect)));
  } else {
    s_settings.mode = (uint8_t)start_effect;
  }
  gm_engine_set_effect(&s_engine, start_effect, false);
  // Start the HUD's burn-in orbit at a random position, so short trips do not
  // all use the same one.
  gm_hud_set_orbit_step(&s_hud, (int)(esp_random() & 3));
  apply_settings();

  if (mpu6050_begin(&s_imu, BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_IMU_ADDRESS)) {
    print_fmt(con, "imu: MPU-6050 found (WHO_AM_I 0x%02X%s)\n", mpu6050_who_am_i(&s_imu),
              mpu6050_clone(&s_imu) ? ", clone" : "");
    s_source = SOURCE_IMU;
    s_imu_live = true;
    if (!s_calibration.valid)
      print_ln(con,
               "imu: not calibrated. Tell it which sensor axis faces the windshield (cal fwd +y, for example), "
               "park level, then: cal rest, then save");
  } else {
    print_fmt(con, "imu: not found (WHO_AM_I 0x%02X); showing demo scenario '%s' and retrying. Type: help\n",
              mpu6050_who_am_i(&s_imu), gm_scenario_name(s_demo_scenario));
    s_source = SOURCE_DEMO;
    s_demo_because_no_imu = true;
  }

  serial_cli_init(&s_cli, &k_app);
  xTaskCreatePinnedToCore(imu_task, "imu", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 2, NULL, 1);
  print_str(con, "> ");

  uint32_t last_refresh_ms = millis();
  for (;;) {
    serial_cli_poll(&s_cli, conio_read, con);
    poll_button();

    if (s_settings.reinit_minutes > 0 && millis() - last_refresh_ms > s_settings.reinit_minutes * 60000u) {
      last_refresh_ms = millis();
      s_want_refresh = true;  // soft: re-send registers without blanking the panels
    }
    const cal_state_t cs = s_cal_state;
    if (cs == CAL_DONE || cs == CAL_FAILED) {
      print_fmt(con, "\r\n%s\r\n> ", s_cal_message);
      s_cal_state = CAL_IDLE;
    }
    if (s_imu_event) {
      s_imu_event = false;
      print_fmt(con, "\r\n%s\r\n> ", s_imu_event_text);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
