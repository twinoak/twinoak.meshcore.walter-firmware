// A trial the wake before it cannot sink (task p14-05), on the wake bench with the scripted
// meshmaster.dk and a simulated nRF52840 behind the RAK.
//
// **The morning of 15 November 2026.** Thomas applied v0.1.6 to the unit (meshmaster.dk job
// 30474). It booted, went on trial, and five minutes later rolled itself back: *the backend was
// not reached inside the trial window*. It had never asked. The wake that took the apply ended
// in a failed call, which leaves the agent's library a hold; the hold is saved across the restart
// and put back whole at the next boot, because the time away is 0 for every boot that is not a
// timer wake; and the agent's wake sends nothing while a hold is owed. So the trial's one
// heartbeat was never posted, the window ran out, and a good image went on the bad list - taking
// with it the very diagnostics that were built to find out why this unit misbehaves. The
// rolled-back v0.1.4 owed the same hold and the same result, and it was the *timer* wake eleven
// minutes later that finally told the server what had happened.
//
// The four things this file holds:
//
//   a trial boot owes no wait        the wait is laid aside before the state is handed to the
//                                    library, so the trial's first wake posts a heartbeat
//                                    whatever the wake before it ended with;
//   and no other boot does           a boot that is not on trial and owes nobody a result still
//                                    owes the whole wait, exactly as before;
//   a heartbeat that fails is        the trial's own loop asks the seam for one more each time
//   tried again                      the library's wait has run out, until one is answered or
//                                    the window ends;
//   and the figures are kept         how many went out and what the last came to, so that
//                                    *not reached* can be told from *not asked*.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "fixtures/manifest_files.h"

using bench::AgentBench;
using bench::Candidate;
using core::cycle::Pass;
using core::image::Which;
namespace power = core::power;
namespace upd = core::update;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// Heartbeats **put on the wire**, answered or not. `MeshMasterScript::heartbeats` keeps the
// bodies the server took, and a scripted 503 is answered before the route is reached, so it is
// the call log that counts the tries.
int heartbeatsSent(AgentBench& bench) {
  int n = 0;
  for (const std::string& call : bench.server().calls) n += call == "/agent/v1/heartbeat";
  return n;
}

// A unit ready for an update (test_firmware_jobs.cpp's own helper): the golden image on the chip
// and in the store, running, and a modem that works.
void readyUnit(AgentBench& bench, const Candidate& golden) {
  REQUIRE(bench.world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
}

// A battery reading taken while the radio answered: a self-switch asks for one before anything
// else and the gauge is on the radio's I2C (test_radio_health_and_the_refusals.cpp's helper).
void primeBattery(AgentBench& bench) {
  (void)bench.v().link.hello();
  upd::BatteryReading reading;
  REQUIRE(bench.v().battery.read(&reading));
}

// One exchange of the agent, as the wake cycle's step 8 makes it. By hand and not `oneWake()`,
// because a wake ends in a sleep and the fake sleeper moves the clock past the hold the wake
// just set - `state()` afterwards would read 0 s owed.
interfaces::Exchanged oneExchange(AgentBench& bench) {
  interfaces::BackendAnswer out;
  return bench.exchange(bench::knowledgeFor(power::Band::kPlenty), 1800, &out);
}

// The new image in the other slot, offered by the server and taken: what `self switch` needs.
void offerSelfImage(AgentBench& bench, const Candidate& image) {
  bench::serveRelease(bench.world.http.server, image, bench::kBase, nullptr, nullptr, nullptr, true);
  bench.server().queue(
      bench::ownJob(21, agent::kOfferType, bench::offerPayload(image, agent::kProductWalter)).json());
  (void)bench.oneWake();
  REQUIRE(bench::member(bench::objectOf(bench.lastResult(21), "data"), "stored") == "true");
  upd::SelfPayload received;
  REQUIRE(bench.v().self.received(&received));
}

// The 15 November wake, in one line: the server names a Retry-After of 300 seconds, the heartbeat
// fails, and the hold that leaves is saved. Answers the state as the store now holds it, which is
// what the boot after a restart reads.
mm::WakeState aWaitOf300Seconds(AgentBench& bench) {
  bench.server().failWith(503, "300");
  REQUIRE(oneExchange(bench) == interfaces::Exchanged::kFailed);
  const mm::WakeState saved = bench.half().loop.state();
  REQUIRE(saved.holdForMs > 290u * 1000u);
  bench.server().answerAgain();
  return saved;
}

// `self switch`, and the restart it asks for: the trial image boots in the other slot.
void switchAndBoot(AgentBench& bench) {
  primeBattery(bench);
  upd::SwitchRequest request{};
  REQUIRE(bench.v().self.switchNow(request) == upd::SelfReason::kNone);
  REQUIRE(bench.v().restart.asked == 1);
  bench.world.own.reboot();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  REQUIRE(bench.v().self.onTrial());
}

}  // namespace

