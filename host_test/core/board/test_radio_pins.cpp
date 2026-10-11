// RadioPins against the fake pins (task p0-03): the safe state, the reset pulse and its spacing
// (across a simulated deep sleep, with the clock continuing and with it restarted), the kill
// cycle's order and bounds and what a failure in the middle leaves behind, the SWD and UART
// rules, the holds. The fake checks the rules of docs/ARCHITECTURE.md §2 on every transition
// (fakes/fake_pins.h), so every test here is also "no rule was broken".
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"

using core::board::kKillTiming;
using core::board::kNReset;
using core::board::kPwrKill;
using core::board::kResetPulseMs;
using core::board::kResetSpacingMs;
using core::board::kSpare;
using core::board::kSwdClk;
using core::board::kSwdIo;
using core::board::kWalterRx;
using core::board::kWalterTx;
using core::board::Outcome;
using core::board::RadioPins;
using core::board::RadioPower;
using fakes::OpKind;
using interfaces::PinMode;

namespace {

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
};

// Settles the rail after construction: the state "nothing to say, radio powered".
void settle(Rig& rig, RadioPins& rp) {
  rig.clock.advance(kKillTiming.settle_ms);
  rp.safe();
}

}  // namespace

TEST_CASE("construction is the safe state: nothing driven, nothing pulled until the rail is believed on") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    CHECK_MESSAGE(rig.pins.modeOf(pin.gpio) == PinMode::kInput, pin.net);
    CHECK_MESSAGE(!rig.pins.driving(pin.gpio), pin.net);
    CHECK_MESSAGE(!rig.pins.pulling(pin.gpio), pin.net);
  }
  CHECK(rp.power() == RadioPower::kUnknown);
  CHECK(!rp.uartEnabled());
  CHECK(!rp.swdHeld());
  CHECK(!rp.killDriven());
  CHECK(!rp.resetAsserted());

  // The settle time after boot passes: the rail is believed on, and the safe state pulls TX up
  // (idle high for the radio's receiver) - but still drives nothing.
  settle(rig, rp);
  CHECK(rp.power() == RadioPower::kOn);
  CHECK(rig.pins.modeOf(kWalterTx.gpio) == PinMode::kInputPullUp);
  CHECK(!rig.pins.driving(kWalterTx.gpio));
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    CHECK_MESSAGE(!rig.pins.driving(pin.gpio), pin.net);
  }
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("a reset pulse is open-drain low for 100 ms then input, and never push-pull") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  rig.pins.clearLog();
  REQUIRE(rp.resetPulse() == Outcome::kOk);
  const auto t = rig.pins.transitionsOf(kNReset.gpio);
  // level low (a repeat of the default: not a change), then open-drain, then input
  REQUIRE(t.size() == 2);
  CHECK(t[0].op.kind == OpKind::kMode);
  CHECK(t[0].op.mode == PinMode::kOpenDrain);
  CHECK(t[1].op.kind == OpKind::kMode);
  CHECK(t[1].op.mode == PinMode::kInput);
  CHECK(t[1].at_ms - t[0].at_ms == kResetPulseMs);
  CHECK(!rp.resetAsserted());
  CHECK(rp.lastResetKnown());
  CHECK(rp.lastResetEndMs() == t[1].at_ms);
  CHECK(rig.retained.has(interfaces::RetainedSlot::kResetRecord));
  // At no point was the reset pin push-pull or written high: the fake would have failed the test.
  CHECK(rig.pins.countViolations("reset-driven-high") == 0);
  for (const fakes::Transition& x : rig.pins.log) {
    if (x.op.gpio != kNReset.gpio) continue;
    CHECK(!(x.op.kind == OpKind::kMode && x.op.mode == PinMode::kOutput));
    CHECK(!(x.op.kind == OpKind::kLevel && x.op.level));
  }
}

