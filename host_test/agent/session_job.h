// A session of the radio as a scanner **as a job**, on the wake bench: the fixture task p16-04's
// cases were written on, moved here by task p18-03 so that its own cases run on the same unit and
// against the same server.
//
// What it is: `bench::AgentBench` with a simulated nRF52840 behind the RAK, the golden image in
// the store and on the chip, a scanner image in the scanner slot, a scripted NoiseScope on the
// UART (`sim/scripted_scanner.h`, which makes the port a wire of §1's baud rate with the Walter's
// receive ring behind it), the real session machine of task p16-03, the real flasher and the
// scripted meshmaster.dk.
//
// **And one rule held at every call** (task p18-03): from the moment a step's command is said
// until the step has ended, nothing of this end may reach the server. The constructor installs a
// hook on the scripted server that fails any test in which a request arrives while the machine
// says it is listening to a step - so every case that runs a step holds the rule, whether it was
// written for it or not, and a case that wants to see the old behaviour has to take the hook off.
#pragma once

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "core/flasher/flasher.h"
#include "core/update/scanner_session.h"
#include "sim/scripted_scanner.h"
#include "walter_agent/firmware_jobs.h"

namespace bench {

namespace sj_upd = core::update;

inline bool has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// The `step` of every entry of a journal array, in order.
inline std::vector<std::string> stepsOf(const std::string& journal) {
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

// One object of a JSON array, by index, braces and all.
inline std::string objectAt(const std::string& array, std::size_t index) {
  std::size_t at = 0;
  for (std::size_t i = 0; i <= index; ++i) {
    at = array.find('{', at);
    if (at == std::string::npos) return "";
    if (i == index) break;
    int depth = 0;
    for (std::size_t k = at; k < array.size(); ++k) {
      if (array[k] == '{') ++depth;
      if (array[k] == '}' && --depth == 0) {
        at = k + 1;
        break;
      }
    }
  }
  return bench::objectOf("{\"x\":" + array.substr(at), "x");
}

// The lines of one post's body, which is NDJSON and not JSON.
inline std::vector<std::string> bodyLines(const std::string& body) {
  std::vector<std::string> lines;
  std::size_t at = 0;
  while (at < body.size()) {
    const std::size_t end = body.find('\n', at);
    if (end == std::string::npos) {
      lines.push_back(body.substr(at));
      break;
    }
    lines.push_back(body.substr(at, end - at));
    at = end + 1;
  }
  return lines;
}

// The flasher's progress sink with a stopwatch in it, as task p14-04's own test has it: at the
// first page of every flash's scan it spends `takes_ms` of the world's clock, and it keeps when
// each flash began. The simulated chip's NVMC timings are microseconds a word, so without this a
// flash in a test takes no time at all and the lease's five-minute timer never fires.
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

// A unit a session can be run on: a chip for the radio to run from, the golden image in the store
// and on the chip, a scanner image in the scanner slot, a scripted NoiseScope on the UART and the
// server reachable.
struct SessionJob : AgentBench {
  Candidate golden{"update-golden"};
  Candidate scanner{"store-scanner"};
  ScriptedScanner scripted;
  // Every call the server took while the machine said it was listening to a step. Nought is the
  // rule of task p18-03's first deliverable, and the hook below is what makes every case check it.
  std::vector<std::string> calls_in_a_step;

  explicit SessionJob(bool with_scanner = true, bool golden_in_store = true) : AgentBench() {
    if (golden_in_store) {
      REQUIRE(world.images.put(core::image::Which::kGolden, bench::Slot{0}, golden) ==
              bench::StoreError::kNone);
    }
    if (with_scanner) {
      REQUIRE(world.images.put(core::image::Which::kScanner, bench::Slot{0}, scanner) ==
              bench::StoreError::kNone);
    }
    world.chip->installOnChip(golden);
    world.chip->radioBoots();
    scripted.install(world.rak);
    modemWorks();
    watchForCallsInAStep();
  }

  // **The rule, at every call of the API.** The machine itself is asked - `listening()` is true
  // from the moment a step's command was said until the step has ended - so this holds the thing
  // the task asks for and not a guess at where the calls would be.
  void watchForCallsInAStep() {
    server().on_call = [this](const std::string& path) {
      if (walter == nullptr || !walter->session.listening()) return;
      calls_in_a_step.push_back(path);
      CHECK_MESSAGE(false, "a call to the server between a step's command and its end: " << path);
    };
  }

  // The chip holding the scanner image and the radio answering as one, which is where a session
  // cut off in its steps left the radio.
  void theRadioIsAScanner() {
    world.chip->installOnChip(scanner);
    world.chip->radioBoots();
    REQUIRE(world.rak.scanner);
  }

  // A session left open in the journal at `stage`, as a reset leaves it (the session object of
  // that life is gone and the entry is all there is), and the job that owes its result.
  void anOpenSessionOwedBy(long long job_id, sj_upd::Stage stage, std::uint8_t steps) {
    aSessionOwedBy(job_id, stage, steps, true);
  }

  // The same for a stage that **closes** a session (task p18-06): `refused` is one, and a result
  // owed by a session that closed is exactly the case a restart between the ending and the post
  // leaves behind - the journal says what happened and the entry says who is owed.
  void aSessionOwedBy(long long job_id, sj_upd::Stage stage, std::uint8_t steps,
                      bool expect_open = false) {
    sj_upd::SessionPayload p;
    p.stage = stage;
    p.step = stage == sj_upd::Stage::kStep ? 1 : 0;
    p.slot = 0;
    p.at_epoch = 1760000000;
    // A session's own first entry, so that what is posted afterwards has a story in it.
    sj_upd::SessionPayload opened = p;
    opened.stage = sj_upd::Stage::kCheck;
    std::uint8_t bytes[sj_upd::kPayloadMax];
    REQUIRE(walter->journal.append(sj_upd::kind::kScannerSession, bytes,
                                   sj_upd::encode(opened, bytes)) == sj_upd::JournalError::kNone);
    if (stage == sj_upd::Stage::kStep) {
      const char* command = "dwell 869.618 60";
      std::size_t i = 0;
      for (; command[i] != 0 && i < sj_upd::kCommandMax; ++i) p.command[i] = command[i];
      p.command[i] = 0;
    }
    REQUIRE(walter->journal.append(sj_upd::kind::kScannerSession, bytes,
                                   sj_upd::encode(p, bytes)) == sj_upd::JournalError::kNone);
    sj_upd::Mark mark;
    REQUIRE(sj_upd::readMark(walter->journal, &mark));
    REQUIRE(mark.found);
    REQUIRE(mark.stage == stage);
    if (expect_open) REQUIRE(mark.open);
    // And the entry the result is owed by, which a job writes before the scanner goes on.
    agent::JobEntry owed;
    owed.job_id = job_id;
    owed.state = agent::JobEntryState::kOwed;
    owed.product = agent::kProductForSession;
    owed.slot = 0;
    owed.at_epoch = 1760000000;
    owed.steps = steps;
    const char* version = "0.1.6";
    for (std::size_t i = 0; i < sj_upd::kShortVersion && version[i] != 0; ++i) {
      owed.to_version[i] = version[i];
    }
    std::uint8_t job_bytes[agent::kJobEntryBytes];
    agent::encode(owed, job_bytes);
    REQUIRE(walter->journal.append(sj_upd::kind::kAgentJob, job_bytes, agent::kJobEntryBytes) ==
            sj_upd::JournalError::kNone);
    REQUIRE(half().firmware.resultOwed());
  }

  bool sessionIsOpen() {
    sj_upd::Mark mark;
    return sj_upd::readMark(walter->journal, &mark) && mark.open;
  }

  // The posts of a scanner's lines for one job, in order.
  const std::vector<bench::SeenLines>& posts(long long job_id) { return server().lines[job_id]; }
};

// The result's **own** `lines` and `linesUnanswered`, which come after the `steps` array - whose
// rows carry a `lines` of their own, and `bench::member` has no notion of depth.
inline std::string topMember(const std::string& data, const std::string& key) {
  const std::string steps = bench::objectOf(data, "steps");
  const std::size_t at = data.find(steps);
  if (at == std::string::npos) return bench::member(data, key);
  return bench::member(data.substr(at + steps.size()), key);
}

// What the result's `steps` array says, as one string: `clock/7/1810 done/3/4312`.
inline std::string stepRows(const std::string& data) {
  const std::string steps = bench::objectOf(data, "steps");
  std::string text;
  for (std::size_t i = 0; i < sj_upd::kStepsMax; ++i) {
    const std::string one = objectAt(steps, i);
    if (one.empty()) break;
    text += bench::member(one, "ended") + "/" + bench::member(one, "lines") + "/" +
            bench::member(one, "bytes") + " ";
  }
  return text;
}

}  // namespace bench
