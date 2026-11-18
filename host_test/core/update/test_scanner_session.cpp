// A session: the scanner image, a program of steps, and the golden image back (task p16-03).
// `components/core/include/core/update/scanner_session.h` is the machine and `docs/update.md`'s
// *A session* is the rails. Against the scripted flasher, the scripted scanner of task p16-01 and
// the simulated flash.
//
// What this file is about is **the middle going wrong**: a scanner that never comes up, a caller
// that sits on a step's lines, a step that fills the room, an `ack` that says not ok, a switch
// back the battery refuses. The power going at every write is
// test_scanner_session_power_loss.cpp.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/console/heap_report.h"
#include "core/cycle/wake_guard.h"
#include "core/update/scanner_session.h"
#include "session_bench.h"

using bench::Ending;
using bench::Refusal;
using bench::SessionUnit;
using bench::Stage;
using bench::StepEnd;
using bench::programOf;
namespace upd = core::update;

namespace {

// Was `stop` said to the scanner?
bool stopWasSaid(const SessionUnit& unit) { return unit.scripted.wasSaid("stop"); }

// The journalled stages as one string, for a message.
std::string stagesOf(const std::vector<Stage>& stages) {
  std::string text;
  for (Stage stage : stages) text += std::string(upd::name(stage)) + " ";
  return text;
}

// Where in what the scanner was told the line beginning `prefix` is. **The link client's own §9
// probe says `info` before any of this** - that is how it decided the radio was a scanner at all
// (task p1-02) - so the session's greeting is found rather than assumed to be first.
std::size_t saidAt(const SessionUnit& unit, const std::string& prefix) {
  for (std::size_t i = 0; i < unit.scripted.said.size(); ++i) {
    if (unit.scripted.said[i].rfind(prefix, 0) == 0) return i;
  }
  return unit.scripted.said.size();
}

// The heap behind its seam, scripted read by read: a session reads it three times - before the
// room is taken, just after, and when the steps are over - and the `room` line has to carry all
// three.
struct ScriptedHeap final : core::console::HeapReport {
  std::vector<Figures> script;
  std::size_t reads = 0;

  Figures read() override {
    REQUIRE_FALSE(script.empty());
    const Figures f = script[reads < script.size() ? reads : script.size() - 1];
    ++reads;
    return f;
  }
};

}  // namespace

// ----------------------------------------------------------------- the whole thing, twice over

