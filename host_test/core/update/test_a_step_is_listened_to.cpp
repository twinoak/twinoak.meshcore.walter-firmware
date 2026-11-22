// A step is listened to (task p18-03), the half of it that is core's.
//
// On 21 November the unit at Bellevue ran a session of six steps. The fifth was
// `hist 869.418 869.818 25`; the scanner numbered its lines `seq` 245 to 260 and its `done` 261,
// and meshmaster.dk holds 245 to 251 and 261. Nine measurement lines are simply not there, and
// the step's result says *done, 9 lines* with nothing anywhere saying that any are missing.
//
// What was on this side of the UART: the port is not read while a call to the server is out, and
// the receive ring was 1 024 bytes. So this file is about the three things that follow:
//
//   the moment    a step's journal entry and its command are **two** calls of the machine, so
//                 whoever drives it has a moment to tell a server what is about to happen while
//                 the scanner is still silent. `listening()` is false in it and true after it;
//   the ring      sized from the baud rate and the longest the reader is away, and a port that
//                 is read every `kListenIdleMs` loses nothing at the baud rate for ten seconds;
//   the loss      counted at the port, carried to the client's counters, and written into the
//                 journal at the end of any step that lost anything.
//
// The job's half - no call to the server between a step's command and its end, and the lease that
// covers a step before it begins - is host_test/agent/test_a_step_is_listened_to.cpp.
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/link/client/client.h"
#include "core/update/scanner_session.h"
#include "session_bench.h"
#include "sim/link_bench.h"

namespace lc = core::link::client;
namespace upd = core::update;

using bench::programOf;
using bench::SessionUnit;
using bench::Stage;
using bench::StepEnd;

namespace {

// A line of `size` bytes with a `seq` of its own, as `bench::scannerLine` makes one.
std::string lineOf(const char* what, unsigned seq, std::size_t size) {
  return bench::scannerLine(what, seq, size);
}

}  // namespace

// ------------------------------------------------------------------ the ring, by arithmetic

TEST_CASE("the receive ring follows from the baud rate and the longest the reader is away") {
  // The numbers, in one place, and the `static_assert`s beside them are what hold them: this case
  // is here so that the arithmetic is written down in a test too, and so that a reader of the
  // suite meets the figures.
  CHECK(lc::kBaud == 115200u);
  CHECK(lc::kBytesPerSecond == 11520u);  // 8N1 is ten bits a byte
  // The reader's absence: the session loop's own idle, and what it does between two reads. The
  // first of the two is `core::update::kListenIdleMs` and a `static_assert` in
  // core/update/scanner_session.h holds them equal.
  CHECK(lc::kReaderIdleMs == core::update::kListenIdleMs);
  CHECK(lc::kReaderAwayMs == 150u);
  CHECK(lc::kCarriedWhileAway == 1728u);  // 150 ms of this line
  // And the ring, with better than twice that as the margin. 4 096 bytes is 355 ms of the line.
  CHECK(lc::kRxRingBytes == 4096u);
  const bool twice_what_the_line_carries = lc::kRxRingBytes >= 2 * lc::kCarriedWhileAway;
  CHECK(twice_what_the_line_carries);
  // What 21 November had, and what it was worth: 1 024 bytes is 88 ms of this line, which is less
  // than the loop's own idle - so a reader doing nothing wrong at all lost bytes.
  const bool the_old_ring_was_under_one_idle = 1024u < lc::kBytesPerSecond * lc::kReaderIdleMs / 1000u;
  CHECK(the_old_ring_was_under_one_idle);
}

// ------------------------------------------------------------------ ten seconds at the baud rate

// A bench whose radio is a scanner, and whose port is a wire of §1's baud rate with the Walter's
// own ring behind it - the two knobs `bench::ScriptedScanner::install()` turns, turned by hand
// here because this case scripts the lines itself.
namespace {

struct WireBench : sim::LinkBench {
  WireBench() {
    radio.scanner = true;
    radio.wire_rate = true;
    REQUIRE(client.hello().fault == lc::Fault::kLinkDown);
    REQUIRE(client.mode() == lc::Mode::kText);
  }
};

}  // namespace

