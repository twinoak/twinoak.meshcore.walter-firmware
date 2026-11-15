// The three counts of a flooded transaction, kept (task p14-02).
//
// With task p14-01 the agent library applies its rule on this board: the first flood of a
// transaction goes out in the radio's default scope and the second without it. Whether that
// second try still finds repeaters the first one misses is a number, and the number decides the
// day the try is switched off (meshmaster.dk's D-47). A companion exit node sends it in every
// heartbeat and meshmaster.dk prints *Floods scoped dk; one unscoped retry; answered 5 scoped,
// 0 unscoped, 56 not at all*; this board wrote its own heartbeat and left the object out.
//
// The library's counts are **since boot**, and this board boots at every wake, so what it has to
// send is totals of its own beside them. What this file holds:
//
//   the keeping        `agent::FloodCounters` over a store: a wake's counts added once, kept
//                      across a boot, a power cut, a value of another version and a store that
//                      will not take a write;
//   the object         the heartbeat's `flood` member for a radio of 1.3 with a scope and
//                      without one, held against **meshmaster.dk's own contract fixture**
//                      (`AgentHeartbeatFlood.json`) - and a radio of 1.2 whose body nothing of
//                      this task can reach;
//   the line           `agent state` and the report's `agent` section: the scope and the rule in
//                      the library's own words, and the totals where its line has the counts;
//   the older state    a unit that wakes on this firmware with everything v0.1.6 saved keeps
//                      every counter it had, and its three totals start at 0;
//   the wake that died between its transaction and its save.
//
// The four wakes of a scripted unit that floods - one answered in the scope, one answered only
// without it, one never answered, and the heartbeat after them - are in
// host_test/agent-backend/test_the_three_counts_over_four_wakes.cpp, where the library's own
// simulated stock repeater stands behind the RAK and a flood is a flood.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "agent/agent_bench.h"
#include "fakes/sim_key_value.h"
#include "walter_agent/flood_counters.h"

using bench::AgentBench;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// The hex one value of the store holds, as `FloodCounters::save()` writes it: a version byte and
// the three totals, little-endian. Built here by hand, so that a test can put a unit's history in
// the store before it boots and so that a change of the shape fails here rather than quietly
// reading something else's bytes.
std::string floodHex(std::uint32_t scoped, std::uint32_t unscoped, std::uint32_t none,
                     std::uint8_t version = agent::kFloodTotalsVersion) {
  std::uint8_t bytes[agent::kFloodTotalsBytes] = {};
  bytes[0] = version;
  const std::uint32_t three[3] = {scoped, unscoped, none};
  for (int i = 0; i < 3; ++i) {
    for (int b = 0; b < 4; ++b) {
      bytes[1 + 4 * i + b] = static_cast<std::uint8_t>((three[i] >> (8 * b)) & 0xFFu);
    }
  }
  const char digits[] = "0123456789abcdef";
  std::string hex;
  for (std::uint8_t b : bytes) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }
  return hex;
}

// A section written straight into a string, as `twinoak.report` writes it into the result's
// buffer (test_unit_report.cpp's own).
class StringOut final : public interfaces::TextOut {
 public:
  void write(const char* text, std::size_t length) override { out.append(text, length); }
  bool writeBytes(const std::uint8_t*, std::size_t) override { return false; }
  std::string out;
};

mm::FloodCounts theCounts(std::uint32_t scoped, std::uint32_t unscoped, std::uint32_t none) {
  mm::FloodCounts out;
  out.answeredScoped = scoped;
  out.answeredUnscoped = unscoped;
  out.unanswered = none;
  return out;
}

// One of meshmaster.dk's copied contract fixtures, as test_heartbeat_body.cpp reads them.
std::string readFixture(const char* name) {
  const std::string path = std::string(FIXTURES_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "the copied contract fixture is missing");
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  while (!text.empty()) {
    const char last = text.back();
    if (last != '\n') {
      if (last != '\r') break;
    }
    text.pop_back();
  }
  return text;
}

// The JSON body out of what `agent heartbeat` printed: the line above it says how many bytes it
// is, and the body is the rest.
std::string bodyOf(const std::string& printed) {
  const std::size_t at = printed.find('{');
  REQUIRE(at != std::string::npos);
  std::string body = printed.substr(at);
  while (!body.empty()) {
    const char last = body.back();
    if (last != '\n') break;
    body.pop_back();
  }
  return body;
}

