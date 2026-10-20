// The one door (task p5-02; spec/SYSTEM.md S-5a), against the simulated nRF52840 with both kinds
// of access-port protection - failure cases first: every grant against every port state, every
// write without an erase, every address the door must refuse, an erase that never finishes, a
// grant used twice, and the tripwire firing in a test that did not arm the chip. Then the five
// operations on each factory state, and the whole of S-5a's sequence at the door's level on
// each kind - the erase, the one soft reset, the base's words, the UICR, APPROTECT as the kind
// says, every word read back, one pin reset, and the port after it.
//
// Every test here that erases or writes arms the chip with allowProvisioning(); the tests that
// show a refusal arm it too, so that a guard which let something through would be seen by the
// simulator and not hidden by the tripwire. The windows declared are the base's: below the
// application region and from the bootloader up (rail R4).
#include <doctest.h>

#include <cstdio>
#include <string>

#include "core/flasher/board_profile.h"
#include "core/nrf52/provision_door.h"
#include "core/nrf52/provision_grant.h"
#include "sim/nrf52_bench.h"

using core::board::Outcome;
using core::nrf52::ApprotectKind;
using core::nrf52::BlankCheck;
using core::nrf52::DoorResult;
using core::nrf52::DoorStatus;
using core::nrf52::GrantKind;
using core::nrf52::grantFromTypedCommand;
using core::nrf52::Protection;
using core::nrf52::ProvisionDoor;
using core::nrf52::ProvisionGrant;
using core::nrf52::Status;
namespace regs = core::nrf52;

namespace {

constexpr std::uint32_t kVariantLegacy = 0x41414330;    // "AAC0": build code C, hardware-only APPROTECT
constexpr std::uint32_t kVariantImproved = 0x41414630;  // "AAF0": build code F, revision 3 (IN-141)
constexpr const char* kThisDeviceId = "0BADF00DDEADBEEF";  // the simulated chip's: %08X%08X of DEVICEID[1], [0]
constexpr const char* kOtherDeviceId = "0BADF00DDEADBEEE";
const core::flasher::BoardProfile& kProfile = core::flasher::kRak4630Single;

// A word as a person reads it; std::string, because doctest streams a char pointer as a pointer.
std::string hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(v));
  return std::string(buf);
}

// The base's addresses, as FIRMWARE-MANIFEST.md §4a has them: the MBR at 0, the SoftDevice's
// information structure at 0x3000, the bootloader at 0xF4000, NRFFW[0] and [1] in UICR.
constexpr std::uint32_t kMbr[4] = {0x20000400, 0x00000A81, 0x00000A87, 0x00000A89};
constexpr std::uint32_t kBootloader[4] = {0x20010000, 0x000F4ABD, 0x000F4AC1, 0x000F4AC5};
constexpr std::uint32_t kNrfFw[2] = {0x000F4000, 0x000FE000};

struct DoorBench : sim::Nrf52Bench {
  bool improved;
  DoorBench(bool improved_kind, bool locked) : improved(improved_kind) {
    chip.info_variant = improved ? kVariantImproved : kVariantLegacy;
    if (locked) {
      chip.factoryLocked();
    } else {
      chip.factoryOpen();
    }
    chip.allowProvisioning();
    // Rail R4: the base's windows - everything the application region and the file system are not.
    chip.onlyTouch(0, kProfile.app.begin);
    chip.alsoTouch(kProfile.bootloader_start, chip.flashBytes());
  }
  // The session: the lines, Nrf52::open() - which stops at kProtected on a closed chip, with the
  // debug port connected and powered, as the door needs it.
  core::nrf52::Result session() {
    REQUIRE(acquire() == Outcome::kOk);
    return nrf.open();
  }
  ApprotectKind kind() const { return improved ? ApprotectKind::kImproved : ApprotectKind::kLegacy; }
  // The sequence's first steps at the door's level: erase, the one soft reset if the port is
  // still closed, and the chip identified again.
  DoorResult eraseAndOpenPort(ProvisionDoor& door) {
    DoorResult r = door.eraseAll();
    if (!r.ok()) return r;
    if (!r.port_open) r = door.softReset();
    return r;
  }
  void nothingWritten() {
    CHECK(chip.counters().writes == 0);
    CHECK(chip.counters().erases == 0);
    CHECK(chip.counters().uicr_writes == 0);
    CHECK(chip.counters().config_writes == 0);
    CHECK(chip.counters().erase_alls == 0);
    CHECK(chip.counters().tripwire == 0);
  }
};

