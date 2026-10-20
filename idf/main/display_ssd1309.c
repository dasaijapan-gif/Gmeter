#include "display_ssd1309.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const int k_cs_pins[8] = BOARD_OLED_CS_PINS;

static inline void cs_low(int panel) { gpio_set_level((gpio_num_t)k_cs_pins[panel], 0); }
static inline void cs_high(int panel) { gpio_set_level((gpio_num_t)k_cs_pins[panel], 1); }
static inline void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// Init sequences (sent with D/C low). Contrast (0x81) and remap (0xA0/0xA1,
// 0xC0/0xC8) are patched per panel at runtime.
#if GM_PANEL_MODEL == 0
// WiseChip UG-2856KLBAG01 (128x56 transparent, SSD1309): the panel maker's
// sequence plus horizontal addressing. VCC (12 V) is supplied by the breakout.
static const uint8_t k_init[] = {
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
static const uint8_t k_init[] = {
    0xFD, 0x12, 0xAE, 0xD5, 0xA0, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x20, 0x00,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x6F, 0xD9, 0xD3, 0xDB, 0x20, 0xA4, 0xA6,
};
#else
// 128x64 SSD1306 with internal charge pump (3.3 V supply).
static const uint8_t k_init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E,
};
#endif

// DMA-capable staging for command bytes (callers build them on the stack) and
// a blank page row. Guarded by the driver's lock.
static DMA_ATTR uint8_t s_cmd[sizeof(k_init) + 8];
static DMA_ATTR uint8_t s_zeros[128];

// ---- bus access ---------------------------------------------------------------------------
// The equivalent of Arduino's beginTransaction()/endTransaction(): one task at
// a time drives the bus, D/C and the chip selects.
static void begin_transaction(panel_stack_t* d) {
  xSemaphoreTake(d->lock, portMAX_DELAY);
  if (!d->stalled) spi_device_acquire_bus(d->spi, portMAX_DELAY);
}

static void end_transaction(panel_stack_t* d) {
  if (!d->stalled) spi_device_release_bus(d->spi);
  xSemaphoreGive(d->lock);
}

// Sends n bytes from DMA-capable memory and waits for the end of the transfer
// by polling, not with the interrupt-driven spi_device_transmit: the transfer
// blocks like the Arduino driver's, and Espressif's QEMU only completes polled
// SPI transactions.
//
// The wait is bounded. The longest real transfer (1 KB at 1 MHz) takes about
// 8 ms, and an SPI master needs nothing from the panels to finish, so on a
// real chip this never times out. It does in Espressif's ESP32-S3 emulator,
// which does not run the SPI2 peripheral: the driver then stops using the bus
// (it cannot abort the open transaction) and the firmware runs on without
// panel output instead of hanging.
#define SPI_TIMEOUT_MS 500

static void transfer(panel_stack_t* d, const uint8_t* bytes, size_t n) {
  if (d->stalled) return;
  memset(&d->trans, 0, sizeof(d->trans));
  d->trans.length = n * 8;
  d->trans.tx_buffer = bytes;
  if (spi_device_polling_start(d->spi, &d->trans, portMAX_DELAY) != ESP_OK) return;
  if (spi_device_polling_end(d->spi, pdMS_TO_TICKS(SPI_TIMEOUT_MS)) == ESP_ERR_TIMEOUT) d->stalled = true;
}

// Command bytes, copied to DMA-capable memory first.
static void command(panel_stack_t* d, int panel, const uint8_t* bytes, size_t n) {
  if (n > sizeof(s_cmd)) n = sizeof(s_cmd);
  memcpy(s_cmd, bytes, n);
  gpio_set_level((gpio_num_t)BOARD_OLED_DC, 0);
  cs_low(panel);
  transfer(d, s_cmd, n);
  cs_high(panel);
}

static void command1(panel_stack_t* d, int panel, uint8_t b) { command(d, panel, &b, 1); }

// Pixel data: the SPI DMA reads it straight from `bytes` (DMA-capable memory).
static void data(panel_stack_t* d, int panel, const uint8_t* bytes, size_t n) {
  gpio_set_level((gpio_num_t)BOARD_OLED_DC, 1);
  cs_low(panel);
  transfer(d, bytes, n);
  cs_high(panel);
}

static void set_window(panel_stack_t* d, int panel, uint8_t last_page) {
  const uint8_t win[] = {0x21, 0x00, 0x7F, 0x22, 0x00, last_page};
  command(d, panel, win, sizeof(win));
}

