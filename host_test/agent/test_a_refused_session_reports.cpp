// **A refused session reports** (task p18-06).
//
// NoiseScope session 5, 24 November 2026, meshmaster.dk job 43526, Walter 0.1.12: *flashing-scanner*
// at 13:13:34Z, the first try's failure at 13:14:07Z, **refused** at 13:15:01Z - and then nothing.
// The result said `"outcome":"refused"` with no `refusal`, and meshmaster.dk's door answers that
// with *A refused session names which precondition stopped it* and keeps **nothing** of the result.
// The job stayed leased for its hour and ended as *the exit node took this job and never reported a
// result*, which is not what happened.
//
// Four things are held here:
//
//   the word. Every way a session and an apply end `refused` carries one of the contract's words,
//   and `flash` is the one for a flash that never began (`test_every_refusal_has_a_word.cpp` counts
//   them);
//   the lines. The second try's failure has a line of its own, under the step it happened at, and
//   the ending has its own sentence - whole, because no line this firmware writes into a journal is
//   ever cut (`test_no_journal_line_is_cut.cpp` holds the bound);
//   the reading. A session's result posted from the journal after a restart says what the journal's
//   last stage was and not what one bit said: `refused` for a session that was refused, `restored`
//   only for one the golden image came back from;
//   the owing. A result the door gives its verdict on - a 400, a 404 or a 409 - settles the entry
//   and is not posted again, because the same bytes would get the same answer at every wake.
//
// The script of the server holds the door's own rule from this task on
// (`meshmaster_script.h`, `sessionRefusals`), so a `refused` result with no word fails a test here
// rather than a session on a unit.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "agent/session_job.h"
#include "core/update/radio_update.h"
#include "core/update/scanner_session.h"

using bench::AgentBench;
using bench::Candidate;
using bench::has;
using bench::objectAt;
using bench::SessionJob;
using bench::stepsOf;
using core::image::Which;
namespace upd = core::update;

namespace {

using FailAt = sim::Nrf52Chip::FailFlashAt;

const std::string NL = "\n";

// The task's own *The contract* line, read at run time: the one line of
// `tasks/phase-18/06-a-refused-session-reports.md` that begins a JSON object. meshmaster.dk's task
// 260 carries the same line, and the fixture is it until that task makes the server's own file
// (`host_test/fixtures/contracts-ahead/README.md`).
std::string taskContractLine() {
  const std::string path =
      std::string(TWINOAK_REPO_ROOT) + "/tasks/phase-18/06-a-refused-session-reports.md";
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read " << path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("{\"", 0) == 0) lines.push_back(line);
  }
  REQUIRE(lines.size() == 1);
  return lines[0];
}

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

// Every `note` of a journal array, in order.
std::vector<std::string> notesOf(const std::string& journal) {
  std::vector<std::string> notes;
  std::size_t at = 0;
  while ((at = journal.find("\"note\":\"", at)) != std::string::npos) {
    const std::size_t begin = at + 8;
    const std::size_t end = journal.find('"', begin);
    if (end == std::string::npos) break;
    notes.push_back(journal.substr(begin, end - begin));
    at = end;
  }
  return notes;
}

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& one : lines) text += one + "\n";
  return text;
}

// The `at` of every entry of a journal array, in order.
std::vector<std::string> timesOf(const std::string& journal) {
  std::vector<std::string> times;
  std::size_t at = 0;
  while ((at = journal.find("\"at\":\"", at)) != std::string::npos) {
    const std::size_t begin = at + 6;
    const std::size_t end = journal.find('"', begin);
    if (end == std::string::npos) break;
    times.push_back(journal.substr(begin, end - begin));
    at = end;
  }
  return times;
}

