// The three counts of a flooded transaction over four wakes of a scripted unit (task p14-02),
// with the library's own simulated stock repeater behind the RAK - so that a flood is a flood,
// scoped or not, and the counting is the library's own and not a number this test sets.
//
// The acceptance criterion in one sentence: a flooded login answered in the scope in the first
// wake, one answered only without the scope in the second, one never answered in the third, and
// the fourth wake's heartbeat says 1, 1, 1. The library counts **per flooded transaction** and
// **since boot**, and this board boots at every wake, so the three can only read 1, 1, 1 at that
// fourth heartbeat if the totals crossed three deep sleeps - which is the whole of this task.
//
// GoogleTest, because the repeater is (host_test/agent-core/test/support/FakeMesh.h).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "agent_mesh_bench.h"
#include "walter_agent/flood_counters.h"

namespace {

using bench::MeshBench;
using core::cycle::Ending;
using core::cycle::Pass;

// A `status` read of a repeater this unit holds **no path for**: every send of it floods, which is
// what `.withNoPath()` says (the job builder's default is a two-hop path, and a job with a path
// goes direct and is counted by nobody).
fake::Job floodedStatus(long long id) {
  return fake::Job(id, "status")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("acl")
      .leasedUntil(0)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{\"version\":false}")
      .withNoPath();
}

// What one wake came to: the kept totals after it, and the floods the RAK put on the air **in
// that wake** - §6.2's own answer for each, which says whether the packet carried a transport
// code, so what is held is what the radio did and not what this end meant to ask for. (The RAK is
// the world's and its list is the unit's whole life, so the wake's own floods are the tail of it.)
struct Wake {
  agent::FloodTotals totals;
  std::vector<bool> floods;
  std::uint32_t unscoped_asks = 0;  // `FLOOD_SCOPE` 2 requests of this wake
};

Wake aWakeWithAFloodedJob(MeshBench& bench, long long id) {
  const std::size_t sends = bench.rak().sent.size();
  const std::uint32_t asks = bench.agent.world.rak.flood_scope_unscoped;
  bench.agent.server().forgetQueued();
  bench.agent.server().queue(floodedStatus(id).json());
  const Pass& pass = bench.agent.oneWake();
  EXPECT_EQ(pass.ending, Ending::kSlept);
  Wake out;
  // The totals of the Walter this wake ran as: a life lasts until the next boot, so these are the
  // ones that wake added to and wrote down.
  out.totals = bench.agent.half().floods.totals();
  for (std::size_t i = sends; i < bench.rak().sent.size(); ++i) {
    if (bench.rak().sent[i].flood) out.floods.push_back(bench.rak().sent[i].scoped);
  }
  out.unscoped_asks = bench.agent.world.rak.flood_scope_unscoped - asks;
  return out;
}

// The rule, over the floods of one wake: the first in the radio's scope and every one after it
// without it (agent/FloodScope.h, `floodModeFor`).
void theScopeFirstThenWithoutIt(const Wake& wake) {
  ASSERT_GE(wake.floods.size(), 2u);
  EXPECT_TRUE(wake.floods[0]) << "the first flood of a transaction goes out in the radio's scope";
  for (std::size_t i = 1; i < wake.floods.size(); ++i) {
    EXPECT_FALSE(wake.floods[i]) << "flood " << i << " of the transaction drops the scope";
  }
  // One `FLOOD_SCOPE` 2 for each of those, immediately before the send it is for (§6.5).
  EXPECT_EQ(wake.unscoped_asks, wake.floods.size() - 1);
}

std::string member(const std::string& json, const char* key) { return bench::member(json, key); }

}  // namespace

