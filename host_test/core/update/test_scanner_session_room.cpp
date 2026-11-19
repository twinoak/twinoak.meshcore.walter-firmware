// The room of a session, and where it comes from (task p16-05).
//
// Task p16-03 gave a step 8 192 bytes of the internal heap, because that is what a unit in service
// has in one piece. The module has 2 MB of PSRAM, switched off since the first task; this task
// switches it on **for that one buffer**, so a step's room is 65 536 bytes where there is PSRAM
// that answers and 8 192 bytes where there is not.
//
// What this file is about is the four boards: one with PSRAM, one with none, one whose chip does
// not come up (the 8 MB octal variant driven in quad mode, a chip that was never fitted, one that
// has stopped answering) and one that comes up and **lies** - which `FakeScratch` scripts with two
// mappings of one memfd, so that the upper half of every room it gives out is the lower half of
// it. No test here can bring a real PSRAM up: `docs/bench.md`'s ⚠ items are that proof, and
// `tasks/phase-16/FOLLOWUPS.md` carries it.
//
// Rail **R24** is the other half of this: the PSRAM is named in one file, asked for by one file,
// and `sdkconfig.defaults` keeps malloc(), `.bss` and `.noinit` out of it.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/console/heap_report.h"
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

// The heap behind its seam, scripted read by read: a session reads it three times - before the
// room is taken, just after, and when the steps are over.
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

// The three reads of the unit in service, which is where every figure in this file comes from:
// *heap: 27472 bytes free, largest free block 12288, least free since this boot 25300*.
ScriptedHeap theUnitInService() {
  ScriptedHeap heap;
  core::console::HeapReport::Figures before;
  before.free_bytes = 27472;
  before.largest_block = 12288;
  before.min_free = 25300;
  core::console::HeapReport::Figures after = before;
  after.largest_block = 4096;  // only true when the room came out of this heap
  core::console::HeapReport::Figures last = before;
  last.min_free = 17108;  // the least free while the room was out
  heap.script.push_back(before);
  heap.script.push_back(after);
  heap.script.push_back(last);
  return heap;
}

// A session over this unit's objects with a heap seam of our own, so that the `room` line's
// figures are a test's and not a simulator's.
struct Session {
  ScriptedHeap heap = theUnitInService();
  SessionUnit& unit;
  upd::ScannerSession session;

  explicit Session(SessionUnit& u)
      : unit(u),
        session(u.walter->update, u.walter->client, u.world.store.store, u.walter->journal,
                u.world.clock, u.world.store.wall, u.walter->battery, u.walter->scratch, &heap) {}

  // The stages this run went through, for a message when one of them is not the one expected.
  std::string notes;

  Stage run(int max_steps = 400000) {
    int steps = 0;
    while (session.active() && !unit.world.dead && steps++ < max_steps) {
      const upd::SessionStep s = session.step();
      if (s.changed) notes += std::string(upd::name(s.after)) + " (" + s.note + ") ";
      if (s.stalled) break;
      if (s.waited) unit.world.clock.delayMs(upd::kListenIdleMs);
    }
    CHECK(steps < max_steps);
    return session.stage();
  }
};

std::string lineOf(const upd::RoomFigures& figures) {
  char line[upd::kRoomLineMax + 1];
  const std::size_t n = upd::roomLine(figures, line, sizeof line);
  CHECK(n <= upd::kRoomLineMax);
  CHECK(n == std::strlen(line));
  return std::string(line);
}

constexpr std::size_t kTwoMegabytes = 2u * 1024u * 1024u;

}  // namespace

// -------------------------------------------------------------- the board that has some PSRAM

