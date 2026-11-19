// A session is a job (task p16-04): `twinoak.noisescope.session` on the wake bench, with a
// simulated nRF52840 behind the RAK, a scripted NoiseScope on the UART, the real session machine
// of task p16-03, the real flasher and the scripted meshmaster.dk.
//
// What these cases are about is the three things a mast needs and a cable does not:
//
//   the lines     a step's lines go into a post and nowhere else (S-12), in bodies of at most
//                 8 192 bytes cut at a line's end, with `{from}` counting what has been handed
//                 over. A post nothing answers is sent once more and then counted as unanswered:
//                 the radio is never held for a server.
//   the lease     two flashes of a quarter of an hour, with the server's whole ceiling asked for
//                 before each of them and 900 seconds every five minutes in between.
//   the result    `restored`, `refused` or `stuck`, with a row for every step of the payload -
//                 **and a result that outlives a restart**, posted from the journal by the wake
//                 that ends the session nobody was running.
#include <doctest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent_bench.h"
#include "core/flasher/flasher.h"
#include "core/update/scanner_session.h"
#include "sim/scripted_scanner.h"
#include "walter_agent/firmware_jobs.h"

using bench::AgentBench;
using bench::Candidate;
using core::cycle::Pass;
using core::image::Which;
namespace upd = core::update;

namespace {

bool has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// The five lines of the task's own *The contract*, read at run time: every line of
// `tasks/phase-16/04-a-session-is-a-job.md` that begins a JSON object, in the order it appears.
// The fixtures in `host_test/fixtures/contracts-ahead/` are **those lines** until meshmaster.dk's
// task 236 makes the server's own files (that directory's README).
std::vector<std::string> taskContractLines() {
  const std::string path =
      std::string(TWINOAK_REPO_ROOT) + "/tasks/phase-16/04-a-session-is-a-job.md";
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read " << path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("{\"", 0) == 0) lines.push_back(line);
  }
  return lines;
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