TEST(AgentFloodCounts, FourWakesOfAUnitThatFloodsAndTheHeartbeatThatSaysOneOneOne) {
  MeshBench bench;
  // A radio of link 1.3 whose `region default` is `dk`, turned before the first HELLO: the
  // session's version is what the HELLO's answer named (§5.1).
  bench.agent.world.rak.speakLink13("dk");
  bench.agent.modemWorks();
  // §6.3's path return: a flooded reply teaches this end a path. So the login is the **one**
  // flooded transaction of the job and the request after it goes direct - one count a wake, which
  // is what makes 1, 1, 1 a sentence a person can read rather than an arithmetic accident.
  bench.repeater().setReturnPath({0xa1, 0x7f}, 2);

  // -- the first wake: a flooded login, answered in the radio's scope ---------------------------
  {
    const Wake wake = aWakeWithAFloodedJob(bench, 501);
    EXPECT_EQ(member(bench.agent.lastResult(501), "outcome"), "ok");
    ASSERT_EQ(wake.floods.size(), 1u) << "the login floods and the request that follows goes direct";
    EXPECT_TRUE(wake.floods[0]) << "the first flood of a transaction goes out in the radio's scope";
    EXPECT_EQ(wake.unscoped_asks, 0u);
    EXPECT_EQ(wake.totals.answered_scoped, 1u);
    EXPECT_EQ(wake.totals.answered_unscoped, 0u);
    EXPECT_EQ(wake.totals.unanswered, 0u);
    // And it is in the store, because the next boot is the only reader of it.
    EXPECT_EQ(bench.agent.half().floods.writes().writes, 1u);
    EXPECT_FALSE(bench.agent.world.kv.raw(agent::kFloodTotalsKey).empty());
  }

  // -- the second wake: the scoped flood silent, the try without it answered --------------------
  {
    // The repeater is silent to the next send, which is the transaction's first flood. The rule
    // then drops the scope for the one after it (`FLOOD_SCOPE` 2 immediately before that send),
    // and that one is answered - which is the number this whole object exists to report.
    bench.repeater().beSilent(1);
    const Wake wake = aWakeWithAFloodedJob(bench, 502);
    bench.repeater().answerAgain();
    theScopeFirstThenWithoutIt(wake);
    EXPECT_EQ(member(bench.agent.lastResult(502), "outcome"), "ok");
    // The totals the wake before it left, plus this wake's one.
    EXPECT_EQ(wake.totals.answered_scoped, 1u);
    EXPECT_EQ(wake.totals.answered_unscoped, 1u);
    EXPECT_EQ(wake.totals.unanswered, 0u);
  }

  // -- the third wake: a flood nothing answers, in either scope ---------------------------------
  {
    bench.repeater().beSilentAlways();
    const Wake wake = aWakeWithAFloodedJob(bench, 503);
    bench.repeater().answerAgain();
    theScopeFirstThenWithoutIt(wake);
    EXPECT_NE(member(bench.agent.lastResult(503), "outcome"), "ok");
    EXPECT_EQ(wake.totals.answered_scoped, 1u);
    EXPECT_EQ(wake.totals.answered_unscoped, 1u);
    EXPECT_EQ(wake.totals.unanswered, 1u) << "it flooded and nothing came back, in either scope";
  }

  // -- the fourth wake: the heartbeat says 1, 1, 1 ----------------------------------------------
  bench.agent.server().forgetQueued();
  const std::size_t before = bench.agent.server().heartbeats.size();
  const Pass& pass = bench.agent.oneWake();
  EXPECT_EQ(pass.ending, Ending::kSlept);
  ASSERT_GT(bench.agent.server().heartbeats.size(), before);
  const std::string body = bench.agent.server().heartbeats.back();
  EXPECT_EQ(bench::objectOf(body, "flood"),
            "{\"scope\":\"dk\",\"unscopedRetry\":true,\"answeredScoped\":1,"
            "\"answeredUnscoped\":1,\"unanswered\":1}")
      << body;
  // The library's own counts in that wake are 0, 0, 0 - nothing has flooded in this boot - which
  // is exactly what this unit would have been able to say without the totals.
  EXPECT_EQ(bench.agent.half().jobs.floodCounts().answeredScoped, 0u);
  EXPECT_EQ(bench.agent.half().jobs.floodCounts().answeredUnscoped, 0u);
  EXPECT_EQ(bench.agent.half().jobs.floodCounts().unanswered, 0u);
  // And `agent state` says the same three, with the library's own words for the rule.
  const std::string state = bench.agent.v().runLine("agent state");
  EXPECT_NE(state.find("flood: scope dk, answered scoped 1 / unscoped 1 / none 1 in all"),
            std::string::npos)
      << state;
  EXPECT_NE(state.find("the second flood of a transaction goes unscoped"), std::string::npos)
      << state;
}

