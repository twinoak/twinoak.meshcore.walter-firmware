// A wake takes jobs until none are left or the battery says sleep (task p18-02), with a repeater
// on the air - so a job costs transactions and takes minutes of airtime, which is the half the
// cases on the doctest bench cannot see.
//
// Two things are only reachable here:
//
//   *no transactions*  the last reason of the seven. A `config.read` of the server's own key list
//                      is 38 transactions at `normal`'s cap of 38 (`agent::intent::kNormal`), so
//                      the claim after it has nothing to pay a job with - and that is not a
//                      failure, it is a wake that has spent what its band promised.
//   the airtime        a wake of several claims is paced by the budget the heartbeat's reply
//                      handed out, so the jobs of the second claim queue behind the first's on
//                      the air exactly as two jobs of one claim do.
//
// GoogleTest, because the repeater is.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "agent_mesh_bench.h"
#include "core/cycle/wake_guard.h"
#include "core/cycle/wake_room.h"

namespace {

using bench::MeshBench;
using core::cycle::Ending;

std::string outcomeOf(MeshBench& bench, long long id) {
  return bench::member(bench.agent.lastResult(id), "outcome");
}

// The scene: the modem up, the neighbour's config seeded so a read is answers and not `??:`
// lines, and the server's own default airtime budget.
void theScene(MeshBench& bench) {
  bench.agent.modemWorks();
  bench::seedNeighbourConfig(bench.repeater());
}

}  // namespace

TEST(AWakeClaimsUntilTheBatterySaysSleep, TheLastReasonIsNoTransactions) {
  MeshBench bench;
  theScene(bench);
  // One `config.read` of the thirty keys the server asks for daily - which is what `normal`'s cap
  // is written from and all but a few transactions of it - and four `status` jobs behind it, each
  // in a batch of its own so that each claim is handed one. The band allows four claims; the
  // transactions run out first.
  bench.agent.server().queue(bench::neighbourConfigReadServerList(860).json());
  for (long long id = 861; id <= 864; ++id) {
    bench.agent.server().queue(bench::neighbourStatus(id).json());
  }

  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kNormal), 3600, &out);
  EXPECT_EQ(how, interfaces::Exchanged::kAsked);

  // The read ran and was answered, and so did what the rest of the cap could pay for; the claim
  // after that was not made, because what is left cannot hold a job at the cheapest price there
  // is (`mm::kTransactionsPerJobAtLeast`). That is a wake that spent what its band promised, and
  // not a failure: nothing was leased for a job it could not pay for.
  EXPECT_EQ(outcomeOf(bench, 860), "ok") << bench.agent.lastResult(860);
  const mm::WakeReport& report = bench.agent.half().backend.lastReport();
  EXPECT_GE(report.claimsMade, 1);
  EXPECT_LT(report.claimsMade, agent::intent::kClaimsNormal);
  EXPECT_GT(report.transactionsSpent, agent::intent::kNormal - mm::kTransactionsPerJobAtLeast);
  EXPECT_LE(report.transactionsSpent, agent::intent::kNormal);
  EXPECT_EQ(report.jobsNotRun, 0);
  EXPECT_EQ(bench.agent.half().backend.claimsEnded(), interfaces::Claims::kNoTransactions);
  EXPECT_GT(bench.agent.server().queuedJobs(), 0u);
}

TEST(AWakeClaimsUntilTheBatterySaysSleep, TwoClaimsOfMeshJobsArePacedByTheBudgetAndBothAreAnswered) {
  MeshBench bench;
  theScene(bench);
  // Two claims' worth of the unit's own hourly work: a `status` of the neighbour in each batch.
  bench.agent.server().queue(bench::neighbourStatus(870).json());
  bench.agent.server().queue(bench::neighbourStatus(871).json());

  const core::cycle::Pass& pass = bench.agent.oneWake();

  EXPECT_EQ(bench.agent.server().claims, 2);
  EXPECT_EQ(bench.agent.half().backend.lastReport().claimsMade, 2);
  EXPECT_EQ(bench.agent.half().backend.lastReport().jobsRun, 2);
  EXPECT_EQ(outcomeOf(bench, 870), "ok") << bench.agent.lastResult(870);
  EXPECT_EQ(outcomeOf(bench, 871), "ok") << bench.agent.lastResult(871);
  EXPECT_EQ(bench.agent.half().backend.claimsEnded(), interfaces::Claims::kNothingWaiting);
  // **And the second claim's lease was covered as the first's was**: the bound was raised at each
  // of them, and the wake went to sleep rather than being ended by the reset the bound exists for.
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_LT(pass.spent_ms, pass.bound_ms);
  EXPECT_LE(pass.bound_ms, core::cycle::kRaisedBoundMax);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().no_room, 0);
}
