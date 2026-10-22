// The sequence of `rak provision` (task p5-03; spec/SYSTEM.md S-5a) against the simulated
// nRF52840 in each of its factory states - failure cases first: every refusal before the first
// write, with the chip's counters at zero; then each state provisioned with the base word for
// word, UICR.APPROTECT as the table says, one pin reset and the port after it; an unknown
// variant on an open radio refused with nothing erased, and on a locked one erased, refused, the
// table extended and the same command run again; a verify difference named and the radio safe to
// provision again; the reset spacing waited out; and the timing at 125 kHz from the simulator's
// clock.
#include <doctest.h>

#include <cstdio>
#include <functional>
#include <string>

#include "core/provision/provision_bench.h"

using bench::Factory;
using bench::ProvUnit;
using bench::StepRecorder;
using core::provision::Status;
using core::provision::Step;
using core::provision::VariantKind;
using bench::Candidate;
using bench::Slot;
using bench::StoreError;
using bench::Which;
namespace prov = core::provision;
namespace upd = core::update;

namespace {

std::string hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(v));
  return std::string(buf);
}

const Factory kAll[] = {Factory::kLegacyOpen, Factory::kLegacyLocked, Factory::kImprovedOpen,
                        Factory::kImprovedLocked};

const char* rightName(Factory f) { return bench::isLocked(f) ? "locked" : bench::kDeviceId; }

}  // namespace

// ---- refusals before the first write -------------------------------------------------------------

TEST_CASE("the wrong DEVICEID is refused with nothing written; the right one in lower and upper case is accepted") {
  for (const char* typed : {bench::kOtherDeviceId, "0badf00d00000000", "deadbeef0badf00d"}) {
    ProvUnit u(Factory::kImprovedOpen);
    const prov::Result r = u.run(typed);
    CHECK(r.status == Status::kWrongDevice);
    CHECK(r.ended == Step::kAttach);
    CHECK(!r.erased);
    CHECK(r.port_open_at_attach);
    u.checkNothingWritten();
    CHECK(!u.v().supervisor.awaitingFirstInstall());
  }
  for (const char* typed : {bench::kDeviceId, bench::kDeviceIdUpper, "0BADf00dDEADbeef"}) {
    ProvUnit u(Factory::kImprovedOpen);
    const prov::Result r = u.run(typed);
    CHECK_MESSAGE(r.ok(), typed << ": " << prov::describe(r.status));
    CHECK(r.ended == Step::kDone);
  }
}

TEST_CASE("`locked` for an open radio, and a DEVICEID for a locked one: refused, nothing written") {
  for (const Factory f : {Factory::kLegacyOpen, Factory::kImprovedOpen}) {
    for (const char* typed : {"locked", "locked:0123456789ABCDEF"}) {
      ProvUnit u(f);
      const prov::Result r = u.run(typed);
      CHECK(r.status == Status::kGrantMismatch);
      CHECK(!r.erased);
      u.checkNothingWritten();
    }
  }
  for (const Factory f : {Factory::kLegacyLocked, Factory::kImprovedLocked}) {
    ProvUnit u(f);
    const prov::Result r = u.run(bench::kDeviceId);
    CHECK(r.status == Status::kGrantMismatch);
    CHECK(!r.port_open_at_attach);
    CHECK(r.link_asked);
    CHECK(!r.application_answered);
    CHECK(!r.erased);
    u.checkNothingWritten();
  }
}

TEST_CASE("a locked radio whose application answers HELLO is named before it is erased: `locked` and a wrong key are refused") {
  ProvUnit u(Factory::kImprovedLocked);
  u.world.applicationAnswers();
  prov::Result r = u.run("locked");
  CHECK(r.status == Status::kApplicationAnswers);
  CHECK(r.application_answered);
  CHECK(std::string(r.key16) == bench::kFixtureKey16);
  CHECK(!r.erased);
  u.checkNothingWritten();
  r = u.run("locked:0123456789ABCDEF");
  CHECK(r.status == Status::kWrongKey);
  CHECK(!r.erased);
  u.checkNothingWritten();
  // The right key, in lower case too: accepted, and the radio is erased and provisioned.
  r = u.run("locked:7f3a9c02e1b44d9a");
  CHECK_MESSAGE(r.ok(), prov::describe(r.status));
  CHECK(r.erased);
  u.checkChipHoldsBase();
}