// **The two things of the fixture this bench's world cannot have**, substituted so that everything
// else is held byte for byte (the report says so under *Deviations*):
//
//   the clock. The unit's was the network's at 13:13:33Z; the bench's is its own simulated one, and
//   a test that pinned the five stamps to the second would be pinning how long each call of the
//   wake takes;
//   the address. `0x036000` is where the scan stalled on the unit, which is where that chip's
//   SoftDevice puts the application; the generated images of this suite load at `0x026000` and the
//   simulated chip's SoftDevice says so too, so the note cannot read anything else.
std::string asTheUnitWouldHaveSentIt(const std::string& data, const std::string& fixture) {
  const std::vector<std::string> mine = timesOf(data);
  const std::vector<std::string> theirs = timesOf(fixture);
  REQUIRE(mine.size() == theirs.size());
  std::string text = data;
  for (std::size_t i = 0; i < mine.size(); ++i) {
    const std::size_t at = text.find("\"at\":\"" + mine[i] + "\"");
    REQUIRE(at != std::string::npos);
    text = text.substr(0, at) + "\"at\":\"" + theirs[i] + "\"" +
           text.substr(at + 6 + mine[i].size() + 1);
  }
  std::size_t where = 0;
  while ((where = text.find("0x026000", where)) != std::string::npos) {
    text = text.substr(0, where) + "0x036000" + text.substr(where + 8);
    where += 8;
  }
  return text;
}

// The newest `kind::kAgentJob` entry of the journal.
bool lastJobEntry(bench::WakeWalter& w, agent::JobEntry* out) {
  upd::Entry e;
  if (!w.journal.lastOf(upd::kind::kAgentJob, &e)) return false;
  return agent::decode(e, out);
}

}  // namespace

// ------------------------------------------------------------------ the contract, byte for byte

TEST_CASE("the refused session's fixture is the task's own line, and a result written from its values is that fixture") {
  const std::string fixture = aheadFixture("NoiseScopeSessionResultRefused");
  CHECK(fixture == taskContractLine());

  // The five lines of 24 November, and the two words of the result.
  const std::uint32_t at = 1795526013;  // 2026-11-24T13:13:33Z
  struct Written {
    std::uint32_t epoch;
    const char* step;
    const char* note;
  };
  const Written written[5] = {
      {at, "check", "battery 4112 mV, link up, clock set"},
      {at + 1, "flashing-scanner", "the scanner image is going on"},
      {at + 34, "flashing-scanner", "scan: swd wait-timeout at 0x036000; nothing written"},
      {at + 88, "flashing-scanner", "2nd try scan: swd wait-timeout at 0x036000; nothing written"},
      {at + 88, "refused", "the flash did not begin, twice; the radio is as it was"},
  };
  agent::JobJournalLine journal[5];
  for (std::size_t i = 0; i < 5; ++i) {
    journal[i].at_epoch = written[i].epoch;
    journal[i].step = written[i].step;
    for (std::size_t k = 0; written[i].note[k] != 0; ++k) journal[i].note[k] = written[i].note[k];
  }

  agent::SessionResult result;
  result.outcome = agent::SessionOutcome::kRefused;
  result.steps = 1;
  result.step[0] = agent::SessionStepResult{agent::endWord(upd::StepEnd::kNone), 0, 0};
  result.lines = 0;
  result.lines_unanswered = 0;
  result.refusal = upd::word(upd::Refusal::kFlash);

  char out[agent::kDataBytes];
  const std::size_t n = agent::writeSessionResult(result, journal, 5, out, sizeof out);
  REQUIRE(n != 0);
  CHECK(std::string(out, n) == fixture);
  // The epochs of the fixture are the run's own times, read back as the fixture writes them.
  const std::vector<std::string> times = timesOf(fixture);
  REQUIRE(times.size() == 5);
  CHECK(times[0] == "2026-11-24T13:13:33Z");
  CHECK(times[4] == "2026-11-24T13:15:01Z");
}

// ------------------------------------------------------------------ 24 November, on the bench

