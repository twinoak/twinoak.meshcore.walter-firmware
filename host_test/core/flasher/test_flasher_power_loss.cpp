// Power lost at every flash operation in turn (task p1-05) - the test the simulator exists for.
// For a multi-page image, over a region that is blank and over one holding another image, from a
// settings page that started erased, as a UF2 install leaves it (valid, CRC check off) and as a
// DFU with a recorded CRC leaves it: after each interruption the chip is in a legal half-way
// state, neither bootloader starts a partly written application, flash() again finishes the job
// with no more erases than needed, the final contents are exact, and no page outside the image's
// pages and the settings page was ever erased or written, in any run.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/flasher/flasher.h"
#include "flasher_bench.h"

using sim::digestOf;
using sim::fl::Image;
using sim::fl::MemoryImage;
using sim::fl::Result;
using sim::fl::Step;
using Bootloader = sim::Nrf52Chip::Bootloader;
namespace bs = core::flasher::bootloader_settings;

namespace {

enum class Start { kBlankSettings, kUf2Settings, kDfuCrcSettings };

const char* name(Start s) {
  switch (s) {
    case Start::kBlankSettings: return "settings erased";
    case Start::kUf2Settings: return "settings as a UF2 install leaves them (valid, CRC off)";
    case Start::kDfuCrcSettings: return "settings as a DFU with a recorded CRC leaves them";
  }
  return "?";
}

std::uint16_t crc16Of(const std::vector<std::uint8_t>& bytes) {
  std::uint16_t crc = 0xFFFF;
  for (std::uint8_t byte : bytes) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8));
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000u) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u) : static_cast<std::uint16_t>(crc << 1);
    }
  }
  return crc;
}

struct Scenario {
  bool old_image;  // the region holds another image first
  Start start;
};

// The bench arranged for a scenario: the old image (if any), the settings page as the scenario
// says, a canary page above the image that must never change.
void arrange(sim::FlasherBench& b, const Scenario& sc, const std::vector<std::uint8_t>& old_image) {
  if (sc.old_image) b.install(old_image, 0x26000);
  switch (sc.start) {
    case Start::kBlankSettings: break;
    case Start::kUf2Settings: b.setSettings(bs::validRecord(0)); break;  // msc_uf2.c: app_size 0, app_crc 0
    case Start::kDfuCrcSettings: {
      bs::Record r = bs::validRecord(static_cast<std::uint32_t>(old_image.size()));
      r.bank_0_crc = sc.old_image ? crc16Of(old_image) : 0x1234;
      b.setSettings(r);
      break;
    }
  }
  b.chip.fillFlash(0x29000, 0x2A000, 0x0BAD0BAD);  // the canary: the page above the old image
}

// What the bootloaders may accept after an interruption: a complete image - the old one, if it
// was there and nothing of it changed, or the new one - and never anything else.
void checkNeverHalf(sim::FlasherBench& b, const std::vector<std::uint8_t>& old_image, bool had_old,
                    const std::vector<std::uint8_t>& image, unsigned op, bool* accepted_new) {
  const bool stock = b.chip.bootloaderAccepts(Bootloader::kStock);
  const bool otafix = b.chip.bootloaderAccepts(Bootloader::kOtafix);
  const bool holds_new = b.chipHolds(image, 0x26000);
  const bool holds_old = had_old && b.chipHolds(old_image, 0x26000);
  *accepted_new = (stock || otafix) && holds_new;
  if (stock || otafix) {
    const bool complete = holds_new || holds_old;
    CHECK_MESSAGE(complete, "op " << op << ": a bootloader would start a partly written application (stock "
                                  << stock << ", OTAFIX " << otafix << ")");
  }
  // OTAFIX never accepts through an erased settings page; so when OTAFIX accepts, bank 0 is valid.
  if (otafix) CHECK(b.settings().bank_0 == bs::kBankValidApp);
}

}  // namespace

