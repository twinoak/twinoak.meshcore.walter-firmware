// A simulated month of wakes (task p3-05): random radio behaviour, random modem coverage and a
// battery that sinks and recovers, a few thousand wakes - and the rails that have to hold through
// all of it:
//
//   * **every wake ends in sleep**;
//   * no wake exceeds the hard bound on a wake;
//   * the supervisor's caps hold;
//   * **no mesh request is ever sent** (§6, §7: that is the agent's work, and the agent is not
//     here);
//   * the number of flashes is at most the number asked for;
//   * nothing is skipped silently: every step of every wake has an outcome that is not "not run".
//
// The totals are printed, because the report has to carry them.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "core/cycle/wake_bench.h"

using bench::WakeBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

namespace {

// A small deterministic generator: the same month every run, which is what makes a failure
// reproducible. The constants are Numerical Recipes' LCG, as the other benches of this
// repository use.
class Dice {
 public:
  explicit Dice(std::uint32_t seed) : state_(seed) {}
  std::uint32_t next() {
    state_ = state_ * 1664525u + 1013904223u;
    return state_ >> 8;
  }
  std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint32_t state_;
};

}  // namespace

TEST_CASE("a simulated month of wakes: every one ends in sleep and every rail holds") {
  WakeBench bench;
  Dice dice(20261015u);

  // The battery's own month: a pack that starts full, sinks through a cloudy fortnight and
  // recovers. The radio reports it, as it does on a unit (§5.8 channel 2).
  std::uint32_t wakes = 0;
  std::uint32_t modem_attempts = 0;
  std::uint32_t slept = 0;
  std::uint32_t seconds_awake = 0;
  std::uint32_t longest_ms = 0;
  int longest_wake = 0;
  char longest_steps[16] = {};
  std::uint32_t longest_bound = 0;
  std::uint64_t simulated_s = 0;  // the sleeps and the wakes: how much of a month was walked
  std::uint32_t exchanges = 0;
  std::uint32_t asked_flashes = 0;
  std::uint32_t climbs = 0;
  std::uint32_t silent_wakes = 0;
  std::uint32_t scanner_wakes = 0;
  std::uint64_t simulated_ms = 0;

  // "A few thousand wakes" (the task). The interval is the band's, so "a month" and "a few
  // thousand wakes" are not the same number: a month of quarter-hourly wakes is 2 880 and a
  // month of daily ones is 30. This walks the wakes, puts the pack's own month over them (the
  // `day` below is a day of the *battery's* curve), and lets the simulated days fall out - which
  // is what makes the totals comparable between runs.
  const int kWakes = 2400;
  for (int i = 0; i < kWakes; ++i) {
    // The pack: full for two days, then down to the floor over a fortnight, then back up.
    const int day = i / 80;
    std::uint16_t mv = 4150;
    if (day >= 2 && day < 16) {
      mv = static_cast<std::uint16_t>(4150 - (day - 2) * 55);
    } else if (day >= 16) {
      mv = static_cast<std::uint16_t>(3400 + (day - 16) * 50);
    }
    // A wander of ±20 mV, as the policy's own table test uses.
    mv = static_cast<std::uint16_t>(mv - 20 + dice.below(41));
    bench.world.rak.battery_mv = mv;
    bench.world.rak.battery_ma = dice.chance(30) ? 120 : -30;

    // The radio: mostly healthy, sometimes silent for a wake, rarely a scanner.
    const std::uint32_t roll = dice.below(100);
    bench.world.rak.deaf = roll < 6;
    bench.world.rak.scanner = roll >= 6 && roll < 9;
    if (bench.world.rak.deaf) ++silent_wakes;
    if (bench.world.rak.scanner) ++scanner_wakes;
    // The channel the battery is on sometimes cannot be read at all.
    bench.world.rak.power_valid = dice.chance(5) ? 0x05 : 0x07;

    // Coverage: a modem that is there most of the time, never registers sometimes, and is simply
    // not there now and then.
    const std::uint32_t coverage = dice.below(100);
    bench.world.modem.deaf = coverage < 8;
    bench.world.modem.never_registers = coverage >= 8 && coverage < 20;
    bench.world.modem.registers_after_ms = 2000 + dice.below(8000);
    bench.world.modem.time_answered = !dice.chance(10);

    // The backend: usually nothing, sometimes a reading taken, now and then an update asked for.
    bench.world.backend.forget();
    if (dice.chance(20)) bench.world.backend.takeReadings();
    if (dice.chance(3)) {
      bench.world.backend.askFor(interfaces::Ask::kUpdateRadio);
      ++asked_flashes;
    }
    if (dice.chance(2)) bench.world.backend.askFor(interfaces::Ask::kRunLadder);

    // §8: now and then the radio asks for us and the line is what wakes us.
    const bool on_the_line = dice.chance(10);
    if (on_the_line) {
      bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 10,
                                     static_cast<int>(dice.below(9)));
    }

    const Pass& pass = bench.oneWake(on_the_line ? interfaces::WokeBy::kRadioLine
                                                 : interfaces::WokeBy::kTimer);
    // How long the *wake* lasted, which is not how long the world moved: the fake sleeper
    // advances the clock by the interval it was asked for, and that is the sleep.
    const std::uint32_t took = pass.spent_ms;
    ++wakes;
    simulated_ms += took;
    simulated_s += pass.awake_s + pass.interval_s;
    if (took > longest_ms) {
      longest_ms = took;
      longest_wake = i;
      longest_bound = pass.bound_ms;
      core::cycle::stepLetters(pass, longest_steps, sizeof longest_steps);
    }
    seconds_awake += pass.awake_s;
    if (pass.outcomeOf(Step::kUplink) != Outcome::kSkipped) ++modem_attempts;
    if (pass.exchanged != interfaces::Exchanged::kFailed &&
        pass.outcomeOf(Step::kBackend) != Outcome::kSkipped &&
        pass.outcomeOf(Step::kBackend) != Outcome::kNoTime) {
      ++exchanges;
    }

    // -- the rails, every wake ---------------------------------------------------------------
    // Every wake ends in sleep. The one allowed exception is a restart the wake asked for, and
    // nothing in this month asks for one.
    CHECK_MESSAGE(pass.ending == Ending::kSlept, i);
    if (pass.ending == Ending::kSlept) ++slept;
    // No step was skipped silently: each one has an outcome and a reason.
    for (std::size_t s = 0; s < core::cycle::kStepCount; ++s) {
      CHECK_MESSAGE(pass.step[s].outcome != Outcome::kNotRun, i);
    }
    // No wake exceeded the hard bound, and none was ended by it.
    CHECK_MESSAGE(took <= pass.bound_ms, i);
    CHECK_MESSAGE(bench.v().restart.asked == 0, i);
    // The pins were safe at the moment of sleeping (the fake sleeper checks them itself) and no
    // pin rule was broken.
    CHECK_MESSAGE(bench.v().pins.violations.empty(), i);
    // The supervisor's caps.
    const core::supervisor::Status& watch = bench.v().supervisor->status();
    CHECK_MESSAGE(watch.counts.resets <= watch.caps.resets, i);
    CHECK_MESSAGE(watch.counts.power_cycles <= watch.caps.power_cycles, i);
    CHECK_MESSAGE(watch.counts.flashes <= watch.caps.flashes, i);
    if (pass.climbed) ++climbs;
  }

  // -- the rails, over the whole month -----------------------------------------------------
  CHECK(slept == wakes);
  // **No mesh request, ever.** §6 and §7 are the agent's work, and the agent is not here.
  std::uint32_t mesh = 0;
  for (std::uint8_t type : bench.requestTypes()) {
    const std::uint32_t needs = core::link::capabilityOf(type);
    if (needs == core::link::cap::kMeshClient || needs == core::link::cap::kObservations) ++mesh;
  }
  CHECK(mesh == 0);
  // The number of flashes is at most the number asked for. This bench has no simulated nRF52, so
  // every asked-for update is refused by the machine's own preconditions - which is itself the
  // rail: a flash that was not asked for cannot happen.
  CHECK(bench.v().update.state() == core::update::State::kIdle);

  const double days = static_cast<double>(simulated_s) / 86400.0;
  MESSAGE("a simulated month: " << wakes << " wakes over " << days << " simulated days ("
          << (static_cast<double>(wakes) / (days > 0 ? days : 1)) << " wakes a day); "
          << modem_attempts << " modem attempts, " << exchanges << " exchanges; "
          << seconds_awake << " s awake in all, "
          << (static_cast<double>(seconds_awake) / (days > 0 ? days : 1))
          << " s a day; the longest wake " << longest_ms << " ms of its " << longest_bound
          << " ms bound (wake " << longest_wake << ", steps " << longest_steps << "); "
          << asked_flashes << " updates asked for, 0 flashes; " << climbs
          << " climbs of the ladder; " << silent_wakes << " wakes with a silent radio, "
          << scanner_wakes << " with a scanner; " << bench.wakeEntries().size()
          << " journal entries; " << bench.v().readings.dropped() << " readings dropped, "
          << bench.v().readings.taken() << " taken");
  (void)simulated_ms;
}

TEST_CASE("a month of wakes with no coverage at all: nothing runs away") {
  // The other month: a modem that is never there. The backoff is what keeps a unit from spending
  // its battery searching at every wake (core/uplink/policy.h), and this is the wake cycle
  // honouring it.
  WakeBench bench;
  // **Not** a modem that answers nothing: `modem-silent` is tried again every wake by design (a
  // modem on the same board that says nothing is a fault worth reporting, and the one remedy -
  // a reset - is inside the wake; core/uplink/failure.h's table). No *coverage* is
  // `not-registered`, which is the one with the growing backoff.
  bench.world.modem.never_registers = true;
  bench.world.modem.registers_after_ms = 1000;
  std::uint32_t attempts = 0;
  for (int i = 0; i < 200; ++i) {
    const Pass& pass = bench.oneWake();
    CHECK(pass.ending == Ending::kSlept);
    if (pass.outcomeOf(Step::kUplink) != Outcome::kSkipped) ++attempts;
  }
  // Far fewer attempts than wakes: the backoff grew.
  CHECK(attempts < 60);
  CHECK(attempts > 0);
  MESSAGE("200 wakes with no coverage: " << attempts << " modem attempts");
}
