// The battery says how many claims a wake makes (task p18-02).
//
// A wake used to make **one** claim and run the two jobs it was handed, so a NoiseScope sample of
// six steps and a report took five wakes - which is what 21 November 2026 at Bellevue cost. The
// copied library can now make more (`mm::WakeIntent::maxClaimsThisWake`, upstream's task
// p1.20-69) and Thomas's rule for this board is that it takes jobs until none are left or the
// battery says sleep. These cases are that rule on the wake bench, with the scripted
// meshmaster.dk answering a result with `jobsWaiting` as the server does (its task p4.47-253):
//
//   the number    the band of the battery, `plenty` eight down to `critical` none, and a worse
//                 band never promising more
//   the room      the bound on the wake covers **every** claim, and a wake of several is never
//                 ended by the reset the bound exists for
//   the reason    why the last claim was the last, in the seven words of `interfaces::Claims`,
//                 in the report, at the console and in the journal's own line
//   `holding`     the work this firmware keeps outside the library's buffer, named in the claim,
//                 so that a server which hands back every running lease without a result
//                 (meshmaster.dk's task p4.47-254) does not hand this unit its own work back
//                 mid-flight - and no job is run twice and no flash begun twice
//
// Where the rest of it is: `host_test/agent/test_modem_uplink.cpp` is the two gates themselves -
// the room before a claim and the latch that ends the asking - at the call level;
// `host_test/core/cycle/test_wake_room.cpp` is the arithmetic of the room, case by case;
// `host_test/agent/test_a_claim_that_was_lost.cpp` is the library's own one-more-claim, which is
// not this unit's number; and `host_test/agent-backend/test_a_wake_claims_until_the_battery_says_
// sleep.cpp` is a wake of several claims with a repeater on the air, where a job costs
// transactions.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "agent/session_job.h"
#include "core/cycle/wake_room.h"
#include "fixtures/manifest_files.h"

using bench::AgentBench;
using bench::Candidate;
using bench::SessionJob;
using core::cycle::Ending;
using core::cycle::Pass;
using interfaces::Claims;
namespace power = core::power;