// Every kind and state, for the tests that run over all four.
struct Factory {
  const char* name;
  bool improved;
  bool locked;
};
const Factory kFactories[] = {
    {"legacy, open", false, false},
    {"legacy, locked by its firmware", false, true},
    {"improved, open", true, false},
    {"improved, locked by default", true, true},
};

}  // namespace

// ---- the grant against the chip ----------------------------------------------------------------

TEST_CASE("a DEVICEID grant on a closed port is refused at opening, nothing written") {
  for (const Factory& f : kFactories) {
    if (!f.locked) continue;
    DoorBench b(f.improved, true);
    CHECK(b.session().status == Status::kProtected);
    ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
    const DoorResult r = door.open();
    CHECK(r.status == DoorStatus::kGrantMismatch);
    CHECK(!r.port_open);
    CHECK(!door.opened());
    CHECK(door.counters().refusals == 1);
    b.nothingWritten();
    CHECK(b.chip.approtect);
    b.close();
  }
}

TEST_CASE("a locked grant, with or without a key, on an open port is refused at opening") {
  for (const Factory& f : kFactories) {
    if (f.locked) continue;
    for (const char* typed : {"locked", "locked:0123456789ABCDEF"}) {
      DoorBench b(f.improved, false);
      REQUIRE(b.session().ok());
      ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(typed));
      const DoorResult r = door.open();
      CHECK(r.status == DoorStatus::kGrantMismatch);
      CHECK(r.port_open);
      CHECK(!door.opened());
      b.nothingWritten();
      b.close();
    }
  }
}

TEST_CASE("a DEVICEID that is not this chip's is refused; this chip's in lower case is accepted") {
  DoorBench b(true, false);
  REQUIRE(b.session().ok());
  ProvisionDoor wrong(b.nrf, b.dp, b.clock, grantFromTypedCommand(kOtherDeviceId));
  CHECK(wrong.open().status == DoorStatus::kWrongDevice);
  CHECK(!wrong.opened());
  // One word right and the other not: still not this chip's.
  ProvisionDoor half(b.nrf, b.dp, b.clock, grantFromTypedCommand("0BADF00D00000000"));
  CHECK(half.open().status == DoorStatus::kWrongDevice);
  ProvisionDoor swapped(b.nrf, b.dp, b.clock, grantFromTypedCommand("DEADBEEF0BADF00D"));
  CHECK(swapped.open().status == DoorStatus::kWrongDevice);
  ProvisionDoor right(b.nrf, b.dp, b.clock, grantFromTypedCommand("0badf00ddeadbeef"));
  const DoorResult r = right.open();
  CHECK(r.ok());
  CHECK(r.port_open);
  CHECK(right.opened());
  CHECK(!right.erased());
  b.nothingWritten();
  b.close();
}

TEST_CASE("a malformed grant is refused without a register read; no session is refused as such") {
  DoorBench b(true, false);
  // Before Nrf52::open(): a valid grant, no session.
  ProvisionDoor early(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  CHECK(early.open().status == DoorStatus::kNotOpen);
  REQUIRE(b.session().ok());
  const unsigned reads_before = b.target.counters().ap_reads;
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand("0BADF00DDEADBEE"));
  CHECK(door.grantKind() == GrantKind::kNone);
  CHECK(door.open().status == DoorStatus::kGrantMalformed);
  CHECK(b.target.counters().ap_reads == reads_before);
  CHECK(!door.opened());
  b.nothingWritten();
  b.close();
}

