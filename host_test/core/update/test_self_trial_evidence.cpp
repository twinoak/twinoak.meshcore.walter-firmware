// A rollback says what killed it (task p11-03), on the bench of self_bench.h.
//
// What happened on 9 November 2026: 0.1.3 was switched to over LTE, it never reached its main
// loop, the bootloader came back to 0.1.2, and the unit's whole account of it was *the image never
// reached its main loop (the bootloader gave up on it)*. The reason was on the board: the old
// image booted with `esp_reset_reason()` = panic and the `coredump` partition held
// *assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)*. These tests are that
// trial, made again with the real dump in the simulated partition:
//
//   * the finding boot records its own reset reason - which is the reset that ended the trial;
//   * a dump whose checksum holds **and whose application ELF SHA-256 is the trial image's** is
//     attributed to it, with its panic line and the task that crashed;
//   * a dump from any other image is not attributed, whatever else is true;
//   * it is recorded once, at the boot that can - and read back afterwards from the journal, with
//     the whole panic line out of the dump that nothing erases.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/update/self_update.h"
#include "fixtures/coredump_fixture.h"
#include "self_bench.h"

using bench::Candidate;
using bench::SelfBoot;
using bench::SelfPayload;
using bench::SelfReason;
using bench::SelfState;
using bench::SelfUnit;
namespace upd = core::update;
using Reset = core::health::ResetReason;
using Dump = core::health::DumpState;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// A unit that has switched to a new image in slot 1 and is now running it (the switch's reboot),
// with that slot's application description carrying the ELF SHA-256 the fixture's dump names -
// so that a dump found at the next boot is that image's own.
struct TrialOnTheMast {
  SelfUnit unit;
  Candidate image{"self-a"};

  explicit TrialOnTheMast(bool with_sha = true, bool with_dump = true) {
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    REQUIRE(unit.receiveAndSwitch(image));
    if (with_sha) unit.world.own.setElfSha(1, fixtures::kFixtureElfSha);
    if (with_dump) fixtures::putCoreDumpFixture(unit.world.coredump_flash);
    unit.reboot(Reset::kSoftware);
    REQUIRE(unit.world.own.runningIndex() == 1);
  }

  // The image never reaches its main loop: no `begin()` is called in that life, and the next
  // reset - a panic, here - aborts the slot and brings the old image back.
  SelfBoot killedByAPanic() {
    unit.reboot(Reset::kPanic);
    REQUIRE(unit.world.own.runningIndex() == 0);
    return unit.walter->boot();
  }

  upd::SelfUpdate::LastTrial trial() {
    upd::SelfUpdate::LastTrial t;
    REQUIRE(unit.walter->self.lastTrial(&t));
    return t;
  }
};

}  // namespace

