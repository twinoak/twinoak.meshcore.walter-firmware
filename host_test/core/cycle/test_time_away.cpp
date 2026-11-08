// **How long the board was away** (task p10-01, core/cycle/wake_reason.h): the one measure of a
// deep sleep a Walter has once `esp_timer` has started again at zero, and the rule it is measured
// under - *never more than really passed*, because the agent's library takes it off a wait the
// server asked for (the exit-node firmware's task p1.10-42).
//
// A timer wake has slept the seconds the last sleep asked its timer for, less the timer's own
// error (`kTimerErrorPercent`, ten per cent: the internal RC oscillator the ESP32-S3's
// RTC_SLOW_CLK defaults to, for which Espressif publishes no tolerance - the header cites the
// Kconfig and the page). **Every other wake passes 0**: a wake on the radio's line, a spurious
// one, a person's cable, a power-on, a reset, a crash, a watchdog. The sleep was cut short by
// whatever woke us and nothing here knows how much of it had passed, and 0 is the behaviour this
// firmware had before it measured any.
#include <doctest.h>

#include "core/cycle/wake_reason.h"
#include "cycle_bench.h"

using bench::CycleBench;
using core::cycle::Wake;

namespace {

// The believed part of a sleep, in milliseconds: the arithmetic the tests below are written
// against, so that a change to `kTimerErrorPercent` moves the figures and not the rule.
std::uint32_t believed(std::uint32_t slept_s) {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(slept_s) * 1000u *
                                    (100u - core::cycle::kTimerErrorPercent) / 100u);
}

}  // namespace

TEST_CASE("a boot that followed no sleep of ours knows of no time away") {
  CycleBench b;
  b.v().begin();
  CHECK(b.duty().reason().lastSleepAskedS() == 0);
  CHECK(b.duty().reason().timeAwayMs() == 0);
  // And a wake reason that has not been worked out answers 0 rather than a figure of its own.
  CycleBench fresh;
  CHECK(fresh.duty().reason().timeAwayMs() == 0);
}

TEST_CASE("a timer wake was away the interval the sleep asked for, less the timer's error") {
  CycleBench b;
  const std::uint32_t slept_s = 900;  // the `plenty` band's quarter of an hour
  b.v().begin();
  (void)b.duty().decide();
  const core::cycle::SleepResult slept = b.duty().sleepFor(slept_s);
  REQUIRE(slept.refusal == core::cycle::SleepRefusal::kNone);
  REQUIRE(slept.seconds == slept_s);

  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.wake().wake == Wake::kTimer);
  CHECK(b.duty().reason().lastSleepAskedS() == slept_s);
  CHECK(b.duty().reason().timeAwayMs() == believed(slept_s));
  // Never more than really passed: 810 s of a 900 s sleep, and a hold of 30 s or 120 s is spent
  // whole by it, which is the whole point.
  CHECK(b.duty().reason().timeAwayMs() == 810u * 1000u);
  CHECK(b.duty().reason().timeAwayMs() < slept_s * 1000u);
}

TEST_CASE("an hour's sleep, and a day's: the same arithmetic") {
  const std::uint32_t intervals[] = {3600u, 21600u, 86400u};
  for (std::uint32_t slept_s : intervals) {
    CycleBench b;
    b.v().begin();
    (void)b.duty().decide();
    REQUIRE(b.duty().sleepFor(slept_s).refusal == core::cycle::SleepRefusal::kNone);
    b.boot(interfaces::WokeBy::kTimer);
    b.v().begin();
    CHECK_MESSAGE(b.duty().reason().timeAwayMs() == believed(slept_s), slept_s);
  }
}

TEST_CASE("a wake on the radio's line was away a length nothing here can measure: 0") {
  // The sleep armed the timer for a quarter of an hour *and* the line, and the line is what woke
  // the board. How much of the quarter of an hour had passed is the one thing this end cannot
  // know, so it claims none of it - and the agent's hold is still owed whole.
  CycleBench b;
  b.v().begin();
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);
  REQUIRE(b.v().sleeper.last().radio_line);

  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  REQUIRE(b.wake().wake == Wake::kSpurious);  // nothing valid inside the window: still the line
  CHECK(b.duty().reason().lastSleepAskedS() == 900);  // the sleep is known...
  CHECK(b.duty().reason().timeAwayMs() == 0);         // ...and none of it is claimed
}