TEST_CASE("one grant, one opening: a grant used twice is refused, and so is a door opened twice") {
  DoorBench b(true, false);
  REQUIRE(b.session().ok());
  ProvisionGrant grant = grantFromTypedCommand(kThisDeviceId);
  ProvisionDoor first(b.nrf, b.dp, b.clock, std::move(grant));
  CHECK(grant.spent());  // NOLINT(bugprone-use-after-move): the point
  ProvisionDoor second(b.nrf, b.dp, b.clock, std::move(grant));  // NOLINT(bugprone-use-after-move)
  CHECK(second.open().status == DoorStatus::kGrantSpent);
  CHECK(!second.opened());
  CHECK(first.open().ok());
  CHECK(first.open().status == DoorStatus::kAlreadyOpened);
  CHECK(first.opened());
  // A refused opening spends the grant too: the typed line bought one attempt.
  ProvisionGrant other = grantFromTypedCommand(kOtherDeviceId);
  ProvisionDoor refused(b.nrf, b.dp, b.clock, std::move(other));
  CHECK(refused.open().status == DoorStatus::kWrongDevice);
  CHECK(refused.open().status == DoorStatus::kGrantSpent);
  b.nothingWritten();
  b.close();
}

// ---- the writes without an erase -----------------------------------------------------------------

TEST_CASE("each write without a successful eraseAll in this opening is refused before any access") {
  DoorBench b(true, false);
  REQUIRE(b.session().ok());
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  REQUIRE(door.open().ok());
  const std::uint32_t word = 0x12345678;
  CHECK(door.writeFlash(0, kMbr, 4).status == DoorStatus::kNotErased);
  CHECK(door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).status == DoorStatus::kNotErased);
  CHECK(door.writeApprotect(ApprotectKind::kImproved).status == DoorStatus::kNotErased);
  CHECK(door.writeApprotect(ApprotectKind::kLegacy).status == DoorStatus::kNotErased);
  CHECK(door.writeUicr(regs::uicr::kApprotect, &word, 1).status == DoorStatus::kNotErased);
  CHECK(door.counters().refusals == 5);
  CHECK(b.chip.log().empty());
  b.nothingWritten();
  // And before the door is open at all, everything but open() is refused as such.
  ProvisionDoor closed(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  bool port = true;
  CHECK(closed.approtectStatus(&port).status == DoorStatus::kDoorClosed);
  CHECK(closed.eraseAll().status == DoorStatus::kDoorClosed);
  CHECK(closed.softReset().status == DoorStatus::kDoorClosed);
  CHECK(closed.writeFlash(0, kMbr, 4).status == DoorStatus::kDoorClosed);
  CHECK(closed.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).status == DoorStatus::kDoorClosed);
  CHECK(closed.writeApprotect(ApprotectKind::kImproved).status == DoorStatus::kDoorClosed);
  BlankCheck blank;
  CHECK(closed.blankCheck(&blank).status == DoorStatus::kDoorClosed);
  b.nothingWritten();
  b.close();
}

TEST_CASE("an erase that never finishes: the timeout, and the writes still refused") {
  DoorBench b(true, false);
  b.chip.erase_all_never_finishes = true;
  REQUIRE(b.session().ok());
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  REQUIRE(door.open().ok());
  const std::uint64_t t0 = b.clock.now_ms;
  const DoorResult r = door.eraseAll(500);
  CHECK(r.status == DoorStatus::kEraseTimeout);
  CHECK(!door.erased());
  CHECK(b.clock.now_ms - t0 >= 500);
  CHECK(b.clock.now_ms - t0 < 600);
  CHECK(b.chip.counters().erase_all_status_reads >= 50);
  // The chip did erase (the simulator's ERASEALL happened; only its status never came): what the
  // door knows is that it cannot say so, and it refuses to write.
  CHECK(b.chip.counters().erase_alls == 1);
  CHECK(door.writeFlash(0, kMbr, 4).status == DoorStatus::kNotErased);
  CHECK(door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).status == DoorStatus::kNotErased);
  CHECK(door.writeApprotect(ApprotectKind::kImproved).status == DoorStatus::kNotErased);
  CHECK(b.chip.counters().writes == 0);
  CHECK(b.chip.counters().uicr_writes == 0);
  b.close();
}

// ---- the addresses -------------------------------------------------------------------------------

