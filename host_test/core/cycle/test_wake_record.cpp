// The wake's record (task p3-05): the one compact line a person reads after a week on a mast -
// its format, the letters, what the journal keeps of it and what it must never carry.
//
// The format is documented in docs/cycle.md and this is what holds the two together: every letter
// of the table appears in the document, and every field of the line is checked here.
#include <doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/cycle/agent_attention.h"
#include "core/cycle/wake_bench.h"

namespace fs = std::filesystem;

using bench::WakeBench;
using core::cycle::letter;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

namespace {

std::string readFile(const fs::path& p) {
  REQUIRE_MESSAGE(fs::is_regular_file(p), p.string());
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string cycleDoc() { return readFile(fs::path(TWINOAK_REPO_ROOT) / "docs" / "cycle.md"); }

std::string lineOf(const Pass& pass) {
  char line[224] = {};
  (void)core::cycle::recordLine(pass, line, sizeof line);
  return std::string(line);
}

}  // namespace

TEST_CASE("the record's letters are one per outcome, distinct, and all documented") {
  const std::string doc = cycleDoc();
  char seen[static_cast<std::size_t>(Outcome::kCount)] = {};
  for (std::uint8_t o = 0; o < static_cast<std::uint8_t>(Outcome::kCount); ++o) {
    const Outcome outcome = static_cast<Outcome>(o);
    const char c = letter(outcome);
    CHECK(c != '?');
    // No two outcomes share a letter.
    for (std::uint8_t k = 0; k < o; ++k) CHECK(seen[k] != c);
    seen[o] = c;
    // And the document says what it means: a backquoted letter, and the words of describe().
    const std::string quoted = std::string("`") + c + "`";
    CHECK_MESSAGE(doc.find(quoted) != std::string::npos, std::string(1, c));
    CHECK_MESSAGE(doc.find(core::cycle::describe(outcome)) != std::string::npos,
                  std::string(core::cycle::describe(outcome)));
  }
}

TEST_CASE("docs/cycle.md names every step, its bound and the backend's closed set") {
  const std::string doc = cycleDoc();
  for (std::size_t i = 0; i < core::cycle::kStepCount; ++i) {
    const core::cycle::Step step = static_cast<core::cycle::Step>(i);
    CHECK_MESSAGE(doc.find(core::cycle::name(step)) != std::string::npos,
                  std::string(core::cycle::name(step)));
  }
  // Every member of the closed set, by the words this firmware uses for it.
  for (std::uint8_t a = 0; a < static_cast<std::uint8_t>(interfaces::Ask::kCount); ++a) {
    const char* what = interfaces::describe(static_cast<interfaces::Ask>(a));
    CHECK_MESSAGE(doc.find(what) != std::string::npos, std::string(what));
  }
  // And every refusal: "I did not do what you asked, because" is the half a backend cannot work
  // out for itself, so an implementer has to be able to read the list.
  for (std::uint8_t r = 1; r < static_cast<std::uint8_t>(interfaces::Refusal::kCount); ++r) {
    const char* why = interfaces::describe(static_cast<interfaces::Refusal>(r));
    CHECK_MESSAGE(doc.find(why) != std::string::npos, std::string(why));
  }
  // The page for whoever implements the seam, which is the hand-over to the agent work.
  CHECK(doc.find("implementer") != std::string::npos);
  CHECK(doc.find("interfaces/backend.h") != std::string::npos);
}

TEST_CASE("the record's line: the wake, the radio, the band, the letters, the seconds, the interval") {
  WakeBench bench;
  bench.modemWorks();
  const Pass& pass = bench.oneWake();
  const std::string line = lineOf(pass);

  CHECK(line.rfind("wake ", 0) == 0);
  CHECK(line.find("timer") != std::string::npos);
  CHECK(line.find("radio healthy") != std::string::npos);
  CHECK(line.find("band plenty") != std::string::npos);
  CHECK(line.find("steps ") != std::string::npos);
  CHECK(line.find("s awake") != std::string::npos);
  CHECK(line.find("next 15 min") != std::string::npos);
  // One letter per step, in order, and nothing else between them.
  char letters[16] = {};
  core::cycle::stepLetters(pass, letters, sizeof letters);
  CHECK(std::strlen(letters) == core::cycle::kStepCount);
  CHECK(line.find(letters) != std::string::npos);
  // It is one line, and it fits a terminal.
  CHECK(line.find('\n') == std::string::npos);
  CHECK(line.size() <= 120);
}

TEST_CASE("the record names an attention when the radio asked for us") {
  WakeBench bench;
  bench.modemWorks();
  // §8: the radio raises an attention and the line wakes us; the event arrives inside the window.
  bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 10, 0);
  const Pass& pass = bench.oneWake(interfaces::WokeBy::kRadioLine);
  const std::string line = lineOf(pass);
  CHECK(pass.attention == core::link::attention::kPersonAsked);
  CHECK(line.find("attention") != std::string::npos);
  CHECK(line.find("a person asked for the walter") != std::string::npos);
}