TEST_CASE("a board with PSRAM holds a step's lines in 64 kB of it") {
  SessionUnit unit;
  unit.walter->scratch.externalOf(kTwoMegabytes);
  Session run(unit);
  const upd::Program program = programOf({{"sweep 863 870 25", 10}});
  REQUIRE(run.session.start(program, bench::Slot{0}, &unit.sink));
  // Taken before anything is journalled and long before anything is flashed, which is what makes
  // "the pattern is read back before the first line goes in" true by construction: the room is
  // in hand and proved when start() returns, and the scanner image has not gone on yet.
  CHECK(run.session.stage() == Stage::kCheck);
  CHECK(unit.walter->scratch.bring_ups == 1);
  CHECK(unit.walter->scratch.external_asks == 1);
  CHECK(unit.walter->scratch.external_last_asked == upd::kStepBytesExternal);
  CHECK(unit.walter->scratch.asks == 0);  // nothing of the internal heap was taken at all
  CHECK(run.session.status().room.bytes == upd::kStepBytesExternal);

  REQUIRE(run.run() == Stage::kRestored);

  // The `room` entry, read back out of flash: the room, where it came from, and what the
  // **internal** heap had while it was out.
  upd::SessionPayload room;
  REQUIRE(unit.entryOf(Stage::kRoom, &room));
  CHECK(room.room.known);
  CHECK(room.room.bytes == upd::kStepBytesExternal);
  CHECK(room.room.external == kTwoMegabytes);
  CHECK(room.room.free_bytes == 27472);
  CHECK(room.room.block == 12288);
  CHECK(room.room.least_free == 17108);
  CHECK(lineOf(room.room) == "65536 psram of 2097152; internal 27472, block 12288; least 17108");

  // And it was given back: nothing is out, and the PSRAM is whole again.
  CHECK(unit.walter->scratch.out == 0);
  CHECK(unit.walter->scratch.gives == 1);
  const interfaces::Scratch::ExternalRoom outside = unit.walter->scratch.external();
  CHECK(outside.state == interfaces::Scratch::External::kUp);
  CHECK(outside.bytes == kTwoMegabytes);
  CHECK(outside.free_bytes == kTwoMegabytes);
}

TEST_CASE("the room line fits its 72 bytes at the worst figures either kind of board can have") {
  // Six digits of internal heap (this chip has under a megabyte of it) and the 8 MB variant's
  // PSRAM, which is the longest either line can be.
  upd::RoomFigures big;
  big.known = true;
  big.bytes = upd::kStepBytesExternal;
  big.external = 8u * 1024u * 1024u;
  big.free_bytes = 999999;
  big.block = 999999;
  big.left_block = 999999;
  big.least_free = 999999;
  CHECK(lineOf(big) == "65536 psram of 8388608; internal 999999, block 999999; least 999999");

  upd::RoomFigures small = big;
  small.external = 0;
  small.bytes = upd::kStepBytes;
  CHECK(lineOf(small) == "8192 of 999999, block 999999; left block 999999; least free 999999");

  // A board with PSRAM and a build with no heap seam: the room is still said.
  upd::RoomFigures blind = big;
  blind.known = false;
  CHECK(lineOf(blind) == "65536 psram of 8388608; this build reports no heap figures");
}

// ------------------------------------------------- the three boards that hold 8 192 bytes instead

namespace {

// The three ways a board does not get the big room, each scripted on the seam.
struct NoBigRoom {
  const char* what;
  void (*script)(fakes::FakeScratch&);
};

void noneAtAll(fakes::FakeScratch& scratch) { scratch.noExternal(); }
void doesNotComeUp(fakes::FakeScratch& scratch) { scratch.externalWillNotComeUp(kTwoMegabytes); }
void failsItsPattern(fakes::FakeScratch& scratch) {
  scratch.externalHasADeadAddressLine(kTwoMegabytes);
}

const NoBigRoom kNoBigRoom[] = {
    {"a board with no external memory at all", noneAtAll},
    {"a board whose chip does not come up: the octal variant, or one never fitted", doesNotComeUp},
    {"a board whose chip comes up and does not hold what is written to it", failsItsPattern},
};

}  // namespace

TEST_CASE("no big room: the session runs in 8 192 bytes of the internal heap, and says so") {
  for (const NoBigRoom& board : kNoBigRoom) {
    SUBCASE(board.what) {
      SessionUnit unit;
      board.script(unit.walter->scratch);
      Session run(unit);
      const upd::Program program = programOf({{"sweep 863 870 25", 10}});
      REQUIRE(run.session.start(program, bench::Slot{0}, &unit.sink));
      // **Before the first line goes in**: the fall-back has already happened when start()
      // returns, so a chip that lies costs the session nothing but the big room.
      CHECK(run.session.status().room.bytes == upd::kStepBytes);
      CHECK(unit.walter->scratch.asks == 1);
      CHECK(unit.walter->scratch.last_asked == upd::kStepBytes);
      CHECK(unit.walter->scratch.out == 1);

      REQUIRE(run.run() == Stage::kRestored);
      CHECK(run.session.status().ending == Ending::kRestored);
      REQUIRE(unit.sink.steps.size() == 1);
      CHECK(unit.sink.steps[0].end == StepEnd::kDone);

      // Task p16-03's own line, unchanged, with no word about a PSRAM in it.
      upd::SessionPayload room;
      REQUIRE(unit.entryOf(Stage::kRoom, &room));
      CHECK(room.room.bytes == upd::kStepBytes);
      CHECK(room.room.external == 0);
      CHECK(lineOf(room.room) == "8192 of 27472, block 12288; left block 4096; least free 17108");
      CHECK(unit.walter->scratch.out == 0);
    }
  }
}

