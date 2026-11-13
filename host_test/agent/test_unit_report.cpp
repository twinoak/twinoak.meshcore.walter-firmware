// `twinoak.report` (task p13-01): what the unit says about itself, by section.
//
// On 13 November a wake request reached the unit's radio, the radio answered *ok*, and the unit
// did not check in. `wake` says whether it woke and why, `agent state` says what wait was owed,
// `lte` says how far the uplink got - and all three are behind a USB port on a unit meant to sit
// on a mast. This job returns those lines, and these are the tests of it:
//
//   * the contract's shape, out of a claimed job on the wake bench;
//   * **one printer, two readers**: a section's text is byte for byte what the console command
//     prints in the same state, for every section where the state does not move between the two
//     readings - and, where it does (the three that ask the radio, and the two that cannot be
//     read twice), what moved and why;
//   * all eleven names answered; `calls` - which is task 02's - and a name nobody has in
//     `unknown`;
//   * a scripted unit woken by the radio's line inside a wait, and one with three spurious wakes:
//     the report says so;
//   * a result that would pass the contract's 8 000 bytes: cut at a line end, the rest left, and
//     still JSON;
//   * no secret and no stranger's words in a report of all eleven sections;
//   * the heartbeat's job types;
//   * **nothing changed**: no counter, no queue, no record.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/agent_bench.h"
#include "core/console/report_source.h"

using bench::AgentBench;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

namespace {

// The eleven names, in the table's order (tasks/phase-13/01-what-the-unit-says.md).
const char* const kAll[] = {"wakes", "agent",     "uplink",   "observations", "jobs", "journal",
                            "radio", "power",     "restarts", "settings",     "log"};
constexpr int kAllCount = static_cast<int>(sizeof kAll / sizeof kAll[0]);

bool has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// A section written straight into a string, as the job writes it into the result's buffer.
class StringOut final : public interfaces::TextOut {
 public:
  void write(const char* text, std::size_t length) override { out.append(text, length); }
  bool writeBytes(const std::uint8_t*, std::size_t) override { return false; }
  std::string out;
};

std::string viaSection(AgentBench& bench, const char* name) {
  StringOut out;
  REQUIRE(bench.v().console->writeSection(name, out));
  return out.out;
}

// -- reading a result back -----------------------------------------------------------------------

// A JSON string's escapes undone, as mm::JsonWriter made them.
std::string unescaped(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\') {
      out.push_back(text[i]);
      continue;
    }
    if (++i >= text.size()) break;
    switch (text[i]) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'u': {
        // Only the escapes this console can produce: a control character, as \u00XX.
        if (i + 4 < text.size()) {
          out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 4), nullptr, 16) & 0xFF));
          i += 4;
        }
        break;
      }
      default: out.push_back(text[i]); break;
    }
  }
  return out;
}

struct Section {
  std::string name;
  std::string text;
  bool cut = false;
};

// The `sections` array of a result's data, object by object.
std::vector<Section> sectionsOf(const std::string& data) {
  std::vector<Section> out;
  const std::string array = bench::objectOf(data, "sections");
  int depth = 0;
  bool in_string = false;
  std::size_t begin = 0;
  for (std::size_t i = 0; i < array.size(); ++i) {
    const char c = array[i];
    if (in_string) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      if (depth++ == 0) begin = i;
    } else if (c == '}') {
      if (--depth == 0) {
        const std::string one = array.substr(begin, i - begin + 1);
        Section s;
        s.name = bench::member(one, "name");
        s.text = unescaped(bench::member(one, "text"));
        s.cut = bench::member(one, "cut") == "true";
        out.push_back(s);
      }
    }
  }
  return out;
}

