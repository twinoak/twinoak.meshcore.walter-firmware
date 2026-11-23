// **An image is started either way** (task p18-04): on a unit whose reset line reaches its radio,
// and on one whose does not.
//
// Since task p18-01 every flash ends in a pulse on nRESET, which is the one reset that clears the
// watchdog a scanner leaves behind - and which gets no confirmation from the chip. The unit at
// Bellevue says that assumption does not hold everywhere: on 21 November the supervisor wrote
// *cured by rung 2, a reset pulse* and the radio's own `uptimeS` and `RESETREAS` show no restart
// answering one (tasks/phase-18/FOLLOWUPS.md, *A reset pulse that restarted nothing*). On such a
// unit the flasher as task p18-01 left it leaves the radio halted after every flash: an apply
// rolls back and ends `stuck`, and a session's scanner never speaks.
//
// What this file holds the three answers to:
//
//   1. **what the chip says**: UICR.PSELRESET read at every flash, and no pulse where it says
//      there is no reset pin - the image is started through the debug port instead;
//   2. **what a pulse did**: an empty boot window after a pulse is worth one look at the core,
//      and a core still halted where the flash left it is a pulse that reached nothing;
//   3. **what that costs**: a soft reset does not clear a watchdog a scanner started, so a proof
//      that follows one waits the watchdog out before it begins.
//
// The unit at Bellevue as it was read is `kEnabledNotWired`: UICR says the pin reset is enabled,
// and the pulse reaches nothing.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/update/radio_update.h"
#include "core/update/scanner_session.h"
#include "session_bench.h"
#include "update_bench.h"

using bench::Boot;
using bench::Ending;
using bench::Kind;
using bench::ReadyUnit;
using bench::Reason;
using bench::SessionUnit;
using bench::Stage;
using bench::State;
using bench::Which;
using bench::programOf;
namespace upd = core::update;
namespace fl = core::flasher;

