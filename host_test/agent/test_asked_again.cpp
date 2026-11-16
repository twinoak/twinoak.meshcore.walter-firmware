// What a unit in service says about the one more ask (task p15-03).
//
// `host_test/core/net/test_read_again.cpp` is the rule itself - the three answers, the one ask per
// call, what the record writes down. This is the other half: a whole Walter, a wake whose receive
// fails the way the unit in service fails one, and the two totals
//
//   * printed by `agent calls` and carried by the report's `calls` section;
//   * kept across a deep sleep, a restart and a power cut, in one value of the settings store;
//   * written only when one of them moved - and that is **not** a wake in which a call failed: an
//     ask that recovered the answer made its call come back 2xx, so there is nothing in the list
//     of failures to hang it beside, and it would be lost without a key of its own;
//   * forgotten with the list, so that a unit after `agent forget` says what a new unit says.
//
// They are the only way to tell, on a unit nobody has a cable to, whether this rule ever does
// anything - which is the whole of the task's fourth deliverable.
#include <doctest.h>

#include <string>

#include "agent/agent_bench.h"
#include "core/net/call_record.h"
#include "core/net/read_again.h"
#include "walter_agent/call_log.h"

using bench::AgentBench;

namespace {

bool has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// The one line of `agent calls` these totals print, out of the whole output.
std::string theLine(const std::string& said) {
  const std::size_t at = said.find("  asked again after a failed receive:");
  if (at == std::string::npos) return std::string();
  const std::size_t end = said.find('\n', at);
  return said.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// 16 November's receive, on every connection this socket makes, with the modem still holding the
// answer when it is asked a second time - the case the task exists for.
void theStallThatRecovers(AgentBench& bench) {
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.not_yet_receives = 2;
  socket.not_yet_ms = 50;           // the board's kReceivePollMs
  socket.receive_hangs_ms = 30000;  // the library's command timeout
  socket.receive_state = 2;         // WALTER_MODEM_STATE_TIMEOUT: nothing came back at all
  socket.receive_cme = -1;
  socket.ring_after_ms = 100;
  socket.ring_bytes = 878;          // the length every lost answer had at the server
  socket.close_after_ms = 500;
  socket.second_ask = sim::SecondAsk::kHeld;
}

}  // namespace

TEST_CASE("agent calls says how often the modem was asked again and how often that answered") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallThatRecovers(bench);
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  const std::string line = theLine(said);
  REQUIRE_FALSE(line.empty());
  CHECK(has(line, "since this unit was made"));
  CHECK(has(line, "of them answered"));
  // Every call of that wake had its answer read by the second ask, so every one of them came back
  // 2xx and the list of failures is empty - which is exactly why these two need a key of their own.
  CHECK(has(said, "no call has failed since the list began"));
  CHECK_FALSE(bench.world.kv.has(core::net::kCallRecordKey));
  CHECK(bench.world.kv.has(core::net::kReadAgainKey));
  // Numbers only: no token, no route, no byte of a body (spec/SYSTEM.md S-12).
  CHECK_FALSE(has(line, "Bearer"));
  CHECK_FALSE(has(line, bench.v().settings.serverToken()));
  CHECK_FALSE(has(line, "/agent/v1/"));

  // **A sleep.** The next boot reads the pair back out of the store.
  bench.boot();
  bench.v().begin();
  CHECK(theLine(bench.v().runLine("agent calls")) == line);

  // **A restart**, which is a boot that did not come from sleep.
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  CHECK(theLine(bench.v().runLine("agent calls")) == line);

  // **A power cut**, which takes RTC memory and leaves the settings store: the reason this is in
  // the store at all, and the moment a person asks the question these totals answer.
  bench.world.powerCut();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  bench.v().begin();
  CHECK(theLine(bench.v().runLine("agent calls")) == line);
}

TEST_CASE("a unit whose receives have never failed says the modem has never been asked again") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK(theLine(said) == "  asked again after a failed receive: never yet");
  // And no flash spent on it: a unit whose modem behaves never writes this key in its life.
  CHECK_FALSE(bench.world.kv.has(core::net::kReadAgainKey));
}

TEST_CASE("the report's `calls` section carries the two totals") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallThatRecovers(bench);
  bench.server().queue(
      bench::ownJob(28401, agent::kReportType, "{\"sections\":[\"calls\"]}").json());
  (void)bench.oneWake();

  const std::string data = bench.lastData(28401);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(has(data, "\"name\":\"calls\""));
  CHECK(has(data, "asked again after a failed receive: "));
  CHECK(has(data, "of them answered"));
  CHECK(has(data, "\"cut\":false"));
  CHECK_FALSE(has(data, "Bearer"));
}

TEST_CASE("forgetting the list forgets the two totals with it") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallThatRecovers(bench);
  (void)bench.oneWake();
  REQUIRE(bench.world.kv.has(core::net::kReadAgainKey));

  bench.half().calls.forget();
  CHECK_FALSE(bench.world.kv.has(core::net::kReadAgainKey));
  CHECK(theLine(bench.v().runLine("agent calls")) == "  asked again after a failed receive: never yet");
}
