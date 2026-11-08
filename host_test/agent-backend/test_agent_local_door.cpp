// The local door as the library now understands it (task p9-05).
//
// `LinkMesh::answersLocally()` is the Walter's override of the virtual the exit-node firmware's
// tasks p1.10-40 and p1.10-41 added to `mm::IMesh`, and these are the cases it is for - each
// through the **real** `JobRunner` (agent-core as synced, at 8c24da5b) and the **real**
// `LinkMesh` over the simulated RAK, with the library's simulated stock repeater behind it:
//
//   - **the read that reset the Walter.** A 34-line `config.read` of the unit's own repeater at
//     the unit's real budget - *2 a minute, gap 20 s* - waited about 40 s before its third line
//     on 7 November 2026 and would have spent eleven minutes waiting for air it never used.
//     It now waits for nothing, spends nothing and reports 0 transactions.
//   - **the read that was refused.** The same job with **no admin password in the claim at all**,
//     which is what meshmaster.dk hands a unit at 0.1.2 for its own repeater
//     (`LocalDoor.HandsNothing`): the login is the blank one at `admin` and the job runs.
//   - **and nothing moved for a repeater that is not the unit's own**: that job is still paced by
//     the gap and still refused unsent when its claim carries no password, because that login
//     does go on the air.
//
// The watchdog's own figures are next door, in test_agent_watchdog.cpp: the same read, measured
// for the longest stretch of simulated time with no yield in it.
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <string>

#include <agent/Airtime.h>

#include "agent_mesh_bench.h"

namespace {

using bench::MeshBench;

bool has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// The line the library adds once per local job, after `beginJob` (`JobRunner::runClaimed`).
constexpr const char* kNoAirLine = "this mesh answers for this repeater without the air";
// The line the material decision adds for one (`loginMaterialFor`), and the one it used to add
// instead when the class was `admin` and the claim carried nothing.
constexpr const char* kNoPasswordLine = "answered by the board, no password needed";
constexpr const char* kNothingToLogInWith = "nothing to log in with";

TEST(AgentLocalDoor, A34LineLocalConfigReadWaitsForNoBudgetSpendsNoneAndTakesSeconds) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownConfigReadServerList(80).json());

  fakes::FakeClock& clock = bench.agent.world.clock;
  interfaces::BackendAnswer out;
  const std::uint32_t tx_before = bench.agent.half().mesh.counters().tx;
  const std::uint32_t local_jobs_before = bench.agent.half().mesh.linkCounters().local_jobs;
  const std::uint64_t t0 = clock.now_ms;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 3600, &out);
  const std::uint64_t took_ms = clock.now_ms - t0;

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(80);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  EXPECT_NE(bench.agent.lastData(80).find("TwinOak Risskov"), std::string::npos);

  // The budget it was paced at is the one the unit has - if that changes, the figures below mean
  // something else - and **nothing of it was spent**.
  const mm::AirtimeGate& gate = bench.agent.half().jobs.gate();
  EXPECT_EQ(gate.budget().maxTransactionsPerMinute, 2);
  EXPECT_EQ(gate.budget().minGapSeconds, 20);
  EXPECT_EQ(gate.transmissions(), 0u) << "a local job counts no transaction (EXIT-NODES §7)";
  EXPECT_EQ(gate.totalWaitMs(), 0u) << "and waits for no slot";
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;

  // Nothing on the air, and the door is the one that served it.
  EXPECT_EQ(bench.agent.half().mesh.counters().tx, tx_before);
  EXPECT_EQ(bench.rak().client_tx(), 0u);
  EXPECT_GT(bench.agent.half().mesh.linkCounters().local_jobs, local_jobs_before);
  // The access list is the one request the door is known to swallow, and `readAccessList` asks
  // with `retry` - so it is asked twice and answered with silence twice
  // (tasks/phase-9/FOLLOWUPS.md's item for this task). At 0 transactions both are free; this
  // holds the figure, so that a door which learns to answer it is a visible change.
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_silent, 2u);

  // Seconds, where the same read was seventeen minutes of simulated time before this change.
  std::cout << "a 34-line local config.read at 2 a minute, gap 20 s: " << took_ms
            << " ms simulated, " << gate.transmissions() << " transactions, "
            << gate.totalWaitMs() << " ms of them waiting for the budget\n";
  EXPECT_LT(took_ms, 60u * 1000u) << "the read took " << took_ms << " ms of simulated time";
}

