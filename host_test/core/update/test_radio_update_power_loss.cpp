// The radio's update with the Walter resetting at every journal entry (task p2-03) - of the good
// path and of a rollback - and with the power going during a journal write. Phase 2's rule: "a
// test in which nothing goes wrong proves very little here."
//
// A reset "at" an entry is the Walter dying the moment the entry's magic word has reached flash
// and before the action it announces has been taken (or finished): its flashes stop answering,
// its debug port and its UART go dead, whatever the machine was doing fails, and a new Walter is
// built over the same world and calls resume(). The radio keeps whatever state it was in - a
// chip half flashed and in its bootloader, or an application running - because the Walter's
// reset is not the radio's.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/update/radio_update.h"
#include "update_bench.h"

using bench::Boot;
using bench::Kind;
using bench::ReadyUnit;
using bench::Reason;
using bench::State;
using bench::Which;
namespace upd = core::update;

namespace {

// Runs one scenario on a fresh unit: the candidate boots as `boot` says, and the golden image
// normally. Returns the end state; `entries` is how many journal entries the run wrote.
struct Scenario {
  Boot boot = Boot::kNormal;
  State expected = State::kDone;
};

// The clean run: how many journal entries it writes, and its outcome.
std::uint32_t entriesOfACleanRun(const Scenario& sc) {
  ReadyUnit unit;
  unit.world.boot = sc.boot;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run([&](const upd::StepResult& s) {
    if (s.changed && s.after == State::kRollingBack) unit.world.boot = Boot::kNormal;
  });
  REQUIRE(end == sc.expected);
  return w.journal.validEntries();
}

// What must hold at the end of a resumed run, whatever the reset did.
void checkOutcome(ReadyUnit& unit, const Scenario& sc) {
  if (sc.expected == State::kDone) {
    CHECK(unit.goldenVersion() == unit.candidate.version);
    CHECK_FALSE(unit.read(Which::kCandidate).present);
    CHECK(unit.world.chipHolds(unit.candidate));
    CHECK(unit.world.running_version == unit.candidate.version);
    CHECK(unit.world.store.store.badCount() == 0);
  } else {
    CHECK(unit.goldenVersion() == unit.golden.version);
    CHECK(unit.world.chipHolds(unit.golden));
    CHECK(unit.world.running_version == unit.golden.version);
    CHECK(unit.world.store.store.isBad(unit.read(Which::kCandidate).sha256));
  }
  CHECK(unit.world.store.store.verify(Which::kGolden, bench::Slot{0}) == bench::StoreError::kNone);
  // Every flash operation on the chip was inside the application's pages or the settings page:
  // the chip is told so (onlyTouch / alsoTouch) and fails the test itself otherwise; and no
  // forbidden register was ever touched.
  CHECK(unit.world.chip.counters().tripwire == 0);
  CHECK(unit.world.chip.errors().empty());
  CHECK(unit.world.chip.counters().overwrites == 0);
}

// The loop: for every entry k of the clean run, a fresh unit whose Walter dies right after
// writing entry k, is rebuilt, resumes, and must reach the same outcome.
void resetAtEveryEntry(const Scenario& sc, const char* name, std::string* one_story) {
  const std::uint32_t total = entriesOfACleanRun(sc);
  REQUIRE(total >= 6);
  int resumed_runs = 0;
  int already_terminal = 0;
  std::uint32_t resumed_entries_max = 0;
  for (std::uint32_t k = 1; k <= total; ++k) {
    CAPTURE(k);
    ReadyUnit unit;
    unit.world.boot = sc.boot;
    std::uint32_t written = 0;
    unit.world.journal_flash.on_op = [&](const sim::FlashOp& op) {
      if (op.kind != sim::FlashOp::Kind::kWrite || op.offset % upd::kEntryBytes != upd::kMagicOffset) return;
      if (++written == k) unit.walter->die();
    };
    REQUIRE(unit.walter->update.start(unit.request()));
    State end = unit.walter->run([&](const upd::StepResult& s) {
      if (s.changed && s.after == State::kRollingBack) unit.world.boot = Boot::kNormal;
    });
    REQUIRE(unit.world.dead);  // every k is inside the run
    std::string story = "reset after entry " + std::to_string(k) + " (" +
                        upd::describe(unit.walter->transitions.empty() ? State::kCheck : unit.walter->transitions.back().after) +
                        ")\n" + unit.walter->transitionText();

    // The Walter comes back and reads its journal.
    unit.world.journal_flash.on_op = nullptr;
    unit.reboot();
    bench::Walter& w = *unit.walter;
    const bool resumed = w.update.resume();
    if (!resumed) {
      // Only a terminal entry can leave nothing to resume - the last one of the run.
      ++already_terminal;
      CHECK(k == total);
      upd::Entry e;
      upd::Payload p;
      REQUIRE(w.journal.lastOf(upd::kind::kRadioUpdate, &e));
      REQUIRE(upd::decode(e, &p));
      CHECK(p.state == sc.expected);
      checkOutcome(unit, sc);
      continue;
    }
    ++resumed_runs;
    CHECK(w.update.status().resumed);
    end = w.run([&](const upd::StepResult& s) {
      if (s.changed && s.after == State::kRollingBack) unit.world.boot = Boot::kNormal;
    });
    story += "-- resumed at " + std::string(upd::describe(w.transitions.empty() ? w.update.state() : w.transitions.front().before)) + "\n" +
             w.transitionText();
    INFO(story);
    CHECK(end == sc.expected);
    CHECK_FALSE(unit.world.dead);
    checkOutcome(unit, sc);
    if (w.journal.validEntries() > resumed_entries_max) resumed_entries_max = w.journal.validEntries();
    // The journal's last entry is the terminal state, and the resumed entry carries its flag.
    upd::Entry e;
    upd::Payload p;
    REQUIRE(w.journal.lastOf(upd::kind::kRadioUpdate, &e));
    REQUIRE(upd::decode(e, &p));
    CHECK(p.state == sc.expected);
    if (one_story != nullptr && k == (total + 1) / 2) *one_story = story;
    // A second reset after the end has nothing to resume.
    unit.reboot();
    CHECK_FALSE(unit.walter->update.resume());
  }
  // std::string, not const char*: doctest streams a char pointer as a pointer.
  MESSAGE(std::string(name) << ": " << total << " journal entries in the clean run; the Walter reset after each in turn: "
                            << resumed_runs << " resumed and reached " << std::string(upd::describe(sc.expected)) << ", "
                            << already_terminal << " found the run already over; the longest resumed run wrote "
                            << resumed_entries_max << " entries");
}

}  // namespace

