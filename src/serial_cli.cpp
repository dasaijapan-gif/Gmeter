#include "serial_cli.hpp"

#include <cmath>
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
bool parseInt(const char* s, int& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  const long v = strtol(s, &end, 10);
  if (*end != '\0' || v < -100000 || v > 100000) return false;
  out = static_cast<int>(v);
  return true;
}
// Collects output in a fixed buffer; anything past the end is dropped and noted.
class BufferPrint : public Print {
 public:
  BufferPrint(char* buf, size_t cap) : buf_(buf), cap_(cap) {}
  size_t write(uint8_t c) override {
    if (len_ >= cap_) { overflowed_ = true; return 0; }
    buf_[len_++] = static_cast<char>(c);
    return 1;
  }
  size_t write(const uint8_t* data, size_t n) override {
    size_t i = 0;
    while (i < n && write(data[i])) ++i;
    return i;
  }
  size_t length() const { return len_; }
  bool overflowed() const { return overflowed_; }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool overflowed_ = false;
};

// Commands that touch the Engine or an Effect run under the engine lock.
bool needsEngine(const char* cmd) {
  static const char* kCmds[] = {"list", "mode", "get", "set", "cycle", "readout", "anim"};
  for (const char* c : kCmds)
    if (!strcmp(cmd, c)) return true;
  return false;
}
}  // namespace

bool SerialCli::parseFloat(const char* s, float& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  const float v = strtof(s, &end);
  if (end == s || *end != '\0' || !std::isfinite(v)) return false;
  out = v;
  return true;
}

void SerialCli::poll(Stream& io) {
  while (io.available() > 0) {
    const int c = io.read();
    if (c < 0) break;
    // CR, LF or CRLF end a line (an LF straight after a CR is swallowed).
    if (c == '\n' && lastCr_) { lastCr_ = false; continue; }
    lastCr_ = (c == '\r');
    if (c == '\r' || c == '\n') {
      io.print("\r\n");
      buf_[len_] = '\0';
      if (overflow_) io.println("line too long (max 95 characters); ignored");
      else if (len_ > 0) execute(buf_, io);
      len_ = 0;
      overflow_ = false;
      io.print("> ");
      continue;
    }
    if (c == 0x08 || c == 0x7F) {  // backspace / delete
      if (len_ > 0) { --len_; io.print("\b \b"); }
      continue;
    }
    if (c < 0x20 || c > 0x7E) continue;  // ignore other control bytes
    if (len_ + 1 < sizeof(buf_)) {
      buf_[len_++] = static_cast<char>(c);
      io.write(static_cast<uint8_t>(c));
    } else {
      overflow_ = true;
    }
  }
}

void SerialCli::help(Print& out) {
  if (app_.fixedEffect()) out.printf("single-effect build: always shows %s (mode, cycle and anim are disabled)\n", app_.fixedEffect());
  out.println("commands:");
  out.println("  status                 fps, timings, g, imu, heap");
  out.println("  list                   effects and demo scenarios");
  out.println("  mode <name|n|next>     switch effect");
  out.println("  get [param]            show current effect parameters");
  out.println("  set <param> <value>    tune current effect (RAM only; not kept by save)");
  out.println("  src imu|serial|demo [scenario]   g-force source");
  out.println("  g <lat> <vert> <lon>   inject g (src serial), in g, +/-8");
  out.println("  cal fwd <+x|-x|+y|-y|+z|-z>   sensor axis facing the windshield (set before cal rest)");
  out.println("  cal rest               2 s rest capture (car level, parked)");
  out.println("  cal brake              1.5 s capture while braking firmly in a straight line");
  out.println("  cal clear              forget calibration and settings");
  out.println("  zero on|off            slow re-zero while parked");
  out.println("  filter <hz>            low-pass cutoff (3-5 Hz typical)");
  out.println("  contrast <n|all> <0-255> | ramp <front> <back>");
  out.println("  cycle <seconds>        auto-rotate effects (0 = off)");
  out.println("  readout on|off         numeric g overlay");
  out.println("  invert on|off          inverted pixels");
  out.println("  anim <state|auto>      pin the animation clip: idle accelerate brake left right bump");
  out.println("  test [n|off]           light panel n with its index (no arg or off: back to normal)");
  out.println("  reinit [minutes]       hard reset the panels now, or set the periodic refresh (0 = off)");
  out.println("  save                   persist mode, contrast ramp, readout, invert, zero, filter, cycle, refresh, axis, calibration");
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
  // Peek at the command word to decide whether the engine lock is needed.
  char word[12] = {0};
  {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && n + 1 < sizeof(word)) { word[n] = p[n]; ++n; }
  }
  if (!needsEngine(word)) {
    run(line, out);
    return;
  }
  // Format the reply under the lock, send it after.
  BufferPrint reply(reply_, sizeof(reply_));
  app_.lockEngine();
  run(line, reply);
  app_.unlockEngine();
  out.write(reinterpret_cast<const uint8_t*>(reply_), reply.length());
  if (reply.overflowed()) out.println("(reply truncated)");
}