namespace {

bool carries(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// `count` `twinoak.report` jobs of this unit's own, from `first`, as one batch of the server's
// queue: a claim takes two at a time out of it, so five is three claims. A report is answered by
// the agent's own extension over the console, so nothing of these cases needs a radio on the air
// and the wake's length is the uplink's alone.
std::vector<std::string> reports(long long first, int count,
                                 const char* sections = "[\"wakes\"]") {
  std::vector<std::string> jobs;
  for (int i = 0; i < count; ++i) {
    jobs.push_back(bench::ownJob(first + i, agent::kReportType,
                                 std::string("{\"sections\":") + sections + "}")
                       .json());
  }
  return jobs;
}

// A unit ready for an update: the golden image on the chip and in the store, running.
void readyUnit(AgentBench& bench, const Candidate& golden) {
  REQUIRE(bench.world.images.put(core::image::Which::kGolden, bench::Slot{0}, golden) ==
          bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
}

// The newest wake entry of the journal, which is where a person reads this a month later.
core::cycle::WakePayload lastWakeEntry(AgentBench& bench) {
  const std::vector<core::cycle::WakePayload> entries = bench.wakeEntries();
  REQUIRE_FALSE(entries.empty());
  return entries.front();
}

// What the two readers that read a wake **already run** say: the `jobs` section a
// `twinoak.report` carries to meshmaster.dk, and the journal's own line. (`agent wake` is the
// third, and it is a wake of its own: the case for it drives the wake with it.)
void theWordIs(AgentBench& bench, const std::string& word, int claims, int jobs_run) {
  const std::string jobs = bench.v().runLine("agent jobs");
  INFO(jobs);
  CHECK(carries(jobs, "claims    " + std::to_string(claims) + " last wake, " +
                          std::to_string(jobs_run) + " job(s) run; the last was the last: " + word));
  const std::string journal = bench.v().runLine("journal");
  INFO(journal);
  CHECK(carries(journal, std::to_string(claims) + " claim(s), " + std::to_string(jobs_run) +
                             " job(s) run; the last was the last: " + word));
}

}  // namespace

// -- the number, band by band --------------------------------------------------------------------

TEST_CASE("plenty makes up to 8 claims, normal 4, saving and below 1, and a worse band never promises more") {
  CHECK(agent::intent::claimsFor(power::Band::kPlenty) == 8);
  CHECK(agent::intent::claimsFor(power::Band::kNormal) == 4);
  CHECK(agent::intent::claimsFor(power::Band::kSaving) == 1);
  // `critical` does not bring the modem up at all, so the number is moot - and 0 is the library's
  // own "an agent that proposes nothing", which is one claim if ever a wake got there.
  CHECK(agent::intent::claimsFor(power::Band::kCritical) == 0);
  // No reading at all is the medium one, as its interval and its transaction cap are.
  CHECK(agent::intent::claimsFor(power::Band::kUnknown) ==
        agent::intent::claimsFor(power::Band::kNormal));

  // The order, which `walter_agent/identity.h`'s static_assert holds at compile time: a band that
  // is worse never promises more claims, as it never promises more transactions.
  const power::Band worse[] = {power::Band::kPlenty, power::Band::kNormal, power::Band::kSaving,
                               power::Band::kCritical};
  for (std::size_t i = 1; i < sizeof worse / sizeof worse[0]; ++i) {
    CAPTURE(power::name(worse[i]));
    CHECK(agent::intent::claimsFor(worse[i]) < agent::intent::claimsFor(worse[i - 1]));
    CHECK(agent::intent::forBand(worse[i]) <= agent::intent::forBand(worse[i - 1]));
  }

  // And `intentFor` carries it, beside the interval and the transaction cap. It is **not** sent to
  // the server: it is this end's own bound.
  const mm::WakeIntent intent = agent::intentFor(power::Band::kPlenty, 900);
  CHECK(intent.maxClaimsThisWake == 8);
  CHECK(intent.maxTransactionsThisWake == agent::intent::kPlenty);
  CHECK(intent.nextCheckInS == 900u);
  CHECK(agent::intentFor(power::Band::kSaving, 21600).maxClaimsThisWake == 1);
}

TEST_CASE("the band's claims are in `agent id`, beside its transactions") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  (void)bench.oneWake();
  CHECK(carries(bench.v().runLine("agent id"), "at most " + std::to_string(agent::intent::kPlenty) +
                                                   " transaction(s) and 8 claim(s) this wake (band plenty)"));
}

// -- the claims a wake makes ---------------------------------------------------------------------

TEST_CASE("five jobs waiting in plenty are three claims and five jobs in one wake") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(50100, 5));

  const Pass& pass = bench.oneWake();

  // Two, two and one: the server hands over `mm::kClaimMax` at a time, and the answer to each
  // claim's last result says more waits until the queue is empty.
  CHECK(bench.server().claims == 3);
  CHECK(bench.server().claim_maxes == std::vector<int>{2, 2, 2});
  const mm::WakeReport& report = bench.half().backend.lastReport();
  CHECK(report.claimsMade == 3);
  CHECK(report.jobsClaimed == 5);
  CHECK(report.jobsRun == 5);
  CHECK(report.jobsNotRun == 0);
  CHECK(report.resultsKept == 0);
  for (long long id = 50100; id < 50105; ++id) {
    CAPTURE(id);
    REQUIRE(bench.server().results[id].size() == 1);
    CHECK(carries(bench.lastResult(id), "\"outcome\":\"ok\""));
  }
  CHECK(bench.server().queuedJobs() == 0);

  // **The first claim states no `transactionsLeft` and every one after it does** (API §3): the
  // wake has spent something by then, and the server hands out no job this wake cannot pay for.
  REQUIRE(bench.server().claim_bodies.size() == 3);
  CHECK_FALSE(carries(bench.server().claim_bodies[0], "transactionsLeft"));
  CHECK(carries(bench.server().claim_bodies[1], "transactionsLeft"));
  CHECK(carries(bench.server().claim_bodies[2], "transactionsLeft"));
  // And every one of them says this unit holds nothing: the results all went.
  for (const std::string& body : bench.server().claim_bodies) {
    CHECK(carries(body, "\"holding\":[]"));
  }
  CHECK(bench.server().claims_handed_back == 0);

  // **And the wake was never ended by the reset the bound exists for.** Three claims, five jobs,
  // and it went to sleep inside the bound with no call refused for want of room.
  CHECK(pass.ending == Ending::kSlept);
  CHECK(pass.spent_ms < pass.bound_ms);
  CHECK_FALSE(bench.world.restart_asked);
  CHECK(bench.half().server.wakesRoom().no_room == 0);
  CHECK(bench.half().server.uplinkCounters().budget == 0);
  MESSAGE("three claims and five jobs in " << pass.spent_ms << " ms of the " << pass.bound_ms
                                           << " ms bound, "
                                           << bench.half().server.uplinkCounters().requests
                                           << " requests, " << report.bytesPosted << " bytes posted");

  // The reason the last claim was the last, in all three places a person reads it.
  CHECK(bench.half().backend.claimsEnded() == Claims::kNothingWaiting);
  const core::cycle::WakePayload entry = lastWakeEntry(bench);
  CHECK(entry.claims == 3);
  CHECK(entry.jobs_run == 5);
  CHECK(entry.claims_end == Claims::kNothingWaiting);
  theWordIs(bench, "nothing waiting", 3, 5);
}

