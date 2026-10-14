// The caps (task p2-05): how many times a day the supervisor may reset the radio, cut its rail
// and reflash it - and that the answer survives everything that can happen to the Walter in
// between. "One that does too much power-cycles a radio every minute and drains the battery it
// shares ... is the one a bug produces", so these are the tests that matter most here.
//
// A cap is forgiven only when a whole day can be *shown* to have passed. The three ways the
// showing can fail - a restart, a clock that came back at zero after a deep sleep, a power cut
// that took RTC memory with it - each keep the counts, which is the side to err on.
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/supervisor/supervisor.h"
#include "supervisor_bench.h"

using bench::Rung;
using bench::RungOutcome;
using bench::SupWalter;
using bench::WatchedUnit;
namespace sup = core::supervisor;

namespace {

// A radio that goes quiet, is cured by a reset, and goes quiet again: one climb per call.
void quietAgain(WatchedUnit& unit) {
  unit.world.cured = false;
  unit.world.cure = Rung::kReset;
  unit.world.goSilent();
  unit.walter->ladder();
}

}  // namespace

TEST_CASE("a radio that fails again after every cure reaches each day's cap and no more") {
  WatchedUnit unit;
  unit.walter->beat();  // a battery reading to remember

  // Twelve incidents inside one day, every one of them curable by a reset.
  for (int i = 0; i < 12; ++i) quietAgain(unit);

  // Six resets, not twelve: the cap. After it, the climbs go on to rung 3 and then rung 4, and
  // each of those has a cap of its own.
  CHECK(unit.status().counts.resets == sup::kResetsPerDay);
  CHECK(unit.status().counts.power_cycles == sup::kPowerCyclesPerDay);
  CHECK(unit.status().counts.flashes == 0);  // the chip holds the golden image: nothing to flash
  // And what the radio actually got is what the counts say (the fake pins counted it).
  CHECK(unit.walter->pins.reset_pulses == static_cast<int>(sup::kResetsPerDay));
  CHECK(unit.walter->pins.kill_cycles == static_cast<int>(sup::kPowerCyclesPerDay));
  CHECK(unit.walter->pins.violations.empty());

  // Every refusal is itself reported: in the journal, and in the incident it belongs to.
  int capped_resets = 0;
  int capped_cycles = 0;
  for (const sup::Payload& p : unit.walter->story()) {
    if (p.what != sup::Wrote::kRung) continue;
    if (p.outcome != static_cast<std::uint8_t>(RungOutcome::kRefusedCap)) continue;
    if (p.rung == Rung::kReset) ++capped_resets;
    if (p.rung == Rung::kPowerCycle) ++capped_cycles;
  }
  CHECK(capped_resets > 0);
  CHECK(capped_cycles > 0);
  CHECK(unit.incident(0).rung[1] == RungOutcome::kRefusedCap);
  CHECK(unit.status().stop_reason != sup::StopReason::kNone);
}

TEST_CASE("the counts survive a restart of the Walter and a wake from deep sleep") {
  WatchedUnit unit;
  unit.walter->beat();
  for (int i = 0; i < 3; ++i) quietAgain(unit);
  REQUIRE(unit.status().counts.resets == 3);

  // A restart: RTC memory is kept, so the counts and their window are exactly where they were.
  unit.reboot();
  CHECK(unit.status().counts.resets == 3);
  CHECK_FALSE(unit.status().counts_from_journal);
  CHECK_FALSE(unit.status().window_reanchored);
  for (int i = 0; i < 3; ++i) quietAgain(unit);
  CHECK(unit.status().counts.resets == sup::kResetsPerDay);

  // A wake from deep sleep, on a Walter with no time of day - which is every Walter until phase
  // 3's modem reads one from the network (interfaces/wall_clock.h). ESP-IDF's monotonic clock
  // restarts at zero, so the window's age cannot be measured against the record any more. The
  // worst is assumed: the window begins again here and the counts stay, so a sleep forgives
  // nothing.
  unit.world.u.store.wall.forget();
  const int spent = unit.world.reset_pulses_seen;
  unit.world.deepSleepWake();
  unit.reboot(core::health::ResetReason::kDeepSleepWake);
  CHECK(unit.status().counts.resets == sup::kResetsPerDay);
  CHECK(unit.status().window_reanchored);
  unit.world.u.clock.advance(sup::kWalterPowerUpGraceMs + 1);
  quietAgain(unit);
  CHECK(unit.status().counts.resets == sup::kResetsPerDay);  // still capped
  CHECK(unit.world.reset_pulses_seen == spent);              // and no reset reached the radio
}

