// The agent behind the seam (task p4-05, walter_agent/agent_backend.h), on the wake bench with
// the scripted meshmaster.dk: a wake with nothing waiting is one heartbeat with the intent and a
// sleep by the policy's own interval; the server's longer interval is honoured and a shorter one
// is not; the loop's state round-trips through the key-value store across a simulated deep sleep
// and a backoff is honoured by the next wake; a 401 halts the loop and raises the Walter's own
// attention; and a result the server would not take is kept in the flash window across the sleep
// and posted by the next wake, so that no job is left leased.
#include <doctest.h>

#include <string>

#include "agent/agent_bench.h"

using bench::AgentBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

TEST_CASE("a wake with nothing waiting: one heartbeat with the intent, and sleep by the policy's interval") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  const Pass& pass = bench.oneWake();
  CHECK(pass.ending == Ending::kSlept);
  // The step did something (the heartbeat, and the observation batch the §7 drain filled from the
  // radio's ring), so it is not failed and not skipped.
  CHECK(pass.outcomeOf(Step::kBackend) != Outcome::kFailed);
  CHECK(pass.outcomeOf(Step::kBackend) != Outcome::kSkipped);
  REQUIRE(bench.server().heartbeats.size() == 1);
  CHECK(bench.server().configs == 1);
  CHECK(bench.server().claims == 0);
  const std::string& hb = bench.server().heartbeats[0];
  // The intent: the policy's own interval (plenty, a quarter of an hour) and the band's cap.
  CHECK(hb.find("\"nextCheckInS\":900") != std::string::npos);
  CHECK(hb.find("\"maxTransactionsThisWake\":" + std::to_string(agent::intent::kPlenty)) != std::string::npos);
  CHECK(hb.find(std::string("\"companionPublicKey\":\"") + bench::kOwnKeyHex + "\"") != std::string::npos);
  CHECK(hb.find("\"transport\":\"native\"") != std::string::npos);
  // The four firmware types are advertised after the thirteen.
  CHECK(hb.find("\"twinoak.fw.offer\",\"twinoak.fw.apply\",\"twinoak.fw.state\",\"twinoak.power.cycle\"") !=
        std::string::npos);
  // The sleep is the policy's: the server accepted what was proposed, so nothing is longer.
  CHECK(pass.interval_s == 900);
  CHECK_FALSE(pass.lengthened);
  CHECK(bench.server().lastAccepted() == 900);
  // The heartbeat is the trial's third condition, told.
  CHECK(bench.v().trial.reached());
}

TEST_CASE("the server's accepted interval is honoured only when it is longer: the battery wins") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().accept(7200);
  Pass pass = bench.oneWake();
  CHECK(pass.interval_s == 7200);
  CHECK(pass.lengthened);
  CHECK(bench.half().backend.counters().longer_sleeps == 1);

  bench.server().accept(60);
  pass = bench.oneWake();
  CHECK(pass.interval_s == 900);  // the policy's quarter of an hour, not the server's minute
  CHECK_FALSE(pass.lengthened);
}

TEST_CASE("the loop's state round-trips through the key-value store across a simulated deep sleep") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  CHECK_FALSE(bench.half().backend.stateSurvived());  // the first boot of the unit: nothing to restore
  (void)bench.oneWake();
  REQUIRE(bench.world.kv.has("agent_state"));
  CHECK(bench.half().backend.counters().state_saved == 1);

  // A sleep: the Walter is destroyed and built again; the state comes back out of the store.
  (void)bench.oneWake();
  CHECK(bench.half().backend.stateSurvived());
  const mm::WakeState s = bench.half().loop.state();
  CHECK(s.everHeartbeated);
  CHECK(s.counters.heartbeats == 2);
  CHECK(s.pollIntervalS == 900);
  // A power cut takes the key-value store with it only when it is RTC memory; NVS keeps it.
  CHECK(bench.world.kv.has("agent_state"));
}