TEST_CASE("in saving a wake makes one claim whatever is queued: the band's number") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(50200, 5));

  // The band in the wake's knowledge, as step 8 hands it over: `saving` reaches the modem every
  // fourth wake, and what this case is about is the number and not the duty cycle's own rule.
  interfaces::BackendAnswer out;
  CHECK(bench.exchange(bench::knowledgeFor(power::Band::kSaving), 600, &out) ==
        interfaces::Exchanged::kAsked);

  CHECK(bench.server().claims == 1);
  CHECK(bench.half().backend.lastReport().claimsMade == 1);
  CHECK(bench.half().backend.lastReport().jobsRun == 2);
  CHECK(bench.server().queuedJobs() == 3);  // left for the next wake, as every wake left them
  CHECK(bench.half().backend.claimsEnded() == Claims::kTheBandsNumber);
}

TEST_CASE("`agent wake` at the console says how many claims it made and why the last was the last") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(50800, 3));

  // The console's own wake: the one reader that prints a wake as it finishes it. It runs no step
  // 6, so the modem is brought up first, as a person on the port does it.
  bench.v().runLine("lte up");
  const std::string said = bench.v().runLine("agent wake");
  INFO(said);
  CHECK(bench.server().claims == 2);
  CHECK(carries(said, "2 claim(s), 3 job(s) run; the last claim was the last: nothing waiting"));
  // And the `jobs` section reads the same wake afterwards, which is what a `twinoak.report`
  // carries to the server.
  CHECK(carries(bench.v().runLine("agent jobs"),
                "claims    2 last wake, 3 job(s) run; the last was the last: nothing waiting"));
}

TEST_CASE("the band's number ends a wake of eight claims, and the sixteen jobs all ran") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // Nine claims' worth queued in `plenty`, whose number is eight.
  bench.server().queue(reports(50900, 18));

  const Pass& pass = bench.oneWake();

  CHECK(bench.server().claims == agent::intent::kClaimsPlenty);
  CHECK(bench.half().backend.lastReport().claimsMade == agent::intent::kClaimsPlenty);
  CHECK(bench.half().backend.lastReport().jobsRun == 2 * agent::intent::kClaimsPlenty);
  CHECK(bench.server().queuedJobs() == 2);  // the ninth claim's worth, for the next wake
  CHECK(bench.half().backend.claimsEnded() == Claims::kTheBandsNumber);
  CHECK(lastWakeEntry(bench).claims_end == Claims::kTheBandsNumber);
  CHECK(pass.ending == Ending::kSlept);
  CHECK_FALSE(bench.world.restart_asked);
  MESSAGE("eight claims and sixteen jobs in " << pass.spent_ms << " ms of the " << pass.bound_ms
                                              << " ms bound, "
                                              << bench.half().backend.lastReport().bytesPosted
                                              << " bytes of the " << agent::kWakePostBytes
                                              << " a wake may post");
}

// -- the reasons the last claim was the last -----------------------------------------------------

TEST_CASE("the reason the last claim was the last: nothing waiting") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(50300, 1));

  (void)bench.oneWake();

  CHECK(bench.server().claims == 1);
  CHECK(bench.half().backend.claimsEnded() == Claims::kNothingWaiting);
  CHECK(lastWakeEntry(bench).claims_end == Claims::kNothingWaiting);
}

