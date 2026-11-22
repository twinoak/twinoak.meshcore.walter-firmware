// The session of 21 November (task p18-01): the first NoiseScope session on a unit measured
// everything it was asked to and ended `stuck`.
//
//   22:30:38  flashing-golden: the golden image is going back
//   22:39:52  booting: the radio was reset: waiting for it to answer
//   22:39:54  proving: the golden image's health check
//   22:40:53  stuck: the radio restarted inside the proving period
//
// An hour later the radio said `uptime 2570 s, booted: 3 (watchdog)` and was healthy. The scanner
// starts the nRF52840's hardware watchdog at 60 s and feeds it from its own loop; the repeater
// feeds nothing; and a soft reset - which is what the flasher used to leave by - is not among the
// reset sources that clear a watchdog (the Product Specification's "Reset behavior" table; the
// rules are in host_test/sim/nrf52_chip.h and are held by
// host_test/core/nrf52/test_nrf52_chip_watchdog.cpp). So the watchdog the scanner started reset
// the repeater 59 s into the proving period, and a restart there is a failure by rule.
//
// This file is that session, end to end, on the bench: the scanner image flashed and its watchdog
// started, six steps' worth of program, the golden image back - and the proof passing, because
// the flash is now left by a pulse on the reset line, which clears the watchdog. Put flasher.cpp's
// exit back to its SYSRESETREQ and this file fails: the session ends `stuck` instead, with
// `rolling-back (proving the golden image) -> stuck: the radio restarted inside the proving
// period` where `restored` is.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/update/scanner_session.h"
#include "session_bench.h"

using bench::Ending;
using bench::SessionUnit;
using bench::Stage;
using bench::programOf;
namespace upd = core::update;

namespace {

bool holds(const std::vector<Stage>& stages, Stage wanted) {
  for (Stage stage : stages) {
    if (stage == wanted) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("the session of 21 November: the scanner's watchdog does not outlive the scanner, and the session ends restored") {
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep 863 870 25", 30}, {"hist 869.4 869.8 25", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  // Before the first flash the radio is the repeater, and nothing has started a watchdog.
  CHECK_FALSE(unit.world.chip.watchdogRunning());

  bool watchdog_while_scanning = false;
  bool watchdog_at_the_golden_flash = false;
  const Stage end = unit.run([&](const upd::SessionStep& step) {
    if (unit.world.radio.scanner && unit.world.chip.watchdogRunning()) watchdog_while_scanning = true;
    // The moment the fault was made: the golden image is about to be written over a radio whose
    // watchdog is running, and whatever starts that image decides whether it survives.
    if (step.after == Stage::kFlashingGolden && unit.world.chip.watchdogRunning()) {
      watchdog_at_the_golden_flash = true;
    }
  });
  INFO(unit.stageText());

  // The scanner's watchdog was there, and it was still there when the golden image went back.
  CHECK(watchdog_while_scanning);
  CHECK(watchdog_at_the_golden_flash);

  // And the session ended as 21 November's should have.
  CHECK(end == Stage::kRestored);
  CHECK(unit.session().status().ending == Ending::kRestored);
  const std::vector<Stage> journalled = unit.journalledStages();
  CHECK(journalled == std::vector<Stage>{Stage::kCheck, Stage::kFlashingScanner, Stage::kScanner,
                                         Stage::kStep, Stage::kStep, Stage::kRoom,
                                         Stage::kFlashingGolden, Stage::kBooting, Stage::kProving,
                                         Stage::kRestored});
  CHECK_FALSE(holds(journalled, Stage::kStuck));
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
  CHECK(mark.stage == Stage::kRestored);

  // No restart in the proving period, and the period really was the three minutes a watchdog's
  // 60 s falls inside: that is what the pulse bought.
  CHECK(unit.walter->update.state() == bench::State::kDone);
  CHECK(unit.walter->update.status().reason != bench::Reason::kRestarted);
  CHECK(unit.walter->update.status().prove_elapsed_ms >= upd::kProvingPeriodMs);

  // The watchdog went with the reset that started the golden image, and never fired.
  CHECK(unit.world.chip.resets().watchdog == 0);
  CHECK_FALSE(unit.world.chip.watchdogRunning());
  // Two flashes, two resets of the radio, both of them pulses on the reset line and neither a
  // SYSRESETREQ (task p18-01).
  CHECK(unit.world.chip.resets().pin == 2);
  CHECK(unit.world.chip.counters().aircr_resets == 0);
  CHECK(unit.world.chip.resets().sysresetreq == 0);

  // The repeater is back, running the golden image, and the store never moved.
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  CHECK(unit.walter->client.hello().ok());
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(unit.world.chip.counters().tripwire == 0);
}

TEST_CASE("a scanner image flashed again over a scanner: one watchdog, and the pulse clears it each time") {
  SessionUnit unit;
  const upd::Program program = programOf({{"sweep 863 870 25", 10}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  REQUIRE(unit.run() == Stage::kRestored);
  CHECK(unit.world.chip.resets().watchdog == 0);
  CHECK_FALSE(unit.world.chip.watchdogRunning());

  // A second session over the same radio: the scanner goes on again, starts a watchdog again, and
  // the switch back clears that one too. Two sessions, four flashes, four pulses, no watchdog
  // reset and no rollback.
  bench::RecordingSink second_sink;
  second_sink.clock = &unit.world.clock;
  REQUIRE(unit.session().start(program, bench::Slot{0}, &second_sink));
  REQUIRE(unit.run() == Stage::kRestored);
  CHECK(unit.world.chip.resets().watchdog == 0);
  CHECK_FALSE(unit.world.chip.watchdogRunning());
  CHECK(unit.world.chip.resets().pin == 4);
  CHECK(unit.world.chip.counters().aircr_resets == 0);
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK(unit.world.store.store.badCount() == 0);
}
