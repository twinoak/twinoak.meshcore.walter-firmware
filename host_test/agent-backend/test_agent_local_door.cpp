// The local door as the library now understands it (tasks p9-05, p9-06 and p10-01).
//
// `LinkMesh::answersLocally()` is the Walter's override of the virtual the exit-node firmware's
// tasks p1.10-40 and p1.10-41 added to `mm::IMesh`, and these are the cases it is for - each
// through the **real** `JobRunner` (agent-core as synced, at 6290e092) and the **real**
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

// ---------------------------------------------------------------------------------------------
// A local job is priced as local (task p10-01; the exit-node firmware's p1.10-43)
//
// Task p9-05 told the library a job was local **while it ran**, so it waited for no slot and
// spent none. The wake prices a claimed job **before** it runs it, from a different gate, and
// priced it as air: `n x (minGapSeconds + loginMs)`. A 34-line `config.read` of the unit's own
// repeater came out at 34 x (20 s + 8 s) = **952 s** against the exchange's 1 200 s bound
// (`kBackendBoundMs`) - 248 s of margin on the dearest job this unit really runs, and a shorter
// bound, a longer key list or a larger `minGapSeconds` from the server would have refused, in
// seconds and at no transaction, the very job the door had just made fast. That is
// tasks/phase-9/FOLLOWUPS.md's *The wake still prices a local job as air*.
//
// `LinkMesh::answersLocallyFor()` is this firmware's override of the virtual 43 added, and these
// three cases are the line it draws: the same job on both sides of a wake too small for the air
// price, and the refusal when a refusal is the right answer.

namespace {

// The two prices, from the library's own arithmetic (`AgentLoop::estimateJobMs`) at the unit's
// own budget - *2 a minute, gap 20 s* - for the server's thirty-key read, which is 34
// transactions (`agent_mesh_bench.h`). Named, so that a test fails if either moves.
constexpr std::uint32_t kReadTransactions = 34;
constexpr std::uint32_t kReplyMs = 8000;      // the claim's `timeouts.loginMs` (support/Jobs.h)
constexpr std::uint32_t kGapMs = 20000;       // the server's `minGapSeconds`
constexpr std::uint32_t kLocalPriceS = kReadTransactions * kReplyMs / 1000u;            // 272
constexpr std::uint32_t kAirPriceS = kReadTransactions * (kGapMs + kReplyMs) / 1000u;   // 952
static_assert(kLocalPriceS == 272, "the local price is the reply waits");
static_assert(kAirPriceS == 952, "the air price is the gaps and the replies");

// A wake slice between the two prices: too small for the air price, ample for the local one.
constexpr std::uint32_t kWakeSeconds = 400;
static_assert(kWakeSeconds > kLocalPriceS, "the local price has to fit this wake");
static_assert(kWakeSeconds < kAirPriceS, "and the air price must not");

}  // namespace

TEST(AgentLocalDoor, ALocalJobIsPricedAtItsRepliesSoAWakeTooSmallForTheAirPriceStillRunsIt) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownConfigReadServerList(86, false).json());

  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), kWakeSeconds, &out);

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(86);
  ASSERT_FALSE(result.empty());
  // Run, and run to its end: before this it was `failed`, *wake budget*, at no transaction.
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;
  // No *wake budget* anywhere in it. The log is a kilobyte and drops whole entries when it fills
  // (the exit-node firmware's p1.10-38), so a 34-line read's own first line is not in it - the
  // counters below are what say the door served it.
  EXPECT_FALSE(has(bench::member(result, "log"), "wake budget")) << result;
  EXPECT_EQ(bench.rak().client_tx(), 0u);
  EXPECT_EQ(bench.agent.half().jobs.gate().transmissions(), 0u);
  EXPECT_EQ(bench.agent.half().jobs.gate().totalWaitMs(), 0u);
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_jobs, 1u);
  // And the door answers the same question for the key, before any job has begun, as it does for
  // the job in progress: that agreement is the whole contract of the two virtuals.
  EXPECT_TRUE(bench.agent.half().mesh.answersLocallyFor(bench::kOwnKeyHex));
  EXPECT_FALSE(bench.agent.half().mesh.answersLocally()) << "between jobs the door is shut";
}

