// What is kept for the asking (task p13-02).
//
// On 13 November 2026 meshmaster.dk showed *httpFailures 512* for the unit and nobody could say
// which calls had failed, with what, or when; and a wake request had reached the unit's radio,
// the radio had answered *ok*, and the unit had not checked in. Three things the unit forgot are
// kept now, and these are the tests of them:
//
//   * **the last eight failed calls** - which call, what came back, the wake's number and when -
//     recorded where the uplink makes the call, kept in the settings store, and the same after a
//     deep sleep and after a power cut; a ninth pushing the oldest out; a 401 and a 429 in it,
//     which the library's own `httpFailures` leaves out;
//   * **why a wake sent nothing**, in the list of ten: a wait still owed and its seconds, and the
//     uplink's backoff - the two a wake on the radio's line used to leave nowhere at all;
//   * **four counters in the heartbeat** - the timer, the radio's line, a spurious wake, a silent
//     one - with the right values after each kind of wake, across a sleep and a power cut, and
//     inside meshmaster.dk's bound on the whole `errors` object;
//   * `twinoak.report`'s `calls` section, which is the lines of `agent calls`;
//   * and **the flash**: a wake with no failed call and nothing new to say writes no value of the
//     store it did not write before.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "fakes/sim_key_value.h"
#include "walter_agent/call_log.h"

using bench::AgentBench;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

namespace {

bool has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// Where `needle` is in `text`, for a test about *newest first*.
std::size_t at(const std::string& text, const std::string& needle) {
  const std::size_t found = text.find(needle);
  REQUIRE_MESSAGE(found != std::string::npos, needle);
  return found;
}

// The `errors` object of a heartbeat body, braces and all: what meshmaster.dk serialises and
// bounds at `AgentLimits.Json` (`src/MeshMaster.Core/Transport/Agents.cs`, 4 000 characters).
std::string errorsObjectOf(const std::string& body) {
  const std::size_t key = body.find("\"errors\":");
  REQUIRE(key != std::string::npos);
  const std::size_t open = body.find('{', key);
  REQUIRE(open != std::string::npos);
  const std::size_t close = body.find('}', open);
  REQUIRE(close != std::string::npos);
  return body.substr(open, close - open + 1);
}

// The numbered entries of `agent calls`, without the two lines above them: what a boot that
// recorded nothing of its own still has to show.
std::vector<std::string> entriesOf(const std::string& said) {
  std::vector<std::string> lines;
  std::size_t from = 0;
  while (from < said.size()) {
    const std::size_t end = said.find('\n', from);
    const std::string line = said.substr(from, end == std::string::npos ? end : end - from);
    if (line.size() > 4 && line[0] == ' ' && line[1] == ' ' && line[2] >= '1' && line[2] <= '9' &&
        line[3] == '.') {
      lines.push_back(line);
    }
    if (end == std::string::npos) break;
    from = end + 1;
  }
  return lines;
}

}  // namespace

// -- the last eight failed calls ------------------------------------------------------------------