TEST_CASE("a trial boot owes no wait: the heartbeat goes and the image confirms") {
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  offerSelfImage(bench, Candidate("self-a"));
  const mm::WakeState saved = aWaitOf300Seconds(bench);
  switchAndBoot(bench);

  // The wait was in the store and this boot did not put it back. Nothing else of the state moved:
  // the run of failures and the backoff the same wake left are restored as they were saved.
  CHECK(bench.half().backend.stateSurvived());
  CHECK(bench.half().backend.timeAwayMs() == 0);  // a software reset: nothing measures a sleep
  CHECK(bench.half().backend.holdLaidAsideMs() == saved.holdForMs);
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() == saved.holdForMs);
  CHECK(std::string(bench.half().backend.holdLaidAsideWhy()) == "this image is on trial");
  CHECK(bench.half().loop.state().consecutiveFailures == saved.consecutiveFailures);
  CHECK(bench.half().loop.state().backoffMs == saved.backoffMs);
  CHECK(bench.half().loop.state().counters.httpFailures == saved.counters.httpFailures);
  CHECK(bench.half().loop.retrySeconds() == 0);

  const std::size_t before = bench.server().heartbeats.size();
  const Pass& pass = bench.v().run();
  CHECK(pass.on_trial);
  CHECK(bench.server().heartbeats.size() == before + 1);
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.v().self.status().state == upd::SelfState::kConfirmed);
  CHECK(bench.v().self.status().uplink == upd::Condition::kMet);
}

TEST_CASE("the same wait, and the same trial, with the wait put back: rolled back for want of asking") {
  // What this firmware did until this task, through the real library: `restore(state, awayMs)`
  // with the state exactly as the store holds it, which is what `loadState()` used to hand over.
  // The window and the wait are both 300 s, so the first heartbeat the library would allow comes
  // at the window's end at the earliest - and the trial has already rejected the image.
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  offerSelfImage(bench, Candidate("self-a"));
  const mm::WakeState saved = aWaitOf300Seconds(bench);
  switchAndBoot(bench);
  REQUIRE(bench.half().backend.holdLaidAsideMs() == saved.holdForMs);
  REQUIRE(bench.half().loop.restore(saved, 0));  // the wait owed whole again
  REQUIRE(bench.half().loop.retrySeconds() > 290);

  const std::size_t before = bench.server().heartbeats.size();
  (void)bench.v().run();
  CHECK(bench.server().heartbeats.size() == before);  // not one was sent
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.v().self.status().state == upd::SelfState::kRolledBack);
  CHECK(bench.v().self.status().reason == upd::SelfReason::kNoUplink);
  // And the journal says which kind of *not reached* it was: nothing was ever asked.
  upd::TrialUplink tried;
  REQUIRE(bench.v().self.lastUplink(&tried));
  CHECK(tried.nothingAsked());
  CHECK(tried.last == upd::UplinkVerdict::kNotTried);
}

