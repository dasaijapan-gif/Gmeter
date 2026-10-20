// Driver for a stack of SSD1309/SSD1306 128xN monochrome panels sharing one
// SPI bus (SCK, MOSI, D/C, RES) with one chip-select GPIO per panel.
//
// Frames come straight from gm_frame_t, whose per-layer byte layout is the
// controller's page-major GDDRAM format, so a layer upload is one window
// command plus one contiguous write. At 10 MHz, 8 panels of 7 pages take
// about 6 ms (around 150 fps ceiling). The layer data goes out by DMA while
// the calling task polls for the end of each transfer (a blocking transfer,
// as in the Arduino driver), which is why app_main.c runs it on its own task
// on the other core.
//
// Every function may be called from any task: each one holds the driver's
// lock while it drives the bus, D/C and the chip selects (like a transaction
// on Arduino's SPI class).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gm_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_COUNT GM_D

typedef struct {
  spi_device_handle_t spi;
  SemaphoreHandle_t lock;
  spi_transaction_t trans;  // the transfer in progress
  uint8_t contrast[PANEL_COUNT];
  bool inverted;
  bool stalled;  // the SPI peripheral did not finish a transfer (emulator only); output stopped
  uint32_t last_transfer_us;
} panel_stack_t;

// Configures GPIOs and the SPI bus, pulses reset, initialises every panel and
// clears them.
void panel_stack_begin(panel_stack_t* d);
// Hard reset: pulses RES, re-sends the init sequence, clears GDDRAM and waits
// 100 ms before display-on. The panels go dark for about 0.12 s.
void panel_stack_reinit(panel_stack_t* d);
// Soft refresh: re-sends every register (contrast, remap, addressing...)
// without reset, clear or delay, so nothing visibly blinks. The bus is
// write-only, so a panel scrambled by EMI cannot report it; call this
// periodically.
void panel_stack_refresh(panel_stack_t* d);

// Uploads all layers. Blocks for the SPI transfer. The frame should be in
// DMA-capable memory (heap_caps_malloc with MALLOC_CAP_DMA); otherwise the
// SPI driver copies each layer first.
void panel_stack_write_frame(panel_stack_t* d, const gm_frame_t* frame);
void panel_stack_clear(panel_stack_t* d);

// 0..255 segment current. Rear layers usually need more: see set_contrast_ramp.
void panel_stack_set_contrast(panel_stack_t* d, int panel, uint8_t value);
void panel_stack_set_contrast_ramp(panel_stack_t* d, uint8_t front, uint8_t back);
void panel_stack_set_inverted(panel_stack_t* d, bool on);
void panel_stack_set_display_on(panel_stack_t* d, bool on);
// Draws a double border, the panel index and the layer it maps to on panel
// `panel`; all other panels go dark.
void panel_stack_test_pattern(panel_stack_t* d, int panel);

static inline uint32_t panel_stack_last_transfer_us(const panel_stack_t* d) { return d->last_transfer_us; }
static inline int panel_stack_panel_for_layer(int layer) {
  return GM_PANELS_REVERSED ? (PANEL_COUNT - 1 - layer) : layer;
}

#ifdef __cplusplus
}
#endif