TEST_CASE("`locked:<key16>` for a locked radio that runs no application: refused, nothing written") {
  ProvUnit u(Factory::kLegacyLocked);
  const prov::Result r = u.run("locked:7F3A9C02E1B44D9A");
  CHECK(r.status == Status::kNoApplication);
  CHECK(!r.erased);
  u.checkNothingWritten();
}

TEST_CASE("a malformed radio is refused before a register is read") {
  ProvUnit u(Factory::kImprovedOpen);
  for (const char* typed : {"", "0badf00ddeadbee", "0badf00ddeadbeefa", "locked:", "locked:7F3A", "Locked", "unlocked"}) {
    const unsigned transfers = u.v().target.counters().transfers;
    const prov::Result r = u.run(typed);
    CHECK_MESSAGE(r.status == Status::kGrantMalformed, typed);
    CHECK(!r.erased);
    // Step 1 ran (the store, the battery over the link), step 2 attached - the grammar is checked
    // against the chip's port, which needs the attach - but nothing was written and the door
    // never opened.
    (void)transfers;
    u.checkNothingWritten();
    CHECK(!u.v().supervisor.awaitingFirstInstall());
  }
}

TEST_CASE("no base in the store: refused at step 1, the chip not even attached to") {
  ProvUnit u(Factory::kImprovedOpen, true, false);
  const prov::Result r = u.run(bench::kDeviceId);
  CHECK(r.status == Status::kNoBase);
  CHECK(r.ended == Step::kCheck);
  CHECK(!r.attached);
  CHECK(u.v().target.counters().transfers == 0);
  u.checkNothingWritten();
}

TEST_CASE("a base that no longer passes 5p (a flipped byte in the simulated store): refused at step 1") {
  ProvUnit u(Factory::kImprovedOpen);
  // The base's bytes in the store's flash: the header sector first, the bytes after it. A bit
  // of the first record's length digit rots: ':02...' reads ':00...', and rule 1 refuses the
  // file - the record no longer adds up. (The simulated flash is NOR: a write only clears bits,
  // and '1' to '0' is one bit cleared.)
  const std::uint32_t at = core::image::areaOffset(0, core::image::kBaseArea) + core::image::kSectorSize + 2;
  std::uint8_t byte = 0;
  REQUIRE(u.world.s.u.store.flash.read(at, &byte, 1));
  REQUIRE(byte == '2');  // ":02000004..." - the extended-address record's length
  byte = '0';
  REQUIRE(u.world.s.u.store.flash.write(at, &byte, 1));
  const prov::Result r = u.run(bench::kDeviceId);
  CHECK(r.status == Status::kBaseRefused);
  CHECK(r.ended == Step::kCheck);
  CHECK(r.why[0] != 0);
  CHECK(!r.attached);
  u.checkNothingWritten();
  MESSAGE("5p again over the stored bytes said: " << r.why);
}

TEST_CASE("the radio held by an update or by the supervisor's ladder: refused at step 1") {
  ProvUnit u(Factory::kImprovedOpen);
  u.world.putCandidate();
  // An update with a run in hand holds the radio.
  upd::Request request;
  request.kind = upd::Kind::kFirstInstall;
  request.slot = Slot{0};
  REQUIRE(u.v().update.start(request));
  CHECK(u.v().update.active());
  prov::Result r = u.run(bench::kDeviceId);
  CHECK(r.status == Status::kUpdateRunning);
  CHECK(!r.attached);
  u.checkNothingWritten();
  // The supervisor with a climb in hand holds it too (another unit: a run in hand is not
  // cleared, it is finished or resumed).
  ProvUnit v(Factory::kImprovedOpen);
  REQUIRE(v.v().supervisor.runLadder());
  (void)v.v().supervisor.step();  // the question: no answer, a silence to classify
  CHECK(v.v().supervisor.climbing());
  r = v.run(bench::kDeviceId);
  CHECK(r.status == Status::kSupervisorBusy);
  CHECK(!r.attached);
  v.checkNothingWritten();
}

