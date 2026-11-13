// What a wake did, in this firmware's own words (task p10-02).
//
// Two readings of the same console went wrong on 7 and 8 November 2026, and both were this
// firmware's fault rather than the reader's:
//
//   * `wake` gave each wake's length and nothing else. Three wakes of 40 s and four of 6 s were
//     read first as wakes that skipped the modem and then as a failed connection; `lte` said both
//     readings were wrong.
//   * the journal said *the backend was reached* on wakes that sent nothing. The flag was set for
//     every exchange that did not fail, and a loop held back by a wait answers *nothing to do*.
//
// So this holds the three answers apart - **answered**, **sent nothing (and why)** and
// **failed** - through a whole wake on the bench of phase 3, and then holds the two consoles to
// what each of the three prints. The agent's half of it, where the reason is named from the
// library's own chain, is host_test/agent/test_what_a_wake_did.cpp.
#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/cycle/null_backend.h"
#include "core/cycle/wake_bench.h"

using bench::WakeBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;
using interfaces::Exchanged;
using interfaces::Reached;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

std::string readDoc(const char* name) {
  const std::filesystem::path p = std::filesystem::path(TWINOAK_REPO_ROOT) / "docs" / name;
  REQUIRE_MESSAGE(std::filesystem::is_regular_file(p), p.string());
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Every line of a console answer fits a 100-column terminal, which is what the letters were
// added inside of.
void fitsATerminal(const std::string& text) {
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? end : end - at);
    CHECK_MESSAGE(line.size() <= 100, line);
    if (end == std::string::npos) break;
    at = end + 1;
  }
}

// The line of `wake`'s list whose number is `number`, newest first.
std::string listLine(const std::string& said, int number) {
  char want[16] = {};
  std::snprintf(want, sizeof want, "\n  %2d. ", number);
  const std::size_t at = said.find(want);
  REQUIRE_MESSAGE(at != std::string::npos, said);
  const std::size_t from = at + 1;
  const std::size_t end = said.find('\n', from);
  return said.substr(from, end == std::string::npos ? end : end - from);
}

// The letters `wake` prints for the newest wake in its list, which is the wake in hand.
// "  %2lu. " is six characters, then the reason in a nine-wide field, a space, three letters.
std::string lettersOfTheNewest(const std::string& said) {
  const std::string line = listLine(said, 1);
  REQUIRE(line.size() >= 6 + 9 + 1 + 3);
  return line.substr(6 + 9 + 1, 3);
}

}  // namespace

TEST_CASE("a wake held back by a wait: not reached, and the journal says what it did instead") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.sentNothing(Reached::kWait, 30);

  const Pass& pass = bench.oneWake();
  CHECK(pass.ending == Ending::kSlept);
  // The seam's own answer is *nothing to do* - a wait is not a failure of anybody's - and the
  // reason beside it is what the old two values could not say.
  CHECK(pass.exchanged == Exchanged::kNothing);
  CHECK(pass.reached == Reached::kWait);
  CHECK(pass.owed_s == 30);
  CHECK_FALSE(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kNothing);

  // The journal: an entry, because a timer wake that still owes a wait means a sleep did not
  // cover its hold - and **not** the flag, because nobody was reached.
  const std::vector<core::cycle::WakePayload> entries = bench.wakeEntries();
  REQUIRE(!entries.empty());
  CHECK((entries[0].flags & core::cycle::kWakeFlagAnswered) == 0);
  CHECK(entries[0].reached == Reached::kWait);
  CHECK(entries[0].owed_s == 30);

  const std::string said = bench.v().runLine("journal");
  INFO(said);
  CHECK(has(said, "nothing sent: a wait still owed, 30 s"));
  CHECK_FALSE(has(said, "the backend was reached"));

  // And the one line the console prints at every wake says it too.
  char line[224] = {};
  (void)core::cycle::recordLine(pass, line, sizeof line);
  CHECK(has(std::string(line), "nothing sent: a wait still owed"));
}