// One object of a JSON array, by index, braces and all.
std::string objectAt(const std::string& array, std::size_t index) {
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
std::vector<std::string> bodyLines(const std::string& body) {
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
  bench::ScriptedScanner scripted;

  explicit SessionJob(bool with_scanner = true, bool golden_in_store = true) : AgentBench() {
    if (golden_in_store) {
      REQUIRE(world.images.put(Which::kGolden, bench::Slot{0}, golden) == bench::StoreError::kNone);
    }
    if (with_scanner) {
      REQUIRE(world.images.put(Which::kScanner, bench::Slot{0}, scanner) == bench::StoreError::kNone);
    }
    world.chip->installOnChip(golden);
    world.chip->radioBoots();
    scripted.install(world.rak);
    modemWorks();
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
  void anOpenSessionOwedBy(long long job_id, upd::Stage stage, std::uint8_t steps) {
    upd::SessionPayload p;
    p.stage = stage;
    p.step = stage == upd::Stage::kStep ? 1 : 0;
    p.slot = 0;
    p.at_epoch = 1760000000;
    // A session's own first entry, so that what is posted afterwards has a story in it.
    upd::SessionPayload opened = p;
    opened.stage = upd::Stage::kCheck;
    std::uint8_t bytes[upd::kPayloadMax];
    REQUIRE(walter->journal.append(upd::kind::kScannerSession, bytes,
                                   upd::encode(opened, bytes)) == upd::JournalError::kNone);
    if (stage == upd::Stage::kStep) {
      const char* command = "dwell 869.618 60";
      std::size_t i = 0;
      for (; command[i] != 0 && i < upd::kCommandMax; ++i) p.command[i] = command[i];
      p.command[i] = 0;
    }
    REQUIRE(walter->journal.append(upd::kind::kScannerSession, bytes, upd::encode(p, bytes)) ==
            upd::JournalError::kNone);
    upd::Mark mark;
    REQUIRE(upd::readMark(walter->journal, &mark));
    REQUIRE(mark.open);
    // And the entry the result is owed by, which a job writes before the scanner goes on.
    agent::JobEntry owed;
    owed.job_id = job_id;
    owed.state = agent::JobEntryState::kOwed;
    owed.product = agent::kProductForSession;
    owed.slot = 0;
    owed.at_epoch = 1760000000;
    owed.steps = steps;
    const char* version = "0.1.6";
    for (std::size_t i = 0; i < upd::kShortVersion && version[i] != 0; ++i) {
      owed.to_version[i] = version[i];
    }
    std::uint8_t job_bytes[agent::kJobEntryBytes];
    agent::encode(owed, job_bytes);
    REQUIRE(walter->journal.append(upd::kind::kAgentJob, job_bytes, agent::kJobEntryBytes) ==
            upd::JournalError::kNone);
    REQUIRE(half().firmware.resultOwed());
  }

  bool sessionIsOpen() {
    upd::Mark mark;
    return upd::readMark(walter->journal, &mark) && mark.open;
  }

  // The posts of a scanner's lines for one job, in order.
  const std::vector<bench::SeenLines>& posts(long long job_id) { return server().lines[job_id]; }
};

// The result's **own** `lines` and `linesUnanswered`, which come after the `steps` array - whose
// rows carry a `lines` of their own, and `bench::member` has no notion of depth.
std::string topMember(const std::string& data, const std::string& key) {
  const std::string steps = bench::objectOf(data, "steps");
  const std::size_t at = data.find(steps);
  if (at == std::string::npos) return bench::member(data, key);
  return bench::member(data.substr(at + steps.size()), key);
}

// What the result's `steps` array says, as one string: `clock/7/1810 done/3/4312`.
std::string stepRows(const std::string& data) {
  const std::string steps = bench::objectOf(data, "steps");
  std::string text;
  for (std::size_t i = 0; i < upd::kStepsMax; ++i) {
    const std::string one = objectAt(steps, i);
    if (one.empty()) break;
    text += bench::member(one, "ended") + "/" + bench::member(one, "lines") + "/" +
            bench::member(one, "bytes") + " ";
  }
  return text;
}

}  // namespace

// ------------------------------------------------------------------ the contract, byte for byte

TEST_CASE("the five fixtures of a session are the task's own five lines") {
  const std::vector<std::string> lines = taskContractLines();
  REQUIRE(lines.size() == 5);
  const char* const names[5] = {"NoiseScopeSessionPayload", "NoiseScopeSessionStep",
                                "NoiseScopeLinesReply", "NoiseScopeSessionResult",
                                "NoiseScopeStepResult"};
  for (std::size_t i = 0; i < 5; ++i) {
    CAPTURE(names[i]);
    CHECK(aheadFixture(names[i]) == lines[i]);
  }
  // And the payload the contract bounds at 384 bytes fits the bytes a claim keeps, which is the
  // same figure (walter_agent/modem_uplink.h, kExtensionPayloadBytes).
  CHECK(aheadFixture("NoiseScopeSessionPayload").size() < agent::kExtensionPayloadBytes);
}

TEST_CASE("the payload fixture is read into the six steps, and one of them is the step fixture") {
  const std::string payload = aheadFixture("NoiseScopeSessionPayload");
  upd::Program program;
  long long slot = -1;
  const char* why = "nothing";
  REQUIRE_MESSAGE(agent::readSessionPayload(payload.c_str(), payload.size(), &program, &slot, &why),
                  why);
  CHECK(slot == 0);
  REQUIRE(program.count == 6);
  const char* const commands[6] = {"stat", "sweep", "hist 869.4 869.8 25", "dwell 869.618 60",
                                   "scope 863 870 250 0 1000 max", "cad"};
  const std::uint32_t seconds[6] = {30, 30, 40, 70, 20, 20};
  for (std::size_t i = 0; i < 6; ++i) {
    CAPTURE(i);
    CHECK(std::string(program.step[i].command) == commands[i]);
    CHECK(program.step[i].seconds == seconds[i]);
  }
  // The program the machine of task p16-03 would run: its own pass over it says so.
  CHECK(upd::whyProgram(program) == nullptr);

  // The step fixture is the third of them, member for member.
  const std::string step = aheadFixture("NoiseScopeSessionStep");
  CHECK(bench::member(step, "command") == "hist 869.4 869.8 25");
  CHECK(bench::member(step, "seconds") == "40");
}

TEST_CASE("a result written from the fixture's values is the result fixture, byte for byte") {
  // The journal the fixture carries, line for line: the epochs are its own timestamps.
  const std::uint32_t at = 1794996000;  // 2026-11-18T10:00:00Z
  struct Written {
    std::uint32_t epoch;
    const char* step;
    const char* note;
  };
  const Written written[8] = {
      {at, "check", "battery 4012 mV, link up, clock set"},
      {at + 1, "flashing-scanner", "0.1.6+fcf52d5, 414208 bytes"},
      {at + 902, "scanner", "a scanner answered"},
      {at + 904, "step-1", "stat"},
      {at + 936, "step-2", "sweep"},
      {at + 970, "flashing-golden", "0.1.6+fcf52d5"},
      {at + 1874, "proving", ""},
      {at + 2060, "restored", "0.1.6+fcf52d5 proved"},
  };
  agent::JobJournalLine journal[8];
  for (std::size_t i = 0; i < 8; ++i) {
    journal[i].at_epoch = written[i].epoch;
    journal[i].step = written[i].step;
    for (std::size_t k = 0; written[i].note[k] != 0; ++k) journal[i].note[k] = written[i].note[k];
  }

  agent::SessionResult result;
  result.outcome = agent::SessionOutcome::kRestored;
  result.scanner = "0.1.6+fcf52d5";
  result.steps = 3;
  result.step[0] = agent::SessionStepResult{agent::endWord(upd::StepEnd::kClock), 7, 1810};
  result.step[1] = agent::SessionStepResult{agent::endWord(upd::StepEnd::kDone), 3, 4312};
  result.step[2] = agent::SessionStepResult{agent::endWord(upd::StepEnd::kNone), 0, 0};
  result.lines = 10;
  result.lines_unanswered = 0;
  result.refusal = upd::word(upd::Refusal::kBattery);

  char out[agent::kDataBytes];
  const std::size_t n = agent::writeSessionResult(result, journal, 8, out, sizeof out);
  REQUIRE(n != 0);
  CHECK(std::string(out, n) == aheadFixture("NoiseScopeSessionResult"));
  // And one row of it is the step fixture's shape, key for key.
  const std::string rows = bench::objectOf(std::string(out, n), "steps");
  CHECK(bench::topKeys(objectAt(rows, 0)) == bench::topKeys(aheadFixture("NoiseScopeStepResult")));
}

TEST_CASE("the heartbeat advertises twinoak.noisescope.session as the sixth extension type") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  (void)bench.v().runLine("agent wake");
  REQUIRE_FALSE(bench.server().heartbeats.empty());
  const std::string body = bench.server().heartbeats.back();
  INFO(body);
  CHECK(has(body, std::string("\"") + agent::kSessionType + "\""));
  CHECK(bench.half().firmware.typeCount() == 6);
  CHECK(std::string(bench.half().firmware.type(5)) == agent::kSessionType);
  // The server routes by what the heartbeat advertised, so a session can be claimed at all.
  bool offered = false;
  for (const std::string& one : bench.server().advertised()) offered = offered || one == agent::kSessionType;
  CHECK(offered);
}