// Copies the init table for one panel, patching its contrast, orientation and
// inversion. Every command's argument bytes are skipped explicitly so a
// parameter that happens to equal an opcode is never rewritten.
static size_t build_init(const panel_stack_t* d, int panel, uint8_t* out, bool include_display_off) {
  const bool flipped = (GM_PANEL_FLIP_MASK >> panel) & 1;
  size_t n = 0;
  for (size_t i = 0; i < sizeof(k_init); ++i) {
    const uint8_t op = k_init[i];
    // Argument counts for the opcodes used in the tables.
    int args = 0;
    switch (op) {
      case 0xFD: case 0xD5: case 0xA8: case 0xD3: case 0x20: case 0xDA: case 0x81: case 0xD9: case 0xDB: case 0x8D:
        args = 1;
        break;
      default:
        args = 0;
        break;
    }
    if (op == 0xAE && !include_display_off) continue;
    uint8_t v = op;
    if (op == 0xA1 || op == 0xA0) v = flipped ? 0xA0 : 0xA1;
    else if (op == 0xC8 || op == 0xC0) v = flipped ? 0xC0 : 0xC8;
    else if (op == 0xA6 || op == 0xA7) v = d->inverted ? 0xA7 : 0xA6;
    out[n++] = v;
    for (int a = 0; a < args && i + 1 < sizeof(k_init); ++a) {
      ++i;
      out[n++] = op == 0x81 ? d->contrast[panel] : k_init[i];
    }
  }
  return n;
}

static void init_panel(panel_stack_t* d, int panel) {
  uint8_t seq[sizeof(k_init)];
  const size_t n = build_init(d, panel, seq, true);
  command(d, panel, seq, n);
  // Clear the whole 64-row GDDRAM, including pages a 56-row panel never shows.
  set_window(d, panel, 7);
  for (int page = 0; page < 8; ++page) data(d, panel, s_zeros, sizeof(s_zeros));
  set_window(d, panel, (uint8_t)(GM_PAGES - 1));
}

// ---- public API ---------------------------------------------------------------------------
void panel_stack_begin(panel_stack_t* d) {
  d->lock = xSemaphoreCreateMutex();
  d->inverted = false;
  d->stalled = false;
  d->last_transfer_us = 0;
  uint64_t outputs = (1ull << BOARD_OLED_DC) | (1ull << BOARD_OLED_RESET);
  for (int i = 0; i < PANEL_COUNT; ++i) {
    cs_high(i);  // level first, so no chip select glitches low when it becomes an output
    outputs |= 1ull << k_cs_pins[i];
    d->contrast[i] = 0x8F;
  }
  gpio_set_level((gpio_num_t)BOARD_OLED_DC, 0);
  gpio_config_t io;
  memset(&io, 0, sizeof(io));
  io.pin_bit_mask = outputs;
  io.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&io));

  // Write-only bus: SCK and MOSI on the host's IO_MUX pins, no MISO, chip
  // select and D/C driven by hand (eight panels share the bus).
  spi_bus_config_t bus;
  memset(&bus, 0, sizeof(bus));
  bus.mosi_io_num = BOARD_SPI_MOSI;
  bus.miso_io_num = -1;
  bus.sclk_io_num = BOARD_SPI_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = GM_BYTES_PER_LAYER;
  ESP_ERROR_CHECK(spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
  spi_device_interface_config_t dev;
  memset(&dev, 0, sizeof(dev));
  dev.mode = 0;
  dev.clock_speed_hz = GM_SPI_HZ;
  dev.spics_io_num = -1;
  dev.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(BOARD_SPI_HOST, &dev, &d->spi));
  // D/C sits on the bus's default MISO pin (VSPI: GPIO19, S3 FSPI: GPIO13).
  // The bus was set up without MISO, so it stays ours; drive it again anyway
  // so no panel ever sees an invalid command/data line.
  gpio_set_direction((gpio_num_t)BOARD_OLED_DC, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)BOARD_OLED_DC, 0);
  panel_stack_set_contrast_ramp(d, 0x50, 0xFF);
  panel_stack_reinit(d);
}

void panel_stack_reinit(panel_stack_t* d) {
  // Shared reset: low >= 3 us after VDD is stable.
  gpio_set_level((gpio_num_t)BOARD_OLED_RESET, 1);
  delay_ms(5);
  gpio_set_level((gpio_num_t)BOARD_OLED_RESET, 0);
  delay_ms(5);
  gpio_set_level((gpio_num_t)BOARD_OLED_RESET, 1);
  delay_ms(10);
  begin_transaction(d);
  for (int p = 0; p < PANEL_COUNT; ++p) init_panel(d, p);
  end_transaction(d);
  delay_ms(100);  // panel VCC settle before turning pixels on
  panel_stack_set_display_on(d, true);
}