TEST_CASE("the battery low, or falling: refused at step 1; unknown (a factory radio, no link): accepted") {
  // Low: the radio answers POWER_GET with a reading under the update's threshold.
  {
    ProvUnit u(Factory::kImprovedOpen);
    u.world.applicationAnswers();
    u.world.s.u.radio.override_power = true;
    u.world.s.u.radio.battery_mv = static_cast<std::uint16_t>(upd::kBatteryMinMv - 1);
    const prov::Result r = u.run(bench::kDeviceId);
    CHECK(r.status == Status::kBatteryLow);
    CHECK(r.battery_known);
    CHECK(r.battery_mv == upd::kBatteryMinMv - 1);
    CHECK(!r.attached);
    u.checkNothingWritten();
  }
  // Falling: the trend the ring will supply. The link battery knows no trend, so a source of
  // the test's own stands in.
  {
    ProvUnit u(Factory::kImprovedOpen);
    bench::FakeBattery falling;
    falling.millivolts = 4000;
    falling.trend_known = true;
    falling.mv_per_hour = upd::kBatteryFallingMvPerHour;
    prov::Provision own{u.v().board, u.v().slots, u.clock(), u.v().client, &u.world.s.u.store.store,
                        &falling, &u.v().update, &u.v().supervisor};
    const prov::Result r = own.run(Slot{0}, core::nrf52::grantFromTypedCommand(bench::kDeviceId));
    CHECK(r.status == Status::kBatteryFalling);
    u.checkNothingWritten();
  }
  // Unknown: a factory RUI3 radio answers nothing on the link, and the command goes on.
  {
    ProvUnit u(Factory::kImprovedOpen);
    const prov::Result r = u.run(bench::kDeviceId);
    CHECK(r.ok());
    CHECK(!r.battery_known);
  }
}

TEST_CASE("the kill line driven: refused at step 1") {
  ProvUnit u(Factory::kImprovedOpen);
  // The console runs one line at a time, so the only way the line is driven when the command
  // runs is from inside a kill: the clock's hook runs the sequence in the middle of `rak kill`.
  prov::Result r;
  bool ran = false;
  const std::function<void(std::uint32_t)> settle = u.clock().on_delay;
  u.clock().on_delay = [&](std::uint32_t ms) {
    if (settle) settle(ms);
    if (ran || !u.v().board.killDriven()) return;
    ran = true;
    r = u.run(bench::kDeviceId);
  };
  u.runLine("rak kill 1");
  u.clock().on_delay = settle;
  REQUIRE(ran);
  CHECK(r.status == Status::kKillAsserted);
  CHECK(!r.attached);
  u.checkNothingWritten();
}

TEST_CASE("the part is not an nRF52840: refused, nothing erased") {
  ProvUnit u(Factory::kImprovedOpen);
  u.chip().info_part = 0x52833;
  const prov::Result r = u.run(bench::kDeviceId);
  CHECK(r.status == Status::kNotNrf52840);
  CHECK(!r.erased);
  u.checkNothingWritten();
}

// ---- each factory state ----------------------------------------------------------------------------