TEST_CASE("a chip asked for once in a boot that did not come up is not asked again") {
  SessionUnit unit;
  unit.walter->scratch.externalWillNotComeUp(kTwoMegabytes);
  {
    Session first(unit);
    const upd::Program program = programOf({{"stat", 5}});
    REQUIRE(first.session.start(program, bench::Slot{0}, &unit.sink));
    REQUIRE(first.run() == Stage::kRestored);
  }
  CHECK(unit.walter->scratch.bring_ups == 1);
  // `heap` can say which of the two things this board is, which is the whole reason the seam
  // keeps the flag.
  CHECK(unit.walter->scratch.external().state == interfaces::Scratch::External::kFailed);
  // A deep sleep is a boot: nothing is kept over one, and the next boot asks again.
  unit.walter->scratch.asAfterABoot();
  CHECK(unit.walter->scratch.external().state == interfaces::Scratch::External::kAsleep);
}

// ------------------------------------------------------------------------------- the pattern

TEST_CASE("the pattern: a room that holds what is written to it, and one that does not") {
  fakes::FakeScratch scratch;
  // A good block of the internal heap: the pattern holds, and the room is left full of it.
  char* good = static_cast<char*>(scratch.take(4096));
  REQUIRE(good != nullptr);
  CHECK(upd::patternHolds(good, 4096));
  // The pattern is a value of each word's own index, so no two words of it are the same and a
  // room that answers one address for another is caught.
  CHECK(std::memcmp(good, good + 2048, 2048) != 0);
  scratch.give(good);

  // And a chip with a dead address line: the upper half of the room is the lower half of it.
  scratch.externalHasADeadAddressLine(kTwoMegabytes);
  char* bad = static_cast<char*>(scratch.takeExternal(upd::kStepBytesExternal));
  REQUIRE(bad != nullptr);
  CHECK_FALSE(upd::patternHolds(bad, upd::kStepBytesExternal));
  scratch.give(bad);

  CHECK_FALSE(upd::patternHolds(nullptr, 16));
  CHECK_FALSE(upd::patternHolds(good, 0));
}

TEST_CASE("takeStepRoom leaves the room full of the pattern it read back") {
  fakes::FakeScratch scratch;
  scratch.externalOf(kTwoMegabytes);
  ScriptedHeap heap = theUnitInService();
  upd::RoomFigures figures;
  upd::Room room = upd::takeStepRoom(scratch, &heap, &figures);
  REQUIRE(room.data != nullptr);
  CHECK(room.bytes == upd::kStepBytesExternal);
  CHECK(room.external);
  // What is in the room when it is handed over is what was written and read back - which is how a
  // reader sees that the whole of it was touched before a line of the scanner's went in.
  CHECK(upd::patternHolds(room.data, room.bytes));
  upd::giveStepRoom(scratch, &room);
  CHECK(room.data == nullptr);
  CHECK(room.bytes == 0);
  CHECK(scratch.out == 0);
  // Safe on a room that was never taken, which is what every path that gives one back relies on.
  upd::giveStepRoom(scratch, &room);
  CHECK(scratch.gives == 1);
}

TEST_CASE("neither room to be had: the session is refused noMemory and nothing is touched") {
  SessionUnit unit;
  unit.walter->scratch.noExternal();
  unit.walter->scratch.onlyHas(upd::kStepBytes - 1);
  Session run(unit);
  const upd::Program program = programOf({{"stat", 5}});
  CHECK_FALSE(run.session.start(program, bench::Slot{0}, &unit.sink));
  CHECK(run.session.refusal() == Refusal::kNoMemory);
  CHECK(run.session.status().room.left_block == upd::kStepBytes - 1);
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK_FALSE(unit.entryOf(Stage::kCheck, nullptr));
}

// ------------------------------------------------- what the two rooms do to a minute of `scope`

