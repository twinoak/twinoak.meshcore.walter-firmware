// What a wake did, named from the library's own chain (task p10-02).
//
// `interfaces::Reached` is the second half of the seam's answer - whether a backend was reached
// and, when none was, why nothing was sent - and this is the half of it that only the agent can
// fill. `mm::AgentLoop::wake` decides *whether* anything was sent and says so in one of five
// static sentences; what it does not give is a code, so `AgentBackend::reachedThisWake` walks the
// library's own chain over the same state the library read: halted, then not configured, then the
// uplink, then the hold.
//
// **Every branch of it is walked here through the real library**, so that an upstream sync which
// changed that order fails this file rather than quietly mislabelling a wake. The wake cycle's
// half - the journal's flag, the record's clause, `wake`'s letters - is
// host_test/core/cycle/test_what_a_wake_did.cpp.
#include <doctest.h>

#include <string>

#include "agent/agent_bench.h"

using bench::AgentBench;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;
using interfaces::Exchanged;
using interfaces::Reached;

namespace {

bool has(const char* text, const char* what) { return std::string(text).find(what) != std::string::npos; }

}  // namespace

TEST_CASE("an answered wake: kAnswered, and the jobs it ran cross the seam") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(bench::ownJob(41, agent::kStateType, "{}").json());

  const Pass& pass = bench.oneWake();
  REQUIRE(bench.server().heartbeats.size() == 1);
  CHECK(bench.half().backend.lastReport().jobsRun == 1);
  // The heartbeat came back, so the backend was answered - and the journal may say *reached*.
  CHECK(pass.reached == Reached::kAnswered);
  CHECK(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));
  CHECK(pass.jobs_ran == 1);
  CHECK((bench.cycle().answer().jobs_ran) == 1);
}

TEST_CASE("a wake with nothing waiting is still answered: *nothing to do* is not silence") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  const Pass& pass = bench.oneWake();
  // There were no jobs and nothing to flush, so the seam's own word may well be *nothing to do* -
  // and that is exactly the case the old two values could not tell from a wake that sent nothing.
  CHECK(pass.reached == Reached::kAnswered);
  CHECK(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));
  CHECK(pass.jobs_ran == 0);
}

TEST_CASE("a heartbeat the server did not answer: the exchange failed, and nothing is claimed") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(503);

  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kFailed);
  // The loop *ran* - it has no sentence for this - so the reason is not one of the four; and a
  // heartbeat that failed is never *answered*.
  CHECK(bench.half().backend.lastReport().why[0] == 0);
  CHECK(pass.reached == Reached::kUnsaid);
  CHECK_FALSE(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));
}

TEST_CASE("a wait the server asked for: kWait, with what is left of it beside") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(503, "3600");
  (void)bench.oneWake();
  CHECK(bench.half().backend.lastReport().sleepAtLeastS == 3600);

  // A wake inside the hold: the state carried the wait and the loop sends nothing. The wake
  // cycle cannot produce one of these by itself (step 10 lengthens the sleep to cover a hold),
  // so it is made the way p10-01's own tests make it - by hand, as a person's `agent wake` is.
  bench.boot();
  bench.v().begin();
  interfaces::BackendAnswer out;
  const Exchanged how = bench.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 60, &out);
  CHECK(how == Exchanged::kNothing);
  CHECK(has(bench.half().backend.lastReport().why, "wait"));
  CHECK(out.reached == Reached::kWait);
  CHECK(out.sleep_at_least_s > 0);
  CHECK_FALSE(interfaces::wasAnswered(how, out));
}

TEST_CASE("the uplink is down: kNoUplink, and nothing is sent over it") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  interfaces::BackendKnowledge known = bench::knowledgeFor(core::power::Band::kPlenty);
  known.uplink.up = false;
  interfaces::BackendAnswer out;
  const Exchanged how = bench.exchange(known, 60, &out);
  CHECK(how == Exchanged::kNothing);
  CHECK(bench.server().heartbeats.empty());
  CHECK(has(bench.half().backend.lastReport().why, "uplink"));
  CHECK(out.reached == Reached::kNoUplink);
}

TEST_CASE("a 401 halts the loop, and every wake after it says kHalted") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().failWith(401);
  (void)bench.oneWake();
  CHECK(bench.half().backend.lastReport().halted);
  bench.server().answerAgain();

  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kNothing);
  CHECK(pass.reached == Reached::kHalted);
  CHECK_FALSE(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));

  // **The order is the library's.** A halt and an uplink that is down are both true here, and the
  // library's chain tests the halt first - a 401 is not something a working modem would fix. If
  // an upstream sync reordered that chain this is the case that would fail.
  interfaces::BackendKnowledge known = bench::knowledgeFor(core::power::Band::kPlenty);
  known.uplink.up = false;
  interfaces::BackendAnswer out;
  CHECK(bench.exchange(known, 60, &out) == Exchanged::kNothing);
  CHECK(has(bench.half().backend.lastReport().why, "halted"));
  CHECK(out.reached == Reached::kHalted);
}

TEST_CASE("a unit with no server: kNotConfigured, every wake, and never *reached*") {
  // The one thing AgentBench's own constructor cannot be asked for, so the world is turned by
  // hand: the agent, and then the server's url taken off it.
  bench::WakeBench bench([](bench::WakeWorld& w) {
    w.withAgent();
    w.kv.put("srv_url", "");
  });
  bench.modemWorks();
  REQUIRE(bench.world.agent != nullptr);
  CHECK_FALSE(bench.v().agent->server.configured());

  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kNothing);
  CHECK(has(bench.v().agent->backend.lastReport().why, "not configured"));
  CHECK(pass.reached == Reached::kNotConfigured);
  CHECK_FALSE(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));
  CHECK(bench.world.agent->server.heartbeats.empty());
}

TEST_CASE("`agent wake` says which of the three it was, and whether the loop ran") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  const std::string answered = bench.v().runLine("agent wake");
  INFO(answered);
  CHECK(answered.find("the backend answered") != std::string::npos);
  CHECK(answered.find("the loop did not run") == std::string::npos);

  // And a halted loop: the library's sentence, the seam's word, and *the loop did not run*.
  bench.server().failWith(401);
  (void)bench.v().runLine("agent wake");
  const std::string halted = bench.v().runLine("agent wake");
  INFO(halted);
  CHECK(halted.find("nothing was sent: halted") != std::string::npos);
  CHECK(halted.find("halted on a 401") != std::string::npos);
  CHECK(halted.find("the loop did not run") != std::string::npos);
}
