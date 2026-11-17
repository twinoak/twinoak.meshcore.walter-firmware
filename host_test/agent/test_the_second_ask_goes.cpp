// What a unit in service sees when the second ask goes (task p15-05).
//
// `host_test/core/net/test_no_second_ask.cpp` is the socket's half: a receive that failed with the
// close seen ends the call after one command and no second. This is the other half - a whole
// Walter, and the two things a person who takes this release will notice:
//
//   * **the record of task p15-02 is untouched.** A unit that ran v0.1.8 has a record in its
//     store with a second-ask moment in it, written by the firmware of task p15-03. That moment
//     has to go on printing - it is the only reading anybody has of what the modem answered the
//     ask - so it is decoded and printed here out of **the stored hex**, which is what a unit
//     carries across the update;
//   * **the count goes with the rule.** `agent calls` no longer prints the *asked again* line,
//     and the key those two totals were kept under is erased from the store, once.
//
// The fixtures below are **built by hand to that firmware's format** (core/net/call_record.h, the
// packed layout): 79 bytes, 158 characters of hex, a version byte, the call and how it ended, the
// socket, the bytes sent, the *not yet* count, and then six bytes a moment. **No byte of one was
// read off a unit** - this is a shape, not a reading, and nothing is to be concluded from the
// numbers in it. The shape is the unit's: the two rings and the close are the ones
// `tasks/phase-15/FOLLOWUPS.md` records for wake 254 of the night of 17 November, the receive is
// given up after the library's 30 seconds, and the ask that follows it came back `+CME ERROR: 4`,
// which is what both records read that night showed. The times of the last two are made up to sit
// where they would.
#include <doctest.h>

#include <string>

#include "agent/agent_bench.h"
#include "core/net/call_record.h"
#include "walter_agent/call_log.h"

using bench::AgentBench;

namespace {

bool has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// A record of the firmware of task p15-03, as that firmware wrote it to `agent_call_rx`:
// observations, ended *closed*, socket 1, 308 bytes sent, 3 receives answered *nothing buffered
// yet*; a ring of 815 bytes at +320 ms and one of 5 at +339, the close at +359, a receive that
// failed at +30310 ms after 30 000 ms with nothing answered, and the one more ask at the same
// moment - which failed too, with `+CME ERROR: 4`.
constexpr const char* kRecordOf0_1_8 =
    "018301340103004001032f0300530103050000670104000000667622307500667625000005"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

// The same, with the ask answering *the modem held nothing*: the other thing that firmware could
// write down, and the other line this build still has to print.
constexpr const char* kRecordWithNothingFound =
    "018301340104009600036e0300840304000000c67522307500c67505000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

// The list beside it, under `agent_calls`: one entry, the same call - observations, nothing
// answered, the connection closed before the body was complete, wake 254. `agent calls` prints
// the record under the list, so a unit's two values are seeded together as a unit carries them.
constexpr const char* kListOf0_1_8 =
    "01830000fe00e06e1a690000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000";

// A value of the shape task p15-03 kept its two totals in: a version byte and two 32-bit figures,
// little-endian - 35 asks and 0 answers, which is what the unit's report said on the morning of
// 17 November and what settled the question.
constexpr const char* kTotalsOf0_1_8 = "012300000000000000";

// The record's own lines of `agent calls`: everything from the line that names it onwards.
std::string receivingOf(const std::string& said) {
  const std::size_t at = said.find("  what the modem said in it");
  if (at == std::string::npos) return std::string();
  // Not the block under it: the latest call that was answered is task p15-07's, and no boot of
  // this file's cases has one.
  const std::size_t end = said.find("  no call has been answered in this boot");
  return end == std::string::npos ? said.substr(at) : said.substr(at, end - at);
}

}  // namespace

