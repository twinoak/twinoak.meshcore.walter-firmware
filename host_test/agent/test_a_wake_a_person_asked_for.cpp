// A wake a person asked for (task p14-06), on the wake bench with the real `AgentBackend`, the
// real library and the scripted meshmaster.dk behind it.
//
// **The evening of 13 November 2026.** Thomas pressed *Wake it up* on meshmaster.dk (job 28399).
// The unit's radio answered `ok` at 18:09:02 UTC and raised §8's attention, reason 4. The board
// woke on the line - and nothing came from it until its own timer woke it a quarter of an hour
// later.
//
// Why: a failed call leaves the agent's library a wait; the wait is saved across the sleep and
// put back **less the time the board was away**; and the time away was 0 for every wake but the
// timer's, because the one measure of a sleep this board had was the seconds its own sleep timer
// had been asked for. A wake on the radio's line is not a timer wake, so the wait came back
// whole, and the agent's wake sends nothing while a wait is owed. This unit has had a failed
// call at nearly every wake.
//
// Two things in this file, and they are the two halves of that evening:
//
//   the time away is measured      the radio never sleeps and counts its own seconds, so the
//   for every kind of wake         `uptime_s` written down before the sleep and the one read at
//                                  the boot are the two ends of it - whatever woke the board;
//   and a wake a person asked      §8's reason 4, whether the event woke the board or step 4
//   for owes no wait at all        found it still pending in STATUS, lays the wait aside and
//                                  posts its heartbeat.
#include <doctest.h>

#include <string>

#include "agent/agent_bench.h"

using bench::AgentBench;
using core::cycle::Pass;
namespace att = core::link::attention;
namespace power = core::power;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

int heartbeatsSent(AgentBench& bench) {
  int n = 0;
  for (const std::string& call : bench.server().calls) n += call == "/agent/v1/heartbeat";
  return n;
}

// The radio's own second counter, as this bench turns it. A real radio's climbs with the clock;
// here a test says what it is at each end of a sleep, which is the only way to put a span of
// exactly N seconds across a simulated one.
constexpr std::uint32_t kUptimeBeforeSleep = 4000;

// The 13 November wake, in one line: the server answers 503 with a Retry-After of `wait_s`
// seconds, the heartbeat fails, the wait that leaves is saved, and the sleep writes down the
// radio's uptime. Answers nothing: what matters is in the store and in the retained record.
void aWaitThenASleep(AgentBench& bench, const char* wait_s = "300") {
  bench.world.rak.override_uptime = true;
  bench.world.rak.uptime_s = kUptimeBeforeSleep;
  bench.modemWorks();
  bench.server().failWith(503, wait_s);
  const Pass& failed = bench.oneWake();
  REQUIRE(failed.reached != interfaces::Reached::kAnswered);
  bench.server().answerAgain();
  // The reading belongs to the state the wake saved, and both are in the retained record and the
  // key-value store now.
  REQUIRE(bench.duty().reason().hasRadioReading());
  REQUIRE(bench.duty().reason().radioReadingUptimeS() == kUptimeBeforeSleep);
}

// The wake after it, on the radio's line, with the radio's uptime `on_s` seconds further on.
const Pass& theWakeAfter(AgentBench& bench, std::uint32_t on_s, std::uint8_t reason) {
  bench.world.rak.uptime_s = kUptimeBeforeSleep + on_s;
  bench.world.rak.raiseAttention(reason, reason == att::kObsRingNearlyFull ? 10 : 0, 3);
  const Pass& pass = bench.oneWake(interfaces::WokeBy::kRadioLine);
  REQUIRE(pass.reason == core::cycle::Wake::kAttention);
  REQUIRE(pass.attention == reason);
  return pass;
}

}  // namespace

// -- the time away, measured by the radio ---------------------------------------------------------

TEST_CASE("a sleep longer than the wait: the wake on the line posts its heartbeat") {
  // 400 seconds of the radio's own clock across a sleep that a 300-second wait did not outlive.
  // Until this task the wait came back whole and this wake sent nothing.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench);
  const int before = heartbeatsSent(bench);
  const Pass& pass = theWakeAfter(bench, 400, att::kObsRingNearlyFull);

  CHECK(pass.reached == interfaces::Reached::kAnswered);
  CHECK(heartbeatsSent(bench) == before + 1);
  // The figure, and where it came from: 399 seconds of the 400, less the radio clock's own
  // 500 ppm - and never a millisecond more than really passed.
  const std::uint32_t away = bench.duty().reason().timeAwayMs();
  CHECK(away > 398u * 1000u);
  CHECK(away < 400u * 1000u);
  CHECK(bench.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kRadio);
  // Nothing was laid aside: the wait was *spent*, which is the whole difference.
  CHECK(bench.half().backend.holdLaidAsideMs() == 0);
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() > 290u * 1000u);
}