TEST_CASE("a heartbeat answered 503 and a claim that never came back are both in the list") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();

  // **Two wakes and not one.** The task's criterion names one wake with both failures in it, and
  // the copied library will not run one: a heartbeat that did not come back ends the wake there
  // ("there is no accepted interval, no `jobsWaiting` to believe and nothing to be gained by
  // putting the rest of the wake on a link that just failed", `AgentLoop::wake`). So the two are
  // scripted as the two wakes they have to be - which is also what makes the wake's number in
  // each entry worth keeping.
  bench.server().failRoute("/heartbeat", 503, "30");
  (void)bench.oneWake();
  bench.server().answerAgain();
  bench.server().queue(bench::ownJob(31, "status", "{}").json());  // so the wake claims
  bench.server().silenceRoute("/jobs/claim");
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK(has(said, "did not come back 2xx"));
  // Newest first: the claim of the second wake above the heartbeat of the first. (The entries
  // and not the words: the first line of all names `heartbeat` in *the heartbeat's
  // httpFailures*, which is the counter this list exists beside.)
  CHECK(at(said, "claim, wake ") < at(said, "heartbeat, wake "));
  // Each with its call, what came back, and the wake's number.
  CHECK(has(said, "heartbeat, wake "));
  CHECK(has(said, "http 503"));
  CHECK(has(said, "claim, wake "));
  CHECK(has(said, "nothing answered, "));
  const agent::CallLog& log = bench.half().calls;
  REQUIRE(log.count() == 2);
  CHECK(log.at(0).what == agent::CallKind::kClaim);
  CHECK(log.at(0).status == 0);
  CHECK(log.at(1).what == agent::CallKind::kHeartbeat);
  CHECK(log.at(1).status == 503);
  // The two wakes are two numbers, and the newer one is the larger.
  CHECK(log.at(0).wake > log.at(1).wake);
  // And each is dated by this unit's own clock, which the uplink gave it at step 6.
  CHECK(log.at(0).at_epoch > 0);

  // The same after a deep sleep: the list is in the settings store, which a sleep keeps. The
  // entries, not the whole text - the line above them counts what *this boot* recorded and wrote,
  // and a boot that recorded nothing is the point.
  const std::vector<std::string> before = entriesOf(said);
  REQUIRE(before.size() == 2);
  bench.boot();
  bench.v().begin();
  CHECK(entriesOf(bench.v().runLine("agent calls")) == before);
  CHECK(bench.half().calls.survived());

  // And after a power cut, which takes RTC memory and leaves the store (which is why the list is
  // not in RTC memory - and RTC memory has no slot left either).
  bench.world.powerCut();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  bench.v().begin();
  const agent::CallLog& after = bench.half().calls;
  REQUIRE(after.count() == 2);
  CHECK(after.at(0).what == agent::CallKind::kClaim);
  CHECK(after.at(1).what == agent::CallKind::kHeartbeat);
  CHECK(after.at(1).status == 503);
}

TEST_CASE("a 401 and a 429 are in the list, which the library's httpFailures leaves out") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // A 429 first: a 401 halts the loop and nothing is sent after it.
  bench.server().failRoute("/heartbeat", 429, "5");
  (void)bench.oneWake();
  bench.server().answerAgain();
  bench.server().failRoute("/heartbeat", 401);
  (void)bench.oneWake();

  const agent::CallLog& log = bench.half().calls;
  REQUIRE(log.count() == 2);
  CHECK(log.at(0).status == 401);
  CHECK(log.at(1).status == 429);
  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK(has(said, "http 401"));
  CHECK(has(said, "http 429"));
  CHECK(has(said, "401 and 429 included"));
}

TEST_CASE("a ninth failed call pushes the oldest out, and the store holds the eight") {
  fakes::SimKeyValue kv;
  agent::CallLog log(&kv);
  log.begin();
  CHECK_FALSE(log.survived());
  CHECK(log.count() == 0);

  log.atWake(7);
  for (int i = 1; i <= 9; ++i) {
    log.record(agent::CallKind::kHeartbeat, static_cast<std::uint16_t>(500 + i),
               interfaces::HttpOutcome::kOk, 1794595200u + static_cast<std::uint32_t>(i));
  }
  REQUIRE(log.count() == agent::kCallsKept);
  CHECK(log.counters().recorded == 9);
  CHECK(log.counters().pushed_out == 1);
  // Newest first, and the first of the nine is gone.
  CHECK(log.at(0).status == 509);
  CHECK(log.at(agent::kCallsKept - 1).status == 502);

  // One write, and the eight read back byte for byte on the next boot.
  CHECK(log.save());
  CHECK(kv.writes == 1);
  agent::CallLog again(&kv);
  again.begin();
  CHECK(again.survived());
  REQUIRE(again.count() == agent::kCallsKept);
  for (std::size_t i = 0; i < agent::kCallsKept; ++i) {
    CHECK(again.at(i).what == log.at(i).what);
    CHECK(again.at(i).status == log.at(i).status);
    CHECK(again.at(i).wake == log.at(i).wake);
    CHECK(again.at(i).at_epoch == log.at(i).at_epoch);
  }

  // A value of another shape is refused whole rather than read as a short list: a store written
  // by another firmware, or a flipped character.
  kv.put(agent::kCallsKey, "not hex at all");
  agent::CallLog refused(&kv);
  refused.begin();
  CHECK_FALSE(refused.survived());
  CHECK(refused.count() == 0);
  CHECK(refused.counters().refused == 1);
}

