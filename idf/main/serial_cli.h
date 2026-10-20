// Line-oriented serial command interface for bench testing and tuning.
// Type `help` in the serial monitor (115200 baud). Accepts CR, LF or CRLF line
// endings, echoes typed characters and handles backspace.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_io.h"
#include "gm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the CLI can ask the application to do; implemented in app_main.c.
// Functions marked (engine) touch the engine or an effect; the CLI calls them
// between lock_engine()/unlock_engine() so they never race the render task.
typedef struct {
  void (*lock_engine)(void);
  void (*unlock_engine)(void);
  gm_engine_t* (*engine)(void);  // (engine)
  void (*print_status)(print_t* out);
  void (*print_scenarios)(print_t* out);
  bool (*set_mode)(const char* name_or_index);           // (engine)
  void (*next_mode)(void);                               // (engine)
  bool (*set_source)(const char* name, const char* arg);  // imu | serial | demo [scenario]; arg may be NULL
  void (*inject_g)(float lat, float vert, float lon);
  // Returns NULL when the capture started, otherwise the reason it did not.
  const char* (*start_calibration)(const char* which);  // rest | brake
  bool (*set_forward_axis)(const char* axis);           // +x -x +y -y +z -z
  bool (*save_state)(void);
  bool (*clear_state)(void);
  // Setters that clamp their input return the value actually applied.
  float (*set_filter_hz)(float hz);
  int (*set_contrast)(int panel, int value);       // panel -1 = all
  void (*set_contrast_ramp)(int front, int back);  // each clamped to 0..255
  float (*set_auto_cycle)(float seconds);           // (engine)
  void (*set_readout)(bool on);                     // (engine)
  void (*set_invert)(bool on);
  void (*set_auto_zero)(bool on);
  int (*test_pattern)(int panel);  // panel < 0 = off; returns the panel shown or -1
  void (*reinit_panels)(void);     // hard reset now
  void (*set_refresh_minutes)(int minutes);  // periodic soft refresh, 0 = off
  bool (*pin_anim_state)(const char* name);  // (engine) idle|accelerate|brake|left|right|bump|auto; may be NULL
  void (*reboot)(void);
  // Non-NULL in a single-effect build (the HUD edition): the effect that is
  // always shown. mode, cycle and anim then explain instead of switching.
  const char* (*fixed_effect)(void);
} app_control_t;

// Reads up to `max` waiting bytes without blocking; returns the count.
typedef size_t (*cli_read_fn)(uint8_t* buf, size_t max);

typedef struct {
  const app_control_t* app;
  // Replies to commands that hold the engine lock are formatted here and sent
  // after the lock is released, so the UART (about 11.5 kB/s) never stalls
  // the render task. The longest reply (`get` on the hud effect) is ~1.1 KB.
  char reply[2048];
  char buf[96];
  size_t len;
  bool last_cr;
  bool overflow;
} serial_cli_t;

void serial_cli_init(serial_cli_t* cli, const app_control_t* app);
// Call often from the main loop; consumes the available input bytes and
// echoes to `io`.
void serial_cli_poll(serial_cli_t* cli, cli_read_fn read, print_t* io);
// Executes one command line; used by serial_cli_poll and by tests.
void serial_cli_execute(serial_cli_t* cli, char* line, print_t* out);

// Strict number parsing: the whole token must be a finite number.
bool serial_cli_parse_float(const char* s, float* out);

#ifdef __cplusplus
}
#endif
