// Two radios behind one set of wires (task p1-06), against the simulated selector of
// host_test/sim/selector.h: the board does not exist and nothing in the firmware constructs the
// DualTarget these tests drive, which is exactly why the seam needs testing - these are the only
// proof that "which radio" is a parameter and not a wish.
//
// What is shown: an image flashed into slot 1 and slot 0 untouched; a HELLO that reaches module A
// and not module B; the power-on default; a reset pulse that reaches one module only; and the
// guard that stops an operation on one slot from steering the wires out from under another.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/board/slot.h"
#include "core/flasher/board_profile.h"
#include "core/flasher/flasher.h"
#include "core/flasher/flasher_bench.h"
#include "sim/dual_bench.h"

using core::board::kSlot0;
using core::board::kSlot1;
using core::board::SelectResult;
using core::flasher::Image;
using core::flasher::MemoryImage;

namespace {

// Three pages of plausible application, as the flasher tests build them.
std::vector<std::uint8_t> appImage() { return sim::makeImage(3, false); }

}  // namespace

TEST_CASE("the power-on default is module A, and a unit that asks for nothing talks to A") {
  sim::DualBench b;
  CHECK(b.selector.debugSlot() == kSlot0);
  CHECK(b.selector.uartSlot() == kSlot0);
  CHECK(b.client.slot() == kSlot0);

  REQUIRE(b.client.hello().ok());
  CHECK(b.client.session().boot_id == b.radio_a.boot_id);
  CHECK(b.radio_b.writes.empty());  // module B was never written to
  CHECK(b.pins.violations.empty());
}

TEST_CASE("hello on slot 1 reaches module B, and slot 0's session is module A's") {
  sim::DualBench b;
  REQUIRE(b.client.hello().ok());
  const std::uint32_t a_boot = b.client.session().boot_id;
  CHECK(a_boot == b.radio_a.boot_id);

  REQUIRE(b.client.useSlot(kSlot1));
  REQUIRE(b.client.hello().ok());
  CHECK(b.selector.uartSlot() == kSlot1);
  CHECK(b.client.session().boot_id == b.radio_b.boot_id);
  CHECK(!b.radio_b.writes.empty());

  // And slot 0's session is still module A's: two radios, two sessions.
  CHECK(b.client.sessionOf(kSlot0).boot_id == a_boot);
  CHECK(b.client.sessionOf(kSlot1).boot_id == b.radio_b.boot_id);
  REQUIRE(b.client.useSlot(kSlot0));
  CHECK(b.client.session().boot_id == a_boot);
  CHECK(b.pins.violations.empty());
}

TEST_CASE("a flash of slot 1 reaches module B's chip and leaves module A's flash alone") {
  sim::DualBench b;
  const std::vector<std::uint8_t> image = appImage();
  const std::vector<std::uint8_t> sha = sim::digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.chip_a.onlyTouch(0, 0);  // nothing of module A may be erased or written
  b.chip_b.onlyTouch(0x26000, 0x26000 + 3 * 4096);
  b.chip_b.alsoTouch(core::flasher::kRak4630Single.settings_page,
                     core::flasher::kRak4630Single.settings_page + 4096);

  const core::flasher::Result r =
      b.flasher.flash(img, core::flasher::kRak4630Single, nullptr, kSlot1);
  REQUIRE_MESSAGE(r.ok(), std::string(core::flasher::describe(r.step)) << ": " << std::string(r.why));
  CHECK(b.selector.debugSlot() == kSlot1);

  // Module B holds the image; module A's flash log is empty and its region still blank.
  for (std::size_t i = 0; i < image.size(); i += 4) {
    std::uint32_t want = 0;
    for (unsigned k = 0; k < 4; ++k) want |= static_cast<std::uint32_t>(image[i + k]) << (8 * k);
    REQUIRE(b.chip_b.flashWord(static_cast<std::uint32_t>(0x26000 + i)) == want);
  }
  CHECK(sim::nothingWritten(b.chip_a));
  CHECK(b.chip_a.flashWord(0x26000) == 0xFFFFFFFF);
  CHECK(b.chip_a.counters().tripwire == 0);
  CHECK(b.chip_b.counters().tripwire == 0);
  CHECK(!b.board.swdHeld());
  CHECK(b.pins.violations.empty());
}

