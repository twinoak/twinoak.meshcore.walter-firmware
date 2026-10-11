// What the flasher refuses before it writes (task p1-05), with the chip's log empty and the core
// never halted: an image one page too long for the region, one that starts below it, an empty
// one, an unaligned one, a manifest region wider than the profile; a protected chip; a wrong
// part; a chip whose SoftDevice ends elsewhere, or that has none. And what fails mid-way: a
// target that goes silent (pins released, a clear error, a reset attempted as far as possible), a
// page with a stuck bit (retried once, then a failure naming the address), the lines taken by a
// kill. The pin rules hold on every exit path: the bench's destructor checks.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/flasher/flasher.h"
#include "flasher_bench.h"

using sim::digestOf;
using sim::fl::Image;
using sim::fl::MemoryImage;
using sim::fl::Outcome;
using sim::fl::Result;
using sim::fl::Step;
namespace bs = core::flasher::bootloader_settings;

namespace {

struct Refusal : sim::FlasherBench {
  std::vector<std::uint8_t> image = sim::makeImage(2, false);
  std::vector<std::uint8_t> sha = digestOf(image);
  MemoryImage source{image.data(), static_cast<std::uint32_t>(image.size())};

  void nothingHappened(const Result& r) {
    CHECK(!r.ok());
    CHECK(sim::nothingWritten(chip));
    CHECK(chip.counters().erases == 0);
    CHECK(chip.counters().writes == 0);
    CHECK(!chip.coreHalted());
    CHECK(chip.counters().dhcsr_writes == 0);
    CHECK(chip.counters().aircr_resets == 0);
    CHECK(!r.reset_issued);
    CHECK(!board.swdHeld());
    CHECK((target.ctrlStat() & core::swd::ctrl_stat::kPowerUpRequests) == 0);
  }
};

}  // namespace

TEST_CASE("an image one page too long for the region: refused, nothing happened") {
  Refusal b;
  const std::uint32_t size = b.profile.app.size() + 4096;
  std::vector<std::uint8_t> big(size, 0x5A);
  const MemoryImage source(big.data(), size);
  const Image img{&source, 0x26000, nullptr};
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.outcome == Outcome::kRefused);
  CHECK(r.step == Step::kImage);
  // Exactly the region's size is accepted by the check (and would be flashed).
  CHECK(b.profile.holds(0x26000, b.profile.app.size()));
  CHECK(!b.profile.holds(0x26000, size));
}

TEST_CASE("an image that starts below the region, one at an unaligned address, an empty one: refused") {
  Refusal b;
  Result r = b.flasher.flash(Image{&b.source, 0x25000, b.sha.data()}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kImage);
  CHECK(r.address == 0x25000);
  r = b.flasher.flash(Image{&b.source, 0x26100, b.sha.data()}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kImage);
  const MemoryImage empty(b.image.data(), 0);
  r = b.flasher.flash(Image{&empty, 0x26000, nullptr}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kImage);
  r = b.flasher.flash(Image{nullptr, 0x26000, nullptr}, b.profile, nullptr);
  b.nothingHappened(r);
  // One that ends exactly at the region's end is fine; one byte more is not.
  const std::uint32_t top = b.profile.app.end - 4096;
  std::vector<std::uint8_t> last(4096, 0x11);
  const MemoryImage last_src(last.data(), 4096);
  CHECK(b.profile.holds(top, 4096));
  CHECK(!b.profile.holds(top, 4097));
  CHECK(sim::nothingWritten(b.chip));
}

TEST_CASE("an image whose manifest region is wider than the profile's: the profile says no") {
  // Phase 2's check, on the table that is the authority (ARCHITECTURE §5).
  const sim::fl::BoardProfile& p = sim::fl::kRak4630Single;
  CHECK(!p.accepts(core::nrf52::FlashRegion{0x26000, 0xF4000}));
  CHECK(!p.accepts(core::nrf52::FlashRegion{0x1000, 0xED000}));
  CHECK(p.accepts(core::nrf52::FlashRegion{0x26000, 0xED000}));
}

