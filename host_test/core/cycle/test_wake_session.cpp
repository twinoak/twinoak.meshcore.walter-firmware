// Step 3 and an open session of the radio as a scanner (task p16-03, rails 2 and 7).
//
// A session is begun by a person at the console or by task p16-04's job and by nothing else. What
// a **wake** does with one is the other half of the rails, and it is the half that matters on a
// mast: a board that restarted between the two switches has a site with no repeater, and the only
// thing that gets it back is step 3 reading this board's own journal.
//
// So: the journal is left saying a session is open - which is what a reset, a crash, the bound on
// a wake or a power cut leaves - and the wake after it has to flash the golden image back, prove
// it, say closed, and still reach sleep. And a wake that did that runs no other update step (S-7).
#include <doctest.h>

#include <string>
#include <vector>

#include "core/cycle/wake_bench.h"
#include "core/update/scanner_session.h"

namespace cyc = core::cycle;
namespace upd = core::update;

namespace {

// A unit with a chip for the radio to run from, a golden image in the store and on the chip, and
// a scanner image in the scanner slot: what a session needs, and what one leaves behind.
struct SessionWake : bench::WakeBench {
  bench::Candidate golden{"update-golden"};
  bench::Candidate scanner{"store-scanner"};

  // `golden_in_store` false is a unit whose repeater is on the chip and whose store has nothing
  // to come back to: the switch back is refused and the session stays open (rail 5).
  explicit SessionWake(bool golden_in_store = true)
      : WakeBench([](bench::WakeWorld& w) { w.withChip(); }) {
    if (golden_in_store) {
      REQUIRE(world.images.put(bench::Which::kGolden, bench::Slot{0}, golden) ==
              bench::StoreError::kNone);
    }
    REQUIRE(world.images.put(bench::Which::kScanner, bench::Slot{0}, scanner) ==
            bench::StoreError::kNone);
    world.chip->installOnChip(golden);
    world.chip->radioBoots();
  }

  // The chip holding the scanner image and the radio answering as one, which is where a session
  // that was cut off in its steps left the radio.
  void theRadioIsAScanner() {
    world.chip->installOnChip(scanner);
    world.chip->radioBoots();
    REQUIRE(world.rak.scanner);
  }

  // A session left open in the journal at `stage`, as the wake before this one left it. Written
  // by hand because a *reset* is what leaves it: the session object of that life is gone and the
  // entry is all there is (core/update/scanner_session.h).
  void anOpenSessionAt(upd::Stage stage, const char* command = "dwell 869.618 60") {
    upd::SessionPayload p;
    p.stage = stage;
    p.step = stage == upd::Stage::kStep ? 1 : 0;
    p.slot = 0;
    p.at_epoch = 1760000000;
    if (stage == upd::Stage::kStep) {
      std::size_t i = 0;
      for (; command[i] != 0 && i < upd::kCommandMax; ++i) p.command[i] = command[i];
      p.command[i] = 0;
    }
    std::uint8_t bytes[upd::kPayloadMax];
    const std::size_t size = upd::encode(p, bytes);
    REQUIRE(walter->journal.append(upd::kind::kScannerSession, bytes, size) ==
            upd::JournalError::kNone);
    upd::Mark mark;
    REQUIRE(upd::readMark(walter->journal, &mark));
    REQUIRE(mark.open);
  }

  bool sessionIsOpen() {
    upd::Mark mark;
    return upd::readMark(walter->journal, &mark) && mark.open;
  }

  upd::Stage lastStage() {
    upd::Mark mark;
    return upd::readMark(walter->journal, &mark) ? mark.stage : upd::Stage::kIdle;
  }
};

}  // namespace

TEST_CASE("step 3 takes up an open session: the golden image back, proved, and closed") {
  SessionWake unit;
  unit.theRadioIsAScanner();
  unit.anOpenSessionAt(upd::Stage::kStep);
  unit.modemSilent();  // no server, no uplink: the rail says it needs neither

  const cyc::Pass& pass = unit.oneWake();
  const std::string step3 =
      std::string(cyc::describe(pass.step[2].outcome)) + ": " + pass.step[2].why;
  INFO(step3);
  CHECK(pass.ok(cyc::Step::kResume));
  CHECK(pass.session);
  CHECK(std::string(pass.step[2].why).find("session") != std::string::npos);
  // The repeater is back on the chip, answering as the golden image, and the journal says closed.
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK(unit.world.chip->running_version == unit.golden.version);
  CHECK_FALSE(unit.world.rak.scanner);
  CHECK_FALSE(unit.sessionIsOpen());
  CHECK(unit.lastStage() == upd::Stage::kRestored);
  // And the wake still reached sleep, which is the invariant every wake has.
  CHECK(pass.ending == cyc::Ending::kSlept);
  // The bound was raised to a session's and no further: `allow()` is capped at the guard's own
  // ceiling, and a session's bound is what set it.
  CHECK(pass.bound_ms == upd::kSessionBoundMs);
  // No room was borrowed: a session taken up out of the journal has no steps to run.
  CHECK(unit.v().scratch.asks == 0);
  // The record says so, in the one line a week of wakes is read from.
  char line[240];
  (void)cyc::recordLine(pass, line, sizeof line);
  CHECK(std::string(line).find("a session was taken up") != std::string::npos);
}

