// **What the server is told about a flash that wrote nothing** (task p18-05, deliverables 2 and 4).
//
// On 23 November the job's page said `rolling-back: the flasher did not finish` and nothing else:
// which step of the flasher had failed was in the journal entry's `detail` and printed nowhere, so
// the one question a person had - *what* failed - could not be answered from meshmaster.dk at all,
// nor from a report. These cases hold the note that goes with the job's journal line: the step, the
// status of the layer under it, the address and whether the chip was changed, in the seventy
// characters a note holds.
//
// And the other half: an apply and a session whose flash never began end `refused` - the contract's
// own word - with the radio exactly as the job found it. **And with `flash` under it** (task
// p18-06): p18-05 sent no refusal word at all, and meshmaster.dk's door turns a `refused` session
// without one away whole, which is why session 5 of 24 November never reported.
#include <doctest.h>

#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "agent/session_job.h"
#include "core/flasher/flasher.h"
#include "core/update/radio_update.h"
#include "core/update/scanner_session.h"

using bench::AgentBench;
using bench::Candidate;
using bench::has;
using bench::SessionJob;
using core::cycle::Pass;
using core::image::Which;
namespace upd = core::update;

namespace {

using FailAt = sim::Nrf52Chip::FailFlashAt;

// A unit ready for an update: the golden image on the chip and in the store, running, and a
// candidate waiting.
void readyUnit(AgentBench& bench, const Candidate& golden, const Candidate& candidate) {
  REQUIRE(bench.world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
  REQUIRE(bench.world.images.put(Which::kCandidate, bench::Slot{0}, candidate) ==
          bench::StoreError::kNone);
  bench.world.chip->installOnChip(golden);
  bench.world.chip->radioBoots();
  bench.modemWorks();
}

// Every `note` of a result's journal array, in order.
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

std::string notesText(const std::vector<std::string>& notes) {
  std::string text;
  for (const std::string& one : notes) text += one + "\n";
  return text;
}

bool anyNoteHas(const std::vector<std::string>& notes, const char* needle) {
  for (const std::string& one : notes) {
    if (one.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("an apply whose flash wrote nothing: the job's page says the flash did not begin, and the apply still commits") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden, candidate);
  // The scan's reads stalling on a bus the application is using, once: the flash of 23 November.
  bench.world.chip->chip.failFlashAt(FailAt::kScanRead, 1);
  bench.server().queue(
      bench::ownJob(61, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file)))
          .json());

  const Pass& pass = bench.oneWake();
  CHECK(pass.carried == interfaces::Ask::kUpdateRadio);
  const std::string result = bench.lastResult(61);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  INFO(data);
  CHECK(bench::member(data, "outcome") == "committed");

  // **The note**, in the words seventy characters hold: the step, the status of the layer under
  // it, the address and that nothing was written.
  const std::vector<std::string> notes = notesOf(bench::objectOf(data, "journal"));
  INFO(notesText(notes));
  CHECK(anyNoteHas(notes, "scan: swd wait-timeout at 0x026000; nothing written"));
  MESSAGE("the job's lines: " << notesText(notes));
}

TEST_CASE("an apply whose flash never begins: refused with `flash`, and the radio as the job found it") {
  AgentBench bench;
  const Candidate golden("update-golden");
  const Candidate candidate("update-candidate");
  readyUnit(bench, golden, candidate);
  bench.world.chip->chip.failFlashAt(FailAt::kScanRead, 0);  // every flash
  bench.server().queue(
      bench::ownJob(62, agent::kApplyType, bench::applyPayload(bench::sha256Hex(candidate.file)))
          .json());

  (void)bench.oneWake();
  const std::string result = bench.lastResult(62);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  INFO(data);
  // `refused` is one of the contract's four words, and `flash` is the refusal's - the word
  // meshmaster.dk's task 260 adds to `FirmwareRefusals` for exactly this (task p18-06).
  CHECK(bench::member(data, "outcome") == "refused");
  CHECK(bench::member(result, "outcome") == "failed");
  CHECK(bench::member(data, "refusal") == "flash");
  const std::vector<std::string> notes = notesOf(bench::objectOf(data, "journal"));
  INFO(notesText(notes));
  CHECK(anyNoteHas(notes, "scan: swd wait-timeout at 0x026000; nothing written"));
  CHECK(anyNoteHas(notes, "2nd try scan: swd wait-timeout at 0x026000; nothing written"));
  MESSAGE("the journal the server was given: " << bench::objectOf(data, "journal"));

  // **The radio is as the job found it**: the golden image still golden and still running, the
  // candidate still in the store to be offered again, and not a word written to the chip.
  CHECK(bench.world.images.read(Which::kGolden, bench::Slot{0}).present);
  CHECK(std::string(bench.world.images.read(Which::kGolden, bench::Slot{0}).version) ==
        golden.version);
  CHECK(bench.world.images.read(Which::kCandidate, bench::Slot{0}).present);
  CHECK(bench.world.chip->chip.counters().erases == 0);
  CHECK(bench.world.chip->chip.counters().writes == 0);
  CHECK(bench.world.chip->radio_runs);
  MESSAGE("the job's lines: " << notesText(notes));
}

TEST_CASE("a session whose scanner flash never begins: refused, no outage, and the scanner image on no bad list") {
  SessionJob unit;
  unit.world.chip->chip.failFlashAt(FailAt::kScanRead, 0);
  unit.server().queue(
      bench::ownJob(63, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());

  (void)unit.oneWake();
  const std::string result = unit.lastResult(63);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  INFO(data);
  CHECK(bench::member(data, "outcome") == "refused");
  CHECK(bench::member(data, "refusal") == "flash");
  const std::vector<std::string> notes = notesOf(bench::objectOf(data, "journal"));
  INFO(notesText(notes));
  CHECK(anyNoteHas(notes, "scan: swd wait-timeout at 0x026000; nothing written"));
  // The second try's own line, and the ending's own sentence after it (task p18-06).
  CHECK(anyNoteHas(notes, "2nd try scan: swd wait-timeout at 0x026000; nothing written"));
  CHECK(anyNoteHas(notes, "the flash did not begin, twice; the radio is as it was"));

  // Nothing was flashed, the repeater never left the mesh, and the scanner image is on no bad
  // list - so the next session the server queues can run.
  CHECK(unit.world.chip->chip.counters().erases == 0);
  CHECK(unit.world.chip->chip.counters().writes == 0);
  CHECK(unit.world.chip->radio_runs);
  CHECK_FALSE(unit.world.rak.scanner);
  CHECK(unit.world.images.store.badCount() == 0);
  CHECK_FALSE(unit.sessionIsOpen());
  MESSAGE("the session's lines: " << notesText(notes));
}

TEST_CASE("a session whose scanner flash wrote nothing once: the line goes to the server and the session is restored") {
  SessionJob unit;
  unit.world.chip->chip.failFlashAt(FailAt::kScanRead, 1);
  unit.server().queue(
      bench::ownJob(64, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());

  (void)unit.oneWake();
  const std::string result = unit.lastResult(64);
  REQUIRE_FALSE(result.empty());
  const std::string data = bench::objectOf(result, "data");
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  const std::vector<std::string> notes = notesOf(bench::objectOf(data, "journal"));
  INFO(notesText(notes));
  // **The one line of the radio update's own story a session's page would never have carried**:
  // the flash did not begin, and it is being made once more.
  CHECK(anyNoteHas(notes, "scan: swd wait-timeout at 0x026000; nothing written"));
  CHECK(unit.world.images.store.badCount() == 0);
  MESSAGE("the session's lines: " << notesText(notes));
}