TEST_CASE("a server that asks for a wait: the sleep is lengthened, and the next wake honours the hold") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(503, "3600");
  const Pass& pass = bench.oneWake();
  // The heartbeat did not come back: the exchange failed, and the sleep is at least the hour the
  // server asked for.
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kFailed);
  CHECK(pass.lengthened);
  CHECK(pass.interval_s == 3600);
  CHECK(bench.half().backend.counters().holds == 1);
  CHECK(bench.half().backend.lastReport().sleepAtLeastS == 3600);

  // A wake inside the hold (the clock has not moved an hour): the state carried the wait, and
  // nothing is sent.
  bench.boot();
  bench.v().begin();
  const std::size_t heartbeats = bench.server().heartbeats.size();
  interfaces::BackendAnswer out;
  const interfaces::Exchanged how = bench.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 60, &out);
  CHECK(how == interfaces::Exchanged::kNothing);
  CHECK(bench.server().heartbeats.size() == heartbeats);
  CHECK(std::string(bench.half().backend.lastReport().why).find("wait") != std::string::npos);
  CHECK(out.sleep_at_least_s > 0);
  CHECK(bench.half().backend.stateSurvived());
}

TEST_CASE("a 401 halts the loop, raises the Walter's own attention, and the next wake sends nothing") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(401);
  Pass pass = bench.oneWake();
  CHECK(bench.half().backend.lastReport().halted);
  // The letter `u` in the record: the token was refused (task p4-04's attention).
  CHECK((pass.agent_attention & core::cycle::agent_attention::kTokenRefused) != 0);
  bench.server().answerAgain();
  const std::size_t heartbeats = bench.server().heartbeats.size();
  pass = bench.oneWake();
  CHECK(bench.server().heartbeats.size() == heartbeats);  // halted: a 401 is still a 401 after a sleep
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kNothing);
}

TEST_CASE("a result the server would not take is kept across the sleep and posted by the next wake") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(bench::ownJob(41, agent::kStateType, "{}").json());
  bench.server().failResults(503, 1);
  Pass pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kDone);
  CHECK(bench.half().backend.lastReport().jobsRun == 1);
  CHECK(bench.server().results_refused == 1);
  CHECK(bench.lastResult(41).empty());
  // The result waits in the offline buffer, and the buffer went to the flash window.
  CHECK(bench.half().backend.lastReport().keptImageWritten);
  CHECK(bench.world.agent->kept_flash.erasesOfSector(0) == 1);

  // The next wake: the image read back, the result posted first, under the id it was made with.
  pass = bench.oneWake();
  CHECK(bench.half().backend.counters().kept_restored == 1);
  CHECK(bench.half().backend.lastReport().resultsFlushed == 1);
  REQUIRE_FALSE(bench.lastResult(41).empty());
  CHECK(bench::member(bench.lastResult(41), "outcome") == "ok");
  CHECK(bench.half().jobs.queueDepth() == 0);
}

TEST_CASE("a unit before its repeater: the heartbeat carries no key and no job types, and the identity appears") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // A radio that has no application yet answers HELLO with no mesh-client capability (nothing to
  // carry) and its IDENTITY is a key of all zeros: the heartbeat then carries no key and an
  // empty jobTypes, as a unit registered before its repeater sends (the p4-04 case, over a wake).
  bench.world.rak.caps = core::link::cap::kCli | core::link::cap::kPower | core::link::cap::kEnv |
                         core::link::cap::kAttention;
  bool no_key = true;
  bench.world.rak.on_request = [&no_key](const core::link::Packet& packet, std::vector<std::uint8_t>* payload) {
    if (packet.type != core::link::type::kIdentity || !no_key) return false;
    payload->assign(1 + core::link::kPubkeySize + 1, 0);
    return true;
  };
  (void)bench.oneWake();
  REQUIRE(bench.server().heartbeats.size() == 1);
  // No key, and no mesh job types - the radio carries none. The four `twinoak.*` types are the
  // extension's and are advertised all the same: they concern the unit and its store, not a
  // neighbour, and the server routes them only once the repeater is linked (a deviation from
  // p4-04's empty list, in the report).
  CHECK(bench.server().heartbeats[0].find("companionPublicKey") == std::string::npos);
  CHECK(bench.server().heartbeats[0].find("\"status\"") == std::string::npos);
  CHECK(bench.server().heartbeats[0].find("\"twinoak.fw.offer\"") != std::string::npos);
  no_key = false;
  (void)bench.oneWake();
  REQUIRE(bench.server().heartbeats.size() == 2);
  CHECK(bench.server().heartbeats[1].find(std::string("\"companionPublicKey\":\"") + bench::kOwnKeyHex) !=
        std::string::npos);
  CHECK(bench.server().heartbeats[1].find("\"twinoak.fw.state\"") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// The wait sleeps too (task p10-01)
//
// A wait the server asked for is a number of seconds of **real** time. Until the exit-node
// firmware's task p1.10-42, `restore()` re-based what was left of it against the new boot's
// `millis()` and the sleep in between counted for nothing, so a 30 s hold was spent five seconds
// at a time across six short wakes, each of which sent nothing: the first unit woke 56 times on
// the night of 8 November 2026 and meshmaster.dk heard from it on about every sixth wake.
//
// `restore(state, awayMs)` now takes the time away off the hold, and this firmware measures it -
// `core::cycle::WakeReason::timeAwayMs()`, the seconds the last sleep asked its timer for less
// the timer's own error. These are the cases through the **real** `AgentBackend`, the real
// library, and the state really round-tripped through the key-value store.

namespace {

// What a sleep of `slept_s` is believed to have lasted, in milliseconds: the arithmetic
// core/cycle/wake_reason.h applies, written out so that a change to `kTimerErrorPercent` moves
// these figures and not the rule. The measure's own unit tests are in
// host_test/core/cycle/test_time_away.cpp.
std::uint32_t believedMs(std::uint32_t slept_s) {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(slept_s) * 1000u *
                                    (100u - core::cycle::kTimerErrorPercent) / 100u);
}

}  // namespace

