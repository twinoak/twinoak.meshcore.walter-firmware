// What a wake was doing when it was ended (task p15-06).
//
// In the night of 17 November 2026 the unit took the hourly `status` job of its own repeater at
// 01:29 UTC and again at 02:05, sent its heartbeat, claimed the job, and was never heard again in
// that wake. Five minutes later the bound on a wake ended it by reset, the next boot read that as
// a watchdog, and nothing anywhere said what those five minutes had been spent on.
//
// These are the tests of the one line that says it: written as the wake goes, in RTC memory, and
// read by the boot after.
//
//   * where a wake is, as it walks the ten steps;
//   * a wake that hangs in a claimed job, in a call, and with nothing finer than the step;
//   * the boot after: the journal's line, the list of ten, the report's `wakes` section;
//   * the same for a crash, with the reason the boot already gives;
//   * a wake that reaches sleep: no line, and its entry as it was before this task;
//   * the fifth total of `agent_wakes`, over a sleep and a power cut;
//   * a slot with no record in it, and one of another shape: no word, and nothing refused.
#include <doctest.h>

#include <string>

#include "core/cycle/wake_progress.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"
#include "wake_bench.h"

using bench::WakeBench;
using core::cycle::Doing;
using core::cycle::Ending;
using core::cycle::Step;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// Every line **this task adds** fits a 100-column terminal, which is the rule the list of ten and
// the journal's lines are written inside of (test_what_a_wake_did.cpp says the same). Only this
// task's lines: a wake record's own journal line has been longer than that since task p3-05, and
// this is not the task that shortens it.
void theLinesOfThisTaskFitATerminal(const std::string& text) {
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? end : end - at);
    if (line.find("it was in:") != std::string::npos ||
        line.find("did not reach sleep") != std::string::npos) {
      CHECK_MESSAGE(line.size() <= 100, line);
    }
    if (end == std::string::npos) break;
    at = end + 1;
  }
}

// What a section of the report says, which is how a unit in service is read (task p13-01).
std::string sectionOf(WakeBench& bench, const char* name) {
  fakes::CaptureOut out;
  REQUIRE(bench.v().console->writeSection(name, out));
  return out.text;
}

// A wake that hangs at step 8 and is ended by the bound, and then **the boot after it**, which
// is the reader: its step 1 is where the record is read, the line is journalled and the fifth
// total moves. `reason` is what that boot reads about the wake before - the bound's own restart
// by default.
void aWakeEndedAndTheBootAfter(
    WakeBench& bench, core::health::ResetReason reason = core::health::ResetReason::kSoftware,
    core::health::Restart requested = core::health::Restart::kWakeOverrun) {
  bench.boot();
  bench.v().begin();
  REQUIRE(bench.wakeEndedByItsBound());
  bench.boot(interfaces::WokeBy::kNotFromSleep, reason, requested);
  bench.v().begin();
  // The boot after runs an ordinary wake: the hang was the wake before's.
  bench.world.backend.forget();
  bench.world.backend.answered();
  REQUIRE(bench.v().run().ending == Ending::kSlept);
}

}  // namespace

TEST_CASE("where a wake is, written down as it walks the ten steps") {
  WakeBench bench;
  bench.modemWorks();
  bench.boot();
  bench.v().begin();
  // Before the first step: the record is this wake's own and says nothing finer than that.
  CHECK(bench.duty().progress().now().step == 0);
  CHECK(bench.duty().progress().now().doing == Doing::kNothing);
  CHECK_FALSE(bench.duty().progress().now().reached_sleep);

  const core::cycle::Pass& pass = bench.v().run();
  REQUIRE(pass.ending == Ending::kSlept);
  // The last step a wake takes is the tenth, and it reached sleep - so the boot after has nothing
  // to say about it.
  CHECK(bench.duty().progress().now().step == 10);
  CHECK(bench.duty().progress().now().reached_sleep);
}