// The first object of an array value, braces and all.
std::string firstObject(const std::string& array) {
  int depth = 0;
  bool in_string = false;
  std::size_t begin = 0;
  for (std::size_t i = 0; i < array.size(); ++i) {
    const char c = array[i];
    if (in_string) {
      if (c == '\\') ++i;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{') { if (depth++ == 0) begin = i; }
    else if (c == '}') { if (--depth == 0) return array.substr(begin, i - begin + 1); }
  }
  return "";
}

// The names of a `left` or `unknown` array.
std::vector<std::string> namesOf(const std::string& data, const char* key) {
  std::vector<std::string> out;
  const std::string array = bench::objectOf(data, key);
  std::size_t at = 0;
  while ((at = array.find('"', at)) != std::string::npos) {
    const std::size_t end = array.find('"', at + 1);
    if (end == std::string::npos) break;
    out.push_back(array.substr(at + 1, end - at - 1));
    at = end + 1;
  }
  return out;
}

// A payload that asks for these names.
std::string payloadFor(const std::vector<std::string>& names) {
  std::string out = "{\"sections\":[";
  for (std::size_t i = 0; i < names.size(); ++i) {
    out += (i == 0 ? "\"" : ",\"") + names[i] + "\"";
  }
  return out + "]}";
}

std::string payloadForAll() {
  return payloadFor(std::vector<std::string>(kAll, kAll + kAllCount));
}

// Whole JSON: the braces and brackets balance outside the strings, every escape is one this
// writer makes, and no raw control character is in a string (the contract's "no other control
// character").
bool wholeJson(const std::string& text) {
  int depth = 0;
  bool in_string = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (in_string) {
      if (c == '\\') {
        if (i + 1 >= text.size()) return false;
        const char next = text[i + 1];
        if (std::string("\"\\/bfnrtu").find(next) == std::string::npos) return false;
        ++i;
        continue;
      }
      if (c < 0x20) return false;
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') --depth;
    if (depth < 0) return false;
  }
  return depth == 0 && !in_string;
}

// The two shapes of the contract, as `host_test/fixtures/contracts-ahead/` holds them: this
// firmware's reading of a contract agreed in words, until meshmaster.dk's task 216 makes the
// server's own file (that directory's README, and the follow-up that replaces them).
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

// A unit with a wake behind it and the uplink up, for the console's sake.
void awake(AgentBench& bench) {
  bench.modemWorks();
  bench.v().begin();
  (void)bench.v().runLine("lte up");
}

}  // namespace

TEST_CASE("twinoak.report: the contract's shape, out of a claimed job") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(bench::ownJob(
      28399, agent::kReportType,
      payloadFor({"wakes", "agent", "calls", "uplink", "observations", "jobs"})).json());
  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kDone);
  const std::string result = bench.lastResult(28399);
  REQUIRE_FALSE(result.empty());
  CHECK(bench::member(result, "outcome") == "ok");
  const std::string data = bench.lastData(28399);
  INFO(data);
  CHECK(wholeJson(data));
  CHECK(data.size() <= agent::kReportDataBytes);
  // The shape, against the contract's own two blocks: the top-level keys in their order, and a
  // section object's three.
  CHECK(bench::topKeys(data) == bench::topKeys(aheadFixture("UnitReportResult")));
  const std::string fixture_one =
      firstObject(bench::objectOf(aheadFixture("UnitReportResult"), "sections"));
  CHECK(bench::topKeys(fixture_one) == std::vector<std::string>{"name", "text", "cut"});
  CHECK(bench::topKeys(firstObject(bench::objectOf(data, "sections"))) ==
        bench::topKeys(fixture_one));
  // And the payload this job was given is the payload's own shape.
  CHECK(bench::topKeys(aheadFixture("UnitReportPayload")) == std::vector<std::string>{"sections"});
  // The order asked is the order written, and the one name this firmware has no section for is
  // the one task 02 will bring.
  const std::vector<Section> sections = sectionsOf(data);
  REQUIRE(sections.size() == 5);
  CHECK(sections[0].name == "wakes");
  CHECK(sections[1].name == "agent");
  CHECK(sections[2].name == "uplink");
  CHECK(sections[3].name == "observations");
  CHECK(sections[4].name == "jobs");
  CHECK(namesOf(data, "unknown") == std::vector<std::string>{"calls"});
  CHECK(namesOf(data, "left").empty());
  for (const Section& s : sections) {
    CHECK_FALSE(s.cut);
    CHECK_FALSE(s.text.empty());
    CHECK_MESSAGE(s.text.back() != '\n', "the text ends at the last line, not after it");
  }
  CHECK(has(sections[0].text, "this wake:"));
  CHECK(has(sections[1].text, "agent state:"));
  CHECK(has(sections[2].text, "lte:"));
  CHECK(has(sections[3].text, "agent obs:"));
  CHECK(has(sections[4].text, "budget "));
  // The unit's clock, as `at` carries it: the wake took the network's time at its step 6.
  CHECK(std::stoul(bench::member(data, "at")) > 0u);
}

