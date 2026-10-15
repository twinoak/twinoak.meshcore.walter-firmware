// A sleeper that returns (task p3-04). On a board `interfaces::Sleeper::sleep()` does not come
// back - a wake from deep sleep is a boot - so a test that wants a month of wakes needs a sleep
// that returns and a world that notices it happened. This is both:
//
//   * it **checks the pins at the moment of sleeping** against docs/ARCHITECTURE.md §2 rules 1
//     and 5, and fails the running test at once when they are wrong. That is the task's own
//     requirement - "the pins at the moment of sleeping are safe() in every path that sleeps,
//     errors included - the fake pins assert it when sleep is called" - and the check lives here
//     rather than in fake_pins.h because it is the *sleeper* that knows a sleep is happening;
//   * it advances the fake clock by the seconds slept, so that anything measuring the passage of
//     time sees it;
//   * it records the plan, so a test can say "the timer was armed for six hours and the line
//     was not";
//   * `wake_as` is what the *next* boot will answer `wokeBy()` with, which is how a bench walks
//     a wake on the timer and a wake on the radio's line;
//   * `usb` is what `usbHost()` answers.
//
// A "boot" in a test is the core objects constructed again over the same FakeRetained, which is
// the pattern host_test/core/update/update_bench.h already uses for a reset in the middle of an
// update.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "core/board/pin_map.h"
#include "core/board/radio_pins.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "interfaces/sleeper.h"

namespace fakes {

class FakeSleeper final : public interfaces::Sleeper {
 public:
  FakeSleeper(FakeClock& clock, FakePins& pins) : clock_(clock), pins_(pins) {}

  // What the next wokeBy() answers. A test sets it before the sleep whose wake it describes.
  interfaces::WokeBy wake_as = interfaces::WokeBy::kTimer;
  bool usb = false;
  // The chip refuses the plan: what a sleeper answers when a source could not be armed.
  bool refuse = false;
  // Checked at every sleep unless a test turns it off (one test shows the check firing).
  bool check_pins = true;
  // Called after the pins have been checked and before the clock is advanced: how a bench makes
  // the world move while the Walter is away - a battery that sinks, a radio that was reset.
  std::function<void(const interfaces::SleepPlan&)> on_sleep;

  std::vector<interfaces::SleepPlan> plans;
  std::uint32_t sleeps = 0;
  std::uint64_t seconds_slept = 0;
  std::uint32_t usb_reads = 0;

  bool sleep(const interfaces::SleepPlan& plan) override {
    // A sleep with neither source is one only a reset would end: the platform refuses it, and so
    // does this.
    if (plan.seconds == 0 && !plan.radio_line) return false;
    armed_ = plan.radio_line;
    if (check_pins) checkPins();
    plans.push_back(plan);
    ++sleeps;
    if (refuse) return false;
    if (on_sleep) on_sleep(plan);
    seconds_slept += plan.seconds;
    // The timer is what the sleep lasts when nothing else fires; a wake on the line is modelled
    // by the test setting `wake_as` and shortening the plan itself.
    clock_.advance(static_cast<std::uint64_t>(plan.seconds) * 1000u);
    return true;
  }

  interfaces::WokeBy wokeBy() override { return wake_as; }

  bool usbHost() override {
    ++usb_reads;
    return usb;
  }

  // The last plan, or an empty one.
  interfaces::SleepPlan last() const {
    return plans.empty() ? interfaces::SleepPlan{} : plans.back();
  }

 private:
  // §2 rule 1 (the safe state) and rule 5 (the pads held in it) at the moment of sleeping. The
  // wake pin is the one exception: when the plan arms it, it carries the chip's weak pull-up and
  // is held by the platform's own wake preparation, not by RadioPins (core/board/radio_pins.h).
  void checkPins() {
    using core::board::kRadioPins;
    for (const core::board::PinDef& pin : kRadioPins) {
      const bool is_wake_pin = pin.gpio == core::board::wakePinGpio();
      INFO("the pins at the moment of sleeping, " << pin.net);
      // Nothing is driving.
      CHECK_FALSE(pins_.driving(pin.gpio));
      if (is_wake_pin) {
        // Armed: a pull-up and no hold of ours. Not armed: held like the rest.
        CHECK(pins_.held(pin.gpio) == !armed_);
        if (armed_) CHECK(pins_.pulling(pin.gpio));
      } else {
        CHECK(pins_.held(pin.gpio));
      }
    }
    CHECK_FALSE(pins_.killDriven());
    CHECK_FALSE(pins_.resetAsserted());
  }

  FakeClock& clock_;
  FakePins& pins_;
  bool armed_ = false;
};

}  // namespace fakes
