// A claim whose answer did not come back whole, and the one more this wake makes (the copied
// library's `v0.7.6`, upstream's task p1.20-70, brought here by the sync pile's task 02).
//
// The server leases the jobs of a claim **in the moment it answers**, so a claim whose answer
// never arrived has left it holding leases this unit knows nothing of: on the evening of
// 21 November 2026 at Bellevue that cost a download its one attempt. Since the bump every claim
// says what this end holds (`holding`) and a claim whose answer was lost is made **once** more in
// the same wake - and the server hands the leases back to a claim that says it does not hold them
// (meshmaster.dk's task p4.47-254).
//
// Nothing of this firmware asked for it: `maxClaimsThisWake` is unset here, so this unit still
// makes one claim a wake and the second claim below is the one exception the library makes
// without being asked. What this file holds is what a Walter does with it, which is the question
// the task of the bump asks: the second claim is made inside **this wake's** bound and the
// uplink's own budgets, the jobs come back, their results go, and the wake ends by its sleep and
// never by the guard's reset.
//
// `host_test/agent/test_what_is_kept.cpp` holds the other half a person sees - the pair of claims
// in `agent calls` when neither answer came back.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "walter_agent/call_log.h"

using bench::AgentBench;
using core::cycle::Ending;

namespace {

bool has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// Two jobs of the unit's own, which is one claim's worth (`mm::kClaimMax`): a `twinoak.report` is
// answered by the agent's own extension, so nothing of this test needs a radio or a repeater and
// the wake's length is the uplink's alone.
std::vector<std::string> twoOwnJobs(long long first) {
  return std::vector<std::string>{
      bench::ownJob(first, agent::kReportType, "{\"sections\":[\"wakes\"]}").json(),
      bench::ownJob(first + 1, agent::kReportType, "{\"sections\":[\"settings\"]}").json()};
}

}  // namespace

TEST_CASE("a claim whose answer was lost is made once more in the same wake, and both jobs come back") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(twoOwnJobs(39302));
  bench.server().loseTheAnswerToClaims(1);

  const core::cycle::Pass& pass = bench.oneWake();

  // One claim and one more: never a loop, and never a third.
  CHECK(bench.server().claims == 2);
  REQUIRE(bench.server().claim_bodies.size() == 2);
  // Both claims say the same thing, because nothing was spent and nothing changed between them -
  // the same `max`, and `holding` empty, since this unit keeps no result for either job yet.
  CHECK(bench.server().claim_bodies[0] == bench.server().claim_bodies[1]);
  CHECK(has(bench.server().claim_bodies[0], "\"holding\":[]"));
  CHECK_FALSE(has(bench.server().claim_bodies[0], "transactionsLeft"));

  // The two leases the first claim took out were handed back to the second, which is the server's
  // half of the bump, and nothing is left leased and silent on the server.
  CHECK(bench.server().claims_handed_back == 2);
  CHECK(bench.server().leasedAndLost() == 0);
  CHECK(bench.server().queuedJobs() == 0);

  // Both jobs ran once and both results were posted - the jobs of the claim the unit never heard
  // the answer to.
  REQUIRE(bench.server().results[39302].size() == 1);
  REQUIRE(bench.server().results[39303].size() == 1);
  CHECK(bench.lastResult(39302).find("\"outcome\":\"ok\"") != std::string::npos);
  CHECK(bench.lastResult(39303).find("\"outcome\":\"ok\"") != std::string::npos);

  // **Inside this wake's bound and the uplink's own budgets.** The wake ended at step 10 by going
  // to sleep; no call was held back for want of room in the bound or of the wake's uplink
  // deadline, and the guard never had to end anything.
  CHECK(pass.ending == Ending::kSlept);
  CHECK(pass.spent_ms < pass.bound_ms);
  CHECK(bench.half().server.uplinkCounters().budget == 0);
  CHECK(bench.half().server.uplinkCounters().held == 0);
  CHECK(bench.half().server.wakesRoom().no_room == 0);
  MESSAGE("the wake was " << pass.spent_ms << " ms of its " << pass.bound_ms
                          << " ms bound (" << pass.awake_s << " s awake), with "
                          << bench.server().claims << " claims and "
                          << bench.half().server.uplinkCounters().requests << " requests");

  // And the pair is one failure in the list a person reads, not two: the first claim is the one
  // that did not come back, and the wake went on to do its work.
  CHECK(bench.half().calls.count() == 1);
  CHECK(bench.half().calls.at(0).what == agent::CallKind::kClaim);
  CHECK(bench.half().calls.at(0).status == 0);
}

TEST_CASE("a claim the server refused is not asked again, and the wake still ends by its sleep") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(twoOwnJobs(39402));
  // A refusal is the server having spoken (a 503 with its Retry-After): asking again would only
  // be refused again, so the library asks once - which is what every wake did before the bump.
  bench.server().failRoute("/jobs/claim", 503, "30");

  const core::cycle::Pass& pass = bench.oneWake();

  // One request to the route and no second: the refusal never reaches the claim handler, which is
  // why the script's own `claims` stays at nought.
  CHECK(bench.server().requestsTo("/agent/v1/jobs/claim") == 1);
  CHECK(bench.server().claims == 0);
  CHECK(bench.server().queuedJobs() == 2);  // nothing was leased, so nothing was lost
  CHECK(bench.server().leasedAndLost() == 0);
  CHECK(pass.ending == Ending::kSlept);
  CHECK(bench.half().calls.count() == 1);
  CHECK(bench.half().calls.at(0).what == agent::CallKind::kClaim);
  CHECK(bench.half().calls.at(0).status == 503);
}

TEST_CASE("a job handed over again whose result this unit kept is not run twice: the kept result is posted") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // The first wake runs the job and cannot post its result: the server took the job's claim and
  // then refused the result, which is what the offline buffer is for (EXIT-NODES §5).
  bench.server().queue(bench::ownJob(39502, agent::kReportType, "{\"sections\":[\"wakes\"]}").json());
  bench.server().failResults(503, 1);
  (void)bench.oneWake();
  CHECK(bench.server().results[39502].empty());

  // The next wake: the server has heard nothing of that job, so it hands it over again. This end
  // holds a result for it, says so in the claim's `holding`, and the library posts the **kept**
  // result rather than running the job a second time.
  // The kept result's own post is refused once more at the results step, so that the claim of
  // this wake still has it to say: without that the result would be taken before the claim went
  // out and there would be nothing for the server to hand back.
  bench.server().answerAgain();
  bench.server().failResults(503, 1);
  bench.server().queue(bench::ownJob(39502, agent::kReportType, "{\"sections\":[\"wakes\"]}").json());
  (void)bench.oneWake();

  REQUIRE_FALSE(bench.server().claim_bodies.empty());
  const std::string& latest = bench.server().claim_bodies.back();
  CHECK(has(latest, "\"holding\":[39502]"));
  REQUIRE(bench.server().results[39502].size() == 1);
  CHECK(bench.lastResult(39502).find("\"outcome\":\"ok\"") != std::string::npos);
}