TEST_CASE("a section's text is the console command's own lines, byte for byte") {
  AgentBench bench(nullptr, false);
  awake(bench);

  // The sections whose state does not move between two readings in one boot: the section first -
  // it writes nothing to the port and keeps nothing in the ring - then the command.
  SUBCASE("wakes is `wake`") {
    const std::string section = viaSection(bench, "wakes");
    CHECK(section == bench.v().runLine("wake"));
    CHECK(has(section, "the clock:"));
  }
  SUBCASE("agent is `agent state`") {
    const std::string section = viaSection(bench, "agent");
    CHECK(section == bench.v().runLine("agent state"));
    CHECK(has(section, "the clock:"));
  }
  SUBCASE("uplink is `lte`") {
    const std::string section = viaSection(bench, "uplink");
    CHECK(section == bench.v().runLine("lte"));
  }
  SUBCASE("observations is `agent obs`, without its pull") {
    const std::string section = viaSection(bench, "observations");
    CHECK(section == bench.v().runLine("agent obs"));
    CHECK_FALSE(has(section, "the radio's ring drained"));
  }
  SUBCASE("jobs is `agent jobs`") {
    const std::string section = viaSection(bench, "jobs");
    CHECK(section == bench.v().runLine("agent jobs"));
  }
  SUBCASE("journal is `journal`") {
    const std::string section = viaSection(bench, "journal");
    CHECK(section == bench.v().runLine("journal"));
  }
  SUBCASE("restarts is `reboots` and `heap`") {
    const std::string section = viaSection(bench, "restarts");
    const std::string reboots = bench.v().runLine("reboots");
    CHECK(section == reboots + bench.v().runLine("heap"));
  }
  SUBCASE("settings is every setting, as the console prints them") {
    const std::string section = viaSection(bench, "settings");
    CHECK(section == bench.v().runLine("get"));
    CHECK(has(section, "server.token"));
    CHECK(has(section, "set"));
  }

  // And the three that cannot be read twice into the same answer, each for a reason of its own.
  SUBCASE("radio is `rak status`, `rak watch` and the counts of `rak events`") {
    // Not byte for byte: the section asks the radio, which is what the task allows it, and the
    // link's own counters and the radio's uptime have moved by the time the commands are typed.
    const std::string section = viaSection(bench, "radio");
    CHECK(has(section, "radio status:"));
    CHECK(has(section, "rak watch:"));
    CHECK(has(section, "events queued"));
    CHECK(has(section, "link: "));  // the retries and timeouts, new to `rak events` in this task
    CHECK(has(section, "retries"));
    const std::string typed = bench.v().runLine("rak status");
    CHECK(has(typed, "radio status:"));
  }
  SUBCASE("power is the band this wake decided, and `power` decides a new one") {
    const std::size_t before = bench.duty().battery().sampleCount();
    const std::string section = viaSection(bench, "power");
    CHECK(has(section, "band: "));
    CHECK(has(section, "thresholds (PLACEHOLDERS"));
    CHECK(has(section, "next wake:"));
    CHECK_MESSAGE(bench.duty().battery().sampleCount() == before,
                  "the section takes no reading: `power` does, and a reading is a sample");
  }
  SUBCASE("log is `dump`, and the report's own lines are not in it") {
    (void)bench.v().runLine("status");  // something to find in the ring
    const std::string section = viaSection(bench, "log");
    CHECK(has(section, "dump: "));
    CHECK(has(section, "status"));
    // Not byte for byte: `dump` typed at the port keeps its own first line in the ring it then
    // prints, and a section keeps nothing. That is the point of keeping nothing - a `log` full of
    // the last report says nothing about the port - and a second section is the same bytes as
    // the first.
    const std::string second = viaSection(bench, "log");
    CHECK_MESSAGE(section == second, "the first section's lines did not reach the ring");
  }
}

