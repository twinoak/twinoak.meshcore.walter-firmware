// A session of the radio as a scanner, on the update bench (task p16-03): the world of
// update_bench.h - the store's flash, the journal's flash, the simulated nRF52840 and the
// scripted radio - with a scanner image in the store, a scripted NoiseScope on the other end of
// the UART, and a sink that keeps what each step handed over.
//
// The scanner's half - a scripted NoiseScope, its `ack`s, its lines and the knobs a session's
// five step endings need - is `host_test/sim/scripted_scanner.h`, where task p16-04's job can
// reach it too.
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
#include "sim/scripted_scanner.h"
#include "update_bench.h"

namespace bench {

using upd::Ending;
using upd::Refusal;
using upd::Stage;
using upd::StepEnd;

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
  std::uint32_t losses = 0;  // times the port lost bytes while this step ran (task p18-03)
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
    taken.losses = report.losses;
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