TEST_CASE("the session of 24 November: refused with flash, five whole lines, and the fixture's own result") {
  SessionJob unit;
  // The battery the unit read that afternoon, for the `check` line the fixture carries.
  unit.world.rak.battery_mv = 4112;
  // The scan's reads stalling on a bus the application is using, at every flash: three sessions of
  // three on 0.1.12 failed at the same address, 32 or 33 s in, and the second try failed in this
  // one. Why is a task of its own (this task's *Out of scope*).
  unit.world.chip->chip.failFlashAt(FailAt::kScanRead, 0);
  unit.server().queue(
      bench::ownJob(43526, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());

  (void)unit.oneWake();
  const std::string result = unit.lastResult(43526);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  INFO(data);

  // **The result is the fixture**, member for member and byte for byte, but for the clock and the
  // address this world cannot have.
  const std::string fixture = aheadFixture("NoiseScopeSessionResultRefused");
  CHECK(asTheUnitWouldHaveSentIt(data, fixture) == fixture);

  // And the five lines, said out loud: the check, the flash begun, each try's own failure under the
  // step it happened at, and the ending's own sentence.
  const std::string journal = bench::objectOf(data, "journal");
  const std::vector<std::string> steps = stepsOf(journal);
  const std::vector<std::string> notes = notesOf(journal);
  REQUIRE(steps.size() == 5);
  REQUIRE(notes.size() == 5);
  CHECK(steps[0] == "check");
  CHECK(steps[1] == "flashing-scanner");
  CHECK(steps[2] == "flashing-scanner");
  CHECK(steps[3] == "flashing-scanner");
  CHECK(steps[4] == "refused");
  CHECK(notes[2] == "scan: swd wait-timeout at 0x026000; nothing written");
  CHECK(notes[3] == "2nd try scan: swd wait-timeout at 0x026000; nothing written");
  CHECK(notes[4] == "the flash did not begin, twice; the radio is as it was");
  // Whole: every one of them is shorter than the note a journal line holds, so none was cut.
  for (const std::string& note : notes) {
    CAPTURE(note);
    CHECK(note.size() < agent::kJournalNoteBytes);
  }

  // The server took it: the rule the script now holds is the door's, and `flash` is in the list
  // the server has after its task 260.
  CHECK(unit.server().results_refused_for_the_refusal == 0);
  CHECK(unit.server().results[43526].size() == 1);
  CHECK(bench::member(data, "refusal") == "flash");
  CHECK(bench::member(data, "outcome") == "refused");
  // The radio never left the mesh, so nothing of this is a rollback: no flash, no bad list.
  CHECK(unit.world.chip->chip.counters().erases == 0);
  CHECK(unit.world.images.store.badCount() == 0);
  MESSAGE("the result the server was given:" << NL << data);
  MESSAGE("the journal's five lines:" << NL << joined(notes));
}

TEST_CASE("the same session against the list the server has today: the result is turned away, the entry is settled, and the next wake posts nothing") {
  SessionJob unit;
  unit.world.rak.battery_mv = 4112;
  // The server as it is deployed on 24 November: `NoiseScopeRefusals.All` without `flash`. The
  // door answers *a refusal is one of ...* with a 400 and keeps nothing - no worse than the 400 the
  // unit really met, which was for no word at all.
  unit.server().sessionRefusalsAsDeployed();
  unit.world.chip->chip.failFlashAt(FailAt::kScanRead, 0);
  unit.server().queue(
      bench::ownJob(43526, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());

  (void)unit.oneWake();
  CHECK(unit.server().results_refused_for_the_refusal == 1);
  CHECK(has(unit.server().last_violation, "A refusal is one of"));
  CHECK(unit.server().results[43526].empty());  // nothing of it was kept, as on the server
  MESSAGE("what the door said: " << unit.server().last_violation);

  // **The entry is settled**: the journal says the server did not take it, with the status, and
  // `resultOwed` finds nothing to post.
  agent::JobEntry entry;
  REQUIRE(lastJobEntry(*unit.walter, &entry));
  CHECK(entry.job_id == 43526);
  CHECK(entry.state == agent::JobEntryState::kNotTaken);
  CHECK(entry.http_status == 400);
  CHECK(unit.half().firmware.counters().results_not_taken == 1);
  CHECK_FALSE(unit.half().firmware.resultOwed());

  // **And the next wake posts nothing**: the same bytes would get the same answer.
  const int posts = unit.server().requestsTo("/agent/v1/jobs/43526/result");
  CHECK(posts == 1);
  (void)unit.oneWake();
  CHECK(unit.server().requestsTo("/agent/v1/jobs/43526/result") == posts);
  CHECK(unit.server().results[43526].empty());
}

// ------------------------------------------------------------------ posted from the journal

TEST_CASE("a session that ended refused and whose result a restart cut off: the next wake says refused with flash, not restored") {
  SessionJob unit;
  // The journal as the refusal left it - the stage is `refused`, which **closes** a session - and
  // the entry that says whose result is owed. Before this task the post read one bit, open or
  // closed, and a session that was refused was reported `restored`: the opposite of what happened.
  unit.aSessionOwedBy(43526, upd::Stage::kRefused, 1);
  REQUIRE(unit.half().firmware.resultOwed());
  CHECK_FALSE(unit.sessionIsOpen());

  (void)unit.oneWake();
  const std::string result = unit.lastResult(43526);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  INFO(data);
  CHECK(bench::member(data, "outcome") == "refused");
  CHECK(bench::member(data, "refusal") == "flash");
  CHECK(bench::member(result, "outcome") == "failed");
  // One row for the one step the payload had, and nothing measured.
  CHECK(bench::member(objectAt(bench::objectOf(data, "steps"), 0), "ended") == "notRun");
  CHECK(unit.server().results_refused_for_the_refusal == 0);
  CHECK_FALSE(unit.half().firmware.resultOwed());
  MESSAGE("the result posted from the journal:" << NL << data);
}

TEST_CASE("a session that was restored and whose result a restart cut off still says restored") {
  SessionJob unit;
  unit.aSessionOwedBy(777, upd::Stage::kRestored, 2);
  (void)unit.oneWake();
  const std::string data = bench::objectOf(unit.lastResult(777), "data");
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(bench::member(data, "refusal").empty());
}

TEST_CASE("a session left open and whose result a restart cut off still says stuck") {
  SessionJob unit;
  unit.theRadioIsAScanner();
  unit.anOpenSessionOwedBy(778, upd::Stage::kStep, 2);
  (void)unit.oneWake();
  const std::string data = bench::objectOf(unit.lastResult(778), "data");
  INFO(data);
  // Either this wake put the golden image back - and then it is `restored` - or it did not and the
  // session stays open. What it may never be is `refused`, which means nothing was touched.
  CHECK(bench::member(data, "refusal").empty());
  CHECK(bench::member(data, "outcome") != "refused");
}

// ------------------------------------------------------------------ the other two words

TEST_CASE("a session refused for want of memory says noMemory, and one the journal would not open says journal") {
  SUBCASE("no room for a step's lines") {
    SessionJob unit;
    unit.server().queue(
        bench::ownJob(801, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());
    // `oneWake()` builds the Walter over again, as a boot does, so the heap the session borrows
    // from is this life's: the knob goes on between the boot and the pass.
    unit.boot();
    unit.walter->scratch.onlyHas(upd::kStepBytes - 1);
    unit.walter->begin();
    (void)unit.walter->run();
    const std::string data = bench::objectOf(unit.lastResult(801), "data");
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "noMemory");
    CHECK(unit.server().results_refused_for_the_refusal == 0);
    CHECK(unit.world.chip->chip.counters().erases == 0);
  }
  SUBCASE("the journal would not take the entry that opens a session") {
    SessionJob unit;
    // A journal that takes no entry and reads as it always did: nothing of the session may begin,
    // because that entry is the only thing that would get the repeater back after a reset. Before
    // this task the result went out with no word at all - the one refusal the server could do
    // nothing about was also the one it was never told of.
    unit.world.journal_flash.refuse_writes = true;
    unit.server().queue(
        bench::ownJob(802, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());
    (void)unit.oneWake();
    const std::string data = bench::objectOf(unit.lastResult(802), "data");
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "journal");
    CHECK(unit.server().results_refused_for_the_refusal == 0);
    CHECK(unit.world.chip->chip.counters().erases == 0);
    MESSAGE("the result of a session the journal would not open:" << NL << data);
  }
}

// ------------------------------------------------------------------ an apply, the same two ways

TEST_CASE("an apply whose flash never begins: refused with flash, and a result the server will not take is settled once") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  REQUIRE(bench.world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) ==
          bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
  bench.world.chip->chip.failFlashAt(FailAt::kScanRead, 0);
  bench.server().queue(
      bench::ownJob(900, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file)))
          .json());

  SUBCASE("the server takes it") {
    (void)bench.oneWake();
    const std::string data = bench::objectOf(bench.lastResult(900), "data");
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "flash");
    // The failure's own line under the step it happened at, and the ending's own sentence.
    const std::vector<std::string> notes = notesOf(bench::objectOf(data, "journal"));
    const std::vector<std::string> steps = stepsOf(bench::objectOf(data, "journal"));
    REQUIRE(notes.size() >= 2);
    CHECK(notes[notes.size() - 2] ==
          "2nd try scan: swd wait-timeout at 0x026000; nothing written");
    CHECK(steps[steps.size() - 2] == "flashing-candidate");
    CHECK(notes.back() == "the flash did not begin, twice; the radio is as it was");
    CHECK(steps.back() == "refused");
    for (const std::string& note : notes) {
      CAPTURE(note);
      CHECK(note.size() < agent::kJournalNoteBytes);
    }
    MESSAGE("the apply's lines:" << NL << joined(notes));
  }
  SUBCASE("the server's door refuses the body") {
    // A 400 on the body, whatever its reason: the one answer the same bytes would get at every
    // wake. The entry is settled with the status and nothing is posted again.
    bench.server().failResults(400, 1);
    (void)bench.oneWake();
    CHECK(bench.lastResult(900).empty());
    agent::JobEntry entry;
    REQUIRE(lastJobEntry(*bench.walter, &entry));
    CHECK(entry.job_id == 900);
    CHECK(entry.state == agent::JobEntryState::kNotTaken);
    CHECK(entry.http_status == 400);
    CHECK(bench.half().firmware.counters().results_not_taken == 1);
    const int posts = bench.server().requestsTo("/agent/v1/jobs/900/result");
    (void)bench.oneWake();
    CHECK(bench.server().requestsTo("/agent/v1/jobs/900/result") == posts);
  }
}