TEST_CASE("all eleven names are answered; a name nobody has is unknown") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(bench::ownJob(7, agent::kReportType,
                                     payloadFor({"wakes", "agent", "uplink", "observations", "jobs",
                                                 "journal", "radio", "power", "restarts",
                                                 "settings", "log"}))
                           .json());
  (void)bench.oneWake();
  const std::string data = bench.lastData(7);
  REQUIRE_FALSE(data.empty());
  CHECK(wholeJson(data));
  const std::vector<Section> sections = sectionsOf(data);
  std::vector<std::string> written;
  for (const Section& s : sections) written.push_back(s.name);
  for (const std::string& name : namesOf(data, "left")) written.push_back(name);
  CHECK(written == std::vector<std::string>(kAll, kAll + kAllCount));
  CHECK(namesOf(data, "unknown").empty());
  // What it costs, for the report of this task: the bytes of one answer against the 32 kB a wake
  // may post (walter_agent/agent_backend.h, kWakePostBytes).
  std::string sizes;
  for (const Section& s : sections) sizes += " " + s.name + "=" + std::to_string(s.text.size());
  MESSAGE("all eleven sections on a fresh unit: " << data.size() << " bytes of data, "
          << sections.size() << " written, " << namesOf(data, "left").size() << " left;" << sizes);

  // `calls` is task 02's, and a name nobody has at all: both are unknown, which is what unknown
  // is for.
  AgentBench other(nullptr, false);
  other.modemWorks();
  other.server().queue(
      bench::ownJob(8, agent::kReportType, payloadFor({"calls", "weather", "agent"})).json());
  (void)other.oneWake();
  const std::string second = other.lastData(8);
  CHECK(namesOf(second, "unknown") == std::vector<std::string>{"calls", "weather"});
  REQUIRE(sectionsOf(second).size() == 1);
  CHECK(sectionsOf(second)[0].name == "agent");
}

TEST_CASE("a wake on the radio's line with a wait still owed: the report says so") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // A failed call leaves a wait owed; the wake after it is one the radio's line asked for, and
  // `WakeReason::timeAwayMs()` is 0 for it, so the whole wait is still owed and nothing is sent.
  bench.server().away(true);
  (void)bench.oneWake();
  bench.server().away(false);
  (void)bench.oneWake(interfaces::WokeBy::kRadioLine);

  // The report of that state, read from the console's own sections (no wake is needed to read
  // what a wake left).
  const std::string wakes = viaSection(bench, "wakes");
  INFO(wakes);
  CHECK(has(wakes, "the radio's line"));
  CHECK(has(wakes, "the clock:"));
  const std::string state = viaSection(bench, "agent");
  INFO(state);
  CHECK(has(state, "hold "));
  CHECK(has(state, "consecutive failure(s)"));
  CHECK(has(state, "owed before the sleep"));
  CHECK(has(state, "the clock:"));
}

TEST_CASE("three spurious wakes: the count, and the line disarmed") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  for (int i = 0; i < 3; ++i) (void)bench.oneWake(interfaces::WokeBy::kRadioLine);
  const std::string wakes = viaSection(bench, "wakes");
  INFO(wakes);
  CHECK(has(wakes, "spurious"));
  CHECK(has(wakes, "the radio's line:"));
}