TEST_CASE("the record carries no secret, no IMEI, no ICCID and no IMSI") {
  WakeBench bench;
  bench.modemWorks();
  char err[96] = {};
  REQUIRE(bench.v().settings.put(core::settings::def(core::settings::Key::kLtePin), "86421357",
                                 err, sizeof err));
  REQUIRE(bench.v().settings.put(core::settings::def(core::settings::Key::kServerToken),
                                 "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV", err, sizeof err));
  // A month of them, with the line and the journal entry of each.
  std::string everything;
  for (int i = 0; i < 40; ++i) {
    const Pass& pass = bench.oneWake();
    everything += lineOf(pass);
    everything += bench.v().runLine("journal");
  }
  const interfaces::Identity numbers = sim::FakeModem::makeIdentity();
  const char* const forbidden[] = {"86421357", "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV",
                                   numbers.imei, numbers.iccid, numbers.imsi};
  for (const char* secret : forbidden) {
    if (secret[0] == 0) continue;
    CHECK_MESSAGE(everything.find(secret) == std::string::npos, std::string(secret));
  }
}

TEST_CASE("the journal entry: encoded, decoded, and only when the wake has news") {
  WakeBench bench;
  bench.modemWorks();
  // The first wake has news by definition: the band changed from nothing to `plenty`.
  (void)bench.oneWake();
  auto entries = bench.wakeEntries();
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].band == core::power::Band::kPlenty);
  CHECK(entries[0].radio == core::supervisor::Health::kHealthy);
  CHECK(entries[0].wake == core::cycle::Wake::kTimer);
  CHECK(entries[0].interval_s == core::power::kPlentyMinutes * 60u);
  CHECK((entries[0].flags & core::cycle::kWakeFlagSlept) != 0);
  CHECK(entries[0].step[static_cast<std::size_t>(Step::kSleep)] == Outcome::kDone);

  // And then three quiet wakes in the same band, which write **nothing**: the journal is for an
  // update that must not be guessed at, and 96 wake records a day would wrap it in ten days
  // (tasks/phase-3/reports/05.md, *Deviations*).
  const std::size_t before = bench.wakeEntries().size();
  for (int i = 0; i < 3; ++i) {
    const Pass& quiet = bench.oneWake();
    // Each of them *did* set the radio's clock - §5.4 asks for that at every HELLO whose boot_id
    // this end has not seen, and a Walter out of deep sleep has seen none - and that is exactly
    // why setting the clock is not news.
    CHECK(quiet.clock_set);
    CHECK(quiet.ending == core::cycle::Ending::kSlept);
  }
  CHECK(bench.wakeEntries().size() == before);

  // A wake with news again: the modem goes away, so the uplink step fails.
  bench.modemSilent();
  (void)bench.oneWake();
  entries = bench.wakeEntries();
  CHECK(entries.size() == before + 1);
  CHECK(entries[0].step[static_cast<std::size_t>(Step::kUplink)] == Outcome::kFailed);
}

