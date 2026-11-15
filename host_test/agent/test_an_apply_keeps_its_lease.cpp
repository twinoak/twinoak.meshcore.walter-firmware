// An apply keeps its lease (task p14-04), on the wake bench with a simulated nRF52840 behind the
// RAK, the real update machine, the real flasher and the scripted meshmaster.dk - with **the
// flash taking as long as it really does**. The unit's own journal says a flash of the radio's
// application region is 14 min 44 s (13 November, job 28560) and 14 min 51 s (14 November, job
// 29475); the simulated chip's flash costs a fifth of a second, so the time is put back by a
// progress sink on the flasher, which spends the named stretch of the test's clock at the start
// of every flash. Everything else is the firmware's own.
//
// What these cases are about:
//
//   the lease     the flash is **one step** of the machine and the machine cannot come back to
//                 ask for more lease in the middle of it. meshmaster.dk sets a lease to *now plus
//                 what was asked*, so the 900 seconds an apply used to ask for turned the thirty
//                 minutes the claim gave into fifteen, and the flash ended 15 seconds (the 13th)
//                 and 8 seconds (the 14th) before the lease. One image bigger and the server
//                 fails a job whose radio is being updated perfectly well.
//   the silence   for eighteen minutes the job's page said *nothing has come back yet*. The
//                 journal is posted as it is written now, a line at a time.
//   the bound     a rollback is the flash and the proof **twice**. At the thirty minutes an apply
//                 used to raise the wake to, the bound fell in the middle of the second flash:
//                 the two cases at the end show what that does and what it does now.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "core/cycle/wake_guard.h"
#include "core/flasher/flasher.h"
#include "core/update/radio_update.h"
#include "fixtures/manifest_files.h"

using bench::AgentBench;
using bench::Candidate;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;
using core::image::Which;

namespace {

// The measured flash, rounded up to the half-minute above the two runs: 15 min 30 s.
constexpr std::uint64_t kMeasuredFlashMs = 930000;
// What an apply raised the wake to before this task, and what `kRaisedBoundMax` was.
constexpr std::uint64_t kOldBoundMs = 30u * 60 * 1000;

// The flasher's progress sink with a stopwatch in it: at the first page of every flash's scan it
// spends `takes_ms` of the world's clock, and it keeps when each flash began and ended. The
// simulated chip's own NVMC timings are microseconds a word (host_test/sim/nrf52_chip.h), which is
// what makes a whole-suite run affordable and what makes a flash in a test take no time at all.
class SlowFlash final : public core::flasher::ProgressSink {
 public:
  SlowFlash(fakes::FakeClock& clock, std::uint64_t takes_ms) : clock_(clock), takes_ms_(takes_ms) {}

  void onProgress(const core::flasher::Progress& p) override {
    if (p.step != core::flasher::Step::kScan || p.pages_done != 1) return;
    began_ms.push_back(clock_.nowMs());
    clock_.advance(takes_ms_);
    ended_ms.push_back(clock_.nowMs());
  }

  std::size_t flashes() const { return began_ms.size(); }

  std::vector<std::uint64_t> began_ms;
  std::vector<std::uint64_t> ended_ms;