TEST_CASE("a wake that hangs in the second request of a claimed status job: the boot after says so") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.hangsInJob("status", 1);
  aWakeEndedAndTheBootAfter(bench);

  // 1. The journal's line.
  const std::vector<core::cycle::MidwayPayload> said = bench.midwayEntries();
  REQUIRE(said.size() == 1);
  const core::cycle::WakeWhere& where = said[0].where;
  CHECK(where.endedMidway());
  CHECK(where.step == static_cast<std::uint8_t>(Step::kBackend) + 1);  // step 8
  CHECK(where.doing == Doing::kJob);
  CHECK(where.in_job);
  CHECK(std::string(where.what) == "status");
  CHECK(where.requests == 1);
  CHECK(where.forS() > 60);  // the bound is five minutes and the job had it all
  // And this firmware's own bound is what the boot read: a watchdog, which is what it is.
  CHECK(said[0].wake == static_cast<std::uint8_t>(core::cycle::Wake::kWatchdog));

  const std::string journal = bench.v().runLine("journal");
  INFO(journal);
  CHECK(has(journal, "the wake before did not reach sleep (watchdog)"));
  CHECK(has(journal, "step 8 (backend), a `status` job, 1 request(s)"));
  theLinesOfThisTaskFitATerminal(journal);

  // 2. The list of ten, on the entry of the wake it is about and nowhere else.
  const std::string wake = bench.v().runLine("wake");
  INFO(wake);
  CHECK(has(wake, "it never reached sleep (a reset, a crash or the bound)"));
  CHECK(has(wake, "it was in: step 8 (backend), a `status` job, 1 request(s)"));
  theLinesOfThisTaskFitATerminal(wake);

  // 3. And the report's `wakes` section, which is read from afar.
  const std::string report = sectionOf(bench, "wakes");
  INFO(report);
  CHECK(has(report, "it was in: step 8 (backend), a `status` job, 1 request(s)"));
}

TEST_CASE("a wake that hangs in the observations call: they name the call") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.hangsInCall("observations");
  aWakeEndedAndTheBootAfter(bench);

  const std::vector<core::cycle::MidwayPayload> said = bench.midwayEntries();
  REQUIRE(said.size() == 1);
  CHECK(said[0].where.doing == Doing::kCall);
  CHECK(std::string(said[0].where.what) == "observations");
  CHECK(said[0].where.on_modem);  // a call that is out is the modem being waited on
  CHECK_FALSE(said[0].where.in_job);

  const std::string wake = bench.v().runLine("wake");
  INFO(wake);
  CHECK(has(wake, "the call out: `observations`, on the modem"));
  theLinesOfThisTaskFitATerminal(wake);
}

TEST_CASE("a wake ended with nothing finer written down than the step names the step") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.hangsInStep();
  aWakeEndedAndTheBootAfter(bench);

  const std::vector<core::cycle::MidwayPayload> said = bench.midwayEntries();
  REQUIRE(said.size() == 1);
  CHECK(said[0].where.step == static_cast<std::uint8_t>(Step::kBackend) + 1);
  CHECK(said[0].where.doing == Doing::kStep);
  CHECK(std::string(said[0].where.what).empty());

  const std::string wake = bench.v().runLine("wake");
  INFO(wake);
  CHECK(has(wake, "it was in: step 8 (backend), 29"));  // the seconds, and nothing between
  CHECK_FALSE(has(wake, "` job,"));
  CHECK_FALSE(has(wake, "the call out:"));
}

TEST_CASE("a wake ended before its uplink was up names that step") {
  WakeBench bench;
  bench.modemWorks();
  bench.boot();
  bench.v().begin();
  // No step of 3 to 7 can outlast the bound on a wake by itself - every one of their bounds is
  // inside it (core/cycle/wake_cycle.h) - so the step a wake was in when it was ended is written
  // here rather than waited for, which is exactly what `begin(Step)` does in a pass.
  bench.duty().progress().atStep(static_cast<std::uint8_t>(Step::kUplink) + 1);
  bench.world.clock.advance(42000);
  bench.duty().progress().stillAwake();

  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware,
             core::health::Restart::kWakeOverrun);
  bench.v().begin();
  const core::cycle::WakeWhere& where = bench.duty().progress().before();
  REQUIRE(where.endedMidway());
  CHECK(where.step == static_cast<std::uint8_t>(Step::kUplink) + 1);  // step 6
  CHECK(where.doing == Doing::kStep);
  CHECK(where.forS() == 42);

  char line[120];
  (void)core::cycle::describeWhere(where, line, sizeof line);
  CHECK(std::string(line) == "step 6 (uplink), 42 s in it");
}

