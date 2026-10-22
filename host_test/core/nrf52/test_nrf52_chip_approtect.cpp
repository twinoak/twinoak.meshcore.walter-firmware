// The simulated chip's two kinds of access-port protection (task p5-02), held to the Product
// Specification's "Access port protection" and "CTRL-AP" and to NVMC's UICR rules - the model the
// door's tests rest on, tested on its own: which build codes are which kind; the three factory
// states; what the CTRL-AP's ERASEALL, its soft reset, SYSRESETREQ, a pin reset and a power-on
// each do to the port on each kind; the resets counted by kind; UICR written like flash, nWRITE
// times between erases; and the tripwire still armed on a chip that was not told otherwise.
#include <doctest.h>

#include <string>

#include "sim/nrf52_bench.h"

using core::board::Outcome;
namespace regs = core::nrf52;

namespace {

struct ChipBench : sim::Nrf52Bench {
  ChipBench() {
    chip.allowProvisioning();
    chip.onlyTouch(0, chip.flashBytes());  // a test about the chip itself may declare the whole array
  }
  void session() {
    REQUIRE(acquire() == Outcome::kOk);
    nrf.open();
  }
  void eraseAllByHand() {
    REQUIRE(dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kEraseAll, 1).ok());
  }
  void softResetByHand() {
    REQUIRE(dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kReset, 1).ok());
    REQUIRE(dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kReset, 0).ok());
  }
  std::uint32_t eraseAllStatus() {
    const core::swd::Result r = dp.readAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kEraseAllStatus);
    REQUIRE(r.ok());
    return r.value;
  }
};

constexpr std::uint32_t kAAC0 = 0x41414330;
constexpr std::uint32_t kAAD0 = 0x41414430;
constexpr std::uint32_t kAAF0 = 0x41414630;
constexpr std::uint32_t kAAB0 = 0x41414230;

}  // namespace

TEST_CASE("the kind by build code: A-D hardware-only, F and later improved; a variant that is not text counts as improved") {
  ChipBench b;
  for (const std::uint32_t v : {0x41414141u /*AAAA*/, kAAB0, kAAC0, kAAD0, 0x41414431u /*AAD1*/}) {
    b.chip.info_variant = v;
    CHECK(!b.chip.improvedApprotect());
  }
  for (const std::uint32_t v : {kAAF0, 0x41414631u /*AAF1*/, 0x41414730u /*AAG0*/, 0xFFFFFFFFu}) {
    b.chip.info_variant = v;
    CHECK(b.chip.improvedApprotect());
  }
}

TEST_CASE("the factory states: RUI3 in every flash word, NRFFW[0] and PSELRESET set, APPROTECT and the port as each kind ships") {
  SUBCASE("legacy, open: UICR.APPROTECT erased") {
    ChipBench b;
    b.chip.info_variant = kAAC0;
    b.chip.factoryOpen();
    CHECK(!b.chip.approtect);
    CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFFFF);
    CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
    CHECK(b.chip.uicrWord(regs::uicr::kPselReset0) == 18);
    CHECK(b.chip.flashWord(0) != 0xFFFFFFFF);
    CHECK(b.chip.flashWord(0x80000) != 0xFFFFFFFF);
    CHECK(b.chip.flashWord(0xFFFFC) != 0xFFFFFFFF);
  }
  SUBCASE("legacy, locked by its firmware: PALL = Enabled (0x00)") {
    ChipBench b;
    b.chip.info_variant = kAAD0;
    b.chip.factoryLocked();
    CHECK(b.chip.approtect);
    CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFF00);
  }
  SUBCASE("improved, open: HwDisabled and a bootloader that opens the port") {
    ChipBench b;
    b.chip.info_variant = kAAF0;
    b.chip.factoryOpen();
    CHECK(!b.chip.approtect);
    CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFF5A);
    CHECK(b.chip.bootloader_opens_port);
  }
  SUBCASE("improved, locked by default: UICR.APPROTECT erased") {
    ChipBench b;
    b.chip.info_variant = kAAF0;
    b.chip.factoryLocked();
    CHECK(b.chip.approtect);
    CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFFFF);
  }
}