TEST_CASE("a sleep shorter than the wait: what is left of it is owed, and no more") {
  // 100 seconds on. The wait was 300, so about 200 are still owed - no less than the 200 really
  // left, because this end may never believe more time passed than did, and no more than the 300
  // it would have owed with nothing measured at all.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench);
  const int before = heartbeatsSent(bench);
  const Pass& pass = theWakeAfter(bench, 100, att::kObsRingNearlyFull);

  CHECK(pass.reached == interfaces::Reached::kWait);
  CHECK(heartbeatsSent(bench) == before);
  // What was put back at the boot: no less than the two hundred seconds really left, because
  // this end may never believe more time passed than did, and no more than the three hundred it
  // would have owed with nothing measured at all.
  const std::uint32_t owed_at_boot =
      bench.half().backend.holdOwedBeforeSleepMs() - bench.half().backend.timeAwayMs();
  CHECK(owed_at_boot >= 200u * 1000u);
  CHECK(owed_at_boot <= 300u * 1000u);
  // And what the wake answers with is that, less the seconds the wake has itself spent getting
  // as far as the exchange - the attention window, the radio, the modem.
  CHECK(pass.owed_s <= 300);
  CHECK(pass.owed_s + pass.awake_s >= 200);
  CHECK(bench.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kRadio);
  // And the entry of that wake says what measured it, which is what `wake` prints a boot later.
  const core::cycle::WakeEntry& newest = bench.duty().reason().at(0);
  CHECK((newest.did & core::cycle::kAwayByRadio) != 0);
  CHECK((newest.did & core::cycle::kAwayByTimer) == 0);
  CHECK(has(bench.v().runLine("wake"), "(the time away: the radio)"));
}

TEST_CASE("another boot_id at the boot: the radio restarted too, so the wait is owed whole") {
  // The radio's seconds are only the same seconds if it is the same boot of the radio. One that
  // restarted while the board slept has counted from 0 since some moment nothing here knows, so
  // its uptime says nothing about the sleep - and the answer is the one this firmware had before
  // it measured anything.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench);
  const int before = heartbeatsSent(bench);
  bench.world.rak.restart(false);  // no EV_BOOT: the Walter was asleep and missed it
  const Pass& pass = theWakeAfter(bench, 400, att::kObsRingNearlyFull);

  CHECK(pass.reached == interfaces::Reached::kWait);
  CHECK(heartbeatsSent(bench) == before);
  CHECK(pass.owed_s > 290);
  CHECK(bench.duty().reason().timeAwayMs() == 0);
  CHECK(bench.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
  CHECK(has(bench.v().runLine("wake"), "(the time away: nothing measured it)"));
}

TEST_CASE("an uptime below the one written down: 0, and not a span that ran backwards") {
  // The same radio by its boot_id and a smaller uptime is a radio this end does not understand.
  // 0 is the safe answer and the only one: a negative span would be a wait called back early.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench);
  bench.world.rak.uptime_s = kUptimeBeforeSleep - 50;
  bench.world.rak.raiseAttention(att::kObsRingNearlyFull, 3500, 3);
  const Pass& pass = bench.oneWake(interfaces::WokeBy::kRadioLine);

  CHECK(pass.reached == interfaces::Reached::kWait);
  CHECK(pass.owed_s > 290);
  CHECK(bench.duty().reason().timeAwayMs() == 0);
  CHECK(bench.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
}

TEST_CASE("a state saved with no reading after it: the wait is owed whole") {
  // The reading belongs to the state it was taken after. A wake that saves its state and never
  // reaches the sleep - a crash, a watchdog, a restart for an update - leaves none, and the
  // reading an *earlier* sleep wrote must not be used against the newer state: it was taken
  // before it, so it would measure from too early a moment and believe more time passed than
  // did. Here the radio's uptime is 1000 seconds further on, and none of it is claimed.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench);
  // A boot that is not a timer wake, and in it one exchange by hand: it saves the state again,
  // which voids the reading. Then the board is restarted before it can sleep.
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  REQUIRE(bench.duty().reason().hasRadioReading());
  interfaces::BackendAnswer out;
  (void)bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 1800, &out);
  CHECK_FALSE(bench.duty().reason().hasRadioReading());

  bench.world.rak.uptime_s = kUptimeBeforeSleep + 1000;
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  REQUIRE(bench.duty().wake().wake == core::cycle::Wake::kReset);
  CHECK(bench.half().backend.timeAwayMs() == 0);
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() > 290u * 1000u);
  CHECK(bench.half().backend.holdLaidAsideMs() == 0);
  CHECK(bench.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kNothing);
}

// -- a wake a person asked for ---------------------------------------------------------------------