TEST_CASE("a wake ended by a crash in the same place: the same line, with the boot's own reason") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.hangsInJob("config.read", 7);
  // Not the bound this time: a panic in the same place. The record is the same record - it is
  // written as the wake goes and nothing about the ending writes it - and the reason is the one
  // the boot already gives.
  aWakeEndedAndTheBootAfter(bench, core::health::ResetReason::kPanic,
                            core::health::Restart::kNotRequested);

  const std::vector<core::cycle::MidwayPayload> said = bench.midwayEntries();
  REQUIRE(said.size() == 1);
  CHECK(said[0].wake == static_cast<std::uint8_t>(core::cycle::Wake::kCrash));
  CHECK(said[0].where.doing == Doing::kJob);
  CHECK(std::string(said[0].where.what) == "config.read");
  CHECK(said[0].where.requests == 7);

  const std::string journal = bench.v().runLine("journal");
  INFO(journal);
  CHECK(has(journal, "the wake before did not reach sleep (crash)"));
  CHECK(has(journal, "a `config.read` job, 7 request(s)"));
}

TEST_CASE("a wake that reaches sleep leaves no word, and its entry is what it was") {
  WakeBench bench;
  bench.modemWorks();
  REQUIRE(bench.oneWake().ending == Ending::kSlept);
  bench.boot();
  bench.v().begin();

  CHECK(bench.duty().progress().before().said);
  CHECK(bench.duty().progress().before().reached_sleep);
  CHECK_FALSE(bench.duty().progress().before().endedMidway());
  CHECK(bench.midwayEntries().empty());

  const std::string wake = bench.v().runLine("wake");
  INFO(wake);
  CHECK_FALSE(has(wake, "it was in:"));
  // The entry of the wake that slept says how long it was awake, as it did before this task.
  CHECK(has(wake, "awake "));
  const std::string journal = bench.v().runLine("journal");
  CHECK_FALSE(has(journal, "did not reach sleep"));
}

TEST_CASE("the fifth total moves by one for each, survives a sleep and a power cut, and is in the heartbeat") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.hangsInJob("status", 2);

  // Three wakes that did not reach sleep: the bound, a crash, and the chip's own watchdog.
  for (int i = 0; i < 3; ++i) {
    bench.boot();
    bench.v().begin();
    REQUIRE(bench.wakeEndedByItsBound());
    const core::health::ResetReason reason = i == 0 ? core::health::ResetReason::kSoftware
                                             : i == 1 ? core::health::ResetReason::kPanic
                                                      : core::health::ResetReason::kWatchdog;
    bench.boot(interfaces::WokeBy::kNotFromSleep, reason,
               i == 0 ? core::health::Restart::kWakeOverrun
                      : core::health::Restart::kNotRequested);
    bench.v().begin();
    // The boot reads the record and counts it; `cycle now` in the same boot counts it once.
    CHECK(bench.v().cycle->last().ending == Ending::kRunning);
    bench.world.backend.forget();
    bench.world.backend.answered();
    (void)bench.v().run();
    CHECK(bench.v().wake_counters.totals().midway == static_cast<std::uint32_t>(i + 1));
    bench.world.backend.hangsInJob("status", 2);
  }

  // A sleep keeps it...
  bench.world.backend.forget();
  bench.boot();
  bench.v().begin();
  CHECK(bench.v().wake_counters.survived());
  CHECK(bench.v().wake_counters.totals().midway == 3);
  // ... and so does a power cut, which takes RTC memory and leaves the settings store. The record
  // of where the last wake was goes with RTC memory, so that wake leaves no word - which is the
  // honest answer - and the total it had already been counted in stays.
  bench.world.powerCut();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  bench.v().begin();
  CHECK(bench.v().wake_counters.totals().midway == 3);
  CHECK_FALSE(bench.duty().progress().before().said);
  CHECK(bench.midwayEntries().size() == 3);  // the journal is flash and keeps all three
}

