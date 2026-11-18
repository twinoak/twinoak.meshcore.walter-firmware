// A session of the radio as a scanner, on the update bench (task p16-03): the world of
// update_bench.h - the store's flash, the journal's flash, the simulated nRF52840 and the
// scripted radio - with a scanner image in the store, a scripted NoiseScope on the other end of
// the UART, and a sink that keeps what each step handed over.
//
// The scanner's half is the repeater repository's own `docs/noisescope.md` §5: **every command is
// answered with an `ack` line**, a finite activity ends with a `done`, and a measurement line is
// one NDJSON object. The knobs here are what a session's five step endings need - no `ack`, an
// `ack` that is not ok, lines without a `done`, more lines than the room holds - and nothing else.
#pragma once

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/update/scanner_session.h"
#include "update_bench.h"

namespace bench {

using upd::Ending;
using upd::Refusal;
using upd::Stage;
using upd::StepEnd;

// The first word of a command line, which is what an `ack` is matched on.
inline std::string firstWordOf(const std::string& line) {
  const std::size_t space = line.find(' ');
  return space == std::string::npos ? line : line.substr(0, space);
}

// NoiseScope's own lines, as §5 writes them.
inline std::string scannerAck(const std::string& command, bool ok, const std::string& message) {
  return "{\"t\":\"ack\",\"cmd\":\"" + command + "\",\"ok\":" + (ok ? "true" : "false") +
         ",\"msg\":\"" + message + "\"}";
}

inline std::string scannerDone(const std::string& command) {
  return "{\"t\":\"done\",\"cmd\":\"" + command + "\",\"seq\":18,\"ts\":1754820100,\"ms\":112000}";
}

// A measurement line of `size` bytes, shaped like a real one: the command's own `t`, a sequence
// number, and an array padded to the length wanted.
inline std::string scannerLine(const std::string& what, unsigned seq, std::size_t size) {
  const std::string head = "{\"t\":\"" + what + "\",\"seq\":" + std::to_string(seq) +
                           ",\"ts\":1754820100,\"f0\":863.000,\"f1\":870.000,\"n\":281,\"avg\":[";
  const std::string tail = "]}";
  REQUIRE(size > head.size() + tail.size());
  return head + std::string(size - head.size() - tail.size(), '7') + tail;
}

inline const char* kScannerInfo =
    "{\"t\":\"info\",\"fw\":\"noisescope\",\"ver\":\"0.1.0\",\"role\":\"scanner\","
    "\"board\":\"rak4630-single\",\"devid\":\"deadbeef01020304\",\"rtc\":1,\"auto_min\":10}";

// What this scanner answers one measuring command with.
struct Answer {
  bool ack = true;         // an `ack` at all. false is the `silent` ending
  bool ok = true;          // ... and it says ok. false is the `refused` ending
  std::size_t lines = 1;   // measurement lines after the `ack`
  std::size_t line_bytes = 300;
  bool done = true;        // a `done` line after them. false leaves the step to its clock
};

// What the scripted scanner answers each command with. One hook on the radio, installed for the
// whole life of a world: the greeting's three words get an `ack` (and `info` its own line), the
// six that measure get an `ack` and then lines, and `stop` gets an `ack`. `answer` is what every
// measuring command gets; `answers["hist"]` is what one of them gets instead.
struct ScriptedScanner {
  Answer answer;
  std::map<std::string, Answer> answers;
  std::uint32_t first_line_ms = 50;
  std::uint32_t between_ms = 50;
  std::vector<std::string> said;  // every command this scanner was given, in order

  const Answer& answerFor(const std::string& word) const {
    const std::map<std::string, Answer>::const_iterator at = answers.find(word);
    return at == answers.end() ? answer : at->second;
  }

