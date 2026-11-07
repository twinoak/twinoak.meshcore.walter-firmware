// The radio known before a job is claimed (task p8-04), on the wake bench with the scripted
// meshmaster.dk. 6 November, on the first unit: `agent wake` with no `rak hello` before it ran a
// job for the unit's **own** repeater as an ordinary mesh job, so the association's discover went
// out on the air from the radio to itself. Two things were missing and both are held here:
//
//   the session   the local door is armed only once `Identity::refresh()` has the radio's key,
//                 and that needs a `HELLO` in this boot. A timer wake's step 2 opens one; the
//                 console's `agent wake` runs no step 2 and opened none. Now every wake asks
//                 (`Identity::open()`), and a wake whose step 2 has already asked asks nothing.
//   the list      with no key, `LinkMesh::supports()` carries no mesh type, so the heartbeat
//                 offers none and the server routes none - a job this end cannot tell is its own
//                 repeater's is never claimed, rather than claimed and then refused.
//
// And the fake was taught the server's own rule: `MeshMasterScript` hands over only the types the
// last heartbeat advertised, so a wake that offers no mesh type can no longer be seen running a
// mesh job in these tests (the phase's theme - every fake did what the code expected).
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"

using bench::AgentBench;
using core::cycle::Pass;

namespace {

namespace link = core::link;

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// A stranger's repeater, as the mesh jobs of this file address one: not this unit's key.
constexpr const char* kStrangerKey = "1111111111111111111111111111111111111111111111111111111111111111";

// A `discover` for the unit's own repeater - the association's own job, which is what went on the
// air on 6 November. No path, so a mesh job would flood; through the local door the synthetic
// admin login teaches it that the UART *is* the direct path and nothing is sent.
std::string ownDiscover(long long id) {
  return fake::Job(id, "discover")
      .forRepeater(17, bench::kOwnName, bench::kOwnKeyHex)
      .atClass("admin")
      .withAdminPassword("adm1n-pass")
      .leasedUntil(0)
      .withNoPath()
      .json();
}

// A `status` read of somebody else's repeater: a mesh job, and the one a wake with no key must
// not claim.
std::string strangerStatus(long long id) {
  return fake::Job(id, "status")
      .forRepeater(18, "Grenaa Chimney #1", kStrangerKey)
      .atClass("none")
      .leasedUntil(0)
      .asDirectNeighbour()
      .json();
}

// Whether any §6 send ever reached the radio. `client_tx` (§6.4) counts exactly the sends the RAK
// executed, so "no SEND_* request was made" is "client_tx did not move", read from this end. A
// JOB_BEGIN is deliberately not one of these: it names the peer and puts no packet on the air,
// and the runner begins a job on the mesh even for a firmware type.
bool anythingWentOnTheAir(const AgentBench& bench) {
  for (const sim::ScriptedRadio::Seen& one : bench.world.rak.requests) {
    if (one.type >= link::type::kSendLogin && one.type <= link::type::kSendTrace) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("agent wake with no rak hello opens the session, and the unit's own job goes through the local door") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  // The boot's own case: nothing has been asked of the radio, so there is no session, no key and
  // no local door - which is where `agent wake` was typed on 6 November.
  REQUIRE_FALSE(bench.v().link.session().up);
  REQUIRE_FALSE(bench.half().identity.known());
  REQUIRE_FALSE(bench.half().mesh.hasOwnKey());

  (void)bench.v().runLine("lte up");
  bench.server().queue(ownDiscover(60));
  const std::string text = bench.v().runLine("agent wake");
  INFO(text);

  // The wake asked for the session itself, once, and read the identity out of it.
  CHECK(bench.half().identity.counters().sessions_asked == 1);
  CHECK(bench.half().identity.counters().no_session == 0);
  CHECK(bench.half().identity.known());
  CHECK(bench.half().mesh.hasOwnKey());
  // So the job was begun for the unit's own repeater and nothing left the radio.
  CHECK(bench.half().mesh.linkCounters().local_jobs == 1);
  CHECK(bench.half().mesh.linkCounters().sends == 0);
  CHECK_FALSE(anythingWentOnTheAir(bench));
  // And it was answered: a lease the server handed out is honoured in the same wake.
  REQUIRE_FALSE(bench.lastResult(60).empty());
  CHECK(bench::member(bench.lastResult(60), "outcome") == "ok");
  // One line says which of the two wakes this was, and why.
  CHECK(has(text, "the radio is known"));
  CHECK(has(text, "goes through the local door"));
  // S-12: the key is never written down, not even in the line that says it is known.
  CHECK_FALSE(has(text, bench::kOwnKeyHex));
}

TEST_CASE("a wake before the fix: without the key no mesh type is offered, so no mesh job is claimed") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  // The radio answers HELLO - it carries MESH_CLIENT - but its IDENTITY is a key of all zeros: a
  // repeater firmware that has made no identity yet. This end cannot tell a job for its own
  // repeater from a stranger's, so it offers no mesh type at all.
  bench.world.rak.on_request = [](const link::Packet& packet, std::vector<std::uint8_t>* payload) {
    if (packet.type != link::type::kIdentity) return false;
    payload->assign(1 + link::kPubkeySize + 1, 0);
    return true;
  };
  bench.server().queue({strangerStatus(65), bench::ownJob(66, agent::kStateType, "{}").json()});
  const std::string text = bench.v().runLine("agent wake");
  INFO(text);

  CHECK(bench.v().link.session().up);
  CHECK_FALSE(bench.half().identity.known());
  CHECK_FALSE(bench.half().mesh.hasOwnKey());
  CHECK_FALSE(bench.half().mesh.supports("status"));
  REQUIRE(bench.server().heartbeats.size() == 1);
  CHECK_FALSE(has(bench.server().heartbeats[0], "\"status\""));
  CHECK(has(bench.server().heartbeats[0], "twinoak.fw.state"));
  // The server kept the mesh job and handed over the unit's own.
  CHECK(bench.lastResult(65).empty());
  REQUIRE_FALSE(bench.lastResult(66).empty());
  CHECK(bench.server().jobs_not_routed >= 1);
  CHECK(bench.server().queuedJobs() == 1);
  CHECK_FALSE(anythingWentOnTheAir(bench));
  CHECK(has(text, "the radio answered but has made no identity yet"));
  // The whole of it: the library's line buffer is 239 characters and this is the longer of the
  // two reasons, so a line that grew would be cut off exactly where it says why.
  CHECK(has(text, "is not claimed"));
}

TEST_CASE("a silent radio: the unit's own job types are still claimed and run, and no mesh job is") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // A radio that answers nothing at all: no session, no key, and a power cycle is exactly what a
  // job for one of these is for.
  bench.world.rak.deaf = true;
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  bench.server().queue({strangerStatus(70), bench::ownJob(71, agent::kStateType, "{}").json()});
  const std::string text = bench.v().runLine("agent wake");
  INFO(text);

