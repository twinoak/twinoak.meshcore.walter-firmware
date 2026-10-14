// The console's three supervisor commands (task p2-05; docs/console.md, docs/supervisor.md):
// `rak watch`, which only ever reads - looking at the supervisor must not be able to reset a
// radio - `rak watch reset-caps`, and `rak ladder`, which climbs and prints each rung.
#include <doctest.h>

#include <string>

#include "core/console/parser.h"
#include "core/supervisor/supervisor.h"
#include "supervisor_bench.h"

using bench::ReadyUnit;
using bench::Rung;
using bench::WatchedUnit;
namespace sup = core::supervisor;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

}  // namespace

TEST_CASE("the parser takes the three lines and refuses the rest") {
  using core::console::parseLine;
  using core::console::Verb;
  CHECK(parseLine("rak watch").command.verb == Verb::kRakWatch);
  CHECK(parseLine("rak watch reset-caps").command.verb == Verb::kRakWatchResetCaps);
  CHECK(parseLine("rak ladder").command.verb == Verb::kRakLadder);
  CHECK(parseLine("rak @1 watch").command.slot == 1);

  CHECK_FALSE(parseLine("rak watch resetcaps").ok);
  CHECK(std::string(parseLine("rak watch resetcaps").error).find("reset-caps") != std::string::npos);
  CHECK_FALSE(parseLine("rak watch reset-caps now").ok);
  CHECK_FALSE(parseLine("rak ladder now").ok);
}

TEST_CASE("rak watch on a healthy unit reads and does nothing") {
  WatchedUnit unit;
  unit.walter->beat();
  const std::string text = unit.walter->runLine("rak watch");
  CHECK(has(text, "rak watch: slot 0: the radio is healthy"));
  CHECK(has(text, "healthy since"));
  CHECK(has(text, "today: resets 0/6, power cycles 0/3, golden-image flashes 0/1"));
  CHECK(has(text, "battery:"));
  CHECK(has(text, "no incident has been recorded on this unit"));

  // It stepped nothing: a silent radio looked at a hundred times is never reset by the looking.
  unit.world.goSilent();
  for (int i = 0; i < 50; ++i) {
    unit.world.u.clock.advance(sup::kHeartbeatIntervalMs);
    unit.walter->runLine("rak watch");
  }
  CHECK(unit.walter->pins.reset_pulses == 0);
  CHECK(unit.walter->pins.kill_cycles == 0);
  CHECK(unit.walter->supervisorEntries() == 0);
  CHECK(unit.walter->supervisor.incidentCount() == 0);
}

TEST_CASE("rak ladder climbs, prints each rung, and rak watch then has the incident") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  unit.world.cure = Rung::kPowerCycle;

  const std::string climbed = unit.walter->runLine("rak ladder");
  CHECK(has(climbed, "rak ladder: slot 0: asking the radio"));
  CHECK(has(climbed, "rung 1, ask again later"));
  CHECK(has(climbed, "rung 2, a reset pulse"));
  CHECK(has(climbed, "rung 3, a power cycle"));
  CHECK_FALSE(has(climbed, "rung 4, what is on the chip"));  // it came back at rung 3
  CHECK(has(climbed, "the radio is back after rung 3, a power cycle"));

  const std::string watched = unit.walter->runLine("rak watch");
  CHECK(has(watched, "today: resets 1/6, power cycles 1/3, golden-image flashes 0/1"));
  CHECK(has(watched, "the last 1 incidents, newest first"));
  CHECK(has(watched, "the radio went quiet; cured by rung 3, a power cycle"));
  CHECK(has(watched, "rungs tried: 1, 2, 3"));
}

TEST_CASE("rak ladder on a healthy radio says so and climbs nothing") {
  WatchedUnit unit;
  const std::string text = unit.walter->runLine("rak ladder");
  CHECK(has(text, "rak ladder: the radio is healthy: nothing to climb"));
  CHECK(unit.walter->pins.reset_pulses == 0);
  CHECK(unit.walter->pins.kill_cycles == 0);
  CHECK(unit.walter->supervisor.incidentCount() == 0);
}

TEST_CASE("rak ladder on a radio running the scanner leaves it alone") {
  WatchedUnit unit;
  unit.world.runScanner();  // a session that has never heard a frame from it (§9)
  const std::string text = unit.walter->runLine("rak ladder");
  CHECK(has(text, "the radio is running the scanner (NoiseScope): not a fault, left alone"));
  CHECK(unit.walter->pins.reset_pulses == 0);
}

TEST_CASE("a stop is printed by rak watch, at the boot, and lifted by reset-caps") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  REQUIRE(unit.walter->ladder() == sup::State::kStopped);

  const std::string stopped = unit.walter->runLine("rak watch");
  // The whole view, in the test log: this is the incident record as a check-in would carry it,
  // and what docs/supervisor.md's example is taken from.
  MESSAGE("rak watch after a climb that stopped:\n" << stopped);
  CHECK(has(stopped, "STOPPED: the chip holds the golden image"));
  CHECK(has(stopped, "rungs tried: 1, 2, 3, 4"));
  CHECK(has(stopped, "or until 'rak watch reset-caps', 'rak ladder' or the server"));
  CHECK(has(stopped, "stopped"));

  // The next boot says it without being asked: a unit found quiet should announce it.
  unit.walter->out.clear();
  unit.reboot();
  CHECK(has(unit.walter->out.text, "supervisor: STOPPED:"));

  const std::string lifted = unit.walter->runLine("rak watch reset-caps");
  CHECK(has(lifted, "the day's counts are empty again and the stop is lifted"));
  CHECK_FALSE(unit.status().stopped);
  CHECK_FALSE(has(unit.walter->runLine("rak watch"), "STOPPED"));
}

TEST_CASE("the boot line says what the day has already spent") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.cure = Rung::kReset;
  unit.world.goSilent();
  REQUIRE(unit.walter->ladder() == sup::State::kIdle);
  REQUIRE(unit.status().counts.resets == 1);

  unit.walter->out.clear();
  unit.reboot();
  CHECK(has(unit.walter->out.text, "supervisor: today already: 1 resets, 0 power cycles, 0 flashes"));
  // And after a power cut it says where the numbers came from.
  unit.walter->out.clear();
  unit.powerCut();
  CHECK(has(unit.walter->out.text, "read back from the journal"));
}

TEST_CASE("a console with no supervisor answers in one line") {
  ReadyUnit unit;  // task p2-03's bench: a console built without one
  const std::string text = unit.walter->runLine("rak watch");
  CHECK(has(text, "error: there is no supervisor in this build"));
  CHECK(has(unit.walter->runLine("rak ladder"), "error: there is no supervisor in this build"));
  CHECK(has(unit.walter->runLine("rak watch reset-caps"), "error: there is no supervisor in this build"));
}

TEST_CASE("help names the three commands") {
  WatchedUnit unit;
  const std::string text = unit.walter->runLine("help");
  CHECK(has(text, "rak watch"));
  CHECK(has(text, "rak watch reset-caps"));
  CHECK(has(text, "rak ladder"));
}
