#pragma once
#include <cstdint>
#include <cstddef>
extern uint32_t fakeMs;
inline uint32_t millis() { return fakeMs; }
inline void delay(uint32_t ms) { fakeMs += ms; }
struct TestSerial {
  void println(const char*) {}
  template<class... Args> void printf(const char*, Args...) {}
};
inline TestSerial Serial;
struct RebootRequested {};
struct TestESP { void restart() { throw RebootRequested{}; } };
inline TestESP ESP;
