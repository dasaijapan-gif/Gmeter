#include "serial_cli.hpp"

#include <cstdlib>
#include <cstring>

#include "gmeter/scenario.hpp"

namespace {
char* nextToken(char*& cursor) {
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  if (!*cursor) return nullptr;
  char* start = cursor;
  while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
  if (*cursor) *cursor++ = '\0';
  return start;
}
bool parseOnOff(const char* s, bool& out) {
  if (!s) return false;
  if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) { out = true; return true; }
  if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) { out = false; return true; }
  return false;
}
}  // namespace

void SerialCli::poll(Stream& io) {
  while (io.available() > 0) {
    const int c = io.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      buf_[len_] = '\0';
      if (len_ > 0) execute(buf_, io);
      len_ = 0;
      io.print("> ");
      continue;
    }
    if (len_ + 1 < sizeof(buf_)) buf_[len_++] = static_cast<char>(c);
  }
}

void SerialCli::help(Print& out) {
  out.println("commands:");
  out.println("  status                 fps, timings, g, imu, heap");
  out.println("  list                   effects and demo scenarios");
  out.println("  mode <name|n|next>     switch effect");
  out.println("  get [param]            show current effect parameters");
  out.println("  set <param> <value>    tune current effect");
  out.println("  src imu|serial|demo [scenario]   g-force source");
  out.println("  g <lat> <vert> <lon>   inject g (src serial), in g");
  out.println("  cal rest               2 s rest capture (car level, parked)");
  out.println("  cal brake              1.5 s capture while braking straight");
  out.println("  cal fwd <+x|-x|+y|-y|+z|-z>   sensor axis facing the car's front");
  out.println("  cal clear              forget calibration and settings");
  out.println("  zero on|off            slow re-zero while parked");
  out.println("  filter <hz>            low-pass cutoff (3-5 Hz typical)");
  out.println("  contrast <n|all> <0-255> | ramp <front> <back>");
  out.println("  cycle <seconds>        auto-rotate effects (0 = off)");
  out.println("  readout on|off         numeric g overlay");
  out.println("  invert on|off          inverted pixels");
  out.println("  test [n]               light panel n with its index (no arg: back to normal)");
  out.println("  reinit                 re-send panel init");
  out.println("  save                   persist settings + calibration");
  out.println("  reboot");
}

void SerialCli::listParams(Print& out) {
  gm::Effect* e = app_.engine().current();
  if (!e) return;
  const gm::ParamTable& p = e->params();
  out.printf("[%s] parameters:\n", e->name());
  for (int i = 0; i < p.count(); ++i) {
    const gm::Param& q = p.at(i);
    out.printf("  %-12s %9.3f  [%g..%g]  %s\n", q.name, static_cast<double>(*q.value), static_cast<double>(q.min),
               static_cast<double>(q.max), q.help);
  }
}