TEST_CASE("a Walter reset at every journal entry of the good path: resume() reaches done every time") {
  std::string story;
  resetAtEveryEntry(Scenario{Boot::kNormal, State::kDone}, "the good path", &story);
  MESSAGE("one resumed run of the good path:\n" << story);
}

TEST_CASE("a Walter reset at every journal entry of a rollback: resume() reaches restored every time") {
  std::string story;
  resetAtEveryEntry(Scenario{Boot::kSilent, State::kRestored}, "a rollback", &story);
  MESSAGE("one resumed run of a rollback:\n" << story);
}

TEST_CASE("the power goes during every journal write of the good path: the torn entry is ignored, the one before governs, and the run finishes") {
  // The clean run's journal operations (two writes per entry: the body, then the magic).
  std::uint32_t ops = 0;
  {
    ReadyUnit unit;
    REQUIRE(unit.walter->update.start(unit.request()));
    REQUIRE(unit.walter->run() == State::kDone);
    ops = unit.world.journal_flash.operations();
  }
  REQUIRE(ops >= 12);
  int torn_bodies = 0, torn_magics = 0;
  for (std::uint32_t op = 1; op <= ops; ++op) {
    CAPTURE(op);
    ReadyUnit unit;
    unit.world.journal_flash.lose_power_at_op = op;
    // A torn journal write is the Walter's power going: everything of the Walter stops with it.
    unit.world.journal_flash.on_power_loss = [&] { unit.walter->die(); };
    // When the cut is inside the very first entry (the check's), start() itself fails - the
    // machine does not act without its journal - and the Walter is dead before anything moved.
    const bool started = unit.walter->update.start(unit.request());
    if (started) {
      (void)unit.walter->run();
    } else {
      CHECK(unit.walter->update.reason() == Reason::kJournal);
      CHECK_FALSE(unit.walter->update.active());
    }
    REQUIRE(unit.world.dead);
    const std::uint32_t entries_before = (op - 1) / 2;  // whole entries written before the cut
    const bool torn_body = op % 2 == 1;
    if (torn_body) ++torn_bodies; else ++torn_magics;

    unit.world.journal_flash.on_power_loss = nullptr;
    unit.reboot();
    bench::Walter& w = *unit.walter;
    // The entry before the torn one governs: the journal's newest valid seq is the number of
    // whole entries, and the torn slot is used (the next entry goes past it).
    CHECK(w.journal.validEntries() == entries_before);
    CHECK(w.journal.nextSeq() == entries_before + 1);
    upd::Entry e;
    if (entries_before > 0) {
      REQUIRE(w.journal.last(&e));
      CHECK(e.seq == entries_before);
    } else {
      CHECK_FALSE(w.journal.last(&e));
    }
    const bool resumed = w.update.resume();
    if (entries_before == 0) {
      // Not even the check was journalled: nothing happened, and nothing is to resume. The
      // candidate is still there to try again.
      CHECK_FALSE(resumed);
      CHECK(unit.read(Which::kCandidate).present);
      REQUIRE(w.update.start(unit.request()));
    } else if (!resumed) {
      // The torn entry was the terminal one and the entry before it says committing: but a
      // committing entry is not terminal, so this cannot happen - unless the whole run was over.
      FAIL("nothing to resume after " << entries_before << " whole entries");
    }
    const State end = w.run();
    INFO(w.transitionText());
    CHECK(end == State::kDone);
    CHECK(unit.goldenVersion() == unit.candidate.version);
    CHECK(unit.world.running_version == unit.candidate.version);
    CHECK(unit.world.chip.counters().tripwire == 0);
  }
  MESSAGE("the good path's " << ops << " journal writes, the power going at each: " << torn_bodies
                             << " torn bodies and " << torn_magics
                             << " torn magic words, each ignored, the run finished every time");
}
