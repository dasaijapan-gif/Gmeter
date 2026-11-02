#include "serial_cli.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gm_config.h"
#include "gm_math.h"
#include "storage.h"

static char* next_token(char** cursor) {
  char* c = *cursor;
  while (*c == ' ' || *c == '\t') ++c;
  if (!*c) {
    *cursor = c;
    return NULL;
  }
  char* start = c;
  while (*c && *c != ' ' && *c != '\t') ++c;
  if (*c) *c++ = '\0';
  *cursor = c;
  return start;
}

static bool parse_on_off(const char* s, bool* out) {
  if (!s) return false;
  if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) {
    *out = true;
    return true;
  }
  if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) {
    *out = false;
    return true;
  }
  return false;
}

static bool parse_int(const char* s, int* out) {
  if (!s || !*s) return false;
  char* end = NULL;
  const long v = strtol(s, &end, 10);
  if (*end != '\0' || v < -100000 || v > 100000) return false;
  *out = (int)v;
  return true;
}

// Collects output in a fixed buffer; anything past the end is dropped and noted.
typedef struct {
  print_t base;
  char* buf;
  size_t cap;
  size_t len;
  bool overflowed;
} buffer_print_t;

static size_t buffer_print_write(print_t* self, const char* data, size_t n) {
  buffer_print_t* b = (buffer_print_t*)self;
  size_t i = 0;
  while (i < n) {
    if (b->len >= b->cap) {
      b->overflowed = true;
      break;
    }
    b->buf[b->len++] = data[i++];
  }
  return i;
}

static void buffer_print_init(buffer_print_t* b, char* buf, size_t cap) {
  b->base.write = buffer_print_write;
  b->buf = buf;
  b->cap = cap;
  b->len = 0;
  b->overflowed = false;
}

// Commands that touch the engine or an effect run under the engine lock.
static bool needs_engine(const char* cmd) {
  static const char* const k_cmds[] = {"list", "mode", "get", "set", "cycle", "readout", "anim"};
  for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); ++i)
    if (!strcmp(cmd, k_cmds[i])) return true;
  return false;
}

// Strict number parsing: the whole token must be a finite number.
static bool parse_float(const char* s, float* out) {
  if (!s || !*s) return false;
  char* end = NULL;
  const float v = strtof(s, &end);
  if (end == s || *end != '\0' || !isfinite(v)) return false;
  *out = v;
  return true;
}

static void execute(serial_cli_t* cli, char* line, print_t* out);

void serial_cli_poll(serial_cli_t* cli) {
  print_t* io = conio_out();
  uint8_t chunk[64];
  size_t got;
  while ((got = conio_read(chunk, sizeof(chunk))) > 0) {
    for (size_t k = 0; k < got; ++k) {
      const int c = chunk[k];
      // CR, LF or CRLF end a line (an LF straight after a CR is swallowed).
      if (c == '\n' && cli->last_cr) {
        cli->last_cr = false;
        continue;
      }
      cli->last_cr = (c == '\r');
      if (c == '\r' || c == '\n') {
        print_str(io, "\r\n");
        cli->buf[cli->len] = '\0';
        if (cli->overflow) print_ln(io, "line too long (max 95 characters); ignored");
        else if (cli->len > 0) execute(cli, cli->buf, io);
        cli->len = 0;
        cli->overflow = false;
        print_str(io, "> ");
        continue;
      }
      if (c == 0x08 || c == 0x7F) {  // backspace / delete
        if (cli->len > 0) {
          --cli->len;
          print_str(io, "\b \b");
        }
        continue;
      }
      if (c < 0x20 || c > 0x7E) continue;  // ignore other control bytes
      if (cli->len + 1 < sizeof(cli->buf)) {
        cli->buf[cli->len++] = (char)c;
        const char ch = (char)c;
        print_write(io, &ch, 1);
      } else {
        cli->overflow = true;
      }
    }
  }
}

