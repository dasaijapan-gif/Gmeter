// Line-oriented serial command interface for bench testing and tuning.
// Type `help` in the serial monitor (115200 baud). Accepts CR, LF or CRLF line
// endings, echoes typed characters and handles backspace.
#pragma once

#include <Arduino.h>

#include "gmeter/engine.hpp"

// What the CLI can ask the application to do; implemented in main.cpp.
// Methods marked (engine) touch the Engine or an Effect; the CLI calls them
// between lockEngine()/unlockEngine() so they never race the render task.
class AppControl {
 public:
  virtual ~AppControl() = default;
  virtual void lockEngine() = 0;
  virtual void unlockEngine() = 0;
  virtual gm::Engine& engine() = 0;                               // (engine)
  virtual void printStatus(Print& out) = 0;
  virtual void printScenarios(Print& out) = 0;
  virtual bool setMode(const char* nameOrIndex) = 0;              // (engine)
  virtual void nextMode() = 0;                                    // (engine)
  virtual bool setSource(const char* name, const char* arg) = 0;  // imu | serial | demo [scenario]
  virtual void injectG(float lat, float vert, float lon) = 0;
  // Returns nullptr when the capture started, otherwise the reason it did not.
  virtual const char* startCalibration(const char* which) = 0;    // rest | brake
  virtual bool setForwardAxis(const char* axis) = 0;              // +x -x +y -y +z -z
  virtual bool saveState() = 0;
  virtual bool clearState() = 0;
  // Setters that clamp their input return the value actually applied.
  virtual float setFilterHz(float hz) = 0;
  virtual int setContrast(int panel, int value) = 0;              // panel -1 = all
  virtual void setContrastRamp(int front, int back) = 0;         // each clamped to 0..255
  virtual float setAutoCycle(float seconds) = 0;                  // (engine)
  virtual void setReadout(bool on) = 0;                           // (engine)
  virtual void setInvert(bool on) = 0;
  virtual void setAutoZero(bool on) = 0;
  virtual int testPattern(int panel) = 0;                         // panel < 0 = off; returns the panel shown or -1
  virtual void reinitPanels() = 0;                                // hard reset now
  virtual void setRefreshMinutes(int minutes) = 0;                // periodic soft refresh, 0 = off
  virtual bool pinAnimState(const char* name) = 0;                // (engine) idle|accelerate|brake|left|right|bump|auto
  virtual void reboot() = 0;
  // Non-null in a single-effect build (e.g. the HUD edition): the effect that
  // is always shown. mode, cycle and anim then explain instead of switching.
  virtual const char* fixedEffect() const = 0;
};

class SerialCli {
 public:
  explicit SerialCli(AppControl& app) : app_(app) {}
  // Call often from loop(); consumes available serial bytes.
  void poll(Stream& io);
  // Executes one command line; used by poll() and by tests.
  void execute(char* line, Print& out);

  // Strict number parsing: the whole token must be a finite number.
  static bool parseFloat(const char* s, float& out);

 private:
  AppControl& app_;
  // Replies to commands that hold the engine lock are formatted here and sent
  // after the lock is released, so the UART (about 11.5 kB/s) never stalls
  // the render task. The longest reply (`get` on the hud effect) is ~1.1 KB.
  char reply_[2048];
  char buf_[96];
  size_t len_ = 0;
  bool lastCr_ = false;
  bool overflow_ = false;

  void help(Print& out);
  void listParams(Print& out);
  void run(char* line, Print& out);
};
