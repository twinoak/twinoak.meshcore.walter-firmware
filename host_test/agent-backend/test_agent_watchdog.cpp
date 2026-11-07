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
// With `kOneTickMs` back at 10 - the code of 7 November - the stretch measured here is **39,999
// ms**, thirteen hundred times the budget below and 1.33 times the watchdog (report p9-01, *What
// it measured*). The two seconds are p8-01's figure, fifteen times inside the watchdog's thirty.
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

// What the whole job spends: the **34 lines** of agent_mesh_bench.h - the login, `region` and
// `region default`, the twenty-eight `get`s, `ver`, `board` and the access-list request - and one
// more, because the local door answers no access list at link 1.1 (`LinkMesh::localRequest`: the
// library's rule is that the ACL's silence is *no evidence either way*) and `readAccessList` asks
// with `retry`, so that one request costs two. The board spent the same thirty-five on
// 7 November. Measured, not predicted: if agent-core's pacing changes, this figure is what says
// so (the note in tasks/phase-9/FOLLOWUPS.md).
constexpr std::uint32_t kTransactionsOfTheRead = 35;

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
  // Most of a job at this budget is waiting, and the waiting is what the fault was made of.
  EXPECT_GT(gate.totalWaitMs(), 900u * 1000u);

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
  // And it blocked a great many times rather than once: the waiting is where it must happen.
  EXPECT_GT(blocking, 900u * 1000u);
}

}  // namespace
