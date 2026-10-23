// The console's three lines into the agent's loop (task p4-05): `agent wake` - one wake of the
// loop now, the steps and the report printed - `agent jobs` and `agent state`; and a build
// without a loop answering in one line. Over the wake bench with the agent, the uplink brought
// up as a person would (`lte up`).
#include <doctest.h>

#include <string>

#include "agent/agent_bench.h"

using bench::AgentBench;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

}  // namespace

TEST_CASE("agent wake runs one wake of the loop and prints the steps and the report") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  std::string text = bench.v().runLine("agent wake");
  CHECK(has(text, "the uplink is"));  // down: 'lte up' first
  CHECK(bench.server().heartbeats.empty());

  text = bench.v().runLine("lte up");
  REQUIRE(bench.v().uplink.state() == interfaces::UplinkState::kUp);
  bench.server().queue(bench::ownJob(5, agent::kStateType, "{}").json());
  text = bench.v().runLine("agent wake");
  INFO(text);
  CHECK(has(text, "agent wake: one wake of the loop now"));
  CHECK(has(text, "config done, heartbeat done, results skipped, claim done, jobs done, observations"));
  CHECK(has(text, "1 job(s) claimed, 1 run, 0 not run"));
  CHECK(has(text, "accepted interval"));
  CHECK(has(text, "firmware jobs:"));
  CHECK(has(text, "agent wake: asked for something"));
  CHECK(bench.server().heartbeats.size() == 1);
  REQUIRE_FALSE(bench.lastResult(5).empty());
  // No token in what was printed, and none in the ring `dump` prints.
  CHECK_FALSE(has(text, "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV"));
  CHECK_FALSE(has(bench.v().runLine("dump"), "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV"));
}

TEST_CASE("agent jobs prints the last ten as the library remembers them; agent state the saved state") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  std::string text = bench.v().runLine("agent jobs");
  CHECK(has(text, "last ten  none yet"));
  CHECK(has(text, "firmware  0 offer(s)"));

  text = bench.v().runLine("agent state");
  INFO(text);
  CHECK(has(text, "agent state: not restored across the last sleep"));
  CHECK(has(text, "never heartbeated"));
  CHECK(has(text, "the sleep keeps: a flash window"));

  (void)bench.v().runLine("lte up");
  bench.server().queue(bench::ownJob(6, agent::kStateType, "{}").json());
  (void)bench.v().runLine("agent wake");
  text = bench.v().runLine("agent jobs");
  INFO(text);
  CHECK(has(text, "last ten  newest first"));
  CHECK(has(text, "twinoak.fw."));
  CHECK(has(text, "1 state(s)"));
  text = bench.v().runLine("agent state");
  CHECK(has(text, "has heartbeated"));
  CHECK(has(text, "1 heartbeat(s)"));
  // After a sleep the state is restored, and the line says so.
  (void)bench.oneWake();
  text = bench.v().runLine("agent state");
  CHECK(has(text, "agent state: restored across the last sleep"));
}

TEST_CASE("a build without the agent's loop answers the three lines in one sentence each") {
  bench::WakeBench bench;  // phase 3's bench: the null backend, no loop
  bench.v().begin();
  for (const char* line : {"agent wake", "agent jobs", "agent state"}) {
    const std::string text = bench.v().runLine(line);
    CHECK_MESSAGE(has(text, "this build has no agent loop"), line);
  }
}