TEST_CASE("writeUicr at UICR.APPROTECT, across it, or outside UICR's registers: refused before any access") {
  DoorBench b(true, false);
  REQUIRE(b.session().ok());
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  REQUIRE(door.open().ok());
  REQUIRE(b.eraseAndOpenPort(door).ok());
  REQUIRE(door.erased());
  const std::uint32_t words[4] = {0x5A, 0x5A, 0x5A, 0x5A};
  const unsigned config_before = b.chip.counters().config_writes;
  DoorResult r = door.writeUicr(regs::uicr::kApprotect, words, 1);
  CHECK(r.status == DoorStatus::kApprotectAddress);
  CHECK(r.address == regs::uicr::kApprotect);
  r = door.writeUicr(regs::uicr::kApprotect - 4, words, 2);  // PSELRESET[1] and APPROTECT
  CHECK(r.status == DoorStatus::kApprotectAddress);
  r = door.writeUicr(regs::uicr::kPselReset0, words, 4);  // PSELRESET[0], [1], APPROTECT, NFCPINS
  CHECK(r.status == DoorStatus::kApprotectAddress);
  r = door.writeUicr(regs::uicr::kBase - 4, words, 1);  // FICR's last word
  CHECK(r.status == DoorStatus::kOutsideUicr);
  r = door.writeUicr(regs::uicr::kEnd, words, 1);  // one past REGOUT0
  CHECK(r.status == DoorStatus::kOutsideUicr);
  r = door.writeUicr(regs::uicr::kRegOut0, words, 2);  // REGOUT0 and one past
  CHECK(r.status == DoorStatus::kOutsideUicr);
  r = door.writeUicr(0x00000000, words, 1);  // flash is not UICR
  CHECK(r.status == DoorStatus::kOutsideUicr);
  r = door.writeUicr(0x10002000, words, 1);
  CHECK(r.status == DoorStatus::kOutsideUicr);
  r = door.writeUicr(regs::uicr::kNrfFw0 + 2, words, 1);
  CHECK(r.status == DoorStatus::kNotAligned);
  r = door.writeUicr(regs::uicr::kNrfFw0, words, 0);
  CHECK(r.status == DoorStatus::kBadArgument);
  r = door.writeUicr(regs::uicr::kNrfFw0, nullptr, 1);
  CHECK(r.status == DoorStatus::kBadArgument);
  // A count that would wrap the address.
  r = door.writeUicr(regs::uicr::kNrfFw0, words, 0x40000000u);
  CHECK(r.status == DoorStatus::kOutsideUicr);
  CHECK(door.counters().refusals == 12);
  CHECK(b.chip.counters().config_writes == config_before);
  CHECK(b.chip.counters().uicr_writes == 0);
  CHECK(door.counters().uicr_words == 0);
  // And the two words the base needs, right beside the refused ones, go through.
  r = door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2);
  CHECK(r.ok());
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0 + 4) == 0x000FE000);
  CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFFFF);
  CHECK(b.chip.config() == regs::nvmc::kConfigRen);
  b.close();
}