TEST_CASE("the first reset after boot waits a second: the power-up glitch may have reset the radio just now") {
  Rig rig;
  rig.clock.advance(5000);  // the clock need not start at zero
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  CHECK(rp.msUntilResetAllowed() == kResetSpacingMs);
  rig.clock.advance(300);
  CHECK(rp.msUntilResetAllowed() == kResetSpacingMs - 300);
  REQUIRE(rp.resetPulse() == Outcome::kOk);
  const int asserted = rig.pins.indexOf(kNReset.gpio, OpKind::kMode, PinMode::kOpenDrain, false);
  REQUIRE(asserted >= 0);
  CHECK(rig.pins.log[static_cast<std::size_t>(asserted)].at_ms == 5000 + kResetSpacingMs);
}

TEST_CASE("two resets in a row: the second starts at least a second after the first ended") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  REQUIRE(rp.resetPulse() == Outcome::kOk);
  const std::uint64_t first_end = rp.lastResetEndMs();
  CHECK(rp.msUntilResetAllowed() == kResetSpacingMs);
  REQUIRE(rp.resetPulse() == Outcome::kOk);  // at once: it waits, it does not refuse
  const int second = rig.pins.indexOf(kNReset.gpio, OpKind::kMode, PinMode::kOpenDrain, false,
                                      static_cast<std::size_t>(rig.pins.indexOf(kNReset.gpio, OpKind::kMode, PinMode::kInput, false)));
  REQUIRE(second >= 0);
  CHECK(rig.pins.log[static_cast<std::size_t>(second)].at_ms == first_end + kResetSpacingMs);
  CHECK(rig.pins.countViolations("reset-spacing") == 0);

  SUBCASE("and half a second later again: still a second apart") {
    rig.clock.advance(500);
    REQUIRE(rp.resetPulse() == Outcome::kOk);
    CHECK(rig.pins.countViolations("reset-spacing") == 0);
  }
}

TEST_CASE("the spacing holds across a simulated deep sleep") {
  Rig rig;
  std::uint64_t ended = 0;
  {
    RadioPins before(rig.pins, rig.clock, rig.retained);
    settle(rig, before);
    REQUIRE(before.resetPulse() == Outcome::kOk);
    ended = before.lastResetEndMs();
  }

  SUBCASE("the clock continued through the sleep: the record is honoured to the millisecond") {
    rig.clock.advance(300);  // asleep, then awake: a new RadioPins over the same retained storage
    RadioPins after(rig.pins, rig.clock, rig.retained);
    CHECK(after.lastResetEndMs() == ended);
    CHECK(after.msUntilResetAllowed() == kResetSpacingMs - 300);
    rig.clock.advance(kKillTiming.settle_ms);  // the rail settles meanwhile
    REQUIRE(after.resetPulse() == Outcome::kOk);
    const int asserted = rig.pins.indexOf(kNReset.gpio, OpKind::kMode, PinMode::kOpenDrain, false,
                                          rig.pins.log.size() - 3);
    REQUIRE(asserted >= 0);
    CHECK(rig.pins.log[static_cast<std::size_t>(asserted)].at_ms == ended + kResetSpacingMs);
    CHECK(rig.pins.countViolations("reset-spacing") == 0);
  }

  SUBCASE("the clock restarted at zero (ESP-IDF's timer after deep sleep): the worst is assumed") {
    fakes::FakeClock clock2;
    fakes::FakePins pins2(clock2);
    RadioPins after(pins2, clock2, rig.retained);
    CHECK(after.lastResetEndMs() == 0);
    CHECK(after.msUntilResetAllowed() == kResetSpacingMs);
    REQUIRE(after.resetPulse() == Outcome::kOk);
    const int asserted = pins2.indexOf(kNReset.gpio, OpKind::kMode, PinMode::kOpenDrain, false);
    REQUIRE(asserted >= 0);
    CHECK(pins2.log[static_cast<std::size_t>(asserted)].at_ms == kResetSpacingMs);
  }

  SUBCASE("a power cut: no record, the worst is assumed") {
    rig.retained.powerCut();
    rig.clock.advance(50);
    RadioPins after(rig.pins, rig.clock, rig.retained);
    CHECK(after.msUntilResetAllowed() == kResetSpacingMs);
  }
}