// ------------------------------------------------------------------ a session, end to end

TEST_CASE("a session of two steps: the lines posted step by step, and the result's counts") {
  SessionJob unit;
  bench::Answer three;
  three.lines = 3;
  three.line_bytes = 200;
  bench::Answer twenty_one;
  twenty_one.lines = 21;
  twenty_one.line_bytes = 200;
  unit.scripted.answers["stat"] = three;
  unit.scripted.answers["sweep"] = twenty_one;
  unit.server().queue(bench::ownJob(41, agent::kSessionType,
                                    bench::sessionPayload({{"stat", 30}, {"sweep", 30}}))
                          .json());

  const Pass& pass = unit.oneWake();
  INFO(unit.v().out.text);
  CHECK(pass.carried == interfaces::Ask::kUpdateRadio);
  CHECK(pass.ending == core::cycle::Ending::kSlept);

  // **The posts**: `{from}` 0 and then 3 - the greeting's three `ack`s and its `info` line are
  // the first step's, so the first post carries more than the three `stat` lines.
  const std::vector<bench::SeenLines>& posts = unit.posts(41);
  REQUIRE(posts.size() == 2);
  CHECK(posts[0].from == 0);
  const std::vector<std::string> first = bodyLines(posts[0].body);
  CHECK(posts[1].from == first.size());
  // Every line whole and in order, each ended with a newline, and nothing but what the scanner
  // said: the body is NDJSON and not JSON.
  for (const bench::SeenLines& post : posts) {
    CAPTURE(post.from);
    CHECK(post.body.back() == '\n');
    CHECK(post.body.size() <= agent::kLinesPostBytes);
    for (const std::string& line : bodyLines(post.body)) {
      REQUIRE_FALSE(line.empty());
      CHECK(line.front() == '{');
      CHECK(line.back() == '}');
    }
  }
  // The 21 measurement lines, the command's own `ack` and its `done`.
  const std::vector<std::string> second = bodyLines(posts[1].body);
  CHECK(second.size() == 23);
  CHECK(has(posts[0].body, "\"t\":\"info\""));
  CHECK(has(posts[1].body, "\"t\":\"sweep\""));

  // **The result**: restored, a row for each step, the two counts, and the scanner's version.
  const std::string data = unit.lastData(41);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(bench::member(unit.lastResult(41), "outcome") == "ok");
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(bench::member(data, "scanner") == std::string(unit.scanner.version));
  const std::size_t handed = first.size() + second.size();
  CHECK(topMember(data, "lines") == std::to_string(handed));
  CHECK(topMember(data, "linesUnanswered") == "0");
  INFO(stepRows(data));
  CHECK(stepRows(data) == "done/" + std::to_string(first.size()) + "/" +
                              std::to_string(posts[0].body.size()) + " done/" +
                              std::to_string(second.size()) + "/" +
                              std::to_string(posts[1].body.size()) + " ");
  // The repeater is back on the chip and proved, and the journal says the session is closed.
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK_FALSE(unit.world.rak.scanner);
  CHECK_FALSE(unit.sessionIsOpen());
  CHECK(unit.v().scratch.out == 0);  // the room was given back
  CHECK(unit.half().firmware.counters().sessions == 1);
  CHECK(unit.half().firmware.counters().lines_posted == handed);  // the counter and the result agree
  CHECK(unit.half().firmware.counters().lines_unanswered == 0);

  // **The journal as progress**, one post an entry, and the last one's note is `restored`.
  const std::vector<std::string>& reports = unit.server().progress[41];
  const std::string journal = bench::objectOf(data, "journal");
  const std::vector<std::string> steps = stepsOf(journal);
  INFO(journal);
  CHECK(steps.size() == reports.size());
  CHECK(steps == std::vector<std::string>{"check", "flashing-scanner", "scanner", "step-1",
                                           "step-2", "room", "flashing-golden", "booting",
                                           "proving", "restored"});
  for (std::size_t i = 0; i < reports.size(); ++i) {
    CAPTURE(i);
    const std::vector<std::string> so_far =
        stepsOf(bench::objectOf(bench::objectOf(reports[i], "data"), "journal"));
    CHECK(so_far.size() == i + 1);
    CHECK(so_far.back() == steps[i]);
  }
  CHECK(has(reports.back(), "\"note\":\"restored\""));
  // The step entries carry the commands, and **the room line is in the journal the server got**:
  // the heap's five figures reach the server with every session.
  CHECK(has(journal, "\"step\":\"step-1\",\"note\":\"stat\""));
  CHECK(has(journal, "\"step\":\"step-2\",\"note\":\"sweep\""));
  CHECK(has(journal, "\"step\":\"room\",\"note\":\"8192 of "));
  // And so does the last progress post before the switch back, which is the `room` one.
  bool room_posted = false;
  for (const std::string& report : reports) {
    if (has(report, "\"note\":\"room\"")) {
      room_posted = true;
      CHECK(has(report, "\"step\":\"room\",\"note\":\"8192 of "));
    }
  }
  CHECK(room_posted);
}