void panel_stack_refresh(panel_stack_t* d) {
  uint8_t seq[sizeof(k_init) + 1];
  begin_transaction(d);
  for (int p = 0; p < PANEL_COUNT; ++p) {
    size_t n = build_init(d, p, seq, false);
    seq[n++] = 0xAF;  // display on (in case a glitch turned it off)
    command(d, p, seq, n);
    set_window(d, p, (uint8_t)(GM_PAGES - 1));
  }
  end_transaction(d);
}

void panel_stack_write_frame(panel_stack_t* d, const gm_frame_t* frame) {
  const int64_t t0 = esp_timer_get_time();
  begin_transaction(d);
  for (int layer = 0; layer < PANEL_COUNT; ++layer) {
    const int panel = panel_stack_panel_for_layer(layer);
    // Re-sending the window every frame re-synchronises a panel whose
    // address pointer was disturbed by noise.
    set_window(d, panel, (uint8_t)(GM_PAGES - 1));
    data(d, panel, frame->bits[layer], GM_BYTES_PER_LAYER);
  }
  end_transaction(d);
  d->last_transfer_us = (uint32_t)(esp_timer_get_time() - t0);
}

void panel_stack_clear(panel_stack_t* d) {
  // Stream one blank row per page instead of keeping a whole blank frame
  // (7 KB of static DRAM) around.
  begin_transaction(d);
  for (int p = 0; p < PANEL_COUNT; ++p) {
    set_window(d, p, (uint8_t)(GM_PAGES - 1));
    for (int page = 0; page < GM_PAGES; ++page) data(d, p, s_zeros, sizeof(s_zeros));
  }
  end_transaction(d);
}

void panel_stack_set_contrast(panel_stack_t* d, int panel, uint8_t value) {
  if (panel < 0 || panel >= PANEL_COUNT) return;
  d->contrast[panel] = value;
  const uint8_t cmd[] = {0x81, value};
  begin_transaction(d);
  command(d, panel, cmd, sizeof(cmd));
  end_transaction(d);
}

void panel_stack_set_contrast_ramp(panel_stack_t* d, uint8_t front, uint8_t back) {
  for (int layer = 0; layer < PANEL_COUNT; ++layer) {
    const float t = PANEL_COUNT > 1 ? (float)layer / (float)(PANEL_COUNT - 1) : 0.0f;
    const int v = (int)(front + ((int)back - (int)front) * t + 0.5f);
    panel_stack_set_contrast(d, panel_stack_panel_for_layer(layer), (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
  }
}

void panel_stack_set_inverted(panel_stack_t* d, bool on) {
  d->inverted = on;
  begin_transaction(d);
  for (int p = 0; p < PANEL_COUNT; ++p) command1(d, p, on ? 0xA7 : 0xA6);
  end_transaction(d);
}

void panel_stack_set_display_on(panel_stack_t* d, bool on) {
  begin_transaction(d);
  for (int p = 0; p < PANEL_COUNT; ++p) command1(d, p, on ? 0xAF : 0xAE);
  end_transaction(d);
}

void panel_stack_test_pattern(panel_stack_t* d, int panel) {
  // Diagnostic only: borrow a frame from the heap rather than holding 7 KB of
  // static DRAM for the life of the firmware.
  gm_frame_t* f = (gm_frame_t*)heap_caps_malloc(sizeof(gm_frame_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!f) return;
  gm_frame_clear(f);
  if (panel >= 0 && panel < PANEL_COUNT) {
    const int layer = GM_PANELS_REVERSED ? (PANEL_COUNT - 1 - panel) : panel;
    gm_frame_rect(f, layer, 0, 0, GM_W - 1, GM_H - 1);
    gm_frame_rect(f, layer, 2, 2, GM_W - 3, GM_H - 3);
    char label[16];
    snprintf(label, sizeof(label), "PANEL %d", panel);
    gm_frame_text(f, layer, GM_W / 2 - 14, GM_H / 2 - 2, label);
    char where[16];
    snprintf(where, sizeof(where), "L%d%s", layer, layer == 0 ? " FRONT" : (layer == PANEL_COUNT - 1 ? " BACK" : ""));
    gm_frame_text(f, layer, 4, 4, where);
  }
  panel_stack_write_frame(d, f);
  heap_caps_free(f);
}
