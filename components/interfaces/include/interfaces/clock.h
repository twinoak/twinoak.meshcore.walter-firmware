// Time, as core/ sees it: a monotonic millisecond clock and two delays. The host tests give a fake
// whose delays advance the clock instantly; platform/ gives ESP-IDF's timer and FreeRTOS' delay.
#pragma once

#include <cstdint>

namespace interfaces {

class Clock {
 public:
  virtual ~Clock() = default;

  // Milliseconds since this boot. Never goes backwards within a boot. Today it restarts at zero
  // after a deep sleep (ESP-IDF's esp_timer does: docs/en/api-reference/system/esp_timer.rst,
  // "Upon wakeup from deep sleep, the initialization timer restarts from zero"); the duty cycle
  // task (phase 3) decides whether the platform carries the time slept across. Code that keeps a
  // time in retained storage must therefore be ready for a clock smaller than the time it kept,
  // and read that as "a new epoch; assume the worst" (core/board/radio_pins.cpp does).
  virtual std::uint64_t nowMs() = 0;

  // Blocks for at least this long. Other tasks run meanwhile - **at every length, including 1**
  // (task p9-01): on the board a millisecond is a FreeRTOS tick (CONFIG_FREERTOS_HZ=1000), so
  // delayMs(n >= 1) is always a vTaskDelay and never a busy wait, and the idle task the task
  // watchdog watches gets its slot. A loop that waits on this therefore cannot starve the
  // watchdog however long it waits; a loop that waits on delayUs still can.
  virtual void delayMs(std::uint32_t ms) = 0;

  // A busy wait for a few microseconds: the SWD half-period (phase 1). This one holds the CPU, so
  // a loop around it longer than a second needs a delayMs between its operations
  // (core/swd/idle_pause.h).
  virtual void delayUs(std::uint32_t us) = 0;
};

}  // namespace interfaces