TEST_CASE("a boot that is not on trial and owes nobody a result: the wait is owed whole, as ever") {
  // The other half of the rule. A restart, a power-on, a crash, a wake on the radio's line: the
  // time away is 0 for all of them and the wait is still real time, so it is owed whole. Nothing
  // of task p14-05 touches this, and the day it does a unit that cannot reach its server stops
  // backing off.
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  const mm::WakeState saved = aWaitOf300Seconds(bench);

  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.half().backend.holdLaidAsideMs() == 0);
  CHECK(bench.half().backend.holdOwedBeforeSleepMs() == saved.holdForMs);
  CHECK(bench.half().loop.retrySeconds() > 290);

  const std::size_t before = bench.server().heartbeats.size();
  CHECK(oneExchange(bench) == interfaces::Exchanged::kNothing);
  CHECK(bench.server().heartbeats.size() == before);
  CHECK(has(bench.half().backend.lastReport().why, "wait"));
}

TEST_CASE("a trial whose first heartbeat fails: the second is the one that confirms it") {
  // One slow attach, one gateway restarting, one deploy. The trial's own loop asks the seam for
  // one more heartbeat each time the library's wait has run out; nothing else of the wake is
  // repeated for it.
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  offerSelfImage(bench, Candidate("self-a"));
  switchAndBoot(bench);
  bench.server().calls.clear();
  bench.server().failNextOn("/heartbeat", 503, 1);  // the wake's own, and nothing after it

  const Pass& pass = bench.v().run();
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.v().self.status().state == upd::SelfState::kConfirmed);
  CHECK(heartbeatsSent(bench) == 2);          // two on the wire
  CHECK(bench.v().cycle->trialBeats() == 1);  // one of them the trial's
  CHECK(bench.half().backend.counters().trial_beats == 1);
  CHECK(bench.half().backend.counters().trial_beats_held > 0);  // and the asks the wait held off
  CHECK(bench.v().self.status().uplink_tries == 2);
  CHECK(bench.v().self.status().uplink_last == upd::UplinkVerdict::kReached);
  // **Nothing but the heartbeat.** Every other route the wake took, it took once.
  int heartbeats = 0;
  int others = 0;
  for (const std::string& call : bench.server().calls) {
    if (call == "/agent/v1/heartbeat") ++heartbeats;
    else ++others;
  }
  CHECK(heartbeats == 2);
  CHECK(others <= 1);  // the config call of this boot, at most
  // The wake's record says the server was reached: step 8 failed, and the trial got through
  // afterwards in the same wake.
  CHECK(pass.reached == interfaces::Reached::kAnswered);
}

TEST_CASE("a trial in which every heartbeat fails: rolled back, and the journal says how many were tried") {
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  offerSelfImage(bench, Candidate("self-a"));
  switchAndBoot(bench);
  bench.server().calls.clear();
  bench.server().failWith(503);

  (void)bench.v().run();
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.v().self.status().state == upd::SelfState::kRolledBack);
  CHECK(bench.v().self.status().reason == upd::SelfReason::kNoUplink);
  // The wake's own heartbeat, and the trial's for as long as the window lasted - each one when
  // the library's growing backoff had run out, and not one before it.
  const int sent = heartbeatsSent(bench);
  CHECK(sent > 1);
  CHECK(bench.v().cycle->trialBeats() == sent - 1);

  upd::TrialUplink tried;
  REQUIRE(bench.v().self.lastUplink(&tried));
  CHECK_FALSE(tried.nothingAsked());
  CHECK(tried.tries == static_cast<std::uint32_t>(sent));
  CHECK(tried.last == upd::UplinkVerdict::kFailed);
  CHECK(tried.elapsed_ms >= upd::kTrialWindowMs);
  // What a person reads of the same figures is held by the console's own tests
  // (host_test/core/console/test_console_self.cpp): this bench's console has no receiver and
  // answers `self status` with *there is no self-update in this build*.
}

