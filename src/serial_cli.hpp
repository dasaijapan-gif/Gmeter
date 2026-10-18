// Line-oriented serial command interface for bench testing and tuning.
// Type `help` in the serial monitor (115200 baud).
#pragma once

#include <Arduino.h>

#include "gmeter/engine.hpp"

// What the CLI can ask the application to do; implemented in main.cpp.
class AppControl {
 public:
  virtual ~AppControl() = default;
  virtual gm::Engine& engine() = 0;
  virtual void printStatus(Print& out) = 0;
  virtual void printScenarios(Print& out) = 0;
  virtual bool setMode(const char* nameOrIndex) = 0;
  virtual void nextMode() = 0;
  virtual bool setSource(const char* name, const char* arg) = 0;  // imu | serial | demo [scenario]
  virtual void injectG(float lat, float vert, float lon) = 0;
  virtual bool startCalibration(const char* which) = 0;           // rest | brake
  virtual bool setForwardAxis(const char* axis) = 0;              // +x -x +y -y +z -z
  virtual bool saveState() = 0;
  virtual bool clearState() = 0;
  virtual void setFilterHz(float hz) = 0;
  virtual void setContrast(int panel, int value) = 0;             // panel -1 = all
  virtual void setContrastRamp(int front, int back) = 0;
  virtual void setAutoCycle(float seconds) = 0;
  virtual void setReadout(bool on) = 0;
  virtual void setInvert(bool on) = 0;
  virtual void setAutoZero(bool on) = 0;
  virtual void testPattern(int panel) = 0;
  virtual void reinitPanels() = 0;
  virtual void reboot() = 0;
};

class SerialCli {
 public:
  explicit SerialCli(AppControl& app) : app_(app) {}
  // Call often from loop(); consumes available serial bytes.
  void poll(Stream& io);
  // Executes one command line; used by poll() and by tests.
  void execute(char* line, Print& out);

 private:
  AppControl& app_;
  char buf_[96];
  size_t len_ = 0;

  void help(Print& out);
  void listParams(Print& out);
};
