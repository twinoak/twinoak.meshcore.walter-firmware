// Rails R1 and R2 (task p3-06): **nRESET is never output-high**, and **every radio-facing pin is
// a plain input before the kill line goes high, with two resets at least a second apart**.
//
// These are not one test's business. They are checked by the fake pins on every transition of
// every test in this suite (`strict`), and again at every fake's destruction, where a violation
// no test declared with `allow()` fails that test whatever `strict` said - so the rails cannot be
// turned off by a line in a test. This file is the suite's own statement about that: it prints
// what the whole run did to a set of simulated wires, holds the undeclared count to zero, and
// shows the destructor's check working and the declaration mechanism working, so that a tally of
// zero cannot be a tally that counts nothing.
//
// doctest runs test cases in file order, and `host_test/rails/` sorts after `host_test/core/`, so
// the numbers printed below are the whole suite's. The assertion does not depend on that: an
// undeclared violation anywhere fails its own test at the moment it is destroyed, and the count
// here is zero at every point of the run.
//
// The grep half of both rails - that the nRESET pin reaches the driver only through ResetLine,
// which can only assert low and release; that no GPIO output mode is configured outside
// platform/pins.cpp; that the seven pins are named only in core/board/ - is tools/check_rails.py,
// which runs before this is compiled.
#include <doctest.h>

#include <string>

#include "core/board/pin_map.h"
#include "core/board/radio_pins.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"

using core::board::kNReset;
using core::board::kPwrKill;
using core::board::kResetSpacingMs;
using core::board::kWalterTx;
using interfaces::PinMode;

TEST_CASE("R1, R2: over the whole suite, no pin rule was broken that a test did not ask for") {
  const fakes::PinTally& tally = fakes::pinTally();

  // The rail. Every violation any fake recorded is either one a test declared (two files do, to
  // show a rule firing) or a failure of that test; this is the suite's count of the second kind.
  CHECK(tally.undeclared == 0);
  for (const auto& entry : tally.undeclared_by_rule) {
    FAIL_CHECK("a pin rule was broken " << entry.second << " time(s) with no test declaring it: "
                                        << entry.first);
  }

  // The four rails' own rules, named, with what the suite saw of each.
  std::string seen;
  for (const char* rule : {"reset-driven-high", "reset-spacing", "driven-while-killed",
                           "kill-driven-low", "kill-bounds", "reset-pulse-length",
                           "swdclk-while-reset", "selector-window"}) {
    const auto it = tally.by_rule.find(rule);
    const unsigned declared = it == tally.by_rule.end() ? 0u : it->second;
    const auto bad = tally.undeclared_by_rule.find(rule);
    seen += std::string(rule) + " " + std::to_string(declared) + " (declared), " +
            std::to_string(bad == tally.undeclared_by_rule.end() ? 0u : bad->second) +
            " (undeclared); ";
  }

  MESSAGE("the pin rails over the whole suite: " << tally.instances << " sets of fake pins, "
          << tally.operations << " transitions checked, " << tally.reset_pulses
          << " reset pulses, " << tally.kill_cycles << " kill cycles; " << tally.violations
          << " violations, " << tally.declared << " of them declared by the test that wanted to "
          << "see one, " << tally.undeclared << " undeclared. " << seen);
  // The suite really did drive these wires: a tally of zero undeclared violations over no
  // transitions at all would be a rail that is not being checked.
  CHECK(tally.instances > 20);
  CHECK(tally.operations > 1000);
}

TEST_CASE("the destructor's check fires: a test that drives nRESET high with strict off still fails") {
  // What the rail refuses, and the mechanism that refuses it. `report_undeclared` is false so
  // that this test does not fail itself and does not dirty the suite's tally - the only use for
  // that switch, and the reason it is named after what it turns off.
  const unsigned undeclared_before = fakes::pinTally().undeclared;
  {
    fakes::FakeClock clock;
    fakes::FakePins pins{clock};
    pins.strict = false;             // "do not fail me at the transition"
    pins.report_undeclared = false;  // ... and this test is about the destructor, not the wire
    pins.write(kNReset.gpio, true);
    pins.setMode(kNReset.gpio, PinMode::kOutput);  // push-pull high: the thing that must never be
    CHECK(pins.countViolations("reset-driven-high") == 2);
    CHECK(pins.undeclaredViolations() == 2);
  }
  CHECK(fakes::pinTally().undeclared == undeclared_before);

  // And with the rule declared, the same two are not undeclared: the mechanism a test uses to
  // say "I want to see this fire" (host_test/core/board/test_fake_pins_rules.cpp).
  {
    fakes::FakeClock clock;
    fakes::FakePins pins{clock};
    pins.strict = false;
    pins.allow("reset-driven-high");
    pins.write(kNReset.gpio, true);
    CHECK(pins.countViolations("reset-driven-high") == 1);
    CHECK(pins.undeclaredViolations() == 0);
  }
  CHECK(fakes::pinTally().undeclared == undeclared_before);
}