// A unit whose radio speaks link 1.3 with `scope` as its `region default`, on the wake bench with
// no chip (nothing here flashes anything). `speakLink13` is turned before the first HELLO, which
// is what decides the session's version (§5.1), and the HELLO is said here because a session is
// what §6.5 needs - on a board a wake's step 2 opens one.
struct ScopedUnit {
  AgentBench bench{nullptr, false};

  explicit ScopedUnit(const char* scope = "dk") {
    bench.world.rak.speakLink13(scope == nullptr ? "" : scope);
    bench.modemWorks();
    begin();
  }

  // A life of this Walter: the console begun and the radio greeted, as a wake leaves it.
  void begin() {
    bench.v().begin();
    REQUIRE(bench.v().link.hello().ok());
  }
  void reboot() {
    bench.boot(interfaces::WokeBy::kTimer);
    begin();
  }

  std::string heartbeat() { return bodyOf(bench.v().runLine("agent heartbeat")); }
};

}  // namespace

// -- the keeping -------------------------------------------------------------------------------

TEST_CASE("a wake's counts are added to the totals once, and the totals outlive the boot") {
  fakes::SimKeyValue store;
  {
    agent::FloodCounters floods(&store);
    floods.begin();
    CHECK_FALSE(floods.survived());
    CHECK(floods.totals().answered_scoped == 0);
    // A first wake: one transaction answered in the scope. The library's counter is since boot,
    // so what the totals take is the difference - which at the first take is the whole of it.
    floods.take(theCounts(1, 0, 0));
    CHECK(floods.totals().answered_scoped == 1);
    // **Twice in the same wake adds nothing**: `take()` is the difference against what it has
    // already taken, which is what makes the end of a wake a safe place to call it from.
    floods.take(theCounts(1, 0, 0));
    CHECK(floods.totals().answered_scoped == 1);
    // A second transaction in the same boot, answered only without the scope, and a third that
    // nothing answered: the library's counters grew, and only the growth is added.
    floods.take(theCounts(1, 1, 1));
    CHECK(floods.totals().answered_unscoped == 1);
    CHECK(floods.totals().unanswered == 1);
    CHECK(floods.save());
    CHECK(floods.writes().writes == 1);
    // Nothing moved since: nothing is written, which is what an exit node whose repeaters all
    // have paths costs the flash.
    CHECK(floods.save());
    CHECK(floods.writes().writes == 1);
  }
  // The next boot - a new object over the same store, as a life of this bench is - reads them
  // back, and its own `take()` starts from the library's counter at 0 again.
  agent::FloodCounters next(&store);
  next.begin();
  CHECK(next.survived());
  CHECK(next.totals().answered_scoped == 1);
  CHECK(next.totals().answered_unscoped == 1);
  CHECK(next.totals().unanswered == 1);
  next.take(theCounts(2, 0, 0));
  CHECK(next.totals().answered_scoped == 3);
  CHECK(next.save());
  CHECK(store.raw(agent::kFloodTotalsKey) == floodHex(3, 1, 1));
}