TEST_CASE("ERASEALL: everything to 0xFF, ERASEALLSTATUS busy for the set number of reads, the port by the kind") {
  SUBCASE("improved: open at once, until a pin or power reset") {
    ChipBench b;
    b.chip.info_variant = kAAF0;
    b.chip.factoryLocked();
    b.chip.erase_all_busy_reads = 2;
    b.session();
    CHECK(b.nrf.protection() == regs::Protection::kProtected);
    b.eraseAllByHand();
    CHECK(b.eraseAllStatus() == 1);
    CHECK(b.eraseAllStatus() == 1);
    CHECK(b.eraseAllStatus() == 0);
    CHECK(b.eraseAllStatus() == 0);
    CHECK(b.chip.counters().erase_alls == 1);
    CHECK(b.chip.counters().tripwire == 0);
    CHECK(!b.chip.approtect);
    for (std::uint32_t a = 0; a < b.chip.flashBytes(); a += 4) {
      if (b.chip.flashWord(a) != 0xFFFFFFFF) {
        FAIL_CHECK("flash not erased at " << a);
        break;
      }
    }
    for (std::uint32_t a = regs::uicr::kBase; a < regs::uicr::kEnd; a += 4) {
      if (b.chip.uicrWord(a) != 0xFFFFFFFF) {
        FAIL_CHECK("UICR not erased at " << a);
        break;
      }
    }
    // RAM too: a word the test wrote before is gone.
    CHECK(b.nrf.open().ok());
    std::uint32_t ram = 0;
    REQUIRE(b.nrf.read(0x20000000, &ram, 1).ok());
    CHECK(ram == 0xFFFFFFFF);
    // A soft reset keeps it open; a pin reset closes it (nothing in flash opens it).
    b.softResetByHand();
    CHECK(b.chip.resets().ctrl_ap_soft == 1);
    CHECK(!b.chip.approtect);
    REQUIRE(b.nrf.open().ok());  // the session again: the soft reset by hand left the layer's view stale
    CHECK(b.nrf.systemReset().ok());  // SYSRESETREQ, through the memory port
    CHECK(b.chip.resets().sysresetreq == 1);
    CHECK(!b.chip.approtect);
    b.chip.pinReset();
    CHECK(b.chip.resets().pin == 1);
    CHECK(b.chip.approtect);
    // The pin reset put the debug port back in its reset state: a session has to begin again
    // before anything else is said to it, so the lines are released without a close.
    CHECK(b.target.inResetState());
    b.release();
  }
  SUBCASE("legacy: the erased UICR is read at the next reset - soft or pin") {
    ChipBench b;
    b.chip.info_variant = kAAC0;
    b.chip.factoryLocked();
    b.session();
    b.eraseAllByHand();
    while (b.eraseAllStatus() != 0) {
    }
    CHECK(b.chip.approtect);  // not yet
    b.softResetByHand();
    CHECK(!b.chip.approtect);
    CHECK(b.chip.resets().ctrl_ap_soft == 1);
    b.close();
    ChipBench c;
    c.chip.info_variant = kAAC0;
    c.chip.factoryLocked();
    c.session();
    c.eraseAllByHand();
    CHECK(c.chip.approtect);
    c.chip.pinReset();
    CHECK(!c.chip.approtect);
    CHECK(c.chip.resets().pin == 1);
    CHECK(c.nrf.open().ok());  // a new session over the reset port: now readable
    CHECK(c.nrf.identified());
    c.close();
  }
  SUBCASE("busy for ever") {
    ChipBench b;
    b.chip.info_variant = kAAF0;
    b.chip.factoryOpen();
    b.chip.erase_all_never_finishes = true;
    b.session();
    b.eraseAllByHand();
    for (int i = 0; i < 20; ++i) CHECK(b.eraseAllStatus() == 1);
    b.close();
  }
}

TEST_CASE("a pin reset and a power-on on an improved part: open only with HwDisabled, firmware in flash and a bootloader that opens it") {
  ChipBench b;
  b.chip.info_variant = kAAF0;
  b.chip.factoryLocked();
  b.session();
  b.eraseAllByHand();
  CHECK(!b.chip.approtect);
  // HwDisabled, no firmware: closed.
  b.chip.setUicrWord(regs::uicr::kApprotect, 0x0000005A);
  b.chip.pinReset();
  CHECK(b.chip.approtect);
  // HwDisabled, firmware, a bootloader that opens the port: open.
  b.chip.approtect = false;
  b.chip.setFlashWord(0, 0x20000400);
  b.chip.pinReset();
  CHECK(!b.chip.approtect);
  b.chip.powerOn();
  CHECK(!b.chip.approtect);
  // The same, but the bootloader was not built to open it: closed (spec/SYSTEM.md 10.2).
  b.chip.bootloader_opens_port = false;
  b.chip.pinReset();
  CHECK(b.chip.approtect);
  // And without HwDisabled, whatever the firmware: closed.
  b.chip.bootloader_opens_port = true;
  b.chip.setUicrWord(regs::uicr::kApprotect, 0xFFFFFFFF);
  b.chip.pinReset();
  CHECK(b.chip.approtect);
  CHECK(b.chip.resets().pin == 4);
  CHECK(b.chip.resets().power_on == 1);
  CHECK(b.chip.resets().ctrl_ap_soft == 0);
  // The debug port was reset with the part: a session has to be opened again, and can be.
  CHECK(b.target.inResetState());
  b.chip.approtect = false;
  CHECK(b.nrf.open().ok());
  b.close();
}