TEST(AgentFloodCounts, AFloodThatWentOutInAnotherScopeSaysSoUnderTheThree) {
  // The one thing that can make the three wrong, and the line that says so (the phase's
  // follow-up): the library files a transaction under the mode its rule **asked** for, so a flood
  // asked `Scoped` whose answer says it went out unscoped is counted as `answeredScoped`. Here the
  // radio's `region default` is cleared under this end's feet between the HELLO and the job, which
  // is exactly that - and `agent state` must say how many of the three could be under the wrong
  // one, because this end cannot correct the library's figure.
  MeshBench bench;
  bench.agent.world.rak.speakLink13("dk");
  bench.agent.modemWorks();
  bench.repeater().setReturnPath({0xa1, 0x7f}, 2);

  // A wake whose scope held: no second line.
  ASSERT_EQ(aWakeWithAFloodedJob(bench, 701).totals.answered_scoped, 1u);
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().scope_surprises, 0u);
  EXPECT_EQ(bench.agent.v().runLine("agent state").find("had not asked for"), std::string::npos);

  // And one whose radio stops scoping **after** the name has been read - so the next flood is
  // asked for in a scope the radio no longer holds. One exchange by hand and not a whole wake,
  // because a wake begins with a boot and a boot reads the name again.
  bench.agent.boot(interfaces::WokeBy::kTimer);
  bench.agent.v().begin();
  ASSERT_TRUE(bench.agent.v().link.hello().ok());
  ASSERT_TRUE(bench.agent.half().mesh.askFloodScope());
  bench.agent.world.rak.flood_scope.clear();
  const std::size_t sends = bench.rak().sent.size();
  bench.agent.server().forgetQueued();
  bench.agent.server().queue(floodedStatus(702).json());
  interfaces::BackendAnswer answer;
  (void)bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &answer);
  ASSERT_GT(bench.rak().sent.size(), sends);
  EXPECT_FALSE(bench.rak().sent[sends].scoped) << "the radio no longer holds a scope to put on it";
  EXPECT_GE(bench.agent.half().mesh.linkCounters().scope_surprises, 1u);
  const std::string state = bench.agent.v().runLine("agent state");
  EXPECT_NE(state.find("flood(s) went out in a scope this end had not asked for"),
            std::string::npos)
      << state;
}

TEST(AgentFloodCounts, APowerCutTakesNothingButTheWakeThatWasRunning) {
  // The same unit, two wakes and the power cut between them - which is the one thing RTC memory
  // would not have survived and the reason these three are in the settings store.
  MeshBench bench;
  bench.agent.world.rak.speakLink13("dk");
  bench.agent.modemWorks();
  bench.repeater().setReturnPath({0xa1, 0x7f}, 2);

  ASSERT_EQ(aWakeWithAFloodedJob(bench, 601).totals.answered_scoped, 1u);
  // The power goes: RTC memory is gone, the store is flash and is not.
  bench.agent.world.powerCut();
  bench.agent.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  bench.agent.v().begin();
  EXPECT_TRUE(bench.agent.half().floods.survived());
  EXPECT_EQ(bench.agent.half().floods.totals().answered_scoped, 1u);

  EXPECT_EQ(aWakeWithAFloodedJob(bench, 602).totals.answered_scoped, 2u);
}