TEST_CASE("an answered wake with jobs: reached, and three letters say so") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.ranJobs(2);

  const Pass& pass = bench.oneWake();
  CHECK(pass.exchanged == Exchanged::kAsked);
  CHECK(pass.reached == Reached::kAnswered);
  CHECK(pass.jobs_ran == 2);
  CHECK(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));

  const std::vector<core::cycle::WakePayload> entries = bench.wakeEntries();
  REQUIRE(!entries.empty());
  CHECK((entries[0].flags & core::cycle::kWakeFlagAnswered) != 0);
  CHECK(entries[0].reached == Reached::kAnswered);

  const std::string journal = bench.v().runLine("journal");
  INFO(journal);
  CHECK(has(journal, "the backend was reached"));
  CHECK_FALSE(has(journal, "nothing sent"));

  // `wake`'s list: the uplink came up, the server answered, a job ran.
  const std::string said = bench.v().runLine("wake");
  INFO(said);
  CHECK(has(said, "M the uplink came up, S the server answered, J a job ran"));
  CHECK(lettersOfTheNewest(said) == "MSJ");
  fitsATerminal(said);
  // And the record line of such a wake carries no clause: there is nothing to explain.
  char line[224] = {};
  (void)core::cycle::recordLine(pass, line, sizeof line);
  CHECK_FALSE(has(std::string(line), "nothing sent"));
}

TEST_CASE("a heartbeat that failed: the step failed, nothing is claimed, and the letters say S not") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.answers = Exchanged::kFailed;

  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kFailed);
  // **An exchange that failed says nothing about who was reached**: *the exchange failed* is the
  // step's own outcome and the whole of the news, so whatever it left behind is dropped.
  CHECK(pass.reached == Reached::kUnsaid);
  CHECK_FALSE(interfaces::wasAnswered(pass.exchanged, bench.cycle().answer()));

  const std::vector<core::cycle::WakePayload> entries = bench.wakeEntries();
  REQUIRE(!entries.empty());
  CHECK((entries[0].flags & core::cycle::kWakeFlagAnswered) == 0);
  CHECK(entries[0].reached == Reached::kUnsaid);

  const std::string journal = bench.v().runLine("journal");
  INFO(journal);
  CHECK_FALSE(has(journal, "the backend was reached"));
  // And no *nothing sent* either: the step's own `x` is what a failed exchange reads as, and a
  // reason beside it would be a reason the exchange never gave.
  CHECK_FALSE(has(journal, "nothing sent"));

  const std::string said = bench.v().runLine("wake");
  INFO(said);
  CHECK(lettersOfTheNewest(said) == "M--");
  fitsATerminal(said);
}

TEST_CASE("a wake that never got its uplink: no letter of the three, and the step says why") {
  WakeBench bench;
  bench.modemSilent();
  bench.world.backend.ranJobs(1);  // it would have, had anything reached it

  const Pass& pass = bench.oneWake();
  CHECK(pass.outcomeOf(Step::kUplink) == Outcome::kFailed);
  CHECK(pass.outcomeOf(Step::kBackend) == Outcome::kSkipped);
  // **Since task p13-02 the wake says why, and step 6 is who says it.** Step 8 needs step 6, so
  // the seam was never reached and `kUnsaid` was all a reader got where the reason was: the
  // modem did not come up, and that is `kNoUplink`.
  CHECK(pass.reached == Reached::kNoUplink);

  const std::string said = bench.v().runLine("wake");
  INFO(said);
  // `---` and not `???`: this wake ran to its end and did none of the three, which is a fact and
  // not a silence.
  CHECK(lettersOfTheNewest(said) == "---");
  // ... and the list says why, which is what 13 November was missing.
  CHECK(has(said, "nothing sent: no uplink"));
}