TEST_CASE("a step whose lines fill the room: one post of at most 8 192 bytes, every line whole") {
  SessionJob unit;
  bench::Answer flood;
  flood.lines = 20;
  flood.line_bytes = 1000;  // 20 kB into a room of 8 192 (task p16-03's own `full` case)
  flood.done = false;
  unit.scripted.answers["hist"] = flood;
  unit.server().queue(
      bench::ownJob(42, agent::kSessionType, bench::sessionPayload({{"hist 869.4 869.8 25", 60}}))
          .json());

  (void)unit.oneWake();
  const std::vector<bench::SeenLines>& posts = unit.posts(42);
  REQUIRE(posts.size() == 1);
  CHECK(posts[0].from == 0);
  CHECK(posts[0].body.size() <= agent::kLinesPostBytes);
  // Within one line of the bound: the body is cut at a line's end and not at a byte.
  CHECK(posts[0].body.size() > agent::kLinesPostBytes - 1100);
  std::size_t thousands = 0;
  for (const std::string& line : bodyLines(posts[0].body)) {
    CHECK(line.front() == '{');
    CHECK(line.back() == '}');
    thousands += line.size() >= 1000 ? 1 : 0;
  }
  CHECK(thousands >= 5);
  const std::string data = unit.lastData(42);
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(has(stepRows(data), "full/"));
}

// And the same step on a board that has the PSRAM (task p16-05): the room is 64 kB, so a minute
// of `scope` is kept whole and goes out as four posts instead of ending `full` after one.
TEST_CASE("a step of 30 kB of lines in the big room: four posts, each cut between lines") {
  SessionJob unit;
  bench::Answer scope;
  scope.lines = 60;
  scope.line_bytes = 480;  // 60 frames of 480 bytes: about 29 kB, a minute of `scope`
  scope.done = false;
  unit.scripted.answers["scope"] = scope;
  unit.server().queue(bench::ownJob(44, agent::kSessionType,
                                    bench::sessionPayload({{"scope 863 870 250 0 1000", 70}}))
                          .json());
  // The board, after the boot and before the wake: the seam is the life's, and the PSRAM is what
  // this case is about.
  unit.boot();
  unit.v().scratch.externalOf(2u * 1024u * 1024u);
  unit.v().begin();
  (void)unit.v().run();

  CHECK(unit.v().scratch.external_last_asked == upd::kStepBytesExternal);
  const std::vector<bench::SeenLines>& posts = unit.posts(44);
  REQUIRE(posts.size() == 4);
  std::size_t counted = 0;
  std::size_t frames = 0;
  for (const bench::SeenLines& post : posts) {
    CAPTURE(post.from);
    // `{from}` is the lines handed to posts before this one, and nothing else keeps the two ends
    // in step (task p16-04).
    CHECK(post.from == counted);
    CHECK(post.body.size() <= agent::kLinesPostBytes);
    // Cut between lines and not at a byte: the body ends with a newline and every line in it is
    // whole, and the cut is within one line of the bound.
    REQUIRE_FALSE(post.body.empty());
    const bool ends_with_a_newline = post.body.back() == '\n';
    CHECK(ends_with_a_newline);
    std::vector<std::string> lines = bodyLines(post.body);
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    for (const std::string& line : lines) {
      CHECK(line.front() == '{');
      CHECK(line.back() == '}');
      frames += line.size() == scope.line_bytes ? 1 : 0;
    }
    counted += lines.size();
  }
  for (std::size_t i = 0; i + 1 < posts.size(); ++i) {
    CAPTURE(i);
    // Every post but the last is full to within one line of the bound.
    CHECK(posts[i].body.size() > agent::kLinesPostBytes - (scope.line_bytes + 1));
  }
  // All sixty frames went out whole, which is the thing the small room could not do.
  CHECK(frames == scope.lines);
  const std::string data = unit.lastData(44);
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(has(stepRows(data), "clock/"));
  // And the `room` line in the result's journal says where the room came from.
  CHECK(data.find("65536 psram of 2097152") != std::string::npos);
}