void SerialCli::run(char* line, Print& out) {
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
  if (app_.fixedEffect() && (!strcmp(cmd, "mode") || !strcmp(cmd, "cycle") ||
                             (!strcmp(cmd, "anim") && strcmp(app_.fixedEffect(), "anim")))) {
    out.printf("this firmware always shows %s (single-effect build); flash the standard image to switch effects\n",
               app_.fixedEffect());
    return;
  }
  if (!strcmp(cmd, "mode")) {
    if (!a1) { out.println("usage: mode <name|n|next>"); return; }
    if (!strcmp(a1, "next")) { app_.nextMode(); }
    else if (!app_.setMode(a1)) { out.println("unknown effect (list)"); return; }
    out.printf("mode: %s\n", app_.engine().current()->name());
    return;
  }
  if (!strcmp(cmd, "get")) {
    if (!a1) { listParams(out); return; }
    float v;
    if (app_.engine().current() && app_.engine().current()->params().get(a1, v)) out.printf("%s = %.4f\n", a1, static_cast<double>(v));
    else out.println("unknown parameter (get)");
    return;
  }
  if (!strcmp(cmd, "set")) {
    float v = 0.0f;
    if (!a1 || !a2) { out.println("usage: set <param> <value>"); return; }
    if (!parseFloat(a2, v)) { out.println("bad number"); return; }
    if (app_.engine().setParam(a1, v)) {
      app_.engine().current()->params().get(a1, v);
      out.printf("%s = %.4f\n", a1, static_cast<double>(v));
    } else out.println("unknown parameter (get)");
    return;
  }
  if (!strcmp(cmd, "src")) {
    if (!a1 || !app_.setSource(a1, a2)) out.println("usage: src imu | src serial | src demo [scenario]");
    else out.printf("source: %s%s%s\n", a1, a2 ? " " : "", a2 ? a2 : "");
    return;
  }
  if (!strcmp(cmd, "g")) {
    float lat, vert, lon;
    if (!parseFloat(a1, lat) || !parseFloat(a2, vert) || !parseFloat(a3, lon)) { out.println("usage: g <lat> <vert> <lon>"); return; }
    app_.injectG(lat, vert, lon);
    return;
  }
  if (!strcmp(cmd, "cal")) {
    if (!a1) { out.println("usage: cal fwd <axis> | rest | brake | clear"); return; }
    if (!strcmp(a1, "rest") || !strcmp(a1, "brake")) {
      const char* why = app_.startCalibration(a1);
      if (why) out.printf("cannot start: %s\n", why);
      else out.println(!strcmp(a1, "rest") ? "capturing for 2 s, keep still..." : "capturing for 1.5 s, brake firmly now...");
    } else if (!strcmp(a1, "fwd")) {
      out.println(app_.setForwardAxis(a2) ? "forward axis set; now run cal rest" : "usage: cal fwd +x|-x|+y|-y|+z|-z");
    } else if (!strcmp(a1, "clear")) {
      out.println(app_.clearState() ? "cleared; reboot to apply defaults" : "clear failed");
    } else out.println("usage: cal fwd <axis> | rest | brake | clear");
    return;
  }
  if (!strcmp(cmd, "zero")) {
    bool on;
    if (parseOnOff(a1, on)) { app_.setAutoZero(on); out.printf("auto zero: %s\n", on ? "on" : "off"); }
    else out.println("usage: zero on|off");
    return;
  }
  if (!strcmp(cmd, "filter")) {
    float hz;
    if (parseFloat(a1, hz)) out.printf("filter: %.1f Hz\n", static_cast<double>(app_.setFilterHz(hz)));
    else out.println("usage: filter <hz>");
    return;
  }
  if (!strcmp(cmd, "contrast")) {
    int v;
    if (!a1 || !parseInt(a2, v)) { out.println("usage: contrast <n|all> <0-255>"); return; }
    int panel = -1;
    if (strcmp(a1, "all") && (!parseInt(a1, panel) || panel < 0 || panel >= gm::kD)) { out.println("usage: contrast <n|all> <0-255>"); return; }
    const int applied = app_.setContrast(panel, v);
    if (panel < 0) out.printf("contrast: all panels %d\n", applied);
    else out.printf("contrast: panel %d = %d\n", panel, applied);
    return;
  }
  if (!strcmp(cmd, "ramp")) {
    int f, b;
    if (!parseInt(a1, f) || !parseInt(a2, b)) { out.println("usage: ramp <front> <back>"); return; }
    f = f < 0 ? 0 : (f > 255 ? 255 : f);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    app_.setContrastRamp(f, b);
    out.printf("contrast ramp: front %d, back %d\n", f, b);
    return;
  }
  if (!strcmp(cmd, "cycle")) {
    float s;
    if (!parseFloat(a1, s)) { out.println("usage: cycle <seconds>"); return; }
    const float applied = app_.setAutoCycle(s);
    if (applied > 0.0f) out.printf("auto cycle: every %.0f s\n", static_cast<double>(applied));
    else out.println("auto cycle: off");
    return;
  }
  if (!strcmp(cmd, "readout")) {
    bool on;
    if (parseOnOff(a1, on)) { app_.setReadout(on); out.printf("readout: %s\n", on ? "on" : "off"); }
    else out.println("usage: readout on|off");
    return;
  }
  if (!strcmp(cmd, "invert")) {
    bool on;
    if (parseOnOff(a1, on)) { app_.setInvert(on); out.printf("invert: %s\n", on ? "on" : "off"); }
    else out.println("usage: invert on|off");
    return;
  }
  if (!strcmp(cmd, "anim")) {
    if (!app_.pinAnimState(a1)) out.println("usage: anim idle|accelerate|brake|left|right|bump|auto");
    else out.printf("anim: %s\n", (!a1 || !strcmp(a1, "auto")) ? "follows g-force" : a1);
    return;
  }
  if (!strcmp(cmd, "test")) {
    int n = -1;
    if (a1 && strcmp(a1, "off") && !parseInt(a1, n)) { out.println("usage: test [panel|off]"); return; }
    const int shown = app_.testPattern(n);
    if (shown < 0) out.println("test pattern: off");
    else out.printf("test pattern: panel %d (type test to go back)\n", shown);
    return;
  }
  if (!strcmp(cmd, "reinit")) {
    if (!a1) { app_.reinitPanels(); out.println("panels reset and re-initialised"); return; }
    int m;
    if (!parseInt(a1, m) || m < 0 || m > 255) { out.println("usage: reinit [minutes 0-255]"); return; }
    app_.setRefreshMinutes(m);
    out.printf("periodic refresh: %s\n", m ? "on" : "off");
    return;
  }
  if (!strcmp(cmd, "save")) { out.println(app_.saveState() ? "saved" : "save failed"); return; }
  if (!strcmp(cmd, "reboot")) { app_.reboot(); return; }
  out.println("unknown command (help)");
}
