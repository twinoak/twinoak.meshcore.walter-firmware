// One wake, start to finish (task p3-05): the good wake - every step, in order, once - and then
// each step failing alone, which is what the task asks for first and what this firmware exists
// for. The dependency table is data and is walked here; `test_wake_month.cpp` is the long one.
#include <doctest.h>

#include <string>

#include "core/cycle/wake_bench.h"
#include "core/link/codec/wire.h"

using bench::WakeBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;
using core::power::Band;

namespace {

// What the record's letters say about one step.
char letterOf(const Pass& pass, Step step) {
  return core::cycle::letter(pass.outcomeOf(step));
}

}  // namespace

TEST_CASE("the good wake: every step, in order, once, and it ends in sleep") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.answers = interfaces::Exchanged::kNothing;

  const Pass& pass = bench.oneWake();

  // Every step reached a conclusion, and none of them is "not run".
  for (std::size_t i = 0; i < core::cycle::kStepCount; ++i) {
    const Step step = static_cast<Step>(i);
    CHECK_MESSAGE(pass.outcomeOf(step) != Outcome::kNotRun, core::cycle::name(step));
  }
  // The ones that must have *done* something in a good wake.
  CHECK(pass.ok(Step::kWhy));
  CHECK(pass.ok(Step::kRadio));
  CHECK(pass.ok(Step::kState));
  CHECK(pass.ok(Step::kBand));
  CHECK(pass.ok(Step::kUplink));
  CHECK(pass.ok(Step::kSleep));
  // The radio is healthy, the band is `plenty` (the bench's 4100 mV), and the wake slept.
  CHECK(pass.radio == core::supervisor::Health::kHealthy);
  CHECK(pass.band == Band::kPlenty);
  CHECK(pass.ending == Ending::kSlept);
  CHECK(bench.v().sleeper.sleeps == 1);
  // The clock was set: the radio has no RTC, so a boot of it is a clock to set (§5.4).
  CHECK(pass.clock_set);
  // The backend was asked exactly once, and it asked for nothing, so no update step ran.
  CHECK(bench.world.backend.exchanges == 1);
  CHECK(pass.exchanged == interfaces::Exchanged::kNothing);
  CHECK(pass.carried == interfaces::Ask::kNothing);
  CHECK(pass.outcomeOf(Step::kUpdate) == Outcome::kNothing);
  // And the wake is inside its bound, which is the only hard rail.
  CHECK(pass.awake_s * 1000u < core::cycle::kWakeBoundMs);
  CHECK(bench.v().restart.asked == 0);
}

TEST_CASE("nothing runs twice: a second run() inside a pass answers the pass in hand") {
  WakeBench bench;
  bench.modemWorks();
  bench.boot();
  bench.v().begin();
  const Pass& first = bench.v().run();
  const std::uint32_t exchanges = bench.world.backend.exchanges;
  // A pass that has ended is not "in hand", so this is a second whole wake - which is what
  // `cycle now` twice is. What must not happen is a step running twice *inside* one pass, and
  // the counters are what show it: one exchange a pass, one sleep a pass.
  CHECK(exchanges == 1);
  CHECK(first.ending == Ending::kSlept);
  CHECK(bench.v().sleeper.sleeps == 1);
}

TEST_CASE("the step table is the dependency table, and it is data") {
  using core::cycle::def;
  using core::cycle::kSteps;
  using core::cycle::stepBit;
  // Every row is its own step, in order, with a name and a bound.
  for (std::size_t i = 0; i < core::cycle::kStepCount; ++i) {
    const core::cycle::StepDef& d = kSteps[i];
    CHECK(static_cast<std::size_t>(d.step) == i);
    CHECK(std::string(d.name) == core::cycle::name(d.step));
    CHECK(d.bound_ms > 0);
    // A step may depend only on steps *before* it: a cycle in the table would be a wake that
    // could never run.
    for (std::size_t k = i; k < core::cycle::kStepCount; ++k) {
      CHECK_MESSAGE((d.needs & stepBit(static_cast<Step>(k))) == 0, d.name);
    }
  }
  // The three the whole design rests on, said out loud so that a change has to come here.
  CHECK(def(Step::kSleep).needs == 0);     // every wake ends in sleep
  CHECK(def(Step::kSleep).least_ms == 0);  // and it is never refused for want of time
  CHECK(def(Step::kBand).needs == 0);      // "no reading at all" is its own band
  CHECK(def(Step::kUplink).needs == stepBit(Step::kBand));
  CHECK(def(Step::kBackend).needs == stepBit(Step::kUplink));
  CHECK(def(Step::kClock).needs == (stepBit(Step::kState) | stepBit(Step::kUplink)));
  CHECK(def(Step::kUpdate).needs == stepBit(Step::kBand));
}

