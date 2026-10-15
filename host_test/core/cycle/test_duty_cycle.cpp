// The duty cycle over the whole bench (task p3-04): a **simulated month of wakes** with a modem
// that drops, a radio that goes quiet and a battery that sinks, in which nothing runs twice,
// nothing runs unbounded and every rail holds (the phase's own exit criterion,
// tasks/phase-3/README.md); and a week with the radio silent.
//
// This is the test the phase asks for, so it is written to notice the failures and not the good
// path: every wake is counted, every sleep's plan is checked, the pins are checked at the moment
// of sleeping by the fake sleeper, and the pin rules are checked at every transition by the fake
// pins.
#include <doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "core/cycle/duty_cycle.h"
#include "cycle_bench.h"

using bench::CycleBench;
using core::cycle::SleepRefusal;
using core::cycle::Wake;
using core::power::Band;

TEST_CASE("a simulated month of wakes: a sinking battery, a radio that goes quiet, a modem that drops") {
  CycleBench b;
  // A month, run as wakes rather than minutes: each wake is a boot, so the clock is the bench's
  // and the time between wakes is what the sleeper advances it by.
  constexpr int kDays = 30;
  std::uint16_t mv = 4150;
  int wakes = 0;
  int modem_wakes = 0;
  int attention_wakes = 0;
  int spurious_wakes = 0;
  int quiet_wakes = 0;
  int sleeps = 0;
  int refusals = 0;
  int journal_entries = 0;
  std::uint64_t simulated_s = 0;
  std::vector<int> bands(static_cast<std::size_t>(Band::kCount), 0);

  // Where the radio goes quiet for a day, and where the modem refuses to come up.
  constexpr std::uint64_t kQuietFrom = 10ull * 24 * 3600;
  constexpr std::uint64_t kQuietTo = 11ull * 24 * 3600;

  while (simulated_s < static_cast<std::uint64_t>(kDays) * 24 * 3600) {
    const bool quiet = simulated_s >= kQuietFrom && simulated_s < kQuietTo;
    // Every tenth wake is one the radio asked for; every fifteenth is the line twitching for
    // nothing. The rest are the timer.
    interfaces::WokeBy woke = interfaces::WokeBy::kTimer;
    if (!quiet && wakes % 10 == 9) {
      b.world.rak.raiseAttention(core::link::attention::kObsRingNearlyFull, 3, 2);
      woke = interfaces::WokeBy::kRadioLine;
    } else if (!quiet && wakes % 15 == 14) {
      b.world.rak.sendBytes({0x00, 0x00, 0x3c, 0x3c}, 0);
      woke = interfaces::WokeBy::kRadioLine;
    }

    b.boot(woke);
    b.world.rak.deaf = quiet;
    b.world.rak.battery_mv = mv;
    b.world.rak.battery_ma = -25;
    b.v().begin();
    ++wakes;

    // The wake's own work, in the order docs/ARCHITECTURE.md §10 gives it. Nothing here invents
    // a wake cycle: that is task p3-05's. What is exercised is that each of these is bounded and
    // that the way out is always the same one.
    const bool had_reading = b.duty().readPower();
    if (!had_reading) ++quiet_wakes;
    const core::power::Decision d = b.duty().decide();
    ++bands[static_cast<std::size_t>(d.band)];
    switch (b.wake().wake) {
      case Wake::kAttention: ++attention_wakes; break;
      case Wake::kSpurious: ++spurious_wakes; break;
      default: break;
    }

    // The modem, when the band allows it in this wake. A modem that drops: the fake answers
    // nothing from the twentieth wake on, so the policy's own backoff takes over - and the band
    // still decides whether it is tried at all.
    if (d.modem_this_wake) {
      ++modem_wakes;
      b.v().w.modem.deaf = wakes >= 20;
      (void)b.v().uplink.bringUp(core::uplink::kWakeBudgetMs);
      b.duty().modemWasUsed();
    } else {
      b.duty().modemWasSkipped();
    }

    // The way out.
    const core::cycle::SleepResult r = b.duty().sleepNow();
    if (r.refusal != SleepRefusal::kNone) {
      ++refusals;
      break;  // a refusal in this test would be a bug: nothing here is updating
    }
    ++sleeps;
    if (r.journalled) ++journal_entries;
    // The pads are held and the plan is the band's.
    CHECK(r.pins_held);
    CHECK(r.seconds == d.interval_seconds);
    CHECK(b.v().sleeper.last().seconds == d.interval_seconds);
    CHECK(b.v().sleeper.last().radio_line == r.radio_line_armed);
    // **Nothing runs unbounded**: a wake that did its whole work is minutes, not hours.
    CHECK_MESSAGE(r.awake_s < core::cycle::kWakeBoundMs / 1000, r.awake_s);

    simulated_s += r.seconds;
    // The pack: the baseline drain plus what this wake cost, and a little sun every day.
    const std::uint32_t drain = 2 + (d.modem_this_wake ? 6u : 0u) + r.seconds / 3600;
    mv = mv > drain + 3000 ? static_cast<std::uint16_t>(mv - drain) : 3000;
    if ((simulated_s / 3600) % 24 == 12) mv = static_cast<std::uint16_t>(mv + 8);
  }

  std::ostringstream said;
  said << "a simulated month: " << wakes << " wakes over " << simulated_s / 86400
       << " days, " << sleeps << " sleeps, " << modem_wakes << " modem wakes, "
       << attention_wakes << " attention wakes, " << spurious_wakes << " spurious, "
       << quiet_wakes << " with no battery reading, " << journal_entries
       << " journal entries; bands: plenty " << bands[static_cast<std::size_t>(Band::kPlenty)]
       << ", normal " << bands[static_cast<std::size_t>(Band::kNormal)] << ", saving "
       << bands[static_cast<std::size_t>(Band::kSaving)] << ", critical "
       << bands[static_cast<std::size_t>(Band::kCritical)] << ", no-reading "
       << bands[static_cast<std::size_t>(Band::kUnknown)] << "; ending at " << mv << " mV";
  MESSAGE(said.str());

  // **Nothing ran twice and nothing was refused**: every wake ended in exactly one sleep.
  CHECK(refusals == 0);
  CHECK(sleeps == wakes);
  CHECK(static_cast<int>(b.v().sleeper.sleeps) == 1);  // one sleep in the *last* Walter's life
  // Every band was visited, which is what a sinking pack does.
  CHECK(bands[static_cast<std::size_t>(Band::kPlenty)] > 0);
  CHECK(bands[static_cast<std::size_t>(Band::kCritical)] > 0);
  // The radio's quiet day was seen as "no reading", and never as `plenty`.
  CHECK(quiet_wakes > 0);
  CHECK(bands[static_cast<std::size_t>(Band::kUnknown)] >= quiet_wakes);
  // The line twitched and was heard, and the attentions were heard too.
  CHECK(attention_wakes > 0);
  CHECK(spurious_wakes > 0);
  // **The journal did not fill**: this is the whole reason an unremarkable sleep is not written
  // down. A month of wakes writes a handful of entries, not a thousand (the ring is 1024).
  CHECK_MESSAGE(journal_entries < 64, journal_entries);
  // And the pin rules held at every transition of every wake, which the fake pins check
  // themselves; this is the count.
  CHECK(b.v().pins.violations.empty());
}