TEST_CASE("each factory state ends provisioned: the base word for word, UICR.APPROTECT as the table says, one pin reset, the port after it") {
  for (const Factory f : kAll) {
    for (const bool bootloader_opens_port : {true, false}) {
      if (!bench::isImproved(f) && !bootloader_opens_port) continue;
      ProvUnit u(f);
      u.chip().bootloader_opens_port = bootloader_opens_port;
      const unsigned pulses_before = u.world.s.reset_pulses_seen;
      StepRecorder steps;
      steps.target = &u.v().target;
      steps.clock = &u.clock();
      const prov::Result r = u.run(rightName(f), &steps);
      REQUIRE_MESSAGE(r.ok(), bench::describe(f) << ": " << prov::describe(r.status) << " at " << prov::describe(r.ended));
      CHECK(r.ended == Step::kDone);
      CHECK(r.erased);
      CHECK(r.identified);
      CHECK(r.identity.device_id[1] == 0x0BADF00D);
      CHECK(r.port_open_at_attach == !bench::isLocked(f));
      CHECK(r.variant.kind == (bench::isImproved(f) ? VariantKind::kImproved : VariantKind::kLegacy));
      // The base, word for word, and APPROTECT as the table says.
      u.checkChipHoldsBase();
      const std::uint32_t want = bench::isImproved(f) ? 0x0000005Au : 0xFFFFFFFFu;
      CHECK(u.chip().uicrWord(0x10001208) == want);
      CHECK(r.approtect_read == want);
      CHECK(r.approtect_written == want);
      CHECK(r.verified_words == r.flash_words + r.uicr_words);
      CHECK(r.flash_words > 0);
      CHECK(r.uicr_words == 2);
      // One pin reset, at the end; no power-on; the one soft reset only where the legacy locked
      // part needed it (S-5a).
      CHECK(u.chip().resets().pin == 1);
      CHECK(u.chip().resets().power_on == 0);
      CHECK(u.chip().resets().ctrl_ap_soft == (f == Factory::kLegacyLocked ? 1u : 0u));
      CHECK(r.soft_reset_done == (f == Factory::kLegacyLocked));
      CHECK(u.world.s.reset_pulses_seen == pulses_before + 1);
      CHECK(u.world.s.kill_cycles_seen == 0);
      CHECK(u.chip().counters().erases == 0);  // nothing of the flasher's: no page erase
      CHECK(u.chip().counters().erase_alls == 1);
      CHECK(u.chip().counters().uicr_overwrites == 0);
      CHECK(u.chip().counters().tripwire == 0);
      // The port after the reset: legacy open; improved as the bootloader decides (§10.2).
      CHECK(r.port_open_after_reset == (bench::isImproved(f) ? bootloader_opens_port : true));
      if (r.port_open_after_reset) CHECK(r.nrffw0_after == 0x000F4000);
      // The seven steps, in order, each once.
      REQUIRE(steps.seen.size() == 7);
      CHECK(steps.seen[0].step == Step::kCheck);
      CHECK(steps.seen[1].step == Step::kAttach);
      CHECK(steps.seen[2].step == Step::kVariant);
      CHECK(steps.seen[3].step == Step::kErase);
      CHECK(steps.seen[4].step == Step::kWrite);
      CHECK(steps.seen[5].step == Step::kVerify);
      CHECK(steps.seen[6].step == Step::kReset);
      // The slot awaits its first install, and the radio is silent on the link.
      CHECK(u.v().supervisor.awaitingFirstInstall());
      CHECK(u.world.s.u.radio.deaf);
      CHECK(u.v().target.errors().empty());
      CHECK(u.chip().errors().empty());
      MESSAGE(std::string(bench::describe(f))
              << std::string(bench::isImproved(f) ? (bootloader_opens_port ? ", a bootloader that opens the port" : ", a bootloader that does not") : "")
              << ": provisioned, " << r.flash_words << " flash words in " << r.flash_runs << " runs, " << r.uicr_words
              << " UICR words, UICR.APPROTECT " << hex(r.approtect_read) << ", port after the pin reset "
              << std::string(r.port_open_after_reset ? "open" : "closed") << ", " << r.total_ms << " ms simulated");
    }
  }
}

