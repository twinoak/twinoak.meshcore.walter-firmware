// The flasher's good paths (task p1-05): a full image into a blank region, into a region holding
// another image, the same image again with zero erases and zero writes; what the settings page
// says before, between and after; the profile's checks; verify() changing nothing; the pins
// released and the reset recorded; the progress reported; the time a flash takes at 125 kHz.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/flasher/flasher.h"
#include "flasher_bench.h"

using core::nrf52::FlashRegion;
using sim::digestOf;
using sim::fl::Image;
using sim::fl::MemoryImage;
using sim::fl::Outcome;
using sim::fl::Result;
using sim::fl::Step;
namespace bs = core::flasher::bootloader_settings;

TEST_CASE("the board profile: the numbers from the linker script, and what a manifest may ask") {
  const sim::fl::BoardProfile& p = sim::fl::kRak4630Single;
  CHECK(std::string(p.name) == "rak4630-single");
  CHECK(p.app.begin == 0x26000);
  CHECK(p.app.end == 0xED000);
  CHECK(p.softdevice_size == 0x26000);
  CHECK(p.settings_page == 0xFF000);
  CHECK(p.bootloader_start == 0xF4000);
  CHECK(p.page_size == 4096);
  // A manifest's app_region must lie inside: the repeater's own (0x26000..0xED000) does, the
  // companion's narrower one does, anything wider or elsewhere does not.
  CHECK(p.accepts(FlashRegion{0x26000, 0xED000}));
  CHECK(p.accepts(FlashRegion{0x26000, 0xD4000}));
  CHECK(p.accepts(FlashRegion{0x27000, 0x80000}));
  CHECK(!p.accepts(FlashRegion{0x26000, 0xEE000}));
  CHECK(!p.accepts(FlashRegion{0x25000, 0xED000}));
  CHECK(!p.accepts(FlashRegion{0x1000, 0x26000}));
  CHECK(!p.accepts(FlashRegion{0x26000, 0x26000}));
  CHECK(!p.accepts(FlashRegion{0xED000, 0x100000}));
  CHECK(p.holds(0x26000, 0xED000 - 0x26000));
  CHECK(!p.holds(0x26000, 0xED000 - 0x26000 + 1));
  CHECK(!p.holds(0x26000, 0));
  CHECK(sim::fl::findProfile("rak4630-single") == &p);
  CHECK(sim::fl::findProfile("rak4630-dual-a") == nullptr);
  CHECK(sim::fl::findProfile(nullptr) == nullptr);
}

TEST_CASE("the settings record: the layout the bootloader reads, encoded and decoded") {
  std::uint32_t words[bs::kRecordWords];
  bs::encode(bs::validRecord(0x12345), words);
  CHECK(words[0] == 0x00000001);  // bank_0 = BANK_VALID_APP, bank_0_crc = 0
  CHECK(words[1] == 0xFFFF00FF);  // bank_1 = BANK_INVALID_APP, the padding erased
  CHECK(words[2] == 0x12345);     // bank_0_size
  CHECK(words[3] == 0);
  CHECK(words[6] == 0);
  const bs::Record back = bs::decode(words);
  CHECK(back.bank_0 == bs::kBankValidApp);
  CHECK(back.bank_0_crc == 0);
  CHECK(back.bank_0_size == 0x12345);
  CHECK(back.validWithoutCrc());
  bs::encode(bs::invalidRecord(), words);
  CHECK(words[0] == 0x000000FF);
  const std::uint32_t blank[bs::kRecordWords] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                                 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  CHECK(bs::decode(blank).blank());
}

