// A session with the Walter dying at every write (task p16-03, rails 1 and 2). Phase 2's rule
// said of the radio's update - "a test in which nothing goes wrong proves very little here" - and
// a session is where it matters most: between the two flashes the site has no repeater, and the
// only thing that gets it back is this board's own journal.
//
// Two lists of writes, both walked end to end:
//
//   * every entry the **session's** journal makes - `check`, `flashing-scanner`, `scanner`, each
//     step, `room`, `flashing-golden`, `booting`, `proving`, `restored`: the Walter dies the
//     moment the entry's magic word has reached flash and before the action it announces;
//   * every **page** of both flashes, through the flasher's own progress - the scan, the first
//     page, every page after it and the read-back. The flasher's steps are what a write of this
//     firmware's is in any sense a test can name (one page is thousands of bits on the simulated
//     SWD wire, and a bit is not a write); the flasher's own power-loss loops, a flash operation
//     at a time, are task p1-05's and task p2-03's.
//
// In every case the next boot's step 3 - `resume()` and nothing else: no server, no uplink, no
// console - must end with the golden image on the chip, proved, and the journal saying closed.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/cycle/wake_guard.h"
#include "core/flasher/flasher.h"
#include "core/update/scanner_session.h"
#include "session_bench.h"

using bench::SessionUnit;
using bench::Stage;
namespace upd = core::update;

namespace {

// The program every scenario here runs: one step whose `done` never comes, so that a cut can land
// inside a step as well as around it.
upd::Program oneStep() {
  return bench::programOf({{"dwell 869.618 60", 10}});
}

// What must hold once the dust has settled, whatever the cut did.
void checkTheRepeaterIsBack(SessionUnit& unit) {
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK(mark.found);
  CHECK_FALSE(mark.open);
  CHECK(mark.stage == Stage::kRestored);
  // The store never moved, and the golden image still verifies.
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.world.store.store.verify(bench::Which::kGolden, bench::Slot{0}) ==
        bench::StoreError::kNone);
  // Every flash operation was inside the application's pages or the settings page (the chip
  // fails the test itself otherwise), and no forbidden register was ever touched.
  CHECK(unit.world.chip.counters().tripwire == 0);
  CHECK(unit.world.chip.errors().empty());
  // And nothing of the heap is still out: a session whose Walter died took its room with it.
  CHECK(unit.walter->scratch.out == 0);
}

// The next boot, over and over: `resume()` and `step()` until the session is closed or it has had
// its tries. Answers how many boots it took.
int bootsUntilClosed(SessionUnit& unit, int at_most = 6) {
  int boots = 0;
  for (; boots < at_most; ++boots) {
    unit.world.journal_flash.on_op = nullptr;
    unit.reboot();
    // What step 3 does, in main's own order: the radio's own update first, then the session.
    if (unit.walter->update.resume()) (void)unit.walter->run();
    if (!unit.session().resume()) break;
    (void)unit.run();
    if (unit.session().stage() == Stage::kRestored) {
      ++boots;
      break;
    }
  }
  return boots;
}

// One clean run of the session, to learn how many journal entries it writes.
std::uint32_t entriesOfACleanRun() {
  SessionUnit unit;
  bench::Answer slow;
  slow.done = false;
  unit.scripted.answers["dwell"] = slow;
  const upd::Program program = oneStep();
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  REQUIRE(unit.run() == Stage::kRestored);
  std::uint32_t entries = 0;
  for (Stage stage : unit.journalledStages()) {
    (void)stage;
    ++entries;
  }
  return entries;
}

}  // namespace

