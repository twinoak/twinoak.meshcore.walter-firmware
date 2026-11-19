// A wake outlasts what it took on (task p16-06).
//
// In the night of 17 November 2026 this unit took the hourly `status` of its own repeater at
// 01:29:53 UTC and was next heard at 01:34:56 as a new boot; the same at 02:05:24 and 02:10:28.
// The job was leased for four minutes, was taken at both wakes and answered at neither, and each
// reset was read as a watchdog and followed by half an hour of sleep: over two hours with nothing
// measured. What is known is the arithmetic - step 8 promised the exchange `kBackendBoundMs` and
// the bound on the wake was `kWakeBoundMs`, five minutes - and this file is that arithmetic, in a
// wake of the cycle's own and never through `agent wake`.
//
// **The scene.** A repeater on the air behind the RAK, a scripted meshmaster.dk, and a modem whose
// attach takes seventy seconds, because that is what makes the night's shape reachable at all: a
// four-minute job fits inside five minutes only when everything else in the wake takes under one,
// and on the unit the radio's question, the attach, the heartbeat, the claim and an observations
// call that stalls are that minute.
//
// **What the library's word for a silent radio is.** A `status` whose repeater answers nothing is
// `unreachable` and not `timeout`: the login gets two tries (`agent/LoginSequence.h`) and the job
// ends there, in about half a minute. `timeout` is the library's word for a job the **lease**
// ended - `JobRunner::runJob`'s first line and `JobRunner::spend`'s *the budget's next slot is
// past the lease* - and that is the case the cut lease of `core/cycle/wake_room.h` makes. Both are
// a result the server is told of, which is the deliverable; the report says so under its
// deviations.
//
// GoogleTest, because the repeater is.
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agent_mesh_bench.h"
#include "core/cycle/wake_guard.h"
#include "core/cycle/wake_room.h"

namespace {

using bench::MeshBench;
using core::cycle::Ending;

// An attach that takes seventy seconds, well inside step 6's own bound (`kUplinkBoundMs`, 150 s).
// It is what makes the night's shape reachable: the wake is a minute and a half old before its
// claim, so a four-minute job cannot fit the ordinary bound.
constexpr std::uint32_t kSlowAttachMs = 70000;
// How far the figures below may be off the arithmetic: a second or two of calls between the
// claim and the moment the cover is worked out.
constexpr std::uint32_t kSlackS = 10;

// The room the calls after one job are kept: a result and the observations, each at the modem's
// command timeout (`core::cycle::roomForCallsAfter`).
constexpr std::uint32_t kRoomOneJobS = 60;
constexpr std::uint32_t kRoomTwoJobsS = 90;

std::uint32_t epochNow(MeshBench& bench) {
  return bench::kNetworkEpoch + static_cast<std::uint32_t>(bench.agent.world.clock.nowMs() / 1000u);
}

// A lease `seconds` past the moment this wake's claim will be made.
//
// `epochNow()` and not the world's clock plus the wake: the unit's own idea of the time comes off
// the network at step 7 and the fake modem answers the epoch of the moment the Walter booted
// (`host_test/core/cycle/wake_bench.h`), so the seventy seconds of the attach are not in it. What
// the unit believes at its claim is this, plus the second or two the config and the heartbeat take.
std::uint32_t leaseIn(MeshBench& bench, std::uint32_t seconds) {
  return epochNow(bench) + seconds;
}

// The unit's own hourly `status` of a neighbour, leased, as job 32763 was.
fake::Job leasedStatus(long long id, std::uint32_t lease_epoch) {
  return fake::Job(id, "status")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("acl")
      .leasedUntil(lease_epoch)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{\"version\":false}")
      .asDirectNeighbour();
}

// A `config.read` of a neighbour over the air, the one job of this unit that really takes minutes:
// the server's own key list, cut to `keys`, paced by the airtime budget at a transaction every
// half-minute (`host_test/agent-backend/test_agent_watchdog.cpp` measures the whole of it).
fake::Job leasedRead(long long id, int keys, std::uint32_t lease_epoch) {
  std::string payload = "{\"keys\":[";
  for (int i = 0; i < keys; ++i) {
    if (i > 0) payload += ",";
    payload += "\"";
    payload += bench::serverReadKeys()[i];
    payload += "\"";
  }
  payload += "]}";
  return fake::Job(id, "config.read")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("admin")
      .leasedUntil(lease_epoch)
      .withAdminPassword("adm1n-pass")
      .withPayload(payload)
      .asDirectNeighbour();
}

// The scene every case shares: the modem up with the slow attach, the repeater's config seeded so
// a read is answers and not `??:` lines.
void theNight(MeshBench& bench) {
  bench.agent.modemWorks();
  bench.agent.world.modem.registers_after_ms = kSlowAttachMs;
  bench::seedNeighbourConfig(bench.repeater());
}

// **One route's receive hangs for the modem's command timeout**, as a failing observations post
// does on this unit: the knob is the socket's and the route is the server's, so the script's own
// hook is wrapped rather than replaced (`sim::FakeTlsSocket`, `receive_hangs_ms` - it overruns the
// deadline, as the library's blocking call does).
void hangTheReceiveOn(MeshBench& bench, const std::string& suffix, std::uint32_t hangs_ms) {
  sim::FakeHttp& http = bench.agent.world.http;
  const std::function<bool(const sim::SeenRequest&, sim::ScriptedReply*)> before = http.server.on_request;
  http.server.on_request = [&http, before, suffix, hangs_ms](const sim::SeenRequest& request,
                                                             sim::ScriptedReply* reply) {
    if (request.path.find(suffix) != std::string::npos) {
      http.socket.receive_hangs_ms = hangs_ms;
    } else {
      http.socket.receive_hangs_ms = 0;
    }
    return before ? before(request, reply) : false;
  };
}

std::string outcomeOf(MeshBench& bench, long long id) {
  return bench::member(bench.agent.lastResult(id), "outcome");
}

}  // namespace