TEST_CASE("a result that would pass 8 000 bytes is cut at a line end, and the rest are left") {
  AgentBench bench(nullptr, false);
  awake(bench);
  // Something in the ring: `status`, `wake` and `journal` typed a few times fill the four
  // kilobytes `dump` prints, so that `log` alone is half the result.
  for (int i = 0; i < 6; ++i) {
    (void)bench.v().runLine("status");
    (void)bench.v().runLine("wake");
    (void)bench.v().runLine("get all");
  }
  bench.server().queue(
      bench::ownJob(9, agent::kReportType,
                    payloadFor({"log", "settings", "wakes", "journal", "agent", "uplink", "jobs",
                                "observations", "power", "restarts", "radio"}))
          .json());
  (void)bench.v().runLine("agent wake");
  const std::string data = bench.lastData(9);
  REQUIRE_FALSE(data.empty());
  INFO(data.substr(0, 400));
  CHECK(wholeJson(data));
  CHECK(data.size() <= agent::kReportDataBytes);

  const std::vector<Section> sections = sectionsOf(data);
  REQUIRE_FALSE(sections.empty());
  int cut = 0;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (sections[i].cut) {
      ++cut;
      CHECK_MESSAGE(i + 1 == sections.size(), "only the last section written can be the cut one");
    }
    const bool ends_at_a_line = sections[i].text.empty() || sections[i].text.back() != '\n';
    CHECK_MESSAGE(ends_at_a_line,
                  "a cut is made at a line end, and the text never ends in a newline");
  }
  CHECK_MESSAGE(cut == 1, "the result was full: the section that did not fit whole is marked");
  MESSAGE("a full result: " << data.size() << " bytes of data, " << sections.size()
          << " written (the last cut), " << namesOf(data, "left").size() << " left");
  CHECK_FALSE(namesOf(data, "left").empty());
  // Every line of a cut section is a whole line of the command it came from - read again, when
  // the cut one is a section whose state has not moved since (the three that ask the radio have).
  const std::string& last = sections.back().name;
  if (last != "radio" && last != "jobs" && last != "log" && last != "power") {
    const std::string whole = viaSection(bench, last.c_str());
    CHECK(whole.compare(0, sections.back().text.size(), sections.back().text) == 0);
  }
}

TEST_CASE("no secret and nothing a stranger said is in a report of all eleven sections") {
  AgentBench bench(nullptr, false);
  awake(bench);
  const char* const kPin = "86421357";
  const char* const kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";  // the bench's own
  const char* const kTypedToken = "mma_Xq91TyPeDaTtHeCaBlE7fGhJkLmNoPqR";
  const char* const kPassword = "a-repeater-admin-password-9f2a";
  const char* const kStrangerSaid = "what the mesh said at Grenaa";
  (void)bench.v().runLine((std::string("set lte.pin ") + kPin).c_str());
  (void)bench.v().runLine((std::string("set server.token ") + kTypedToken).c_str());
  // And what the radio said as text, which §S-12 says is never written down.
  bench.world.rak.sendLine(std::string("{\"noise\":-112,\"note\":\"") + kStrangerSaid + "\"}");
  bench.world.clock.advance(50);
  CHECK(has(bench.v().runLine("rak text"), kStrangerSaid));

  bench.server().queue(bench::ownJob(11, agent::kReportType, payloadForAll())
                           .withAdminPassword(kPassword)
                           .json());
  (void)bench.v().runLine("agent wake");
  std::string data = bench.lastData(11);
  REQUIRE_FALSE(data.empty());
  // Whatever did not fit this one, in a second report of its own: every section is searched.
  for (const Section& s : sectionsOf(data)) data += s.text;
  for (int i = 0; i < kAllCount; ++i) data += viaSection(bench, kAll[i]);

  for (const char* needle : {kPin, kToken, kTypedToken, kPassword, kStrangerSaid}) {
    CHECK_MESSAGE(!has(data, needle), "the report carries " << std::string(needle));
  }
  // The control: a grep that has stopped matching anything proves nothing.
  CHECK(has(data, "server.token"));
  CHECK(has(data, "lte.pin"));
}