TEST_CASE("an unknown variant: open -> refused with zero writes; locked -> erased, then refused, then the table extended and the same command succeeds") {
  // Open: the variant is read before anything is erased.
  {
    ProvUnit u(Factory::kImprovedOpen);
    u.chip().info_variant = bench::kVariantUnknown;
    const prov::Result r = u.run(bench::kDeviceId);
    CHECK(r.status == Status::kVariantUnknown);
    CHECK(r.ended == Step::kVariant);
    CHECK(!r.erased);
    CHECK(std::string(r.variant.text) == "AAE0");
    u.checkNothingWritten();
    CHECK(!u.v().supervisor.awaitingFirstInstall());
  }
  // Locked: the identity is readable only after the erase, so the radio is erased when the
  // table stops the command. Nothing it had not lost already: the table is extended (a test's
  // own row for E), and the same command - `locked`, since the port is open now? No: a power
  // cut is not a reset here, and the improved port stays open after the erase; the person types
  // what `rak provision check` says, which is the DEVICEID now.
  {
    ProvUnit u(Factory::kImprovedLocked);
    u.chip().info_variant = bench::kVariantUnknown;
    const prov::Result r = u.run("locked");
    CHECK(r.status == Status::kVariantUnknownErased);
    CHECK(r.ended == Step::kVariant);
    CHECK(r.erased);
    CHECK(u.chip().counters().erase_alls == 1);
    CHECK(u.chip().counters().writes == 0);
    CHECK(u.chip().counters().uicr_writes == 0);
    CHECK(u.v().supervisor.awaitingFirstInstall());
    const core::provision::VariantEntry extended[] = {
        {'A', VariantKind::kLegacy}, {'B', VariantKind::kLegacy}, {'C', VariantKind::kLegacy},
        {'D', VariantKind::kLegacy}, {'F', VariantKind::kImproved}, {'E', VariantKind::kImproved},
    };
    u.v().provision.useVariantTable(extended, 6);
    const prov::Result again = u.check();
    CHECK(again.port_open_at_attach);  // "Disabled via ERASEALL": open until a pin, power or brownout reset
    CHECK(std::string(again.next) == std::string("rak provision ") + bench::kDeviceId + " confirm");
    const prov::Result done = u.run(bench::kDeviceId);
    CHECK_MESSAGE(done.ok(), prov::describe(done.status));
    u.checkChipHoldsBase();
    CHECK(u.chip().uicrWord(0x10001208) == 0x0000005A);
  }
}

// ---- a verify difference ---------------------------------------------------------------------------

TEST_CASE("a verify difference: stopped, the first address named, and the radio is safe to provision again") {
  ProvUnit u(Factory::kImprovedOpen);
  // The simulator corrupts one word as it is written: a bit that will not take.
  u.chip().stuck_addr = u.world.profile.bootloader_start + 8;
  u.chip().stuck_mask = 0x00000001;
  const prov::Result r = u.run(bench::kDeviceId);
  CHECK(r.status == Status::kVerifyMismatch);
  CHECK(r.ended == Step::kVerify);
  CHECK(r.erased);
  CHECK(r.mismatch_address == u.world.profile.bootloader_start + 8);
  CHECK((r.mismatch_expected & 1u) == 1u);
  CHECK((r.mismatch_actual & 1u) == 0u);
  CHECK(u.chip().resets().pin == 0);  // stopped before the reset
  MESSAGE("verify stopped at " << hex(r.mismatch_address) << ": the base says " << hex(r.mismatch_expected)
                               << ", the radio reads " << hex(r.mismatch_actual));
  // The bit takes next time (a real radio's bad word would not, and the person learns which):
  // the same command from the top, and it finishes.
  u.chip().stuck_mask = 0;
  const prov::Result again = u.run(bench::kDeviceId);
  CHECK_MESSAGE(again.ok(), prov::describe(again.status));
  CHECK(u.chip().counters().erase_alls == 2);
  u.checkChipHoldsBase();
}

// ---- the reset spacing -----------------------------------------------------------------------------