TEST_CASE("the radio fails alone: the uplink is still used to say so, and the wake sleeps") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.rak.deaf = true;  // the radio answers nothing at all
  const Pass& pass = bench.oneWake();

  // The radio step still *succeeded*: a silent radio is a named state of the wake and not a
  // failure of the step that found it out.
  CHECK(pass.ok(Step::kRadio));
  CHECK(pass.radio == core::supervisor::Health::kSilent);
  // The state could not be read, and the clock depends on it: skipped, with the reason.
  CHECK(pass.outcomeOf(Step::kState) == Outcome::kFailed);
  CHECK(pass.outcomeOf(Step::kClock) == Outcome::kSkipped);
  CHECK(std::string(pass.step[static_cast<std::size_t>(Step::kClock)].why) == "state");
  // The band has no reading, which is its own band - and it allows the modem, because a Walter
  // whose radio has died is exactly the one that has to report it.
  CHECK(pass.band == Band::kUnknown);
  CHECK(pass.ok(Step::kUplink));
  const bool backend_ran = pass.ok(Step::kBackend) ||
                           pass.outcomeOf(Step::kBackend) == Outcome::kNothing;
  CHECK(backend_ran);
  CHECK(pass.ending == Ending::kSlept);
}

TEST_CASE("the uplink fails alone: the clock and the backend are skipped, the rest runs") {
  WakeBench bench;
  bench.modemSilent();
  const Pass& pass = bench.oneWake();

  CHECK(pass.ok(Step::kRadio));
  CHECK(pass.ok(Step::kState));
  CHECK(pass.ok(Step::kBand));
  CHECK(pass.outcomeOf(Step::kUplink) == Outcome::kFailed);
  // Both of the uplink's dependents are skipped, named by the step they were waiting for.
  CHECK(pass.outcomeOf(Step::kClock) == Outcome::kSkipped);
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kSkipped);
  CHECK(std::string(pass.step[static_cast<std::size_t>(Step::kBackend)].why) == "uplink");
  // Nothing was asked of the backend, so nothing is carried out - and the wake still sleeps.
  CHECK(bench.world.backend.exchanges == 0);
  CHECK(pass.ending == Ending::kSlept);
  CHECK(letterOf(pass, Step::kUplink) == 'x');
  CHECK(letterOf(pass, Step::kBackend) == 's');
  CHECK(letterOf(pass, Step::kSleep) == 'y');
}

TEST_CASE("the backend fails alone: the readings are kept and the refusals are not forgotten") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.answers = interfaces::Exchanged::kFailed;
  const Pass& pass = bench.oneWake();

  CHECK(pass.ok(Step::kUplink));
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kFailed);
  CHECK(pass.ending == Ending::kSlept);
  // One reading was taken this wake and nobody took it away.
  CHECK(bench.v().readings.count() == 1);
  CHECK_FALSE(pass.readings_taken);
}

TEST_CASE("the battery cannot be read: the band is `no-reading` and the wake still reports") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.rak.power_valid = 0;  // §5.8: channel 2's bit is clear
  const Pass& pass = bench.oneWake();

  CHECK(pass.ok(Step::kState));  // STATUS answered; the reading did not
  CHECK(pass.band == Band::kUnknown);
  CHECK(pass.ok(Step::kUplink));  // `no-reading` allows the modem
  CHECK(pass.ending == Ending::kSlept);
  // And the reading that was kept says the battery was not known, rather than saying 0 mV.
  REQUIRE(bench.v().readings.count() == 1);
  CHECK_FALSE(bench.v().readings.at(0).battery_known);
}

TEST_CASE("a wake with no modem at all in `critical`: the modem is not touched and it sleeps") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.rak.battery_mv = 3300;  // below the floor
  const Pass& pass = bench.oneWake();

  CHECK(pass.band == Band::kCritical);
  CHECK(pass.outcomeOf(Step::kUplink) == Outcome::kSkipped);
  CHECK(bench.world.modem.presents == 0);  // nothing of the modem was asked anything
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kSkipped);
  CHECK(pass.ending == Ending::kSlept);
  // A day, which is `critical`'s interval, and `wake.interval` cannot shorten it.
  CHECK(pass.interval_s == core::power::kCriticalMinutes * 60u);
}

TEST_CASE("the radio is a scanner: no clock, no telemetry, and the wake reports and sleeps") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.rak.scanner = true;
  const Pass& pass = bench.oneWake();

  CHECK(pass.radio == core::supervisor::Health::kScanner);
  CHECK(pass.ok(Step::kRadio));  // a scanner is a state, not a fault (§9)
  CHECK(pass.outcomeOf(Step::kState) == Outcome::kSkipped);
  CHECK(pass.outcomeOf(Step::kClock) == Outcome::kSkipped);
  CHECK_FALSE(pass.clock_set);
  // The uplink is still used - that is how anybody learns the radio is a scanner.
  CHECK(pass.ok(Step::kUplink));
  CHECK(pass.ending == Ending::kSlept);
}

TEST_CASE("a wake that found a USB host still runs a pass when it is asked to") {
  // On a board `main/` does not run the cycle while a host is there (a person is at the console).
  // `cycle now` is how they ask for one, and it has to work.
  WakeBench bench;
  bench.modemWorks();
  bench.world.usb = true;
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kResetPin);
  bench.v().begin();
  const std::string said = bench.v().runLine("cycle now");
  CHECK(bench::has(said, "wake"));
  CHECK(bench::has(said, "the wake:"));
  // The guard does not fire while a host is there, whatever the pass did.
  CHECK(bench.v().restart.asked == 0);
}