TEST_CASE("a power cut, a value of another version, a value of another shape, and no store at all") {
  fakes::SimKeyValue store;
  store.put(agent::kFloodTotalsKey, floodHex(5, 6, 7).c_str());

  SUBCASE("the value as this firmware wrote it") {
    agent::FloodCounters floods(&store);
    floods.begin();
    CHECK(floods.survived());
    CHECK(floods.totals().answered_scoped == 5);
    CHECK(floods.totals().answered_unscoped == 6);
    CHECK(floods.totals().unanswered == 7);
    CHECK(floods.writes().refused == 0);
  }
  SUBCASE("a power cut: the store was never written, and the totals start at 0") {
    store.powerCut();
    agent::FloodCounters floods(&store);
    floods.begin();
    CHECK_FALSE(floods.survived());
    CHECK(floods.totals().answered_scoped == 0);
    // Not a refusal: there was nothing there to refuse.
    CHECK(floods.writes().refused == 0);
  }
  SUBCASE("a version this firmware does not know: refused, and the totals start at 0") {
    store.put(agent::kFloodTotalsKey, floodHex(5, 6, 7, 2).c_str());
    agent::FloodCounters floods(&store);
    floods.begin();
    CHECK_FALSE(floods.survived());
    CHECK(floods.totals().answered_scoped == 0);
    CHECK(floods.writes().refused == 1);
  }
  SUBCASE("a value of another length, and one that is not hex") {
    store.put(agent::kFloodTotalsKey, "01020304");
    agent::FloodCounters shorter(&store);
    shorter.begin();
    CHECK_FALSE(shorter.survived());
    CHECK(shorter.writes().refused == 1);

    std::string bad = floodHex(5, 6, 7);
    bad[3] = 'z';
    store.put(agent::kFloodTotalsKey, bad.c_str());
    agent::FloodCounters not_hex(&store);
    not_hex.begin();
    CHECK_FALSE(not_hex.survived());
    CHECK(not_hex.writes().refused == 1);
  }
  SUBCASE("a store that will not take a write: the totals count for this boot and no further") {
    agent::FloodCounters floods(&store);
    floods.begin();
    REQUIRE(floods.totals().answered_scoped == 5);
    store.readOnly();
    floods.take(theCounts(1, 0, 0));
    CHECK(floods.totals().answered_scoped == 6);
    CHECK_FALSE(floods.save());
    CHECK(floods.writes().writes == 0);
    CHECK(store.raw(agent::kFloodTotalsKey) == floodHex(5, 6, 7));
  }
  SUBCASE("no store: counted for this boot alone, as every total of this firmware is") {
    agent::FloodCounters floods(nullptr);
    floods.begin();
    CHECK_FALSE(floods.survived());
    floods.take(theCounts(1, 2, 3));
    CHECK(floods.totals().answered_unscoped == 2);
    CHECK_FALSE(floods.save());
  }
  SUBCASE("forget: the key erased and the totals back to 0") {
    agent::FloodCounters floods(&store);
    floods.begin();
    REQUIRE(floods.survived());
    floods.forget();
    CHECK(floods.totals().unanswered == 0);
    CHECK_FALSE(store.has(agent::kFloodTotalsKey));
  }
}

// -- the heartbeat's `flood` object ------------------------------------------------------------

TEST_CASE("a radio of 1.3 whose scope is dk: the flood object is the contract fixture's, with the totals") {
  // The fixture's own three numbers, put in the store as a unit's history: what is held then is
  // the **shape** - the member, its place, its order, its spelling - against bytes that are
  // meshmaster.dk's and not ours (`AgentHeartbeatFlood.json`, the copied contract fixture whose
  // heartbeat carries a `flood` object).
  const std::string expected = bench::objectOf(readFixture("AgentHeartbeatFlood.json"), "flood");
  REQUIRE(expected ==
          "{\"scope\":\"dk\",\"unscopedRetry\":true,\"answeredScoped\":41,"
          "\"answeredUnscoped\":3,\"unanswered\":2}");

  ScopedUnit unit;
  unit.bench.world.kv.put(agent::kFloodTotalsKey, floodHex(41, 3, 2).c_str());
  unit.reboot();
  REQUIRE(unit.bench.half().floods.survived());

  const std::string body = unit.heartbeat();
  CHECK_MESSAGE(bench::objectOf(body, "flood") == expected, body);
  REQUIRE(body.size() > expected.size() + 9);
  // And where the library writes it: last, after the two of an agent that paces itself, as the
  // record has it (`AgentFlood` is `AgentHeartbeat`'s last member).
  CHECK(has(body, "\"maxTransactionsThisWake\":"));
  CHECK(body.find("\"flood\":") > body.find("\"maxTransactionsThisWake\":"));
  CHECK(body.substr(body.size() - expected.size() - 9) == "\"flood\":" + expected + "}");
  // The name came off the radio over §6.5, and nothing of §6.5 went on the air.
  CHECK(std::string(unit.bench.half().mesh.floodScope()) == "dk");
  CHECK(unit.bench.world.rak.flood_scope_asks >= 1);
  CHECK(unit.bench.half().mesh.counters().tx == 0);
}

TEST_CASE("a radio of 1.3 with no default scope: the scope is * and there is no unscoped retry") {
  // §6.5: a radio that holds no `region default` floods with no transport code at all, so there
  // is nothing for the library to drop for one send - and the contract asks for `false` there.
  ScopedUnit unit(nullptr);
  const std::string body = unit.heartbeat();
  CHECK_MESSAGE(bench::objectOf(body, "flood") ==
                    "{\"scope\":\"*\",\"unscopedRetry\":false,\"answeredScoped\":0,"
                    "\"answeredUnscoped\":0,\"unanswered\":0}",
                body);
  CHECK(mm::floodScopeIsUnscoped(unit.bench.half().mesh.floodScope()));
}