  void install(sim::ScriptedRadio& radio) {
    radio.on_text_command = [this, &radio](const std::string& line) {
      said.push_back(line);
      const std::string word = firstWordOf(line);
      const bool measures = word == "stat" || word == "sweep" || word == "scope" ||
                            word == "hist" || word == "dwell" || word == "cad";
      if (!measures) {
        // `time`, `auto`, `info`, `stop`: an `ack`, and for `info` the line §5 promises.
        radio.sendLine(scannerAck(word, true, "ok"));
        if (word == "info") radio.sendLine(kScannerInfo, 10);
        return true;
      }
      const Answer& a = answerFor(word);
      if (!a.ack) return true;  // nothing at all: the step ends `silent`
      radio.sendLine(scannerAck(word, a.ok, a.ok ? "started" : "bad arguments"));
      if (!a.ok) return true;
      std::uint32_t at = first_line_ms;
      for (std::size_t i = 0; i < a.lines; ++i) {
        radio.sendLine(scannerLine(word, static_cast<unsigned>(17 + i), a.line_bytes), at);
        at += between_ms;
      }
      if (a.done) radio.sendLine(scannerDone(word), at);
      return true;
    };
  }

  // What the scanner was told, after the greeting's three words.
  std::vector<std::string> commandsAfterGreeting() const {
    std::vector<std::string> rest;
    for (const std::string& line : said) {
      const std::string word = firstWordOf(line);
      if (word == "time" || word == "auto" || word == "info") continue;
      rest.push_back(line);
    }
    return rest;
  }

  bool wasSaid(const std::string& line) const {
    for (const std::string& seen : said) {
      if (seen == line) return true;
    }
    return false;
  }
};

// What a step handed over, kept. The lines are a view into the session's room and are valid only
// for the length of the call, so they are copied here - which a test may do and the firmware
// may not (S-12: nothing of a scanner's line is written down by this firmware).
struct StepTaken {
  std::size_t index = 0;
  std::string command;
  std::uint32_t seconds = 0;
  StepEnd end = StepEnd::kNone;
  std::size_t used = 0;
  std::size_t count = 0;
  bool first = false;
  std::vector<std::string> lines;
};

// The caller of a session: it keeps every step's lines, and it can be told to sit on them for a
// while before it returns, which is what rail 3's ceiling is about.
struct RecordingSink final : upd::SessionSink {
  std::vector<StepTaken> steps;
  fakes::FakeClock* clock = nullptr;
  std::uint32_t sit_for_ms = 0;  // how long this caller holds each step's lines before returning

  void onStepLines(const upd::StepReport& report) override {
    StepTaken taken;
    taken.index = report.index;
    taken.command = report.command;
    taken.seconds = report.seconds;
    taken.end = report.end;
    taken.used = report.used;
    taken.count = report.count;
    taken.first = report.first;
    for (std::size_t at = 0; at < report.used;) {
      std::size_t end = at;
      while (end < report.used && report.lines[end] != '\n') ++end;
      taken.lines.push_back(std::string(report.lines + at, end - at));
      at = end + 1;
    }
    // Every line is whole: the client reserves the byte for the line ending, so a line laid down
    // in the room is `{` ... `}` and nothing less.
    for (const std::string& line : taken.lines) {
      REQUIRE_FALSE(line.empty());
      const bool begins_with_brace = line.front() == '{';
      CHECK(begins_with_brace);
      const bool ends_with_brace = line.back() == '}';
      CHECK(ends_with_brace);
    }
    steps.push_back(taken);
    if (sit_for_ms != 0 && clock != nullptr) clock->delayMs(sit_for_ms);
  }