TEST_CASE("writeFlash into the application region or the file system: refused before any access, nothing written") {
  DoorBench b(true, false);
  REQUIRE(b.session().ok());
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  REQUIRE(door.open().ok());
  REQUIRE(b.eraseAndOpenPort(door).ok());
  const std::uint32_t words[4] = {1, 2, 3, 4};
  const unsigned config_before = b.chip.counters().config_writes;
  DoorResult r = door.writeFlash(kProfile.app.begin, words, 4);  // the first word of the application
  CHECK(r.status == DoorStatus::kApplicationRegion);
  CHECK(r.address == kProfile.app.begin);
  r = door.writeFlash(kProfile.app.end - 4, words, 1);  // the last
  CHECK(r.status == DoorStatus::kApplicationRegion);
  r = door.writeFlash(kProfile.app.end, words, 4);  // the file system's first words
  CHECK(r.status == DoorStatus::kApplicationRegion);
  r = door.writeFlash(kProfile.bootloader_start - 4, words, 1);  // its last
  CHECK(r.status == DoorStatus::kApplicationRegion);
  r = door.writeFlash(kProfile.app.begin - 4, words, 2);  // a run straddling into the application
  CHECK(r.status == DoorStatus::kApplicationRegion);
  r = door.writeFlash(kProfile.bootloader_start - 4, words, 2);  // a run straddling out of the file system
  CHECK(r.status == DoorStatus::kApplicationRegion);
  r = door.writeFlash(0x00100000, words, 1);  // past the flash
  CHECK(r.status == DoorStatus::kOutsideFlash);
  r = door.writeFlash(0x000FFFFC, words, 2);
  CHECK(r.status == DoorStatus::kOutsideFlash);
  r = door.writeFlash(0x000FFFFC, words, 0x40000000u);  // a count that wraps
  CHECK(r.status == DoorStatus::kOutsideFlash);
  r = door.writeFlash(regs::uicr::kNrfFw0, words, 1);  // UICR is not flash
  CHECK(r.status == DoorStatus::kOutsideFlash);
  r = door.writeFlash(2, words, 1);
  CHECK(r.status == DoorStatus::kNotAligned);
  r = door.writeFlash(0, words, 0);
  CHECK(r.status == DoorStatus::kBadArgument);
  CHECK(door.counters().refusals == 12);
  CHECK(door.counters().flash_words == 0);
  CHECK(b.chip.counters().writes == 0);
  CHECK(b.chip.counters().config_writes == config_before);
  CHECK(b.nrf.counters().refusals == 0);  // the door's guard came first; the nRF52 layer saw nothing
  // The base's own runs, either side of the forbidden span, go through.
  CHECK(door.writeFlash(0, kMbr, 4).ok());
  CHECK(door.writeFlash(kProfile.app.begin - 4, words, 1).ok());  // the SoftDevice's last word
  CHECK(door.writeFlash(kProfile.bootloader_start, kBootloader, 4).ok());
  CHECK(door.writeFlash(0x000FFFFC, words, 1).ok());  // the last word of flash (the settings page)
  CHECK(door.counters().flash_words == 10);
  CHECK(b.chip.counters().writes == 10);
  CHECK(b.chip.flashWord(0) == 0x20000400);
  CHECK(b.chip.flashWord(kProfile.bootloader_start) == 0x20010000);
  CHECK(b.chip.flashWord(kProfile.app.begin) == 0xFFFFFFFF);
  CHECK(b.chip.flashWord(kProfile.app.end) == 0xFFFFFFFF);
  CHECK(b.chip.errors().empty());
  b.close();
}

// ---- the erase on each factory state ----------------------------------------------------------------

TEST_CASE("eraseAll on each factory state: flash, UICR and RAM to 0xFF, the status read back, the port as the kind says") {
  for (const Factory& f : kFactories) {
    DoorBench b(f.improved, f.locked);
    const core::nrf52::Result session = b.session();
    CHECK(session.ok() == !f.locked);
    const std::uint32_t approtect_before = b.chip.uicrWord(regs::uicr::kApprotect);
    CHECK(b.chip.flashWord(0) != 0xFFFFFFFF);  // RUI3
    CHECK(b.chip.flashWord(kProfile.app.begin) != 0xFFFFFFFF);
    ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(f.locked ? "locked" : kThisDeviceId));
    REQUIRE(door.open().ok());
    const DoorResult r = door.eraseAll();
    CHECK(r.ok());
    CHECK(door.erased());
    CHECK(b.chip.counters().erase_alls == 1);
    CHECK(b.chip.counters().erase_all_status_reads == 4);  // busy three times, then ready
    CHECK(b.chip.flashWord(0) == 0xFFFFFFFF);
    CHECK(b.chip.flashWord(kProfile.app.begin) == 0xFFFFFFFF);
    CHECK(b.chip.flashWord(0xFFFFC) == 0xFFFFFFFF);
    CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0xFFFFFFFF);
    CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFFFF);
    CHECK(b.chip.uicrWord(regs::uicr::kRegOut0) == 0xFFFFFFFF);
    if (f.improved) {
      // "Disabled via ERASEALL": open now, and the session is identified again.
      CHECK(r.port_open);
      CHECK(!b.chip.approtect);
      CHECK(b.nrf.protection() == Protection::kOpen);
      CHECK(b.nrf.identified());
      CHECK(b.nrf.identity().device_id[1] == 0x0BADF00D);
    } else if (f.locked) {
      // The erased UICR takes effect at the next reset: still closed, said so, nothing assumed.
      CHECK(!r.port_open);
      CHECK(b.chip.approtect);
      CHECK(b.nrf.protection() == Protection::kProtected);
      CHECK(!b.nrf.identified());
    } else {
      CHECK(r.port_open);
      CHECK(b.nrf.identified());
    }
    CHECK(door.counters().reconnects == 1);
    CHECK(b.chip.resets().pin == 0);
    CHECK(b.chip.resets().power_on == 0);
    CHECK(b.chip.resets().ctrl_ap_soft == 0);
    MESSAGE(std::string(f.name) << ": UICR.APPROTECT " << hex(approtect_before) << " -> erased; port after the erase "
                                << std::string(r.port_open ? "open" : "closed"));
    b.close();
  }
}