// -- the night's own shape ----------------------------------------------------------------------

TEST(AWakeOutlastsWhatItTookOn, AStatusTheRadioNeverAnswersIsToldOfAndTheWakeSleeps) {
  MeshBench bench;
  theNight(bench);
  bench.repeater().beSilentAlways();
  bench.agent.server().queue(leasedStatus(32763, leaseIn(bench, 240)).json());

  const core::cycle::Pass& pass = bench.agent.oneWake();

  // The result went, and it says the radio was not reached.
  EXPECT_EQ(outcomeOf(bench, 32763), "unreachable") << bench.agent.lastResult(32763);
  // The bound covered the four minutes the wake took on, and the calls after them.
  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 1);
  EXPECT_GT(pass.bound_ms, core::cycle::kWakeBoundMs);
  // The bound counts from the boot, so it is the attach, the lease and that room.
  EXPECT_GE(pass.bound_ms / 1000u, kSlowAttachMs / 1000u + 240u + kRoomOneJobS - kSlackS);
  EXPECT_LT(pass.bound_ms / 1000u, kSlowAttachMs / 1000u + 240u + kRoomOneJobS + kSlackS);
  // And the wake went to sleep, at its ordinary interval, with no restart asked for.
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_EQ(pass.interval_s, 900u);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().no_room, 0);
}

TEST(AWakeOutlastsWhatItTookOn, TheSameWithTheObservationsCallStallingForTheCommandsTimeout) {
  MeshBench bench;
  theNight(bench);
  bench.repeater().beSilentAlways();
  hangTheReceiveOn(bench, "/observations", core::uplink::kModemCallCeilingMs);
  bench.agent.server().queue(leasedStatus(32764, leaseIn(bench, 240)).json());

  const std::uint64_t began = bench.agent.world.clock.nowMs();
  const core::cycle::Pass& pass = bench.agent.oneWake();

  EXPECT_EQ(outcomeOf(bench, 32764), "unreachable") << bench.agent.lastResult(32764);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_EQ(pass.interval_s, 900u);
  // The stall really happened: the wake is thirty seconds longer than it would have been.
  EXPECT_GT(bench.agent.world.clock.nowMs() - began, core::uplink::kModemCallCeilingMs);
}