TEST_CASE("the journal prints a wake record in words") {
  WakeBench bench;
  bench.modemWorks();
  (void)bench.oneWake();
  const std::string said = bench.v().runLine("journal");
  CHECK(said.find("wake ") != std::string::npos);
  CHECK(said.find("radio healthy") != std::string::npos);
  CHECK(said.find("band plenty") != std::string::npos);
  CHECK(said.find("steps ") != std::string::npos);
}

TEST_CASE("encode and decode are each other's inverse, and a foreign entry is refused") {
  core::cycle::WakePayload p{};
  p.wake = core::cycle::Wake::kAttention;
  p.band = core::power::Band::kSaving;
  p.radio = core::supervisor::Health::kScanner;
  p.flags = core::cycle::kWakeFlagClimbed | core::cycle::kWakeFlagClockSet;
  for (std::size_t i = 0; i < core::cycle::kStepCount; ++i) {
    p.step[i] = static_cast<Outcome>(i % static_cast<std::size_t>(Outcome::kCount));
  }
  p.asks = interfaces::askBit(interfaces::Ask::kFetchRelease);
  p.carried = interfaces::Ask::kFetchRelease;
  p.awake_s = 123456;
  p.interval_s = 21600;
  p.at_epoch = 1760000000u;
  p.wakes = 4242;

  std::uint8_t bytes[core::cycle::kWakePayloadBytes];
  core::cycle::encode(p, bytes);
  core::update::Entry entry{};
  entry.kind = core::update::kind::kWakeCycle;
  entry.size = core::cycle::kWakePayloadBytes;
  std::memcpy(entry.payload, bytes, sizeof bytes);

  core::cycle::WakePayload back{};
  REQUIRE(core::cycle::decode(entry, &back));
  CHECK(back.wake == p.wake);
  CHECK(back.band == p.band);
  CHECK(back.radio == p.radio);
  CHECK(back.flags == p.flags);
  CHECK(back.asks == p.asks);
  CHECK(back.carried == p.carried);
  CHECK(back.awake_s == p.awake_s);
  CHECK(back.interval_s == p.interval_s);
  CHECK(back.at_epoch == p.at_epoch);
  CHECK(back.wakes == p.wakes);
  for (std::size_t i = 0; i < core::cycle::kStepCount; ++i) CHECK(back.step[i] == p.step[i]);

  // A duty-cycle entry is not a wake entry, and neither is a corrupt one.
  entry.kind = core::update::kind::kDutyCycle;
  CHECK_FALSE(core::cycle::decode(entry, &back));
  entry.kind = core::update::kind::kWakeCycle;
  entry.payload[0] = 200;  // not a Wake
  CHECK_FALSE(core::cycle::decode(entry, &back));
}

// -- the Walter's own attention (task p4-04) ----------------------------------------------------
//
// Not §8's, which is the radio asking for us: these two are the agent's, and they reach a person
// through this line and through `status` (core/cycle/agent_attention.h).

TEST_CASE("the agent's letters are distinct, documented, and in the record's line") {
  const std::string doc = cycleDoc();
  char seen[static_cast<std::size_t>(core::cycle::AgentAttentionKind::kCount)] = {};
  for (std::uint8_t k = 1; k < static_cast<std::uint8_t>(core::cycle::AgentAttentionKind::kCount);
       ++k) {
    const core::cycle::AgentAttentionKind kind = static_cast<core::cycle::AgentAttentionKind>(k);
    const char c = core::cycle::letter(kind);
    CHECK(c != '?');
    for (std::uint8_t j = 1; j < k; ++j) CHECK(seen[j] != c);
    seen[k] = c;
    // No letter of the agent's is a letter of a step's outcome: one line carries both.
    for (std::uint8_t o = 0; o < static_cast<std::uint8_t>(Outcome::kCount); ++o) {
      CHECK(core::cycle::letter(static_cast<Outcome>(o)) != c);
    }
    CHECK_MESSAGE(doc.find(std::string("`") + c + "`") != std::string::npos, std::string(1, c));
    CHECK_MESSAGE(doc.find(core::cycle::describe(kind)) != std::string::npos,
                  std::string(core::cycle::describe(kind)));
  }

  Pass pass;
  CHECK_MESSAGE(lineOf(pass).find(", agent ") == std::string::npos,
                "nothing raised, nothing said");
  pass.agent_attention = core::cycle::agent_attention::kTokenRefused;
  CHECK(lineOf(pass).find(", agent u") != std::string::npos);
  pass.agent_attention = core::cycle::agent_attention::kRebootWanted;
  CHECK(lineOf(pass).find(", agent h") != std::string::npos);
  pass.agent_attention = core::cycle::agent_attention::kAll;
  CHECK(lineOf(pass).find(", agent uh") != std::string::npos);
}

