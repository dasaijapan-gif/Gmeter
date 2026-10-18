// Driver for a stack of SSD1309/SSD1306 128xN monochrome panels sharing one
// SPI bus (SCK, MOSI, D/C, RES) with one chip-select GPIO per panel.
//
// Frames come straight from gm::VoxelFrame, whose per-layer byte layout is
// the controller's page-major GDDRAM format, so a layer upload is one window
// command plus one contiguous write. At 10 MHz, 8 panels of 7 pages take
// about 6 ms (around 150 fps ceiling); the transfer is a blocking CPU copy
// (Arduino SPI has no DMA queue), which is why main.cpp runs it on its own
// task on the other core.
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "board_config.hpp"
#include "gmeter/voxel_frame.hpp"

class PanelStack {
 public:
  static constexpr int kPanels = gm::kD;

  // Configures GPIOs, pulses reset, initialises every panel and clears them.
  void begin();
  // Re-sends the init sequence (write-only bus: a panel scrambled by EMI
  // cannot report it, so call this periodically or on demand).
  void reinit();

  // Uploads all layers. Blocks for the SPI transfer.
  void writeFrame(const gm::VoxelFrame& frame);
  void clear();

  // 0..255 segment current. Rear layers usually need more: see setContrastRamp.
  void setContrast(int panel, uint8_t value);
  void setContrastRamp(uint8_t front, uint8_t back);
  void setInverted(bool on);
  void setDisplayOn(bool on);
  // Lights panel `panel` entirely (all others dark) and shows its index on it.
  void testPattern(int panel);

  uint32_t lastTransferUs() const { return lastTransferUs_; }
  int panelForLayer(int layer) const { return GM_PANELS_REVERSED ? (kPanels - 1 - layer) : layer; }

 private:
  SPIClass* spi_ = nullptr;
  uint8_t contrast_[kPanels];
  bool inverted_ = false;
  uint32_t lastTransferUs_ = 0;

  void initPanel(int panel);
  void command(int panel, const uint8_t* bytes, size_t n);
  void command1(int panel, uint8_t b) { command(panel, &b, 1); }
  void data(int panel, const uint8_t* bytes, size_t n);
  void setWindow(int panel, uint8_t lastPage);
};
