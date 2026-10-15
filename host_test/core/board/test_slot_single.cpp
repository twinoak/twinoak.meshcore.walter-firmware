// A single-radio unit, which is every unit today (spec/SYSTEM.md S-11), with the slot seam in
// place (task p1-06). Two things to show:
//
//   1. **nothing changed.** Slot 0 is what every call means without a slot, and taking it moves
//      no pin at all - so the phase's earlier tests are the proof that the behaviour is the same,
//      and these are the proof that slot 0 costs nothing.
//   2. **slot 1 is refused, everywhere**, and the dual adapter's selector sequence never reaches
//      the wires. The fake pins fail a test the moment SWDCLK moves while nRESET is asserted
//      (`swdclk-while-reset`) unless a selector has been declared on the bench, and no bench of a
//      single-radio unit declares one. The last case here shows that rule firing on purpose.
#include <doctest.h>

#include <string>

#include "core/board/dual_target.h"
#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/flasher/board_profile.h"
#include "core/flasher/flasher_bench.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "sim/link_bench.h"

using core::board::kKillTiming;
using core::board::kSlot0;
using core::board::kSlot1;
using core::board::Outcome;
using core::board::RadioPins;
using core::board::SelectResult;
using core::board::SingleTarget;
using core::board::Slot;
using core::board::SlotLock;

namespace {

// The seven wires of a unit as built: no selector on them, so the fake pins hold every rule of
// docs/ARCHITECTURE.md §2 at its strictest.
struct SingleRig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  RadioPins board{pins, clock, retained};
  SingleTarget single;
  SlotLock slots{single};

  SingleRig() {
    clock.advance(kKillTiming.settle_ms);
    board.safe();
  }
};

}  // namespace

TEST_CASE("the fake pins of a single-radio unit do not allow a selector sequence") {
  SingleRig rig;
  CHECK(!rig.pins.selector_allowed);
}

TEST_CASE("slot 0 costs nothing: taking it moves no pin and leaves the rules untouched") {
  SingleRig rig;
  const std::size_t before = rig.pins.log.size();
  for (int i = 0; i < 5; ++i) {
    const core::board::SlotHold hold(rig.slots, kSlot0);
    CHECK(hold.ok());
  }
  CHECK(rig.pins.log.size() == before);
  CHECK(rig.pins.selector_clocks == 0);
  CHECK(rig.pins.selector_windows == 0);
  CHECK(rig.board.selectorPrograms() == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("slot 1 is refused by the lock, and nothing reaches a pin") {
  SingleRig rig;
  const core::board::SlotHold hold(rig.slots, kSlot1);
  CHECK(!hold.ok());
  CHECK(hold.result() == SelectResult::kNoSuchSlot);
  CHECK(rig.pins.selector_clocks == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("slot 1 has no board profile on this unit, and slot 0 is the one board there is") {
  CHECK(core::flasher::kSlotsPresent == 1);
  CHECK(core::flasher::profileForSlot(kSlot0) == &core::flasher::kRak4630Single);
  CHECK(core::flasher::profileForSlot(kSlot1) == nullptr);
  CHECK(core::flasher::findProfile(kSlot1, "rak4630-single") == nullptr);
}

TEST_CASE("the flasher refuses slot 1 at the first step, with nothing written") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> bytes = sim::makeImage(1, false);
  const std::vector<std::uint8_t> digest = sim::digestOf(bytes);
  const core::flasher::MemoryImage source(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  const core::flasher::Image image{&source, 0x26000, digest.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(bytes.size()));

  const core::flasher::Result r = b.flasher.flash(image, b.profile, nullptr, kSlot1);
  CHECK(r.outcome == core::flasher::Outcome::kRefused);
  CHECK(r.step == core::flasher::Step::kSelect);
  CHECK(sim::nothingWritten(b.chip));
  CHECK(!b.board.swdHeld());
  CHECK(b.pins.selector_clocks == 0);
  // And slot 0, the default, flashes as it always did.
  CHECK(b.flasher.flash(image, b.profile, nullptr).ok());
}

TEST_CASE("the link client: slot 0 is the session it keeps, and slot 1 is refused") {
  sim::LinkBench b;
  CHECK(b.client.slot() == kSlot0);
  CHECK(b.client.useSlot(kSlot0));
  CHECK(!b.client.useSlot(kSlot1));
  CHECK(!b.client.useSlot(Slot{9}));
  CHECK(b.client.slot() == kSlot0);

  // A session on slot 0, and slot 1's session empty - a unit with one radio cannot have one.
  REQUIRE(b.client.hello().ok());
  CHECK(b.client.session().up);
  CHECK(b.client.sessionOf(kSlot0).boot_id == b.radio.boot_id);
  CHECK(!b.client.sessionOf(kSlot1).up);
  CHECK(b.client.modeOf(kSlot1) == core::link::client::Mode::kUnknown);
  CHECK(b.pins.selector_clocks == 0);
  CHECK(b.pins.violations.empty());
}

// The rule firing on purpose: the one operation that may move the clock while reset is asserted,
// called on a bench that has no selector - which is what a single-radio unit's bench is. It is
// the test above's negative, and it failed for exactly this reason while it was being written.
TEST_CASE("the selector sequence on a single-radio unit's wires breaks swdclk-while-reset") {
  SingleRig rig;
  rig.pins.strict = false;  // see the rule fire instead of failing this test
  rig.pins.allow("swdclk-while-reset");  // ... and declare it, or the destructor fails it anyway
  core::board::DualTarget dual(rig.board);  // never constructed by the firmware; here on purpose
  REQUIRE(dual.select(kSlot1) == SelectResult::kOk);
  CHECK(rig.pins.countViolations("swdclk-while-reset") >= 1);
  CHECK(rig.pins.selector_clocks >= 2);
  // And the same window is not held against the reset pulse's length, because it is not a pulse.
  CHECK(rig.pins.countViolations("reset-pulse-length") == 0);
  CHECK(rig.pins.selector_windows == 1);
}