TEST_CASE("the reason the last claim was the last: a claim failed") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(50600, 2));
  bench.server().queue(reports(50610, 2));
  // The first claim is answered and the second is refused with a 503: the server spoke, so it is
  // not asked again and the wake's jobs end there. The script's hook is called before it answers
  // a request, so the refusal is installed in time for the very claim that asks for it.
  bench.server().on_call = [&bench](const std::string& path) {
    if (path == "/agent/v1/jobs/claim" && bench.server().claims == 1) {
      bench.server().failRoute("/jobs/claim", 503, "30");
    }
  };

  (void)bench.oneWake();

  // The refusal never reaches the claim handler, so the script's own count is the one claim it
  // answered; the library made two.
  CHECK(bench.server().claims == 1);
  CHECK(bench.server().requestsTo("/agent/v1/jobs/claim") == 2);
  CHECK(bench.half().backend.lastReport().claimsMade == 1);
  CHECK(bench.half().backend.claimsEnded() == Claims::kAClaimFailed);
  CHECK(lastWakeEntry(bench).claims_end == Claims::kAClaimFailed);
  CHECK(bench.server().queuedJobs() == 2);  // nothing was leased, so nothing was lost
}

TEST_CASE("the reason the last claim was the last: a result kept for want of bytes") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // Reports of every section this console has: a result of a few kB each, so the 32 kB a wake may
  // post (`agent::kWakePostBytes`) runs out before the band's eight claims do. A result that does
  // not fit is **kept** for the next wake with the timestamps it was made with, and a wake that
  // could not post what it has does not take on more.
  bench.server().queue(reports(
      51000, 18,
      "[\"wakes\",\"agent\",\"calls\",\"uplink\",\"observations\",\"jobs\",\"journal\",\"power\","
      "\"restarts\",\"settings\",\"log\"]"));

  const Pass& pass = bench.oneWake();

  const mm::WakeReport& report = bench.half().backend.lastReport();
  CHECK(report.resultsKept >= 1);
  CHECK(report.claimsMade < agent::intent::kClaimsPlenty);
  CHECK(report.bytesPosted <= agent::kWakePostBytes);
  CHECK(bench.half().backend.claimsEnded() == Claims::kAResultKept);
  CHECK(lastWakeEntry(bench).claims_end == Claims::kAResultKept);
  CHECK(pass.ending == Ending::kSlept);
  MESSAGE("the wake posted " << report.bytesPosted << " bytes of its " << agent::kWakePostBytes
                             << " in " << report.claimsMade << " claim(s) and " << report.jobsRun
                             << " job(s), and kept " << report.resultsKept << " result(s)");
}

TEST_CASE("the reason the last claim was the last: no room, where the wake could not cover one") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(50700, 2));
  // A wake a session has held open to within a minute of the ceiling: there is room for a call -
  // the heartbeat goes, and this is not a wake that failed - and none for a claim and the calls
  // after its jobs. So no claim is made, nothing is leased, and the work waits for the next wake.
  core::cycle::WakeGuard& guard = bench.duty().guard();
  guard.arm();
  guard.allow(core::cycle::kRaisedBoundMax);
  bench.world.clock.advance(core::cycle::kRaisedBoundMax - 60000u);

  interfaces::BackendAnswer out;
  (void)bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 600, &out);

  CHECK(bench.server().heartbeats.size() == 1);
  CHECK(bench.server().claims == 0);
  CHECK(bench.server().queuedJobs() == 2);
  CHECK(bench.half().backend.lastReport().claimsMade == 0);
  CHECK(bench.half().backend.claimsEnded() == Claims::kNoRoom);
  // And the claim step's own outcome is what a wake that made no claim at all says beside it.
  CHECK(bench.half().backend.lastReport().claim == mm::StepOutcome::Skipped);
  CHECK(carries(bench.v().runLine("agent jobs"), "claims    none last wake (the claim step: skipped)"));
}

// -- after a job that takes the radio away -------------------------------------------------------