static void help(print_t* out) {
  const char* fixed = app_fixed_effect();
  if (fixed) print_fmt(out, "single-effect build: always shows %s (mode, cycle and anim are disabled)\n", fixed);
  print_ln(out, "commands:");
  print_ln(out, "  status                 fps, timings, g, imu, heap");
  print_ln(out, "  list                   effects and demo scenarios");
  print_ln(out, "  mode <name|n|next>     switch effect");
  print_ln(out, "  get [param]            show current effect parameters");
  print_ln(out, "  set <param> <value>    tune current effect (RAM only; not kept by save)");
  print_ln(out, "  src imu|serial|demo [scenario]   g-force source");
  print_ln(out, "  g <lat> <vert> <lon>   inject g (src serial), in g, +/-8");
  print_ln(out, "  cal fwd <+x|-x|+y|-y|+z|-z>   sensor axis facing the windshield (set before cal rest)");
  print_ln(out, "  cal rest               2 s rest capture (car level, parked)");
  print_ln(out, "  cal brake              1.5 s capture while braking firmly in a straight line");
  print_ln(out, "  cal clear              forget calibration and settings");
  print_ln(out, "  zero on|off            slow re-zero while parked");
  print_ln(out, "  filter <hz>            low-pass cutoff (3-5 Hz typical)");
  print_ln(out, "  contrast <n|all> <0-255> | ramp <front> <back>");
  print_ln(out, "  cycle <seconds>        auto-rotate effects (0 = off)");
  print_ln(out, "  readout on|off         numeric g overlay");
  print_ln(out, "  invert on|off          inverted pixels");
  print_ln(out, "  anim <state|auto>      pin the animation clip: idle accelerate brake left right bump");
  print_ln(out, "  test [n|off]           light panel n with its index (no arg or off: back to normal)");
  print_ln(out, "  reinit [minutes]       hard reset the panels now, or set the periodic refresh (0 = off)");
  print_ln(out,
           "  save                   persist mode, contrast ramp, readout, invert, zero, filter, cycle, refresh, axis, "
           "calibration");
  print_ln(out, "  reboot");
}

static void list_params(print_t* out) {
  gm_effect_t* e = gm_engine_current(app_engine());
  if (!e) return;
  const gm_param_table_t* p = gm_effect_params(e);
  print_fmt(out, "[%s] parameters:\n", gm_effect_name(e));
  for (int i = 0; i < gm_params_count(p); ++i) {
    const gm_param_t* q = gm_params_at(p, i);
    print_fmt(out, "  %-12s %9.3f  [%g..%g]  %s\n", q->name, (double)*q->value, (double)q->min, (double)q->max,
              q->help);
  }
}

// `<name> on|off`: applies the switch and confirms it as "<label>: on|off".
static void on_off(const char* name, const char* label, void (*set)(bool), const char* a1, print_t* out) {
  bool on;
  if (!parse_on_off(a1, &on)) {
    print_fmt(out, "usage: %s on|off\r\n", name);
    return;
  }
  set(on);
  print_fmt(out, "%s: %s\n", label, on ? "on" : "off");
}