TEST_CASE("what did not answer is kept with how far the uplink had got, and nothing else") {
  fakes::SimKeyValue kv;
  agent::CallLog log(&kv);
  log.begin();
  log.atWake(70000);  // past what the field holds: clamped, never wrapped to a small number
  log.record(agent::CallKind::kDownload, 0, interfaces::HttpOutcome::kTlsFailed, 0);
  REQUIRE(log.count() == 1);
  CHECK(log.at(0).what == agent::CallKind::kDownload);
  CHECK(log.at(0).outcome == interfaces::HttpOutcome::kTlsFailed);
  CHECK(log.at(0).wake == agent::kWakeAtMost);
  CHECK(log.at(0).at_epoch == 0);
  CHECK(log.save());
  agent::CallLog again(&kv);
  again.begin();
  REQUIRE(again.count() == 1);
  CHECK(again.at(0).outcome == interfaces::HttpOutcome::kTlsFailed);
  CHECK(again.at(0).wake == agent::kWakeAtMost);

  // Forgotten is forgotten: the key goes with the list.
  again.forget();
  CHECK(again.count() == 0);
  agent::CallLog empty(&kv);
  empty.begin();
  CHECK_FALSE(empty.survived());
}

// -- why a wake sent nothing ---------------------------------------------------------------------

TEST_CASE("a wake on the radio's line inside a wait says so, and how many seconds") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // A failed call leaves a wait owed. The wake after it is one the radio's line asked for, and
  // `WakeReason::timeAwayMs()` is 0 for anything but the timer, so the whole wait is still owed
  // and nothing is sent. That is the wake of 13 November, and until this task its line in `wake`
  // was `---` and nothing else.
  bench.server().away(true);
  (void)bench.oneWake();
  bench.server().away(false);
  bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 3);
  const Pass& pass = bench.oneWake(interfaces::WokeBy::kRadioLine);
  REQUIRE(pass.reason == core::cycle::Wake::kAttention);
  REQUIRE(pass.reached == interfaces::Reached::kWait);
  CHECK(pass.owed_s > 0);

  const std::string said = bench.v().runLine("wake");
  INFO(said);
  CHECK(has(said, "nothing sent: a wait still owed, "));
  CHECK(has(said, " s\n"));
  // The seconds are the wait's own, not a round number this test invented.
  CHECK(has(said, "a wait still owed, " + std::to_string(pass.owed_s) + " s"));
  // And the entry is that wake's: the newest of the list.
  const core::cycle::WakeEntry& newest = bench.duty().reason().at(0);
  CHECK(newest.sent == static_cast<std::uint8_t>(interfaces::Reached::kWait));
  CHECK(newest.owed_s == pass.owed_s);
}

TEST_CASE("a wake the uplink's backoff skipped says that, where it used to say nothing") {
  AgentBench bench(nullptr, false);
  // A modem that never registers - no coverage - is the failure whose retry **grows**
  // (`core/uplink/failure.h`: *not-registered*, "next wake, then every 2nd, 4th... capped"; a
  // silent modem is retried every wake after a reset and would never reach a skipped step). Two
  // failures put the interval at two wakes, so the wake after them does not touch the modem at
  // all. Step 8 needs step 6, so no backend answered for that wake and the record said *it did
  // not say*.
  bench.modemWorks();
  bench.world.modem.never_registers = true;
  (void)bench.oneWake();
  (void)bench.oneWake();
  bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 3);
  const Pass& pass = bench.oneWake(interfaces::WokeBy::kRadioLine);
  REQUIRE(pass.outcomeOf(Step::kUplink) == Outcome::kSkipped);
  CHECK(pass.reached == interfaces::Reached::kUplinkBackoff);

  const std::string said = bench.v().runLine("wake");
  INFO(said);
  CHECK(has(said, "nothing sent: the uplink's backoff"));
  CHECK(bench.duty().reason().at(0).sent ==
        static_cast<std::uint8_t>(interfaces::Reached::kUplinkBackoff));
  // The record line and the list say the same thing, in the same words.
  char line[224] = {};
  (void)core::cycle::recordLine(pass, line, sizeof line);
  CHECK(has(std::string(line), "nothing sent: the uplink's backoff"));
}