TEST(AgentLocalDoor, AnAdminLocalJobWithNoPasswordInTheClaimRunsToItsEnd) {
  MeshBench bench;
  bench.agent.modemWorks();
  // No `credential` object at all: the claim meshmaster.dk makes for a unit's own repeater at
  // 0.1.2. Before this, the library answered `unauthorized` with nothing sent.
  bench.agent.server().queue(bench::ownCli(81, "ver", false).json());

  interfaces::BackendAnswer out;
  const std::uint32_t tx_before = bench.agent.half().mesh.counters().tx;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(81);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  const std::string log = bench::member(result, "log");
  EXPECT_TRUE(has(log, kNoPasswordLine)) << log;
  EXPECT_TRUE(has(log, "at class admin")) << log;
  EXPECT_TRUE(has(log, kNoAirLine)) << log;
  EXPECT_FALSE(has(log, kNothingToLogInWith)) << log;
  // The line really ran on the console, and nothing went on the air for it.
  EXPECT_FALSE(bench.rak().cli_lines.empty());
  EXPECT_EQ(bench.rak().cli_lines.back(), "ver");
  EXPECT_EQ(bench.agent.half().mesh.counters().tx, tx_before);
  EXPECT_EQ(bench.rak().client_tx(), 0u);
  EXPECT_EQ(bench.agent.half().jobs.gate().transmissions(), 0u);
}

TEST(AgentLocalDoor, AJobForANeighbourIsStillPacedByTheBudget) {
  MeshBench bench;
  bench.agent.modemWorks();
  // Two jobs for the repeater that is **not** the unit's own, at the unit's own budget: the
  // second transaction of the first job already has to wait out the 20 s gap.
  bench.agent.server().queue(bench::neighbourStatus(82).json());

  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(82);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  // Paced exactly as it was: a login and a request, each a transaction, with the gap between
  // them, and every one of them on the air.
  const mm::AirtimeGate& gate = bench.agent.half().jobs.gate();
  EXPECT_EQ(gate.transmissions(), 2u);
  // The gap is counted from the previous send, so what is waited is the 20 s less the time the
  // login's own exchange took (a few hundred milliseconds of the simulated road).
  EXPECT_GT(gate.totalWaitMs(), 19u * 1000u) << "the 20 s gap was waited out";
  EXPECT_EQ(bench::member(result, "transactions"), "2") << result;
  EXPECT_EQ(bench.rak().client_tx(), 2u);
  EXPECT_FALSE(has(bench::member(result, "log"), kNoAirLine));
  // And the door was not opened for it: the local counters stand still.
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_jobs, 0u);
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_requests, 0u);
}

TEST(AgentLocalDoor, AnAdminJobForANeighbourWithNoPasswordIsStillRefusedUnsent) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::neighbourCli(83, "ver", false).json());

  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(83);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "unauthorized") << result;
  const std::string log = bench::member(result, "log");
  EXPECT_TRUE(has(log, kNothingToLogInWith)) << log;
  EXPECT_FALSE(has(log, kNoPasswordLine)) << log;
  EXPECT_FALSE(has(log, kNoAirLine)) << log;
  // Nothing was sent for it: no packet, no transaction, no CLI line.
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;
  EXPECT_EQ(bench.rak().client_tx(), 0u);
  EXPECT_EQ(bench.agent.half().jobs.gate().transmissions(), 0u);
  // The job's own command ran nowhere. The console the RAK answers is the unit's **own**
  // repeater's, and the wake reads `get radio` over it for the heartbeat's radio block, so the
  // list is not empty - what must not be in it is this job's line, which would mean a claim for
  // a neighbour had been served through the local door.
  for (const std::string& line : bench.rak().cli_lines) {
    EXPECT_NE(line, "ver") << "a neighbour's CLI line ran on the unit's own repeater";
  }
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_jobs, 0u);
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_requests, 0u);
}

// The door closes with the job: `answersLocally()` is true only between a `beginJob` for the
// unit's own key and its `endJob`, which is what keeps a job for a stranger paced after a local
// one. The link suite holds the shape of that (test_link_mesh.cpp); this holds it through a
// whole wake, because a door left open is how a neighbour's job would quietly stop being paced.
TEST(AgentLocalDoor, ALocalJobLeavesTheNextJobForANeighbourPacedAndTheDoorShut) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownCli(84, "ver", false).json());
  bench.agent.server().queue(bench::neighbourStatus(85).json());

  interfaces::BackendAnswer out;
  for (int pass = 0; pass < 3; pass++) {
    (void)bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);
  }

  EXPECT_FALSE(bench.agent.half().mesh.local()) << "between jobs the door is shut";
  EXPECT_FALSE(bench.agent.half().mesh.answersLocally());
  const std::string local = bench.agent.lastResult(84);
  const std::string neighbour = bench.agent.lastResult(85);
  ASSERT_FALSE(local.empty());
  ASSERT_FALSE(neighbour.empty());
  EXPECT_EQ(bench::member(local, "outcome"), "ok") << local;
  EXPECT_EQ(bench::member(local, "transactions"), "0") << local;
  EXPECT_EQ(bench::member(neighbour, "outcome"), "ok") << neighbour;
  EXPECT_EQ(bench::member(neighbour, "transactions"), "2") << neighbour;
  // The gate saw the neighbour's two and nothing of the local job's.
  EXPECT_EQ(bench.agent.half().jobs.gate().transmissions(), 2u);
  EXPECT_EQ(bench.rak().client_tx(), 2u);
}

}  // namespace