TEST_CASE("a flash of slot 0 after one of slot 1 goes to module A, and B keeps what it was given") {
  sim::DualBench b;
  const std::vector<std::uint8_t> image = appImage();
  const std::vector<std::uint8_t> sha = sim::digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};

  REQUIRE(b.flasher.flash(img, core::flasher::kRak4630Single, nullptr, kSlot1).ok());
  const std::size_t b_log = b.chip_b.log().size();
  REQUIRE(b.flasher.flash(img, core::flasher::kRak4630Single, nullptr, kSlot0).ok());
  CHECK(b.selector.debugSlot() == kSlot0);
  CHECK(b.chip_a.flashWord(0x26000) != 0xFFFFFFFF);
  CHECK(b.chip_b.log().size() == b_log);  // nothing more happened on B
  CHECK(b.pins.violations.empty());
}

TEST_CASE("a reset pulse reaches the selected module only, and the window reaches neither") {
  sim::DualBench b;
  REQUIRE(b.dual.select(kSlot1) == SelectResult::kOk);
  REQUIRE(b.board.resetPulse() == core::board::Outcome::kOk);
  CHECK(b.selector.resets(kSlot1) == 1);
  CHECK(b.selector.resets(kSlot0) == 0);

  REQUIRE(b.dual.select(kSlot0) == SelectResult::kOk);
  REQUIRE(b.board.resetPulse() == core::board::Outcome::kOk);
  CHECK(b.selector.resets(kSlot0) == 1);
  CHECK(b.selector.resets(kSlot1) == 1);

  // Two selections and two pulses: the two windows were filtered out, the two pulses were not.
  CHECK(b.selector.windows() == 2);
  CHECK(b.selector.resetsDelivered() == 2);
  CHECK(b.pins.violations.empty());
}

TEST_CASE("the guard: another slot cannot be selected while an operation holds one") {
  sim::DualBench b;
  const core::board::SlotHold hold(b.slots, kSlot1);
  REQUIRE(hold.ok());
  CHECK(b.selector.debugSlot() == kSlot1);

  const core::board::SlotHold other(b.slots, kSlot0);
  CHECK(!other.ok());
  CHECK(other.result() == SelectResult::kBusy);
  CHECK(b.selector.debugSlot() == kSlot1);  // nothing was steered
  // ... and the link client cannot move to another radio while this hold stands, so a request
  // on it cannot even be begun.
  REQUIRE(b.client.slot() == kSlot0);
  CHECK(b.client.useSlot(kSlot0));   // already there: nothing to move, nothing refused
  CHECK(!b.client.useSlot(kSlot1));  // and it cannot move away while slot 1 is held elsewhere
  CHECK(b.client.hello().fault == core::link::client::Fault::kSlotBusy);
  CHECK(b.pins.violations.empty());
}

TEST_CASE("the guard holds mid-request: the selection cannot move and the request is unharmed") {
  sim::DualBench b;
  // Module A answers a second late, so the client is inside its response wait when the hook runs.
  b.radio_a.answer_delay_ms = 20;
  bool tried = false;
  b.clock.on_delay = [&](std::uint32_t) {
    if (tried || !b.slots.held()) return;
    tried = true;
    // Another slot, from the middle of the exchange: refused, and the wires do not move.
    CHECK(b.slots.take(kSlot1) == SelectResult::kBusy);
    CHECK(!b.client.useSlot(kSlot1));
    CHECK(b.selector.uartSlot() == kSlot0);
  };
  const auto hello = b.client.hello();
  b.clock.on_delay = nullptr;
  CHECK(tried);
  REQUIRE(hello.ok());
  CHECK(hello.attempts == 1);  // no retry: nothing corrupted the exchange
  CHECK(b.client.session().boot_id == b.radio_a.boot_id);
  CHECK(b.client.slot() == kSlot0);
  CHECK(b.pins.violations.empty());
}

TEST_CASE("the selector is programmed before every operation, as the document asks") {
  sim::DualBench b;
  const unsigned before = b.board.selectorPrograms();
  REQUIRE(b.client.hello().ok());
  CHECK(b.board.selectorPrograms() > before);
  const unsigned after_hello = b.board.selectorPrograms();
  REQUIRE(b.client.ping(1).ok());
  CHECK(b.board.selectorPrograms() > after_hello);
  CHECK(b.pins.violations.empty());
}
