// The task watchdog and the agent's waits (task p9-01).
//
// On 7 November 2026, 19:12 UTC, Walter 0.1.1 on the first unit reset in the middle of job 22032 -
// a scheduled `config.read` of the unit's **own** repeater, answered through the local door:
// `task_wdt: … IDLE0 (CPU 0) … CPU 0: main … Aborting`, a core dump to flash, a reboot. The
// decoded backtrace (tasks/phase-8/FOLLOWUPS.md, *A busy wait below one tick still starves the
// watchdog*) runs `EspClock::delayMs` ← `WalterPlatform::delayMs` ← `JobRunner::idle` ←
// `JobRunner::spend` ← `cliLine` ← `readRegions` ← `runConfigRead`.
//
// Nothing was wrong with the job. agent-core waits out the airtime budget with `delayMs(1)` in a
// loop (`JobRunner::idle`), the unit's budget is *2 a minute, gap 20 s*, and a read of 34 lines
// makes some of those waits 40 s long - past the 30 s watchdog. Every one of the 40,000
// `delayMs(1)` was `esp_rom_delay_us(1000)`, because `pdMS_TO_TICKS(1)` was zero at
// `CONFIG_FREERTOS_HZ=100`: forty seconds of the main task holding CPU 0 and IDLE0 never running.
// p8-01 had put pauses into the flasher's loop; the clock itself was still a spin below a tick.
//
// This is the test that would have caught it, and the one that holds the fix:
//
//   - the whole job, through the **real** `JobRunner` (agent-core as synced) and the **real**
//     `LinkMesh` over the simulated RAK and the library's simulated stock repeater - the local
//     door, so not a packet goes on the air;
//   - the server's own 30-key read list, which is the 34 lines of `agent_mesh_bench.h`;
//   - the budget the unit really has, which is the scripted meshmaster.dk's default;
//   - and the measurement is the fake clock's (`fakes/fake_clock.h`): the longest stretch of
//     simulated time with no **blocking** delay in it, which is what the watchdog counts. The
//     fake's rule is the board's - `delayMs(ms)` blocks when `ms >= kOneTickMs` - and p9-01's
//     change is that `kOneTickMs` is 1, because `CONFIG_FREERTOS_HZ` is 1000 and a millisecond is
//     a tick.
//
// With `kOneTickMs` back at 10 - the code of 7 November - the stretch measured here is
// **1,020,000 ms**: the whole read is one stretch, seventeen minutes of simulated time with
// fifteen blocking delays in it and none of them inside the job, thirty-four times the watchdog's
// thirty seconds. The board died in the first 40 s wait of it; the simulation runs on past the
// point the Walter rebooted, which is why the figure is the read and not the wait. After, it is
// **0 ms** with 1,020,015 blocking delays - every millisecond of the waiting is a yield. The two
// seconds are p8-01's figure, fifteen times inside the watchdog's thirty.
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <string>

#include <agent/Airtime.h>

#include "agent_mesh_bench.h"
#include "fakes/fake_clock.h"