TEST_CASE("a session of two steps: open, the scanner, the steps, the golden image back, closed") {
  SessionUnit unit;
  // Step 1 is answered with a `done`; step 2's `hist` is not, so its seconds are what ends it.
  bench::Answer hist;
  hist.done = false;
  hist.lines = 2;
  unit.scripted.answers["hist"] = hist;
  const upd::Program program = programOf({{"sweep 863 870 25", 30}, {"hist 869.4 869.8 25", 5}});

  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  // Open **before the first flash**: the journal says so and the chip still holds the repeater.
  CHECK(unit.session().stage() == Stage::kCheck);
  CHECK(unit.journalledStages() == std::vector<Stage>{Stage::kCheck});
  CHECK(unit.world.chipHolds(unit.golden));

  const Stage end = unit.run();
  INFO(unit.stageText());
  CHECK(end == Stage::kRestored);
  CHECK(unit.session().status().ending == Ending::kRestored);

  // The journal, in order, read back out of flash.
  const std::vector<Stage> journalled = unit.journalledStages();
  INFO(stagesOf(journalled));
  CHECK(journalled == std::vector<Stage>{Stage::kCheck, Stage::kFlashingScanner, Stage::kScanner,
                                          Stage::kStep, Stage::kStep, Stage::kRoom,
                                          Stage::kFlashingGolden, Stage::kBooting, Stage::kProving,
                                          Stage::kRestored});
  // And `restored` is the one stage that means closed.
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK(mark.found);
  CHECK_FALSE(mark.open);
  CHECK(mark.stage == Stage::kRestored);

  // The radio answers HELLO as the golden image again, and the store did not move.
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  CHECK(unit.walter->client.hello().ok());
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.read(bench::Which::kScanner).present);
  CHECK(unit.read(bench::Which::kCandidate).present);
  CHECK(unit.world.store.store.badCount() == 0);

  // The two steps ended as scripted, and `stop` was said for the one the clock ended.
  REQUIRE(unit.sink.steps.size() == 2);
  CHECK(unit.sink.steps[0].index == 1);
  CHECK(unit.sink.steps[0].end == StepEnd::kDone);
  CHECK(unit.sink.steps[1].index == 2);
  CHECK(unit.sink.steps[1].end == StepEnd::kClock);
  CHECK(stopWasSaid(unit));
  // The commands, in the program's order, and both in the journal with their words.
  const std::vector<std::string> rest = unit.scripted.commandsAfterGreeting();
  REQUIRE(rest.size() >= 3);
  CHECK(rest[0] == "sweep 863 870 25");
  CHECK(rest[1] == "hist 869.4 869.8 25");
  CHECK(rest[2] == "stop");
  upd::SessionPayload step_entry;
  REQUIRE(unit.entryOf(Stage::kStep, &step_entry));
  CHECK(step_entry.step == 2);
  CHECK(std::string(step_entry.command) == "hist 869.4 869.8 25");

  // The room was given back, and nothing of it is still out.
  CHECK(unit.walter->scratch.out == 0);
  CHECK(unit.walter->scratch.asks == 1);
  CHECK(unit.walter->scratch.gives == 1);
  CHECK(unit.walter->scratch.last_asked == upd::kStepBytes);
  // And the pins were never driven in a way a rule forbids: the Walter's own destructor checks it.
  CHECK(unit.world.chip.counters().tripwire == 0);
}

TEST_CASE("the greeting is said before the first command, and its info line is that step's") {
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  REQUIRE(unit.run() == Stage::kRestored);

  // `time <epoch>`, `auto 0`, `info`, and then the step's command - in that order, and all four
  // before anything else the session says.
  const std::size_t greeting = saidAt(unit, "time ");
  REQUIRE(unit.scripted.said.size() >= greeting + 4);
  CHECK(unit.scripted.said[greeting] == "time 1760000000");
  CHECK(unit.scripted.said[greeting + 1] == "auto 0");
  CHECK(unit.scripted.said[greeting + 2] == "info");
  CHECK(unit.scripted.said[greeting + 3] == "sweep");

  // The three acks and the info line are among the lines the first step handed over.
  REQUIRE(unit.sink.steps.size() == 1);
  CHECK(unit.sink.steps[0].first);
  CHECK(unit.sink.has(1, "\"cmd\":\"time\""));
  CHECK(unit.sink.has(1, "\"cmd\":\"auto\""));
  CHECK(unit.sink.has(1, "\"cmd\":\"info\""));
  CHECK(unit.sink.has(1, "\"t\":\"info\""));
  CHECK(unit.sink.has(1, "\"t\":\"sweep\""));
  CHECK(unit.sink.has(1, "\"t\":\"done\""));
}

TEST_CASE("what the scanner said between steps is thrown away") {
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep", 10}, {"cad", 10}});
  // A caller that sits on step 1's lines for a second: the scanner's heartbeat lands in that
  // second and must not be counted as step 2's.
  unit.sink.sit_for_ms = 1000;
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unit.run([&unit](const upd::SessionStep& s) {
    if (s.changed && s.after == Stage::kStep && unit.session().status().step == 1) {
      unit.world.radio.sendLine(bench::scannerLine("stat", 99, 200), 10);
    }
  });
  REQUIRE(unit.sink.steps.size() == 2);
  // Step 2's lines are its own: the `cad` ack, its line and its `done`, and no `stat`.
  CHECK(unit.sink.has(2, "\"t\":\"cad\""));
  CHECK_FALSE(unit.sink.has(2, "\"t\":\"stat\""));
  CHECK_FALSE(unit.sink.has(2, "\"cmd\":\"sweep\""));
}

