// **How long the board was away** (task p10-01, core/cycle/wake_reason.h): the one measure of a
// deep sleep a Walter has once `esp_timer` has started again at zero, and the rule it is measured
// under - *never more than really passed*, because the agent's library takes it off a wait the
// server asked for (the exit-node firmware's task p1.10-42).
//
// A timer wake has slept the seconds the last sleep asked its timer for, less the timer's own
// error (`kTimerErrorPercent`, ten per cent: the internal RC oscillator the ESP32-S3's
// RTC_SLOW_CLK defaults to, for which Espressif publishes no tolerance - the header cites the
// Kconfig and the page).
//
// **And every other wake is measured by the radio** (task p14-06). Until it, every other wake
// passed 0 - a wake on the radio's line, a spurious one, a person's cable, a power-on, a reset,
// a crash, a watchdog - because the sleep had been cut short by whatever woke the board and
// nothing here knew how much of it had passed. The radio knew: it never sleeps and counts its
// own seconds, and its `uptime_s` written down before the sleep and read again at the boot are
// the two ends of the span. The cost of not asking it was 13 November 2026, when *Wake it up*
// reached the unit's radio and the board sent nothing because a failed call's wait came back
// whole (meshmaster.dk job 28399).
//
// 0 is still the answer when nothing could measure it: no reading, a radio that answered
// nothing, a radio that restarted too (another `boot_id`), an `uptime_s` below the one written
// down. The rule is the same for both halves - *never more than really passed* - and the
// arithmetic errs that way at every step: a whole second off the span for the truncation at
// either end, and the radio clock's own 500 ppm rounded up.
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

TEST_CASE("a wake on the radio's line with no reading to measure against: 0") {
  // The sleep armed the timer for a quarter of an hour *and* the line, and the line is what woke
  // the board. Nothing was written down before that sleep - this Walter never opened a session,
  // so the sleep had no radio to ask - so there is nothing to measure against and none of the
  // quarter of an hour is claimed.
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
  CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
  CHECK_FALSE(b.duty().reason().hasRadioReading());
}

TEST_CASE("nor a person's cable, a power-on, a reset, a crash or a watchdog, with none") {
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

// -- the radio's own seconds (task p14-06) ---------------------------------------------------------

namespace {

// What a span of `span_s` radio seconds is believed to have been, in milliseconds: a whole second
// off for the truncation at either end, then the radio clock's own error rounded up. Written out
// so that a change to `kRadioClockErrorPpm` moves these figures and not the rule.
std::uint32_t believedRadio(std::uint32_t span_s) {
  if (span_s <= core::cycle::kRadioTruncationS) return 0;
  const std::uint64_t ms =
      static_cast<std::uint64_t>(span_s - core::cycle::kRadioTruncationS) * 1000u;
  const std::uint64_t error = (ms * core::cycle::kRadioClockErrorPpm + 999999u) / 1000000u;
  return static_cast<std::uint32_t>(ms - error);
}

// A Walter that opens a session, sleeps, and writes the radio's uptime down as it goes: the
// sleep's reading needs a session, and `readPower()` is what a wake opens one with.
void sleepWithAReading(CycleBench& b, std::uint32_t uptime_s, std::uint32_t seconds = 900) {
  b.world.rak.override_uptime = true;
  b.world.rak.uptime_s = uptime_s;
  b.v().begin();
  REQUIRE(b.duty().readPower());
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepFor(seconds).refusal == core::cycle::SleepRefusal::kNone);
  REQUIRE(b.duty().reason().hasRadioReading());
  CHECK(b.duty().reason().radioReadingUptimeS() == uptime_s);
}

}  // namespace

TEST_CASE("a wake on the radio's line is away what the radio's uptime says it was") {
  CycleBench b;
  sleepWithAReading(b, 4000);
  b.world.rak.uptime_s = 4000 + 400;

  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  REQUIRE(b.wake().wake == Wake::kSpurious);  // the line, with nothing valid on it: still measured
  CHECK(b.duty().reason().timeAwayMs() == believedRadio(400));
  CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kRadio);
  // 398 800 ms of the 400 000 that really passed, and never one more.
  CHECK(b.duty().reason().timeAwayMs() == 398800u);
  CHECK(b.duty().reason().timeAwayMs() < 400u * 1000u);
  // And the wake's own entry says what measured it, which is what `wake` prints a boot later.
  CHECK((b.duty().reason().at(0).did & core::cycle::kAwayByRadio) != 0);
  CHECK((b.duty().reason().at(0).did & core::cycle::kDidSaid) == 0);  // not one of the three letters
}

