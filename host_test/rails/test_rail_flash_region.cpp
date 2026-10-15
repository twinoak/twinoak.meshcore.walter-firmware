// Rail R4 (task p3-06): **the flasher writes only inside the board's application region and the
// bootloader's settings page** - never the file system (the node's identity and settings), never
// the SoftDevice, never the bootloader's own code, never the MBR parameter page, never UICR
// (docs/ARCHITECTURE.md §5).
//
// Like the pin rails, this is the suite's and not one test's. The simulated chip fails the
// running test at any erase or write outside the window the test declared with `onlyTouch()` and
// `alsoTouch()`, and - since this task - fails a test that flashed without declaring a window at
// all, at the chip's destructor. So "asserted at the end of every test that flashes" is a
// property of the simulator. This file prints what the whole suite did to simulated flash and
// holds both counts to zero.
//
// The grep half - that core/nrf52/nrf52.h has no erase or write without a FlashRegion, so the
// region cannot be forgotten by a caller - is tools/check_rails.py.
#include <doctest.h>

#include <cstdio>
#include <string>

#include "core/flasher/board_profile.h"
#include "sim/nrf52_bench.h"
#include "sim/nrf52_chip.h"

namespace {

// An address as a person reads a flash map.
std::string hex(std::uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "0x%06x", value);
  return std::string(buffer);
}

}  // namespace

TEST_CASE("R4: over the whole suite, nothing was erased or written outside a declared region") {
  const sim::FlashTally& tally = sim::flashTally();
  CHECK(tally.outside == 0);
  CHECK(tally.undeclared == 0);
  // And the suite really did flash: a tally of zero over no erases at all proves nothing.
  CHECK(tally.flashed > 10);
  CHECK(tally.erases > 100);
  CHECK(tally.writes > 10000);
  // std::string, not the const char*: doctest streams a char pointer as a pointer, and a message
  // that cannot name the board is no use (host_test/fakes/fake_pins.cpp says the same).
  const core::flasher::BoardProfile& p = core::flasher::kRak4630Single;
  MESSAGE("simulated flash over the whole suite: " << tally.chips << " chips, " << tally.flashed
          << " of them erased or written (" << tally.erases << " page erases, " << tally.writes
          << " word writes); " << tally.outside
          << " operations outside the declared region, " << tally.undeclared
          << " chips flashed by a test that declared none. The region a unit's flasher may touch "
          << "is the board profile's: " << std::string(p.name) << " " << hex(p.app.begin) << "-"
          << hex(p.app.end) << " and the settings page at " << hex(p.settings_page));
}

TEST_CASE("R4: the guard bites - an erase outside the declared window is counted and reported") {
  // The file system's first page (`0xED000`, where the board profile's application region ends:
  // the node's identity and its settings live above it). A test about the flash controller may
  // declare the whole chip and touch it; a test that declared the application region may not.
  const unsigned outside_before = sim::flashTally().outside;
  SUBCASE("declared: the whole chip, which is what the nRF52 layer's own tests declare") {
    sim::Nrf52Bench b;
    b.chip.onlyTouch(0, b.chip.flashBytes());
    REQUIRE(b.open().ok());
    const core::nrf52::FlashRegion all{0, 0x100000};
    CHECK(b.nrf.erasePage(all, 0xED000).ok());
    CHECK(b.chip.errors().empty());
    CHECK(sim::flashTally().outside == outside_before);
    b.close();
  }
  SUBCASE("not declared: the same erase, with the application region declared, is a violation") {
    sim::Nrf52Bench b;
    b.chip.onlyTouch(core::flasher::kRak4630Single.app.begin,
                     core::flasher::kRak4630Single.app.end);
    b.chip.strict = false;          // the sentence is what this subcase is about
    b.chip.report_outside = false;  // ... and it must not dirty the suite's tally above
    REQUIRE(b.open().ok());
    const core::nrf52::FlashRegion all{0, 0x100000};  // a region the nRF52 layer would allow
    CHECK(b.nrf.erasePage(all, 0xED000).ok());
    CHECK(b.chip.countErrors("outside the region the test allowed") == 1);
    CHECK(sim::flashTally().outside == outside_before);
    MESSAGE("the chip said: " << b.chip.errors()[0]);
    b.chip.clearErrors();
    b.close();
  }
}