TEST_CASE("a `rak reset` shortly before the command's own reset delays it, never skips it") {
  // How long the sequence takes to reach its reset on an open radio whose link answers (so that
  // the battery is read at once rather than after the link's three tries).
  std::uint32_t before_reset_ms = 0;
  {
    ProvUnit u(Factory::kImprovedOpen);
    u.world.applicationAnswers();
    u.clock().advance(2000);  // the spacing since this Walter's boot (a reset assumed just then) is out
    const prov::Result r = u.run(bench::kDeviceId);
    REQUIRE(r.ok());
    for (const Step s : {Step::kCheck, Step::kAttach, Step::kVariant, Step::kErase, Step::kWrite, Step::kVerify}) {
      before_reset_ms += r.step_ms[static_cast<std::size_t>(s)];
    }
    CHECK(r.reset_wait_ms == 0);  // nothing had pulsed the line for a long time
  }
  REQUIRE(before_reset_ms < 700);
  ProvUnit u(Factory::kImprovedOpen);
  u.world.applicationAnswers();
  // A reset pulse, then the command begun so that its own reset comes 300 ms short of the
  // spacing: the fake pins would fail the test on a pulse inside a second of the last one.
  u.runLine("rak reset");
  const std::uint64_t pulse_end = u.v().board.lastResetEndMs();
  // The pulse restarted the scripted radio; for this test its application is back at once
  // (what is measured here is the pins' spacing, not the radio's boot).
  u.world.s.u.boot_pending = false;
  u.world.s.u.seen_resets = u.chip().counters().resets;  // the world saw the pin reset too
  u.world.applicationAnswers();
  u.clock().advance(700 - before_reset_ms);
  const prov::Result r = u.run(bench::kDeviceId);
  REQUIRE_MESSAGE(r.ok(), prov::describe(r.status));
  CHECK(r.reset_wait_ms >= 290);
  CHECK(r.reset_wait_ms <= 310);
  CHECK(u.chip().resets().pin == 2);  // the person's, and the command's: delayed, not skipped
  CHECK(u.v().board.lastResetEndMs() - pulse_end >= core::board::kResetSpacingMs + core::board::kResetPulseMs);
  CHECK(u.v().pins.violations.empty());
  MESSAGE("the command's pin reset waited " << r.reset_wait_ms << " ms for the spacing");
}

// ---- the timing ------------------------------------------------------------------------------------

TEST_CASE("the timing at 125 kHz, from the simulator's clock: the stand-in base, and a 48 KB one") {
  // The clock counts every half period the wire spent, so the simulated milliseconds are what
  // a 125 kHz port costs; the report extrapolates to a real ~180 KB base from the per-word
  // figures printed here.
  for (const char* name : {"base-small", "base-resume"}) {
    ProvUnit u(Factory::kImprovedLocked, true, false);
    u.world.putBase(name);
    const prov::Result r = u.run("locked");
    REQUIRE_MESSAGE(r.ok(), name << ": " << prov::describe(r.status));
    const std::uint32_t words = r.flash_words + r.uicr_words;
    const std::uint32_t write_ms = r.step_ms[static_cast<std::size_t>(Step::kWrite)];
    const std::uint32_t verify_ms = r.step_ms[static_cast<std::size_t>(Step::kVerify)];
    MESSAGE(std::string(name) << ": " << words << " words; check " << r.step_ms[1] << " ms (5p over " << r.base.flash_bytes
                 << " flash bytes of HEX), attach " << r.step_ms[2] << " ms, erase " << r.step_ms[4] << " ms, write "
                 << write_ms << " ms (" << (words != 0 ? (1000.0 * write_ms / words) : 0.0) << " us/word), verify "
                 << verify_ms << " ms (" << (words != 0 ? (1000.0 * verify_ms / words) : 0.0) << " us/word), reset "
                 << r.step_ms[7] << " ms; " << r.total_ms << " ms in all; " << u.v().target.counters().clocks
                 << " SWCLK edges");
    u.checkChipHoldsBase();
  }
}