TEST_CASE("a protected chip: said so, no memory access, nothing written") {
  Refusal b;
  b.chip.approtect = true;
  const Result r = b.flasher.flash(Image{&b.source, 0x26000, b.sha.data()}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kProtected);
  CHECK(r.nrf.status == core::nrf52::Status::kProtected);
  CHECK(b.chip.counters().protected_accesses == 0);
  CHECK(!b.flasher.verify(Image{&b.source, 0x26000, b.sha.data()}, b.profile).ok());
  CHECK(b.flasher.flashTest(b.profile, nullptr).step == Step::kProtected);
}

TEST_CASE("a wrong part: refused at identification, nothing written") {
  Refusal b;
  b.chip.info_part = 0x52833;
  const Result r = b.flasher.flash(Image{&b.source, 0x26000, b.sha.data()}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kIdentify);
  CHECK(r.nrf.status == core::nrf52::Status::kNotNrf52840);
}

TEST_CASE("a SoftDevice that ends elsewhere, or none: refused before the image is looked at") {
  Refusal b;
  b.chip.installSoftDevice(0x27000, 0x0100);  // S140 7.x
  Result r = b.flasher.flash(Image{&b.source, 0x26000, b.sha.data()}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kSoftDevice);
  CHECK(r.address == 0x27000);
  b.chip.setFlashWord(0x3004, 0x00000000);  // no magic: no SoftDevice
  r = b.flasher.flash(Image{&b.source, 0x26000, b.sha.data()}, b.profile, nullptr);
  b.nothingHappened(r);
  CHECK(r.step == Step::kSoftDevice);
  CHECK(r.address == sim::fl::kSoftDeviceInfoAddress);
}

TEST_CASE("the lines refused (the rail off): refused at the pins, no transfer at all") {
  Refusal b;
  bool done = false;
  Result r;
  b.clock.on_delay = [&](std::uint32_t) {
    if (done || !b.board.killDriven()) return;
    done = true;
    r = b.flasher.flash(Image{&b.source, 0x26000, b.sha.data()}, b.profile, nullptr);
  };
  REQUIRE(b.board.killCycle(1000) == core::board::Outcome::kOk);
  CHECK(done);
  CHECK(r.step == Step::kPins);
  CHECK(r.outcome == Outcome::kRefused);
  CHECK(b.target.counters().clocks == 0);
  CHECK(sim::nothingWritten(b.chip));
}

TEST_CASE("a target that goes silent mid-flash: a clear error, the pins released, the step named") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(3, true, 2);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  b.install(sim::makeImage(3, true, 8), 0x26000);  // so that pages are erased: READY polls to be silent during
  // After the first erase has started (the first delayMs of a READY poll), the radio stops
  // answering.
  unsigned erases_seen = 0;
  b.clock.on_delay = [&](std::uint32_t) {
    if (b.chip.counters().erases >= 2 && erases_seen == 0) {
      erases_seen = 1;
      b.target.silent = true;
    }
  };
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(!r.ok());
  CHECK(r.outcome == Outcome::kFailed);
  CHECK(r.nrf.status == core::nrf52::Status::kSwd);
  CHECK(r.nrf.swd == core::swd::Status::kNoTarget);
  CHECK((r.step == Step::kFirstPageErase || r.step == Step::kPages || r.step == Step::kSettingsInvalidate));
  CHECK(!b.board.swdHeld());
  CHECK(!r.reset_issued);  // pointless on a silent target: not attempted
  CHECK(b.chip.counters().erases >= 1);
  // The chip, if it were listening, would have no startable application: the first page is
  // erased or the settings say bank 0 invalid.
  CHECK(!b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));
  // Heard again: the flash completes.
  b.target.silent = false;
  b.clock.on_delay = nullptr;
  const Result again = b.flasher.flash(img, b.profile, nullptr);
  CHECK_MESSAGE(again.ok(), "the second flash failed while " << std::string(sim::fl::describe(again.step)) << ": "
                                                             << std::string(again.why) << " ("
                                                             << std::string(core::nrf52::describe(again.nrf.status)) << ", "
                                                             << std::string(core::swd::describe(again.nrf.swd)) << ") at "
                                                             << again.address);
  CHECK(b.chipHolds(image, 0x26000));
}