TEST_CASE("a person's cable, a reset, a crash and a watchdog are measured the same way") {
  struct Case {
    interfaces::WokeBy woke;
    core::health::ResetReason reset;
    bool usb;
    Wake expected;
  };
  const Case cases[] = {
      {interfaces::WokeBy::kTimer, core::health::ResetReason::kDeepSleepWake, true, Wake::kUsb},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kResetPin, false, Wake::kReset},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic, false, Wake::kCrash},
      {interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kWatchdog, false, Wake::kWatchdog},
  };
  for (const Case& c : cases) {
    CycleBench b;
    sleepWithAReading(b, 1000);
    b.world.rak.uptime_s = 1000 + 61;
    b.world.usb = c.usb;
    b.boot(c.woke, c.reset);
    b.v().begin();
    REQUIRE_MESSAGE(b.wake().wake == c.expected, static_cast<int>(c.expected));
    CHECK_MESSAGE(b.duty().reason().timeAwayMs() == believedRadio(61), static_cast<int>(c.expected));
    CHECK_MESSAGE(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kRadio,
                  static_cast<int>(c.expected));
  }
}

TEST_CASE("a timer wake keeps the timer's figure, whatever the radio would have said") {
  // The one wake this task did not change. The radio is not asked at all: the sleep timer's own
  // seconds are the measure, and a radio whose uptime is far further on does not move them.
  CycleBench b;
  sleepWithAReading(b, 1000);
  b.world.rak.uptime_s = 1000 + 5000;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.wake().wake == Wake::kTimer);
  CHECK(b.duty().reason().timeAwayMs() == believed(900));
  CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kTimer);
  CHECK((b.duty().reason().at(0).did & core::cycle::kAwayByTimer) != 0);
  // And nothing was asked of the radio for it: a timer wake is nearly every wake this unit has.
  CHECK(b.v().link.counters().requests == 0);
}

TEST_CASE("another boot_id, a smaller uptime, a silent radio: 0 and the reason said as nothing") {
  SUBCASE("the radio restarted while the board slept") {
    CycleBench b;
    sleepWithAReading(b, 4000);
    b.world.rak.uptime_s = 4000 + 400;
    b.world.rak.restart(false);  // a new boot_id, and no EV_BOOT: the board was asleep
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    CHECK(b.duty().reason().timeAwayMs() == 0);
    CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
  }
  SUBCASE("an uptime below the one written down") {
    CycleBench b;
    sleepWithAReading(b, 4000);
    b.world.rak.uptime_s = 3999;
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    CHECK(b.duty().reason().timeAwayMs() == 0);
    CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
  }
  SUBCASE("a radio that answers nothing at the boot") {
    CycleBench b;
    sleepWithAReading(b, 4000);
    b.world.rak.uptime_s = 4000 + 400;
    b.world.rak.deaf = true;
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    CHECK(b.duty().reason().timeAwayMs() == 0);
    CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
  }
  SUBCASE("a span of one second is no time at all, and is still a measurement") {
    CycleBench b;
    sleepWithAReading(b, 4000);
    b.world.rak.uptime_s = 4001;
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    CHECK(b.duty().reason().timeAwayMs() == 0);
    CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kRadio);
  }
}

TEST_CASE("a sleep that had no radio to ask, and a state saved after the reading, leave none") {
  SUBCASE("the radio did not answer the sleep") {
    CycleBench b;
    b.world.rak.override_uptime = true;
    b.world.rak.uptime_s = 4000;
    b.v().begin();
    REQUIRE(b.duty().readPower());
    b.world.rak.deaf = true;
    (void)b.duty().decide();
    REQUIRE(b.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);
    CHECK_FALSE(b.duty().reason().hasRadioReading());
  }
  SUBCASE("a reading forgotten is a reading gone") {
    CycleBench b;
    sleepWithAReading(b, 4000);
    b.world.rak.uptime_s = 4000 + 400;
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    REQUIRE(b.duty().reason().timeAwayMs() > 0);
    // What the agent does when it saves its state again: the reading is void, and the next boot
    // measures nothing (walter_agent/agent_backend.cpp, `saveState`).
    b.duty().reason().forgetRadioReading();
    b.world.rak.uptime_s = 4000 + 1400;
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    CHECK(b.duty().reason().timeAwayMs() == 0);
    CHECK(b.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
  }
}

TEST_CASE("the duty cycle's record still fits a retained slot, with nothing to spare") {
  // 128 bytes exactly (interfaces/retained.h, kRetainedRecordMax). The static_assert in
  // core/cycle/wake_reason.h is the rail; this is the sentence that says the record is **full**,
  // so that the next thing which has to cross a sleep is known to need a slot of its own.
  CycleBench b;
  sleepWithAReading(b, 4000);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  CHECK(b.duty().reason().recordSurvived());
  CHECK(b.duty().reason().hasRadioReading());
}