TEST_CASE("a failed call and a quarter of an hour's sleep: the next wake heartbeats") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(503);
  Pass pass = bench.oneWake();
  // The wake that failed: a hold, and a sleep of the band's quarter of an hour - which is longer
  // than the hold, so it is what the timer is asked for.
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kFailed);
  CHECK(pass.interval_s == 900);
  CHECK(bench.half().loop.state().consecutiveFailures > 0);

  // The next wake, on the timer, with the server answering again. **It heartbeats**: the sleep
  // outlasted the hold, so nothing of it is owed. (What the hold was is read here and not before
  // the sleep: the fake sleeper advances the clock by the seconds it was asked for, so `state()`
  // afterwards reports what is left of the hold *now*, which is nothing. The figure the store
  // really carried across is what `restore()` was handed.)
  bench.server().answerAgain();
  const std::size_t before = bench.server().heartbeats.size();
  pass = bench.oneWake();
  CHECK(bench.half().backend.stateSurvived());
  CHECK(bench.half().backend.timeAwayMs() == believedMs(900));
  const std::uint32_t owed_ms = bench.half().backend.holdOwedBeforeSleepMs();
  CHECK(owed_ms > 0);
  CHECK(owed_ms < bench.half().backend.timeAwayMs());  // which is why nothing is owed now
  CHECK(bench.server().heartbeats.size() == before + 1);
  CHECK(bench.half().backend.lastReport().heartbeat == mm::StepOutcome::Done);
  CHECK(bench.half().backend.lastReport().why[0] == 0);  // not "a wait ... is still running"
  CHECK(bench.half().backend.lastReport().sleepAtLeastS == 0);
  CHECK(pass.outcomeOf(Step::kBackend) != Outcome::kFailed);
  // The run of failures was ended by an answer; while it lasted, it lasted through the sleep.
  CHECK(bench.half().loop.state().consecutiveFailures == 0);
}

TEST_CASE("the same at the hour the normal band sleeps") {
  AgentBench bench(nullptr, false);
  // A pack between the lower and the upper threshold: the `normal` band, an hour between wakes -
  // this unit's usual interval, and the one that made one failed call six hours of silence.
  bench.world.rak.battery_mv = 3800;
  bench.modemWorks();
  bench.server().failWith(503);
  Pass pass = bench.oneWake();
  REQUIRE(pass.band == core::power::Band::kNormal);
  REQUIRE(pass.interval_s == 3600);

  bench.server().answerAgain();
  const std::size_t before = bench.server().heartbeats.size();
  pass = bench.oneWake();
  CHECK(bench.half().backend.timeAwayMs() == believedMs(3600));
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() > 0);
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() < bench.half().backend.timeAwayMs());
  CHECK(bench.server().heartbeats.size() == before + 1);
  CHECK(bench.half().backend.lastReport().heartbeat == mm::StepOutcome::Done);
}

