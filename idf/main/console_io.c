// Serial console on the UART (default) or the USB Serial/JTAG port; see
// console_io.h.
#include "console_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/uart_vfs.h"
#else
#include "esp_vfs_dev.h"
#endif
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#endif
#else
// Any other console (USB CDC on the S3's USB-OTG, or none): standard I/O.
#include <fcntl.h>
#include <unistd.h>
#endif

// Room for the longest CLI reply (the 2 KB reply buffer) without waiting.
#define CONSOLE_TX_BUFFER 2048
#define CONSOLE_RX_BUFFER 256

// ---- print helpers -----------------------------------------------------------------------
size_t print_str(print_t* p, const char* s) { return print_write(p, s, strlen(s)); }

size_t print_ln(print_t* p, const char* s) {
  size_t n = print_str(p, s);
  n += print_write(p, "\r\n", 2);
  return n;
}

size_t print_fmt(print_t* p, const char* fmt, ...) {
  // Same approach as Arduino's Print::printf: format on the stack, fall back
  // to the heap for long lines.
  char local[128];
  char* text = local;
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int len = vsnprintf(local, sizeof(local), fmt, copy);
  va_end(copy);
  if (len < 0) {
    va_end(args);
    return 0;
  }
  if (len >= (int)sizeof(local)) {
    text = (char*)malloc((size_t)len + 1);
    if (!text) {
      va_end(args);
      return 0;
    }
    len = vsnprintf(text, (size_t)len + 1, fmt, args);
  }
  va_end(args);
  const size_t n = print_write(p, text, (size_t)len);
  if (text != local) free(text);
  return n;
}

// ---- the console -------------------------------------------------------------------------
static size_t conio_write(print_t* self, const char* data, size_t n) {
  (void)self;
  if (n == 0) return 0;
#if CONFIG_ESP_CONSOLE_UART
  const int sent = uart_write_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, data, n);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  // Gives up after 50 ms when no terminal is reading, instead of stalling.
  const int sent = usb_serial_jtag_write_bytes(data, n, pdMS_TO_TICKS(50));
#else
  const int sent = (int)fwrite(data, 1, n, stdout);
  fflush(stdout);
#endif
  return sent > 0 ? (size_t)sent : 0;
}

static print_t s_conio = {conio_write};

print_t* conio_out(void) { return &s_conio; }

void conio_init(void) {
#if CONFIG_ESP_CONSOLE_UART
  const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
  // The baud rate was set at start-up (CONFIG_ESP_CONSOLE_UART_BAUDRATE).
  if (!uart_is_driver_installed(port)) uart_driver_install(port, CONSOLE_RX_BUFFER, CONSOLE_TX_BUFFER, 0, NULL, 0);
  // ESP-IDF's log lines then use the driver too, so they cannot cut into ours.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  uart_vfs_dev_use_driver(port);
#else
  esp_vfs_dev_uart_use_driver(port);
#endif
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = CONSOLE_TX_BUFFER, .rx_buffer_size = CONSOLE_RX_BUFFER};
  usb_serial_jtag_driver_install(&cfg);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  usb_serial_jtag_vfs_use_driver();
#else
  esp_vfs_usb_serial_jtag_use_driver();
#endif
#else
  setvbuf(stdout, NULL, _IOFBF, CONSOLE_TX_BUFFER);
  const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif
}

size_t conio_read(uint8_t* buf, size_t max) {
  if (max == 0) return 0;
#if CONFIG_ESP_CONSOLE_UART
  const int n = uart_read_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, buf, (uint32_t)max, 0);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  const int n = usb_serial_jtag_read_bytes(buf, (uint32_t)max, 0);
#else
  const int n = (int)read(STDIN_FILENO, buf, max);
#endif
  return n > 0 ? (size_t)n : 0;
}

void conio_flush(int timeout_ms) {
#if CONFIG_ESP_CONSOLE_UART
  uart_wait_tx_done((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(timeout_ms));
#else
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(timeout_ms < 20 ? timeout_ms : 20));
#endif
}