TEST_CASE("a post whose answer never comes is sent once more, and its lines are unanswered") {
  SessionJob unit;
  bench::Answer four;
  four.lines = 4;
  four.line_bytes = 200;
  unit.scripted.answer = four;
  // The first step's post is taken and never answered, twice; the second step's goes.
  unit.server().silenceNextLines(2);
  unit.server().queue(bench::ownJob(43, agent::kSessionType,
                                    bench::sessionPayload({{"stat", 20}, {"cad", 20}}))
                          .json());

  (void)unit.oneWake();
  const std::vector<bench::SeenLines>& posts = unit.posts(43);
  REQUIRE(posts.size() == 3);
  // Twice with the same `{from}`, and then the next lines with the next one.
  CHECK(posts[0].from == 0);
  CHECK(posts[1].from == 0);
  CHECK(posts[1].body == posts[0].body);
  const std::size_t lost = bodyLines(posts[0].body).size();
  CHECK(posts[2].from == lost);
  const std::string data = unit.lastData(43);
  INFO(data);
  CHECK(topMember(data, "linesUnanswered") == std::to_string(lost));
  CHECK(topMember(data, "lines") == std::to_string(lost + bodyLines(posts[2].body).size()));
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(unit.half().firmware.counters().lines_unanswered == lost);
}

TEST_CASE("a reply that says full: no further post, the steps still run, the result is restored") {
  SessionJob unit;
  bench::Answer two;
  two.lines = 2;
  two.line_bytes = 200;
  unit.scripted.answer = two;
  unit.server().roomForLines(1);  // the first post fills the job's room
  unit.server().queue(bench::ownJob(44, agent::kSessionType,
                                    bench::sessionPayload({{"stat", 20}, {"cad", 20}, {"sweep", 20}}))
                          .json());

  (void)unit.oneWake();
  CHECK(unit.posts(44).size() == 1);
  const std::string data = unit.lastData(44);
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  // The three steps all ran - the scanner was told all three commands - and the two after the
  // room was used up handed nothing to a post.
  CHECK(stepRows(data) == "done/" + std::to_string(bodyLines(unit.posts(44)[0].body).size()) + "/" +
                              std::to_string(unit.posts(44)[0].body.size()) +
                              " done/0/0 done/0/0 ");
  const std::vector<std::string> said = unit.scripted.commandsAfterGreeting();
  REQUIRE(said.size() >= 3);
  CHECK(said[0] == "stat");
  CHECK(said[1] == "cad");
  CHECK(said[2] == "sweep");
  CHECK_FALSE(unit.sessionIsOpen());
}

TEST_CASE("no uplink from the second step on: the steps run, the repeater comes back, the result follows") {
  SessionJob unit;
  bench::Answer two;
  two.lines = 2;
  two.line_bytes = 200;
  unit.scripted.answer = two;
  // The server goes away the moment the second command is said, and stays away for the rest of
  // the wake: every post from there on has nothing to answer it.
  unit.scripted.on_command = [&unit](const std::string& line) {
    if (line == "cad") unit.server().away(true);
  };
  unit.server().queue(bench::ownJob(45, agent::kSessionType,
                                    bench::sessionPayload({{"stat", 20}, {"cad", 20}}))
                          .json());

  (void)unit.oneWake();
  // The radio was never held for the server: both steps ran and the golden image is back.
  const std::vector<std::string> said = unit.scripted.commandsAfterGreeting();
  REQUIRE(said.size() >= 2);
  CHECK(said[1] == "cad");
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK_FALSE(unit.world.rak.scanner);
  CHECK_FALSE(unit.sessionIsOpen());
  // Nothing of the result reached the server in that wake.
  CHECK(unit.lastResult(45).empty());
  CHECK(unit.half().firmware.counters().lines_unanswered > 0);

  // The next wake that reaches the server carries it: the library kept the result the wake made.
  unit.server().away(false);
  (void)unit.oneWake();
  const std::string data = unit.lastData(45);
  INFO(data);
  REQUIRE_FALSE(data.empty());
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(topMember(data, "linesUnanswered") != "0");
}

