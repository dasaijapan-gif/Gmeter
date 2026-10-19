#include "display_ssd1309.hpp"

#include <esp_timer.h>

#include <memory>
#include <new>

#include "board_config.hpp"

namespace {

using gm::kBytesPerLayer;
using gm::kPages;

inline void csLow(int panel) { digitalWrite(board::kOledCs[panel], LOW); }
inline void csHigh(int panel) { digitalWrite(board::kOledCs[panel], HIGH); }

// Init sequences (sent with D/C low). Contrast (0x81) and remap (0xA0/0xA1,
// 0xC0/0xC8) are patched per panel at runtime.
#if GM_PANEL_MODEL == 0
// WiseChip UG-2856KLBAG01 (128x56 transparent, SSD1309): the panel maker's
// sequence plus horizontal addressing. VCC (12 V) is supplied by the breakout.
const uint8_t kInit[] = {
    0xFD, 0x12,  // command unlock
    0xAE,        // display off
    0xD5, 0xA0,  // clock divide 1, oscillator 0xA (~116 Hz frame rate at 56 MUX)
    0xA8, 0x37,  // multiplex ratio 56
    0xD3, 0x00,  // display offset 0
    0x40,        // start line 0
    0x20, 0x00,  // horizontal addressing mode
    0xA1,        // segment remap (patched)
    0xC8,        // COM scan direction (patched)
    0xDA, 0x12,  // alternative COM pin configuration
    0x81, 0x8F,  // contrast (patched)
    0xD9, 0x25,  // pre-charge
    0xDB, 0x34,  // VCOMH deselect level 0.78 VCC
    0xA4,        // follow RAM
    0xA6,        // non-inverted
};
#elif GM_PANEL_MODEL == 1
// Generic 128x64 SSD1309 module (u8g2 "noname" values).
const uint8_t kInit[] = {
    0xFD, 0x12, 0xAE, 0xD5, 0xA0, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x20, 0x00,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x6F, 0xD9, 0xD3, 0xDB, 0x20, 0xA4, 0xA6,
};
#else
// 128x64 SSD1306 with internal charge pump (3.3 V supply).
const uint8_t kInit[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E,
};
#endif

}  // namespace

void PanelStack::begin() {
  for (int i = 0; i < kPanels; ++i) {
    pinMode(board::kOledCs[i], OUTPUT);
    csHigh(i);
    contrast_[i] = 0x8F;
  }
  pinMode(board::kOledDc, OUTPUT);
  pinMode(board::kOledReset, OUTPUT);
  digitalWrite(board::kOledDc, LOW);

#if defined(GM_BOARD_ESP32_S3)
  static SPIClass spiBus(FSPI);
#else
  static SPIClass spiBus(VSPI);
#endif
  spi_ = &spiBus;
  spi_->begin(board::kSpiSck, -1, board::kSpiMosi, -1);
  setContrastRamp(0x50, 0xFF);
  reinit();
}

void PanelStack::reinit() {
  // Shared reset: low >= 3 us after VDD is stable.
  digitalWrite(board::kOledReset, HIGH);
  delay(5);
  digitalWrite(board::kOledReset, LOW);
  delay(5);
  digitalWrite(board::kOledReset, HIGH);
  delay(10);
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int p = 0; p < kPanels; ++p) initPanel(p);
  spi_->endTransaction();
  delay(100);  // panel VCC settle before turning pixels on
  setDisplayOn(true);
}

// Copies the init table for one panel, patching its contrast, orientation and
// inversion. Every command's argument bytes are skipped explicitly so a
// parameter that happens to equal an opcode is never rewritten.
size_t PanelStack::buildInit(int panel, uint8_t* out, bool includeDisplayOff) const {
  const bool flipped = (GM_PANEL_FLIP_MASK >> panel) & 1;
  size_t n = 0;
  for (size_t i = 0; i < sizeof(kInit); ++i) {
    const uint8_t op = kInit[i];
    // Argument counts for the opcodes used in the tables.
    int args = 0;
    switch (op) {
      case 0xFD: case 0xD5: case 0xA8: case 0xD3: case 0x20: case 0xDA: case 0x81: case 0xD9: case 0xDB: case 0x8D: args = 1; break;
      default: args = 0; break;
    }
    if (op == 0xAE && !includeDisplayOff) continue;
    uint8_t v = op;
    if (op == 0xA1 || op == 0xA0) v = flipped ? 0xA0 : 0xA1;
    else if (op == 0xC8 || op == 0xC0) v = flipped ? 0xC0 : 0xC8;
    else if (op == 0xA6 || op == 0xA7) v = inverted_ ? 0xA7 : 0xA6;
    out[n++] = v;
    for (int a = 0; a < args && i + 1 < sizeof(kInit); ++a) {
      ++i;
      out[n++] = op == 0x81 ? contrast_[panel] : kInit[i];
    }
  }
  return n;
}