TEST_CASE("a pin reset through RadioPins reaches the chip, and is counted as a pin reset") {
  ChipBench b;
  b.chip.info_variant = kAAC0;
  b.chip.factoryLocked();
  CHECK(b.chip.approtect);
  b.chip.setUicrWord(regs::uicr::kApprotect, 0xFFFFFFFF);  // as if erased
  CHECK(b.board.resetPulse() == Outcome::kOk);
  CHECK(b.chip.resets().pin == 1);
  CHECK(!b.chip.approtect);  // legacy: UICR read at the reset
  b.session();
  CHECK(b.nrf.identified());
  b.close();
}

TEST_CASE("UICR is written like flash: CONFIG = Wen, bits only cleared, counted past nWRITE, busy afterwards") {
  ChipBench b;
  b.chip.info_variant = kAAF0;
  b.chip.factoryOpen();
  b.session();
  b.eraseAllByHand();
  CHECK(b.nrf.open().ok());
  core::swd::MemAp& mem = b.nrf.memAp();
  // With CONFIG read-only: refused, recorded, and - strict off - a sentence.
  b.chip.strict = false;
  CHECK(mem.write32(regs::uicr::kNrfFw0, 0x000F4000).ok());
  CHECK(b.chip.counters().rejected_writes == 1);
  CHECK(b.chip.countErrors("UICR write") == 1);
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0xFFFFFFFF);
  b.chip.clearErrors();
  b.chip.strict = true;
  // With Wen: written, bits only cleared, the controller busy for tWRITE - made 10 ms here so
  // that the wire's own time (eight idle clocks at 125 kHz outlast a 41 us write) does not hide it.
  b.chip.write_time_us = 10000;
  REQUIRE(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigWen).ok());
  CHECK(mem.write32(regs::uicr::kNrfFw0, 0x000F4000).ok());
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
  CHECK(b.chip.nvmcBusy());
  CHECK(b.nrf.waitFlashReady(50).ok());
  CHECK(!b.chip.nvmcBusy());
  CHECK(mem.write32(regs::uicr::kNrfFw0, 0xFFFF0000).ok());  // a second write: AND
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F0000);
  CHECK(b.chip.counters().uicr_overwrites == 0);
  CHECK(mem.write32(regs::uicr::kNrfFw0, 0xFFFFFFFF).ok());  // a third: past nWRITE
  CHECK(b.chip.counters().uicr_overwrites == 1);
  CHECK(b.chip.counters().uicr_writes == 3);
  CHECK(b.chip.countLog(sim::FlashEvent::Kind::kUicrWrite) == 3);
  REQUIRE(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigRen).ok());
  CHECK(b.chip.counters().tripwire == 0);
  b.close();
}

TEST_CASE("NVMC.ERASEALL and ERASEUICR trip even on a chip armed for provisioning: the door does not use them") {
  ChipBench b;
  b.chip.strict = false;
  b.chip.info_variant = kAAF0;
  b.chip.factoryOpen();
  b.session();
  core::swd::MemAp& mem = b.nrf.memAp();
  REQUIRE(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigEen).ok());
  CHECK(mem.write32(regs::nvmc::kEraseAll, 1).ok());
  CHECK(mem.write32(regs::nvmc::kEraseUicr, 1).ok());
  REQUIRE(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigRen).ok());
  CHECK(b.chip.counters().tripwire == 2);
  CHECK(b.chip.countErrors("NVMC.ERASEALL written") == 1);
  CHECK(b.chip.countErrors("NVMC.ERASEUICR written") == 1);
  CHECK(b.chip.flashWord(0) != 0xFFFFFFFF);
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
  b.chip.clearErrors();
  b.close();
}

TEST_CASE("a chip not armed: the CTRL-AP's ERASEALL and a UICR write trip, as before this task") {
  sim::Nrf52Bench b;
  b.chip.strict = false;
  b.chip.factoryOpen();
  CHECK(!b.chip.provisioningAllowed());
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.nrf.open().ok());
  REQUIRE(b.dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kEraseAll, 1).ok());
  CHECK(b.chip.counters().tripwire == 1);
  CHECK(b.chip.counters().erase_alls == 0);
  CHECK(b.chip.flashWord(0) != 0xFFFFFFFF);
  core::swd::MemAp& mem = b.nrf.memAp();
  REQUIRE(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigWen).ok());
  CHECK(mem.write32(regs::uicr::kNrfFw0, 0).ok());
  CHECK(b.chip.counters().tripwire == 2);
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
  REQUIRE(mem.write32(regs::nvmc::kConfig, regs::nvmc::kConfigRen).ok());
  CHECK(b.chip.countErrors("CTRL-AP ERASEALL written") == 1);
  CHECK(b.chip.countErrors("a write to UICR") == 1);
  b.chip.clearErrors();
  b.nrf.close();
  b.release();
}