// --------------------------------------------------------------- the middle going wrong

TEST_CASE("the scanner image flashed and no scanner inside its window: no step, the repeater back") {
  SessionUnit unit;
  unit.world.boot = bench::Boot::kSilent;  // a scanner build whose console never comes up
  const upd::Program program = programOf({{"sweep", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  const Stage end = unit.run([&unit](const upd::SessionStep& s) {
    // Once the golden image is going back, let it come up.
    if (s.changed && s.after == Stage::kFlashingGolden) unit.world.boot = bench::Boot::kNormal;
  });
  INFO(unit.stageText());
  CHECK(end == Stage::kRestored);
  CHECK(unit.sink.steps.empty());
  CHECK(unit.scripted.said.empty());  // nothing was said to a scanner that was not there
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
  // The update machine's own rule is unchanged: a scanner image that did not show itself goes on
  // the bad list, and this session did not change that.
  CHECK(unit.world.store.store.isBad(unit.read(bench::Which::kScanner).sha256));
  CHECK(unit.walter->scratch.out == 0);
}

TEST_CASE("a caller that sits on a step's lines past the ceiling: no further step, the switch back") {
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep", 10}, {"cad", 10}, {"dwell 869.618 10", 10}});
  // Each step's lines are held for eleven minutes: the ceiling is twenty, so step 1 and step 2
  // run and step 3 is never begun.
  unit.sink.sit_for_ms = 11u * 60 * 1000;
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  const Stage end = unit.run();
  INFO(unit.stageText());
  CHECK(end == Stage::kRestored);
  REQUIRE(unit.sink.steps.size() == 2);
  CHECK(unit.sink.steps[0].command == "sweep");
  CHECK(unit.sink.steps[1].command == "cad");
  CHECK_FALSE(unit.scripted.wasSaid("dwell 869.618 10"));
  CHECK(unit.session().status().scanner_ms >= upd::kSessionCeilingMs);
  CHECK(unit.world.running_version == unit.golden.version);
}

TEST_CASE("a step scripted to send 20 kB ends full, stop is said, and the lines are whole") {
  SessionUnit unit;
  bench::Answer hist;
  hist.lines = 20;
  hist.line_bytes = 1000;
  hist.done = false;
  unit.scripted.answers["hist"] = hist;
  const upd::Program program = programOf({{"hist 863 870 25", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  REQUIRE(unit.run() == Stage::kRestored);

  REQUIRE(unit.sink.steps.size() == 1);
  const bench::StepTaken& step = unit.sink.steps[0];
  CHECK(step.end == StepEnd::kFull);
  CHECK(stopWasSaid(unit));
  // Under the room, and every line whole (the sink checks `{`…`}` on each as it takes it). The
  // short ones are the greeting's own acks, which are this step's too.
  CHECK(step.used <= upd::kStepBytes);
  CHECK(step.used > upd::kStepBytes - 1100);  // a 1 000-byte line did not fit in what was left
  std::size_t big = 0;
  for (const std::string& line : step.lines) {
    if (line.size() >= 900) ++big;
  }
  CHECK(big >= 5);
  CHECK(big < 20);  // the twenty the scanner sent did not all fit, which is what `full` means
}

TEST_CASE("an ack that is not ok ends the step refused and the next step runs; no ack is silent") {
  SUBCASE("not ok") {
    SessionUnit unit;
    bench::Answer bad;
    bad.ok = false;
    unit.scripted.answers["scope"] = bad;
    const upd::Program program = programOf({{"scope 863 870", 10}, {"sweep", 10}});
    REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
    REQUIRE(unit.run() == Stage::kRestored);
    REQUIRE(unit.sink.steps.size() == 2);
    CHECK(unit.sink.steps[0].end == StepEnd::kRefused);
    CHECK(unit.sink.steps[1].end == StepEnd::kDone);  // the next step still runs
  }
  SUBCASE("no ack at all") {
    SessionUnit unit;
    bench::Answer mute;
    mute.ack = false;
    unit.scripted.answers["scope"] = mute;
    const upd::Program program = programOf({{"scope 863 870", 10}, {"sweep", 10}});
    REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
    REQUIRE(unit.run() == Stage::kRestored);
    REQUIRE(unit.sink.steps.size() == 2);
    CHECK(unit.sink.steps[0].end == StepEnd::kSilent);
    CHECK(unit.sink.steps[1].end == StepEnd::kDone);
  }
}

// --------------------------------------------------------------- the room, measured

TEST_CASE("the room is measured at every session: one line, five figures, 72 bytes or less") {
  SessionUnit unit;
  // The three reads, scripted: 27 472 free with a 12 288-byte block before the room is taken,
  // a 4 096-byte block after it, and 17 108 as the least free since boot when the steps are over.
  // Five figures a test can tell apart, and none of them the fake's default.
  ScriptedHeap heap;
  heap.script.push_back(core::console::HeapReport::Figures{27472, 12288, 0, 16384, 7560});
  heap.script.push_back(core::console::HeapReport::Figures{19280, 4096, 0, 16384, 7560});
  heap.script.push_back(core::console::HeapReport::Figures{19280, 4096, 17108, 16384, 7560});
  upd::ScannerSession session(unit.walter->update, unit.walter->client, unit.world.store.store,
                             unit.walter->journal, unit.world.clock, unit.world.store.wall,
                             unit.walter->battery, unit.walter->scratch, &heap);
  const upd::Program program = programOf({{"sweep", 10}});
  REQUIRE(session.start(program, bench::Slot{0}, &unit.sink));
  int steps = 0;
  while (session.active() && steps++ < 200000) {
    const upd::SessionStep s = session.step();
    if (s.stalled) break;
    if (s.waited) unit.world.clock.delayMs(upd::kListenIdleMs);
  }
  REQUIRE(session.stage() == Stage::kRestored);
  CHECK(heap.reads == 3);

  upd::SessionPayload room;
  REQUIRE(unit.entryOf(Stage::kRoom, &room));
  CHECK(room.room.known);
  CHECK(room.room.bytes == upd::kStepBytes);
  CHECK(room.room.free_bytes == 27472);
  CHECK(room.room.block == 12288);
  CHECK(room.room.left_block == 4096);
  CHECK(room.room.least_free == 17108);
  char line[upd::kRoomLineMax + 1];
  const std::size_t n = upd::roomLine(room.room, line, sizeof line);
  CHECK(n <= upd::kRoomLineMax);
  CHECK(std::string(line) == "8192 of 27472, block 12288; left block 4096; least free 17108");

  // One `room` line and no more.
  const std::vector<Stage> journalled = unit.journalledStages();
  int rooms = 0;
  for (Stage stage : journalled) {
    if (stage == Stage::kRoom) ++rooms;
  }
  CHECK(rooms == 1);
}

TEST_CASE("a session with no heap seam says so in the room line") {
  SessionUnit unit;
  // The same objects, with no heap report: what every host test but one is, and what a build
  // whose platform hands over none is.
  upd::ScannerSession blind(unit.walter->update, unit.walter->client, unit.world.store.store,
                            unit.walter->journal, unit.world.clock, unit.world.store.wall,
                            unit.walter->battery, unit.walter->scratch, nullptr);
  const upd::Program program = programOf({{"sweep", 10}});
  REQUIRE(blind.start(program, bench::Slot{0}, &unit.sink));
  int steps = 0;
  while (blind.active() && steps++ < 200000) {
    const upd::SessionStep s = blind.step();
    if (s.stalled) break;
    if (s.waited) unit.world.clock.delayMs(upd::kListenIdleMs);
  }
  REQUIRE(blind.stage() == Stage::kRestored);
  upd::SessionPayload room;
  REQUIRE(unit.entryOf(Stage::kRoom, &room));
  CHECK_FALSE(room.room.known);
  char line[upd::kRoomLineMax + 1];
  const std::size_t n = upd::roomLine(room.room, line, sizeof line);
  CHECK(n <= upd::kRoomLineMax);
  CHECK(std::string(line) == "8192 taken; this build reports no heap figures");
}

// --------------------------------------------------------------- not begun unless it can be ended

namespace {

// Every refusal: the program, what to break, and the word. The radio is untouched in all of them.
struct Refused {
  const char* what;
  Refusal word;
};

}  // namespace

TEST_CASE("each refusal with its word, and the radio untouched") {
  const upd::Program good = programOf({{"sweep", 30}});

  SUBCASE("no scanner image") {
    SessionUnit unit(false);
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kNoScanner);
  }
  SUBCASE("a scanner image of another band") {
    SessionUnit unit(false);
    const bench::Candidate other("store-scanner-433");
    REQUIRE(unit.world.store.put(bench::Which::kScanner, bench::Slot{0}, other) ==
            bench::StoreError::kNone);
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kNoScanner);
  }
  SUBCASE("no golden image") {
    // The repeater is running on the chip and answers; the store has nothing to come back to.
    SessionUnit unit(true, false);
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kNoGolden);
  }
  SUBCASE("the battery below its figure") {
    SessionUnit unit;
    unit.walter->battery.millivolts = upd::kBatteryMinMv - 1;
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kBattery);
  }
  SUBCASE("the battery unreadable") {
    SessionUnit unit;
    unit.walter->battery.known = false;
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kBattery);
  }
  SUBCASE("the clock not set") {
    SessionUnit unit;
    unit.world.store.wall.forget();
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kClock);
  }
  SUBCASE("the radio silent") {
    SessionUnit unit;
    unit.world.radio.deaf = true;
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kLink);
  }
  SUBCASE("a radio update in progress") {
    SessionUnit unit;
    REQUIRE(unit.walter->update.start(unit.request(bench::Kind::kUpdate)));
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kInProgress);
  }
  SUBCASE("this firmware on trial") {
    SessionUnit unit;
    const std::uint8_t trial[1] = {upd::self_update::kOnTrial};
    REQUIRE(unit.walter->journal.append(upd::kind::kSelfUpdate, trial, sizeof trial) ==
            upd::JournalError::kNone);
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kInProgress);
  }
  SUBCASE("no room") {
    SessionUnit unit;
    unit.walter->scratch.onlyHas(upd::kStepBytes - 1);
    CHECK_FALSE(unit.session().start(good, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kNoMemory);
    CHECK(unit.walter->scratch.out == 0);
  }
  SUBCASE("seven steps") {
    SessionUnit unit;
    const upd::Program seven = programOf({{"sweep", 5}, {"sweep", 5}, {"sweep", 5}, {"sweep", 5},
                                          {"sweep", 5}, {"sweep", 5}, {"sweep", 5}});
    CHECK(upd::judge(seven) == Refusal::kProgram);
    CHECK_FALSE(unit.session().start(seven, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
  SUBCASE("a command of 41 characters") {
    SessionUnit unit;
    // The field is 41 bytes: a command of 41 characters fills it with no room for a terminator,
    // which is exactly how a caller hands one over, and the judge reads the field's own bounds
    // rather than running off the end of it.
    upd::Program long_one = programOf({{"sweep", 30}});
    for (std::size_t i = 0; i < 41; ++i) long_one.step[0].command[i] = i < 5 ? "sweep"[i] : '1';
    CHECK(upd::whyProgram(long_one) != nullptr);
    CHECK_FALSE(unit.session().start(long_one, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
  SUBCASE("a command of 40 characters is allowed") {
    SessionUnit unit;
    upd::Program forty = programOf({{"sweep", 30}});
    for (std::size_t i = 0; i < 40; ++i) forty.step[0].command[i] = i < 6 ? "sweep "[i] : '1';
    forty.step[0].command[40] = 0;
    CHECK(upd::whyProgram(forty) == nullptr);
  }
  SUBCASE("a command beginning reboot") {
    SessionUnit unit;
    const upd::Program reboot = programOf({{"reboot", 30}});
    CHECK_FALSE(unit.session().start(reboot, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
  SUBCASE("a command with a semicolon") {
    SessionUnit unit;
    const upd::Program semicolon = programOf({{"sweep;reboot", 30}});
    CHECK_FALSE(unit.session().start(semicolon, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
  SUBCASE("4 seconds") {
    SessionUnit unit;
    const upd::Program four = programOf({{"sweep", 4}});
    CHECK_FALSE(unit.session().start(four, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
  SUBCASE("301 seconds") {
    SessionUnit unit;
    const upd::Program long_step = programOf({{"sweep", 301}});
    CHECK_FALSE(unit.session().start(long_step, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
  SUBCASE("601 seconds in all") {
    SessionUnit unit;
    const upd::Program over = programOf({{"sweep", 300}, {"hist", 300}, {"cad", 5}});
    CHECK_FALSE(unit.session().start(over, bench::Slot{0}, &unit.sink));
    CHECK(unit.session().refusal() == Refusal::kProgram);
  }
}

TEST_CASE("a refusal journals nothing, flashes nothing and says nothing to the radio") {
  SessionUnit unit;
  const std::uint32_t entries = unit.walter->journal.validEntries();
  const unsigned writes = unit.world.chip.counters().writes;
  unit.walter->battery.millivolts = 3000;
  const upd::Program program = programOf({{"sweep", 30}});
  CHECK_FALSE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  CHECK(unit.session().refusal() == Refusal::kBattery);
  CHECK(unit.session().status().ending == Ending::kRefused);
  CHECK(unit.walter->journal.validEntries() == entries);
  CHECK(unit.world.chip.counters().writes == writes);
  CHECK(unit.scripted.said.empty());
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.walter->scratch.out == 0);
  upd::Mark mark;
  CHECK_FALSE(upd::readMark(unit.walter->journal, &mark));
}

TEST_CASE("every word of a refusal is its own, and every program rule has a phrase") {
  // The nine words, each different, each a word a job's result may carry.
  const Refusal all[] = {Refusal::kBattery, Refusal::kLink,     Refusal::kClock,
                         Refusal::kInProgress, Refusal::kNoGolden, Refusal::kNoScanner,
                         Refusal::kProgram, Refusal::kNoMemory, Refusal::kJournal};
  for (Refusal a : all) {
    CHECK(std::string(upd::word(a)) != "?");
    CHECK(std::string(upd::describe(a)) != "?");
    for (Refusal b : all) {
      if (a == b) continue;
      CHECK(std::string(upd::word(a)) != std::string(upd::word(b)));
    }
  }
  CHECK(std::string(upd::word(Refusal::kNoScanner)) == "noScanner");
  CHECK(std::string(upd::word(Refusal::kInProgress)) == "inProgress");
  // And a good program has no phrase at all.
  CHECK(upd::whyProgram(programOf({{"sweep 863 870 25", 30}})) == nullptr);
  CHECK(upd::whyProgram(programOf({{"SWEEP", 30}})) != nullptr);   // upper case is not a command
  CHECK(upd::whyProgram(programOf({{"sweep ", 30}})) != nullptr);  // nor one that ends in a space
  CHECK(upd::whyProgram(programOf({{"stop", 30}})) != nullptr);    // the session says `stop` itself
  for (std::size_t i = 0; i < upd::kVerbCount; ++i) {
    upd::Program one = programOf({{"sweep", 30}});
    std::size_t k = 0;
    for (; upd::kVerbs[i][k] != 0; ++k) one.step[0].command[k] = upd::kVerbs[i][k];
    one.step[0].command[k] = 0;
    CHECK_MESSAGE(upd::whyProgram(one) == nullptr, upd::kVerbs[i]);
  }
}

// --------------------------------------------------------------- a switch back that is refused

TEST_CASE("a switch back refused for the battery: open, and the next wake tries again") {
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  // The battery goes below the threshold while the radio is a scanner: the switch back's own
  // check refuses, and nothing puts the repeater back in this wake.
  const Stage end = unit.run([&unit](const upd::SessionStep& s) {
    if (s.changed && s.after == Stage::kRoom) unit.walter->battery.millivolts = 3000;
  });
  INFO(unit.stageText());
  CHECK(end == Stage::kStuck);
  CHECK(unit.session().status().ending == Ending::kStuck);
  CHECK(unit.session().status().tries == 1);
  CHECK(unit.world.radio.scanner);  // still a scanner
  // One journal line for that try, and the session is still open.
  const std::vector<Stage> journalled = unit.journalledStages();
  INFO(stagesOf(journalled));
  CHECK(journalled.back() == Stage::kStuck);
  int stucks = 0;
  for (Stage stage : journalled) {
    if (stage == Stage::kStuck) ++stucks;
  }
  CHECK(stucks == 1);
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK(mark.open);
  // The room was given back all the same: a session that cannot end still owes nothing to the heap.
  CHECK(unit.walter->scratch.out == 0);

  // The next wake, with the battery still low: one more line, still open.
  unit.reboot();
  unit.walter->battery.millivolts = 3000;
  REQUIRE(unit.session().resume());
  CHECK(unit.session().status().resumed);
  CHECK(unit.run() == Stage::kStuck);
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK(mark.open);
  CHECK(unit.world.radio.scanner);

  // And the wake after, with the battery back: restored and closed.
  unit.reboot();
  REQUIRE(unit.session().resume());
  CHECK(unit.run() == Stage::kRestored);
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  CHECK(unit.walter->scratch.out == 0);
}

TEST_CASE("a restart in the middle of a step: the next boot ends the session") {
  SessionUnit unit;
  const upd::Program program = programOf({{"dwell 869.618 60", 300}});
  bench::Answer slow;
  slow.done = false;
  slow.lines = 3;
  unit.scripted.answers["dwell"] = slow;
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  // The Walter dies while the step is running.
  unit.run([&unit](const upd::SessionStep&) {
    if (unit.session().stage() == Stage::kStep && unit.session().status().scanner_ms > 500) {
      unit.walter->die();
    }
  });
  REQUIRE(unit.world.dead);
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK(mark.open);
  CHECK(mark.stage == Stage::kStep);
  CHECK(mark.step == 1);

  // The next boot: no program, no sink, no console - the golden image back and proved.
  unit.reboot();
  CHECK_FALSE(unit.walter->update.resume());  // the scanner's own run ended `done`: nothing to resume
  REQUIRE(unit.session().resume());
  const Stage end = unit.run();
  INFO(unit.stageText());
  CHECK(end == Stage::kRestored);
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
  // A resumed run takes no room from the heap at all.
  CHECK(unit.walter->scratch.asks == 0);
  // ... and its entries say they were written after a restart.
  upd::SessionPayload restored;
  REQUIRE(unit.entryOf(Stage::kRestored, &restored));
  CHECK((restored.flags & upd::kSessionFlagResumed) != 0);
  CHECK(restored.at_epoch == 1760000000);
}

TEST_CASE("the golden image already back and proved is not flashed a second time") {
  // The case a power cut leaves behind: the switch back's own run reached `done` and the
  // session's `restored` entry never got written. The next wake must not spend a quarter of an
  // hour flashing an image that is already on the chip and proved.
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unit.run([&unit](const upd::SessionStep&) {
    // The Walter dies the moment the switch back's own run is proved - the step that drove it
    // there - and so before the session's `restored` entry is written.
    if (unit.walter->update.state() == bench::State::kDone &&
        unit.walter->update.status().kind == bench::Kind::kRoleRepeater) {
      unit.walter->die();
    }
  });
  REQUIRE(unit.world.dead);
  // The mark is read after the boot: a dead Walter's journal flash answers nothing, and walking
  // back past the radio update's own newest entry needs a read (core/update/journal.h).
  unit.reboot();
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  REQUIRE(mark.open);
  const unsigned writes = unit.world.chip.counters().writes;
  REQUIRE(unit.session().resume());
  CHECK(unit.run() == Stage::kRestored);
  CHECK(unit.world.chip.counters().writes == writes);  // not one word of flash
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
}

// --------------------------------------------------------------- what nothing else may do

TEST_CASE("a scanner nobody opened a session for is left alone, and the mark says so") {
  SessionUnit unit;
  // `rak role scanner` by hand: the update machine's own run, with no session around it.
  REQUIRE(unit.walter->update.start(unit.request(bench::Kind::kRoleScanner)));
  REQUIRE(unit.walter->run() == bench::State::kDone);
  CHECK(unit.world.radio.scanner);
  // No session was ever opened, so there is nothing for a wake to take up - at this wake or the
  // next three.
  upd::Mark mark;
  CHECK_FALSE(upd::readMark(unit.walter->journal, &mark));
  for (int wake = 0; wake < 4; ++wake) {
    unit.reboot();
    CHECK_FALSE(unit.session().resume());
    CHECK_FALSE(unit.session().active());
    CHECK(unit.world.radio.scanner);
    CHECK_FALSE(unit.world.chipHolds(unit.golden));
    unit.world.clock.advance(15u * 60 * 1000);
  }
  CHECK(unit.walter->scratch.asks == 0);
}

TEST_CASE("an open session is ended and never begun again") {
  SessionUnit unit;
  const upd::Program program = programOf({{"dwell 869.618 60", 300}});
  bench::Answer slow;
  slow.done = false;
  unit.scripted.answers["dwell"] = slow;
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unit.run([&unit](const upd::SessionStep&) {
    if (unit.session().stage() == Stage::kStep) unit.walter->die();
  });
  unit.reboot();
  // A second `start()` on a board whose journal says a session is open is refused, and the
  // refusal is the one that says why.
  CHECK_FALSE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  CHECK(unit.session().refusal() == Refusal::kInProgress);
  CHECK(unit.walter->scratch.out == 0);
  // And `resume()` is what does get it back.
  REQUIRE(unit.session().resume());
  CHECK(unit.run() == Stage::kRestored);
}

TEST_CASE("every stage and every ending has a word and a phrase, and only restored is closed") {
  const Stage all[] = {Stage::kIdle,  Stage::kCheck, Stage::kFlashingScanner, Stage::kScanner,
                       Stage::kStep,  Stage::kRoom,  Stage::kFlashingGolden,  Stage::kBooting,
                       Stage::kProving, Stage::kRestored, Stage::kStuck};
  for (Stage stage : all) {
    CHECK(std::string(upd::name(stage)) != "?");
    CHECK(std::string(upd::describe(stage)) != "?");
  }
  CHECK_FALSE(upd::isOpen(Stage::kIdle));
  CHECK_FALSE(upd::isOpen(Stage::kRestored));
  CHECK(upd::isOpen(Stage::kStuck));
  CHECK(upd::isOpen(Stage::kCheck));
  CHECK(upd::isOpen(Stage::kStep));
  const StepEnd ends[] = {StepEnd::kNone, StepEnd::kDone, StepEnd::kClock, StepEnd::kFull,
                          StepEnd::kRefused, StepEnd::kSilent};
  for (StepEnd end : ends) {
    CHECK(std::string(upd::name(end)) != "?");
    CHECK(std::string(upd::describe(end)) != "?");
  }
  const Ending endings[] = {Ending::kRunning, Ending::kRestored, Ending::kRefused, Ending::kStuck};
  for (Ending ending : endings) {
    CHECK(std::string(upd::name(ending)) != "?");
    CHECK(std::string(upd::describe(ending)) != "?");
  }
  // The bound a wake asks for is two passes of the update machine and the ceiling, and it is one
  // a wake may ask for.
  CHECK(upd::kSessionBoundMs == upd::kWholeRunMs + upd::kSessionCeilingMs);
  CHECK(upd::kSessionBoundMs <= core::cycle::kRaisedBoundMax);
}