TEST_CASE("a record kept by the firmware of task p15-03 still prints its second ask") {
  AgentBench bench(nullptr, false);
  // Into the store before the boot, as the update finds it: the key, the value, and nothing else
  // of that firmware.
  bench.world.kv.put(agent::kCallsKey, kListOf0_1_8);
  bench.world.kv.put(core::net::kCallRecordKey, kRecordOf0_1_8);
  bench.boot();
  bench.v().begin();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  const std::string record = receivingOf(said);
  REQUIRE_FALSE(record.empty());
  // Every line of it, as that firmware printed them - the ask included.
  CHECK(has(record, "what the modem said in it (observations), on socket 1: 308 bytes sent"));
  CHECK(has(record, "3 receives answered \"nothing buffered yet\""));
  CHECK(has(record, "it ended as: the connection closed before the body was complete"));
  CHECK(has(record, "+320 ms: a ring: the modem announced 815 bytes"));
  CHECK(has(record, "+339 ms: a ring: the modem announced 5 bytes"));
  CHECK(has(record, "+359 ms: the other end closed"));
  // With when it was asked, which task p15-07 prints from the duration this record already
  // carried: +30310 less 30000.
  CHECK(has(record, "+30310 ms (asked at +310 ms): a receive failed after 30000 ms: nothing answered"));
  CHECK(has(record, "+30310 ms: asked again after that: it failed too, +CME ERROR: 4"));

  // The other answer that firmware could write down, and the other line.
  bench.world.kv.put(core::net::kCallRecordKey, kRecordWithNothingFound);
  bench.boot();
  bench.v().begin();
  const std::string other = receivingOf(bench.v().runLine("agent calls"));
  INFO(other);
  CHECK(has(other, "+30150 ms (asked at +150 ms): a receive failed after 30000 ms: nothing answered"));
  CHECK(has(other, "+30150 ms: asked again after that: the modem held nothing"));
}

TEST_CASE("the two totals are gone: no line, and the key erased once") {
  AgentBench bench(nullptr, false);
  // A unit coming from v0.1.8 has both values. The record is kept, as the test above holds; the
  // totals are not.
  bench.world.kv.put(agent::kCallsKey, kListOf0_1_8);
  bench.world.kv.put(core::net::kCallRecordKey, kRecordOf0_1_8);
  bench.world.kv.put(agent::kRetiredReadAgainKey, kTotalsOf0_1_8);
  REQUIRE(bench.world.kv.has(agent::kRetiredReadAgainKey));

  bench.boot();
  bench.v().begin();

  // **The key is gone**, in the first boot of this firmware and without being asked.
  CHECK_FALSE(bench.world.kv.has(agent::kRetiredReadAgainKey));
  // ... and the record beside it was not touched by that.
  CHECK(bench.world.kv.has(core::net::kCallRecordKey));

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK_FALSE(has(said, "asked again after a failed receive"));
  CHECK_FALSE(has(said, "of them answered"));
  // The list and the record still say everything they said.
  CHECK(has(said, "what the modem said in it (observations)"));
  CHECK(has(said, "asked again after that: it failed too, +CME ERROR: 4"));
}

TEST_CASE("a wake asks nothing again and writes no key of its own") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  // 16 November's modem, on every connection: the polls, a ring, the close, and one command
  // unanswered for the library's 30 seconds.
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.not_yet_receives = 3;
  socket.not_yet_ms = 50;
  socket.receive_hangs_ms = 30000;
  socket.receive_state = 2;
  socket.receive_cme = -1;
  socket.ring_after_ms = 100;
  socket.ring_bytes = 878;
  socket.close_after_ms = 500;
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  // The call failed, the list and the record say so, and nothing of this wake says *asked again*.
  CHECK(has(said, "nothing answered, the connection closed before the body was complete"));
  CHECK(bench.world.kv.has(core::net::kCallRecordKey));
  CHECK_FALSE(has(said, "asked again"));
  CHECK_FALSE(bench.world.kv.has(agent::kRetiredReadAgainKey));
}