void SerialCli::execute(char* line, Print& out) {
  char* cursor = line;
  const char* cmd = nextToken(cursor);
  if (!cmd) return;
  const char* a1 = nextToken(cursor);
  const char* a2 = nextToken(cursor);
  const char* a3 = nextToken(cursor);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { help(out); return; }
  if (!strcmp(cmd, "status")) { app_.printStatus(out); return; }
  if (!strcmp(cmd, "list")) {
    gm::Engine& eng = app_.engine();
    out.println("effects:");
    for (int i = 0; i < eng.effectCount(); ++i)
      out.printf("  %d %s%s\n", i, eng.effect(i)->name(), i == eng.currentIndex() ? "  (active)" : "");
    app_.printScenarios(out);
    return;
  }
  if (!strcmp(cmd, "mode")) {
    if (!a1) { out.println("usage: mode <name|n|next>"); return; }
    if (!strcmp(a1, "next")) { app_.nextMode(); }
    else if (!app_.setMode(a1)) { out.println("unknown effect"); return; }
    out.printf("mode: %s\n", app_.engine().current()->name());
    return;
  }
  if (!strcmp(cmd, "get")) {
    if (!a1) { listParams(out); return; }
    float v;
    if (app_.engine().current() && app_.engine().current()->params().get(a1, v)) out.printf("%s = %.4f\n", a1, static_cast<double>(v));
    else out.println("unknown parameter");
    return;
  }
  if (!strcmp(cmd, "set")) {
    if (!a1 || !a2) { out.println("usage: set <param> <value>"); return; }
    if (app_.engine().setParam(a1, strtof(a2, nullptr))) {
      float v; app_.engine().current()->params().get(a1, v);
      out.printf("%s = %.4f\n", a1, static_cast<double>(v));
    } else out.println("unknown parameter (try: get)");
    return;
  }
  if (!strcmp(cmd, "src")) {
    if (!a1 || !app_.setSource(a1, a2)) out.println("usage: src imu | src serial | src demo [scenario]");
    return;
  }
  if (!strcmp(cmd, "g")) {
    if (!a1 || !a2 || !a3) { out.println("usage: g <lat> <vert> <lon>"); return; }
    app_.injectG(strtof(a1, nullptr), strtof(a2, nullptr), strtof(a3, nullptr));
    return;
  }
  if (!strcmp(cmd, "cal")) {
    if (!a1) { out.println("usage: cal rest|brake|fwd <axis>|clear"); return; }
    if (!strcmp(a1, "rest") || !strcmp(a1, "brake")) {
      out.println(app_.startCalibration(a1) ? "capturing..." : "cannot start (imu missing or busy)");
    } else if (!strcmp(a1, "fwd")) {
      out.println(app_.setForwardAxis(a2) ? "forward axis set; run cal rest" : "usage: cal fwd +x|-x|+y|-y|+z|-z");
    } else if (!strcmp(a1, "clear")) {
      out.println(app_.clearState() ? "cleared; reboot to apply defaults" : "clear failed");
    } else out.println("usage: cal rest|brake|fwd <axis>|clear");
    return;
  }
  if (!strcmp(cmd, "zero")) { bool on; if (parseOnOff(a1, on)) app_.setAutoZero(on); else out.println("usage: zero on|off"); return; }
  if (!strcmp(cmd, "filter")) { if (a1) app_.setFilterHz(strtof(a1, nullptr)); else out.println("usage: filter <hz>"); return; }
  if (!strcmp(cmd, "contrast")) {
    if (!a1 || !a2) { out.println("usage: contrast <n|all> <0-255>"); return; }
    app_.setContrast(!strcmp(a1, "all") ? -1 : atoi(a1), atoi(a2));
    return;
  }
  if (!strcmp(cmd, "ramp")) {
    if (!a1 || !a2) { out.println("usage: ramp <front> <back>"); return; }
    app_.setContrastRamp(atoi(a1), atoi(a2));
    return;
  }
  if (!strcmp(cmd, "cycle")) { if (a1) app_.setAutoCycle(strtof(a1, nullptr)); else out.println("usage: cycle <seconds>"); return; }
  if (!strcmp(cmd, "readout")) { bool on; if (parseOnOff(a1, on)) app_.setReadout(on); else out.println("usage: readout on|off"); return; }
  if (!strcmp(cmd, "invert")) { bool on; if (parseOnOff(a1, on)) app_.setInvert(on); else out.println("usage: invert on|off"); return; }
  if (!strcmp(cmd, "test")) { app_.testPattern(a1 ? atoi(a1) : -1); return; }
  if (!strcmp(cmd, "reinit")) { app_.reinitPanels(); out.println("panels re-initialised"); return; }
  if (!strcmp(cmd, "save")) { out.println(app_.saveState() ? "saved" : "save failed"); return; }
  if (!strcmp(cmd, "reboot")) { app_.reboot(); return; }
  out.println("unknown command (help)");
}