TEST_CASE("the owed result of a rollback is not held up either: the old image's first wake posts it") {
  // The whole of 15 November, with this task's change in: the apply, the trial, the rollback -
  // and then the boot of the old image, which owes meshmaster.dk the apply's result and owes the
  // library a hold the trial's own failed heartbeats left. Until this it sent nothing and the
  // timer wake eleven minutes later is what told the server.
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  const Candidate image("self-a");
  bench::serveRelease(bench.world.http.server, image, bench::kBase, nullptr, nullptr, nullptr, true);
  bench.server().queue(
      bench::ownJob(21, agent::kOfferType, bench::offerPayload(image, agent::kProductWalter)).json());
  (void)bench.oneWake();
  REQUIRE(bench::member(bench::objectOf(bench.lastResult(21), "data"), "stored") == "true");
  bench.server().queue(
      bench::ownJob(22, agent::kApplyType,
                    bench::applyPayload(bench::sha256Hex(image.file), agent::kProductWalter))
          .json());
  (void)bench.oneWake();
  REQUIRE(bench.v().restart.asked == 1);
  REQUIRE(bench.lastResult(22).empty());
  REQUIRE(bench.half().firmware.resultOwed());

  // The trial, with a server that answers nothing: rolled back for want of an uplink, and the
  // heartbeats it did try leave the library a hold of its own.
  bench.world.own.reboot();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  REQUIRE(bench.v().self.onTrial());
  bench.server().failWith(503, "300");
  (void)bench.v().run();
  REQUIRE(bench.v().self.status().state == upd::SelfState::kRolledBack);
  REQUIRE(bench.v().self.status().reason == upd::SelfReason::kNoUplink);
  REQUIRE(bench.half().loop.state().holdForMs > 0);

  // The old image boots. It owes a result, so the wait is laid aside and its **first** wake is
  // the one that posts it.
  bench.server().answerAgain();
  bench.world.own.reboot();
  REQUIRE(bench.world.own.runningIndex() == 0);
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  CHECK_FALSE(bench.v().self.onTrial());
  CHECK(bench.half().firmware.owedResultDue());
  CHECK(bench.half().backend.holdLaidAsideMs() > 0);
  CHECK(std::string(bench.half().backend.holdLaidAsideWhy()) == "a rollback's result is owed");

  (void)bench.v().run();
  const std::string result = bench.lastResult(22);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  CHECK(bench::member(data, "outcome") == "rolledBack");
  // ... and the server is told which kind of *not reached* it was, in the same sentence.
  const std::string reason = bench::member(data, "reason");
  INFO(reason);
  CHECK(has(reason, "rolled back: the backend was not reached inside the trial window"));
  CHECK(has(reason, "heartbeat(s) tried in "));
  CHECK(has(reason, "the last: the server was not reached"));
  CHECK(has(bench::objectOf(data, "journal"), "\"step\":\"uplink\""));
  CHECK_FALSE(bench.half().firmware.resultOwed());
}

TEST_CASE("agent state says the wait was laid aside, and what the trial asked for") {
  AgentBench bench;
  readyUnit(bench, Candidate("update-golden"));
  offerSelfImage(bench, Candidate("self-a"));
  const mm::WakeState saved = aWaitOf300Seconds(bench);
  switchAndBoot(bench);
  const std::string state = bench.v().runLine("agent state");
  INFO(state);
  CHECK(has(state, "agent state: restored across the last sleep"));
  const std::string laid = "; " + std::to_string(saved.holdForMs / 1000u) +
                           " s owed before the sleep, laid aside: this image is on trial\n";
  CHECK(has(state, laid.c_str()));
  CHECK(has(state, "the trial asked for 0 heartbeat(s) of its own, held off 0 time(s)"));
}