TEST(AWakeOutlastsWhatItTookOn, AResultsCallThatIsNeverAnsweredGoesAtTheNextWake) {
  MeshBench bench;
  theNight(bench);
  bench.repeater().beSilentAlways();
  bench.agent.server().silenceRoute("/jobs/32765/result");
  bench.agent.server().queue(leasedStatus(32765, leaseIn(bench, 240)).json());

  const core::cycle::Pass& first = bench.agent.oneWake();
  // The server heard nothing of the job, and the wake still slept.
  EXPECT_TRUE(bench.agent.lastResult(32765).empty());
  EXPECT_EQ(first.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_EQ(first.interval_s, 900u);

  // ... and the result is kept, as an unsent one is: the next wake posts it.
  bench.agent.server().answerAgain();
  const core::cycle::Pass& second = bench.agent.oneWake();
  EXPECT_EQ(second.ending, Ending::kSlept);
  EXPECT_EQ(outcomeOf(bench, 32765), "unreachable") << bench.agent.lastResult(32765);
}

TEST(AWakeOutlastsWhatItTookOn, TheNightOf17NovemberTwiceOver) {
  MeshBench bench;
  theNight(bench);
  bench.repeater().beSilentAlways();
  // The same job at two wakes, as the server handed job 32763 out twice.
  bench.agent.server().queue(leasedStatus(32763, leaseIn(bench, 240)).json());

  const core::cycle::Pass& first = bench.agent.oneWake();
  EXPECT_EQ(first.ending, Ending::kSlept);
  EXPECT_EQ(first.interval_s, 900u);

  bench.agent.server().queue(leasedStatus(32763, leaseIn(bench, 240)).json());
  const core::cycle::Pass& second = bench.agent.oneWake();
  EXPECT_EQ(second.ending, Ending::kSlept);
  EXPECT_EQ(second.interval_s, 900u);

  // Two results, and the one thing the night never had: no reset, so no half-hour of sleep.
  EXPECT_EQ(bench.agent.server().results[32763].size(), 2u);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_EQ(bench.agent.duty().guard().status().overruns, 0u);
}


// -- a lease the bound is raised to cover -------------------------------------------------------

TEST(AWakeOutlastsWhatItTookOn, AJobThatCannotFinishInsideItsLeaseIsReportedTimeout) {
  MeshBench bench;
  theNight(bench);
  // A read of nineteen keys is ten minutes of airtime-budgeted transactions and the server leased
  // it five, which is the night's own arithmetic: the job cannot be finished and the one thing
  // that must still happen is that the server is told so.
  bench.agent.server().queue(leasedRead(800, 19, leaseIn(bench, 300)).json());

  const core::cycle::Pass& pass = bench.agent.oneWake();

  // The library ended it by its own rule - the next slot is past the lease - and the result went.
  EXPECT_EQ(outcomeOf(bench, 800), "timeout") << bench.agent.lastResult(800);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_EQ(pass.interval_s, 900u);
  // And it is the cover that made that possible: the wake ran minutes past the ordinary bound,
  // which is where the wakes of 17 November were reset holding this job.
  EXPECT_GT(pass.awake_s * 1000u, core::cycle::kWakeBoundMs);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 1);
}

TEST(AWakeOutlastsWhatItTookOn, AJobLeasedForElevenMinutesThatTakesTenRunsToItsEnd) {
  MeshBench bench;
  theNight(bench);
  bench.agent.server().queue(leasedRead(810, 19, leaseIn(bench, 660)).json());

  const core::cycle::Pass& pass = bench.agent.oneWake();

  EXPECT_EQ(outcomeOf(bench, 810), "ok") << bench.agent.lastResult(810);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 1);
  EXPECT_GE(pass.bound_ms / 1000u, kSlowAttachMs / 1000u + 660u + kRoomOneJobS - kSlackS);
  EXPECT_LT(pass.bound_ms / 1000u, kSlowAttachMs / 1000u + 660u + kRoomOneJobS + kSlackS);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  // And the proof that the cover is what carried it: the wake outlasted the ordinary bound by
  // minutes, which is the wake that was reset twice in the night of 17 November.
  EXPECT_GT(pass.awake_s * 1000u, core::cycle::kWakeBoundMs);
}

TEST(AWakeOutlastsWhatItTookOn, OfTwoLeasesTheLaterGoverns) {
  MeshBench bench;
  theNight(bench);
  // One claim, two jobs (`mm::kClaimMax`): four minutes and eleven.
  bench.agent.server().queue(std::vector<std::string>{
      leasedStatus(820, leaseIn(bench, 240)).json(),
      leasedRead(821, 19, leaseIn(bench, 660)).json()});

  const core::cycle::Pass& pass = bench.agent.oneWake();

  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 2);
  // The later lease and the room for two results and the observations.
  // The later lease and the room for two results and the observations.
  EXPECT_GE(pass.bound_ms / 1000u, kSlowAttachMs / 1000u + 660u + kRoomTwoJobsS - kSlackS);
  EXPECT_LT(pass.bound_ms / 1000u, kSlowAttachMs / 1000u + 660u + kRoomTwoJobsS + kSlackS);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
}