TEST_CASE("`cycle dry` does nothing: no frame, no modem, no sleep, no journal entry") {
  WakeBench bench;
  bench.modemWorks();
  bench.boot();
  bench.v().begin();
  const std::size_t frames_before = bench.world.rak.requests.size();
  const int modem_before = bench.world.modem.presents;
  const std::uint32_t entries_before = bench.v().journal.validEntries();

  const std::string said = bench.v().runLine("cycle dry");

  CHECK(bench.world.rak.requests.size() == frames_before);
  CHECK(bench.world.modem.presents == modem_before);
  CHECK(bench.v().sleeper.sleeps == 0);
  CHECK(bench.v().journal.validEntries() == entries_before);
  CHECK(bench.world.backend.exchanges == 0);
  CHECK(bench.cycle().last().dry);
  CHECK(bench.cycle().last().ending == Ending::kDry);
  // And it says what it would do, step by step, with the bounds.
  CHECK(bench::has(said, "doing none of it"));
  CHECK(bench::has(said, "up to"));
  for (std::size_t i = 0; i < core::cycle::kStepCount; ++i) {
    CHECK(bench::has(said, core::cycle::name(static_cast<Step>(i))));
  }
}

TEST_CASE("a build with nothing to do it with: every step answers, and the wake sleeps") {
  // The factory case: no store, no journal, no supervisor, no update machine. A step with
  // nothing to do it with is `-` and the wake goes on.
  bench::WakeWorld world;
  world.modem.deaf = false;
  bench::WakeBootClock clock{world.clock};
  fakes::FakePins pins{world.clock};
  pins.record_log = false;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::board::RadioPins board{pins, clock, world.retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::cycle::NetworkClock wall{clock};
  core::link::client::LinkPort port{board, world.rak};
  core::link::client::Client link{port, clock, wall, slots};
  core::health::Health health{world.retained, clock};
  health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{world.modem, clock, world.retained, modem_settings};
  uplink.begin();
  fakes::FakeSleeper sleeper{world.clock, pins};
  core::cycle::TelemetryRing readings{world.retained};
  readings.begin();
  world.clock.advance(core::board::kKillTiming.settle_ms);
  board.safe();
  REQUIRE(port.open() == core::board::Outcome::kOk);
  core::cycle::DutyCycle duty{sleeper,         board, link, uplink, health, clock,
                              wall,            restart, world.retained};
  core::cycle::NullBackend nothing;
  core::cycle::WakeCycle cycle{duty, link, uplink, nothing, readings, wall, clock};
  duty.begin();
  duty.guard().arm();

  const Pass& pass = cycle.run();
  CHECK(pass.outcomeOf(Step::kRadio) == Outcome::kNothing);
  // No supervisor means nothing has worked out what the radio is, and **nothing of this firmware
  // touches the radio before it has**: the two steps that need step 2 are skipped, named by it.
  // The band and the uplink still run, so a Walter with no store and no journal still reports.
  CHECK(pass.outcomeOf(Step::kResume) == Outcome::kSkipped);
  CHECK(pass.outcomeOf(Step::kState) == Outcome::kSkipped);
  CHECK(pass.ok(Step::kBand));
  CHECK(pass.ending == Ending::kSlept);
  CHECK(sleeper.sleeps == 1);
  CHECK(pins.violations.empty());
}

TEST_CASE("`lte up` at the console gives this Walter a clock, so the link has a time to give") {
  // Until task p9-04 only step 6 of a wake ever filled `NetworkClock`, so a boot a person drove
  // by hand had no time of its own however long the uplink had been up - and `rak update`'s §5.4
  // clock check was skipped in every one of them, saying *until phase 3's modem*
  // (tasks/phase-8/FOLLOWUPS.md).
  WakeBench bench;
  bench.modemWorks();
  bench.world.usb = true;
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kResetPin);
  bench.v().begin();
  CHECK_FALSE(bench.v().wall.known());
  interfaces::WallTime none{};
  CHECK_FALSE(bench.v().wall.now(&none));
  // The link has nothing to give the radio: a TIME_SET is not sent.
  const std::uint32_t sets_before = bench.v().link.counters().time_sets;

  const std::string said = bench.v().runLine("lte up");
  INFO(said);
  CHECK(bench::has(said, "from the network"));
  CHECK(bench::has(said, "this Walter's clock is set from it"));
  CHECK(bench.v().wall.known());
  interfaces::WallTime now{};
  REQUIRE(bench.v().wall.now(&now));
  CHECK(now.epoch != 0);
  CHECK(now.source == core::link::time_source::kCellular);
  // And the link now has one, which is what §5.4's check of an update reads.
  (void)bench.v().link.hello();
  (void)bench.v().link.serviceTime();
  CHECK(bench.v().link.counters().time_sets > sets_before);
}