TEST_CASE("a power cut takes the retained record, and the counts come back from the journal") {
  WatchedUnit unit;
  unit.walter->beat();
  for (int i = 0; i < 6; ++i) quietAgain(unit);
  REQUIRE(unit.status().counts.resets == sup::kResetsPerDay);

  unit.powerCut();  // RTC memory gone; the journal is in flash and is not
  CHECK(unit.status().counts.resets == sup::kResetsPerDay);
  CHECK(unit.status().counts_from_journal);
  // And the cap still holds: the reset the next climb would have asked for is refused.
  const int before = unit.walter->pins.reset_pulses;
  quietAgain(unit);
  CHECK(unit.walter->pins.reset_pulses == before);
  CHECK(unit.incident(0).rung[1] == RungOutcome::kRefusedCap);
}

TEST_CASE("the day ends and the caps are new again") {
  WatchedUnit unit;
  unit.walter->beat();
  for (int i = 0; i < 8; ++i) quietAgain(unit);
  REQUIRE(unit.status().counts.resets == sup::kResetsPerDay);
  const int resets_in_the_first_day = unit.walter->pins.reset_pulses;

  // A whole day after the window began, and the counts are zero: a new day, new caps. The
  // window is dated here (the bench has a time of day), and a date is what governs: a monotonic
  // clock that has run on for a day while the time of day stood still proves nothing.
  unit.world.u.store.wall.give(unit.status().window_began_epoch + sup::kCapWindowS + 1);
  unit.world.u.clock.advance(sup::kCapWindowMs);
  quietAgain(unit);
  CHECK(unit.status().counts.resets == 1);
  CHECK(unit.walter->pins.reset_pulses == resets_in_the_first_day + 1);

  // Not a minute before the day was over, though.
  WatchedUnit tight;
  tight.walter->beat();
  for (int i = 0; i < 8; ++i) quietAgain(tight);
  const int spent = tight.walter->pins.reset_pulses;
  tight.world.u.store.wall.give(tight.status().window_began_epoch + sup::kCapWindowS - 60);
  tight.world.u.clock.advance(sup::kCapWindowMs - 60000);
  quietAgain(tight);
  CHECK(tight.status().counts.resets == sup::kResetsPerDay);
  CHECK(tight.walter->pins.reset_pulses == spent);
}

TEST_CASE("a day that can be dated is a day, whatever the Walter's clock did") {
  // With a time of day (phase 3's modem fills interfaces::WallClock; the bench's wall clock has
  // one), the window is dated, so a restart that loses the monotonic clock does not stop the
  // supervisor from knowing that a day has passed.
  WatchedUnit unit;
  REQUIRE(unit.walter->beat().after == sup::State::kIdle);
  for (int i = 0; i < 8; ++i) quietAgain(unit);
  REQUIRE(unit.status().counts.resets == sup::kResetsPerDay);
  REQUIRE(unit.status().window_began_epoch != 0);

  // A day later by the wall clock, and the Walter asleep in between (its own clock back to zero).
  unit.world.u.store.wall.give(unit.status().window_began_epoch + sup::kCapWindowS + 1);
  unit.world.deepSleepWake();
  unit.reboot(core::health::ResetReason::kDeepSleepWake);
  CHECK(unit.status().counts.resets == 0);  // the day is over, and it can be shown
  unit.world.u.clock.advance(sup::kWalterPowerUpGraceMs + 1);
  quietAgain(unit);
  CHECK(unit.status().counts.resets == 1);
}

TEST_CASE("rak watch reset-caps empties the day and lifts a stop") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  unit.walter->ladder();
  REQUIRE(unit.status().stopped);
  REQUIRE(unit.status().counts.resets == 1);

  unit.walter->supervisor.resetCaps();
  CHECK(unit.status().counts.resets == 0);
  CHECK(unit.status().counts.power_cycles == 0);
  CHECK(unit.status().counts.flashes == 0);
  CHECK_FALSE(unit.status().stopped);
  // Journalled, so that a person emptying the counts is in the story like everything else.
  bool journalled = false;
  for (const sup::Payload& p : unit.walter->story()) {
    if (p.what == sup::Wrote::kCapsReset && (p.flags & sup::kFlagByHand) != 0) journalled = true;
  }
  CHECK(journalled);
  // And it survives a restart.
  unit.reboot();
  CHECK(unit.status().counts.resets == 0);
  CHECK_FALSE(unit.status().stopped);
}

TEST_CASE("the flash cap: the golden image goes back once a day and no more") {
  WatchedUnit unit;
  unit.walter->beat();
  // A radio whose chip keeps losing its application: the first incident reflashes it, the second
  // finds the day's one flash already spent.
  for (int i = 0; i < 2; ++i) {
    unit.world.cured = false;
    unit.world.cure = Rung::kNone;
    unit.world.wipeApplication();
    unit.walter->ladder();
    if (i == 0) {
      CHECK(unit.status().counts.flashes == 1);
      CHECK(unit.world.u.chipHolds(unit.golden));
    }
  }
  CHECK(unit.status().counts.flashes == sup::kFlashesPerDay);
  CHECK(unit.status().stopped);
  CHECK(unit.status().stop_reason == sup::StopReason::kCapReached);
  CHECK(unit.incident(0).rung[3] == RungOutcome::kRefusedCap);
}