TEST_CASE("a wake that ended a session runs no other update step (S-7)") {
  SessionWake unit;
  unit.theRadioIsAScanner();
  unit.anOpenSessionAt(upd::Stage::kScanner);
  unit.modemWorks();
  // A console command asked for the radio's update as well: step 9 refuses it, because step 3
  // already spent this wake's one update step on the session.
  unit.boot();
  unit.v().begin();
  unit.cycle().askFor(interfaces::Ask::kUpdateRadio);
  const cyc::Pass& pass = unit.v().run();
  CHECK(pass.session);
  CHECK(pass.outcomeOf(cyc::Step::kUpdate) == cyc::Outcome::kRefused);
  CHECK(std::string(pass.step[8].why).find("one update step a wake") != std::string::npos);
  CHECK_FALSE(unit.sessionIsOpen());
}

TEST_CASE("an ordinary wake takes up nothing and borrows nothing") {
  SessionWake unit;
  unit.modemSilent();
  const cyc::Pass& pass = unit.oneWake();
  CHECK_FALSE(pass.session);
  CHECK(pass.outcomeOf(cyc::Step::kResume) == cyc::Outcome::kNothing);
  CHECK(unit.v().scratch.asks == 0);
  CHECK(pass.bound_ms == cyc::kWakeBoundMs);
  char line[240];
  (void)cyc::recordLine(pass, line, sizeof line);
  CHECK(std::string(line).find("a session") == std::string::npos);
}

TEST_CASE("a scanner nobody opened a session for is left alone, wake after wake") {
  SessionWake unit;
  unit.theRadioIsAScanner();
  unit.modemSilent();
  for (int wake = 0; wake < 4; ++wake) {
    CAPTURE(wake);
    const cyc::Pass& pass = unit.oneWake();
    CHECK_FALSE(pass.session);
    CHECK(pass.radio == core::supervisor::Health::kScanner);
    // §9: a scanner answers no frame, so step 4 is skipped - and the wake still sleeps.
    CHECK(pass.outcomeOf(cyc::Step::kState) == cyc::Outcome::kSkipped);
    CHECK(pass.ending == cyc::Ending::kSlept);
    // Nothing flashed it over: the chip still holds the scanner image.
    CHECK(unit.world.chip->chipHolds(unit.scanner));
    CHECK(unit.world.rak.scanner);
    // And the supervisor says which of the two scanners this is.
    CHECK_FALSE(unit.v().supervisor->status().session_open);
    const std::string watch = unit.v().runLine("rak watch");
    CHECK(watch.find("running the scanner, left alone") != std::string::npos);
  }
}

TEST_CASE("a switch back a wake cannot make leaves the session open, and rak watch says so") {
  // No golden image in the store: the switch back is refused, the session stays open (rail 5),
  // and the wake sleeps all the same.
  SessionWake unit(false);
  unit.theRadioIsAScanner();
  unit.anOpenSessionAt(upd::Stage::kStep);
  unit.modemSilent();
  const cyc::Pass& pass = unit.oneWake();
  CHECK(pass.session);
  CHECK(unit.sessionIsOpen());
  CHECK(unit.lastStage() == upd::Stage::kStuck);
  CHECK(pass.ending == cyc::Ending::kSlept);
  CHECK(unit.world.rak.scanner);  // still a scanner: nothing was flashed over it
  // §9 classified the scanner at step 2 and read the journal at the same moment, so `rak watch`
  // can say which of the two scanners this is.
  CHECK(unit.v().supervisor->health() == core::supervisor::Health::kScanner);
  CHECK(unit.v().supervisor->status().session_open);
  CHECK(unit.v().supervisor->status().session_since_epoch == 1760000000);
  const std::string watch = unit.v().runLine("rak watch");
  CHECK(watch.find("in a session, since epoch 1760000000") != std::string::npos);
  CHECK(watch.find("the next wake puts the golden image back") != std::string::npos);
  // And the wake after it tries again - one journal line a try.
  const cyc::Pass& again = unit.oneWake();
  CHECK(again.session);
  CHECK(unit.sessionIsOpen());
}

TEST_CASE("the wake after one that ended a session has nothing to take up") {
  SessionWake unit;
  unit.theRadioIsAScanner();
  unit.anOpenSessionAt(upd::Stage::kStep);
  unit.modemSilent();
  const cyc::Pass& first = unit.oneWake();
  CHECK(first.session);
  CHECK_FALSE(unit.sessionIsOpen());
  // And a second wake has nothing to take up.
  const cyc::Pass& second = unit.oneWake();
  CHECK_FALSE(second.session);
  CHECK(second.outcomeOf(cyc::Step::kResume) == cyc::Outcome::kNothing);
}