TEST_CASE("a wake that never reached sleep wrote nothing down, and `wake` says `???` for it") {
  WakeBench bench;
  bench.modemWorks();
  // A boot that worked out why it was awake and was then cut off - a reset, a crash, the bound -
  // before step 10 could record what it did.
  bench.boot();
  bench.v().begin();
  bench.boot();
  bench.v().begin();

  const std::string said = bench.v().runLine("wake");
  INFO(said);
  // Two entries: this wake, and the one before it that wrote nothing.
  const std::string second = listLine(said, 2);
  INFO(second);
  CHECK(has(second, "???"));
  CHECK(has(second, "it never reached sleep"));
  fitsATerminal(said);
}

TEST_CASE("the news rule: a timer wake owing a wait is news, and the other reasons are not") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.sentNothing(Reached::kNotConfigured);
  // The first wake has news whatever happens in it: the band changed from nothing to `plenty`.
  (void)bench.oneWake();
  const std::size_t after_first = bench.wakeEntries().size();
  REQUIRE(after_first == 1);

  // A unit with no server configured sends nothing at every wake for ever, so **that is not
  // news**: one entry a wake would wrap the 1024-entry log in ten days at the quarter-hour
  // interval, which is the whole reason the journal keeps only news (task p3-04).
  for (int i = 0; i < 3; ++i) {
    const Pass& quiet = bench.oneWake();
    CHECK(quiet.reached == Reached::kNotConfigured);
    CHECK(quiet.ending == Ending::kSlept);
  }
  CHECK(bench.wakeEntries().size() == after_first);
  // It is in the wake's own line all the same, which is where a person looking at the port sees it.
  char line[224] = {};
  (void)core::cycle::recordLine(bench.cycle().last(), line, sizeof line);
  CHECK(has(std::string(line), "nothing sent: not configured"));

  // A wait owed on a timer wake **is** news: since task p10-01 a sleep takes its own length off
  // the wait and step 10 lengthens the sleep to cover one, so a timer wake that still owes means
  // a sleep did not cover its hold - the failure of the night of 8 November 2026.
  bench.world.backend.sentNothing(Reached::kWait, 45);
  (void)bench.oneWake();
  CHECK(bench.wakeEntries().size() == after_first + 1);
  (void)bench.oneWake();
  CHECK(bench.wakeEntries().size() == after_first + 2);

  // And on the radio's line it is not: a line wake inside a hold is expected, and p10-01's own
  // rule is that no part of such a sleep is claimed, so the wait is owed on purpose.
  const std::size_t before_line = bench.wakeEntries().size();
  const Pass& line_wake = bench.oneWake(interfaces::WokeBy::kRadioLine);
  CHECK(line_wake.reached == Reached::kWait);
  CHECK(bench.wakeEntries().size() == before_line);
}

TEST_CASE("the journal entry: the two new fields round-trip, and an entry of 0.1.2 still reads") {
  core::cycle::WakePayload p{};
  p.wake = core::cycle::Wake::kTimer;
  p.band = core::power::Band::kNormal;
  p.radio = core::supervisor::Health::kHealthy;
  p.flags = core::cycle::kWakeFlagSlept;
  p.reached = Reached::kWait;
  p.owed_s = 1800;
  p.wakes = 56;
  std::uint8_t bytes[core::cycle::kWakePayloadBytes];
  core::cycle::encode(p, bytes);

  core::update::Entry e;
  e.kind = core::update::kind::kWakeCycle;
  e.size = static_cast<std::uint8_t>(core::cycle::kWakePayloadBytes);
  std::memcpy(e.payload, bytes, sizeof bytes);
  core::cycle::WakePayload got{};
  REQUIRE(core::cycle::decode(e, &got));
  CHECK(got.reached == Reached::kWait);
  CHECK(got.owed_s == 1800);
  CHECK(got.wakes == 56);

  // **A payload of 0.1.2's length still decodes**, and answers *nothing was said* for the two
  // fields it never held: a unit's journal is the log of the firmware that wrote it, and an
  // update is not allowed to blind it.
  e.size = static_cast<std::uint8_t>(core::cycle::kWakePayloadBytesV1);
  for (std::size_t i = core::cycle::kWakePayloadBytesV1; i < sizeof e.payload; ++i) {
    e.payload[i] = 0xAB;  // whatever is past the old payload is not read
  }
  core::cycle::WakePayload old{};
  REQUIRE(core::cycle::decode(e, &old));
  CHECK(old.wakes == 56);
  CHECK(old.reached == Reached::kUnsaid);
  CHECK(old.owed_s == 0);

  // A byte that is no Reached is refused rather than cast.
  e.size = static_cast<std::uint8_t>(core::cycle::kWakePayloadBytes);
  std::memcpy(e.payload, bytes, sizeof bytes);
  e.payload[32] = static_cast<std::uint8_t>(Reached::kCount);
  core::cycle::WakePayload refused{};
  CHECK_FALSE(core::cycle::decode(e, &refused));
}