TEST_CASE("nor a person's cable, a power-on, a reset, a crash or a watchdog") {
  struct Case {
    interfaces::WokeBy woke;
    core::health::ResetReason reset;
    bool usb;
    Wake expected;
  };
  const Case cases[] = {
      {interfaces::WokeBy::kTimer, core::health::ResetReason::kDeepSleepWake, true, Wake::kUsb},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn, false, Wake::kPowerOn},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kResetPin, false, Wake::kReset},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic, false, Wake::kCrash},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kWatchdog, false, Wake::kWatchdog},
  };
  for (const Case& c : cases) {
    CycleBench b;
    b.v().begin();
    (void)b.duty().decide();
    REQUIRE(b.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);
    b.world.usb = c.usb;
    b.boot(c.woke, c.reset);
    b.v().begin();
    REQUIRE_MESSAGE(b.wake().wake == c.expected, static_cast<int>(c.expected));
    CHECK_MESSAGE(b.duty().reason().timeAwayMs() == 0, static_cast<int>(c.expected));
  }
}

TEST_CASE("a sleep with no timer at all is a sleep of no known length") {
  // `sleep 0` with the radio's line armed: a sleep only the line or a reset ends. Nothing asked
  // the timer for anything, so there is nothing to believe - and a wake the chip then called the
  // timer's could only be a chip this firmware does not understand.
  CycleBench b;
  b.v().begin();
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepFor(0).refusal == core::cycle::SleepRefusal::kNone);
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  CHECK(b.duty().reason().lastSleepAskedS() == 0);
  CHECK(b.duty().reason().timeAwayMs() == 0);
}

TEST_CASE("a power cut, and a record of another layout, leave no sleep to believe") {
  CycleBench b;
  b.v().begin();
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);

  SUBCASE("a power cut") {
    b.world.powerCut();
    b.boot(interfaces::WokeBy::kTimer);
    b.v().begin();
    CHECK_FALSE(b.duty().reason().recordSurvived());
    CHECK(b.duty().reason().timeAwayMs() == 0);
  }
  SUBCASE("a firmware whose record is another shape - which 0.1.3's first boot is") {
    // The duty-cycle record grew by four bytes in this task, and a record of another size in the
    // same slot is not the same record (interfaces/retained.h). So the first boot after the
    // update reads none: the wake log, the band's hysteresis and this figure all start again, for
    // one wake, exactly as after a power cut.
    b.world.retained.truncate(interfaces::RetainedSlot::kDutyCycle);
    b.boot(interfaces::WokeBy::kTimer);
    b.v().begin();
    CHECK_FALSE(b.duty().reason().recordSurvived());
    CHECK(b.duty().reason().timeAwayMs() == 0);
  }
}

TEST_CASE("the figure is the sleep this boot followed, not the one this boot is about to take") {
  // `endingInSleep` overwrites the record's copy with the seconds of the sleep it is entering, so
  // a reader after that would get the wrong one. The latch in `load()` is what holds them apart.
  CycleBench b;
  b.v().begin();
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.duty().reason().timeAwayMs() == believed(900));
  // This wake now sleeps for an hour. What it was away is still the quarter of an hour behind it.
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepFor(3600).refusal == core::cycle::SleepRefusal::kNone);
  CHECK(b.duty().reason().lastSleepAskedS() == 900);
  CHECK(b.duty().reason().timeAwayMs() == believed(900));
  // And the boot after it reads the hour.
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  CHECK(b.duty().reason().timeAwayMs() == believed(3600));
}

TEST_CASE("the sleep's own length is still what the wake log and the journal say it was") {
  // The four bytes are new; nothing that was in the record moved. One sleep, and the entry of the
  // wake it ended still carries how long that wake lasted - not how long the sleep after it was.
  CycleBench b;
  b.v().begin();
  (void)b.duty().decide();
  b.v().clock.delayMs(5000);
  REQUIRE(b.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.duty().reason().count() >= 2);
  CHECK(b.duty().reason().at(1).lasted_s >= 5);
  CHECK(b.duty().reason().at(1).lasted_s < 900);
  CHECK(b.duty().reason().at(0).lasted_s == 0);  // this wake has not reached sleep yet
}
