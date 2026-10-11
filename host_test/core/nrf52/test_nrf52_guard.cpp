// The address guard (task p1-04): erase and write inside the region succeed; one page below, one
// above, straddling the end, a count that wraps the address - refused before any NVMC access, the
// simulator's log empty; a region that is not a region (unaligned, empty, past the flash, over
// UICR) refused as such. The simulator is told the region too, so a guard that let something
// through would fail the test from the other side.
#include <doctest.h>

#include <vector>

#include "sim/nrf52_bench.h"

using core::nrf52::FlashRegion;
using core::nrf52::Result;
using core::nrf52::Status;
namespace regs = core::nrf52;

namespace {

// The RAK4630's application region as the flasher will own it (task p1-05 decides; this is a
// stand-in with the right shape): after the SoftDevice, before the file system and bootloader.
const FlashRegion kApp{0x26000, 0xF0000};

struct GuardBench : sim::Nrf52Bench {
  std::uint32_t data[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  GuardBench() {
    chip.onlyTouch(kApp.begin, kApp.end);
    REQUIRE(open().ok());
  }
  ~GuardBench() {
    CHECK(chip.logInside(kApp.begin, kApp.end));
    CHECK(chip.config() == regs::nvmc::kConfigRen);
  }
  // Nothing reached the flash controller: the log is empty, CONFIG was never written.
  void nothingHappened(const Result& r, Status expected, std::uint32_t address) {
    CHECK(r.status == expected);
    CHECK(r.address == address);
    CHECK(chip.log().empty());
    CHECK(chip.counters().config_writes == 0);
    CHECK(chip.counters().erases == 0);
    CHECK(chip.counters().writes == 0);
    CHECK(chip.counters().rejected_writes == 0);
  }
};

}  // namespace

TEST_CASE("inside the region: the first page, the last page, a write up to the very end") {
  GuardBench b;
  CHECK(b.nrf.erasePage(kApp, kApp.begin).ok());
  CHECK(b.nrf.erasePage(kApp, kApp.end - 4096).ok());
  CHECK(b.nrf.writeWords(kApp, kApp.begin, b.data, 8).ok());
  CHECK(b.nrf.writeWords(kApp, kApp.end - 32, b.data, 8).ok());  // ends exactly at kApp.end
  CHECK(b.chip.counters().erases == 2);
  CHECK(b.chip.counters().writes == 16);
  CHECK(b.nrf.counters().refusals == 0);
  CHECK(b.chip.errors().empty());
  b.close();
}

TEST_CASE("one page below the region: refused before any flash controller access") {
  GuardBench b;
  b.nothingHappened(b.nrf.erasePage(kApp, kApp.begin - 4096), Status::kOutsideRegion, kApp.begin - 4096);
  b.nothingHappened(b.nrf.writeWords(kApp, kApp.begin - 4096, b.data, 8), Status::kOutsideRegion, kApp.begin - 4096);
  b.nothingHappened(b.nrf.writeWords(kApp, kApp.begin - 4, b.data, 1), Status::kOutsideRegion, kApp.begin - 4);
  CHECK(b.nrf.counters().refusals == 3);
  b.close();
}

TEST_CASE("one page above the region: refused before any flash controller access") {
  GuardBench b;
  b.nothingHappened(b.nrf.erasePage(kApp, kApp.end), Status::kOutsideRegion, kApp.end);
  b.nothingHappened(b.nrf.erasePage(kApp, kApp.end + 4096), Status::kOutsideRegion, kApp.end + 4096);
  b.nothingHappened(b.nrf.writeWords(kApp, kApp.end, b.data, 1), Status::kOutsideRegion, kApp.end);
  // The bootloader's page and UICR by address, with a region that does not reach them.
  b.nothingHappened(b.nrf.erasePage(kApp, 0xF4000), Status::kOutsideRegion, 0xF4000);
  b.nothingHappened(b.nrf.writeWords(kApp, regs::uicr::kApprotect, b.data, 1), Status::kOutsideRegion,
                    regs::uicr::kApprotect);
  CHECK(b.chip.counters().tripwire == 0);
  b.close();
}

TEST_CASE("straddling the end: a write that begins inside and ends outside is refused whole") {
  GuardBench b;
  b.nothingHappened(b.nrf.writeWords(kApp, kApp.end - 16, b.data, 8), Status::kOutsideRegion, kApp.end - 16);
  b.nothingHappened(b.nrf.writeWords(kApp, kApp.end - 4, b.data, 2), Status::kOutsideRegion, kApp.end - 4);
  // A page erase cannot straddle (it is page-aligned), but a region of one page and the page
  // after it can be asked for.
  const FlashRegion one{0x40000, 0x41000};
  b.nothingHappened(b.nrf.erasePage(one, 0x41000), Status::kOutsideRegion, 0x41000);
  b.close();
}

TEST_CASE("a word count that would wrap the address is refused, not wrapped") {
  GuardBench b;
  b.nothingHappened(b.nrf.writeWords(kApp, 0x26000, b.data, 0x40000000u), Status::kOutsideRegion, 0x26000);
  b.nothingHappened(b.nrf.writeWords(kApp, 0xEFFFC, b.data, 0x3FFFFFFFu), Status::kOutsideRegion, 0xEFFFC);
  b.close();
}

TEST_CASE("alignment: a page address to the page, a word address to the word") {
  GuardBench b;
  b.nothingHappened(b.nrf.erasePage(kApp, 0x26004), Status::kNotAligned, 0x26004);
  b.nothingHappened(b.nrf.erasePage(kApp, 0x26800), Status::kNotAligned, 0x26800);
  b.nothingHappened(b.nrf.writeWords(kApp, 0x26002, b.data, 1), Status::kNotAligned, 0x26002);
  b.nothingHappened(b.nrf.writeWords(kApp, 0x26000, nullptr, 1), Status::kBadArgument, 0x26000);
  b.nothingHappened(b.nrf.writeWords(kApp, 0x26000, b.data, 0), Status::kBadArgument, 0x26000);
  b.close();
}

TEST_CASE("a region that is not one: unaligned, empty, inverted, past the flash, over UICR or FICR") {
  GuardBench b;
  b.nothingHappened(b.nrf.erasePage(FlashRegion{0x26100, 0xF0000}, 0x27000), Status::kBadRegion, 0x26100);
  b.nothingHappened(b.nrf.erasePage(FlashRegion{0x26000, 0xF0100}, 0x27000), Status::kBadRegion, 0x26000);
  b.nothingHappened(b.nrf.erasePage(FlashRegion{0x26000, 0x26000}, 0x26000), Status::kBadRegion, 0x26000);
  b.nothingHappened(b.nrf.erasePage(FlashRegion{0xF0000, 0x26000}, 0x26000), Status::kBadRegion, 0xF0000);
  b.nothingHappened(b.nrf.erasePage(FlashRegion{0x26000, 0x101000}, 0x27000), Status::kBadRegion, 0x26000);
  // A "region" over UICR: outside the chip's 1 MB of flash, so no region at all - the guard does
  // not need to know what UICR is to refuse it.
  b.nothingHappened(b.nrf.erasePage(FlashRegion{0x10001000, 0x10002000}, 0x10001000), Status::kBadRegion, 0x10001000);
  b.nothingHappened(b.nrf.writeWords(FlashRegion{0x10001000, 0x10002000}, regs::uicr::kApprotect, b.data, 1),
                    Status::kBadRegion, 0x10001000);
  b.nothingHappened(b.nrf.writeWords(FlashRegion{0x10000000, 0x10001000}, 0x10000100, b.data, 1),
                    Status::kBadRegion, 0x10000000);
  // The whole flash is a legal region (the guard is about shape; which region is the flasher's).
  CHECK(b.nrf.erasePage(FlashRegion{0, 0x100000}, 0x26000).ok());
  CHECK(b.chip.counters().tripwire == 0);
  b.close();
}

TEST_CASE("read is not guarded by a region but is read-only, word-aligned, and bounded to the request") {
  GuardBench b;
  std::uint32_t out[4] = {};
  CHECK(b.nrf.read(regs::uicr::kApprotect, out, 1).ok());
  CHECK((out[0] & 0xFF) == regs::uicr::kApprotectHwDisabled);
  CHECK(b.nrf.read(0x10000100, out, 2).ok());
  CHECK(out[0] == 0x52840);
  CHECK(out[1] == 0x41414630);
  CHECK(b.nrf.read(0x10000102, out, 1).status == Status::kNotAligned);
  CHECK(b.nrf.read(0x10000100, nullptr, 1).status == Status::kBadArgument);
  CHECK(b.nrf.read(0x10000100, out, 0).status == Status::kBadArgument);
  CHECK(b.chip.log().empty());
  CHECK(b.chip.counters().tripwire == 0);
  b.close();
}