namespace {

// A minute of `scope` at a frame a second: sixty lines of 482 bytes, which is 250 points. The
// task's own figures.
constexpr std::size_t kScopeFrames = 60;
constexpr std::size_t kScopeBytes = 482;

upd::Program oneMinuteOfScope() { return programOf({{"scope 863 870 250 0 1000", 65}}); }

void scriptAMinuteOfScope(SessionUnit& unit) {
  bench::Answer scope;
  scope.lines = kScopeFrames;
  scope.line_bytes = kScopeBytes;
  scope.done = false;  // a real `scope` is stopped by the clock, not by a `done`
  unit.scripted.answers["scope"] = scope;
  unit.scripted.first_line_ms = 1000;
  unit.scripted.between_ms = 1000;
}

}  // namespace

TEST_CASE("a minute of scope: sixty frames in the big room, and what the small one keeps") {
  SUBCASE("the big room takes all sixty, and the clock ends the step") {
    SessionUnit unit;
    unit.walter->scratch.externalOf(kTwoMegabytes);
    scriptAMinuteOfScope(unit);
    Session run(unit);
    const upd::Program program = oneMinuteOfScope();
    REQUIRE(run.session.start(program, bench::Slot{0}, &unit.sink));
    REQUIRE(run.run() == Stage::kRestored);
    REQUIRE(unit.sink.steps.size() == 1);
    const bench::StepTaken& step = unit.sink.steps[0];
    CHECK(step.end == StepEnd::kClock);
    // The `ack` and the sixty frames; the greeting's three `ack`s and its `info` line are in this
    // step too, because it is the first (task p16-03's deliverable 2).
    std::size_t frames = 0;
    for (const std::string& line : step.lines) {
      if (line.size() == kScopeBytes) ++frames;
    }
    CHECK(frames == kScopeFrames);
    CHECK(step.used < upd::kStepBytesExternal);
    CHECK(step.used > kScopeFrames * kScopeBytes);
    CHECK(unit.scripted.wasSaid("stop"));
  }
  SUBCASE("the small room ends full, with every line it kept whole") {
    SessionUnit unit;
    unit.walter->scratch.noExternal();
    scriptAMinuteOfScope(unit);
    Session run(unit);
    const upd::Program program = oneMinuteOfScope();
    REQUIRE(run.session.start(program, bench::Slot{0}, &unit.sink));
    REQUIRE(run.run() == Stage::kRestored);
    REQUIRE(unit.sink.steps.size() == 1);
    const bench::StepTaken& step = unit.sink.steps[0];
    CHECK(step.end == StepEnd::kFull);
    std::size_t frames = 0;
    for (const std::string& line : step.lines) {
      if (line.size() == kScopeBytes) ++frames;
    }
    // About thirteen frames, as the task says, and no half line: the sink checks every line it
    // takes begins `{` and ends `}`.
    CHECK(frames >= 10);
    CHECK(frames < kScopeFrames);
    CHECK(step.used <= upd::kStepBytes);
    CHECK(unit.scripted.wasSaid("stop"));
  }
}

// ------------------------------------------------------ a journal entry written before this task

TEST_CASE("a room entry of twenty bytes - written before this task - reads as the small room") {
  // The sixth figure is new. An entry a firmware before it wrote has a body of twenty bytes, and
  // the line it prints is task p16-03's, which is what it meant.
  upd::Entry entry;
  entry.kind = upd::kind::kScannerSession;
  entry.seq = 7;
  entry.size = upd::kSessionHeaderBytes + upd::kRoomFigureBytesBefore05;
  std::memset(entry.payload, 0, sizeof entry.payload);
  entry.payload[0] = static_cast<std::uint8_t>(Stage::kRoom);
  const std::uint32_t figures[] = {upd::kStepBytes, 27472, 12288, 4096, 17108};
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t b = 0; b < 4; ++b) {
      entry.payload[upd::kSessionHeaderBytes + i * 4 + b] =
          static_cast<std::uint8_t>((figures[i] >> (8 * b)) & 0xFFu);
    }
  }
  upd::SessionPayload p;
  REQUIRE(upd::decode(entry, &p));
  CHECK(p.room.known);
  CHECK(p.room.bytes == upd::kStepBytes);
  CHECK(p.room.external == 0);
  CHECK(lineOf(p.room) == "8192 of 27472, block 12288; left block 4096; least free 17108");
}