TEST_CASE("power loss at every flash operation: legal state, never half an application, convergence, nothing outside") {
  // Two pages, sparse: every flash operation of a multi-page image is interrupted in turn, and
  // each interruption costs three flashes through the bit-level wire; the pages' count does not
  // change what the loop proves, the operations' kinds do (a settings record, a first-page erase,
  // in-place writes, erases after another image, the final record).
  const std::vector<std::uint8_t> image = sim::makeImage(2, false, 3);
  const std::vector<std::uint8_t> old_image = sim::makeImage(3, false, 9);
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  const std::uint32_t size = static_cast<std::uint32_t>(image.size());

  const Scenario scenarios[] = {
      {false, Start::kBlankSettings}, {false, Start::kUf2Settings}, {false, Start::kDfuCrcSettings},
      {true, Start::kBlankSettings},  {true, Start::kUf2Settings},  {true, Start::kDfuCrcSettings},
  };
  unsigned total_ops = 0, total_runs = 0;
  for (const Scenario& sc : scenarios) {
    // The clean run first: how many flash operations there are to interrupt.
    unsigned ops = 0;
    {
      sim::FlasherBench clean;
      arrange(clean, sc, old_image);
      clean.allow(0x26000, size);
      const Result r = clean.flasher.flash(img, clean.profile, nullptr);
      REQUIRE(r.ok());
      REQUIRE(clean.chipHolds(image, 0x26000));
      ops = clean.chip.counters().erases + clean.chip.counters().writes;
      REQUIRE(ops > 20);
      CHECK(clean.chip.bootloaderAccepts(Bootloader::kStock));
      CHECK(clean.chip.bootloaderAccepts(Bootloader::kOtafix));
    }
    MESSAGE(std::string(sc.old_image ? "over another image, " : "into a blank region, ") << name(sc.start) << ": " << ops
                                                                                            << " flash operations");
    unsigned accepted_new_at = 0;  // the first operation after which a bootloader starts the new image
    unsigned otafix_accepted_new = 0;  // interruptions after which OTAFIX starts the new image
    unsigned refused_while_writing = 0;
    for (unsigned op = 1; op <= ops; ++op) {
      sim::FlasherBench b;
      arrange(b, sc, old_image);
      b.allow(0x26000, size);
      b.chip.lose_power_at_op = op;
      const Result first = b.flasher.flash(img, b.profile, nullptr);
      REQUIRE_MESSAGE(!first.ok(), "op " << op << ": the flash should have been cut short");
      REQUIRE(!b.chip.powered());
      CHECK(!b.board.swdHeld());
      CHECK(first.nrf.status == core::nrf52::Status::kSwd);
      // The canary above the image, the SoftDevice and the bootloader's own pages: untouched.
      CHECK(b.chip.flashWord(0x29000) == 0x0BAD0BAD);
      CHECK(b.chip.flashWord(0x3008) == 0x26000);
      CHECK(b.chip.flashWord(0xFE000) == 0xFFFFFFFF);
      // The settings page: blank, invalid or valid - never another code.
      const bs::Record s = b.settings();
      CHECK((s.bank_0 == bs::kBankBlank || s.bank_0 == bs::kBankInvalidApp || s.bank_0 == bs::kBankValidApp));
      bool accepted_new = false;
      checkNeverHalf(b, old_image, sc.old_image, image, op, &accepted_new);
      if (accepted_new && accepted_new_at == 0) accepted_new_at = op;
      if (!accepted_new) refused_while_writing++;
      if (b.chip.bootloaderAccepts(Bootloader::kOtafix) && b.chipHolds(image, 0x26000)) otafix_accepted_new++;

      // The power back; the same flash again finishes the job.
      b.powerCycle();
      const Result second = b.flasher.flash(img, b.profile, nullptr);
      CHECK_MESSAGE(second.ok(), "op " << op << ": the second run failed while " << sim::fl::describe(second.step));
      CHECK(b.chipHolds(image, 0x26000));
      CHECK(b.chip.bootloaderAccepts(Bootloader::kStock));
      CHECK(b.chip.bootloaderAccepts(Bootloader::kOtafix));
      CHECK(b.settings().validWithoutCrc());
      CHECK(b.chip.flashWord(0x29000) == 0x0BAD0BAD);
      // No more erases than needed: at most one per image page plus two for the settings page.
      CHECK(second.counters.erases <= 2 + 2);
      total_runs += 2;
      // And a third run has nothing to do (every fifth interruption, and the last: it is reads only).
      if (op % 5 == 0 || op == ops) {
        const Result third = b.flasher.flash(img, b.profile, nullptr);
        CHECK(third.ok());
        CHECK(third.counters.erases == 0);
        CHECK(third.counters.words_written == 0);
        total_runs++;
      }
    }
    total_ops += ops;
    // The new image is accepted by nobody while any of its words is still to come. The final
    // record is eight operations (an erase and seven words, word 0 last): from its erase on, the
    // page reads as erased and the newer stock bootloader starts the - by then complete and
    // verified - image through its fallback; OTAFIX accepts only bank_0 = BANK_VALID_APP, which
    // is word 0 of the last operation, and that operation's half-written word leaves bank_0
    // blank, so OTAFIX accepts after none of the interruptions.
    CHECK(accepted_new_at >= ops - 7);
    CHECK(refused_while_writing >= ops - 8);
    CHECK(otafix_accepted_new == 0);
    MESSAGE("  refused by both bootloaders after " << refused_while_writing << " of " << ops
                                                   << " interruptions; the complete image first accepted (stock, through the erased settings page) after operation "
                                                   << accepted_new_at << "; by OTAFIX after none");
  }
  MESSAGE("power-loss loop: " << total_ops << " interruptions over six scenarios, " << total_runs << " flash() runs");
  CHECK(total_ops > 120);
}