  CHECK(bench.half().identity.counters().sessions_asked == 1);
  CHECK(bench.half().identity.counters().no_session == 1);
  CHECK_FALSE(bench.v().link.session().up);
  CHECK_FALSE(bench.half().mesh.hasOwnKey());
  // The heartbeat went out all the same - a Walter whose radio is silent is exactly the one that
  // has to reach its uplink and say so - with the four firmware types and no mesh type.
  REQUIRE(bench.server().heartbeats.size() == 1);
  const std::string& hb = bench.server().heartbeats[0];
  CHECK_FALSE(has(hb, "\"status\""));
  CHECK_FALSE(has(hb, "\"telemetry\""));
  CHECK(has(hb, "\"twinoak.fw.offer\",\"twinoak.fw.apply\",\"twinoak.fw.state\",\"twinoak.power.cycle\""));
  CHECK_FALSE(has(hb, "companionPublicKey"));
  // The firmware job ran and was answered; the mesh job was never claimed.
  REQUIRE_FALSE(bench.lastResult(71).empty());
  CHECK(bench::member(bench.lastResult(71), "outcome") == "ok");
  CHECK(bench.lastResult(70).empty());
  CHECK(bench.server().jobs_not_routed >= 1);
  CHECK(bench.server().queuedJobs() == 1);
  CHECK_FALSE(anythingWentOnTheAir(bench));
  // And the line says it was the silent radio, not a radio without an identity.
  CHECK(has(text, "the radio did not answer a HELLO"));
  CHECK(has(text, "twinoak.power.cycle"));
  CHECK(has(text, "is not claimed"));
}

TEST_CASE("the console's agent wake and a timer wake take the same path: one HELLO, and only when there is none") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(ownDiscover(80));
  // A timer wake: step 2 opened the session, so the agent's own `open()` asks for nothing - and
  // the key is read and the local door armed before the claim, exactly as on the console.
  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(core::cycle::Step::kBackend) == core::cycle::Outcome::kDone);
  CHECK(bench.half().identity.counters().sessions_asked == 0);
  CHECK(bench.half().identity.known());
  CHECK(bench.half().mesh.hasOwnKey());
  CHECK(bench.half().mesh.linkCounters().local_jobs == 1);
  CHECK_FALSE(anythingWentOnTheAir(bench));
  REQUIRE_FALSE(bench.lastResult(80).empty());
  CHECK(bench::member(bench.lastResult(80), "outcome") == "ok");
  // The same line, from the same place, in the log a timer wake leaves behind.
  CHECK(has(bench.world.agent->log, "the radio is known"));
  CHECK(has(bench.world.agent->log, "goes through the local door"));
}