TEST_CASE("a radio of 1.2: no flood object, and nothing of this task can reach the body") {
  // Every unit in service the day this release lands has a radio of 1.2 (the radio's half is the
  // repeater firmware's pile 3.13), and its heartbeat must be byte for byte what it was. The
  // totals are in the store and a wake has been run, and still: no object, no `scope`, no count.
  AgentBench bench(nullptr, false);
  bench.world.kv.put(agent::kFloodTotalsKey, floodHex(41, 3, 2).c_str());
  bench.modemWorks();
  // The store is read at the `begin()` of a life, so the history goes in before the boot that
  // reads it - which is what a unit updated to this firmware does with what the one before saved.
  bench.boot(interfaces::WokeBy::kTimer);
  bench.v().begin();
  REQUIRE(bench.v().link.hello().ok());
  REQUIRE(bench.half().floods.survived());
  const std::string before = bodyOf(bench.v().runLine("agent heartbeat"));
  CHECK_FALSE(has(before, "flood"));
  CHECK_FALSE(has(before, "scope"));
  CHECK(bench.half().mesh.floodScope() == nullptr);
  // And not one frame of §6.5 was sent to a radio that has no capability for it.
  CHECK(bench.world.rak.flood_scope_asks == 0);

  // The same body with the totals erased: the two are identical, which is the whole claim.
  CHECK(bench.world.kv.erase(agent::kFloodTotalsKey));
  bench.boot(interfaces::WokeBy::kTimer);
  bench.v().begin();
  REQUIRE(bench.v().link.hello().ok());
  const std::string after = bodyOf(bench.v().runLine("agent heartbeat"));
  CHECK(before == after);
}

// -- the line a person reads -------------------------------------------------------------------

TEST_CASE("agent state says the scope, the rule and the three totals, and the report carries the line") {
  ScopedUnit unit;
  unit.bench.world.kv.put(agent::kFloodTotalsKey, floodHex(5, 0, 56).c_str());
  unit.reboot();
  // The scope is read where the key is, which is what a wake and `agent id` both do.
  (void)unit.bench.v().runLine("agent id");

  const std::string state = unit.bench.v().runLine("agent state");
  INFO(state);
  CHECK(has(state, "flood: scope dk, answered scoped 5 / unscoped 0 / none 56 in all"));
  // The rule, in the library's own words (`JobRunner::describeFlood`) and not in ours.
  CHECK(has(state, "the second flood of a transaction goes unscoped"));
  // The totals, and **not** the library's counts since boot, which are what its own line prints:
  // 56 unanswered transactions cannot have happened in this boot.
  CHECK_FALSE(has(state, "since boot"));
  // The line ends: a console line is 200 bytes and a line that truncated would lose its newline
  // with its last words.
  const std::size_t at = state.find("  flood: scope");
  REQUIRE(at != std::string::npos);
  CHECK(state.find('\n', at) != std::string::npos);
  CHECK(state.find('\n', at) - at < 200);

  // And the report's `agent` section is that command (task p13-01), so the line is in it.
  StringOut section;
  REQUIRE(unit.bench.v().console->writeSection("agent", section));
  CHECK(has(section.out, "flood: scope dk, answered scoped 5 / unscoped 0 / none 56 in all"));
}

TEST_CASE("the four readings of the rule the library has, each in the line") {
  SUBCASE("a radio of 1.2: this mesh says nothing about scope") {
    AgentBench bench(nullptr, false);
    bench.modemWorks();
    bench.v().begin();
    REQUIRE(bench.v().link.hello().ok());
    const std::string state = bench.v().runLine("agent state");
    INFO(state);
    CHECK(has(state, "flood: scope none, answered scoped 0 / unscoped 0 / none 0 in all"));
    CHECK(has(state, "says nothing about scope"));
  }
  SUBCASE("a radio of 1.3 with no default scope: every flood goes out with no transport code") {
    ScopedUnit unit(nullptr);
    (void)unit.bench.v().runLine("agent id");
    const std::string state = unit.bench.v().runLine("agent state");
    INFO(state);
    CHECK(has(state, "flood: scope *, "));
    CHECK(has(state, "no transport code"));
  }
}