TEST_CASE("the power cut at every entry the session's journal makes: the repeater always comes back") {
  const std::uint32_t total = entriesOfACleanRun();
  REQUIRE(total >= 8);
  int resumed_runs = 0;
  int already_closed = 0;
  for (std::uint32_t k = 1; k <= total; ++k) {
    CAPTURE(k);
    SessionUnit unit;
    bench::Answer slow;
    slow.done = false;
    unit.scripted.answers["dwell"] = slow;
    const upd::Program program = oneStep();
    // The Walter dies the moment the k-th **session** entry's magic word is in flash. Only the
    // session's own kind is counted, so k is an entry of the list this test is about; the radio
    // update's entries fall between them and are not cut points here.
    std::uint32_t written = 0;
    unit.world.journal_flash.on_op = [&](const sim::FlashOp& op) {
      if (op.kind != sim::FlashOp::Kind::kWrite) return;
      if (op.offset % upd::kEntryBytes != upd::kMagicOffset) return;
      if (++written == k) unit.walter->die();
    };
    REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
    (void)unit.run();

    if (!unit.world.dead) {
      // k was past the run's own entries (the radio update's entries share the count): the
      // session closed by itself, which is the other acceptable answer.
      ++already_closed;
      unit.world.journal_flash.on_op = nullptr;
      CHECK(unit.session().stage() == Stage::kRestored);
      checkTheRepeaterIsBack(unit);
      continue;
    }
    ++resumed_runs;
    const int boots = bootsUntilClosed(unit);
    CHECK_MESSAGE(boots >= 1, "the cut left nothing for a wake to take up");
    CHECK(unit.session().stage() == Stage::kRestored);
    checkTheRepeaterIsBack(unit);
  }
  MESSAGE("the session wrote " << total << " entries; " << resumed_runs
                               << " cuts were resumed, " << already_closed
                               << " landed past the run");
  CHECK(resumed_runs >= 6);
}

namespace {

// The flasher's own progress, counted - and the Walter killed at the Nth of them.
struct DieAtPage final : core::flasher::ProgressSink {
  SessionUnit* unit = nullptr;
  std::uint32_t at = 0;   // 0 counts and kills nothing
  std::uint32_t seen = 0;

  void onProgress(const core::flasher::Progress&) override {
    ++seen;
    if (at != 0 && seen == at) unit->walter->die();
  }
};

}  // namespace

TEST_CASE("the power cut at every page of both flashes: the repeater always comes back") {
  // How many of the flasher's steps a whole session is, so that the cut points cover both
  // flashes and not the first one only.
  std::uint32_t total_pages = 0;
  {
    SessionUnit unit;
    bench::Answer slow;
    slow.done = false;
    unit.scripted.answers["dwell"] = slow;
    const upd::Program program = oneStep();
    DieAtPage counter;
    counter.unit = &unit;
    unit.walter->update.setProgress(&counter);
    REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
    REQUIRE(unit.run() == Stage::kRestored);
    unit.walter->update.setProgress(nullptr);
    total_pages = counter.seen;
  }
  REQUIRE(total_pages >= 8);
  int cuts = 0;
  for (std::uint32_t page = 1; page <= total_pages; ++page) {
    CAPTURE(page);
    SessionUnit unit;
    bench::Answer slow;
    slow.done = false;
    unit.scripted.answers["dwell"] = slow;
    const upd::Program program = oneStep();
    DieAtPage killer;
    killer.unit = &unit;
    killer.at = page;
    unit.walter->update.setProgress(&killer);
    REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
    (void)unit.run();
    unit.walter->update.setProgress(nullptr);
    if (!unit.world.dead) {
      CHECK(unit.session().stage() == Stage::kRestored);
      checkTheRepeaterIsBack(unit);
      continue;
    }
    ++cuts;
    const int boots = bootsUntilClosed(unit);
    CHECK_MESSAGE(boots >= 1, "the cut left nothing for a wake to take up");
    CHECK(unit.session().stage() == Stage::kRestored);
    checkTheRepeaterIsBack(unit);
  }
  MESSAGE("a session is " << total_pages << " flasher steps; " << cuts << " cuts were resumed");
  CHECK(cuts >= 8);
}

TEST_CASE("the bound on the wake ends a step: the next boot ends the session") {
  // What the wake guard does is a reset (core/cycle/wake_guard.h), and a reset in the middle of a
  // step is what this is: the Walter is gone, the radio is a scanner, and nothing but the journal
  // says so.
  SessionUnit unit;
  bench::Answer slow;
  slow.done = false;
  slow.lines = 4;
  unit.scripted.answers["dwell"] = slow;
  const upd::Program program = bench::programOf({{"dwell 869.618 300", 300}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unit.run([&unit](const upd::SessionStep&) {
    // The bound on a wake is five minutes; the step was given three hundred seconds of its own.
    if (unit.session().stage() == Stage::kStep &&
        unit.session().status().scanner_ms >= core::cycle::kWakeBoundMs) {
      unit.walter->die();
    }
  });
  REQUIRE(unit.world.dead);
  CHECK(unit.sink.steps.empty());  // the step never handed its lines over
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK(mark.open);
  CHECK(mark.stage == Stage::kStep);

  const int boots = bootsUntilClosed(unit);
  CHECK(boots == 1);
  checkTheRepeaterIsBack(unit);
}