TEST_CASE("the lines lost to a kill mid-flash: lines lost, the pins already the kill's, nothing driven while the rail is off") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(2, true, 2);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  // A kill in the middle of the flash (something else on the Walter deciding so): the first
  // READY poll of the first erase is where it comes.
  bool killed = false;
  b.clock.on_delay = [&](std::uint32_t) {
    if (killed || b.chip.counters().erases == 0) return;
    killed = true;
    b.chip.on_power_loss = nullptr;
    b.target.silent = true;  // the rail is off: the chip is gone
    // The kill takes the lines; the flasher's next pin operation is refused.
    b.board.killCycle(1000);
  };
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(!r.ok());
  CHECK(killed);
  CHECK(r.nrf.status == core::nrf52::Status::kSwd);
  CHECK((r.nrf.swd == core::swd::Status::kLinesLost || r.nrf.swd == core::swd::Status::kNoTarget));
  CHECK(!b.board.swdHeld());
  CHECK(!b.board.killDriven());
}

TEST_CASE("a flash page with a stuck bit: retried once with an erase, then a failure naming the address") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(3, true, 4);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  // A bit the image wants set, stuck at 0, in the second page.
  const std::uint32_t victim = 0x27010;
  std::uint32_t want = 0;
  for (unsigned k = 0; k < 4; ++k) want |= static_cast<std::uint32_t>(image[victim - 0x26000 + k]) << (8 * k);
  std::uint32_t bit = 1;
  while ((want & bit) == 0) bit <<= 1;
  b.chip.stuck_addr = victim;
  b.chip.stuck_mask = bit;
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(!r.ok());
  CHECK(r.outcome == Outcome::kFailed);
  CHECK(r.step == Step::kPages);
  CHECK(r.address == victim);
  CHECK(r.counters.retries == 1);
  CHECK(r.nrf.value == (want & ~bit));
  // The page was erased twice (the write in place, then the retry's erase, then... the first
  // attempt on a blank page needs no erase; the retry does) and written twice.
  unsigned erases_of_page = 0;
  for (const sim::FlashEvent& e : b.chip.log()) erases_of_page += e.kind == sim::FlashEvent::Kind::kErasePage && e.addr == 0x27000;
  CHECK(erases_of_page == 1);
  // The radio was reset on the way out: it sits in its bootloader, bank 0 invalid.
  CHECK(r.reset_issued);
  CHECK(b.chip.counters().aircr_resets == 1);
  CHECK(b.settings().bank_0 == bs::kBankInvalidApp);
  CHECK(!b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));
  CHECK(!b.board.swdHeld());
  // The stuck bit gone (another page, say): the same flash finishes.
  b.chip.stuck_mask = 0;
  CHECK(b.flasher.flash(img, b.profile, nullptr).ok());
  CHECK(b.chipHolds(image, 0x26000));
}

TEST_CASE("a page that will not erase blank (a bit stuck at 0 in a word the image wants blank): retried, then failed") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(2, false, 4);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  b.install(sim::makeImage(2, true, 6), 0x26000);  // every page needs an erase
  b.chip.stuck_addr = 0x27800;  // a word the sparse image leaves blank
  b.chip.stuck_mask = 0x00010000;
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(!r.ok());
  CHECK(r.step == Step::kPages);
  CHECK(r.address == 0x27800);
  CHECK(r.counters.retries == 1);
  unsigned erases_of_page = 0;
  for (const sim::FlashEvent& e : b.chip.log()) erases_of_page += e.kind == sim::FlashEvent::Kind::kErasePage && e.addr == 0x27000;
  CHECK(erases_of_page == 2);
}

TEST_CASE("a stuck bit in the settings page: the record does not verify, the failure names the settings page") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(2, false, 4);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  b.chip.stuck_addr = 0xFF008;  // bank_0_size: 0x2000 for two pages, so bit 13 is one the record needs
  b.chip.stuck_mask = 0x00002000;
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(!r.ok());
  CHECK(r.step == Step::kSettingsValidate);
  CHECK(r.address == 0xFF008);
  CHECK(b.chipHolds(image, 0x26000));  // the image itself is complete
}

TEST_CASE("the forbidden registers are not named by the flasher: a grep of its sources") {
  // test_nrf52_forbidden.cpp reads every source under components/; this is the reminder that
  // the flasher's files are among them, by checking they exist where that test looks.
  CHECK(sim::fl::kRak4630Single.settings_page == 0xFF000);
}