TEST(AgentLocalDoor, TheSameReadForANeighbourIsStillPricedAsAirAndStillRefused) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench::seedNeighbourConfig(bench.repeater());
  bench.agent.server().queue(bench::neighbourConfigReadServerList(87).json());

  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), kWakeSeconds, &out);

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(87);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "failed") << result;
  const std::string log = bench::member(result, "log");
  // The air price, to the second, and no clause: this job really would spend the gaps.
  EXPECT_TRUE(has(log, "needs about " + std::to_string(kAirPriceS) + " s")) << log;
  EXPECT_FALSE(has(log, "answered without the air")) << log;
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;
  EXPECT_EQ(bench.rak().client_tx(), 0u) << "a job refused by the wake sends nothing";
  EXPECT_FALSE(bench.agent.half().mesh.answersLocallyFor(fake::repeaterKey().c_str()))
      << "the door must not answer for a key that is not this radio's";
}

TEST(AgentLocalDoor, ALocalJobTooLargeForItsWakeIsRefusedAtTheLocalPriceAndSaysSo) {
  // The bound has not gone away, it has moved. A wake slice that cannot hold even the replies
  // refuses - and the figure and the clause are what tell a reader the bound was the wake's and
  // not the air's.
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownConfigReadServerList(88, false).json());

  interfaces::BackendAnswer out;
  (void)bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 100, &out);

  const std::string result = bench.agent.lastResult(88);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "failed") << result;
  const std::string log = bench::member(result, "log");
  EXPECT_TRUE(has(log, "needs about " + std::to_string(kLocalPriceS) + " s")) << log;
  EXPECT_TRUE(has(log, "answered without the air")) << log;
  EXPECT_FALSE(has(log, std::to_string(kAirPriceS))) << log;
  EXPECT_EQ(bench.rak().client_tx(), 0u);
}

// ---------------------------------------------------------------------------------------------
// Link 1.2: the four §5.9 answers through a whole wake (task p9-06)
//
// Before 1.2 the local door had no request that yields a status, a neighbour list, the owner info
// or an access list, so a scheduled probe of the unit's **own** repeater came back *failed* - and
// repeater 41 on meshmaster.dk stayed *stale* and had none of the graphs a status read feeds. What
// these hold is the pair: with a 1.2 radio each of them is an answer, and with a 1.1 one each is
// exactly what it was. Both through the real runner and the real `LinkMesh`, over the simulated
// RAK, with the repeater's own request handler behind it (`sim/local_door.h`: the fixtures' bytes).

TEST(AgentLocalDoor, With12AStatusOfTheOwnRepeaterIsTheFixturesReadingAtNoTransaction) {
  MeshBench bench;
  bench.radioAt12();
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownStatus(90).json());

  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(90);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  const std::string data = bench.agent.lastData(90);
  // spec/tools/linkref.py's `repeater_stats()` figures, as the fixture packs them.
  EXPECT_NE(data.find("\"batteryMv\":4012"), std::string::npos) << data;
  EXPECT_NE(data.find("\"uptimeS\":86400"), std::string::npos) << data;
  EXPECT_NE(data.find("\"noiseFloorDbm\":-112"), std::string::npos) << data;
  EXPECT_NE(data.find("\"lastRssi\":-97"), std::string::npos) << data;
  // `{"version":true}` asks for the firmware too, which is the owner info (0x07).
  EXPECT_NE(data.find("\"firmware\":\"v1.18.0\""), std::string::npos) << data;
  // Nothing on the air, nothing of the budget, and the door is what answered it.
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;
  EXPECT_EQ(bench.rak().client_tx(), 0u);
  EXPECT_EQ(bench.agent.half().jobs.gate().transmissions(), 0u);
  EXPECT_GT(bench.agent.half().mesh.linkCounters().local_requests, 0u);
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_unserved, 0u);
  EXPECT_TRUE(bench.agent.half().mesh.answersRequestsLocally());
}