TEST_CASE("a week with the radio silent: the band is never plenty and the interval is medium") {
  CycleBench b;
  b.world.rak.deaf = true;
  int wakes = 0;
  int plenty = 0;
  std::uint64_t simulated_s = 0;
  while (simulated_s < 7ull * 24 * 3600) {
    b.boot(interfaces::WokeBy::kTimer);
    b.world.rak.deaf = true;
    b.v().begin();
    ++wakes;
    CHECK_FALSE(b.duty().readPower());
    const core::power::Decision d = b.duty().decide();
    if (d.band == Band::kPlenty) ++plenty;
    CHECK(d.band == Band::kUnknown);
    CHECK(d.interval_minutes == core::power::kUnknownMinutes);
    CHECK_FALSE(d.may.update);
    const core::cycle::SleepResult r = b.duty().sleepNow();
    REQUIRE(r.refusal == SleepRefusal::kNone);
    // The notice could not be sent - there is nobody to send it to - and that is not a reason to
    // stay awake.
    CHECK_FALSE(r.notice_sent);
    simulated_s += r.seconds;
  }
  CHECK(plenty == 0);
  CHECK(wakes == 7 * 24 * 60 / static_cast<int>(core::power::kUnknownMinutes));
  MESSAGE("a week with the radio silent: " << wakes << " wakes, band no-reading throughout");
}