TEST_CASE("the null backend never claims to have reached anybody") {
  core::cycle::NullBackend backend;
  interfaces::BackendKnowledge known{};
  interfaces::BackendAnswer answer{};
  const Exchanged how = backend.exchange(known, 1000, &answer);
  CHECK(how == Exchanged::kNothing);
  CHECK(answer.reached == Reached::kNotConfigured);
  CHECK_FALSE(interfaces::wasAnswered(how, answer));
  // And it survives being given nowhere to write.
  CHECK(backend.exchange(known, 1000, nullptr) == Exchanged::kNothing);
}

TEST_CASE("kUnsaid is the zero, so a backend that says nothing is never credited") {
  interfaces::BackendAnswer answer{};
  CHECK(answer.reached == Reached::kUnsaid);
  CHECK_FALSE(interfaces::wasAnswered(Exchanged::kNothing, answer));
  CHECK_FALSE(interfaces::wasAnswered(Exchanged::kAsked, answer));
  answer.reached = Reached::kAnswered;
  CHECK(interfaces::wasAnswered(Exchanged::kNothing, answer));
  CHECK(interfaces::wasAnswered(Exchanged::kAsked, answer));
  // Not even a failure that claims it.
  CHECK_FALSE(interfaces::wasAnswered(Exchanged::kFailed, answer));
  // Every value has a phrase, and no two are the same.
  for (std::uint8_t a = 0; a < static_cast<std::uint8_t>(Reached::kCount); ++a) {
    const std::string phrase = interfaces::describe(static_cast<Reached>(a));
    CHECK_MESSAGE(phrase != "?", static_cast<unsigned>(a));
    for (std::uint8_t b = 0; b < a; ++b) {
      CHECK(phrase != std::string(interfaces::describe(static_cast<Reached>(b))));
    }
  }
}

TEST_CASE("the documents carry every reason and every letter, in this firmware's own words") {
  const std::string cycle = readDoc("cycle.md");
  // Every phrase the seam can give, kUnsaid included: a person reading a journal or a record line
  // has to be able to look up the words in front of them.
  for (std::uint8_t r = 0; r < static_cast<std::uint8_t>(Reached::kCount); ++r) {
    const char* why = interfaces::describe(static_cast<Reached>(r));
    CHECK_MESSAGE(has(cycle, why), std::string(why));
  }
  // The journal's one claim, and the clause that replaced it on a wake that sent nothing.
  CHECK(has(cycle, "the backend was reached"));
  CHECK(has(cycle, "nothing sent"));
  // And the three letters, in both pages: the table in docs/cycle.md, the summary beside the
  // command in docs/console.md.
  const std::string console = readDoc("console.md");
  for (const char* letter : {"`M`", "`S`", "`J`", "`?`"}) {
    CHECK_MESSAGE(has(cycle, letter), std::string(letter));
  }
  CHECK(has(console, "the uplink came up"));
  CHECK(has(console, "the server answered"));
  CHECK(has(console, "a job ran"));
}