TEST_CASE("the lease: the server's ceiling before each flash, and 900 seconds in between") {
  SessionJob unit;
  unit.server().queue(
      bench::ownJob(46, agent::kSessionType, bench::sessionPayload({{"sweep", 30}})).json());
  // The measured flash, as task p14-04's test spends it: 15 min 30 s of the world's clock at the
  // first page of each of the two.
  SlowFlash slow(unit.world.clock, 930000);
  unit.boot();
  unit.v().update.setProgress(&slow);
  unit.v().begin();
  (void)unit.v().run();
  REQUIRE(slow.flashes() == 2);

  const std::vector<bench::SeenExtend>& extends = unit.server().extensions[46];
  REQUIRE(extends.size() >= 3);
  for (const bench::SeenExtend& one : extends) {
    CAPTURE(one.seconds);
    CHECK(one.seconds <= bench::kMaxExtendSeconds);
    CHECK_FALSE(one.refused);
    const bool one_of_the_two = one.seconds == agent::kLeaseFlashSeconds ||
                                one.seconds == agent::kLeaseExtendSeconds;
    CHECK(one_of_the_two);
  }
  // The one immediately before each flash asked for the whole of what the server allows.
  for (std::size_t flash = 0; flash < 2; ++flash) {
    CAPTURE(flash);
    bench::SeenExtend before;
    bool found = false;
    for (const bench::SeenExtend& one : extends) {
      if (one.at_ms <= slow.began_ms[flash]) {
        before = one;
        found = true;
      }
    }
    REQUIRE(found);
    CHECK(before.seconds == agent::kLeaseFlashSeconds);
    CHECK(slow.began_ms[flash] - before.at_ms < 60000);
  }
  // And one between the two flashes asked for the ordinary 900: the steps and the proving period
  // are longer than kExtendEveryMs.
  bool nine_hundred = false;
  for (const bench::SeenExtend& one : extends) {
    nine_hundred = nine_hundred ||
                   (one.seconds == agent::kLeaseExtendSeconds && one.at_ms > slow.ended_ms[0]);
  }
  CHECK(nine_hundred);
  // The result still came in inside the lease, which is what the whole arrangement is for.
  const std::vector<bench::PostedResult>& posted = unit.server().results[46];
  REQUIRE_FALSE(posted.empty());
  CHECK_FALSE(posted.back().late);
  CHECK(bench::member(unit.lastData(46), "outcome") == "restored");
}

// ------------------------------------------------------------------ the refusals

TEST_CASE("a session refused: the word, the check line, and the radio untouched") {
  SUBCASE("seven steps") {
    SessionJob unit;
    unit.server().queue(bench::ownJob(47, agent::kSessionType,
                                      bench::sessionPayload({{"stat", 10},
                                                             {"stat", 10},
                                                             {"stat", 10},
                                                             {"stat", 10},
                                                             {"stat", 10},
                                                             {"stat", 10},
                                                             {"stat", 10}}))
                            .json());
    (void)unit.oneWake();
    const std::string data = unit.lastData(47);
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "program");
    CHECK(has(bench::objectOf(data, "journal"), "at most six steps"));
    CHECK(unit.world.chip->chipHolds(unit.golden));
    CHECK(unit.scripted.said.empty());
    CHECK_FALSE(unit.sessionIsOpen());
    CHECK(unit.half().firmware.counters().sessions_refused == 1);
  }
  SUBCASE("a command beginning dfu") {
    SessionJob unit;
    unit.server().queue(
        bench::ownJob(48, agent::kSessionType, bench::sessionPayload({{"dfu", 30}})).json());
    (void)unit.oneWake();
    const std::string data = unit.lastData(48);
    INFO(data);
    CHECK(bench::member(data, "refusal") == "program");
    CHECK(has(bench::objectOf(data, "journal"), "stat, sweep, scope, hist, dwell, cad"));
    CHECK(unit.world.chip->chipHolds(unit.golden));
  }
  SUBCASE("a payload the claim could not keep") {
    SessionJob unit;
    // A payload longer than the bytes a claim keeps: the uplink counts it and keeps an empty
    // text, which is what this end reads as *the payload did not fit*.
    std::vector<std::pair<std::string, int>> steps;
    for (int i = 0; i < 6; ++i) steps.push_back({std::string(39, 's'), 30});
    const std::string payload = bench::sessionPayload(steps);
    REQUIRE(payload.size() >= agent::kExtensionPayloadBytes);
    unit.server().queue(bench::ownJob(49, agent::kSessionType, payload).json());
    (void)unit.oneWake();
    const std::string data = unit.lastData(49);
    INFO(data);
    CHECK(bench::member(data, "refusal") == "program");
    CHECK(has(bench::objectOf(data, "journal"), "the bytes a claim keeps"));
    CHECK(unit.half().firmware.counters().malformed == 1);
    CHECK(unit.world.chip->chipHolds(unit.golden));
  }
  SUBCASE("a slot this board has not") {
    SessionJob unit;
    unit.server().queue(
        bench::ownJob(50, agent::kSessionType, bench::sessionPayload({{"stat", 30}}, 1)).json());
    (void)unit.oneWake();
    const std::string data = unit.lastData(50);
    INFO(data);
    CHECK(bench::member(data, "refusal") == "program");
    CHECK(has(bench::objectOf(data, "journal"), "a slot this board has not"));
    CHECK(unit.world.chip->chipHolds(unit.golden));
  }
  SUBCASE("no scanner image") {
    SessionJob unit(false);
    unit.server().queue(
        bench::ownJob(51, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());
    (void)unit.oneWake();
    const std::string data = unit.lastData(51);
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "noScanner");
    CHECK(stepRows(data) == "notRun/0/0 ");
    CHECK(unit.world.chip->chipHolds(unit.golden));
  }
  SUBCASE("the battery") {
    SessionJob unit;
    // Below the figure an update needs (kBatteryMinMv, 3 700 mV) and above the floor the modem
    // is refused at (kFloorMv, 3 400): the wake still reaches the server, and the session does not
    // begin.
    unit.world.rak.battery_mv = 3650;
    unit.server().queue(
        bench::ownJob(52, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());
    (void)unit.oneWake();
    const std::string data = unit.lastData(52);
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "battery");
    CHECK(unit.world.chip->chipHolds(unit.golden));
  }
}