namespace {

using Wiring = sim::Nrf52Chip::ResetPinIs;

std::vector<State> statesOf(const bench::Walter& w) {
  std::vector<State> out;
  for (const upd::StepResult& s : w.transitions) out.push_back(s.after);
  return out;
}

// Every radio-update entry the journal holds, oldest first.
std::vector<upd::Payload> storyOf(const bench::Walter& w) {
  std::vector<upd::Payload> out;
  upd::Cursor cursor;
  upd::Entry entry;
  bool more = w.journal.first(&cursor, &entry);
  while (more) {
    upd::Payload p;
    if (entry.kind == upd::kind::kRadioUpdate && upd::decode(entry, &p)) out.insert(out.begin(), p);
    more = w.journal.previous(&cursor, &entry);
  }
  return out;
}

std::string storyText(const std::vector<upd::Payload>& story) {
  std::string text;
  for (const upd::Payload& p : story) {
    text += std::string(upd::describe(p.state));
    if (p.reason != Reason::kNone) text += std::string(": ") + upd::describe(p.reason);
    text += "\n";
  }
  return text;
}

bool storyHas(const std::vector<upd::Payload>& story, State state, Reason reason) {
  for (const upd::Payload& p : story) {
    if (p.state == state && p.reason == reason) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------- 1. the unit at Bellevue

TEST_CASE("Bellevue as read: UICR says the pin reset is enabled, the line is dead, and the apply still commits") {
  ReadyUnit unit;
  unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
  bench::Walter& w = *unit.walter;

  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));

  // **It commits**, where the firmware of task p18-01 alone leaves it `stuck`: the look found the
  // core halted, the debug port started the image, and the candidate proved itself.
  CHECK(end == State::kDone);
  CHECK(statesOf(w) == std::vector<State>{State::kFlashing, State::kBooting, State::kProving,
                                          State::kCommitting, State::kDone});
  CHECK(unit.goldenVersion() == unit.candidate.version);
  CHECK(unit.world.chipHolds(unit.candidate));
  CHECK(unit.world.store.store.badCount() == 0);

  // The pulse was made and it reached nothing; the chip was reset by the debug port instead.
  CHECK(unit.world.chip.resets().pin == 0);
  CHECK(unit.world.chip.resets().pin_lost == 1);
  CHECK(unit.world.chip.counters().aircr_resets == 1);

  // The journal's lines, in order: the booting entry that says what the look found sits between
  // `booting` and `proving`, and nothing of the run is missing.
  CHECK(story.size() == 7);
  CHECK(story[0].state == State::kCheck);
  CHECK(story[1].state == State::kFlashing);
  CHECK(story[2].state == State::kBooting);
  CHECK(story[2].reason == Reason::kNone);
  CHECK(story[3].state == State::kBooting);
  CHECK(story[3].reason == Reason::kStartedThroughPort);
  CHECK(story[3].detail == static_cast<std::uint32_t>(core::nrf52::CoreState::kHalted));
  CHECK(story[4].state == State::kProving);
  CHECK(story[5].state == State::kCommitting);
  CHECK(story[6].state == State::kDone);

  // What the machine says about it, and what the unit now remembers of its line: UICR read and
  // enabled, and a pulse seen to restart nothing.
  const upd::Status& st = w.update.status();
  CHECK(st.looked);
  CHECK(st.core_at_look == core::nrf52::CoreState::kHalted);
  CHECK(st.started == fl::Started::kDebugPort);
  CHECK_FALSE(st.waited_for_watchdog);  // this flash was not over a scanner
  const core::board::ResetLineKnown& line = w.board.resetLine();
  CHECK(line.uicr_read);
  CHECK(line.pin_enabled);
  CHECK(line.pin == 18);
  CHECK(line.seen == core::board::ResetLineSeen::kDead);
  MESSAGE("the apply ended " << std::string(upd::describe(end))
                             << " with one lost pulse, one look and one reset through the debug port");
}

TEST_CASE("a line that works: nothing is added - no look, no second window, and the pulse is what starts the image") {
  ReadyUnit unit;  // kEnabledAndWired, which every bench is by default
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(story.size() == 6);  // check, flashing, booting, proving, committing, done: 01's run
  const upd::Status& st = w.update.status();
  CHECK_FALSE(st.looked);
  CHECK_FALSE(st.waited_for_watchdog);
  CHECK(st.started == fl::Started::kResetLine);
  CHECK(unit.world.chip.resets().pin == 1);
  CHECK(unit.world.chip.resets().pin_lost == 0);
  CHECK(unit.world.chip.counters().aircr_resets == 0);
  CHECK(w.board.resetLine().seen == core::board::ResetLineSeen::kRestarts);
  CHECK(w.board.resetLine().pin_enabled);
}

TEST_CASE("UICR says there is no reset pin: no pulse is made on any flash, and the endings are the same") {
  ReadyUnit unit;
  unit.world.chip.resetPinIs(Wiring::kNotEnabled);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);
  CHECK(unit.goldenVersion() == unit.candidate.version);
  // Not one pulse: not a lost one either, because none was made. The debug port did it.
  CHECK(unit.world.chip.resets().pin == 0);
  CHECK(unit.world.chip.resets().pin_lost == 0);
  CHECK(unit.world.chip.counters().aircr_resets == 1);
  const upd::Status& st = w.update.status();
  CHECK(st.started == fl::Started::kDebugPort);
  CHECK_FALSE(st.looked);  // a reset the port made needs no look: it answered S_RESET_ST
  CHECK(storyOf(w).size() == 6);
  const core::board::ResetLineKnown& line = w.board.resetLine();
  CHECK(line.uicr_read);
  CHECK_FALSE(line.pin_enabled);
  CHECK(line.seen == core::board::ResetLineSeen::kUntried);  // no pulse was made to see anything of
}

TEST_CASE("the pulse worked and the image is silent: one look, the core running, nothing sent, and the rollback as it was") {
  ReadyUnit unit;
  unit.world.boot = Boot::kSilent;  // the candidate boots and never answers
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  // The golden image is the one that answers: the knob is the bench's, not an image's, so it is
  // put back when the rollback's flash begins.
  const State end = w.run([&](const upd::StepResult& s) {
    if (s.after == State::kRollingBack) unit.world.boot = Boot::kNormal;
  });
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));

  // The ending there has always been: the candidate failed to boot, went on the bad list, and the
  // golden image came back.
  CHECK(end == State::kRestored);
  CHECK(w.update.status().fail_reason == Reason::kNoBoot);
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.world.store.store.badCount() == 1);

  // One look, and it found a core that was running: nothing was sent to it. The journal is where
  // that is read - the status is the golden image's run by the time this ends.
  CHECK(storyHas(story, State::kBooting, Reason::kPulseStarted));
  CHECK_FALSE(storyHas(story, State::kBooting, Reason::kStartedThroughPort));
  CHECK(unit.world.chip.counters().aircr_resets == 0);  // nothing was sent to a running core
  CHECK(unit.world.chip.resets().pin == 2);             // the two flashes' two pulses
  // And the one thing the look did learn: this unit's reset line works.
  CHECK(w.board.resetLine().seen == core::board::ResetLineSeen::kRestarts);
}