  bool has(std::size_t step, const char* needle) const {
    if (step == 0 || step > steps.size()) return false;
    for (const std::string& line : steps[step - 1].lines) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

// A program of one to six steps, built in one line.
inline upd::Program programOf(std::initializer_list<std::pair<const char*, std::uint32_t>> steps) {
  upd::Program program;
  for (const std::pair<const char*, std::uint32_t>& step : steps) {
    REQUIRE(program.count < upd::kStepsMax + 1);
    if (program.count >= upd::kStepsMax) {
      // One past the limit, on purpose: `count` is what `judge()` refuses.
      ++program.count;
      continue;
    }
    std::size_t i = 0;
    for (; step.first[i] != 0 && i < upd::kCommandMax; ++i) {
      program.step[program.count].command[i] = step.first[i];
    }
    program.step[program.count].command[i] = 0;
    program.step[program.count].seconds = step.second;
    ++program.count;
  }
  return program;
}

// A unit as a session finds it: the golden image in the store and on the chip, running as the
// repeater; a candidate; **and a scanner image in the scanner slot**.
struct SessionUnit {
  UpdateWorld world;
  Candidate golden{"update-golden"};
  Candidate candidate{"update-candidate"};
  Candidate scanner{"store-scanner"};
  ScriptedScanner scripted;
  RecordingSink sink;
  std::unique_ptr<Walter> walter;

  // `golden_in_store` false is a unit whose repeater is *running* and whose store has no golden
  // image to come back to: the link answers, and the check refuses `noGolden`.
  explicit SessionUnit(bool with_scanner = true, bool golden_in_store = true) {
    if (golden_in_store) {
      REQUIRE(world.store.put(Which::kGolden, Slot{0}, golden) == StoreError::kNone);
    }
    REQUIRE(world.store.put(Which::kCandidate, Slot{0}, candidate) == StoreError::kNone);
    if (with_scanner) {
      REQUIRE(world.store.put(Which::kScanner, Slot{0}, scanner) == StoreError::kNone);
    }
    world.installOnChip(golden);
    world.radioBoots();
    scripted.install(world.radio);
    sink.clock = &world.clock;
    walter.reset(new Walter(world));
  }

  // The Walter reset: destroyed and built again over the same world, which is what a restart in
  // the middle of a session is. The session's own state goes with it; the journal does not.
  void reboot() {
    walter.reset();
    walter.reset(new Walter(world));
  }

  upd::ScannerSession& session() { return walter->session; }

  // Drives the session to its end, or until the journal stops it. `on_step` sees every answer.
  Stage run(const std::function<void(const upd::SessionStep&)>& on_step = nullptr,
            int max_steps = 200000) {
    int steps = 0;
    while (session().active() && !world.dead && steps++ < max_steps) {
      const upd::SessionStep s = session().step();
      if (s.changed) stages.push_back(s.after);
      if (on_step) on_step(s);
      if (s.stalled) break;
      if (s.waited) world.clock.delayMs(upd::kListenIdleMs);
    }
    CHECK(steps < max_steps);
    return session().stage();
  }

  // The stages this session journalled, in order, as the journal holds them - read back out of
  // flash and not out of the object, because the journal is what a reset leaves behind.
  std::vector<Stage> journalledStages() const {
    std::vector<Stage> out;
    upd::Cursor cursor;
    upd::Entry entry;
    bool more = walter->journal.first(&cursor, &entry);
    while (more) {
      upd::SessionPayload p;
      if (entry.kind == upd::kind::kScannerSession && upd::decode(entry, &p)) {
        out.insert(out.begin(), p.stage);
      }
      more = walter->journal.previous(&cursor, &entry);
    }
    return out;
  }

  std::string stageText() const {
    std::string text;
    for (Stage stage : stages) {
      text += std::string(upd::name(stage)) + " ";
    }
    return text;
  }

  // The newest session entry of one stage, if the journal holds one.
  bool entryOf(Stage stage, upd::SessionPayload* out) const {
    upd::Cursor cursor;
    upd::Entry entry;
    bool more = walter->journal.first(&cursor, &entry);
    while (more) {
      upd::SessionPayload p;
      if (entry.kind == upd::kind::kScannerSession && upd::decode(entry, &p) && p.stage == stage) {
        if (out != nullptr) *out = p;
        return true;
      }
      more = walter->journal.previous(&cursor, &entry);
    }
    return false;
  }

  // A request of the radio's own update machine, for the tests that drive it beside a session -
  // `rak role scanner` by hand, an update in progress.
  upd::Request request(Kind kind = Kind::kUpdate) const {
    upd::Request r;
    r.kind = kind;
    r.slot = Slot{0};
    return r;
  }

  Record read(Which which) const { return world.store.read(which, Slot{0}); }
  std::string goldenVersion() const {
    const Record r = read(Which::kGolden);
    return r.present ? std::string(r.version) : std::string("(none)");
  }

  std::vector<Stage> stages;
};

}  // namespace bench
