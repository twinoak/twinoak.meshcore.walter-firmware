// A clock whose delays take no time: delayMs() advances it. A test that needs to act in the
// middle of a delay - to try acquiring SWD while the kill line is driven, say - sets on_delay,
// which runs before the clock advances, with the board in the state it is in during the wait.
//
// It also models the one thing about the board's delays that is not "time passes" (task p8-01):
// whether a delay **blocks**, so that the idle task of the core runs and the task watchdog is
// reset. On the board (platform/clock.cpp) that depends on the length: CONFIG_FREERTOS_HZ is
// 100, so pdMS_TO_TICKS(ms) is zero below 10 ms and EspClock busy-waits with esp_rom_delay_us;
// at 10 ms and above it calls vTaskDelay and the scheduler runs something else. delayUs() is
// always a busy wait. The fake answers to both rules and keeps the longest stretch of simulated
// time it has run without a blocking delay - which is what the 30 s task watchdog sees.
#pragma once

#include <cstdint>
#include <functional>

#include "interfaces/clock.h"

namespace fakes {

// One FreeRTOS tick at CONFIG_FREERTOS_HZ = 100 (sdkconfig.defaults): the shortest delay the
// board turns into a vTaskDelay rather than a busy wait.
constexpr std::uint32_t kOneTickMs = 10;

class FakeClock final : public interfaces::Clock {
 public:
  std::uint64_t now_ms = 0;
  std::uint64_t delayed_ms = 0;  // the sum of every delayMs()
  std::function<void(std::uint32_t)> on_delay;

  std::uint64_t nowMs() override { return now_ms; }

  void delayMs(std::uint32_t ms) override {
    delayed_ms += ms;
    if (on_delay && !in_hook_) {
      in_hook_ = true;
      on_delay(ms);
      in_hook_ = false;
    }
    // A delay of a tick or more blocks on the board: the stretch that ends here is the one the
    // watchdog was counting, and the next one starts when the delay is over.
    const bool blocks = ms >= kOneTickMs;
    if (blocks) closeStretch();
    now_ms += ms;
    if (blocks) {
      blocking_delays++;
      stretch_began_ms_ = now_ms;
    }
  }

  std::uint64_t delayed_us = 0;  // the sum of every delayUs(): the SWD tests' stopwatch

  void delayUs(std::uint32_t us) override {
    delayed_us += us;
    us_ += us;
    while (us_ >= 1000) {
      us_ -= 1000;
      now_ms += 1;
    }
  }

  // Time passing outside the code under test - a bench setting the scene, a sleep - not time the
  // firmware spent spinning: it ends the stretch rather than lengthening it.
  void advance(std::uint64_t ms) {
    closeStretch();
    now_ms += ms;
    stretch_began_ms_ = now_ms;
  }

  // The same clock in microseconds, for a simulator whose timings are shorter than a millisecond
  // (the nRF52's flash controller, task p1-04). Not part of interfaces::Clock: the firmware does
  // not need it.
  std::uint64_t nowUs() const { return now_ms * 1000u + us_; }

  // ---- what the task watchdog sees (task p8-01) ----
  // The longest run of simulated time with no blocking delay in it, the one still open included.
  std::uint64_t longest_busy_stretch_ms = 0;
  std::uint64_t blocking_delays = 0;

  std::uint64_t longestBusyStretchMs() const {
    const std::uint64_t open = now_ms - stretch_began_ms_;
    return open > longest_busy_stretch_ms ? open : longest_busy_stretch_ms;
  }
  // Start measuring here: the test's own setting-up is not what it is about.
  void resetBusyStretch() {
    longest_busy_stretch_ms = 0;
    blocking_delays = 0;
    stretch_began_ms_ = now_ms;
  }

 private:
  void closeStretch() {
    const std::uint64_t open = now_ms - stretch_began_ms_;
    if (open > longest_busy_stretch_ms) longest_busy_stretch_ms = open;
  }

  bool in_hook_ = false;
  std::uint32_t us_ = 0;
  std::uint64_t stretch_began_ms_ = 0;
};

}  // namespace fakes
