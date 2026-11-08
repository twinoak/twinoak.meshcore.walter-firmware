// The console's three lines into the agent's loop (task p4-05): `agent wake` - one wake of the
// loop now, the steps and the report printed - `agent jobs` and `agent state`; and a build
// without a loop answering in one line. Over the wake bench with the agent, the uplink brought
// up as a person would (`lte up`).
#include <doctest.h>

#include <string>

#include "core/cycle/wake_guard.h"
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

// -- the local door, where the library's own report cannot see it (task p9-04) --------------------

namespace {

// A `telemetry` for the unit's own repeater, which the local door answers: the shape
// host_test/agent-backend/agent_mesh_bench.h gives it, without the repeater behind the RAK that
// those tests need - nothing of this one depends on the reply.
fake::Job ownTelemetryJob(long long id) {
  return fake::Job(id, "telemetry")
      .forRepeater(17, bench::kOwnName, bench::kOwnKeyHex)
      .atClass("acl")
      .leasedUntil(0)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{}")
      .asDirectNeighbour();
}

// And the same for a neighbour: the key meshmaster.dk held for the unit's radio on 7 November was
// a key like this one - not this radio's, and the job went on the air because nothing said so.
fake::Job neighbourTelemetryJob(long long id) {
  return fake::Job(id, "telemetry")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("acl")
      .leasedUntil(0)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{}")
      .asDirectNeighbour();
}

}  // namespace

TEST_CASE("agent jobs says which jobs went through the local door, and names a key that is not this radio's") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  // Before any wake: the door's counters are there, at zero, and nothing has been begun.
  std::string text = bench.v().runLine("agent jobs");
  INFO(text);
  CHECK(has(text, "local     0 job(s) through the door, 0 request(s) made"));
  CHECK(has(text, "begun     no job has been begun on the link in this boot"));
  // And it begins a line of its own: the `firmware` block above it used to be one `sayf` of
  // more than the 200 bytes a line is printed into, cut at *0 pow* with its newline (task
  // p9-04), so this block ran on from the middle of a word.
  CHECK(has(text, "\nlocal     "));
  CHECK(has(text, "bytes downloaded\n"));

  (void)bench.v().runLine("lte up");
  bench.server().queue(std::vector<std::string>{ownTelemetryJob(70).json(), neighbourTelemetryJob(71).json()});
  (void)bench.v().runLine("agent wake");
  text = bench.v().runLine("agent jobs");
  INFO(text);
  // The door is open - the identity was read before the claim (task p8-04) - and the line says
  // which radio it is open for.
  CHECK(has(text, "this radio's key 7f3a9c02e1b4..."));
  CHECK(has(text, "local     1 job(s) through the door"));
  CHECK(has(text, "begun     newest first"));
  // The own repeater's job: the local door, nothing on the air.
  CHECK(has(text, "7f3a9c02e1b4...  the local door: nothing went on the air"));
  // The neighbour's: the mesh, with both keys shortened side by side. This is the line that
  // would have explained 7 November's telemetry job without the server's audit log.
  CHECK(has(text, "the mesh; NOT this radio's key 7f3a9c02e1b4..."));
  CHECK_FALSE(has(text, "no key for its own repeater"));
  // Neither block prints a whole key, and neither prints the password the own job carried.
  CHECK_FALSE(has(text, bench::kOwnKeyHex));
  CHECK_FALSE(has(bench.v().runLine("dump"), "adm1n-pass"));
}

TEST_CASE("agent id prints the local door's counters, and says when the door is shut") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  // With no session the radio has not been asked anything, so there is no key and the door is
  // shut - which is the state in which no mesh job is carried at all (task p8-04).
  std::string text = bench.v().runLine("agent id");
  INFO(text);
  CHECK(has(text, "local door: shut: no key for this radio, so no mesh job is carried at all"));

  (void)bench.v().runLine("lte up");
  bench.server().queue(ownTelemetryJob(72).json());
  (void)bench.v().runLine("agent wake");
  text = bench.v().runLine("agent id");
  INFO(text);
  CHECK(has(text, "local door: open: a job naming this radio is answered here"));
  CHECK(has(text, "1 job(s),"));
  // And the line above it is whole: seventeen job types are over three hundred characters, so
  // `job types` used to be cut at `twinoak.` by printf's line buffer and lose its newline, which
  // this line then ran on from (task p9-04).
  CHECK(has(text, "twinoak.power.cycle\n"));
  CHECK(has(text, "\nlocal door: "));
}

TEST_CASE("a console agent wake puts the wake's bound back and says what the cable would do") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  core::cycle::WakeGuard& guard = bench.duty().guard();
  REQUIRE(guard.armed());
  REQUIRE(guard.bound() == core::cycle::kWakeBoundMs);
  (void)bench.v().runLine("lte up");

  const std::string text = bench.v().runLine("agent wake");
  INFO(text);
  // The bound is back where it was: a bench session of these does not leave the rest of the boot
  // bounded at twenty minutes from the boot (tasks/phase-6/FOLLOWUPS.md's [04]).
  CHECK(guard.bound() == core::cycle::kWakeBoundMs);
  CHECK(has(text, "the wake's bound is back to 300 s from the boot"));
  CHECK(has(text, "pull the cable and this wake ends in"));
  // And the figure is the guard's own, by the same rule.
  CHECK(guard.endsAfterCableMs() >= core::cycle::kUsbGraceMs);
}