TEST_CASE("a wake on the radio's line soon after: the rest of the hold is still owed") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // A wait the server named, long enough that no wake could outlive it: what this case is about
  // is that a line wake claims none of the sleep, not how much of it was left.
  bench.server().failWith(503, "1800");
  Pass pass = bench.oneWake();
  REQUIRE(pass.outcomeOf(Step::kBackend) == Outcome::kFailed);
  REQUIRE(pass.interval_s == 1800);

  // The radio's line, not the timer. How much of the sleep had passed when the line moved is the
  // one thing this end cannot know, so **0** is passed and the wait is owed whole - which is
  // exactly the behaviour this firmware had before it measured any sleep at all.
  bench.server().answerAgain();
  const std::size_t before = bench.server().heartbeats.size();
  pass = bench.oneWake(interfaces::WokeBy::kRadioLine);
  CHECK(bench.duty().reason().wake() != core::cycle::Wake::kTimer);
  CHECK(bench.half().backend.stateSurvived());
  CHECK(bench.half().backend.timeAwayMs() == 0);
  const std::uint32_t owed_ms = bench.half().backend.holdOwedBeforeSleepMs();
  CHECK(owed_ms > 1700u * 1000u);
  CHECK(bench.server().heartbeats.size() == before);  // nothing was sent
  CHECK(std::string(bench.half().backend.lastReport().why).find("wait") != std::string::npos);
  // The whole of the wait is still owed - within the seconds this wake has itself spent.
  CHECK(bench.half().backend.lastReport().sleepAtLeastS > owed_ms / 1000u - 120u);
  CHECK(bench.half().backend.lastReport().sleepAtLeastS <= owed_ms / 1000u);
}

TEST_CASE("a Retry-After of half an hour and a quarter of an hour's sleep: the rest is still held") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(503, "1800");
  bench.v().begin();
  interfaces::BackendAnswer out;
  REQUIRE(bench.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out) ==
          interfaces::Exchanged::kFailed);
  const std::uint32_t owed_ms = bench.half().loop.state().holdForMs;
  REQUIRE(owed_ms > 1700u * 1000u);

  // A sleep **shorter than the wait**. Through the wake cycle that cannot happen - it lengthens
  // every sleep to a hold the seam carried out (core/cycle/wake_cycle.h) - and through a person's
  // `sleep 900` on the console it can: a quarter of an hour, where half an hour was asked for.
  (void)bench.duty().decide();
  REQUIRE(bench.duty().sleepFor(900).refusal == core::cycle::SleepRefusal::kNone);

  bench.server().answerAgain();
  bench.boot();
  bench.v().begin();
  CHECK(bench.half().backend.timeAwayMs() == believedMs(900));
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() == owed_ms);
  // The wait was **shortened, not forgotten**: what is left is what the sleep did not cover, and
  // the wake still sends nothing.
  const std::size_t before = bench.server().heartbeats.size();
  const interfaces::Exchanged how =
      bench.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);
  CHECK(how == interfaces::Exchanged::kNothing);
  CHECK(bench.server().heartbeats.size() == before);
  CHECK(std::string(bench.half().backend.lastReport().why).find("wait") != std::string::npos);
  const std::uint32_t left_s = (owed_ms - believedMs(900)) / 1000u;
  CHECK(left_s > 0);
  CHECK(out.sleep_at_least_s <= left_s);
  CHECK(out.sleep_at_least_s + 120u > left_s);  // less by the seconds this wake has itself spent
  CHECK(out.sleep_at_least_s < owed_ms / 1000u);  // the remainder, not the whole
}

TEST_CASE("agent state says what the sleep took off the hold") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(503, "1800");
  (void)bench.oneWake();

  bench.server().answerAgain();
  bench.boot();
  bench.v().begin();
  const std::uint32_t owed_ms = bench.half().backend.holdOwedBeforeSleepMs();
  REQUIRE(owed_ms > 1700u * 1000u);
  const std::string restored = bench.v().runLine("agent state");
  INFO(restored);
  CHECK(restored.find("agent state: restored across the last sleep") != std::string::npos);
  const std::string owed = "; " + std::to_string(owed_ms / 1000u) + " s owed before the sleep, " +
                           std::to_string(believedMs(1800) / 1000u) + " s slept\n";
  CHECK(restored.find(owed) != std::string::npos);
  // One line: the clause is a second write of the same one, because 200 bytes is all a line of
  // this console may be - so the newline comes after the clause and not before it.
  const std::size_t hold = restored.find("hold ");
  REQUIRE(hold != std::string::npos);
  CHECK(restored.find('\n', hold) > restored.find(owed));

  // A wake on the radio's line restores the same state and can claim none of the sleep, and the
  // line says so rather than printing a figure it has not got.
  bench.boot(interfaces::WokeBy::kRadioLine);
  bench.v().begin();
  const std::string line_wake = bench.v().runLine("agent state");
  INFO(line_wake);
  CHECK(line_wake.find("no sleep this end could measure") != std::string::npos);
  CHECK(line_wake.find(" s slept") == std::string::npos);
}