TEST_CASE("a session claimed while an update is in progress, or on trial: inProgress") {
  // By hand, with the wake saying what S-7 knows - as the apply's own case does: the two flags
  // are the wake cycle's reading of the two machines, and the job refuses on them before the
  // session machine's own check (task p16-03's `inProgress` covers the journal's half).
  SUBCASE("an update of either MCU is in the air") {
    SessionJob unit;
    (void)unit.oneWake();
    interfaces::BackendKnowledge known = bench::knowledgeFor(core::power::Band::kPlenty);
    known.wake.update_running = true;
    unit.server().queue(
        bench::ownJob(53, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());
    interfaces::BackendAnswer out;
    unit.boot();
    unit.v().begin();
    (void)unit.exchange(known, 600, &out);
    const std::string data = unit.lastData(53);
    REQUIRE_FALSE(data.empty());
    INFO(data);
    CHECK(bench::member(data, "outcome") == "refused");
    CHECK(bench::member(data, "refusal") == "inProgress");
    CHECK(out.carried == interfaces::Ask::kNothing);
    CHECK(unit.world.chip->chipHolds(unit.golden));
    CHECK_FALSE(unit.sessionIsOpen());
  }
  SUBCASE("this firmware on trial") {
    SessionJob unit;
    (void)unit.oneWake();
    interfaces::BackendKnowledge known = bench::knowledgeFor(core::power::Band::kPlenty);
    known.wake.on_trial = true;
    unit.server().queue(
        bench::ownJob(54, agent::kSessionType, bench::sessionPayload({{"stat", 30}})).json());
    interfaces::BackendAnswer out;
    unit.boot();
    unit.v().begin();
    (void)unit.exchange(known, 600, &out);
    const std::string data = unit.lastData(54);
    REQUIRE_FALSE(data.empty());
    INFO(data);
    CHECK(bench::member(data, "refusal") == "inProgress");
    CHECK(has(bench::objectOf(data, "journal"), "on trial"));
    CHECK(unit.world.chip->chipHolds(unit.golden));
  }
}

// ------------------------------------------------------------------ a result that outlives a restart

TEST_CASE("a restart while the radio is a scanner: the next wake restores and posts the result") {
  SessionJob unit;
  unit.theRadioIsAScanner();
  unit.anOpenSessionOwedBy(55, upd::Stage::kStep, 3);
  CHECK(unit.half().firmware.owedResultDue());

  const Pass& pass = unit.oneWake();
  INFO(unit.v().out.text);
  CHECK(pass.session);
  // The repeater is back, the session is closed, and the result the restart owed has gone.
  CHECK(unit.world.chip->chipHolds(unit.golden));
  CHECK_FALSE(unit.sessionIsOpen());
  const std::string data = unit.lastData(55);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(bench::member(data, "outcome") == "restored");
  CHECK(bench::member(unit.lastResult(55), "outcome") == "ok");
  // `notRun` for every step of the payload - the payload went with the RAM that held it - and
  // counts of nought.
  CHECK(stepRows(data) == "notRun/0/0 notRun/0/0 notRun/0/0 ");
  CHECK(topMember(data, "lines") == "0");
  CHECK(topMember(data, "linesUnanswered") == "0");
  CHECK(bench::member(data, "scanner") == "0.1.6");
  // The journal it has: the session's own entries, from the one that opened it.
  const std::vector<std::string> steps = stepsOf(bench::objectOf(data, "journal"));
  REQUIRE_FALSE(steps.empty());
  CHECK(steps.front() == "check");
  CHECK(steps.back() == "restored");
  CHECK(unit.half().firmware.counters().owed_posted == 1);
  // And it is owed no more: the wake after it posts nothing.
  CHECK_FALSE(unit.half().firmware.resultOwed());
  const std::size_t posted = unit.server().results[55].size();
  (void)unit.oneWake();
  CHECK(unit.server().results[55].size() == posted);
}

TEST_CASE("a switch back the wake cannot make: the result says stuck") {
  // No golden image in the store, so the switch back is refused and the session stays open
  // (task p16-03's rail 5) - and the server is told `stuck` rather than left waiting.
  SessionJob unit(true, false);
  unit.theRadioIsAScanner();
  unit.anOpenSessionOwedBy(56, upd::Stage::kStep, 1);

  (void)unit.oneWake();
  CHECK(unit.sessionIsOpen());
  const std::string data = unit.lastData(56);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(bench::member(data, "outcome") == "stuck");
  CHECK(bench::member(unit.lastResult(56), "outcome") == "failed");
  CHECK(unit.world.rak.scanner);  // still a scanner: nothing was flashed over it
}

// ------------------------------------------------------------------ S-12, and the result's bound

TEST_CASE("no text of a scanner's line is in the log, the journal, a report or the job's log") {
  SessionJob unit;
  const char* kMarker = "noisescope-said-this-and-nobody-else";
  unit.scripted.on_command = [&unit, kMarker](const std::string& line) {
    if (line == "stat") {
      unit.world.rak.sendLine(std::string("{\"t\":\"stat\",\"note\":\"") + kMarker + "\"}", 10);
    }
  };
  unit.server().queue(
      bench::ownJob(57, agent::kSessionType, bench::sessionPayload({{"stat", 20}})).json());
  (void)unit.oneWake();

  // It went into a post, which is the one place it may be.
  REQUIRE_FALSE(unit.posts(57).empty());
  bool in_a_post = false;
  for (const bench::SeenLines& post : unit.posts(57)) in_a_post = in_a_post || has(post.body, kMarker);
  CHECK(in_a_post);
  // And nowhere else: not the result, not a progress report, not the job's own log.
  CHECK_FALSE(has(unit.lastResult(57), kMarker));
  for (const std::string& report : unit.server().progress[57]) CHECK_FALSE(has(report, kMarker));
  // Not the console's ring, the journal, the wake's own lines or a report of the unit.
  CHECK_FALSE(has(unit.v().runLine("dump"), kMarker));
  CHECK_FALSE(has(unit.v().runLine("journal"), kMarker));
  CHECK_FALSE(has(unit.v().runLine("agent jobs"), kMarker));
  CHECK_FALSE(has(unit.v().runLine("rak session"), kMarker));
  CHECK_FALSE(has(unit.v().runLine("wake"), kMarker));
  // And the journal's own bytes, which is where a payload would be if one had been kept.
  upd::Cursor cursor;
  upd::Entry entry;
  bool more = unit.v().journal.first(&cursor, &entry);
  while (more) {
    const std::string bytes(reinterpret_cast<const char*>(entry.payload), sizeof entry.payload);
    CHECK_FALSE(has(bytes, "noisescope-said"));
    more = unit.v().journal.previous(&cursor, &entry);
  }
}

TEST_CASE("the result of a session of six steps with a full journal fits kDataBytes") {
  // The longest journal a session can write: every stage, six steps whose commands are the
  // longest the machine takes, and the room line among them.
  // Thirty characters, which is what six steps of a payload bounded at 384 bytes can carry:
  // `{"command":"<30>","seconds":5}` is 56 bytes and six of them with the object around them are
  // 362. A command of the machine's whole forty would not fit a claim at all.
  const std::string command(24, '7');
  std::vector<std::pair<std::string, int>> steps;
  for (int i = 0; i < 6; ++i) steps.push_back({"sweep " + command, 5});
  SessionJob unit;
  bench::Answer quick;
  quick.lines = 1;
  quick.line_bytes = 200;
  unit.scripted.answer = quick;
  unit.server().queue(bench::ownJob(58, agent::kSessionType, bench::sessionPayload(steps)).json());

  (void)unit.oneWake();
  const std::string data = unit.lastData(58);
  REQUIRE_FALSE(data.empty());
  INFO(data.size());
  CHECK(data.size() < agent::kDataBytes);
  CHECK(bench::member(data, "outcome") == "restored");
  const std::vector<std::string> journalled = stepsOf(bench::objectOf(data, "journal"));
  CHECK(journalled.size() == 14);  // check, flashing-scanner, scanner, six steps, room, and four
  CHECK(stepsOf(bench::objectOf(data, "journal")).back() == "restored");
  // Six rows, one for each step of the payload.
  std::size_t rows = 0;
  for (std::size_t i = 0; i < upd::kStepsMax + 1; ++i) {
    if (objectAt(bench::objectOf(data, "steps"), i).empty()) break;
    ++rows;
  }
  CHECK(rows == 6);
}
