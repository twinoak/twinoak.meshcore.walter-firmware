// `rak session` at the console (task p16-03): the read, a whole session of one step, and the one
// thing S-12 asks of it - **no text of a scanner's line in the log, the journal or a report**.
//
// The bench is the radio's update bench with a session on it
// (host_test/core/update/session_bench.h), because a session is two flashes of the simulated
// nRF52840 around a program and the console is what a person drives it with.
#include <doctest.h>

#include <string>

#include "core/update/scanner_session.h"
#include "../update/session_bench.h"

using bench::SessionUnit;
using bench::Stage;
namespace upd = core::update;

namespace {

bool has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("rak session with no words: nothing opened, then open, then closed") {
  SessionUnit unit;
  CHECK(has(unit.walter->runLine("rak session"),
            "no session has been opened on this unit"));

  // One opened and left open: a step running when the Walter died.
  bench::Answer slow;
  slow.done = false;
  unit.scripted.answers["dwell"] = slow;
  const upd::Program program = bench::programOf({{"dwell 869.618 60", 300}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unit.run([&unit](const upd::SessionStep&) {
    if (unit.session().stage() == Stage::kStep) unit.walter->die();
  });
  unit.reboot();
  const std::string open = unit.walter->runLine("rak session");
  CHECK(has(open, "OPEN since epoch 1760000000"));
  CHECK(has(open, "step"));
  CHECK(has(open, "the next wake flashes the golden image back and proves it"));
  CHECK(has(open, "no server, no uplink and no console"));

  // Ended by the wake, and the read says closed - with the room line of that session.
  REQUIRE(unit.session().resume());
  REQUIRE(unit.run() == Stage::kRestored);
  const std::string closed = unit.walter->runLine("rak session");
  CHECK(has(closed, "none open"));
  CHECK(has(closed, "restored"));
  // No `room` line: this session was taken up out of the journal and never took a room at all.
  CHECK_FALSE(has(closed, "room: "));
}

TEST_CASE("rak session <seconds> <command>: a whole session, printed line by line") {
  SessionUnit unit;
  const std::string text = unit.walter->runLine("rak session 30 sweep 863 870 25");
  INFO(text);
  // The two lines that say what it costs, then every stage, then the outcome.
  CHECK(has(text, "rak session: slot 0, one step of 30 s: `sweep 863 870 25`"));
  CHECK(has(text, "the radio is off the mesh for all of it"));
  CHECK(has(text, "session: idle -> check: a session is open in the journal"));
  CHECK(has(text, "session: check -> flashing-scanner"));
  CHECK(has(text, "flashing-scanner -> scanner"));
  CHECK(has(text, "scanner -> step"));
  CHECK(has(text, "step -> room"));
  CHECK(has(text, "room -> flashing-golden"));
  CHECK(has(text, "flashing-golden -> booting"));
  CHECK(has(text, "booting -> proving"));
  CHECK(has(text, "proving -> restored"));
  CHECK(has(text, "rak session: restored: the repeater is back and proved"));
  // The step's own line, and the scanner's lines printed whole: the `sweep` line is 300 bytes and
  // nothing of it is cut.
  CHECK(has(text, "step 1 (`sweep 863 870 25`, 30 s) ended done"));
  CHECK(has(text, "the greeting's acks and its info line among them"));
  CHECK(has(text, "\"t\":\"info\""));
  CHECK(has(text, "\"t\":\"sweep\""));
  CHECK(has(text, "777777"));  // the padding of the fixture's own sweep line, whole
  CHECK(has(text, "room: 8192 of "));
  // And the room is back.
  CHECK(unit.walter->scratch.out == 0);
  CHECK(unit.world.running_version == unit.golden.version);
}

TEST_CASE("rak session: the refusals, each with its word and nothing touched") {
  SUBCASE("the battery") {
    SessionUnit unit;
    unit.walter->battery.millivolts = 3000;
    const std::string text = unit.walter->runLine("rak session 30 sweep");
    CHECK(has(text, "error: rak session: refused: battery"));
    CHECK(unit.world.chipHolds(unit.golden));
  }
  SUBCASE("no scanner image") {
    SessionUnit unit(false);
    CHECK(has(unit.walter->runLine("rak session 30 sweep"),
              "error: rak session: refused: noScanner"));
  }
  SUBCASE("the clock") {
    SessionUnit unit;
    unit.world.store.wall.forget();
    CHECK(has(unit.walter->runLine("rak session 30 sweep"),
              "error: rak session: refused: clock"));
  }
  SUBCASE("no room") {
    SessionUnit unit;
    unit.walter->scratch.onlyHas(upd::kStepBytes - 1);
    const std::string text = unit.walter->runLine("rak session 30 sweep");
    CHECK(has(text, "error: rak session: refused: noMemory"));
    CHECK(has(text, "the room is 8192 bytes"));
  }
  SUBCASE("a command of 41 characters") {
    SessionUnit unit;
    const std::string text =
        unit.walter->runLine("rak session 30 sweep 111111111111111111111111111111111111");
    CHECK(has(text, "error: rak session: refused: program"));
    CHECK(has(text, "at most 40 characters"));
    CHECK(unit.scripted.said.empty());
  }
  SUBCASE("a command beginning reboot") {
    SessionUnit unit;
    const std::string text = unit.walter->runLine("rak session 30 reboot");
    CHECK(has(text, "error: rak session: refused: program"));
    CHECK(has(text, "stat, sweep, scope, hist, dwell, cad"));
  }
  SUBCASE("a command with a semicolon") {
    SessionUnit unit;
    CHECK(has(unit.walter->runLine("rak session 30 sweep;reboot"),
              "error: rak session: refused: program"));
  }
  SUBCASE("4 seconds") {
    SessionUnit unit;
    const std::string text = unit.walter->runLine("rak session 4 sweep");
    CHECK(has(text, "error: rak session: refused: program"));
    CHECK(has(text, "at least 5 seconds"));
  }
  SUBCASE("301 seconds") {
    SessionUnit unit;
    CHECK(has(unit.walter->runLine("rak session 301 sweep"), "at most 300 seconds"));
  }
  SUBCASE("not a number") {
    SessionUnit unit;
    CHECK(has(unit.walter->runLine("rak session sweep 30"),
              "not a whole number of seconds"));
  }
  SUBCASE("no command after the seconds") {
    SessionUnit unit;
    CHECK(has(unit.walter->runLine("rak session 30"), "needs a command after the seconds"));
  }
}

TEST_CASE("the journal prints a session's entries, with the command and the room's figures") {
  SessionUnit unit;
  REQUIRE(unit.walter->runLine("rak session 30 sweep 863 870 25").size() > 0);
  const std::string text = unit.walter->runLine("journal");
  INFO(text);
  CHECK(has(text, "session, restored"));
  CHECK(has(text, "session, proving"));
  CHECK(has(text, "opened at epoch 1760000000"));
  // The last twelve entries only, so the step's own line may have scrolled off; `rak session`
  // prints the room line whatever the journal shows.
  CHECK(has(unit.walter->runLine("rak session"), "room: 8192 of "));
}

// **A step that lost bytes at the port says so where it says how it ended** (task p18-03): the
// typed session calls no server at all, so the only thing that changes for it is this one clause -
// and the journal's own line under it.
TEST_CASE("rak session: a step that lost bytes says so, on its line and in the journal") {
  SessionUnit unit;
  unit.world.radio.rx_ring = 256;  // smaller than one line: the port cannot keep up
  bench::Answer many;
  many.lines = 8;
  many.line_bytes = 400;
  unit.scripted.answers["sweep"] = many;
  unit.scripted.first_line_ms = 20;
  unit.scripted.between_ms = 20;
  const std::string text = unit.walter->runLine("rak session 20 sweep 863 870 25");
  INFO(text);
  CHECK(has(text, "the port lost bytes "));
  CHECK(has(text, " time(s)"));
  // On the same line as the step's ending, which is where a person reads it.
  const std::size_t step_line = text.find("step 1 (`sweep 863 870 25`, 20 s) ended ");
  REQUIRE(step_line != std::string::npos);
  const std::size_t end_of_line = text.find('\n', step_line);
  REQUIRE(end_of_line != std::string::npos);
  CHECK(has(text.substr(step_line, end_of_line - step_line), "the port lost bytes "));
  // And the journal entry of the step's end, printed by `journal` as the entry says it.
  CHECK(has(unit.walter->runLine("journal"), "the port lost bytes "));
}

TEST_CASE("rak session: a step that lost nothing says nothing of a loss") {
  SessionUnit unit;
  const std::string text = unit.walter->runLine("rak session 30 sweep 863 870 25");
  INFO(text);
  CHECK(has(text, "ended done"));
  CHECK_FALSE(has(text, "the port lost bytes "));
}

TEST_CASE("rak session: no byte of a scanner's line is in the log, the journal or a report") {
  SessionUnit unit;
  // A marker only this scanner would ever say, in a line of its own.
  const char* kMarker = "noisescope-said-this-and-nobody-else";
  unit.scripted.answers["sweep"] = bench::Answer{};
  // A named local: the session keeps a pointer to the caller's program for the length of the run
  // (core/update/scanner_session.h), so a temporary would be gone before the first step.
  const upd::Program program = bench::programOf({{"sweep", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  unit.run([&unit, kMarker](const upd::SessionStep& s) {
    if (s.changed && s.after == Stage::kStep) {
      unit.world.radio.sendLine(std::string("{\"t\":\"sweep\",\"note\":\"") + kMarker + "\"}", 10);
    }
  });
  REQUIRE(unit.session().stage() == Stage::kRestored);

  // It is in what the port printed...
  unit.walter->out.clear();
  const std::string printed = unit.walter->runLine("rak session 10 sweep");
  (void)printed;
  // ... and in nothing this firmware wrote down. The marker went out through printText(), which
  // writes to the port with the ring held shut (S-12).
  CHECK_FALSE(has(unit.walter->runLine("dump"), kMarker));
  CHECK_FALSE(has(unit.walter->runLine("journal"), kMarker));
  CHECK_FALSE(has(unit.walter->runLine("rak session"), kMarker));
  CHECK_FALSE(has(unit.walter->runLine("rak update status"), kMarker));
  // And the journal's own bytes, which is where a payload would be if one had been kept.
  upd::Cursor cursor;
  upd::Entry entry;
  bool more = unit.walter->journal.first(&cursor, &entry);
  while (more) {
    const std::string bytes(reinterpret_cast<const char*>(entry.payload), sizeof entry.payload);
    CHECK_FALSE(has(bytes, "noisescope-said"));
    more = unit.walter->journal.previous(&cursor, &entry);
  }
}