// Runs one command: cmd and up to three arguments (NULL when absent).
static void run(const char* cmd, const char* a1, const char* a2, const char* a3, print_t* out) {
  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    help(out);
    return;
  }
  if (!strcmp(cmd, "status")) {
    app_print_status(out);
    return;
  }
  if (!strcmp(cmd, "list")) {
    gm_engine_t* eng = app_engine();
    print_ln(out, "effects:");
    for (int i = 0; i < gm_engine_effect_count(eng); ++i)
      print_fmt(out, "  %d %s%s\n", i, gm_effect_name(gm_engine_effect(eng, i)),
                i == gm_engine_current_index(eng) ? "  (active)" : "");
    app_print_scenarios(out);
    return;
  }
  const char* fixed = app_fixed_effect();
  if (fixed && (!strcmp(cmd, "mode") || !strcmp(cmd, "cycle") || (!strcmp(cmd, "anim") && strcmp(fixed, "anim")))) {
    print_fmt(out, "this firmware always shows %s (single-effect build); flash the standard image to switch effects\n",
              fixed);
    return;
  }
  if (!strcmp(cmd, "mode")) {
    if (!a1) {
      print_ln(out, "usage: mode <name|n|next>");
      return;
    }
    if (!strcmp(a1, "next")) {
      gm_engine_next_effect(app_engine());
    } else if (!app_set_mode(a1)) {
      print_ln(out, "unknown effect (list)");
      return;
    }
    print_fmt(out, "mode: %s\n", gm_effect_name(gm_engine_current(app_engine())));
    return;
  }
  if (!strcmp(cmd, "get")) {
    if (!a1) {
      list_params(out);
      return;
    }
    float v;
    gm_effect_t* cur = gm_engine_current(app_engine());
    if (cur && gm_params_get(gm_effect_params(cur), a1, &v)) print_fmt(out, "%s = %.4f\n", a1, (double)v);
    else print_ln(out, "unknown parameter (get)");
    return;
  }
  if (!strcmp(cmd, "set")) {
    float v = 0.0f;
    if (!a1 || !a2) {
      print_ln(out, "usage: set <param> <value>");
      return;
    }
    if (!parse_float(a2, &v)) {
      print_ln(out, "bad number");
      return;
    }
    if (gm_engine_set_param(app_engine(), a1, v)) {
      gm_params_get(gm_effect_params(gm_engine_current(app_engine())), a1, &v);
      print_fmt(out, "%s = %.4f\n", a1, (double)v);
    } else {
      print_ln(out, "unknown parameter (get)");
    }
    return;
  }
  if (!strcmp(cmd, "src")) {
    if (!a1 || !app_set_source(a1, a2)) print_ln(out, "usage: src imu | src serial | src demo [scenario]");
    else print_fmt(out, "source: %s%s%s\n", a1, a2 ? " " : "", a2 ? a2 : "");
    return;
  }
  if (!strcmp(cmd, "g")) {
    float lat, vert, lon;
    if (!parse_float(a1, &lat) || !parse_float(a2, &vert) || !parse_float(a3, &lon)) {
      print_ln(out, "usage: g <lat> <vert> <lon>");
      return;
    }
    app_inject_g(lat, vert, lon);
    return;
  }
  if (!strcmp(cmd, "cal")) {
    if (a1 && (!strcmp(a1, "rest") || !strcmp(a1, "brake"))) {
      const char* why = app_start_calibration(a1);
      if (why) {
        print_fmt(out, "cannot start: %s\n", why);
      } else {
        print_ln(out, !strcmp(a1, "rest") ? "capturing for 2 s, keep still..."
                                          : "capturing for 1.5 s, brake firmly now...");
      }
    } else if (a1 && !strcmp(a1, "fwd")) {
      print_ln(out,
               app_set_forward_axis(a2) ? "forward axis set; now run cal rest" : "usage: cal fwd +x|-x|+y|-y|+z|-z");
    } else if (a1 && !strcmp(a1, "clear")) {
      print_ln(out, storage_clear() ? "cleared; reboot to apply defaults" : "clear failed");
    } else {
      print_ln(out, "usage: cal fwd <axis> | rest | brake | clear");
    }
    return;
  }
  if (!strcmp(cmd, "zero")) {
    on_off("zero", "auto zero", app_set_auto_zero, a1, out);
    return;
  }
  if (!strcmp(cmd, "filter")) {
    float hz;
    if (parse_float(a1, &hz)) print_fmt(out, "filter: %.1f Hz\n", (double)app_set_filter_hz(hz));
    else print_ln(out, "usage: filter <hz>");
    return;
  }
  if (!strcmp(cmd, "contrast")) {
    int v, panel = -1;
    if (!a1 || !parse_int(a2, &v) || (strcmp(a1, "all") && (!parse_int(a1, &panel) || panel < 0 || panel >= GM_D))) {
      print_ln(out, "usage: contrast <n|all> <0-255>");
      return;
    }
    const int applied = app_set_contrast(panel, v);
    if (panel < 0) print_fmt(out, "contrast: all panels %d\n", applied);
    else print_fmt(out, "contrast: panel %d = %d\n", panel, applied);
    return;
  }
  if (!strcmp(cmd, "ramp")) {
    int f, b;
    if (!parse_int(a1, &f) || !parse_int(a2, &b)) {
      print_ln(out, "usage: ramp <front> <back>");
      return;
    }
    f = gm_clampi(f, 0, 255);
    b = gm_clampi(b, 0, 255);
    app_set_contrast_ramp(f, b);
    print_fmt(out, "contrast ramp: front %d, back %d\n", f, b);
    return;
  }
  if (!strcmp(cmd, "cycle")) {
    float s;
    if (!parse_float(a1, &s)) {
      print_ln(out, "usage: cycle <seconds>");
      return;
    }
    const float applied = app_set_auto_cycle(s);
    if (applied > 0.0f) print_fmt(out, "auto cycle: every %.0f s\n", (double)applied);
    else print_ln(out, "auto cycle: off");
    return;
  }
  if (!strcmp(cmd, "readout")) {
    on_off("readout", "readout", app_set_readout, a1, out);
    return;
  }
  if (!strcmp(cmd, "invert")) {
    on_off("invert", "invert", app_set_invert, a1, out);
    return;
  }
  if (!strcmp(cmd, "anim")) {
    if (!app_pin_anim_state(a1)) print_ln(out, "usage: anim idle|accelerate|brake|left|right|bump|auto");
    else print_fmt(out, "anim: %s\n", (!a1 || !strcmp(a1, "auto")) ? "follows g-force" : a1);
    return;
  }
  if (!strcmp(cmd, "test")) {
    int n = -1;
    if (a1 && strcmp(a1, "off") && !parse_int(a1, &n)) {
      print_ln(out, "usage: test [panel|off]");
      return;
    }
    const int shown = app_test_pattern(n);
    if (shown < 0) print_ln(out, "test pattern: off");
    else print_fmt(out, "test pattern: panel %d (type test to go back)\n", shown);
    return;
  }
  if (!strcmp(cmd, "reinit")) {
    if (!a1) {
      app_reinit_panels();
      print_ln(out, "panels reset and re-initialised");
      return;
    }
    int m;
    if (!parse_int(a1, &m) || m < 0 || m > 255) {
      print_ln(out, "usage: reinit [minutes 0-255]");
      return;
    }
    app_set_refresh_minutes(m);
    print_fmt(out, "periodic refresh: %s\n", m ? "on" : "off");
    return;
  }
  if (!strcmp(cmd, "save")) {
    print_ln(out, app_save_state() ? "saved" : "save failed");
    return;
  }
  if (!strcmp(cmd, "reboot")) {
    app_reboot();
    return;
  }
  print_ln(out, "unknown command (help)");
}

// Runs one command line, under the engine lock if the command needs it.
static void execute(serial_cli_t* cli, char* line, print_t* out) {
  const char* t[4];  // the command and up to three arguments
  for (int i = 0; i < 4; ++i) t[i] = next_token(&line);
  if (!t[0]) return;
  if (!needs_engine(t[0])) {
    run(t[0], t[1], t[2], t[3], out);
    return;
  }
  // Format the reply under the lock, send it after.
  buffer_print_t reply;
  buffer_print_init(&reply, cli->reply, sizeof(cli->reply));
  app_lock_engine();
  run(t[0], t[1], t[2], t[3], &reply.base);
  app_unlock_engine();
  print_write(out, cli->reply, reply.len);
  if (reply.overflowed) print_ln(out, "(reply truncated)");
}