TEST_CASE("a full image into a blank region: every page written in place, the settings written, the radio reset") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(3, false);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  sim::CountingProgress progress;

  const Result r = b.flasher.flash(img, b.profile, &progress);
  CHECK(r.ok());
  CHECK(r.step == Step::kDone);
  CHECK(r.reset_issued);
  CHECK(!r.port_closed_after_reset);
  CHECK(b.chipHolds(image, 0x26000));
  // A blank region: no erase of an image page; the settings page was blank too, so its first
  // record went in without an erase and the second needed one.
  CHECK(r.counters.pages_total == 3);
  CHECK(r.counters.pages_written_in_place == 3);
  CHECK(r.counters.pages_erased == 0);
  CHECK(r.counters.erases == 1);
  CHECK(r.counters.settings_erases == 1);
  CHECK(r.counters.settings_writes == 2);
  CHECK(r.counters.retries == 0);
  CHECK(b.chip.counters().erases == 1);
  // The settings: bank 0 valid, the CRC check off, the size the image's.
  const bs::Record s = b.settings();
  CHECK(s.bank_0 == bs::kBankValidApp);
  CHECK(s.bank_0_crc == 0);
  CHECK(s.bank_0_size == image.size());
  CHECK(s.bank_1 == bs::kBankInvalidApp);
  // Both bootloaders start it.
  CHECK(b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock));
  CHECK(b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));
  // The radio was reset once, through the debug port, and runs; the power-up is withdrawn.
  CHECK(b.chip.counters().aircr_resets == 1);
  CHECK(!b.chip.coreHalted());
  CHECK(!b.chip.debugEnabled());
  CHECK((b.target.ctrlStat() & core::swd::ctrl_stat::kPowerUpRequests) == 0);
  // And RadioPins knows: the next reset pulse waits out the spacing.
  CHECK(b.board.lastResetKnown());
  CHECK(b.board.msUntilResetAllowed() == core::board::kResetSpacingMs);
  // Progress: three passes over three pages.
  CHECK(progress.scan == 3);
  CHECK(progress.pages == 3);
  CHECK(progress.read_back == 3);
  CHECK(progress.last_total == 3);
  CHECK(b.chip.errors().empty());
  CHECK(b.target.errors().empty());
}

TEST_CASE("the same image again: zero erases, zero writes, and still a complete read-back and a reset") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(3, false);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  REQUIRE(b.flasher.flash(img, b.profile, nullptr).ok());
  b.chip.clearLog();
  const unsigned erases = b.chip.counters().erases;
  const unsigned writes = b.chip.counters().writes;

  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(r.counters.pages_skipped == 3);
  CHECK(r.counters.erases == 0);
  CHECK(r.counters.words_written == 0);
  CHECK(r.counters.settings_writes == 0);
  CHECK(b.chip.counters().erases == erases);
  CHECK(b.chip.counters().writes == writes);
  CHECK(b.chip.countLog(sim::FlashEvent::Kind::kErasePage) == 0);
  CHECK(b.chip.countLog(sim::FlashEvent::Kind::kWrite) == 0);
  CHECK(b.chip.countLog(sim::FlashEvent::Kind::kConfig) == 0);
  CHECK(r.counters.words_read >= 2 * 3 * 1024);  // the scan and the read-back
  CHECK(r.reset_issued);
  CHECK(b.chip.counters().aircr_resets == 2);
}

