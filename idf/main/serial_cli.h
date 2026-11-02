// Line-oriented serial command interface for bench testing and tuning.
// Type `help` in the serial monitor (115200 baud). Accepts CR, LF or CRLF line
// endings, echoes typed characters and handles backspace.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "console_io.h"
#include "gm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the CLI asks the application to do; implemented in app_main.c.
// Functions marked (engine) touch the engine or an effect; the CLI calls them
// between app_lock_engine() and app_unlock_engine() so they never race the
// render task.
void app_lock_engine(void);
void app_unlock_engine(void);
gm_engine_t* app_engine(void);  // (engine)
void app_print_status(print_t* out);
void app_print_scenarios(print_t* out);
bool app_set_mode(const char* name_or_index);            // (engine)
bool app_set_source(const char* name, const char* arg);  // imu | serial | demo [scenario]; arg may be NULL
void app_inject_g(float lat, float vert, float lon);
// Returns NULL when the capture started, otherwise the reason it did not.
const char* app_start_calibration(const char* which);  // rest | brake
bool app_set_forward_axis(const char* axis);           // +x -x +y -y +z -z
bool app_save_state(void);
// Setters that clamp their input return the value actually applied.
float app_set_filter_hz(float hz);
int app_set_contrast(int panel, int value);       // panel -1 = all
void app_set_contrast_ramp(int front, int back);  // each clamped to 0..255
float app_set_auto_cycle(float seconds);          // (engine)
void app_set_readout(bool on);                    // (engine)
void app_set_invert(bool on);
void app_set_auto_zero(bool on);
int app_test_pattern(int panel);            // panel < 0 = off; returns the panel shown or -1
void app_reinit_panels(void);               // hard reset now
void app_set_refresh_minutes(int minutes);  // periodic soft refresh, 0 = off
bool app_pin_anim_state(const char* name);  // (engine) idle|accelerate|brake|left|right|bump|auto; may be NULL
void app_reboot(void);
// Non-NULL in a single-effect build (the HUD edition): the effect that is
// always shown. mode, cycle and anim then explain instead of switching.
const char* app_fixed_effect(void);

// The command line's state. A zeroed one (static storage) is ready to use.
typedef struct {
  // Replies to commands that hold the engine lock are formatted here and sent
  // after the lock is released, so the UART (about 11.5 kB/s) never stalls
  // the render task. The longest reply (`get` on the hud effect) is ~1.1 KB.
  char reply[2048];
  char buf[96];
  size_t len;
  bool last_cr;
  bool overflow;
} serial_cli_t;

// Call often from the main loop: reads the console's waiting input bytes,
// echoes them and runs each complete line.
void serial_cli_poll(serial_cli_t* cli);

#ifdef __cplusplus
}
#endif
