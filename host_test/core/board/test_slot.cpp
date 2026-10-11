// The slot seam itself (task p1-06): the value type, the two TargetSelects, the guard that keeps
// one selection at a time, and the sequence DualTarget puts on the wires - held against the
// hardware repository's DUAL-RAK4630-FEASIBILITY.md §3 through the simulated selector.
#include <doctest.h>

#include <string>

#include "core/board/dual_target.h"
#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "sim/selector.h"

using core::board::DualTarget;
using core::board::kKillTiming;
using core::board::kSelectorWindowMaxUs;
using core::board::kSelectorWindowMinUs;
using core::board::kSlot0;
using core::board::kSlot1;
using core::board::Outcome;
using core::board::RadioPins;
using core::board::SelectResult;
using core::board::SingleTarget;
using core::board::Slot;
using core::board::SlotHold;
using core::board::SlotLock;
using core::board::TargetSelect;

namespace {

// A Walter whose wires end at a dual adapter's selector. The firmware never builds this: it is
// here so that DualTarget has something to talk to.
struct SelectorRig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  sim::Selector selector{pins, clock};
  fakes::FakeRetained retained;
  RadioPins board{selector, clock, retained};
  DualTarget dual{board};
  SlotLock slots{dual};

  SelectorRig() {
    clock.advance(kKillTiming.settle_ms);
    board.safe();
  }
};

}  // namespace

// ------------------------------------------------------------------ the value type

TEST_CASE("a slot is 0 or 1, and 0 by default") {
  Slot def;
  CHECK(def.index == 0);
  CHECK(def == kSlot0);
  CHECK(def != kSlot1);
  CHECK(kSlot1.index == 1);
  CHECK(core::board::kSlotCount == 2);
  CHECK(core::board::isSlot(0));
  CHECK(core::board::isSlot(1));
  CHECK(!core::board::isSlot(2));
  CHECK(!core::board::isSlot(255));
}

// ------------------------------------------------------------------ the single adapter

TEST_CASE("SingleTarget: slot 0 is the only one, and selecting it does nothing at all") {
  SingleTarget single;
  CHECK(single.slots() == 1);
  CHECK(single.selected() == kSlot0);
  CHECK(single.select(kSlot0) == SelectResult::kOk);
  CHECK(single.selects() == 1);
  CHECK(single.select(kSlot1) == SelectResult::kNoSuchSlot);
  CHECK(single.select(Slot{7}) == SelectResult::kNoSuchSlot);
  CHECK(single.refusals() == 2);
  CHECK(single.selected() == kSlot0);
}

TEST_CASE("the guard: one selection at a time, nested takes of the same slot, another refused") {
  SingleTarget single;
  SlotLock lock(single);
  CHECK(lock.slots() == 1);
  CHECK(lock.exists(kSlot0));
  CHECK(!lock.exists(kSlot1));
  CHECK(!lock.held());

  {
    const SlotHold outer(lock, kSlot0);
    REQUIRE(outer.ok());
    CHECK(lock.held());
    CHECK(lock.slot() == kSlot0);
    CHECK(single.selects() == 1);
    {
      // An operation inside an operation, on the same slot: allowed, and the selector is not
      // programmed again - it is the same selection.
      const SlotHold inner(lock, kSlot0);
      CHECK(inner.ok());
      CHECK(lock.depth() == 2);
      CHECK(single.selects() == 1);
    }
    CHECK(lock.depth() == 1);
    // Another slot while one is held: refused, and nothing was steered.
    const SlotHold other(lock, kSlot1);
    CHECK(!other.ok());
    CHECK(other.result() == SelectResult::kNoSuchSlot);  // and it does not exist here either
    CHECK(lock.slot() == kSlot0);
  }
  CHECK(!lock.held());
  CHECK(lock.takes() == 2);
}