TEST_CASE("two reports and a session queued are one wake: two claims, the session alone in the second") {
  SessionJob unit;
  bench::Answer three;
  three.lines = 3;
  three.line_bytes = 200;
  unit.scripted.answers["stat"] = three;
  unit.server().queue(reports(52000, 2));
  unit.server().queue({bench::ownJob(52010, agent::kSessionType,
                                     bench::sessionPayload({{"stat", 30}}))
                           .json()});
  unit.server().queue(reports(52020, 1));

  const Pass& pass = unit.oneWake();
  INFO(unit.v().out.text);

  // Three claims: the two reports, then the session alone - a batch of the server's queue is one
  // claim's worth and the session is its own - then the report queued behind it, because the
  // session put the radio back and the supervisor still calls it healthy.
  CHECK(unit.server().claims == 3);
  CHECK(unit.half().backend.lastReport().claimsMade == 3);
  CHECK(unit.half().backend.lastReport().jobsRun == 4);
  for (long long id : {52000LL, 52001LL, 52010LL, 52020LL}) {
    CAPTURE(id);
    REQUIRE(unit.server().results[id].size() == 1);
  }
  CHECK(bench::member(unit.lastData(52010), "outcome") == "restored");
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK_FALSE(unit.sessionIsOpen());
  CHECK(unit.half().backend.claimsEnded() == Claims::kNothingWaiting);
  CHECK(pass.ending == Ending::kSlept);
  CHECK_FALSE(unit.world.restart_asked);
  MESSAGE("a session and three reports in " << pass.spent_ms << " ms of the " << pass.bound_ms
                                            << " ms bound");
}

TEST_CASE("the reason the last claim was the last: the radio is away after a power cycle it did not come back from") {
  AgentBench bench;
  const Candidate golden("update-golden");
  readyUnit(bench, golden);
  bench.server().queue({bench::ownJob(52100, agent::kPowerCycleType, bench::powerCyclePayload(5)).json()});
  bench.server().queue(reports(52110, 1));

  // The rail goes down and the radio never answers again: the job says `radioBack` false, and a
  // wake whose radio is away takes on no more work - whatever the server says waits.
  bench.boot();
  bench.v().begin();
  bench.v().pins.on_kill_cycle = [&bench] { bench.world.rak.deaf = true; };
  (void)bench.v().run();

  CHECK(bench.v().pins.kill_cycles == 1);
  CHECK(bench::member(bench.lastData(52100), "radioBack") == "false");
  CHECK(bench.server().claims == 1);
  CHECK(bench.half().server.claimsStopped() == Claims::kTheRadioIsAway);
  CHECK(bench.half().backend.claimsEnded() == Claims::kTheRadioIsAway);
  CHECK(lastWakeEntry(bench).claims_end == Claims::kTheRadioIsAway);
  CHECK(bench.server().queuedJobs() == 1);  // the report waits for a wake with a radio
  theWordIs(bench, "the radio is away", 1, 1);
}

// -- an offer and its apply in one wake ----------------------------------------------------------

TEST_CASE("an offer answered jobsWaiting is followed by its apply in the same wake") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, candidate);
  bench.server().queue({bench::ownJob(53000, agent::kOfferType, bench::offerPayload(candidate)).json()});
  bench.server().queue({bench::ownJob(53001, agent::kApplyType,
                                      bench::applyPayload(bench::sha256Hex(candidate.file)))
                            .json()});

  const Pass& pass = bench.oneWake();
  INFO(bench.v().out.text);

  // Two claims: the offer stored the candidate, its result's answer said more waits, and the
  // apply came in the next claim and ran under its own bound from where the wake stood.
  CHECK(bench.server().claims == 2);
  CHECK(bench.half().backend.lastReport().claimsMade == 2);
  CHECK(bench::member(bench.lastData(53000), "stored") == "true");
  CHECK(bench::member(bench.lastData(53001), "outcome") == "committed");
  CHECK(std::string(bench.world.images.read(core::image::Which::kGolden).version) == candidate.version);
  CHECK(bench.world.chip->chipHolds(candidate));
  CHECK(pass.ending == Ending::kSlept);
  CHECK_FALSE(bench.world.restart_asked);
  CHECK(pass.bound_ms >= core::update::kWholeRunMs);
  MESSAGE("a radio image offered and applied in one wake: " << pass.spent_ms << " ms of the "
                                                            << pass.bound_ms << " ms bound ("
                                                            << pass.awake_s << " s awake)");
}