TEST_CASE("a kill cycle: release everything, kill high, wait, kill released, settle, then the UART may come back") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  REQUIRE(rp.uartEnable() == Outcome::kOk);
  REQUIRE(rp.swdAcquire() == Outcome::kOk);
  REQUIRE(rp.swd().driveIo(true));
  CHECK(rig.pins.driving(kWalterTx.gpio));
  CHECK(rig.pins.driving(kSwdClk.gpio));
  CHECK(rig.pins.driving(kSwdIo.gpio));
  rig.pins.clearLog();
  const std::uint64_t t0 = rig.clock.now_ms;

  bool seen_off = false;
  rig.clock.on_delay = [&](std::uint32_t) {
    if (!rp.killDriven()) return;
    // In the middle of the kill: the rail is off and nothing may face the radio.
    seen_off = true;
    CHECK(rp.power() == RadioPower::kOff);
    CHECK(rp.swdAcquire() == Outcome::kRailOff);
    CHECK(rp.uartEnable() == Outcome::kRailOff);
    CHECK(rp.resetPulse() == Outcome::kRailOff);
    CHECK(!rp.swd().clock(true));
    for (const core::board::PinDef& pin : core::board::kRadioPins) {
      if (pin.gpio == kPwrKill.gpio) continue;
      CHECK_MESSAGE(rig.pins.modeOf(pin.gpio) == PinMode::kInput, pin.net);
    }
  };
  REQUIRE(rp.killCycle(2000) == Outcome::kOk);
  rig.clock.on_delay = nullptr;
  CHECK(seen_off);

  const int tx_in = rig.pins.indexOf(kWalterTx.gpio, OpKind::kMode, PinMode::kInput, false);
  const int clk_in = rig.pins.indexOf(kSwdClk.gpio, OpKind::kMode, PinMode::kInput, false);
  const int io_in = rig.pins.indexOf(kSwdIo.gpio, OpKind::kMode, PinMode::kInput, false);
  const int kill_high = rig.pins.indexOf(kPwrKill.gpio, OpKind::kLevel, PinMode::kInput, true);
  const int kill_out = rig.pins.indexOf(kPwrKill.gpio, OpKind::kMode, PinMode::kOutput, false);
  const int kill_in = rig.pins.indexOf(kPwrKill.gpio, OpKind::kMode, PinMode::kInput, false);
  const int tx_pull = rig.pins.indexOf(kWalterTx.gpio, OpKind::kMode, PinMode::kInputPullUp, false);
  REQUIRE(tx_in >= 0);
  REQUIRE(clk_in >= 0);
  REQUIRE(io_in >= 0);
  REQUIRE(kill_high >= 0);
  REQUIRE(kill_out >= 0);
  REQUIRE(kill_in >= 0);
  REQUIRE(tx_pull >= 0);
  // The order: everything released, then the level, then the drive, then release, then TX back.
  CHECK(tx_in < kill_high);
  CHECK(clk_in < kill_high);
  CHECK(io_in < kill_high);
  CHECK(kill_high < kill_out);
  CHECK(kill_out < kill_in);
  CHECK(kill_in < tx_pull);
  // The times: off for exactly what was asked, then the settle time.
  CHECK(rig.pins.log[static_cast<std::size_t>(kill_out)].at_ms == t0);
  CHECK(rig.pins.log[static_cast<std::size_t>(kill_in)].at_ms == t0 + 2000);
  CHECK(rig.pins.log[static_cast<std::size_t>(tx_pull)].at_ms == t0 + 2000 + kKillTiming.settle_ms);
  // The kill line was never driven low: it went high, then input.
  for (const fakes::Transition& x : rig.pins.log) {
    if (x.op.gpio != kPwrKill.gpio) continue;
    CHECK(!(x.op.kind == OpKind::kLevel && !x.op.level));
  }
  // Afterwards: the safe state, the rail on, the UART and SWD taken back from their holders.
  CHECK(rp.power() == RadioPower::kOn);
  CHECK(!rp.uartEnabled());
  CHECK(!rp.swdHeld());
  CHECK(!rp.swd().clock(true));
  CHECK(!rig.pins.driving(kSwdClk.gpio));
  CHECK(rig.pins.modeOf(kPwrKill.gpio) == PinMode::kInput);
  CHECK(rp.uartEnable() == Outcome::kOk);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("a kill outside its bounds is refused before any pin is touched") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  rig.pins.clearLog();
  CHECK(rp.killCycle(kKillTiming.min_ms - 1) == Outcome::kKillTooShort);
  CHECK(rp.killCycle(0) == Outcome::kKillTooShort);
  CHECK(rp.killCycle(kKillTiming.max_ms + 1) == Outcome::kKillTooLong);
  CHECK(rp.killCycle(0xFFFFFFFFu) == Outcome::kKillTooLong);
  CHECK(rig.pins.log.empty());
  CHECK(rig.clock.delayed_ms == 0);  // and no time was spent
  CHECK(rp.killCycle(kKillTiming.min_ms) == Outcome::kOk);
  CHECK(rp.killCycle(kKillTiming.max_ms) == Outcome::kOk);
}