TEST_CASE("one look a flash and never two: a second empty window is the ending it always was") {
  ReadyUnit unit;
  unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
  unit.world.boot = Boot::kSilent;  // and the image says nothing when the debug port starts it
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(end == State::kStuck);  // the golden image is as silent as the candidate on this bench
  // Two flashes, two looks - one each - and never two for one flash: each flash's own window ran
  // out twice and only the first of the two was looked at.
  unsigned looks = 0;
  for (const upd::Payload& p : story) {
    if (p.reason == Reason::kStartedThroughPort || p.reason == Reason::kPulseStarted ||
        p.reason == Reason::kLookFailed) {
      ++looks;
    }
  }
  CHECK(looks == 2);
  CHECK(unit.world.chip.counters().aircr_resets == 2);  // one start through the port a flash
  CHECK(unit.world.chip.resets().pin_lost == 2);        // and one lost pulse a flash
}

TEST_CASE("the look meets a closed port: the ending is the one there is, and the journal says the question could not be put") {
  ReadyUnit unit;
  unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  // The flash goes through; the port is shut behind it, as a build code Fxx module whose
  // firmware does not write APPROTECT.DISABLE is shut by any reset (task p18-01's deviation 1.3).
  bool shut = false;
  const State end = w.run([&](const upd::StepResult& s) {
    if (!shut && s.after == State::kBooting) {
      shut = true;
      unit.world.chip.approtect = true;
    }
  });
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(shut);
  CHECK(storyHas(story, State::kBooting, Reason::kLookFailed));
  // Nothing was started, and the run ended where a boot window that stays empty always ended.
  CHECK(w.update.status().fail_reason == Reason::kNoBoot);
  CHECK(unit.world.chip.counters().aircr_resets == 0);
  CHECK(end == State::kStuck);  // the golden image cannot be flashed into a closed chip either
}

// ---------------------------------------------------------------- 2. the scanner's watchdog