TEST(AgentLocalDoor, With12ANeighboursJobPagesThroughTheDoorWithAnOffset) {
  MeshBench bench;
  bench.radioAt12(11);  // eleven rows, nine to a page: a second request at offset 9
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownNeighbours(91).json());

  interfaces::BackendAnswer out;
  (void)bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);

  const std::string result = bench.agent.lastResult(91);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  ASSERT_EQ(bench.door.page_offsets.size(), 2u) << "two pages";
  EXPECT_EQ(bench.door.page_offsets[0], 0);
  EXPECT_EQ(bench.door.page_offsets[1], 9);
  const std::string data = bench.agent.lastData(91);
  EXPECT_NE(data.find("\"prefix\":\"0111223344556677\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"prefix\":\"0b11223344556677\""), std::string::npos)
      << "the eleventh row, which only the second page carries: " << data;
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;
  EXPECT_EQ(bench.rak().client_tx(), 0u);
}

TEST(AgentLocalDoor, With12AConfigReadsAccessListIsTheFixturesEntry) {
  MeshBench bench;
  bench.radioAt12();
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownConfigReadServerList(92, false).json());

  interfaces::BackendAnswer out;
  (void)bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 3600, &out);

  const std::string result = bench.agent.lastResult(92);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  const std::string data = bench.agent.lastData(92);
  EXPECT_NE(data.find("a1b2c3d4e5f6"), std::string::npos)
      << "the fixtures' one admin entry: " << data;
  // The door answers it now, so neither of `readAccessList`'s two tries is silence - the figure
  // task p9-05 held at 2 is 0 here, which is what makes this change a visible one.
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_silent, 0u);
  EXPECT_EQ(bench::member(result, "transactions"), "0") << result;
  EXPECT_EQ(bench.rak().client_tx(), 0u);
}

TEST(AgentLocalDoor, With11TheSameThreeJobsAreExactlyWhatTheyWereBefore12) {
  // No `radioAt12()`: the radio of every case written before this task, and the first unit's own
  // until the repeater firmware's pile 3.9 reaches it. A status and a neighbour read of the unit's
  // own repeater are *failed* with *the packet could not be built*, and a `config.read`'s access
  // list is the silence the library reads as *no ACL in this snapshot*.
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownStatus(93).json());
  bench.agent.server().queue(bench::ownNeighbours(94).json());
  bench.agent.server().queue(bench::ownConfigReadServerList(95, false).json());

  interfaces::BackendAnswer out;
  for (int pass = 0; pass < 4; pass++) {
    (void)bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 3600, &out);
  }

  EXPECT_FALSE(bench.agent.half().mesh.answersRequestsLocally());
  const std::string status = bench.agent.lastResult(93);
  const std::string neighbours = bench.agent.lastResult(94);
  const std::string read = bench.agent.lastResult(95);
  ASSERT_FALSE(status.empty());
  ASSERT_FALSE(neighbours.empty());
  ASSERT_FALSE(read.empty());
  EXPECT_EQ(bench::member(status, "outcome"), "failed") << status;
  EXPECT_TRUE(has(bench::member(status, "log"), "the packet could not be built")) << status;
  EXPECT_EQ(bench::member(neighbours, "outcome"), "failed") << neighbours;
  EXPECT_EQ(bench::member(read, "outcome"), "ok") << read;
  EXPECT_TRUE(has(bench::member(read, "log"), "no ACL in this snapshot")) << read;
  // The counters of the three unserved sends and the two silences the access list's retry makes.
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_unserved, 2u)
      << "the status and the neighbour read; the owner info is never asked after a failed status";
  EXPECT_EQ(bench.agent.half().mesh.linkCounters().local_silent, 2u);
  EXPECT_EQ(bench.rak().client_tx(), 0u) << "and nothing went on the air either way";
  EXPECT_TRUE(bench.rak().local_asked.empty()) << "no LOCAL_REQUEST was sent without the bit";
}

}  // namespace