TEST_CASE("a retained slot with no record of this shape in it: no word, and nothing is refused") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  // **What a unit updated into the release that carries this task reads**: every other slot's
  // record as it was, and nothing at all in this one. There is no word, nothing is refused, and
  // this wake's own record is written over the nothing.
  {
    core::cycle::WakeProgress progress(retained, clock);
    progress.begin();
    CHECK_FALSE(progress.before().said);
    CHECK_FALSE(progress.before().endedMidway());
    progress.atStep(8);
    progress.reachedSleep();
  }
  // And a record of another shape - a firmware whose layout changed again - is refused by the rule
  // the retained store has had since task p3-04, and reads as no record at all.
  REQUIRE(retained.has(interfaces::RetainedSlot::kWakeProgress));
  retained.truncate(interfaces::RetainedSlot::kWakeProgress);
  {
    core::cycle::WakeProgress progress(retained, clock);
    progress.begin();
    CHECK_FALSE(progress.before().said);
  }
  // A record of this task's own shape reads back whole, which is the other half of the rule: the
  // one the paragraph above wrote reached no step, so it is read and it is no word.
  {
    core::cycle::WakeProgress progress(retained, clock);
    progress.begin();
    CHECK(progress.before().said);
    CHECK(progress.before().step == 0);
    CHECK_FALSE(progress.before().endedMidway());
  }
}

TEST_CASE("the wake cycle's own slot gives back the twelve bytes it never used") {
  // The telemetry ring's room shrank to its record's size so that this task's slot had somewhere
  // to come from (interfaces/retained.h, kRetainedSlotBytes). A record of the old 128 bytes does
  // not fit it any more, which is the one cost: the ring's ten readings and its refusals start
  // again at the first boot of the release that carries this.
  fakes::FakeRetained retained;
  std::uint8_t bytes[interfaces::kRetainedRecordMax] = {};
  CHECK_FALSE(retained.store(interfaces::RetainedSlot::kWakeCycle, bytes,
                             interfaces::kRetainedRecordMax));
  CHECK(retained.store(interfaces::RetainedSlot::kWakeCycle, bytes, core::cycle::kRecordBytes));
  // And the slots a unit in service has records in are untouched, so those records survive.
  for (const interfaces::RetainedSlot slot :
       {interfaces::RetainedSlot::kResetRecord, interfaces::RetainedSlot::kHealth,
        interfaces::RetainedSlot::kSupervisor, interfaces::RetainedSlot::kSupervisorIncidents,
        interfaces::RetainedSlot::kUplink, interfaces::RetainedSlot::kTlsTrust,
        interfaces::RetainedSlot::kDutyCycle}) {
    CHECK(interfaces::retainedSlotBytes(slot) == interfaces::kRetainedRecordMax);
  }
}

TEST_CASE("the record fits its retained slot, and a store of one more byte is refused") {
  // The slot is 24 bytes and the record is 24 bytes: the two numbers are held together by a
  // static_assert in core/cycle/wake_progress.h and by this, so that the day one of them moves
  // is the day a test says so.
  CHECK(core::cycle::kProgressBytes == 24);
  CHECK(interfaces::retainedSlotBytes(interfaces::RetainedSlot::kWakeProgress) ==
        core::cycle::kProgressBytes);
  fakes::FakeRetained retained;
  std::uint8_t bytes[core::cycle::kProgressBytes + 1] = {};
  CHECK(retained.store(interfaces::RetainedSlot::kWakeProgress, bytes, core::cycle::kProgressBytes));
  CHECK_FALSE(retained.store(interfaces::RetainedSlot::kWakeProgress, bytes, sizeof bytes));
  // And the slot the twelve bytes came from still holds the record it is for.
  CHECK(core::cycle::kRecordBytes ==
        interfaces::retainedSlotBytes(interfaces::RetainedSlot::kWakeCycle));
}