TEST_CASE("R1: the only handle on nRESET asserts low and releases, and the pulse never goes high") {
  // RadioPins through the fake pins, which is how every other test reaches the line: a pulse is
  // open-drain low and then an input, and the wire comes back to the nRF52's own pull-up.
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  core::board::RadioPins board{pins, clock, retained};

  CHECK(pins.modeOf(kNReset.gpio) == PinMode::kInput);
  clock.advance(kResetSpacingMs);
  REQUIRE(board.resetPulse() == core::board::Outcome::kOk);
  CHECK(pins.modeOf(kNReset.gpio) == PinMode::kInput);
  CHECK(pins.wireLevel(kNReset.gpio));  // released: the radio's pull-up has it
  // Not once, in the whole pulse, was the pin push-pull or written high.
  for (const fakes::Transition& t : pins.transitionsOf(kNReset.gpio)) {
    if (t.op.kind == fakes::OpKind::kMode) {
      const bool ok = t.op.mode == PinMode::kInput || t.op.mode == PinMode::kOpenDrain;
      CHECK_MESSAGE(ok, "nRESET was configured in a mode that can drive it high");
    }
    if (t.op.kind == fakes::OpKind::kLevel) {
      CHECK_MESSAGE(!t.op.level, "nRESET was written high");
    }
  }
  CHECK(pins.violations.empty());
}

TEST_CASE("R2: before the kill line goes high every other radio pin is an input, and TX's pull-up is off") {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  core::board::RadioPins board{pins, clock, retained};
  clock.advance(core::board::kKillTiming.settle_ms);
  REQUIRE(board.uartEnable() == core::board::Outcome::kOk);  // somebody holding the UART
  CHECK(pins.driving(kWalterTx.gpio));

  const std::size_t before = pins.log.size();
  REQUIRE(board.killCycle(core::board::kKillTiming.min_ms) == core::board::Outcome::kOk);

  // The order on the wires: the moment KILL is first driven, nothing else drives or pulls. The
  // fake pins check that continuously (driven-while-killed); this is the same thing read off the
  // log, so that the rail is stated here and not only in the fake.
  int kill_driven_at = -1;
  for (std::size_t i = before; i < pins.log.size(); ++i) {
    const fakes::Transition& t = pins.log[i];
    if (t.op.gpio == kPwrKill.gpio && t.op.kind == fakes::OpKind::kMode &&
        t.op.mode == PinMode::kOutput) {
      kill_driven_at = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(kill_driven_at > 0);
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    if (pin.gpio == kPwrKill.gpio) continue;
    // The last mode this pin was put in before the kill line went high.
    PinMode mode = PinMode::kInput;
    for (int i = 0; i < kill_driven_at; ++i) {
      const fakes::Transition& t = pins.log[static_cast<std::size_t>(i)];
      if (t.op.gpio == pin.gpio && t.op.kind == fakes::OpKind::kMode) mode = t.op.mode;
    }
    CHECK_MESSAGE(mode == PinMode::kInput,
                  std::string(pin.net) << " was not a plain input when the kill line went high");
  }
  CHECK(pins.violations.empty());
  CHECK(pins.kill_cycles == 1);
}

TEST_CASE("R2: two resets are a second apart, and the second waits rather than being refused") {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  core::board::RadioPins board{pins, clock, retained};
  clock.advance(kResetSpacingMs);
  REQUIRE(board.resetPulse() == core::board::Outcome::kOk);
  const std::uint64_t first_ended = clock.nowMs();
  // At once again: it is not refused, it waits.
  REQUIRE(board.resetPulse() == core::board::Outcome::kOk);
  const std::uint64_t second_started = clock.nowMs() - core::board::kResetPulseMs;
  CHECK(second_started - first_ended >= kResetSpacingMs);
  CHECK(pins.countViolations("reset-spacing") == 0);
  CHECK(pins.reset_pulses == 2);
  MESSAGE("the second pulse began " << (second_started - first_ended) << " ms after the first "
          << "ended (the rule is " << kResetSpacingMs << " ms)");
}