TEST_CASE("the reason crosses a sleep: a boot later the list still says why") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().away(true);
  (void)bench.oneWake();
  bench.server().away(false);
  bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 3);
  (void)bench.oneWake(interfaces::WokeBy::kRadioLine);
  const std::uint16_t owed = bench.duty().reason().at(0).owed_s;
  REQUIRE(owed > 0);

  // The next wake: the one before it is now the second of the list, with its reason intact.
  bench.server().away(false);
  (void)bench.oneWake();
  const core::cycle::WakeEntry& before = bench.duty().reason().at(1);
  CHECK(before.sent == static_cast<std::uint8_t>(interfaces::Reached::kWait));
  CHECK(before.owed_s == owed);
  const std::string said = bench.v().runLine("wake");
  INFO(said);
  CHECK(has(said, "nothing sent: a wait still owed, " + std::to_string(owed) + " s"));
}

// -- the four counters -----------------------------------------------------------------------------

TEST_CASE("the heartbeat carries what woke this unit, and the four survive a sleep and a power cut") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();

  // One of each kind, in order: the timer, the radio's line with a valid attention, a line wake
  // with nothing valid on it, and one that sent nothing (a wait still owed).
  (void)bench.oneWake();
  bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 3);
  REQUIRE(bench.oneWake(interfaces::WokeBy::kRadioLine).reason == core::cycle::Wake::kAttention);
  bench.world.rak.sendBytes({0x00, 0x00, 0x11, 0x22}, 0);
  REQUIRE(bench.oneWake(interfaces::WokeBy::kRadioLine).reason == core::cycle::Wake::kSpurious);
  // The silent one: a failed call leaves a wait, and the wake on the line inside it sends nothing.
  bench.server().away(true);
  (void)bench.oneWake();
  bench.server().away(false);
  bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 3);
  const Pass& silent = bench.oneWake(interfaces::WokeBy::kRadioLine);
  REQUIRE(silent.reached == interfaces::Reached::kWait);

  const core::cycle::WakeTotals& totals = bench.v().wake_counters.totals();
  CHECK(totals.timer == 2);     // the two timer wakes above
  CHECK(totals.radio == 2);     // the two valid attentions
  CHECK(totals.spurious == 1);
  CHECK(totals.silent == 1);    // the last one: the others all sent, or tried and failed

  // And the heartbeat says so. A wake that reaches the server carries the totals as they stand.
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  const std::string printed = bench.v().runLine("agent heartbeat");
  INFO(printed);
  CHECK(has(printed, "\"wakesTimer\":2"));
  CHECK(has(printed, "\"wakesRadio\":2"));
  CHECK(has(printed, "\"wakesSpurious\":1"));
  CHECK(has(printed, "\"wakesSilent\":1"));

  // A deep sleep keeps them...
  bench.boot();
  CHECK(bench.v().wake_counters.survived());
  CHECK(bench.v().wake_counters.totals().radio == 2);
  CHECK(bench.v().wake_counters.totals().spurious == 1);
  // ... and so does a power cut, which takes RTC memory and leaves the settings store.
  bench.world.powerCut();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  CHECK(bench.v().wake_counters.survived());
  CHECK(bench.v().wake_counters.totals().timer == 2);
  CHECK(bench.v().wake_counters.totals().radio == 2);
  CHECK(bench.v().wake_counters.totals().spurious == 1);
  CHECK(bench.v().wake_counters.totals().silent == 1);
}