TEST_CASE("softReset: a legacy part reads its erased UICR and opens; an improved part keeps the port; with and without a reconnect needed") {
  for (const bool drops_dap : {false, true}) {
    for (const Factory& f : kFactories) {
      DoorBench b(f.improved, f.locked);
      b.chip.soft_reset_drops_dap = drops_dap;
      b.session();
      ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(f.locked ? "locked" : kThisDeviceId));
      REQUIRE(door.open().ok());
      REQUIRE(door.eraseAll().ok());
      const unsigned line_resets = b.target.counters().line_resets;
      const DoorResult r = door.softReset();
      CHECK(r.ok());
      CHECK(r.port_open);
      CHECK(b.chip.resets().ctrl_ap_soft == 1);
      CHECK(b.chip.resets().pin == 0);
      CHECK(b.chip.resets().power_on == 0);
      CHECK(!b.chip.approtect);
      CHECK(b.nrf.protection() == Protection::kOpen);
      CHECK(b.nrf.identified());
      CHECK(b.target.counters().line_resets > line_resets);  // reconnected, whether it was needed or not
      CHECK(door.counters().soft_resets == 1);
      CHECK(door.counters().reconnects == 2);
      // Still erased in this opening: the writes are allowed.
      CHECK(door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).ok());
      CHECK(b.target.errors().empty());
      b.close();
    }
  }
}

TEST_CASE("why the reconnect: a part that drops its debug port at the soft reset answers a host that does not reconnect with nothing") {
  DoorBench b(true, false);
  b.chip.soft_reset_drops_dap = true;
  b.target.strict = false;  // the refusal is what this test is about
  REQUIRE(b.session().ok());
  // The soft reset by hand, the way a host that trusted the port to survive would do it.
  REQUIRE(b.dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kReset, 1).ok());
  REQUIRE(b.dp.writeAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kReset, 0).ok());
  CHECK(b.chip.resets().ctrl_ap_soft == 1);
  const core::swd::Result r = b.dp.readAp(regs::ctrl_ap::kApsel, regs::ctrl_ap::kApprotectStatus);
  CHECK(!r.ok());
  CHECK(b.target.countErrors("reset state") >= 1);
  MESSAGE("without a reconnect the target said: " << b.target.errors()[0].what);
  b.target.clearErrors();
  // And the door's way: reconnect, and the port answers.
  CHECK(b.nrf.open().ok());
  CHECK(b.nrf.identified());
  b.close();
}

// ---- the whole of S-5a at the door's level --------------------------------------------------------