TEST_CASE("into a region holding another image: the first page erased before any other, every differing page erased, the old settings replaced") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> old_image = sim::makeImage(4, true, 7);
  b.install(old_image, 0x26000);
  b.setSettings(bs::validRecord(static_cast<std::uint32_t>(old_image.size())));  // as a UF2 install left it
  REQUIRE(b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock));
  REQUIRE(b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));

  const std::vector<std::uint8_t> image = sim::makeImage(3, true, 3);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));

  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(b.chipHolds(image, 0x26000));
  CHECK(r.counters.pages_erased == 3);
  CHECK(r.counters.pages_written_in_place == 1);  // the first page: erased in step 4, written in place in step 5
  CHECK(r.counters.settings_erases == 2);  // the old record out, the invalid one in; then the valid one
  CHECK(r.counters.erases == 5);
  // The old image's fourth page is not the new image's: untouched.
  CHECK(b.chip.flashWord(0x29000) != 0xFFFFFFFF);
  for (std::uint32_t a = 0x29000; a < 0x2A000; a += 4) {
    std::uint32_t w = 0;
    for (unsigned k = 0; k < 4; ++k) w |= static_cast<std::uint32_t>(old_image[a - 0x26000 + k]) << (8 * k);
    REQUIRE(b.chip.flashWord(a) == w);
  }
  // The order in the log: the settings page first, then the first page's erase, then pages 1
  // and 2, then the first page's words, then the settings page again.
  std::vector<std::uint32_t> erase_order;
  for (const sim::FlashEvent& e : b.chip.log()) {
    if (e.kind == sim::FlashEvent::Kind::kErasePage) erase_order.push_back(e.addr);
  }
  REQUIRE(erase_order.size() == 5);
  CHECK(erase_order[0] == 0xFF000);
  CHECK(erase_order[1] == 0x26000);
  CHECK(erase_order[2] == 0x27000);
  CHECK(erase_order[3] == 0x28000);
  CHECK(erase_order[4] == 0xFF000);
  // The first page's words come after every other page's.
  std::size_t last_other_write = 0, first_page0_write = b.chip.log().size();
  for (std::size_t i = 0; i < b.chip.log().size(); ++i) {
    const sim::FlashEvent& e = b.chip.log()[i];
    if (e.kind != sim::FlashEvent::Kind::kWrite) continue;
    if (e.addr >= 0x27000 && e.addr < 0x29000) last_other_write = i;
    if (e.addr >= 0x26000 && e.addr < 0x27000 && i < first_page0_write) first_page0_write = i;
  }
  CHECK(last_other_write < first_page0_write);
  CHECK(b.settings().validWithoutCrc());
  CHECK(b.settings().bank_0_size == image.size());
}

TEST_CASE("a settings page with a real CRC that matches the image is left alone; one that does not is rewritten") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(2, false);
  b.install(image, 0x26000);
  // CRC-16/CCITT-FALSE over the image, as nrfutil's settings generator would have recorded it.
  std::uint16_t crc = 0xFFFF;
  for (std::uint8_t byte : image) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8));
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000u) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u) : static_cast<std::uint16_t>(crc << 1);
    }
  }
  bs::Record dfu = bs::validRecord(static_cast<std::uint32_t>(image.size()));
  dfu.bank_0_crc = crc;
  b.setSettings(dfu);
  REQUIRE(b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));

  Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(r.counters.erases == 0);
  CHECK(r.counters.words_written == 0);
  CHECK(b.settings().bank_0_crc == crc);
  // The verify sees the same.
  r = b.flasher.verify(img, b.profile);
  CHECK(r.ok());
  CHECK(r.settings_valid);

  // A CRC that is not the image's: the record is replaced, the image itself untouched.
  dfu.bank_0_crc = static_cast<std::uint16_t>(crc ^ 0x1234);
  b.chip.fillFlash(0xFF000, 0x100000, 0xFFFFFFFF);
  b.setSettings(dfu);
  REQUIRE(!b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));
  r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(r.counters.erases == 1);
  CHECK(r.counters.settings_writes == 1);
  CHECK(r.counters.pages_skipped == 2);
  CHECK(b.settings().validWithoutCrc());
  CHECK(b.chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kOtafix));
}