TEST_CASE("the trial of 9 November, again: the reset reason, the panic line and the crashed task") {
  TrialOnTheMast t;
  CHECK(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
  CHECK(t.unit.walter->self.bootReason() == SelfReason::kNeverBooted);

  const upd::SelfUpdate::LastTrial trial = t.trial();
  // The reset that ended the trial: this boot's, and on 9 November it was a panic.
  CHECK(trial.reason == Reset::kPanic);
  CHECK(trial.dump == Dump::kPresent);
  CHECK(trial.attributed);
  CHECK(trial.dump_now);
  // The whole line, read back out of the dump (the journal's own copy is cut; see below).
  CHECK(std::string(trial.panic) == fixtures::kFixturePanic);
  CHECK_FALSE(trial.panic_cut);
  CHECK(std::string(trial.task) == fixtures::kFixtureCrashedTask);
}

TEST_CASE("the evidence entry is its own journal entry, bounded like every other, right after the rollback's") {
  TrialOnTheMast t;
  REQUIRE(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
  upd::Entry rollback;
  upd::Entry evidence;
  REQUIRE(t.unit.walter->journal.lastOf(upd::kind::kSelfUpdate, &rollback));
  REQUIRE(t.unit.walter->journal.lastOf(upd::kind::kTrialEvidence, &evidence));
  // Written after it, so that "the evidence is the last trial's" needs no bytes of its own.
  CHECK(evidence.seq == rollback.seq + 1);
  CHECK(evidence.size == upd::kEvidencePayloadBytes);
  CHECK(evidence.size <= upd::kPayloadMax);

  upd::TrialEvidence e;
  REQUIRE(upd::decode(evidence, &e));
  CHECK(e.reason == Reset::kPanic);
  CHECK(e.dump == Dump::kPresent);
  CHECK(e.attributed());
  CHECK(std::string(e.task) == fixtures::kFixtureCrashedTask);
  // **The entry's copy of the panic line is cut**, and says so: 48 bytes do not hold a
  // 69-character line beside the rest, which is why lastTrial() reads it again from the dump.
  CHECK(e.panicCut());
  CHECK(std::string(e.panic).size() == upd::kEvidencePanic - 1);
  CHECK(std::string(fixtures::kFixturePanic).rfind(e.panic, 0) == 0);  // a prefix of the real line

  // And the rollback's own entry is untouched by any of it.
  SelfPayload p;
  REQUIRE(upd::decode(rollback, &p));
  CHECK(p.state == SelfState::kRolledBack);
  CHECK(p.reason == SelfReason::kNeverBooted);

  // A self-update entry of its own is not decoded as evidence, and the other way round.
  CHECK_FALSE(upd::decode(rollback, &e));
  SelfPayload not_a_payload;
  CHECK_FALSE(upd::decode(evidence, &not_a_payload));
}

TEST_CASE("a dump whose ELF SHA-256 is not the trial image's is not attributed to it") {
  SUBCASE("the slot's image is another build") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    Candidate image{"self-a"};
    REQUIRE(unit.receiveAndSwitch(image));
    // A dump is there and its checksum holds, but slot 1's own application description names
    // another ELF: it is a dump of something that has since been replaced.
    unit.world.own.setElfSha(1, "deadbeef1");
    fixtures::putCoreDumpFixture(unit.world.coredump_flash);
    unit.reboot(Reset::kSoftware);
    unit.reboot(Reset::kPanic);
    REQUIRE(unit.walter->boot() == SelfBoot::kRollbackRecorded);
    upd::SelfUpdate::LastTrial trial;
    REQUIRE(unit.walter->self.lastTrial(&trial));
    CHECK(trial.reason == Reset::kPanic);   // the reset reason is still recorded
    CHECK(trial.dump == Dump::kPresent);    // and so is the fact that there was a dump
    CHECK_FALSE(trial.attributed);          // but nothing of it is this trial's
    CHECK(trial.panic[0] == 0);
    CHECK(trial.task[0] == 0);
  }
  SUBCASE("the slot carries no ELF SHA-256 at all") {
    TrialOnTheMast t(/*with_sha=*/false);
    REQUIRE(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
    const upd::SelfUpdate::LastTrial trial = t.trial();
    CHECK(trial.dump == Dump::kPresent);
    CHECK_FALSE(trial.attributed);
    CHECK(trial.panic[0] == 0);
  }
  SUBCASE("the dump's checksum does not hold: nothing in it is read, let alone attributed") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    Candidate image{"self-a"};
    REQUIRE(unit.receiveAndSwitch(image));
    unit.world.own.setElfSha(1, fixtures::kFixtureElfSha);
    std::vector<std::uint8_t> bytes = fixtures::coreDumpFixture();
    bytes[3140] = 0;
    fixtures::putDump(unit.world.coredump_flash, bytes);
    unit.reboot(Reset::kSoftware);
    unit.reboot(Reset::kPanic);
    REQUIRE(unit.walter->boot() == SelfBoot::kRollbackRecorded);
    upd::SelfUpdate::LastTrial trial;
    REQUIRE(unit.walter->self.lastTrial(&trial));
    CHECK(trial.dump == Dump::kBadChecksum);
    CHECK_FALSE(trial.attributed);
    CHECK(trial.panic[0] == 0);
  }
  SUBCASE("there is no dump at all: the reset reason alone") {
    TrialOnTheMast t(/*with_sha=*/true, /*with_dump=*/false);
    REQUIRE(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
    const upd::SelfUpdate::LastTrial trial = t.trial();
    CHECK(trial.reason == Reset::kPanic);
    CHECK(trial.dump == Dump::kErased);
    CHECK_FALSE(trial.attributed);
  }
}

TEST_CASE("a switch that was never taken: nothing was tried, so nothing is recorded about it") {
  SelfUnit unit;
  REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
  Candidate image{"self-a"};
  bool stored = false;
  const std::string text = unit.walter->pushSelf(image, &stored);
  REQUIRE_MESSAGE(stored, text);
  // The journal as a Walter that died right after writing *switching*: the entry there, the other
  // slot's state still undefined (test_self_update.cpp's own arrangement for this case).
  SelfPayload p = unit.lastEntry();
  p.state = SelfState::kSwitching;
  p.flags = upd::kFlagRadioUp;
  std::uint8_t bytes[upd::kSelfPayloadBytes];
  upd::encode(p, bytes);
  REQUIRE(unit.walter->journal.append(upd::kind::kSelfUpdate, bytes, sizeof bytes) ==
          upd::JournalError::kNone);
  // The slot's own description matches the dump - the same build has run before - so the three
  // checks of attribution would all pass. They are never asked: nothing of that image was
  // started, so this boot's reset has nothing to do with it.
  unit.world.own.setElfSha(1, fixtures::kFixtureElfSha);
  fixtures::putCoreDumpFixture(unit.world.coredump_flash);
  unit.reboot(Reset::kPanic);
  REQUIRE(unit.walter->boot() == SelfBoot::kRollbackRecorded);
  CHECK(unit.walter->self.bootReason() == SelfReason::kSwitchNotTaken);
  upd::Entry evidence;
  CHECK_FALSE(unit.walter->journal.lastOf(upd::kind::kTrialEvidence, &evidence));
  upd::SelfUpdate::LastTrial trial;
  CHECK_FALSE(unit.walter->self.lastTrial(&trial));
}

TEST_CASE("it is recorded once, at the boot that can: a later wake's reset reason says nothing") {
  TrialOnTheMast t;
  REQUIRE(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
  const upd::SelfUpdate::LastTrial found = t.trial();
  REQUIRE(found.reason == Reset::kPanic);
  upd::Entry first;
  REQUIRE(t.unit.walter->journal.lastOf(upd::kind::kTrialEvidence, &first));

  // The next wake is a timer wake from deep sleep - which is nothing to do with the trial - and
  // it writes no evidence of its own: what `lastTrial()` answers is still the panic.
  t.unit.reboot(Reset::kDeepSleepWake);
  CHECK(t.unit.walter->boot() == SelfBoot::kNothing);
  upd::Entry again;
  REQUIRE(t.unit.walter->journal.lastOf(upd::kind::kTrialEvidence, &again));
  CHECK(again.seq == first.seq);
  upd::SelfUpdate::LastTrial later;
  REQUIRE(t.unit.walter->self.lastTrial(&later));
  CHECK(later.reason == Reset::kPanic);
  CHECK(later.attributed);
  CHECK(std::string(later.panic) == fixtures::kFixturePanic);
}

TEST_CASE("the dump erased afterwards: the journal's cut copy is what is left, and it says so") {
  TrialOnTheMast t;
  REQUIRE(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
  REQUIRE(t.unit.walter->runLine("coredump erase confirm").find("erased") != std::string::npos);
  upd::SelfUpdate::LastTrial trial;
  REQUIRE(t.unit.walter->self.lastTrial(&trial));
  CHECK(trial.attributed);        // the finding was made, and it is not unmade by an erase
  CHECK_FALSE(trial.dump_now);    // but the dump is gone
  CHECK(trial.panic_cut);
  CHECK(std::string(trial.panic).size() == upd::kEvidencePanic - 1);
  CHECK(std::string(fixtures::kFixturePanic).rfind(trial.panic, 0) == 0);
}

TEST_CASE("a story that has moved on: the evidence is not reported for a trial that is not the last") {
  TrialOnTheMast t;
  REQUIRE(t.killedByAPanic() == SelfBoot::kRollbackRecorded);
  REQUIRE(t.unit.walter->self.lastTrial(nullptr) == false);  // a null out is refused, not crashed
  upd::SelfUpdate::LastTrial trial;
  REQUIRE(t.unit.walter->self.lastTrial(&trial));
  // A new image is received: the newest self-update entry is now newer than the evidence, so the
  // evidence is an older trial's and is not reported as this story's.
  t.unit.reboot(Reset::kDeepSleepWake);
  Candidate other{"self-b"};
  bool stored = false;
  const std::string text = t.unit.walter->pushSelf(other, &stored);
  REQUIRE_MESSAGE(stored, text);
  CHECK_FALSE(t.unit.walter->self.lastTrial(&trial));
}

TEST_CASE("self status and the boot line say what killed it") {
  TrialOnTheMast t;
  const SelfBoot boot = t.killedByAPanic();
  REQUIRE(boot == SelfBoot::kRollbackRecorded);
  const std::string status = t.unit.walter->runLine("self status");
  CHECK(has(status, "rolled back: the image never reached its main loop"));
  CHECK(has(status, "what ended the last trial: panic"));
  CHECK(has(status, "the core dump is that image's own: panic: "));
  CHECK(has(status, fixtures::kFixturePanic));
  CHECK(has(status, "the task that crashed: ipc0"));
  // Every line of it ends in a newline: the console cuts a line at 200 bytes and the panic line
  // is 69 of them, so the whole thing has to fit.
  CHECK(status.back() == '\n');
  CHECK(has(status, (std::string(fixtures::kFixturePanic) + "\n").c_str()));

  // And `coredump` says the same about the partition, naming the slot whose image wrote it.
  const std::string dump = t.unit.walter->runLine("coredump");
  CHECK(has(dump, "coredump: a core dump whose checksum holds"));
  CHECK(has(dump, "3204 bytes of 65536"));
  CHECK(has(dump, fixtures::kFixtureElfSha));
  CHECK(has(dump, "that is the other slot's image: slot 1"));
  CHECK(has(dump, fixtures::kFixturePanic));
  CHECK(has(dump, "2 tasks: ipc0 (crashed), ipc1"));
  CHECK(has(dump, "nothing erases it by itself"));
}