TEST_CASE("a month of faults: the sleep lengthens, and the journal says so") {
  CycleBench b;
  std::uint32_t previous = 0;
  for (int i = 0; i < 6; ++i) {
    // Every wake ends in a panic: the next one is longer, not shorter.
    b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic);
    b.world.rak.battery_mv = 4100;
    b.v().begin();
    REQUIRE(b.wake().wake == Wake::kCrash);
    CHECK(b.wake().faults_in_a_row == static_cast<std::uint32_t>(i + 1));
    REQUIRE(b.duty().readPower());
    const core::power::Decision d = b.duty().decide();
    CHECK(d.band == Band::kPlenty);
    CHECK_MESSAGE(d.interval_minutes >= previous, i);
    if (i > 0) CHECK_MESSAGE(d.interval_minutes > core::power::kPlentyMinutes, i);
    previous = d.interval_minutes;
    const core::cycle::SleepResult r = b.duty().sleepNow();
    REQUIRE(r.refusal == SleepRefusal::kNone);
    // A fault is always news.
    CHECK(r.journalled);
  }
  CHECK(previous >= core::power::kPlentyMinutes * 8);
  // A good wake ends the run.
  b.boot(interfaces::WokeBy::kTimer);
  b.world.rak.battery_mv = 4100;
  b.v().begin();
  CHECK(b.wake().faults_in_a_row == 0);
  REQUIRE(b.duty().readPower());
  CHECK(b.duty().decide().interval_minutes == core::power::kPlentyMinutes);
}

TEST_CASE("wake.interval overrides three bands and never critical, over the real settings store") {
  CycleBench b;
  char err[96] = {};
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.v().settings.put(core::settings::def(core::settings::Key::kWakeInterval), "45", err,
                             sizeof err));
  b.duty().settingsChanged();
  // `plenty`: the setting wins.
  b.world.rak.battery_mv = 4100;
  REQUIRE(b.duty().readPower());
  core::power::Decision d = b.duty().decide();
  CHECK(d.band == Band::kPlenty);
  CHECK(d.overridden);
  CHECK(d.interval_minutes == 45);
  CHECK(b.duty().sleepNow().seconds == 45 * 60);

  // `critical`: it does not.
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  b.world.rak.battery_mv = 3200;
  REQUIRE(b.duty().readPower());
  d = b.duty().decide();
  CHECK(d.band == Band::kCritical);
  CHECK_FALSE(d.overridden);
  CHECK(d.interval_minutes == core::power::kCriticalMinutes);
  CHECK(b.duty().sleepNow().seconds == core::power::kCriticalMinutes * 60);

  // `auto` gives the band its own interval back.
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.v().settings.put(core::settings::def(core::settings::Key::kWakeInterval), "auto", err,
                             sizeof err));
  b.duty().settingsChanged();
  b.world.rak.battery_mv = 4100;
  REQUIRE(b.duty().readPower());
  d = b.duty().decide();
  CHECK_FALSE(d.overridden);
  CHECK(d.interval_minutes == core::power::kPlentyMinutes);
}

TEST_CASE("the band crosses a sleep and not a power cut") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.world.rak.battery_mv = 3500;
  b.v().begin();
  REQUIRE(b.duty().readPower());
  REQUIRE(b.duty().decide().band == Band::kSaving);
  REQUIRE(b.duty().sleepNow().refusal == SleepRefusal::kNone);

  // The next wake knows which side of the hysteresis it is on: a pack at 3620 mV is not yet
  // `normal`, because leaving `saving` needs the margin.
  b.boot(interfaces::WokeBy::kTimer);
  b.world.rak.battery_mv = 3620;
  b.v().begin();
  CHECK(b.duty().reason().band() == Band::kSaving);
  REQUIRE(b.duty().readPower());
  CHECK(b.duty().decide().band == Band::kSaving);

  // A power cut forgets it, and then the thresholds decide outright.
  b.world.powerCut();
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  b.world.rak.battery_mv = 3620;
  b.v().begin();
  CHECK(b.duty().reason().band() == Band::kUnknown);
  REQUIRE(b.duty().readPower());
  CHECK(b.duty().decide().band == Band::kNormal);
}

TEST_CASE("saving's every-Nth counter crosses a sleep") {
  CycleBench b;
  int modem_wakes = 0;
  for (int i = 0; i < 12; ++i) {
    b.boot(interfaces::WokeBy::kTimer);
    b.world.rak.battery_mv = 3500;
    b.v().begin();
    REQUIRE(b.duty().readPower());
    const core::power::Decision d = b.duty().decide();
    REQUIRE(d.band == Band::kSaving);
    if (d.modem_this_wake) {
      ++modem_wakes;
      b.duty().modemWasUsed();
    } else {
      b.duty().modemWasSkipped();
    }
    REQUIRE(b.duty().sleepNow().refusal == SleepRefusal::kNone);
  }
  // Twelve six-hourly wakes is three days; one attach a day is what the band is for.
  CHECK(modem_wakes == 12 / static_cast<int>(core::power::kSavingModemEveryNth));
}
