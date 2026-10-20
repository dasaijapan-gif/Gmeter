// Serial console: the command line's input and output and the firmware's own
// messages.
//
// The console is the one chosen in menuconfig (Component config > ESP System
// Settings > Channel for console output): UART0 at 115200 baud by default, or
// the USB Serial/JTAG port of the ESP32-S3. conio_init installs that port's
// driver with a 2 KB transmit buffer, so long replies (get, list, status) are
// queued instead of holding up the caller, and reads never block. ESP-IDF's
// own log lines go through the same driver.
//
// Output goes through a print_t, a small stand-in for Arduino's Print class:
// the console is one; the CLI's reply buffer is another. On the UART and USB
// Serial/JTAG consoles bytes are written as given (no newline translation):
// print_ln ends a line with "\r\n", as Arduino's println does. The USB-OTG CDC
// console goes through ESP-IDF's standard output, which turns "\n" into
// "\r\n".
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct print print_t;
struct print {
  // Writes up to n bytes; returns how many were taken.
  size_t (*write)(print_t* self, const char* data, size_t n);
};

static inline size_t print_write(print_t* p, const char* data, size_t n) { return p->write(p, data, n); }
// Arduino print(s).
size_t print_str(print_t* p, const char* s);
// Arduino println(s): s (may be "") then "\r\n".
size_t print_ln(print_t* p, const char* s);
// Arduino printf.
size_t print_fmt(print_t* p, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Installs the console driver. Call once, before any other conio_* call.
void conio_init(void);
// The console as a print_t.
print_t* conio_out(void);
// Copies up to `max` received bytes into buf without waiting; returns the
// count (0 when nothing is waiting).
size_t conio_read(uint8_t* buf, size_t max);
// Waits up to timeout_ms for queued output to leave (e.g. before a restart).
void conio_flush(int timeout_ms);

#ifdef __cplusplus
}
#endif