 private:
  fakes::FakeClock& clock_;
  std::uint64_t takes_ms_;
};

// A unit ready for an update: the golden image on the chip and in the store, running.
void readyUnit(AgentBench& bench, const Candidate& golden) {
  REQUIRE(bench.world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
}

// One whole wake, as `oneWake()` runs one, with the sink on the flasher of the Walter this boot
// builds (a wake destroys the Walter and makes another, so it cannot be set once at the start).
const Pass& wakeFlashing(AgentBench& bench, SlowFlash& slow) {
  bench.boot();
  bench.v().update.setProgress(&slow);
  bench.v().begin();
  return bench.v().run();
}

// The contract's own shape, as `host_test/fixtures/contracts-ahead/` holds it: this firmware's
// reading of a contract agreed in words, until meshmaster.dk's task 227 makes the server's file
// (that directory's README, and the follow-up that replaces it).
std::string aheadFixture(const char* name) {
  const std::string path =
      std::string(TWINOAK_REPO_ROOT) + "/host_test/fixtures/contracts-ahead/" + name + ".json";
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read " << path);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

// The first object of a JSON array, braces and all.
std::string firstObject(const std::string& array) {
  const std::size_t begin = array.find('{');
  if (begin == std::string::npos) return "";
  return bench::objectOf("{\"x\":" + array.substr(begin), "x");
}

// The **top-level** `note` of a progress body. `member()` has no notion of depth, and a journal
// entry's own `note` comes first in the bytes.
std::string topNote(const std::string& report) {
  const std::string data = bench::objectOf(report, "data");
  const std::size_t after = report.find(data) + data.size();
  return bench::member(report.substr(after), "note");
}

// The `step` of every entry of a journal array, in order.
std::vector<std::string> stepsOf(const std::string& journal) {
  std::vector<std::string> steps;
  std::size_t at = 0;
  while ((at = journal.find("\"step\":\"", at)) != std::string::npos) {
    const std::size_t begin = at + 8;
    const std::size_t end = journal.find('"', begin);
    if (end == std::string::npos) break;
    steps.push_back(journal.substr(begin, end - begin));
    at = end;
  }
  return steps;
}

// The last extension the server took at or before `when`, and whether there was one.
bool extensionBefore(const std::vector<bench::SeenExtend>& all, std::uint64_t when, bench::SeenExtend* out) {
  bool found = false;
  for (const bench::SeenExtend& one : all) {
    if (one.at_ms > when) continue;
    *out = one;
    found = true;
  }
  return found;
}

}  // namespace

TEST_CASE("an apply whose flash takes 15 min 30 s: the lease is never behind the result") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  bench.server().queue(
      bench::ownJob(31, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
  SlowFlash slow(bench.world.clock, kMeasuredFlashMs);

  const Pass& pass = wakeFlashing(bench, slow);
  CHECK(pass.carried == interfaces::Ask::kUpdateRadio);
  REQUIRE(slow.flashes() == 1);

  // It committed, and the result went out in this same wake.
  const std::string result = bench.lastResult(31);
  REQUIRE_FALSE(result.empty());
  CHECK(bench::member(result, "outcome") == "ok");
  CHECK(bench::member(bench::objectOf(result, "data"), "outcome") == "committed");

  // **The lease, by the order and the times of the calls.** The server holds a lease at now plus
  // what was asked, and the result came in before it lapsed.
  const std::vector<bench::PostedResult>& posted = bench.server().results[31];
  REQUIRE_FALSE(posted.empty());
  CHECK_FALSE(posted.back().late);
  const bool inside_the_lease = posted.back().at_ms <= posted.back().lease_until_ms;
  CHECK(inside_the_lease);

  // Nothing asked for more than the server's ceiling, so nothing was refused as invalid.
  const std::vector<bench::SeenExtend>& extends = bench.server().extensions[31];
  REQUIRE(extends.size() >= 2);
  for (const bench::SeenExtend& one : extends) {
    CHECK(one.seconds <= bench::kMaxExtendSeconds);
    CHECK_FALSE(one.refused);
  }

  // And the one immediately before the flash asked for the whole of it.
  bench::SeenExtend before;
  REQUIRE(extensionBefore(extends, slow.began_ms.front(), &before));
  CHECK(before.seconds == agent::kLeaseFlashSeconds);
  CHECK(slow.began_ms.front() - before.at_ms < 60000);  // immediately: no step of the machine between

  // What it was before this task, from the same run: the flash alone outlasts the 900 seconds the
  // apply used to ask for, so the lease would have lapsed with the radio mid-flash.
  const std::uint64_t flashing_ms = slow.ended_ms.front() - before.at_ms;
  CHECK(flashing_ms > static_cast<std::uint64_t>(agent::kLeaseExtendSeconds) * 1000u);
  // The lease the flash's own extension bought outlasts the whole run, proving included.
  CHECK(before.until_ms > posted.back().at_ms);
}

TEST_CASE("a rollback asks for the ceiling immediately before the golden image's flash") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  // The candidate boots and says the old version: the health check's first step fails and the
  // golden image is flashed back - the second flash of the same wake.
  bench.world.chip->boot = bench::Boot::kOldVersion;
  bench.world.chip->old_version = golden.version;
  bench.server().queue(
      bench::ownJob(32, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
  SlowFlash slow(bench.world.clock, kMeasuredFlashMs);

  (void)wakeFlashing(bench, slow);
  REQUIRE(slow.flashes() == 2);
  const std::string data = bench::objectOf(bench.lastResult(32), "data");
  REQUIRE_FALSE(data.empty());
  CHECK(bench::member(data, "outcome") == "rolledBack");
  CHECK(bench.world.chip->chipHolds(golden));

  const std::vector<bench::SeenExtend>& extends = bench.server().extensions[32];
  bench::SeenExtend second;
  REQUIRE(extensionBefore(extends, slow.began_ms[1], &second));
  CHECK(second.seconds == agent::kLeaseFlashSeconds);
  CHECK(slow.began_ms[1] - second.at_ms < 60000);
  // It is a second extension and not the first flash's: the two are a flash and a proof apart.
  CHECK(second.at_ms > slow.ended_ms[0]);

  // **And this is the whole of the finding about the bound.** With the measured flash, the second
  // flash begins before the thirty minutes an apply used to raise the wake to and ends after it:
  // the old bound fell *inside* a step the machine cannot stop.
  CHECK(slow.began_ms[1] - slow.began_ms[0] < kOldBoundMs);
  CHECK(slow.ended_ms[1] - slow.began_ms[0] > kOldBoundMs);
  // The bound now is the machine's own number, and the whole run fits inside it.
  const bool fits = slow.ended_ms[1] - slow.began_ms[0] < agent::kApplyBoundMs;
  CHECK(fits);
}

TEST_CASE("the journal as it is written: one progress report a line, the last one the result's") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  bench.server().queue(
      bench::ownJob(33, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
  (void)bench.oneWake();

  const std::string data = bench::objectOf(bench.lastResult(33), "data");
  REQUIRE_FALSE(data.empty());
  CHECK(bench::member(data, "outcome") == "committed");
  const std::string journal = bench::objectOf(data, "journal");
  const std::vector<std::string>& reports = bench.server().progress[33];

  // One call a line. The result's journal is six lines on a good path (check, flashing-candidate,
  // booting, proving, committing, done) and every one of them was posted as it was written.
  const std::vector<std::string> steps = stepsOf(journal);
  CHECK(steps.size() == 6);
  CHECK(reports.size() == steps.size());
  CHECK(bench.half().firmware.counters().progress_posted == reports.size());
  CHECK(bench.half().firmware.counters().progress_refused == 0);

  // The contract's shape, against the fixture and not a transcription of it.
  const std::string fixture = aheadFixture("FirmwareApplyProgress");
  REQUIRE_FALSE(reports.empty());
  for (const std::string& report : reports) {
    INFO(report);
    CHECK(bench::topKeys(report) == bench::topKeys(fixture));
    CHECK(bench::topKeys(bench::objectOf(report, "data")) ==
          bench::topKeys(bench::objectOf(fixture, "data")));
    const std::string one = firstObject(bench::objectOf(bench::objectOf(report, "data"), "journal"));
    CHECK(bench::topKeys(one) ==
          bench::topKeys(firstObject(bench::objectOf(bench::objectOf(fixture, "data"), "journal"))));
  }

  // The journal grows a line at a time, oldest first, and the note is the newest line's step.
  for (std::size_t i = 0; i < reports.size(); ++i) {
    const std::vector<std::string> so_far =
        stepsOf(bench::objectOf(bench::objectOf(reports[i], "data"), "journal"));
    CHECK(so_far.size() == i + 1);
    CHECK(so_far.back() == steps[i]);
    CHECK(topNote(reports[i]) == steps[i]);
  }
  // And the last one's journal is the result's, entry for entry.
  CHECK(bench::objectOf(bench::objectOf(reports.back(), "data"), "journal") == journal);

  // One progress call between two steps and never inside one: every report was posted while the
  // machine stood still, so no two of them bracket a flash.
  const std::vector<std::uint64_t>& when = bench.server().progress_at_ms[33];
  REQUIRE(when.size() == reports.size());
  for (std::size_t i = 1; i < when.size(); ++i) CHECK(when[i] >= when[i - 1]);
}

TEST_CASE("every progress call refused, or never answered: the apply is what it is without them") {
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");

  // What the journal and the result say when every progress call is taken.
  std::vector<std::string> plain_steps;
  std::string plain_outcome;
  {
    AgentBench bench;
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    bench.server().queue(
        bench::ownJob(34, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
    (void)bench.oneWake();
    const std::string data = bench::objectOf(bench.lastResult(34), "data");
    REQUIRE_FALSE(data.empty());
    plain_outcome = bench::member(data, "outcome");
    plain_steps = stepsOf(bench::objectOf(data, "journal"));
  }

  SUBCASE("the server refuses every one of them") {
    AgentBench bench;
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    bench.server().failRoute("/jobs/34/progress", 503);
    bench.server().queue(
        bench::ownJob(34, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
    (void)bench.oneWake();
    const std::string data = bench::objectOf(bench.lastResult(34), "data");
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == plain_outcome);
    CHECK(stepsOf(bench::objectOf(data, "journal")) == plain_steps);
    CHECK(bench.server().progress[34].empty());
    CHECK(bench.half().firmware.counters().progress_posted == 0);
    CHECK(bench.half().firmware.counters().progress_refused == plain_steps.size());
    CHECK(bench.world.chip->chipHolds(candidate));
  }

  SUBCASE("the server never answers one of them") {
    AgentBench bench;
    readyUnit(bench, golden);
    REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
    bench.server().silenceRoute("/jobs/34/progress");
    bench.server().queue(
        bench::ownJob(34, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
    (void)bench.oneWake();
    const std::string data = bench::objectOf(bench.lastResult(34), "data");
    REQUIRE_FALSE(data.empty());
    CHECK(bench::member(data, "outcome") == plain_outcome);
    CHECK(stepsOf(bench::objectOf(data, "journal")) == plain_steps);
    CHECK(bench.half().firmware.counters().progress_posted == 0);
    CHECK(bench.half().firmware.counters().progress_refused == plain_steps.size());
    CHECK(bench.world.chip->chipHolds(candidate));
  }
}

TEST_CASE("a rollback longer than the bound on the wake: what is cut off, and what is not") {
  // **What a unit did before this task, shown with a flash long enough to do it again.** At the
  // thirty minutes an apply used to raise the wake to, a measured flash of 14 min 51 s put the
  // bound inside the second flash (the case above measures that). The bound is a whole run now,
  // so the same thing needs a longer flash: twenty-five minutes, which puts the second flash
  // across `kApplyBoundMs` exactly as fifteen minutes put it across thirty.
  //
  // What it does is the finding. **The flash is not interrupted** - the bound is read between two
  // steps and a flash is one step, so the radio is never left half-written by it - but the apply
  // returns with no result, the lease is left to lapse, and the server is told nothing at all
  // about a job whose radio is in fact being put back. The work is in the journal and the next
  // boot finishes it; meshmaster.dk fails the job meanwhile.
  constexpr std::uint64_t kFlashAcrossTheBoundMs = 25u * 60 * 1000;
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  bench.world.chip->boot = bench::Boot::kOldVersion;
  bench.world.chip->old_version = golden.version;
  bench.server().queue(
      bench::ownJob(35, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
  SlowFlash slow(bench.world.clock, kFlashAcrossTheBoundMs);

  (void)wakeFlashing(bench, slow);
  REQUIRE(slow.flashes() == 2);
  // The bound fell inside the second flash, and the flash ran to its end all the same.
  CHECK(slow.began_ms[1] - slow.began_ms[0] < agent::kApplyBoundMs);
  CHECK(slow.ended_ms[1] - slow.began_ms[0] > agent::kApplyBoundMs);
  CHECK(bench.world.chip->chipHolds(golden));  // the golden image is on the chip, whole

  // No result, and the lease left: nothing can say what the machine has not decided.
  CHECK(bench.lastResult(35).empty());
  CHECK(bench.half().firmware.counters().rolled_back == 0);
  // The progress reports are what the server did hear, and they are the only thing it heard:
  // the last one says `rolling-back`, which is at least a person reading *the golden image is
  // going back on* rather than nothing at all. The line written **after** the second flash does
  // not reach it - the uplink's deadline for this job is the apply's own bound, and the flash
  // ended past it - so the call is refused here and counted, which is the same ending as a server
  // that would not take it.
  const std::vector<std::string>& reports = bench.server().progress[35];
  REQUIRE_FALSE(reports.empty());
  CHECK(topNote(reports.back()) == "rolling-back");
  CHECK(bench.half().firmware.counters().progress_refused >= 1);

  // **And the next boot puts it right.** The journal carried the run; step 3 resumes it, the
  // golden image proves itself, and the radio is running it.
  bench.world.chip->boot = bench::Boot::kNormal;
  const Pass& after = bench.oneWake();
  CHECK(after.outcomeOf(Step::kResume) == Outcome::kDone);
  CHECK(bench.v().update.state() == core::update::State::kRestored);
  CHECK(bench.world.chip->chipHolds(golden));
  CHECK(bench.world.images.store.badCount() == 1);  // the candidate is on the bad list
}

TEST_CASE("the same rollback at the measured flash: one wake, one result, inside the lease") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) == bench::StoreError::kNone);
  bench.world.chip->boot = bench::Boot::kOldVersion;
  bench.world.chip->old_version = golden.version;
  bench.server().queue(
      bench::ownJob(36, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file))).json());
  SlowFlash slow(bench.world.clock, kMeasuredFlashMs);

  (void)wakeFlashing(bench, slow);
  REQUIRE(slow.flashes() == 2);
  // Past the thirty minutes of before, and inside the bound now.
  CHECK(slow.ended_ms[1] - slow.began_ms[0] > kOldBoundMs);

  const std::vector<bench::PostedResult>& posted = bench.server().results[36];
  REQUIRE_FALSE(posted.empty());
  CHECK(bench::member(bench::objectOf(posted.back().body, "data"), "outcome") == "rolledBack");
  CHECK_FALSE(posted.back().late);
  CHECK(bench.half().firmware.counters().rolled_back == 1);
  CHECK(bench.world.chip->chipHolds(golden));
}