TEST_CASE("the Walter's own image offered and applied in one wake: the switch ends the claiming") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate own("self-a");
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, own, bench::kBase, nullptr, nullptr, nullptr, true);
  bench.server().queue({bench::ownJob(53100, agent::kOfferType,
                                      bench::offerPayload(own, agent::kProductWalter))
                            .json()});
  bench.server().queue({bench::ownJob(53101, agent::kApplyType,
                                      bench::applyPayload(bench::sha256Hex(own.file),
                                                          agent::kProductWalter))
                            .json()});
  bench.server().queue(reports(53110, 1));

  const Pass& pass = bench.oneWake();
  INFO(bench.v().out.text);

  // The apply of this firmware's own image switches the boot slot and restarts the board, so
  // there is no wake left to claim in: the result is owed from the next boot, the job is held
  // outside the library's buffer, and nothing more is claimed.
  CHECK(bench.server().claims == 2);
  CHECK(bench.lastResult(53101).empty());
  CHECK(bench.half().firmware.resultOwed());
  CHECK(bench.half().server.claimsStopped() == Claims::kNoRoom);
  CHECK(bench.server().queuedJobs() == 1);
  MESSAGE("the Walter's own image offered and applied in one wake: " << pass.spent_ms
                                                                     << " ms of the "
                                                                     << pass.bound_ms << " ms bound");
}

// -- what this firmware holds is named in the claim ----------------------------------------------

TEST_CASE("a job the result gate left to its lease is named in no further claim, because the wake makes none") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate big("store-resume");  // 150 000 bytes: three of the download's 64 kB ranges
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, big);
  bench.server().queue({bench::ownJob(54000, agent::kOfferType, bench::offerPayload(big)).json()});
  bench.server().queue(reports(54010, 2));

  // One range this wake: the download stops at a resumable boundary with no result, the lease is
  // left to expire, and the next wake continues it (unit-protocol.md §5). Set after the boot,
  // because a boot builds the backend again.
  bench.boot();
  bench.v().begin();
  bench.half().backend.setDownloadBytesPerWake(core::net::kRangeBytes);
  (void)bench.v().run();

  // **The wake claims no more.** The next claim of this wake would be handed that very job back -
  // the server hands back every lease it has running without a result that the claim did not
  // name - and a wake that could not pay for what it had does not take on more.
  CHECK(bench.server().claims == 1);
  CHECK(bench.half().firmware.counters().offers_left_leased == 1);
  CHECK(bench.half().server.uplinkCounters().withheld == 1);
  CHECK(bench.half().backend.claimsEnded() == Claims::kNoRoom);
  CHECK(bench.server().claims_handed_back == 0);
  CHECK(bench.server().queuedJobs() == 2);
  // And the download kept what it had: the next wake resumes from the range boundary.
  const bench::Record held = bench.world.images.read(core::image::Which::kCandidate);
  CHECK(held.transfer);
  CHECK(held.transferred == core::net::kRangeBytes);
}