TEST_CASE("the heartbeat advertises twinoak.report, and the four others are unchanged") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.v().begin();
  (void)bench.v().runLine("lte up");
  (void)bench.v().runLine("agent wake");
  REQUIRE_FALSE(bench.server().heartbeats.empty());
  const std::string body = bench.server().heartbeats.back();
  INFO(body);
  for (const char* type : {agent::kOfferType, agent::kApplyType, agent::kStateType,
                           agent::kPowerCycleType, agent::kReportType}) {
    CHECK(has(body, std::string("\"") + type + "\""));
  }
  CHECK(bench.half().firmware.typeCount() == 5);
  CHECK(std::string(bench.half().firmware.type(4)) == agent::kReportType);
}

TEST_CASE("no section changes a counter, a queue or a record") {
  AgentBench bench(nullptr, false);
  awake(bench);
  (void)bench.v().runLine("status");

  const std::size_t events_before = bench.v().link.eventsQueued();
  const std::size_t samples_before = bench.duty().battery().sampleCount();
  const std::uint32_t seq_before = bench.v().journal.nextSeq();
  const std::uint32_t wakes_before = bench.duty().wake().wakes;
  const core::power::Band band_before = bench.duty().decision().band;

  std::vector<std::string> first;
  for (int i = 0; i < kAllCount; ++i) first.push_back(viaSection(bench, kAll[i]));
  std::vector<std::string> second;
  for (int i = 0; i < kAllCount; ++i) second.push_back(viaSection(bench, kAll[i]));

  CHECK(bench.v().link.eventsQueued() == events_before);
  CHECK(bench.duty().battery().sampleCount() == samples_before);
  CHECK(bench.v().journal.nextSeq() == seq_before);
  CHECK(bench.duty().wake().wakes == wakes_before);
  CHECK(bench.duty().decision().band == band_before);

  // And what the two readings say: everything but the three that ask the radio (whose counters
  // the asking moves, which the task allows) and `log` (which is the port's own lines, and the
  // port printed nothing in between either).
  for (int i = 0; i < kAllCount; ++i) {
    const std::string name(kAll[i]);
    if (name == "radio" || name == "jobs") continue;
    CHECK_MESSAGE(first[i] == second[i], "the " << name << " section is not the same twice");
  }
}

TEST_CASE("two reports in a row differ in `at`, in `log` and in `jobs`, and in nothing else") {
  AgentBench bench(nullptr, false);
  awake(bench);
  const std::string payload = payloadFor({"settings", "restarts"});
  bench.server().queue(bench::ownJob(21, agent::kReportType, payload).json());
  (void)bench.v().runLine("agent wake");
  bench.server().queue(bench::ownJob(22, agent::kReportType, payload).json());
  bench.world.clock.advance(2000);
  (void)bench.v().runLine("agent wake");

  const std::string one = bench.lastData(21);
  const std::string two = bench.lastData(22);
  REQUIRE_FALSE(one.empty());
  REQUIRE_FALSE(two.empty());
  const std::vector<Section> a = sectionsOf(one);
  const std::vector<Section> b = sectionsOf(two);
  REQUIRE(a.size() == 2);
  REQUIRE(b.size() == 2);
  CHECK(a[0].text == b[0].text);
  CHECK(a[1].text == b[1].text);
  CHECK(bench.half().firmware.counters().reports == 2u);
}

TEST_CASE("a build with no console, and a payload that names no sections") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  bench.server().queue(bench::ownJob(31, agent::kReportType, "{}").json());
  (void)bench.oneWake();
  CHECK(bench::member(bench.lastResult(31), "outcome") == "failed");

  AgentBench other(nullptr, false);
  other.modemWorks();
  other.half().firmware.readTheUnitFrom(nullptr);
  other.server().queue(bench::ownJob(32, agent::kReportType, payloadFor({"wakes"})).json());
  (void)other.v().begin();
  (void)other.v().runLine("lte up");
  (void)other.v().runLine("agent wake");
  CHECK(bench::member(other.lastResult(32), "outcome") == "failed");
}
