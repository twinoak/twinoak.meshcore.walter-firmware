// Where a wake is, written by the agent itself (task p15-06).
//
// `host_test/core/cycle/test_what_a_wake_was_doing.cpp` is the record and the boot after it, over
// the scripted backend. This is the other half: the **real** hooks, on a whole exchange with a
// repeater on the air - so that the two places a wake of the unit in service hangs are the two
// places the record is written from.
//
//   `ModemUplink::call()`         a call to the server, by the name `agent calls` prints.
//   `AgentBackend::noteProgress`  the claimed job the library has in hand, its type and how many
//                                 requests of the radio or the mesh it has made - read off
//                                 `mm::JobRunner::openJob()` and `LinkMeshCounters::requests`,
//                                 neither of which is a change to the copied library.
//   `LinkMesh::await()`           that the link is what is being waited on.
//
// The record is read **while the exchange runs**, at every wait the world takes, because that is
// the only moment it says anything: the wake here reaches sleep and the record ends up saying so.
// GoogleTest, because the repeater is.
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "agent_mesh_bench.h"
#include "core/cycle/wake_progress.h"

namespace {

using bench::MeshBench;

// Every state the record passed through during an exchange, taken at each wait of the world.
struct Seen {
  std::vector<core::cycle::WakeWhere> states;

  bool anyCallNamed(const char* name) const {
    for (const core::cycle::WakeWhere& w : states) {
      if (w.doing == core::cycle::Doing::kCall && std::string(w.what) == name) return true;
    }
    return false;
  }
  bool anyJobNamed(const char* type, std::uint8_t at_least_requests) const {
    for (const core::cycle::WakeWhere& w : states) {
      if (w.doing != core::cycle::Doing::kJob) continue;
      if (std::string(w.what) != type) continue;
      if (w.in_job && w.requests >= at_least_requests) return true;
    }
    return false;
  }
  bool anyOnTheLink() const {
    for (const core::cycle::WakeWhere& w : states) {
      if (w.on_link) return true;
    }
    return false;
  }
  std::uint8_t mostRequests() const {
    std::uint8_t most = 0;
    for (const core::cycle::WakeWhere& w : states) {
      if (w.doing == core::cycle::Doing::kJob && w.requests > most) most = w.requests;
    }
    return most;
  }
};

// The record at every wait the world takes, which is where the library's own pump runs. The hook
// the bench already has there - the repeater's own clock - is kept and run first: a repeater whose
// seconds stopped would answer nothing.
void watch(MeshBench& bench, Seen* seen) {
  const std::function<void(std::uint32_t)> before = bench.agent.world.on_tick;
  bench.agent.world.on_tick = [&bench, seen, before](std::uint32_t ms) {
    if (before) before(ms);
    seen->states.push_back(bench.agent.duty().progress().now());
  };
}

TEST(WhereAWakeIs, AStatusOnANeighbourIsWrittenDownAsTheJobItIsWhileItRuns) {
  MeshBench bench;
  bench.agent.modemWorks();
  Seen seen;
  watch(bench, &seen);
  bench.agent.server().queue(bench::neighbourStatus(50).json());
  interfaces::BackendAnswer out;
  const interfaces::Exchanged how =
      bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);
  ASSERT_EQ(how, interfaces::Exchanged::kAsked);
  ASSERT_FALSE(bench.agent.lastResult(50).empty());

  // The job, by the type the server claimed it as, with the requests it had made of the radio by
  // then - a `status` over the air is a login and a request, so more than none.
  EXPECT_TRUE(seen.anyJobNamed("status", 1)) << "seen " << seen.states.size() << " states";
  EXPECT_GT(seen.mostRequests(), 0);
  // And that the link is what was being waited on, which no other layer knows.
  EXPECT_TRUE(seen.anyOnTheLink());
  // The calls the wake made, each by the name `agent calls` prints for it.
  EXPECT_TRUE(seen.anyCallNamed("heartbeat"));
  EXPECT_TRUE(seen.anyCallNamed("claim"));
  // `a result` and not `result`: the name is `agent::callKindName()`'s, so that the record and
  // `agent calls` cannot drift into two vocabularies for one thing.
  EXPECT_TRUE(seen.anyCallNamed("a result"));

  // A call made **inside** a claimed job leaves the job reading as the job: the result of job 50
  // is posted while the runner still has it open.
  bool a_call_inside_a_job = false;
  for (const core::cycle::WakeWhere& w : seen.states) {
    if (w.doing == core::cycle::Doing::kCall && w.in_job) a_call_inside_a_job = true;
  }
  EXPECT_TRUE(a_call_inside_a_job);
}

TEST(WhereAWakeIs, WhenTheExchangeIsOverNoJobIsBeingRun) {
  MeshBench bench;
  bench.agent.modemWorks();
  Seen seen;
  watch(bench, &seen);
  bench.agent.server().queue(bench::neighbourStatus(51).json());
  interfaces::BackendAnswer out;
  ASSERT_EQ(bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out),
            interfaces::Exchanged::kAsked);
  ASSERT_FALSE(bench.agent.lastResult(51).empty());

  // The job was in the record while it ran...
  EXPECT_TRUE(seen.anyJobNamed("status", 0));
  // ... and the edge of the exchange is where the last one ends, so a wake ended at step 9 or 10
  // does not read as a wake still inside a job the runner finished.
  const core::cycle::WakeWhere& after = bench.agent.duty().progress().now();
  EXPECT_FALSE(after.in_job);
  EXPECT_NE(after.doing, core::cycle::Doing::kJob);
  EXPECT_FALSE(after.on_modem);
  EXPECT_FALSE(after.on_link);
}

}  // namespace