TEST_CASE("the body a wake posts carries the four, and its errors fit the server's bound") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  (void)bench.oneWake();
  REQUIRE_FALSE(bench.server().heartbeats.empty());
  const std::string body = bench.server().heartbeats.back();
  INFO(body);
  for (const char* key : {"wakesTimer", "wakesRadio", "wakesSpurious", "wakesSilent"}) {
    CHECK(has(body, std::string("\"") + key + "\""));
  }
  // The library's six, this board's `radioIdentityLost` when the radio refuses, and these four:
  // meshmaster.dk serialises the whole object and refuses a heartbeat whose `errors` passes
  // `AgentLimits.Json` - **4 000 characters** (`src/MeshMaster.Core/Transport/Agents.cs`), which
  // takes the exit node off the air. Eleven counters are nowhere near it, and this is what holds
  // that true at the next counter somebody adds.
  constexpr std::size_t kServerJsonBound = 4000;
  const std::string errors = errorsObjectOf(body);
  MESSAGE("the heartbeat's errors object: " << errors.size() << " characters of the server's "
          << kServerJsonBound);
  CHECK(errors.size() < kServerJsonBound);
  // And the body itself fits the buffer the uplink writes it into.
  CHECK(body.size() < agent::kHeartbeatBodyBytes);

  // The console prints the same four, from the same function: `agent heartbeat` is not a second
  // opinion about what a wake sends.
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  const std::string printed = bench.v().runLine("agent heartbeat");
  CHECK(has(printed, "\"wakesTimer\":"));
}

// -- the report's section, and the flash -----------------------------------------------------------

TEST_CASE("a claimed twinoak.report for `calls` is the lines of `agent calls`") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // One failure first, so the section has something in it.
  bench.server().failRoute("/heartbeat", 503, "30");
  (void)bench.oneWake();
  bench.server().answerAgain();
  bench.server().queue(bench::ownJob(
      28399, agent::kReportType, "{\"sections\":[\"calls\"]}").json());
  (void)bench.oneWake();

  const std::string data = bench.lastData(28399);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(has(data, "\"name\":\"calls\""));
  CHECK(has(data, "\"unknown\":[]"));
  CHECK(has(data, "agent calls: "));
  CHECK(has(data, "http 503"));
  CHECK(has(data, "heartbeat, wake "));
}

TEST_CASE("a wake with no failed call and nothing new to say writes no more than it did before") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // Two wakes first, so that nothing of the start-up is still settling: the loop's state is in
  // the store, the counters have been written once.
  (void)bench.oneWake();
  (void)bench.oneWake();

  // A wake that is none of the four counters' kinds and in which no call fails: a person's cable.
  // Nothing of this task may write a value for it.
  const int writes_before = bench.world.kv.writes;
  const Pass& pass = bench.oneWake(interfaces::WokeBy::kNotFromSleep);
  REQUIRE(pass.reached == interfaces::Reached::kAnswered);
  const agent::CallLog& log = bench.half().calls;
  CHECK(log.count() == 0);
  CHECK(log.counters().writes == 0);
  CHECK(bench.v().wake_counters.writes().writes == 0);
  // The loop's own `agent_state` is the one value a wake has always written, and the only one
  // this wake wrote: nothing of task p13-02 is behind the difference.
  const int written = bench.world.kv.writes - writes_before;
  MESSAGE("a wake with nothing new to say wrote " << written << " value(s) of the store");
  CHECK(written <= 1);

  // And a timer wake writes one more - the wake counters - because `wakesTimer` moved. That is
  // the whole of what this task costs an ordinary wake.
  const int before_timer = bench.world.kv.writes;
  (void)bench.oneWake();
  CHECK(bench.v().wake_counters.writes().writes == 1);
  CHECK(bench.world.kv.writes - before_timer <= 2);
}