TEST_CASE("a session the bound cut is named in the claim inside its lease, and no second session begins") {
  SessionJob unit;
  // A session left open in the journal at its first step, as the bound on a wake leaves one, with
  // the result owed by job 55000 - and the server holding a lease on it with no result, which is
  // what it has when the owed result's post went unanswered. A wake a person asks for five
  // minutes later is inside that lease.
  unit.anOpenSessionOwedBy(55000, core::update::Stage::kStep, 1);
  unit.theRadioIsAScanner();
  const std::string job =
      bench::ownJob(55000, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json();
  unit.server().leasedWithNoResult(job);

  // Every flash of this wake, counted: step 3's own switch back is one, and a session begun a
  // second time would be two more.
  bench::SlowFlash flashes(unit.world.clock, 0);
  unit.boot();
  unit.v().update.setProgress(&flashes);
  unit.v().begin();
  (void)unit.v().run();

  REQUIRE_FALSE(unit.server().claim_bodies.empty());
  CHECK(carries(unit.server().claim_bodies.front(), "\"holding\":[55000]"));
  // The job was not handed back, so nothing of it was begun a second time: the result the journal
  // owed is what went to the server, and the only flash of the wake is step 3's switch back.
  CHECK(unit.server().claims_handed_back == 0);
  CHECK(unit.server().leasedAndLost() == 1);  // the server still holds it; its lease will lapse
  CHECK(unit.half().firmware.counters().sessions == 0);
  CHECK(flashes.flashes() == 1);
  CHECK(unit.half().firmware.counters().owed_posted == 1);
  REQUIRE(unit.server().results[55000].size() == 1);
  CHECK(unit.world.chip->chipHolds(unit.golden));
}

TEST_CASE("an apply whose result waits for the next boot is named in the claim inside its lease, and nothing is applied twice") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate own("self-a");
  readyUnit(bench, golden);
  bench::serveRelease(bench.world.http.server, own, bench::kBase, nullptr, nullptr, nullptr, true);
  // The wake that switched: the offer stored the image into the other slot and the apply switched
  // the boot slot, so the result is owed from the next boot and nothing was posted.
  bench.server().queue({bench::ownJob(55100, agent::kOfferType,
                                      bench::offerPayload(own, agent::kProductWalter))
                            .json()});
  bench.server().queue({bench::ownJob(55101, agent::kApplyType,
                                      bench::applyPayload(bench::sha256Hex(own.file),
                                                          agent::kProductWalter))
                            .json()});
  (void)bench.oneWake();
  REQUIRE(bench.half().firmware.resultOwed());
  REQUIRE(bench.lastResult(55101).empty());

  // The next boot is the new image on trial, and the server still holds that lease with no
  // result. The claim of this wake names it, so it is not handed back and nothing is applied
  // twice.
  const std::string job = bench::ownJob(55101, agent::kApplyType,
                                        bench::applyPayload(bench::sha256Hex(own.file),
                                                            agent::kProductWalter))
                              .json();
  bench.server().leasedWithNoResult(job);
  bench.world.own.reboot();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  const std::uint32_t switches_before = bench.half().firmware.counters().switches;
  (void)bench.v().run();

  REQUIRE_FALSE(bench.server().claim_bodies.empty());
  CHECK(carries(bench.server().claim_bodies.back(), "\"holding\":[55101]"));
  CHECK(bench.server().claims_handed_back == 0);
  CHECK(bench.half().firmware.counters().switches == switches_before);
  CHECK(bench.v().restart.asked == 0);
  // The result the trial owed went, once, and from the journal rather than from a job.
  REQUIRE(bench.server().results[55101].size() == 1);
  CHECK(bench.half().firmware.counters().owed_posted == 1);
}

TEST_CASE("a claim whose answer was lost is still handed everything back, because nothing of it is named") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(56000, 2));
  bench.server().loseTheAnswerToClaims(1);

  (void)bench.oneWake();

  // The leases the lost claim took out came back to the claim after it, which names nothing: this
  // firmware holds neither job, so `holding` is `[]` and the server's rule hands both over.
  CHECK(bench.server().claims == 2);
  CHECK(bench.server().claims_handed_back == 2);
  CHECK(bench.server().leasedAndLost() == 0);
  for (const std::string& body : bench.server().claim_bodies) {
    CHECK(carries(body, "\"holding\":[]"));
  }
  REQUIRE(bench.server().results[56000].size() == 1);
  REQUIRE(bench.server().results[56001].size() == 1);
}

// -- between two claims nothing of the first is left ---------------------------------------------

TEST_CASE("between two claims of a wake nothing of the first is left: the payloads, the reply and every credential") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(reports(57000, 4));

  (void)bench.oneWake();

  CHECK(bench.half().backend.lastReport().claimsMade == 2);
  // The kept payloads are the last claim's and nothing of the first is among them: `keepPayloads`
  // zeroes all of `mm::kClaimMax` before it reads a claim's bytes, which is what makes the second
  // claim's payloads the only ones this object holds.
  CHECK(bench.half().server.payloadsKept() == 2);
  CHECK(bench.half().server.payloadOf(57000) == nullptr);
  CHECK(bench.half().server.payloadOf(57001) == nullptr);
  REQUIRE(bench.half().server.payloadOf(57002) != nullptr);
  REQUIRE(bench.half().server.payloadOf(57003) != nullptr);
  // The reply buffer is wiped after every call, whether the parse took or not, so no id of the
  // first claim is anywhere in the uplink.
  const std::string image(reinterpret_cast<const char*>(&bench.half().server),
                          sizeof bench.half().server);
  CHECK(image.find("57000") == std::string::npos);
  CHECK(image.find("57001") == std::string::npos);
  // And every credential of a claim is wiped by the library's `endClaim()` before the next claim
  // is read: rail R15 holds the password of a whole wake, and this is the claim-to-claim half.
  CHECK(image.find("guest-pw") == std::string::npos);
}