TEST_CASE("a wake a person asked for owes no wait, however short the sleep was") {
  // Ten seconds on, and a wait of three hundred: measured or not, the wait would still be owed.
  // Reason 4 is the one that does not wait at all - somebody is standing at a screen on
  // meshmaster.dk waiting to hear from this unit, and a failed call of the wake before is not an
  // answer to them.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench);
  const int before = heartbeatsSent(bench);
  const Pass& pass = theWakeAfter(bench, 10, att::kPersonAsked);

  CHECK(pass.reached == interfaces::Reached::kAnswered);
  CHECK(heartbeatsSent(bench) == before + 1);
  // What was laid aside is what was left after the ten seconds came off it, and not a figure
  // invented for the line: 300 s less about nine, less what this wake has itself spent.
  CHECK(bench.half().backend.holdLaidAsideMs() > 280u * 1000u);
  CHECK(bench.half().backend.holdLaidAsideMs() < 292u * 1000u);
  CHECK(std::string(bench.half().backend.holdLaidAsideWhy()) == "a person asked for this Walter");
  // Nothing else of the library's state was lost: the failed call of the wake before is still
  // counted. A wait laid aside is not a unit that has forgotten it failed - the heartbeat this
  // wake posted is what put the run of failures back to 0, which is a success and not a reset.
  CHECK(bench.half().loop.state().counters.httpFailures > 0);
}

TEST_CASE("a timer wake that finds reason 4 pending in STATUS does not wait either") {
  // §8: "a pending attention stays visible in STATUS until the Walter has made a request, so a
  // Walter that woke for its own reasons sees it too". That is the other way a person's press
  // reaches this board - the line was disarmed by spurious wakes, or the board was awake already
  // - and step 4 is where it is read. The wait is laid aside in the exchange that follows, which
  // is why both ways are answered in one place.
  // A Retry-After of half an hour, because a timer wake's own measure is already 810 seconds of
  // the quarter-hour sleep and a wait of 300 would be spent by it: this one is still owed when
  // the attention is found, which is the case the criterion asks for.
  AgentBench bench(nullptr, false);
  aWaitThenASleep(bench, "1800");
  const int before = heartbeatsSent(bench);
  bench.world.rak.override_status_attention = true;
  bench.world.rak.status_attention = att::kPersonAsked;
  const Pass& pass = bench.oneWake();

  REQUIRE(pass.reason == core::cycle::Wake::kTimer);
  REQUIRE(pass.attention == att::kPersonAsked);
  CHECK(pass.reached == interfaces::Reached::kAnswered);
  CHECK(heartbeatsSent(bench) == before + 1);
  // The Retry-After lengthened the sleep as well (step 10 asks the timer for at least what the
  // seam owes), so the timer was asked for 1800 s and believed 1620 of them: about 180 s of the
  // wait were still owed when this wake found the attention, and they are what was laid aside.
  CHECK(bench.half().backend.holdLaidAsideMs() > 150u * 1000u);
  CHECK(bench.half().backend.holdLaidAsideMs() < 200u * 1000u);
  CHECK(has(bench.v().runLine("agent state"),
            "s of a wait was laid aside: a person asked for this Walter\n"));
}

TEST_CASE("a timer wake with no attention keeps the figure and the behaviour it had") {
  // The one path this task did not touch: the timer's own measure, which is the seconds the last
  // sleep asked for less `kTimerErrorPercent`. The radio is not asked anything about it.
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.world.rak.override_uptime = true;
  bench.world.rak.uptime_s = kUptimeBeforeSleep;
  (void)bench.oneWake();
  bench.world.rak.uptime_s = kUptimeBeforeSleep + 4000;  // a radio that says far more than it slept
  const Pass& pass = bench.oneWake();

  REQUIRE(pass.reason == core::cycle::Wake::kTimer);
  CHECK(bench.duty().reason().awayFrom() == core::cycle::WakeReason::AwayFrom::kTimer);
  // 900 s asked for, less a tenth: the timer's figure, and not the radio's 4000.
  CHECK(bench.duty().reason().timeAwayMs() == 810u * 1000u);
  CHECK((bench.duty().reason().at(0).did & core::cycle::kAwayByTimer) != 0);
  CHECK((bench.duty().reason().at(0).did & core::cycle::kAwayByRadio) == 0);
}

// -- what a person reads ------------------------------------------------------------------------

TEST_CASE("agent state names the source of the time away, and a wait that was set aside") {
  AgentBench bench(nullptr, false);

  SUBCASE("the radio") {
    aWaitThenASleep(bench);
    (void)theWakeAfter(bench, 400, att::kObsRingNearlyFull);
    const std::string said = bench.v().runLine("agent state");
    INFO(said);
    CHECK(has(said, " s away, by the radio's uptime\n"));
    CHECK_FALSE(has(said, "was laid aside"));
  }
  SUBCASE("the timer") {
    bench.modemWorks();
    (void)bench.oneWake();
    (void)bench.oneWake();
    const std::string said = bench.v().runLine("agent state");
    INFO(said);
    CHECK(has(said, "810 s away, by the sleep timer\n"));
  }
  SUBCASE("nothing, and a wait a person's wake set aside") {
    aWaitThenASleep(bench);
    bench.world.rak.restart(false);
    (void)theWakeAfter(bench, 400, att::kPersonAsked);
    const std::string said = bench.v().runLine("agent state");
    INFO(said);
    CHECK(has(said, "nothing could measure how long this board was away\n"));
    CHECK(has(said, "s of a wait was laid aside: a person asked for this Walter\n"));
  }
}