TEST_CASE("each factory state, provisioned through the door: the base word for word, APPROTECT as the kind says, one pin reset, the port after it") {
  for (const Factory& f : kFactories) {
    for (const bool bootloader_opens_port : {true, false}) {
      if (!f.improved && !bootloader_opens_port) continue;  // a legacy part does not care
      DoorBench b(f.improved, f.locked);
      b.chip.bootloader_opens_port = bootloader_opens_port;
      b.session();
      ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(f.locked ? "locked" : kThisDeviceId));
      REQUIRE(door.open().ok());
      // Step 4: the erase, the one soft reset if the port is still closed, the blank check.
      DoorResult r = b.eraseAndOpenPort(door);
      REQUIRE(r.ok());
      REQUIRE(r.port_open);
      BlankCheck blank;
      REQUIRE(door.blankCheck(&blank).ok());
      CHECK(blank.blank);
      CHECK(blank.flash0 == 0xFFFFFFFF);
      CHECK(blank.nrffw0 == 0xFFFFFFFF);
      CHECK(blank.approtect == 0xFFFFFFFF);
      CHECK(b.nrf.halt().ok());
      // Step 5: the base's runs - flash first, then UICR, then APPROTECT with the table's kind.
      REQUIRE(door.writeFlash(0, kMbr, 4).ok());
      REQUIRE(door.writeFlash(kProfile.bootloader_start, kBootloader, 4).ok());
      REQUIRE(door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).ok());
      REQUIRE(door.writeApprotect(b.kind()).ok());
      // S-5a: no pin, power-on or brownout reset between the erase and the last UICR write; the
      // one soft reset is allowed.
      CHECK(b.chip.resets().pin == 0);
      CHECK(b.chip.resets().power_on == 0);
      CHECK(b.chip.resets().ctrl_ap_soft == (f.locked && !f.improved ? 1u : 0u));
      // Step 6: every word read back.
      std::uint32_t back[4] = {};
      REQUIRE(b.nrf.read(0, back, 4).ok());
      for (unsigned i = 0; i < 4; ++i) CHECK(back[i] == kMbr[i]);
      REQUIRE(b.nrf.read(kProfile.bootloader_start, back, 4).ok());
      for (unsigned i = 0; i < 4; ++i) CHECK(back[i] == kBootloader[i]);
      REQUIRE(b.nrf.read(regs::uicr::kNrfFw0, back, 2).ok());
      CHECK(back[0] == kNrfFw[0]);
      CHECK(back[1] == kNrfFw[1]);
      REQUIRE(b.nrf.read(regs::uicr::kApprotect, back, 1).ok());
      const std::uint32_t want_approtect = f.improved ? 0x0000005Au : 0xFFFFFFFFu;
      CHECK(back[0] == want_approtect);
      CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == want_approtect);
      CHECK(b.chip.counters().uicr_writes == (f.improved ? 3u : 2u));
      CHECK(b.chip.counters().uicr_overwrites == 0);
      CHECK(b.chip.counters().writes == 8);
      CHECK(b.chip.config() == regs::nvmc::kConfigRen);
      // Step 7: the halt released, one pin reset through RadioPins, the port looked at.
      REQUIRE(b.nrf.run().ok());
      b.close();
      REQUIRE(b.board.resetPulse() == Outcome::kOk);
      CHECK(b.chip.resets().pin == 1);
      REQUIRE(b.acquire() == Outcome::kOk);
      const core::nrf52::Result after = b.nrf.open();
      const bool open_after = after.ok();
      if (!f.improved) {
        CHECK(open_after);  // legacy: UICR.APPROTECT erased reads Disabled
      } else {
        CHECK(open_after == bootloader_opens_port);  // improved: HwDisabled and firmware that writes DISABLE
      }
      if (open_after) {
        CHECK(b.nrf.identified());
        std::uint32_t nrffw0 = 0;
        CHECK(b.nrf.read(regs::uicr::kNrfFw0, &nrffw0, 1).ok());
        CHECK(nrffw0 == 0x000F4000);
      } else {
        CHECK(after.status == Status::kProtected);
      }
      // Nothing of the flasher's: no page erase, no settings page; the base's windows held.
      CHECK(b.chip.counters().erases == 0);
      CHECK(b.chip.errors().empty());
      CHECK(b.target.errors().empty());
      CHECK(b.pins.violations.empty());
      MESSAGE(std::string(f.name)
              << std::string(f.improved ? (bootloader_opens_port ? ", a bootloader that opens the port" : ", a bootloader that does not") : "")
              << ": provisioned, UICR.APPROTECT " << hex(want_approtect) << ", port after the pin reset "
              << std::string(open_after ? "open" : "closed"));
      b.close();
    }
  }
}