TEST_CASE("the bootloaders' rules in the simulator, from their sources: what each accepts") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> image = sim::makeImage(2, false);
  // Nothing at the application's start: both refuse, whatever the settings say.
  b.setSettings(bs::validRecord(0));
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kOtafix));
  // The image there, settings valid with the CRC check off: both accept.
  b.install(image, 0x26000);
  CHECK(b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(b.chip.bootloaderAccepts(Bootloader::kOtafix));
  // Bank 0 invalid: both refuse, the image notwithstanding.
  b.setSettings(bs::invalidRecord());
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kOtafix));
  // The page erased: stock's fallback accepts a plausible vector table, OTAFIX refuses.
  b.chip.fillFlash(0xFF000, 0x100000, 0xFFFFFFFF);
  CHECK(b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kOtafix));
  // ... and not an implausible one: a reset vector outside the application region.
  b.chip.setFlashWord(0x26004, 0x000F4001);
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kStock));
  b.chip.setFlashWord(0x26004, 0x00026101);
  // The stack pointer outside RAM.
  b.chip.setFlashWord(0x26000, 0x20040004);
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kStock));
  b.chip.setFlashWord(0x26000, 0x20040000);
  // A recorded CRC: accepted when it is the image's, refused when it is not.
  bs::Record r = bs::validRecord(static_cast<std::uint32_t>(image.size()));
  r.bank_0_crc = crc16Of(image);
  b.setSettings(r);
  CHECK(b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(b.chip.bootloaderAccepts(Bootloader::kOtafix));
  b.chip.fillFlash(0xFF000, 0x100000, 0xFFFFFFFF);
  r.bank_0_crc = static_cast<std::uint16_t>(r.bank_0_crc ^ 1);
  b.setSettings(r);
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kOtafix));
  // Where the application starts is what the SoftDevice says: without one, the MBR's end.
  CHECK(b.chip.applicationStart() == 0x26000);
  b.chip.setFlashWord(0x3004, 0);
  CHECK(b.chip.applicationStart() == 0x1000);
}

TEST_CASE("interrupted after the first page's erase: the radio has no startable application and the next run writes the rest without a second erase") {
  sim::FlasherBench b;
  const std::vector<std::uint8_t> old_image = sim::makeImage(3, false, 9);
  const std::vector<std::uint8_t> image = sim::makeImage(3, false, 3);
  b.install(old_image, 0x26000);
  b.setSettings(bs::validRecord(0));
  const std::vector<std::uint8_t> sha = digestOf(image);
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Image img{&source, 0x26000, sha.data()};
  b.allow(0x26000, static_cast<std::uint32_t>(image.size()));
  // The settings page's erase and seven words are operations 1..8; the first page's erase is 9.
  b.chip.lose_power_at_op = 10;
  Result r = b.flasher.flash(img, b.profile, nullptr);
  REQUIRE(!r.ok());
  CHECK(r.step == Step::kPages);
  CHECK(b.chip.flashWord(0x26000) == 0xFFFFFFFF);
  CHECK(b.chip.flashWord(0x26004) == 0xFFFFFFFF);
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kStock));
  CHECK(!b.chip.bootloaderAccepts(Bootloader::kOtafix));
  CHECK(b.settings().bank_0 == bs::kBankInvalidApp);
  b.powerCycle();
  r = b.flasher.flash(img, b.profile, nullptr);
  CHECK(r.ok());
  // The first page was already blank: written in place. The second page had a word half-written
  // by the power loss and needed its erase; the third was untouched by the first run.
  CHECK(r.counters.pages_written_in_place >= 1);
  CHECK(r.counters.erases <= 4);
  CHECK(b.chipHolds(image, 0x26000));
}
