// The fake pins' own rule checks, each fired once on purpose (task p0-03's acceptance: "each
// fired once on purpose"). The fake is driven directly here, bypassing RadioPins, with strict
// off so that the violation is recorded instead of failing the test. Every other test runs with
// strict on, where any of these would fail it the moment it happened.
#include <doctest.h>

#include "core/board/pin_map.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"

using core::board::kKillTiming;
using core::board::kNReset;
using core::board::kPwrKill;
using core::board::kSwdClk;
using core::board::kWalterTx;
using interfaces::PinMode;

namespace {

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  // strict off so that the violation is recorded instead of failing the test - and every rule
  // this file fires on purpose declared, because since task p3-06 an undeclared violation fails
  // the test at the fake's destructor whatever `strict` said (fakes/fake_pins.h).
  Rig() {
    pins.strict = false;
    for (const char* rule : {"reset-driven-high", "reset-spacing", "reset-pulse-length",
                             "swdclk-while-reset", "driven-while-killed", "kill-driven-low",
                             "kill-bounds", "selector-window"}) {
      pins.allow(rule);
    }
  }
};

void pulse(Rig& rig, std::uint32_t low_ms) {
  rig.pins.write(kNReset.gpio, false);
  rig.pins.setMode(kNReset.gpio, PinMode::kOpenDrain);
  rig.clock.delayMs(low_ms);
  rig.pins.setMode(kNReset.gpio, PinMode::kInput);
}

}  // namespace

TEST_CASE("rule check: reset-driven-high fires for a high level and for push-pull") {
  Rig rig;
  rig.pins.write(kNReset.gpio, true);
  CHECK(rig.pins.countViolations("reset-driven-high") == 1);
  rig.pins.setMode(kNReset.gpio, PinMode::kOutput);  // push-pull high: the thing that must never be
  CHECK(rig.pins.countViolations("reset-driven-high") == 2);
  CHECK(rig.pins.violations.size() == 2);
  // Push-pull low is a reset pulse by another name, and a wrong one: the fake sees both the mode
  // and, when it ends at once, the pulse length.
  rig.pins.setMode(kNReset.gpio, PinMode::kInput);
  rig.pins.write(kNReset.gpio, false);
  rig.pins.setMode(kNReset.gpio, PinMode::kOutput);
  CHECK(rig.pins.countViolations("reset-driven-high") == 3);
  rig.pins.setMode(kNReset.gpio, PinMode::kInput);
  CHECK(rig.pins.countViolations("reset-pulse-length") == 1);
}

TEST_CASE("rule check: reset-spacing fires for two pulses under a second apart") {
  Rig rig;
  pulse(rig, 100);
  rig.clock.advance(500);
  pulse(rig, 100);
  CHECK(rig.pins.countViolations("reset-spacing") == 1);
  CHECK(rig.pins.violations.size() == 1);
  rig.clock.advance(1000);
  pulse(rig, 100);
  CHECK(rig.pins.violations.size() == 1);  // a second apart: fine
}

TEST_CASE("rule check: reset-pulse-length fires for a short pulse and for a long one") {
  Rig rig;
  pulse(rig, 10);
  CHECK(rig.pins.countViolations("reset-pulse-length") == 1);
  rig.clock.advance(2000);
  pulse(rig, 400);
  CHECK(rig.pins.countViolations("reset-pulse-length") == 2);
  rig.clock.advance(2000);
  pulse(rig, 100);
  CHECK(rig.pins.countViolations("reset-pulse-length") == 2);
}

TEST_CASE("rule check: swdclk-while-reset fires when the clock moves during a pulse") {
  Rig rig;
  rig.pins.write(kSwdClk.gpio, false);
  rig.pins.setMode(kSwdClk.gpio, PinMode::kOutput);
  rig.pins.write(kNReset.gpio, false);
  rig.pins.setMode(kNReset.gpio, PinMode::kOpenDrain);
  rig.pins.write(kSwdClk.gpio, true);
  CHECK(rig.pins.countViolations("swdclk-while-reset") == 1);
  rig.pins.write(kSwdClk.gpio, false);
  CHECK(rig.pins.countViolations("swdclk-while-reset") == 2);
  rig.clock.delayMs(100);
  rig.pins.setMode(kNReset.gpio, PinMode::kInput);
  rig.pins.write(kSwdClk.gpio, true);  // reset released: fine
  CHECK(rig.pins.countViolations("swdclk-while-reset") == 2);
}

TEST_CASE("rule check: driven-while-killed fires for a pin driven as KILL rises, and for one that drives after") {
  Rig rig;
  rig.pins.write(kWalterTx.gpio, true);
  rig.pins.setMode(kWalterTx.gpio, PinMode::kOutput);
  rig.pins.write(kPwrKill.gpio, true);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kOutput);
  CHECK(rig.pins.countViolations("driven-while-killed") == 1);
  rig.pins.setMode(kWalterTx.gpio, PinMode::kInput);
  rig.pins.setMode(kWalterTx.gpio, PinMode::kInputPullUp);  // a pull-up is not nothing either
  CHECK(rig.pins.countViolations("driven-while-killed") == 2);
  rig.pins.setMode(kWalterTx.gpio, PinMode::kInput);
  rig.pins.setMode(kSwdClk.gpio, PinMode::kOutput);
  CHECK(rig.pins.countViolations("driven-while-killed") == 3);
}

TEST_CASE("rule check: kill-driven-low fires for push-pull low and for open-drain") {
  Rig rig;
  rig.pins.setMode(kPwrKill.gpio, PinMode::kOutput);  // level still low
  CHECK(rig.pins.countViolations("kill-driven-low") == 1);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kInput);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kOpenDrain);
  CHECK(rig.pins.countViolations("kill-driven-low") == 2);
}

TEST_CASE("rule check: kill-bounds fires for a kill shorter than the minimum and longer than the maximum") {
  Rig rig;
  rig.pins.write(kPwrKill.gpio, true);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kOutput);
  rig.clock.delayMs(kKillTiming.min_ms - 1);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kInput);
  CHECK(rig.pins.countViolations("kill-bounds") == 1);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kOutput);
  rig.clock.delayMs(kKillTiming.max_ms + 1);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kInput);
  CHECK(rig.pins.countViolations("kill-bounds") == 2);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kOutput);
  rig.clock.delayMs(kKillTiming.min_ms);
  rig.pins.setMode(kPwrKill.gpio, PinMode::kInput);
  CHECK(rig.pins.countViolations("kill-bounds") == 2);
  CHECK(rig.pins.violations.size() == 2);
}

TEST_CASE("the fake's injected failure refuses the operation and changes nothing") {
  Rig rig;
  rig.pins.fail_when = [](const fakes::Op& op) { return op.gpio == kWalterTx.gpio; };
  CHECK(!rig.pins.setMode(kWalterTx.gpio, PinMode::kOutput));
  CHECK(!rig.pins.write(kWalterTx.gpio, true));
  CHECK(rig.pins.modeOf(kWalterTx.gpio) == PinMode::kInput);
  CHECK(!rig.pins.levelOf(kWalterTx.gpio));
  CHECK(rig.pins.log.empty());
  CHECK(rig.pins.setMode(kSwdClk.gpio, PinMode::kOutput));
  CHECK(rig.pins.operations == 3);
}