namespace {

// Two slots, nothing on the wires: the guard's own behaviour, away from a board.
class BothSlots final : public TargetSelect {
 public:
  std::size_t slots() const override { return 2; }
  SelectResult select(Slot slot) override {
    selected_ = slot;
    selects++;
    return SelectResult::kOk;
  }
  Slot selected() const override { return selected_; }
  int selects = 0;

 private:
  Slot selected_{};
};

}  // namespace

TEST_CASE("the guard with two slots: another slot mid-operation is kBusy, never steered") {
  BothSlots both;
  SlotLock lock(both);
  CHECK(lock.exists(kSlot1));
  const SlotHold hold(lock, kSlot1);
  REQUIRE(hold.ok());
  CHECK(both.selected() == kSlot1);
  CHECK(both.selects == 1);

  const SlotHold other(lock, kSlot0);
  CHECK(!other.ok());
  CHECK(other.result() == SelectResult::kBusy);
  CHECK(both.selects == 1);        // nothing was programmed
  CHECK(both.selected() == kSlot1);  // and nothing was steered
  CHECK(lock.refusals() == 1);
}

TEST_CASE("a slot that is not a position of any adapter is refused by the lock") {
  BothSlots both;
  SlotLock lock(both);
  const SlotHold hold(lock, Slot{2});
  CHECK(!hold.ok());
  CHECK(hold.result() == SelectResult::kNoSuchSlot);
  CHECK(both.selects == 0);
}

// ------------------------------------------------------------------ the dual adapter's selector

TEST_CASE("the power-on state is module A: the flip-flops' RC clear (feasibility §3)") {
  SelectorRig rig;
  CHECK(!rig.selector.debugSel());
  CHECK(!rig.selector.uartSel());
  CHECK(rig.selector.debugSlot() == kSlot0);
  CHECK(rig.selector.uartSlot() == kSlot0);
  CHECK(rig.selector.shifts() == 0);
  CHECK(rig.dual.slots() == 2);
  CHECK(rig.dual.selected() == kSlot0);
  CHECK(!rig.dual.everSelected());
}

