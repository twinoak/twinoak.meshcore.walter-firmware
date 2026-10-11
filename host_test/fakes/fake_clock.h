// A clock whose delays take no time: delayMs() advances it. A test that needs to act in the
// middle of a delay - to try acquiring SWD while the kill line is driven, say - sets on_delay,
// which runs before the clock advances, with the board in the state it is in during the wait.
#pragma once

#include <cstdint>
#include <functional>

#include "interfaces/clock.h"

namespace fakes {

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
    now_ms += ms;
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

  void advance(std::uint64_t ms) { now_ms += ms; }

 private:
  bool in_hook_ = false;
  std::uint32_t us_ = 0;
};

}  // namespace fakes