TEST_CASE("a scanner that fills the port for ten seconds: the loop as it is loses no byte") {
  WireBench bench;
  // A line of 280 bytes every 25 ms is 11 240 bytes a second, which is 98 per cent of the baud
  // rate: the line is never idle for the whole ten seconds. Four hundred lines, 112 400 bytes -
  // and each under `kTextLineMax`, so what this case measures is the port and not the client's own
  // line buffer.
  const std::size_t kLineBytes = 280;
  const std::size_t kLines = 400;
  REQUIRE(kLineBytes < lc::kTextLineMax);
  for (std::size_t i = 0; i < kLines; ++i) {
    bench.radio.sendLine(lineOf("sweep", static_cast<unsigned>(100 + i), kLineBytes),
                         static_cast<std::uint32_t>(i * 25));
  }
  // One text line was already seen: §9's `info` probe, which is how the client decided this radio
  // is a scanner at all.
  const std::uint32_t before = bench.client.counters().text_lines;
  // The loop as a session's is: read, then sleep `kListenIdleMs`. Eleven seconds of it, so that
  // the last line has time to arrive.
  for (int tick = 0; tick < 110; ++tick) {
    bench.client.poll();
    bench.clock.delayMs(core::update::kListenIdleMs);
  }
  // Nothing was lost at the port, and the client saw every line whole.
  CHECK(bench.radio.lost_bytes == 0);
  CHECK(bench.radio.losses() == 0);
  CHECK(bench.client.counters().port_losses == 0);
  CHECK(bench.client.counters().text_lines == before + kLines);
  CHECK(bench.client.counters().text_truncated == 0);
}

TEST_CASE("the same ten seconds into the ring of 21 November: the port loses, and says so") {
  WireBench bench;
  bench.radio.rx_ring = 1024;  // the board before this task
  const std::size_t kLineBytes = 280;
  const std::size_t kLines = 400;
  const std::uint32_t before = bench.client.counters().text_lines;
  for (std::size_t i = 0; i < kLines; ++i) {
    bench.radio.sendLine(lineOf("sweep", static_cast<unsigned>(100 + i), kLineBytes),
                         static_cast<std::uint32_t>(i * 25));
  }
  for (int tick = 0; tick < 110; ++tick) {
    bench.client.poll();
    bench.clock.delayMs(core::update::kListenIdleMs);
  }
  // 1 024 bytes is 88 ms of this line and the reader is away for 100: the ring is behind from the
  // first tick, and lines go.
  CHECK(bench.radio.lost_bytes > 0);
  CHECK(bench.client.counters().port_losses > 0);
  CHECK(bench.client.counters().port_losses == bench.radio.losses());
  CHECK(bench.client.counters().text_lines < before + kLines);
}

// ------------------------------------------------------------------ the entry before the command