// ------------------------------------------------------------------ what stays owed

TEST_CASE("a 401, a 429 and a 5xx on a result leave it owed, as they were") {
  CHECK(agent::ModemUplink::resultIsTheServersVerdict(400));
  CHECK(agent::ModemUplink::resultIsTheServersVerdict(404));
  CHECK(agent::ModemUplink::resultIsTheServersVerdict(409));
  // Everything a result may be answered with that can be different at the next wake.
  const int owed[] = {0, 401, 403, 408, 429, 500, 502, 503, 504};
  for (int status : owed) {
    CAPTURE(status);
    CHECK_FALSE(agent::ModemUplink::resultIsTheServersVerdict(status));
  }
}

TEST_CASE("a session's result the server answers 503 stays owed, and the next wake posts it again") {
  SessionJob unit;
  unit.aSessionOwedBy(43527, upd::Stage::kRefused, 1);
  unit.server().failResults(503, 1);
  (void)unit.oneWake();
  CHECK(unit.server().results[43527].empty());
  // Still owed: the entry was not settled, because a 503 says nothing about the body.
  CHECK(unit.half().firmware.resultOwed());
  CHECK(unit.half().firmware.counters().results_not_taken == 0);
  (void)unit.oneWake();
  const std::string data = bench::objectOf(unit.lastResult(43527), "data");
  INFO(data);
  CHECK(bench::member(data, "outcome") == "refused");
  CHECK(bench::member(data, "refusal") == "flash");
  CHECK_FALSE(unit.half().firmware.resultOwed());
}