// -- what an older release left in the store ---------------------------------------------------

TEST_CASE("state saved by v0.1.6 is read: every counter it held is kept, and the totals are 0") {
  // The shape v0.1.6 saved: the library's `WakeState` as hex under `agent_state`
  // (walter_agent/agent_backend.h) and the four wake counters under `agent_wakes`
  // (core/cycle/wake_counters.h). Neither changed in this task - the three totals went under a
  // key of their own - so a unit that wakes on this firmware reads both exactly as it did and its
  // three totals begin at 0, which is what they are.
  mm::WakeState saved;
  saved.pollIntervalS = 1800;
  saved.counters.heartbeats = 512;
  saved.counters.httpFailures = 37;
  saved.counters.batches = 9;
  saved.counters.observationsPosted = 140;
  saved.everHeartbeated = true;
  saved.lastHeartbeatAtEpoch = 1792000000u;
  const auto* at = reinterpret_cast<const std::uint8_t*>(&saved);
  const char digits[] = "0123456789abcdef";
  std::string hex;
  for (std::size_t i = 0; i < sizeof saved; ++i) {
    hex += digits[at[i] >> 4];
    hex += digits[at[i] & 0x0F];
  }

  ScopedUnit unit;
  unit.bench.world.kv.put(agent::kStateKey, hex.c_str());
  REQUIRE_FALSE(unit.bench.world.kv.has(agent::kFloodTotalsKey));
  unit.reboot();

  // Every counter it held, kept.
  CHECK(unit.bench.half().backend.stateSurvived());
  const mm::WakeState& now = unit.bench.half().loop.state();
  CHECK(now.counters.heartbeats == 512);
  CHECK(now.counters.httpFailures == 37);
  CHECK(now.counters.batches == 9);
  CHECK(now.counters.observationsPosted == 140);
  CHECK(now.pollIntervalS == 1800);
  CHECK(now.everHeartbeated);
  // And the three totals start at 0: there is nothing in the store that says otherwise, and the
  // heartbeat says so too.
  CHECK_FALSE(unit.bench.half().floods.survived());
  CHECK(unit.bench.half().floods.totals().answered_scoped == 0);
  const std::string body = unit.heartbeat();
  CHECK_MESSAGE(bench::objectOf(body, "flood") ==
                    "{\"scope\":\"dk\",\"unscopedRetry\":true,\"answeredScoped\":0,"
                    "\"answeredUnscoped\":0,\"unanswered\":0}",
                body);
}

// -- the wake that died before it could save ---------------------------------------------------

TEST_CASE("a wake that dies between its transaction and its save loses that wake's counts, and counts none twice") {
  // The store is the only thing that crosses a boot. A wake that never reached `save()` leaves it
  // holding what it held before that wake - so at most one wake's counts are lost - and the boot
  // after it starts with the library's counter at 0 and with nothing taken, so no transaction can
  // be counted a second time. Here the store is held read-only for the wake that dies, which is
  // the one thing a test can see of a wake that ends in a reset: nothing reached the store.
  fakes::SimKeyValue store;
  store.put(agent::kFloodTotalsKey, floodHex(4, 1, 9).c_str());
  {
    agent::FloodCounters dying(&store);
    dying.begin();
    REQUIRE(dying.totals().answered_scoped == 4);
    store.readOnly();                 // the flash this wake will never reach
    dying.take(theCounts(1, 0, 0));   // a flooded transaction, answered in the scope
    CHECK(dying.totals().answered_scoped == 5);
    CHECK_FALSE(dying.save());
  }
  store.readOnly(false);
  agent::FloodCounters after(&store);
  after.begin();
  CHECK(after.survived());
  CHECK(after.totals().answered_scoped == 4);  // what they were before the wake that died
  CHECK(after.totals().answered_unscoped == 1);
  CHECK(after.totals().unanswered == 9);
  // The transaction of the dead wake is gone and is not counted again: this boot's library
  // counter is its own, and the first thing it counts is the first thing this boot adds.
  after.take(theCounts(1, 0, 0));
  CHECK(after.totals().answered_scoped == 5);
  CHECK(after.save());
  CHECK(store.raw(agent::kFloodTotalsKey) == floodHex(5, 1, 9));
}