TEST_CASE("a step's entry is journalled by one call and its command said by the next") {
  SessionUnit unit;
  const upd::Program program = programOf({{"hist 869.418 869.818 25", 40}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));

  // Up to the answer that enters `step`, nothing of the step has been said.
  bool entered = false;
  while (unit.session().active() && !entered) {
    const upd::SessionStep s = unit.session().step();
    REQUIRE_FALSE(s.stalled);
    if (s.changed && s.after == Stage::kStep) {
      entered = true;
      break;
    }
    if (s.waited) unit.world.clock.delayMs(upd::kListenIdleMs);
  }
  REQUIRE(entered);

  // **The moment.** The entry is in the flash with the command as its body, the machine says it
  // is not listening to anything yet, and `stepPending()` says how many seconds the step about to
  // begin was given - which is what a caller's lease has to cover.
  upd::SessionPayload entry;
  REQUIRE(unit.entryOf(Stage::kStep, &entry));
  CHECK(std::string(entry.command) == "hist 869.418 869.818 25");
  CHECK_FALSE(unit.session().listening());
  CHECK(unit.session().stepPending() == 40u);
  // And the scanner has been told nothing of it: the greeting's three words and no more.
  CHECK(unit.scripted.commandsAfterGreeting().empty());

  // The next call says it, and from then on the machine is listening and `stepPending()` is nought.
  (void)unit.session().step();
  CHECK(unit.session().listening());
  CHECK(unit.session().stepPending() == 0u);
  const std::vector<std::string> said = unit.scripted.commandsAfterGreeting();
  REQUIRE(said.size() == 1);
  CHECK(said[0] == "hist 869.418 869.818 25");

  // The rest of the session, and the step ends: `listening()` is false again the moment its lines
  // have been handed over.
  CHECK(unit.run() == Stage::kRestored);
  CHECK_FALSE(unit.session().listening());
  REQUIRE(unit.sink.steps.size() == 1);
  CHECK(unit.sink.steps[0].end == StepEnd::kDone);
  CHECK(unit.sink.steps[0].losses == 0);
}

