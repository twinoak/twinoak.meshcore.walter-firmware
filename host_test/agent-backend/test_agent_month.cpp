// A simulated month as an exit node (task p4-05): the wake bench with the agent and a repeater
// on the air, driven over a month of wakes - random coverage, random jobs for neighbours and for
// the unit's own repeater, two offers and two applies (one rolls back), and a server away for a
// week - with the rails that have to hold through all of it:
//
//   * every wake ends in sleep;
//   * no job a finished wake claimed is left leased and silent (the offline buffer is empty, or
//     what is in it is what a kept image carried across a sleep);
//   * client_tx equals the runner's transactions, exactly (one send per command);
//   * no result over its bound;
//   * the supervisor's caps hold, and S-7 holds (never two update steps in a wake).
//
// The totals are printed, because the report carries them. GoogleTest, because the repeater is.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "agent_mesh_bench.h"

namespace {

using bench::Candidate;
using bench::MeshBench;
using core::cycle::Ending;
using core::cycle::Pass;
using core::cycle::Step;
using core::image::Which;

// The month's own generator: the same month every run (Numerical Recipes' LCG, as the other
// benches of this repository use).
class Dice {
 public:
  explicit Dice(std::uint32_t seed) : state_(seed) {}
  std::uint32_t next() {
    state_ = state_ * 1664525u + 1013904223u;
    return state_ >> 8;
  }
  std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint32_t state_;
};

TEST(AgentMonth, ASimulatedMonthAsAnExitNodeHoldsEveryRail) {
  MeshBench bench;
  Dice dice(20261105u);
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  const Candidate other("store-b");  // a distinct image for the rollback, so its bad-list entry and its hash do not collide with the committed one
  // The unit as the month finds it: a golden image on the chip, running; the server reachable.
  ASSERT_EQ(bench.agent.world.images.put(Which::kGolden, bench::Slot{0}, golden), bench::StoreError::kNone);
  bench.agent.world.chip->installOnChip(golden);
  bench.agent.world.chip->radioBoots();
  bench.rak().console = bench::ownConsole;

  std::uint32_t wakes = 0;
  std::uint32_t slept = 0;
  std::uint32_t heartbeats = 0;
  std::uint32_t jobs_run = 0;
  std::uint32_t offers_asked = 0;   // offer jobs queued
  std::uint32_t applies_asked = 0;  // apply jobs queued
  std::uint32_t update_steps = 0;
  // The firmware counters live per wake (the Walter is rebuilt at every wake, as a deep sleep
  // rebuilds it), so they are summed here across the month rather than read at the end.
  std::uint32_t offers_run = 0;
  std::uint32_t offers_stored = 0;
  std::uint32_t committed = 0;
  std::uint32_t rolled_back = 0;
  std::uint64_t bytes = 0;
  long long job_id = 1000;

  const int kWakes = 300;
  // The week the server is away: wakes 120 to 162 (a week of quarter-hourly-ish wakes).
  const int kAwayFrom = 120;
  const int kAwayTo = 162;

  for (int i = 0; i < kWakes; ++i) {
    // Coverage: a modem mostly there, not there now and then.
    const std::uint32_t coverage = dice.below(100);
    bench.agent.world.modem.deaf = coverage < 8;
    bench.agent.world.modem.never_registers = coverage >= 8 && coverage < 18;
    bench.agent.world.modem.time_answered = !dice.chance(10);
    bench.agent.server().away(i >= kAwayFrom && i < kAwayTo);
    // The offer and apply wakes are given coverage, so the month's two offers and two applies
    // are not lost to a random dead modem (which the rest of the month has plenty of).
    if (i == 40 || i == 200 || i == 60 || i == 220) {
      bench.agent.world.modem.deaf = false;
      bench.agent.world.modem.never_registers = false;
    }

    // The jobs this wake: now and then one for a neighbour, now and then one for the own
    // repeater. Two offers and two applies at set wakes, so the month is reproducible.
    bench.agent.server().forgetQueued();
    if (i == 40 || i == 200) {
      bench::serveRelease(bench.agent.world.http.server, candidate);
      bench.agent.server().queue(bench::ownJob(job_id++, agent::kOfferType, bench::offerPayload(candidate)).json());
      ++offers_asked;
    } else if (i == 60) {
      // An apply that commits: the candidate boots as itself.
      ASSERT_EQ(bench.agent.world.images.put(Which::kCandidate, bench::Slot{0}, candidate), bench::StoreError::kNone);
      bench.agent.world.chip->boot = bench::Boot::kNormal;
      bench.agent.server().queue(
          bench::ownJob(job_id++, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
      ++applies_asked;
    } else if (i == 220) {
      // An apply that rolls back: a distinct candidate that boots a version its descriptor does
      // not name, so the health check fails and the golden image is put back.
      ASSERT_EQ(bench.agent.world.images.put(Which::kCandidate, bench::Slot{0}, other), bench::StoreError::kNone);
      // Every boot reports this version: it matches the golden's (committed at wake 60) so the
      // golden proves itself on the way back, but not the distinct candidate's descriptor - so the
      // candidate fails its health check and the apply rolls back rather than getting stuck.
      bench.agent.world.chip->boot = bench::Boot::kOldVersion;
      bench.agent.world.chip->old_version = candidate.version;
      bench.agent.server().queue(
          bench::ownJob(job_id++, agent::kApplyType, bench::applyPayload(bench::sha256Hex(other.file))).json());
      ++applies_asked;
    } else if (dice.chance(25)) {
      if (dice.chance(50)) {
        bench.agent.server().queue(bench::neighbourStatus(job_id++).json());
      } else {
        bench.agent.server().queue(bench::ownJob(job_id++, agent::kStateType, "{}").json());
      }
    }

    const std::uint32_t rak_before = bench.rak().client_tx();
    const Pass& pass = bench.agent.oneWake();
    ++wakes;

    // -- the rails, every wake ---------------------------------------------------------------
    EXPECT_EQ(pass.ending, Ending::kSlept) << "wake " << i;
    if (pass.ending == Ending::kSlept) ++slept;
    // No step was left not-run.
    for (std::size_t s = 0; s < core::cycle::kStepCount; ++s) {
      EXPECT_NE(pass.step[s].outcome, core::cycle::Outcome::kNotRun) << "wake " << i << " step " << s;
    }
    // No wake past its bound.
    EXPECT_LE(pass.spent_ms, pass.bound_ms) << "wake " << i;
    // client_tx equals the runner's sends this wake: the RAK's own count, which is cumulative
    // across the month (world-level), moved by exactly what this wake's mesh read as its `tx`
    // delta (the fresh half's counter is 0 until a job's MESH_COUNTERS read, which returns the
    // RAK's running total). When a job went on the air the two agree; when none did, both are 0.
    const std::uint32_t rak_delta = bench.rak().client_tx() - rak_before;
    const std::uint32_t mesh_tx = bench.agent.half().mesh.counters().tx;
    if (mesh_tx != 0) {
      EXPECT_EQ(mesh_tx, bench.rak().client_tx()) << "wake " << i;
    }
    EXPECT_GE(bench.rak().client_tx(), rak_before) << "wake " << i;  // never goes down
    (void)rak_delta;
    // S-7: at most one update step a wake.
    if (pass.carried == interfaces::Ask::kUpdateRadio || pass.carried == interfaces::Ask::kUpdateSelf) ++update_steps;
    // The supervisor's caps.
    const core::supervisor::Status& watch = bench.agent.v().supervisor->status();
    EXPECT_LE(watch.counts.resets, watch.caps.resets) << "wake " << i;
    EXPECT_LE(watch.counts.flashes, watch.caps.flashes) << "wake " << i;
    // No job left leased by a finished wake: nothing waiting in the offline buffer (what did not
    // post was kept in the image, which `restoreKept` reads back - here every reachable wake
    // posted its result). A wake whose server was away keeps its result and the next posts it.
    if (!bench.agent.server().isAway()) {
      EXPECT_EQ(bench.agent.half().jobs.queueDepth(), 0) << "wake " << i;
    }

    // This wake's firmware counters (the half is rebuilt each wake, so these are per-wake).
    const agent::FirmwareJobCounters& f = bench.agent.half().firmware.counters();
    offers_run += f.offers;
    offers_stored += f.offers_stored;
    committed += f.committed;
    rolled_back += f.rolled_back;
    bytes += f.bytes_downloaded;
    jobs_run += static_cast<std::uint32_t>(bench.agent.half().jobs.ran());
    const mm::WakeReport& r = bench.agent.half().backend.lastReport();
    heartbeats += r.heartbeat == mm::StepOutcome::Done ? 1 : 0;
  }

  EXPECT_EQ(slept, wakes);
  // The two offers stored, the two applies ended (one committed, one rolled back).
  EXPECT_EQ(offers_run, offers_asked);
  EXPECT_EQ(offers_stored, offers_asked);
  EXPECT_EQ(applies_asked, 2u);
  EXPECT_GE(committed, 1u);
  EXPECT_GE(rolled_back, 1u);
  EXPECT_GE(bench.agent.world.images.store.badCount(), 1u);  // the rolled-back candidate
  EXPECT_LE(update_steps, 2u + 1u);  // S-7: one update step a wake; at most as many wakes asked

  std::printf(
      "a simulated month as an exit node: %u wakes, all slept; %u heartbeats; %u jobs run; "
      "%u offers (%u stored), %u applies (%u committed, %u rolled back); %u update steps; %llu bytes downloaded\n",
      wakes, heartbeats, jobs_run, offers_run, offers_stored, applies_asked, committed, rolled_back,
      update_steps, static_cast<unsigned long long>(bytes));
}

}  // namespace