TEST(AWakeOutlastsWhatItTookOn, AnHourLongLeaseIsCutInsideTheCeiling) {
  MeshBench bench;
  theNight(bench);
  bench.agent.server().queue(leasedRead(830, 30, leaseIn(bench, 3600)).json());

  const core::cycle::Pass& pass = bench.agent.oneWake();

  // The bound went to the ceiling and no further.
  EXPECT_EQ(pass.bound_ms, core::cycle::kRaisedBoundMax);
  // And the lease the library was handed was **cut**: it ended where the room for the result's
  // call begins, and not the hour past the claim the server had put it at.
  // `host_test/core/cycle/test_wake_room.cpp` is where that second is worked out.
  EXPECT_TRUE(bench.agent.half().server.wakesRoom().cut);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 1);
  // The job ran to its end inside that, was reported, and the wake slept.
  EXPECT_EQ(outcomeOf(bench, 830), "ok") << bench.agent.lastResult(830);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
  EXPECT_LT(pass.awake_s * 1000u, core::cycle::kRaisedBoundMax);
}

TEST(AWakeOutlastsWhatItTookOn, AWakeThatClaimsNothingKeepsItsFiveMinutes) {
  MeshBench bench;
  theNight(bench);

  const core::cycle::Pass& pass = bench.agent.oneWake();

  EXPECT_EQ(pass.bound_ms, core::cycle::kWakeBoundMs);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 0);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().no_room, 0);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_FALSE(bench.agent.world.restart_asked);
}

// -- the wake ends itself, and the rail still ends one that hangs -------------------------------

TEST(AWakeOutlastsWhatItTookOn, ACallTheBoundHasNoRoomToFinishIsNotBegun) {
  MeshBench bench;
  theNight(bench);
  bench.agent.server().queue(leasedStatus(840, leaseIn(bench, 240)).json());
  // **The config call does not come back inside the wake at all.** The modem's blocking call
  // overruns whatever deadline it was given - which is what the library's 30 s command timeout
  // does on the board - and this one is given four minutes to overrun. So by the time the
  // heartbeat comes round, the ordinary bound has nothing left: nothing has been claimed yet, so
  // nothing has raised it.
  hangTheReceiveOn(bench, "/config", 250000u);

  const core::cycle::Pass& pass = bench.agent.oneWake();

  // The heartbeat was not begun - the bound had no room to finish it - so nothing was claimed and
  // the cover never happened.
  EXPECT_GE(bench.agent.half().server.wakesRoom().no_room, 1);
  EXPECT_EQ(bench.agent.half().server.wakesRoom().jobs, 0);
  EXPECT_EQ(bench.agent.server().heartbeats.size(), 0u);
  EXPECT_EQ(bench.agent.server().claims, 0);
  // What was not sent waits as it does today: the job is still on the server's queue, and the
  // wake went to sleep at its ordinary interval rather than finding one more thing to do.
  EXPECT_EQ(bench.agent.server().queuedJobs(), 1u);
  EXPECT_EQ(pass.ending, Ending::kSlept);
  EXPECT_EQ(pass.interval_s, 900u);
}

TEST(AWakeOutlastsWhatItTookOn, AWakeThatHangsPastItsBoundIsStillReset) {
  MeshBench bench;
  theNight(bench);
  // **A job with no lease at all** raises nothing: there is nothing to cover, so the wake keeps
  // its five minutes, and a job of half an hour of airtime-budgeted transactions outlasts them.
  // The rail is untouched - the bound ends the wake by reset, which on a board does not return.
  bench.agent.server().queue(leasedRead(850, 30, 0).json());
  bench.agent.boot();
  bench.agent.v().begin();

  EXPECT_TRUE(bench.agent.wakeEndedByItsBound());
  EXPECT_EQ(bench.agent.duty().guard().status().overruns, 1u);
  EXPECT_EQ(bench.agent.duty().guard().bound(), core::cycle::kWakeBoundMs);
}