TEST_CASE("the step's seconds begin when its command is said, not when its entry is written") {
  SessionUnit unit;
  const upd::Program program = programOf({{"dwell 869.618 60", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  // A scanner that answers and says nothing else: the step runs its thirty seconds out.
  bench::Answer quiet;
  quiet.lines = 0;
  quiet.done = false;
  unit.scripted.answer = quiet;

  std::uint64_t entered_ms = 0;
  std::uint64_t said_ms = 0;
  unit.scripted.on_command = [&unit, &said_ms](const std::string& line) {
    if (line == "dwell 869.618 60") said_ms = unit.world.clock.nowMs();
  };
  bool first = true;
  std::uint64_t ended_ms = 0;
  while (unit.session().active()) {
    const bool was_pending = unit.session().stepPending() != 0;
    if (was_pending && first) {
      // A caller that spends five seconds telling a server, in the one moment it may.
      entered_ms = unit.world.clock.nowMs();
      unit.world.clock.advance(5000);
      first = false;
    }
    const upd::SessionStep s = unit.session().step();
    if (ended_ms == 0 && !unit.sink.steps.empty()) ended_ms = unit.world.clock.nowMs();
    if (s.stalled) break;
    if (s.waited) unit.world.clock.delayMs(upd::kListenIdleMs);
  }
  REQUIRE(unit.sink.steps.size() == 1);
  CHECK(unit.sink.steps[0].end == StepEnd::kClock);
  // The five seconds were not the step's: it ran its thirty from the command.
  CHECK(said_ms >= entered_ms + 5000);
  CHECK(ended_ms >= said_ms + 30000);
  CHECK(ended_ms < said_ms + 30000 + 2 * upd::kListenIdleMs);
}

// ------------------------------------------------------------------ a loss, counted and said

TEST_CASE("a ring too small on purpose: the step says so, in the journal and to its caller") {
  SessionUnit unit;
  unit.world.radio.rx_ring = 256;  // smaller than one line: every one of them is cut
  bench::Answer many;
  many.lines = 8;
  many.line_bytes = 400;
  unit.scripted.answers["sweep"] = many;
  const upd::Program program = programOf({{"sweep", 20}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  CHECK(unit.run() == Stage::kRestored);

  REQUIRE(unit.sink.steps.size() == 1);
  CHECK(unit.sink.steps[0].losses > 0);
  // The journal entry of the step's **end**: the command, how it ended, and the count. It is a
  // second `step` entry and it is written only when something was lost.
  upd::Cursor cursor;
  upd::Entry entry;
  std::size_t step_entries = 0;
  std::size_t lossy = 0;
  upd::SessionPayload lost{};
  bool more = unit.walter->journal.first(&cursor, &entry);
  while (more) {
    upd::SessionPayload p;
    if (entry.kind == upd::kind::kScannerSession && upd::decode(entry, &p) &&
        p.stage == Stage::kStep) {
      ++step_entries;
      if ((p.flags & upd::kSessionFlagLost) != 0) {
        ++lossy;
        lost = p;
      }
    }
    more = unit.walter->journal.previous(&cursor, &entry);
  }
  CHECK(step_entries == 2);  // the one that opened the step and the one that ended it
  CHECK(lossy == 1);
  CHECK(lost.step == 1);
  CHECK(lost.losses == unit.sink.steps[0].losses);
  CHECK(lost.end == unit.sink.steps[0].end);
  // The command is not in **this** entry (a slot has no room for it beside the count): it is in
  // the one that opened the step, which the console and the result's journal read beside it.
  CHECK(std::string(lost.command).empty());
}

TEST_CASE("a step that lost nothing writes no second entry: the journal is what it always was") {
  SessionUnit unit;
  const upd::Program program = programOf({{"stat", 10}, {"cad", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  CHECK(unit.run() == Stage::kRestored);
  REQUIRE(unit.sink.steps.size() == 2);
  CHECK(unit.sink.steps[0].losses == 0);
  CHECK(unit.sink.steps[1].losses == 0);
  std::size_t step_entries = 0;
  upd::Cursor cursor;
  upd::Entry entry;
  bool more = unit.walter->journal.first(&cursor, &entry);
  while (more) {
    upd::SessionPayload p;
    if (entry.kind == upd::kind::kScannerSession && upd::decode(entry, &p) &&
        p.stage == Stage::kStep) {
      ++step_entries;
      CHECK((p.flags & upd::kSessionFlagLost) == 0);
    }
    more = unit.walter->journal.previous(&cursor, &entry);
  }
  CHECK(step_entries == 2);  // one a step, as it has always been
}

TEST_CASE("the journal entry of a lost step: the count and the end, and no command in it") {
  upd::SessionPayload p;
  p.stage = Stage::kStep;
  p.step = 5;
  p.slot = 0;
  p.flags = upd::kSessionFlagLost;
  p.at_epoch = 1795000000;
  p.end = StepEnd::kDone;
  p.losses = 4097;

  std::uint8_t bytes[upd::kPayloadMax];
  const std::size_t n = upd::encode(p, bytes);
  CHECK(n == upd::kSessionHeaderBytes + upd::kSessionLostBytes);
  upd::Entry entry;
  entry.kind = upd::kind::kScannerSession;
  entry.size = static_cast<std::uint8_t>(n);
  for (std::size_t i = 0; i < n; ++i) entry.payload[i] = bytes[i];
  upd::SessionPayload back;
  REQUIRE(upd::decode(entry, &back));
  CHECK(back.stage == Stage::kStep);
  CHECK(back.step == 5);
  CHECK(back.losses == 4097);
  CHECK(back.end == StepEnd::kDone);
  CHECK((back.flags & upd::kSessionFlagLost) != 0);
  CHECK(std::string(back.command).empty());

  // And the entry that **opens** a step is what it always was: the command, no count, no end.
  upd::SessionPayload opened = p;
  opened.flags = 0;
  opened.losses = 0;
  const char* command = "hist 869.418 869.818 25";
  for (std::size_t i = 0; command[i] != 0; ++i) opened.command[i] = command[i];
  const std::size_t m = upd::encode(opened, bytes);
  CHECK(m == upd::kSessionHeaderBytes + 23);
  entry.size = static_cast<std::uint8_t>(m);
  for (std::size_t i = 0; i < m; ++i) entry.payload[i] = bytes[i];
  REQUIRE(upd::decode(entry, &back));
  CHECK(std::string(back.command) == command);
  CHECK(back.losses == 0);
  CHECK(back.end == StepEnd::kNone);
  // The longest command a step may carry still fills a slot and no more.
  for (std::size_t i = 0; i < upd::kCommandMax; ++i) opened.command[i] = 's';
  opened.command[upd::kCommandMax] = 0;
  CHECK(upd::encode(opened, bytes) == upd::kPayloadMax);
}