void PanelStack::refresh() {
  uint8_t seq[sizeof(kInit) + 1];
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int p = 0; p < kPanels; ++p) {
    size_t n = buildInit(p, seq, false);
    seq[n++] = 0xAF;  // display on (in case a glitch turned it off)
    command(p, seq, n);
    setWindow(p, static_cast<uint8_t>(kPages - 1));
  }
  spi_->endTransaction();
}

void PanelStack::initPanel(int panel) {
  uint8_t seq[sizeof(kInit)];
  const size_t n = buildInit(panel, seq, true);
  command(panel, seq, n);
  // Clear the whole 64-row GDDRAM, including pages a 56-row panel never shows.
  static uint8_t zeros[128];
  memset(zeros, 0, sizeof(zeros));
  setWindow(panel, 7);
  for (int page = 0; page < 8; ++page) data(panel, zeros, sizeof(zeros));
  setWindow(panel, static_cast<uint8_t>(kPages - 1));
}

void PanelStack::setWindow(int panel, uint8_t lastPage) {
  const uint8_t win[] = {0x21, 0x00, 0x7F, 0x22, 0x00, lastPage};
  command(panel, win, sizeof(win));
}

void PanelStack::command(int panel, const uint8_t* bytes, size_t n) {
  digitalWrite(board::kOledDc, LOW);
  csLow(panel);
  spi_->writeBytes(bytes, n);
  csHigh(panel);
}

void PanelStack::data(int panel, const uint8_t* bytes, size_t n) {
  digitalWrite(board::kOledDc, HIGH);
  csLow(panel);
  spi_->writeBytes(bytes, n);
  csHigh(panel);
}

void PanelStack::writeFrame(const gm::VoxelFrame& frame) {
  const int64_t t0 = esp_timer_get_time();
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int layer = 0; layer < kPanels; ++layer) {
    const int panel = panelForLayer(layer);
    // Re-sending the window every frame re-synchronises a panel whose
    // address pointer was disturbed by noise.
    setWindow(panel, static_cast<uint8_t>(kPages - 1));
    data(panel, frame.layer(layer), kBytesPerLayer);
  }
  spi_->endTransaction();
  lastTransferUs_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
}

void PanelStack::clear() {
  // Stream one blank row per page instead of keeping a whole blank frame
  // (7 KB of static DRAM) around.
  static const uint8_t kZeros[128] = {};
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int p = 0; p < kPanels; ++p) {
    setWindow(p, static_cast<uint8_t>(kPages - 1));
    for (int page = 0; page < kPages; ++page) data(p, kZeros, sizeof(kZeros));
  }
  spi_->endTransaction();
}

void PanelStack::setContrast(int panel, uint8_t value) {
  if (panel < 0 || panel >= kPanels) return;
  contrast_[panel] = value;
  const uint8_t cmd[] = {0x81, value};
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  command(panel, cmd, sizeof(cmd));
  spi_->endTransaction();
}

void PanelStack::setContrastRamp(uint8_t front, uint8_t back) {
  for (int layer = 0; layer < kPanels; ++layer) {
    const float t = kPanels > 1 ? static_cast<float>(layer) / static_cast<float>(kPanels - 1) : 0.0f;
    const int v = static_cast<int>(front + (static_cast<int>(back) - static_cast<int>(front)) * t + 0.5f);
    setContrast(panelForLayer(layer), static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)));
  }
}

void PanelStack::setInverted(bool on) {
  inverted_ = on;
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int p = 0; p < kPanels; ++p) command1(p, on ? 0xA7 : 0xA6);
  spi_->endTransaction();
}

void PanelStack::setDisplayOn(bool on) {
  spi_->beginTransaction(SPISettings(GM_SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int p = 0; p < kPanels; ++p) command1(p, on ? 0xAF : 0xAE);
  spi_->endTransaction();
}

void PanelStack::testPattern(int panel) {
  // Diagnostic only: borrow a frame from the heap rather than holding 7 KB of
  // static DRAM for the life of the firmware.
  std::unique_ptr<gm::VoxelFrame> frame(new (std::nothrow) gm::VoxelFrame());
  if (!frame) return;
  gm::VoxelFrame& f = *frame;
  f.clear();
  if (panel >= 0 && panel < kPanels) {
    const int layer = GM_PANELS_REVERSED ? (kPanels - 1 - panel) : panel;
    f.rect(layer, 0, 0, gm::kW - 1, gm::kH - 1);
    f.rect(layer, 2, 2, gm::kW - 3, gm::kH - 3);
    char label[16];
    snprintf(label, sizeof(label), "PANEL %d", panel);
    f.text(layer, gm::kW / 2 - 14, gm::kH / 2 - 2, label);
    char where[16];
    snprintf(where, sizeof(where), "L%d%s", layer, layer == 0 ? " FRONT" : (layer == kPanels - 1 ? " BACK" : ""));
    f.text(layer, 4, 4, where);
  }
  writeFrame(f);
}