TEST_CASE("the agent's attention crosses a sleep in the duty cycle's record, and one raise is reported once") {
  WakeBench bench;
  bench.modemWorks();
  bench.boot();
  bench.v().begin();

  // Raised by the agent inside this wake - a 401 from the server at step 8 - and carried by this
  // wake's own record, because that is the record written next.
  core::cycle::AgentAttention attention(&bench.duty().reason(), &bench.v().journal);
  attention.raiseTokenRefused();
  CHECK(bench.duty().reason().agentAttention() == core::cycle::agent_attention::kTokenRefused);

  const Pass& first = bench.v().run();
  CHECK(first.agent_attention == core::cycle::agent_attention::kTokenRefused);
  CHECK(lineOf(first).find(", agent u") != std::string::npos);

  // Taken: the record has carried it, so the next wake says nothing - and the raise survived a
  // sleep on the way, because the byte is in the retained record (a boot in between).
  CHECK(bench.duty().reason().agentAttention() == 0u);
  const Pass& second = bench.oneWake();
  CHECK(second.agent_attention == 0u);
  CHECK(lineOf(second).find(", agent ") == std::string::npos);
}

TEST_CASE("a raise before a sleep is in the next wake's record: the byte is retained") {
  WakeBench bench;
  bench.modemWorks();
  {
    // The first wake raises it after its own record was written, which is the case the retained
    // byte is for: nothing of this wake can carry it any more.
    const Pass& first = bench.oneWake();
    CHECK(first.agent_attention == 0u);
    core::cycle::AgentAttention attention(&bench.duty().reason(), &bench.v().journal);
    attention.raiseRebootWanted("the loop asked and did not get one");
  }
  const Pass& second = bench.oneWake();
  CHECK(second.agent_attention == core::cycle::agent_attention::kRebootWanted);
  CHECK(lineOf(second).find(", agent h") != std::string::npos);
}

TEST_CASE("`status` says the Walter's own attention in a sentence, and the journal keeps a refused restart") {
  WakeBench bench;
  bench.modemWorks();
  bench.boot();
  bench.v().begin();
  core::cycle::AgentAttention attention(&bench.duty().reason(), &bench.v().journal);

  CHECK_MESSAGE(bench.v().runLine("status").find("agent: ATTENTION") == std::string::npos,
                "nothing raised, nothing said");

  attention.raiseTokenRefused();
  const std::string with_token = bench.v().runLine("status");
  CHECK(with_token.find("agent: ATTENTION u") != std::string::npos);
  CHECK(with_token.find("set server.token") != std::string::npos);

  attention.raiseRebootWanted("12 server calls unanswered in 95 min, wifi up");
  const std::string both = bench.v().runLine("status");
  CHECK(both.find("agent: ATTENTION uh") != std::string::npos);

  // And the refused restart is in `journal`, where a person reading a week of a mast will find
  // it - a 401 is not journalled, because it is a state a person fixes and not news about work.
  const std::string log = bench.v().runLine("journal");
  CHECK(log.find("agent, the agent asked for a restart for its health") != std::string::npos);
  CHECK(log.find("12 server calls unanswered in 95 min") != std::string::npos);
}
