// Minimal Arduino/ESP32 API stubs so the firmware sources can be compiled
// (not run) on the host for type checking. Only what src/ uses is declared.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define MSBFIRST 1
#define SPI_MODE0 0
#define PROGMEM
#define IRAM_ATTR
#define ARDUINO_RUNNING_CORE 1
#define APP_CPU_NUM 1
#define PRO_CPU_NUM 0

typedef bool boolean;
typedef uint8_t byte;

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 1; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned) {}
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }
inline long constrain(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

class Print {
 public:
  virtual size_t write(uint8_t) { return 1; }
  size_t print(const char* s) { return std::printf("%s", s); }
  size_t print(int v) { return std::printf("%d", v); }
  size_t print(unsigned v) { return std::printf("%u", v); }
  size_t print(long v) { return std::printf("%ld", v); }
  size_t print(unsigned long v) { return std::printf("%lu", v); }
  size_t print(float v, int d = 2) { return std::printf("%.*f", d, v); }
  size_t print(char c) { return std::printf("%c", c); }
  size_t println() { return std::printf("\n"); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
  size_t println(float v, int d) { size_t n = print(v, d); return n + println(); }
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
inline size_t Print::printf(const char*, ...) { return 0; }

class Stream : public Print {
 public:
  int available() { return 0; }
  int read() { return -1; }
  int peek() { return -1; }
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  void setDebugOutput(bool) {}
};
extern HardwareSerial Serial;

class String {
 public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
 private:
  std::string s_;
};

// FreeRTOS subset.
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define tskIDLE_PRIORITY 0
#define configMAX_PRIORITIES 25
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, BaseType_t) { return pdPASS; }
inline void vTaskDelay(TickType_t) {}
inline void vTaskDelayUntil(TickType_t*, TickType_t) {}
inline TickType_t xTaskGetTickCount() { return 0; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return nullptr; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return nullptr; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
inline void taskYIELD() {}
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }

// esp-idf bits
inline int64_t esp_timer_get_time() { return 0; }
inline uint32_t esp_random() { return 4; }
inline uint32_t ESP_getFreeHeap() { return 200000; }
struct EspClass { uint32_t getFreeHeap() { return 200000; } uint32_t getMinFreeHeap() { return 100000; } void restart() {} uint32_t getCpuFreqMHz() { return 240; } };
extern EspClass ESP;