TEST_CASE("Bellevue as read, a session: the scanner and the golden image both started through the debug port, and it ends restored") {
  SessionUnit unit;
  unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
  const std::uint64_t began_ms = unit.world.clock.nowMs();
  const upd::Program program = programOf({{"sweep 863 870 25", 30}, {"hist 869.4 869.8 25", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));

  // **The outage, as a person on the mesh sees it**: the repeater is off the air from the moment
  // the scanner image is flashed until the golden image is proved, and that is one period, not
  // two - the session of 21 November ended `stuck` and left a second one that lasted until the
  // unit's next timer wake mended it. Counted here as the periods in which no repeater is
  // serving, and beside it the times the radio itself went dark (a flash's own two, and the
  // watchdog's reset of the golden image, which is this unit's price for a dead reset line).
  unsigned outages = 0;
  unsigned dark_periods = 0;
  bool serving = true;
  bool dark = false;
  const Stage end = unit.run([&](const upd::SessionStep&) {
    const bool repeater = unit.world.radio_runs && !unit.world.radio.scanner;
    if (!repeater && serving) ++outages;
    serving = repeater;
    if (!unit.world.radio_runs && !dark) ++dark_periods;
    dark = !unit.world.radio_runs;
  });
  INFO(unit.stageText());
  const std::vector<upd::Payload> story = storyOf(*unit.walter);
  INFO(storyText(story));

  CHECK(end == Stage::kRestored);
  CHECK(unit.session().status().ending == Ending::kRestored);
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  CHECK(unit.sink.steps.size() == 2);

  // Not one pulse reached the chip; both images were started through the debug port.
  CHECK(unit.world.chip.resets().pin == 0);
  CHECK(unit.world.chip.resets().pin_lost == 2);
  CHECK(unit.world.chip.counters().aircr_resets == 2);

  // **And the watchdog the scanner left running.** The golden image was started by a soft reset,
  // which does not clear it, so it reset the repeater once - and the proof began at the boot that
  // followed, not before it. The proof itself is untouched: the full period, no restart in it.
  CHECK(unit.world.chip.resets().watchdog == 1);
  const bool started_through_the_port =
      storyHas(story, State::kRollbackBooting, Reason::kStartedThroughPort) ||
      storyHas(story, State::kBooting, Reason::kStartedThroughPort);
  CHECK(started_through_the_port);
  CHECK(storyHas(story, State::kRollbackBooting, Reason::kAfterTheWatchdog));
  CHECK(unit.walter->update.status().waited_for_watchdog);
  CHECK(unit.walter->update.status().reason != Reason::kRestarted);
  CHECK(unit.walter->update.status().prove_elapsed_ms >= upd::kProvingPeriodMs);
  CHECK_FALSE(unit.world.chip.watchdogRunning());

  // **One outage**: the repeater left the air once, when the scanner was flashed, and was back
  // when the session ended. The radio itself went dark three times inside it - the two flashes
  // and the watchdog's own reset of the golden image - where a unit whose reset line works goes
  // dark twice; that third one is a second or two, inside the one outage, and it is what this
  // unit pays for a line that reaches nothing.
  // Twice, and the second is the watchdog's own reset of the golden image, which is a second or
  // two in the middle of the wait and before the proof: nothing can stop a watchdog a scanner
  // started (PS 6.36.3, "the watchdog's configuration registers ... will be blocked"), and the
  // one reset that clears it is the one this unit's line cannot make. **It is still one outage of
  // the session**: it ends `restored`, with the repeater serving, and leaves no second outage for
  // a later wake to mend - which is what 21 November's `stuck` left.
  CHECK(outages == 2);
  CHECK(dark_periods == 3);
  CHECK(unit.world.radio_runs);  // and it is serving when the session ends
  const std::uint32_t seconds =
      static_cast<std::uint32_t>((unit.world.clock.nowMs() - began_ms) / 1000u);
  CHECK(seconds * 1000u < upd::kSessionBoundMs);
  MESSAGE("the session took " << seconds << " s of its " << upd::kSessionBoundMs / 1000
                              << " s bound and ended restored: the repeater left the air " << outages
                              << " times (the second the watchdog's own reset, inside the wait) and the radio went dark "
                              << dark_periods << " times");
}

TEST_CASE("a session on a line that works: 01's sequence to the frame, with no look and no wait") {
  SessionUnit unit;
  const std::uint64_t began_ms = unit.world.clock.nowMs();
  const upd::Program program = programOf({{"sweep 863 870 25", 30}, {"hist 869.4 869.8 25", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unsigned dark_periods = 0;
  bool dark = false;
  const Stage end = unit.run([&](const upd::SessionStep&) {
    if (!unit.world.radio_runs && !dark) ++dark_periods;
    dark = !unit.world.radio_runs;
  });
  INFO(unit.stageText());
  CHECK(end == Stage::kRestored);
  CHECK(dark_periods == 2);  // one a flash, and nothing else
  CHECK(unit.world.chip.resets().pin == 2);
  CHECK(unit.world.chip.resets().pin_lost == 0);
  CHECK(unit.world.chip.counters().aircr_resets == 0);
  CHECK(unit.world.chip.resets().watchdog == 0);
  CHECK_FALSE(unit.walter->update.status().looked);
  CHECK_FALSE(unit.walter->update.status().waited_for_watchdog);
  const std::uint32_t seconds =
      static_cast<std::uint32_t>((unit.world.clock.nowMs() - began_ms) / 1000u);
  MESSAGE("the same session on a unit whose line works took " << seconds << " s");
}

TEST_CASE("the watchdog that resets nothing: the proof begins when its longest time has passed") {
  SessionUnit unit;
  unit.world.chip.resetPinIs(Wiring::kNotEnabled);  // the debug port starts every image
  // The scanner of this session starts no watchdog at all - a build whose Wdt.h was left out - so
  // the golden image inherits nothing, and the wait ends by the clock instead of by a reset. The
  // firmware cannot know which of the two it is: it waits either way, and says which happened.
  unit.world.scanner_starts_watchdog = false;
  const upd::Program program = programOf({{"sweep 863 870 25", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  const Stage end = unit.run();
  INFO(unit.stageText());
  const std::vector<upd::Payload> story = storyOf(*unit.walter);
  INFO(storyText(story));
  CHECK(end == Stage::kRestored);
  CHECK(unit.world.chip.resets().watchdog == 0);
  CHECK(storyHas(story, State::kRollbackBooting, Reason::kWatchdogTimePassed));
  CHECK(unit.walter->update.status().waited_for_watchdog);
  CHECK(unit.walter->update.status().prove_elapsed_ms >= upd::kProvingPeriodMs);
}

// ---------------------------------------------------------------- 3. the power, pulled

TEST_CASE("the power pulled between the pulse and the look, and between the look and the reset: the next wake ends it") {
  // Three interruption points of the one path this task added, each its own unit: the Walter dies
  // where the task names it, and what matters is that the next boot takes the run up again from
  // the journal and the repeater ends up running a proved image.
  struct Point {
    const char* name;
    int die_at;  // 0: after the flash's pulse, before the look; 1: after the look's reset
  };
  const Point points[] = {{"between the pulse and the look", 0}, {"between the look and the proof", 1}};
  for (const Point& point : points) {
    CAPTURE(point.name);
    ReadyUnit unit;
    unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
    REQUIRE(unit.walter->update.start(unit.request()));
    bool looked_before_dying = false;
    unit.walter->run([&](const upd::StepResult& s) {
      if (unit.world.dead || s.after != State::kBooting) return;
      // 0: the flash's own transition into `booting`, which is the moment after the pulse and
      // before the window has run out. 1: the look has been made and the debug port has started
      // the image, and the window it opened is running.
      if (point.die_at == 0 && s.changed) unit.walter->die();
      if (point.die_at == 1 && unit.walter->update.status().looked) {
        looked_before_dying = true;
        unit.walter->die();
      }
    });
    CHECK(unit.world.dead);
    CHECK(looked_before_dying == (point.die_at == 1));

    // The next boot: the journal is read and the run taken up again.
    unit.reboot();
    INFO(storyText(storyOf(*unit.walter)));
    REQUIRE(unit.walter->update.resume());
    const State end = unit.walter->run();
    INFO(unit.walter->transitionText());
    INFO(storyText(storyOf(*unit.walter)));
    const bool ended_well = end == State::kDone || end == State::kRestored;
    CHECK(upd::isTerminal(end));
    CHECK(ended_well);
    // Whatever it ended as, the radio runs a proved image and the store is consistent.
    CHECK(unit.world.radio_runs);
    CHECK(unit.world.running_version == unit.goldenVersion());
  }
}

TEST_CASE("the power pulled in the wait for the watchdog: the next wake ends it by the rails there are") {
  SessionUnit unit;
  unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
  const upd::Program program = programOf({{"sweep 863 870 25", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  bool died = false;
  unit.run([&](const upd::SessionStep&) {
    if (!died && unit.walter->update.status().waited_for_watchdog &&
        unit.walter->update.state() == State::kRollbackBooting) {
      died = true;
      unit.walter->die();
    }
  });
  CHECK(died);
  CHECK(unit.world.dead);

  // The next boot takes the session up again, in main's own order - the radio's own update first
  // and then the session, which is what step 3 of a wake does.
  unit.reboot();
  REQUIRE(unit.walter->update.resume());
  REQUIRE(unit.session().resume());
  const Stage end = unit.run();
  INFO(unit.stageText());
  INFO(storyText(storyOf(*unit.walter)));
  INFO(std::string(upd::name(end)));
  CHECK(end == Stage::kRestored);
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
}

// ---------------------------------------------------------------- 4. what a person is told

TEST_CASE("the console says what UICR holds and what a pulse has been seen to do") {
  ReadyUnit unit;
  unit.world.chip.resetPinIs(Wiring::kEnabledNotWired);
  bench::Walter& w = *unit.walter;

  // `rak swd id` reads the two registers live.
  const std::string id = w.runLine("rak swd id");
  INFO(id);
  CHECK(id.find("UICR.PSELRESET: 0x00000012, 0x00000012 - the pin reset is enabled") != std::string::npos);
  CHECK(id.find("nRESET is exposed on P0.18; a flash starts the image by pulsing it") != std::string::npos);

  // A flash of the radio says which of the two started the image.
  REQUIRE(w.update.start(unit.request()));
  REQUIRE(w.run() == State::kDone);
  const std::string after = w.runLine("rak swd id");
  INFO(after);
  CHECK(after.find("UICR.PSELRESET") != std::string::npos);

  // And a module with no reset pin says so, with what a flash does about it.
  ReadyUnit none;
  none.world.chip.resetPinIs(Wiring::kNotEnabled);
  const std::string none_id = none.walter->runLine("rak swd id");
  INFO(none_id);
  CHECK(none_id.find("NO pin reset: nRESET is on no pin") != std::string::npos);
  CHECK(none_id.find("a flash starts the image through the debug port") != std::string::npos);
}

TEST_CASE("the flash's own lines: which of the two started the image, and what UICR said") {
  ReadyUnit unit;
  unit.world.chip.resetPinIs(Wiring::kNotEnabled);
  const std::string out = unit.walter->runLine("rak flash-test confirm");
  INFO(out);
  CHECK(out.find("UICR.PSELRESET: the pin reset is NOT enabled on this module") != std::string::npos);
  CHECK(out.find("the radio was started by a reset through the debug port") != std::string::npos);

  ReadyUnit wired;
  const std::string wired_out = wired.walter->runLine("rak flash-test confirm");
  INFO(wired_out);
  CHECK(wired_out.find("UICR.PSELRESET: nRESET is on P0.18") != std::string::npos);
  CHECK(wired_out.find("the radio was started by a pulse on the reset line") != std::string::npos);
}