TEST_CASE("a failure injected in the middle of a kill ends in the safe state") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  REQUIRE(rp.uartEnable() == Outcome::kOk);

  SUBCASE("the driver refuses to drive the kill line") {
    int refusals = 0;
    rig.pins.fail_when = [&](const fakes::Op& op) {
      const bool hit = op.kind == OpKind::kMode && op.gpio == kPwrKill.gpio && op.mode == PinMode::kOutput;
      if (hit) refusals++;
      return hit && refusals == 1;
    };
    CHECK(rp.killCycle(3000) == Outcome::kPinRefused);
    CHECK(refusals == 1);
  }
  SUBCASE("the driver refuses to release the kill line (it is released by the safe state anyway)") {
    int refusals = 0;
    rig.pins.fail_when = [&](const fakes::Op& op) {
      const bool hit = op.kind == OpKind::kMode && op.gpio == kPwrKill.gpio && op.mode == PinMode::kInput;
      if (hit) refusals++;
      return hit && refusals == 1;
    };
    CHECK(rp.killCycle(3000) == Outcome::kPinRefused);
    CHECK(refusals == 2);  // once refused in killCycle, once accepted in safe()
  }
  SUBCASE("the driver refuses to release the UART's pin before the kill") {
    int refusals = 0;
    rig.pins.fail_when = [&](const fakes::Op& op) {
      const bool hit = op.kind == OpKind::kMode && op.gpio == kWalterTx.gpio && op.mode == PinMode::kInput;
      if (hit) refusals++;
      return hit && refusals == 1;
    };
    CHECK(rp.killCycle(3000) == Outcome::kPinRefused);
    CHECK(!rig.pins.killDriven());  // it never got that far
  }

  // In every case: the safe state. Nothing driven; nothing pulled either while the rail is
  // "unknown" (the kill line was driven and released: the settle time), TX pulled up when the
  // kill never got that far; the UART and SWD released; and the next operations work.
  rig.pins.fail_when = nullptr;
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    CHECK_MESSAGE(!rig.pins.driving(pin.gpio), pin.net);
    if (pin.gpio == kWalterTx.gpio) {
      CHECK(rig.pins.modeOf(pin.gpio) ==
            (rp.power() == RadioPower::kOn ? PinMode::kInputPullUp : PinMode::kInput));
    } else {
      CHECK_MESSAGE(rig.pins.modeOf(pin.gpio) == PinMode::kInput, pin.net);
    }
  }
  CHECK(!rp.killDriven());
  CHECK(!rp.uartEnabled());
  CHECK(!rp.swdHeld());
  CHECK(!rp.resetAsserted());
  CHECK(rp.power() != RadioPower::kOff);
  rig.clock.advance(kKillTiming.settle_ms);
  CHECK(rp.power() == RadioPower::kOn);
  CHECK(rp.uartEnable() == Outcome::kOk);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("SWD: not while the rail is settling, not while reset is asserted, not twice; dead after release") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  // Straight after boot the rail is settling: acquiring waits for it (bounded by settle_ms).
  CHECK(rp.power() == RadioPower::kUnknown);
  REQUIRE(rp.swdAcquire() == Outcome::kOk);
  CHECK(rig.clock.delayed_ms == kKillTiming.settle_ms);
  CHECK(rp.power() == RadioPower::kOn);
  CHECK(rig.pins.modeOf(kSwdClk.gpio) == PinMode::kOutput);
  CHECK(!rig.pins.levelOf(kSwdClk.gpio));  // idle low
  CHECK(rig.pins.modeOf(kSwdIo.gpio) == PinMode::kInput);
  CHECK(rp.swdAcquire() == Outcome::kSwdAlreadyHeld);

  // The lines work while held.
  CHECK(rp.swd().clock(true));
  CHECK(rig.pins.levelOf(kSwdClk.gpio));
  CHECK(rp.swd().clock(false));
  CHECK(rp.swd().driveIo(true));
  CHECK(rig.pins.driving(kSwdIo.gpio));
  CHECK(rp.swd().releaseIo());
  CHECK(!rig.pins.driving(kSwdIo.gpio));
  rig.pins.setExternalLevel(kSwdIo.gpio, true);
  CHECK(rp.swd().readIo());

  // A reset while SWD is held is refused: the holder releases first. So the clock can never
  // toggle while reset is asserted.
  CHECK(rp.resetPulse() == Outcome::kSwdHeld);
  CHECK(!rp.resetAsserted());

  // Released: dead.
  rp.swdRelease();
  CHECK(!rp.swdHeld());
  CHECK(!rp.swd().clock(true));
  CHECK(!rp.swd().driveIo(true));
  CHECK(!rp.swd().releaseIo());
  CHECK(!rp.swd().readIo());
  CHECK(rig.pins.modeOf(kSwdClk.gpio) == PinMode::kInput);
  CHECK(rig.pins.modeOf(kSwdIo.gpio) == PinMode::kInput);

  // While reset is asserted (in the middle of the pulse): acquiring is refused and the clock
  // line does not move.
  bool seen = false;
  rig.clock.on_delay = [&](std::uint32_t) {
    if (!rp.resetAsserted()) return;
    seen = true;
    CHECK(rp.swdAcquire() == Outcome::kResetAsserted);
    CHECK(!rp.swd().clock(true));
    CHECK(rig.pins.modeOf(kSwdClk.gpio) == PinMode::kInput);
  };
  REQUIRE(rp.resetPulse() == Outcome::kOk);
  rig.clock.on_delay = nullptr;
  CHECK(seen);
  CHECK(rig.pins.countViolations("swdclk-while-reset") == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("the UART: TX driven high only between enable and release; pulled up, not driven, otherwise") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  REQUIRE(rp.uartEnable() == Outcome::kOk);  // waited for the rail
  CHECK(rig.clock.delayed_ms == kKillTiming.settle_ms);
  CHECK(rp.uartEnabled());
  CHECK(rig.pins.modeOf(kWalterTx.gpio) == PinMode::kOutput);
  CHECK(rig.pins.levelOf(kWalterTx.gpio));
  CHECK(rig.pins.modeOf(kWalterRx.gpio) == PinMode::kInput);
  // The level was written before the pin was made an output: the first driven level is high.
  const int level = rig.pins.indexOf(kWalterTx.gpio, OpKind::kLevel, PinMode::kInput, true);
  const int out = rig.pins.indexOf(kWalterTx.gpio, OpKind::kMode, PinMode::kOutput, false);
  REQUIRE(level >= 0);
  REQUIRE(out >= 0);
  CHECK(level < out);

  rp.uartRelease();
  CHECK(!rp.uartEnabled());
  CHECK(rig.pins.modeOf(kWalterTx.gpio) == PinMode::kInputPullUp);
  CHECK(!rig.pins.driving(kWalterTx.gpio));
  CHECK(rig.pins.wireLevel(kWalterTx.gpio));  // idle high all the same
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("holds for sleep go on after the safe state and come off after it is configured again") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  REQUIRE(rp.uartEnable() == Outcome::kOk);
  rig.pins.clearLog();
  REQUIRE(rp.holdForSleep());
  // Every one of the seven is held, and every mode change came before the first hold.
  std::size_t first_hold = rig.pins.log.size();
  std::size_t last_mode = 0;
  std::size_t holds = 0;
  for (std::size_t i = 0; i < rig.pins.log.size(); ++i) {
    const fakes::Transition& t = rig.pins.log[i];
    if (t.op.kind == OpKind::kHold && t.op.hold) {
      holds++;
      if (i < first_hold) first_hold = i;
    } else if (t.op.kind == OpKind::kMode) {
      last_mode = i;
    }
  }
  CHECK(holds == RadioPins::kPinCount);
  CHECK(last_mode < first_hold);
  for (const core::board::PinDef& pin : core::board::kRadioPins) CHECK(rig.pins.held(pin.gpio));
  CHECK(!rp.uartEnabled());
  CHECK(rig.pins.modeOf(kWalterTx.gpio) == PinMode::kInputPullUp);  // high through the sleep
  CHECK(!rig.pins.driving(kWalterTx.gpio));

  // Waking: the same state configured (the pads are still held, so nothing moves), then released.
  rig.pins.clearLog();
  REQUIRE(rp.releaseHolds());
  first_hold = rig.pins.log.size();
  last_mode = 0;
  for (std::size_t i = 0; i < rig.pins.log.size(); ++i) {
    const fakes::Transition& t = rig.pins.log[i];
    if (t.op.kind == OpKind::kHold && !t.op.hold && i < first_hold) first_hold = i;
    if (t.op.kind == OpKind::kMode) last_mode = i;
  }
  CHECK(last_mode < first_hold);
  for (const core::board::PinDef& pin : core::board::kRadioPins) CHECK(!rig.pins.held(pin.gpio));
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("state() says what each pin is doing") {
  Rig rig;
  RadioPins rp(rig.pins, rig.clock, rig.retained);
  settle(rig, rp);
  CHECK(std::string(rp.state(0).net) == "LORA_nRESET");
  CHECK(rp.state(0).gpio == 1);
  CHECK(rp.state(0).dsub == 3);
  CHECK(rp.state(0).level);  // the nRF52's pull-up, read through the fake
  CHECK(std::string(rp.state(0).use) == "released");
  CHECK(std::string(rp.state(3).net) == "LORA_PWR_KILL");
  CHECK(rp.state(5).mode == PinMode::kInputPullUp);
  REQUIRE(rp.uartEnable() == Outcome::kOk);
  CHECK(std::string(rp.state(5).use) == "uart");
  CHECK(rp.state(5).mode == PinMode::kOutput);
  CHECK(std::string(rp.state(6).use) == "uart");
  REQUIRE(rp.swdAcquire() == Outcome::kOk);
  CHECK(std::string(rp.state(1).use) == "swd");
  CHECK(rp.state(1).mode == PinMode::kOutput);
  CHECK(rp.state(2).mode == PinMode::kInput);
  REQUIRE(rp.swd().driveIo(false));
  CHECK(rp.state(2).mode == PinMode::kOutput);
  CHECK(rp.state(99).net == rp.state(6).net);  // out of range clamps
  CHECK(std::string(core::board::describe(Outcome::kOk)) == "ok");
  CHECK(std::string(core::board::describe(RadioPower::kOn)) == "on");
}

TEST_CASE("the kill timing placeholders are what the report says, and consistent") {
  CHECK(kKillTiming.min_ms == 1000);
  CHECK(kKillTiming.max_ms == 30000);
  CHECK(kKillTiming.settle_ms == 200);
  CHECK(kKillTiming.min_ms <= kKillTiming.max_ms);
  CHECK(kResetPulseMs == 100);
  CHECK(kResetSpacingMs == 1000);
}
