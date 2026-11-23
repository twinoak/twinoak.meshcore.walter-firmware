// **Cured by a reset pulse, or came back by itself** (task p18-04, deliverable 6).
//
// On 21 November the supervisor at Bellevue wrote *the radio went quiet, cured by rung 2, a reset
// pulse; lasted 71 s* - and the radio's own hourly `uptimeS` from 20 November onwards points at
// one boot, 15 November 21:49Z, with no restart anywhere near that pulse
// (tasks/phase-18/FOLLOWUPS.md, *A reset pulse that restarted nothing*). The pulse cured nothing;
// the radio came back on its own, and the word the unit reported was wrong.
//
// So the rung is given the word only when a new boot answered it, by an EV_BOOT or by a boot id
// that is not the one from before the pulse - and either way the observation goes into the reset
// line's own record (core/board/radio_pins.h, ResetLineSeen), where `rak watch` and the check-in's
// report read it. The ladder's order and its caps do not change: this is what a rung is called,
// not when it is climbed.
#include <doctest.h>

#include <string>

#include "supervisor_bench.h"

using bench::Outcome;
using bench::Rung;
using bench::RungOutcome;
using bench::WatchedUnit;
namespace sup = core::supervisor;
namespace board = core::board;

TEST_CASE("the radio restarts at the pulse: cured by rung 2, and the line is seen to work") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  unit.world.cure = Rung::kReset;

  CHECK(unit.walter->ladder() == sup::State::kIdle);
  CHECK(unit.incident().outcome == Outcome::kCured);
  CHECK(unit.incident().cured_at == Rung::kReset);
  CHECK(unit.incident().rung[1] == RungOutcome::kCured);
  CHECK(unit.walter->pins.reset_pulses == 1);
  CHECK(unit.world.reset_pulses_seen == 1);

  // And what the unit now remembers of its reset line.
  CHECK(unit.walter->board.resetLine().seen == board::ResetLineSeen::kRestarts);

  const std::string watch = unit.walter->runLine("rak watch");
  INFO(watch);
  CHECK(watch.find("a pulse was seen to restart the radio") != std::string::npos);
  CHECK(watch.find("cured by rung 2, a reset pulse") != std::string::npos);
}

TEST_CASE("the radio comes back without restarting: the pulse restarted nothing, and the line is seen not to") {
  WatchedUnit unit;
  unit.walter->beat();
  // 21 November at Bellevue: the radio goes quiet, the pulse is made and reaches nothing, and the
  // radio answers again at the moment the ladder next asks - of its own accord.
  unit.world.reset_line_reaches_the_radio = false;
  unit.world.u.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kEnabledNotWired);
  unit.world.goSilent();
  unit.world.cure = Rung::kReset;

  CHECK(unit.walter->ladder() == sup::State::kIdle);
  // The incident is cured - the radio answers - but **not by the rung**: the pulse was made and
  // the radio's boot is the boot it was in before it.
  CHECK(unit.incident().outcome == Outcome::kCured);
  CHECK(unit.incident().cured_at == Rung::kReset);
  CHECK(unit.incident().rung[1] == RungOutcome::kCameBack);
  CHECK(unit.walter->pins.reset_pulses == 1);
  CHECK(unit.world.u.chip.resets().pin_lost == 1);
  CHECK(unit.world.u.chip.resets().pin == 0);
  // The ladder stopped there all the same: the rungs above it were not used.
  CHECK(unit.walter->pins.kill_cycles == 0);
  CHECK(unit.status().counts.resets == 1);

  CHECK(unit.walter->board.resetLine().seen == board::ResetLineSeen::kDead);

  const std::string watch = unit.walter->runLine("rak watch");
  INFO(watch);
  CHECK(watch.find("a pulse was seen to restart nothing") != std::string::npos);
  CHECK(watch.find("by itself (rung 2, a reset pulse restarted nothing)") != std::string::npos);
  CHECK(watch.find("the radio came back without restarting: not this rung's doing") != std::string::npos);
}

TEST_CASE("a radio that never answered this supervisor: no word is given, and nothing is written down") {
  // Nothing of the radio's boot was ever known, so whether the pulse restarted it cannot be told
  // - and a guess is worse than nothing. The incident is cured at rung 2 as it always was.
  WatchedUnit unit;
  // Nothing of the radio has ever been heard by this supervisor: not a PING, and not even the
  // EV_BOOT its boot queued - a Walter that came up after the radio did, and found it quiet.
  unit.world.u.radio.forgetPending();
  unit.world.goSilent();
  unit.world.cure = Rung::kReset;
  unit.world.reset_line_reaches_the_radio = false;
  unit.world.u.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kEnabledNotWired);

  CHECK(unit.walter->ladder() == sup::State::kIdle);
  CHECK(unit.incident().outcome == Outcome::kCured);
  CHECK(unit.incident().cured_at == Rung::kReset);
  CHECK(unit.walter->board.resetLine().seen == board::ResetLineSeen::kUntried);
}

TEST_CASE("the record of the line outlives the Walter: it is kept where the spacing is kept") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  unit.world.cure = Rung::kReset;
  REQUIRE(unit.walter->ladder() == sup::State::kIdle);
  REQUIRE(unit.walter->board.resetLine().seen == board::ResetLineSeen::kRestarts);

  // A reset of the Walter: RTC memory survives it, and so does what it learned about its line.
  unit.reboot();
  CHECK(unit.walter->board.resetLine().seen == board::ResetLineSeen::kRestarts);

  // A power cut does not: RTC memory is gone, and the unit knows nothing of its line again until
  // the next flash and the next pulse say something (interfaces/retained.h).
  unit.powerCut();
  CHECK(unit.walter->board.resetLine().seen == board::ResetLineSeen::kUntried);
  CHECK_FALSE(unit.walter->board.resetLine().uicr_read);
}

TEST_CASE("rung 2 is still rung 2: a pulse that restarts nothing and does not cure climbs on") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.reset_line_reaches_the_radio = false;
  unit.world.u.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kEnabledNotWired);
  unit.world.goSilent();
  unit.world.cure = Rung::kPowerCycle;  // the rail is what brings it back

  CHECK(unit.walter->ladder() == sup::State::kIdle);
  CHECK(unit.incident().cured_at == Rung::kPowerCycle);
  CHECK(unit.incident().rung[1] == RungOutcome::kDone);  // rung 2 was done and did not cure
  CHECK(unit.incident().rung[2] == RungOutcome::kCured);
  CHECK(unit.walter->pins.reset_pulses == 1);
  CHECK(unit.walter->pins.kill_cycles == 1);
  // A pulse that cured nothing says nothing about the line either: the radio was silent before it
  // and silent after it, which is not evidence of what the pulse did.
  CHECK(unit.walter->board.resetLine().seen == board::ResetLineSeen::kUntried);
}