TEST_CASE("DualTarget::select steers both flip-flops, in one filtered window of 100-500 us") {
  SelectorRig rig;
  REQUIRE(rig.dual.select(kSlot1) == SelectResult::kOk);
  CHECK(rig.selector.debugSel());
  CHECK(rig.selector.uartSel());
  CHECK(rig.selector.debugSlot() == kSlot1);
  CHECK(rig.selector.uartSlot() == kSlot1);
  CHECK(rig.selector.shifts() == 2);  // two bits, two rising shift clocks
  CHECK(rig.dual.selected() == kSlot1);
  CHECK(rig.dual.everSelected());
  CHECK(rig.board.selectorPrograms() == 1);

  // The window: filtered out by the adapter's RC, so no radio was reset, and inside the
  // document's 100-500 us.
  CHECK(rig.selector.windows() == 1);
  CHECK(rig.selector.resetsDelivered() == 0);
  CHECK(rig.pins.selector_windows == 1);
  CHECK(rig.pins.selector_clocks >= 2);
  CHECK(rig.pins.last_selector_window_us >= kSelectorWindowMinUs);
  CHECK(rig.pins.last_selector_window_us <= kSelectorWindowMaxUs);
  CHECK(rig.pins.violations.empty());

  // And back to A.
  rig.clock.advance(core::board::kResetSpacingMs);
  REQUIRE(rig.dual.select(kSlot0) == SelectResult::kOk);
  CHECK(!rig.selector.debugSel());
  CHECK(!rig.selector.uartSel());
  CHECK(rig.selector.shifts() == 4);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("the bits go in FF2 first: uart_sel reaches the second flip-flop, debug_sel the first") {
  SelectorRig rig;
  // The one asymmetric case: the UART to B and the debug lines to A. If the order were the other
  // way round, these two would come out swapped.
  REQUIRE(rig.board.selectorProgram(true, false) == Outcome::kOk);
  CHECK(rig.selector.uartSel());
  CHECK(!rig.selector.debugSel());

  rig.clock.advance(core::board::kResetSpacingMs);
  REQUIRE(rig.board.selectorProgram(false, true) == Outcome::kOk);
  CHECK(!rig.selector.uartSel());
  CHECK(rig.selector.debugSel());
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("the window leaves the lines as it found them, and the pins back in the safe state") {
  SelectorRig rig;
  REQUIRE(rig.board.selectorProgram(true, true) == Outcome::kOk);
  CHECK(!rig.board.resetAsserted());
  CHECK(!rig.board.swdHeld());
  CHECK(rig.pins.modeOf(core::board::kSwdClk.gpio) == interfaces::PinMode::kInput);
  CHECK(rig.pins.modeOf(core::board::kSwdIo.gpio) == interfaces::PinMode::kInput);
  CHECK(rig.pins.modeOf(core::board::kNReset.gpio) == interfaces::PinMode::kInput);
  // The SWD lines can be acquired straight afterwards: the selector left nothing held.
  CHECK(rig.board.swdAcquire() == Outcome::kOk);
  rig.board.swdRelease();
}

TEST_CASE("the reset the window implies is spaced like any other reset (rule 2)") {
  SelectorRig rig;
  REQUIRE(rig.board.selectorProgram(false, false) == Outcome::kOk);
  // The record was written: the next pulse - of either kind - waits out the second.
  CHECK(rig.board.msUntilResetAllowed() > 0);
  const std::uint64_t before = rig.clock.now_ms;
  REQUIRE(rig.board.selectorProgram(true, true) == Outcome::kOk);
  CHECK(rig.clock.now_ms - before >= core::board::kResetSpacingMs);
  CHECK(rig.pins.violations.empty());

  // And a real reset after a window waits too, and is not filtered out: module B gets it.
  CHECK(rig.selector.debugSlot() == kSlot1);
  REQUIRE(rig.board.resetPulse() == Outcome::kOk);
  CHECK(rig.selector.resets(kSlot1) == 1);
  CHECK(rig.selector.resets(kSlot0) == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("the selector is refused while the SWD lines are held, and while the rail is off") {
  SelectorRig rig;
  REQUIRE(rig.board.swdAcquire() == Outcome::kOk);
  CHECK(rig.board.selectorProgram(true, true) == Outcome::kSwdHeld);
  CHECK(rig.dual.select(kSlot1) == SelectResult::kRefused);
  CHECK(rig.dual.lastOutcome() == Outcome::kSwdHeld);
  CHECK(rig.board.selectorPrograms() == 0);
  CHECK(rig.selector.shifts() == 0);
  rig.board.swdRelease();

  // While the kill line is driven: nothing faces the radio, selector included.
  bool tried = false;
  rig.clock.on_delay = [&](std::uint32_t) {
    if (tried || !rig.board.killDriven()) return;
    tried = true;
    CHECK(rig.board.selectorProgram(true, true) == Outcome::kRailOff);
    CHECK(rig.dual.select(kSlot1) == SelectResult::kRefused);
  };
  REQUIRE(rig.board.killCycle(kKillTiming.min_ms) == Outcome::kOk);
  CHECK(tried);
  CHECK(rig.board.selectorPrograms() == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("DualTarget refuses a slot that is not a position of the adapter") {
  SelectorRig rig;
  CHECK(rig.dual.select(Slot{2}) == SelectResult::kNoSuchSlot);
  CHECK(rig.dual.select(Slot{255}) == SelectResult::kNoSuchSlot);
  CHECK(rig.dual.refusals() == 2);
  CHECK(rig.board.selectorPrograms() == 0);
  CHECK(rig.selector.shifts() == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("every SelectResult has words") {
  for (const SelectResult r : {SelectResult::kOk, SelectResult::kNoSuchSlot, SelectResult::kBusy,
                               SelectResult::kRefused}) {
    CHECK(std::string(core::board::describe(r)).size() > 1);
  }
}