TEST_CASE("verify() changes nothing: the log is empty, the core is not halted, the digest and the settings are reported") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(3, false);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));

  // Nothing there yet: the verify fails at the read-back, names the first word, writes nothing.
  Result r = b.flasher.verify(img, b.profile);
  CHECK(!r.ok());
  CHECK(r.step == Step::kReadBack);
  CHECK(r.address == 0x26000);
  CHECK(sim::nothingWritten(b.chip));
  CHECK(!b.chip.coreHalted());
  CHECK(b.chip.counters().dhcsr_writes == 0);
  CHECK(!b.board.swdHeld());

  REQUIRE(b.flasher.flash(img, b.profile, nullptr).ok());
  b.chip.clearLog();
  r = b.flasher.verify(img, b.profile);
  CHECK(r.ok());
  CHECK(r.settings_valid);
  CHECK(r.settings.bank_0 == bs::kBankValidApp);
  CHECK(r.counters.words_read == 3 * 1024 + bs::kRecordWords);
  CHECK(sim::nothingWritten(b.chip));
  CHECK(!b.chip.coreHalted());
  CHECK(!r.reset_issued);
  CHECK(b.chip.counters().aircr_resets == 1);  // the flash's, not the verify's

  // A wrong digest: refused at the digest, nothing changed.
  std::vector<std::uint8_t> wrong = sha;
  wrong[5] ^= 0x40;
  const Image bad{&source, 0x26000, wrong.data()};
  r = b.flasher.verify(bad, b.profile);
  CHECK(!r.ok());
  CHECK(r.step == Step::kDigest);
  CHECK(r.outcome == Outcome::kRefused);
  CHECK(sim::nothingWritten(b.chip));

  // The settings page invalid: the bytes are there, but the bootloader would not start them.
  b.setSettings(bs::invalidRecord());
  r = b.flasher.verify(img, b.profile);
  CHECK(r.ok());
  CHECK(!r.settings_valid);
  CHECK(sim::nothingWritten(b.chip));
}

TEST_CASE("an image that is not the application (not at the region's start) leaves the settings page alone") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(2, false, 1, 0x40000);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x40000, sha.data()};
  b.allow(0x40000, static_cast<std::uint32_t>(image.size()));
  b.setSettings(bs::validRecord(1000));
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(b.chipHolds(image, 0x40000));
  CHECK(r.counters.settings_writes == 0);
  CHECK(r.counters.settings_erases == 0);
  CHECK(b.settings().bank_0_size == 1000);
  CHECK(b.chip.logInside(0x40000, 0x42000));
}

TEST_CASE("an image whose length is not a whole number of words is padded with 0xFF") {
  sim::FlasherBench b;
  std::vector<std::uint8_t> image = sim::makeImage(2, false);
  image.resize(image.size() - 4096 + 13);  // one page and 13 bytes
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(r.counters.pages_total == 2);
  CHECK(b.chipHolds(image, 0x26000));
  CHECK(b.settings().bank_0_size == image.size());
  CHECK(b.flasher.verify(img, b.profile).ok());
}

TEST_CASE("a build code Fxx module that closes its port at a reset: the flash completes, the result says the port closed") {
  sim::FlasherBench b;
  b.chip.protects_after_reset = true;
  const std::vector<std::uint8_t> image = sim::makeImage(2, false);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  CHECK(r.reset_issued);
  CHECK(r.port_closed_after_reset);
  CHECK(b.chipHolds(image, 0x26000));
  CHECK(b.chip.counters().aircr_resets == 1);
  CHECK(b.chip.approtect);
  CHECK(!b.board.swdHeld());
}

TEST_CASE("the time a flash takes at 125 kHz: a dense eight-page image into a blank chip, from the simulated clock") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(8, true, 5);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  const std::uint64_t t0 = b.clock.now_ms;
  const std::uint32_t transfers0 = b.flasher.dp().counters().transfers;
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  const std::uint64_t ms = b.clock.now_ms - t0;
  const std::uint32_t transfers = b.flasher.dp().counters().transfers - transfers0;
  const std::uint32_t words = static_cast<std::uint32_t>(image.size() / 4);
  MESSAGE("8 dense pages (" << words << " words) into a blank chip at 125 kHz: " << ms << " ms simulated, "
                            << transfers << " SWD transfers, " << r.counters.words_written << " words written, "
                            << r.counters.words_read << " words read, " << r.counters.erases << " erases; "
                            << (transfers + words / 2) / words << " transfers per image word, "
                            << ms * 1000 / words << " us per image word");
  CHECK(r.counters.words_written == words + 2 * bs::kRecordWords);  // the image, and the two settings records
  CHECK(r.counters.words_read >= 3 * words);
  CHECK(ms > 0);
  // The written words: one transfer each (writeBlock, DRW); the read words: two each (DRW then
  // RDBUFF); plus TAR at every 1 KB, READY polls and the settings. Under ten transfers a word.
  CHECK(transfers < 10 * words);
}