TEST_CASE("a power-on after the erase on an improved part closes the port again: the door's writes are then refused by the chip") {
  DoorBench b(true, true);
  b.session();
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand("locked"));
  REQUIRE(door.open().ok());
  REQUIRE(door.eraseAll().ok());
  CHECK(!b.chip.approtect);
  // The rail went and came back (a brownout, or a cable): a power-on reset.
  b.chip.powerOn();
  CHECK(b.chip.resets().power_on == 1);
  CHECK(b.chip.approtect);  // enabled by default again, nothing in flash to open it
  // The door still believes it erased; the chip says protected, before anything is written.
  CHECK(b.nrf.open().status == Status::kProtected);
  CHECK(door.writeFlash(0, kMbr, 4).status == DoorStatus::kProtected);
  CHECK(door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).status == DoorStatus::kProtected);
  CHECK(door.writeApprotect(ApprotectKind::kImproved).status == DoorStatus::kProtected);
  CHECK(b.chip.counters().writes == 0);
  CHECK(b.chip.counters().uicr_writes == 0);
  // The command is simply run again (S-5a): a new grant, a new door, the same erase.
  ProvisionDoor again(b.nrf, b.dp, b.clock, grantFromTypedCommand("locked"));
  REQUIRE(again.open().ok());
  REQUIRE(again.eraseAll().ok());
  CHECK(again.writeFlash(0, kMbr, 4).ok());
  b.close();
}

TEST_CASE("the legacy kind writes nothing to APPROTECT; the improved kind writes 0x0000005A, the upper bytes cleared") {
  DoorBench b(false, false);
  b.session();
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  REQUIRE(door.open().ok());
  REQUIRE(b.eraseAndOpenPort(door).ok());
  const unsigned uicr_before = b.chip.counters().uicr_writes;
  DoorResult r = door.writeApprotect(ApprotectKind::kLegacy);
  CHECK(r.ok());
  CHECK(r.address == regs::uicr::kApprotect);
  CHECK(b.chip.counters().uicr_writes == uicr_before);
  CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0xFFFFFFFF);
  CHECK(std::string(core::nrf52::describe(ApprotectKind::kLegacy)).find("erased") != std::string::npos);
  // The door never takes a value: the kind is all a caller can say, and the improved kind's word
  // is 0x0000005A, not 0xFFFFFF5A (the whole word is written; FIRMWARE-MANIFEST.md §4a).
  r = door.writeApprotect(ApprotectKind::kImproved);
  CHECK(r.ok());
  CHECK(b.chip.uicrWord(regs::uicr::kApprotect) == 0x0000005A);
  CHECK(b.chip.counters().uicr_writes == uicr_before + 1);
  b.close();
}

// ---- the tripwire -------------------------------------------------------------------------------------

TEST_CASE("the tripwire fires in a test that did not arm the chip: the door's erase is a failed test, nothing erased") {
  sim::Nrf52Bench b;  // not a DoorBench: allowProvisioning() was never called
  b.chip.strict = false;  // the sentence is what this test is about; strict, it is a failed test
  b.chip.info_variant = kVariantImproved;
  b.chip.factoryOpen();
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.nrf.open().ok());
  const std::uint32_t word0 = b.chip.flashWord(0);
  ProvisionDoor door(b.nrf, b.dp, b.clock, grantFromTypedCommand(kThisDeviceId));
  REQUIRE(door.open().ok());
  const DoorResult r = door.eraseAll();
  // The simulator answered the status register (it has no erase to wait for) and the door
  // believes the erase happened; the test it runs in has already failed, and nothing changed.
  CHECK(r.ok());
  CHECK(b.chip.counters().tripwire == 1);
  CHECK(b.chip.countErrors("CTRL-AP ERASEALL written") == 1);
  CHECK(b.chip.counters().erase_alls == 0);
  CHECK(b.chip.flashWord(0) == word0);
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
  MESSAGE("the simulator said: " << b.chip.errors()[0]);
  // A UICR write through the door on the same chip: the fourth message, and nothing written.
  CHECK(door.writeUicr(regs::uicr::kNrfFw0, kNrfFw, 2).ok());  // the door cannot tell; the chip can
  CHECK(b.chip.counters().tripwire >= 2);
  CHECK(b.chip.countErrors("a write to UICR") >= 1);
  CHECK(b.chip.uicrWord(regs::uicr::kNrfFw0) == 0x000F4000);
  b.chip.clearErrors();
  b.nrf.close();
  b.release();
}