namespace {

using bench::MeshBench;

// Fifteen times inside CONFIG_ESP_TASK_WDT_TIMEOUT_S = 30 (sdkconfig.defaults), as task p8-01's
// flasher cases use it: a longer stretch is not yet a reset, and is already a design that depends
// on nothing else being slow.
constexpr std::uint64_t kLongestStretchAllowedMs = 2000;

// What the whole job spends: **nothing**, since task p9-05. The read is still the 34 lines of
// agent_mesh_bench.h - the login, `region` and `region default`, the twenty-eight `get`s, `ver`,
// `board` and the access-list request, which the door swallows and `readAccessList` therefore
// asks twice - but every one of them goes through the local door, `LinkMesh::answersLocally()`
// says so, and the library's `spend` returns before it touches the airtime gate: no wait, no
// slot, no transaction (EXIT-NODES §7 - a transaction is an exchange *over the mesh*). The board
// spent thirty-five on 7 November, and that figure was what this file measured; the note in
// tasks/phase-9/FOLLOWUPS.md said it was what would change when the pacing did.
//
// The watchdog is still what this test is about, and the measurement below is unchanged: the fix
// that matters here is p9-01's, that a `delayMs` of a millisecond or more yields. What the
// change next door does is take the waiting away, so this case now walks the same job with the
// gate never entered - and `test_agent_local_door.cpp` is where the no-wait figures are asserted
// for their own sake.
constexpr std::uint32_t kTransactionsOfTheRead = 0;

TEST(AgentWatchdog, ALocalConfigReadAtTwoAMinuteNeverHoldsTheCpuPastTwoSeconds) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::ownConfigReadServerList(70).json());

  fakes::FakeClock& clock = bench.agent.world.clock;
  interfaces::BackendAnswer out;
  const std::uint32_t tx_before = bench.agent.half().mesh.counters().tx;
  // The measurement starts after the bench has set the scene (the hello of step 2, the chip's
  // boot): what is measured is the exchange.
  clock.resetBusyStretch();
  const std::uint64_t t0 = clock.now_ms;
  // An hour of deadline: the read is about a thousand seconds of budget waiting and the wake's
  // deadline is not what this test is about.
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 3600, &out);
  const std::uint64_t took_ms = clock.now_ms - t0;
  const std::uint64_t longest = clock.longestBusyStretchMs();
  const std::uint64_t blocking = clock.blocking_delays;

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);

  // The job was served, and it was served through the local door: nothing on the air.
  const std::string result = bench.agent.lastResult(70);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok");
  EXPECT_NE(bench.agent.lastData(70).find("TwinOak Risskov"), std::string::npos);
  EXPECT_EQ(bench.agent.half().mesh.counters().tx, tx_before);
  EXPECT_EQ(bench.rak().client_tx(), 0u);

  // The budget it was paced at is the one the unit has, and the read cost what the arithmetic
  // says: if either changes, the figures below mean something else.
  const mm::AirtimeGate& gate = bench.agent.half().jobs.gate();
  EXPECT_EQ(gate.budget().maxTransactionsPerMinute, 2);
  EXPECT_EQ(gate.budget().minGapSeconds, 20);
  EXPECT_EQ(gate.transmissions(), kTransactionsOfTheRead);
  // Most of a job at this budget used to be waiting, and the waiting was what the fault was made
  // of: the gate is no longer entered for a job the door answers, so there is none.
  EXPECT_EQ(gate.totalWaitMs(), 0u);

  // The fault, and the fix. A single 40 s wait is enough to fail this.
  std::cout << "a 34-line local config.read at 2 a minute, gap 20 s: " << took_ms
            << " ms simulated, " << gate.transmissions() << " transactions, "
            << gate.totalWaitMs() << " ms of them waiting for the budget; longest stretch with no "
            << "blocking delay " << longest << " ms; " << blocking << " blocking delays; "
            << bench.agent.half().platform.delays() << " platform delays, "
            << bench.agent.half().platform.pumps() << " pumps\n";
  EXPECT_LE(longest, kLongestStretchAllowedMs)
      << "the config read ran " << longest << " ms of simulated time without letting the idle task "
      << "run; the task watchdog is 30 s";
  // And whatever it did block for, it blocked: nothing of the job spun.
  EXPECT_GT(blocking, 0u);
}

// The same read, for a repeater that is **not** the unit's own (task p9-05).
//
// Task p9-05 took the budget wait away from the local read above, which took p9-01's measurement
// with it: the case that used to walk a thousand seconds of `delayMs(1)` now walks none, so it no
// longer measures the thing it was written to measure. **This is where that measurement lives
// now.** The same thirty keys, the same budget, over the air: thirty-four transactions, the 20 s
// gap waited out between them, about seventeen minutes of simulated time - and not one stretch of
// it longer than two seconds without a yield. With `kOneTickMs` back at 10, the code of
// 7 November, this is the case that reports 1,020,000 ms and fails.
TEST(AgentWatchdog, TheSame34LineReadForANeighbourStillPacesAndStillYields) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench::seedNeighbourConfig(bench.repeater());
  bench.repeater().grant(bench::kOwnKeyHex, mm::kPermAdmin);
  bench.agent.server().queue(bench::neighbourConfigReadServerList(71).json());

  fakes::FakeClock& clock = bench.agent.world.clock;
  interfaces::BackendAnswer out;
  clock.resetBusyStretch();
  const std::uint64_t t0 = clock.now_ms;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 3600, &out);
  const std::uint64_t took_ms = clock.now_ms - t0;
  const std::uint64_t longest = clock.longestBusyStretchMs();
  const std::uint64_t blocking = clock.blocking_delays;

  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  const std::string result = bench.agent.lastResult(71);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(bench::member(result, "outcome"), "ok") << result;
  EXPECT_NE(bench.agent.lastData(71).find("Grenaa Chimney #1"), std::string::npos);

  // It went on the air, and it was paced: the gate took every transaction and the waiting is
  // most of the job.
  const mm::AirtimeGate& gate = bench.agent.half().jobs.gate();
  EXPECT_EQ(gate.budget().maxTransactionsPerMinute, 2);
  EXPECT_EQ(gate.budget().minGapSeconds, 20);
  EXPECT_GE(gate.transmissions(), 34u);
  EXPECT_GT(gate.totalWaitMs(), 900u * 1000u);
  EXPECT_GT(bench.rak().client_tx(), 0u);

  std::cout << "a 34-line config.read of a neighbour at 2 a minute, gap 20 s: " << took_ms
            << " ms simulated, " << gate.transmissions() << " transactions, "
            << gate.totalWaitMs() << " ms of them waiting for the budget; longest stretch with no "
            << "blocking delay " << longest << " ms; " << blocking << " blocking delays\n";
  EXPECT_LE(longest, kLongestStretchAllowedMs)
      << "the config read ran " << longest << " ms of simulated time without letting the idle task "
      << "run; the task watchdog is 30 s";
  // Every millisecond of that waiting is a yield, which is p9-01's whole change.
  EXPECT_GT(blocking, 900u * 1000u);
}

}  // namespace
