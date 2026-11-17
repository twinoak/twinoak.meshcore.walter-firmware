// A call that worked, beside the one that did not, on a whole Walter (task p15-07).
//
// `host_test/core/net/test_a_call_that_worked.cpp` is the record itself - the two times of every
// receive, what the answer said of itself, and the rail that a record kept by an earlier firmware
// still reads. This is the other half: the two blocks as `agent calls` and the report's `calls`
// section print them, through wakes of the bench's Walter against the scripted meshmaster.dk.
//
// The shape scripted here is the unit's own: the config, the heartbeat and the claim are answered
// and the observations post is not (`tasks/phase-15/FOLLOWUPS.md`). So the latest call that was
// answered is the claim, and the newest that failed is the observations post - which is exactly
// the pair a person is meant to read side by side.
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

// Where the second block of `agent calls` begins: the latest call that was answered, or the line
// saying none has been.
std::size_t answeredAt(const std::string& said) {
  const std::size_t worked = said.find("  what the modem said in the latest call that was answered");
  const std::size_t none = said.find("  no call has been answered in this boot");
  return worked < none ? worked : none;  // npos is the larger of the two
}

// The kept record's block: the failed call's, and nothing of the block under it.
std::string failedBlock(const std::string& said) {
  const std::size_t at = said.find("  what the modem said in it");
  if (at == std::string::npos) return std::string();
  const std::size_t end = answeredAt(said);
  return end == std::string::npos ? said.substr(at) : said.substr(at, end - at);
}

// The block under it: the latest call that was answered.
std::string answeredBlock(const std::string& said) {
  const std::size_t at = answeredAt(said);
  return at == std::string::npos ? std::string() : said.substr(at);
}

// The modem a call that works runs over: the answer in several pieces, a ring handed over while
// they are read, and the close 30 ms after the request went out - the shape the record is read by.
void theModemOfAGoodCall(AgentBench& bench) {
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.piece_bytes = 64;
  socket.piece_ms = 20;
  socket.ring_after_ms = 10;
  socket.ring_bytes = 512;
  socket.close_after_ms = 30;
}

// 16 November's modem, on every connection: the polls, a ring, the close, and one command
// unanswered for the library's 30 seconds.
void theStallFromNowOn(AgentBench& bench) {
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.not_yet_receives = 3;
  socket.not_yet_ms = 50;
  socket.receive_hangs_ms = 30000;
  socket.receive_state = 2;
  socket.receive_cme = -1;
  socket.ring_after_ms = 100;
  socket.ring_bytes = 878;
  socket.close_after_ms = 500;
}

void theModemBehavesAgain(AgentBench& bench) {
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.not_yet_receives = 0;
  socket.receive_hangs_ms = 0;
}

// A wake of the unit's own shape: a job to claim, the observations post refused, and the job's
// result refused with it - so that the claim is the latest call the server answered.
void theUnitsOwnWake(AgentBench& bench) {
  bench.modemWorks();
  bench.server().failRoute("/observations", 503);
  bench.server().failResults(503, 4);
  bench.server().queue(
      bench::ownJob(31001, agent::kReportType, "{\"sections\":[\"calls\"]}").json());
}

}  // namespace

TEST_CASE("agent calls prints the latest call that was answered, with its two times and its answer") {
  AgentBench bench(nullptr, false);
  theUnitsOwnWake(bench);
  theModemOfAGoodCall(bench);
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  const std::string worked = answeredBlock(said);
  REQUIRE_FALSE(worked.empty());
  // Which call it was, and the socket it was on.
  CHECK(has(worked, "what the modem said in the latest call that was answered (claim), on socket "));
  CHECK(has(worked, "it ended as: ok"));
  // The ring the reads are placed against, and a receive with both its times.
  CHECK(has(worked, "a ring: the modem announced 512 bytes"));
  CHECK(has(worked, " ms (asked at +"));
  CHECK(has(worked, "a receive returned "));
  // What the answer said of itself: the status, the framing, the bytes, the `Connection: close`.
  CHECK(has(worked, "the answer: http 200, framed by a length, "));
  CHECK(has(worked, "and it said Connection: close"));
  // And where the close fell, which no number above can say.
  CHECK(has(worked, "the other end's close was handed over "));
}

TEST_CASE("both blocks, each named: the call that failed and the call that worked") {
  AgentBench bench(nullptr, false);
  // A wake of the stall first, so that a record of a failed call is in the store.
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();
  const std::string first = bench.v().runLine("agent calls");
  INFO(first);
  REQUIRE_FALSE(failedBlock(first).empty());
  // Nothing was answered in that wake, and the second block says so rather than nothing.
  CHECK(has(first, "no call has been answered in this boot, so there is nothing to lay beside it"));

  // Then a wake in which nothing fails. The kept record is still the earlier wake's - a call that
  // came back 2xx writes no value - and the block under it is this wake's.
  theModemBehavesAgain(bench);
  theModemOfAGoodCall(bench);
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  const std::string failed = failedBlock(said);
  const std::string worked = answeredBlock(said);
  REQUIRE_FALSE(failed.empty());
  REQUIRE_FALSE(worked.empty());
  // Each is named for what it is, and neither is read as the other.
  CHECK(has(failed, "what the modem said in it (heartbeat)"));
  CHECK(has(failed, "it ended as: the connection closed before the body was complete"));
  CHECK(has(failed, "a receive failed after 30000 ms"));
  CHECK(has(worked, "what the modem said in the latest call that was answered ("));
  CHECK(has(worked, "it ended as: ok"));
  CHECK(has(worked, "the answer: http 200, framed by a length, "));
  CHECK_FALSE(has(worked, "a receive failed"));
  // The kept record says nothing of an answer: those facts are not in the stored bytes, so a
  // record read back out of the store reads as it always did.
  CHECK_FALSE(has(failed, "the answer: http"));
  // The failed block is first, so a reader meets the question before the answer.
  CHECK(said.find(failed) < said.find(worked));
}

TEST_CASE("a boot in which nothing has been answered yet says so") {
  AgentBench bench(nullptr, false);
  bench.boot();
  bench.v().begin();
  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK(has(said, "no call has failed since the list began"));
  CHECK(has(said, "no call has been answered in this boot, so there is nothing to lay beside it"));
  CHECK(answeredBlock(said).find("what the modem said") == std::string::npos);
}

TEST_CASE("after a sleep the answered call is gone and the failed one is as it was") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();
  theModemBehavesAgain(bench);
  theUnitsOwnWake(bench);
  theModemOfAGoodCall(bench);
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  const std::string failed = failedBlock(said);
  REQUIRE_FALSE(answeredBlock(said).empty());
  REQUIRE(has(answeredBlock(said), "the answer: http 200"));

  // **A sleep.** The failed call's record is one value of the settings store and crosses it; the
  // call that worked was never written anywhere and does not.
  bench.boot();
  bench.v().begin();
  const std::string after = bench.v().runLine("agent calls");
  INFO(after);
  CHECK(failedBlock(after) == failed);
  CHECK(has(after, "no call has been answered in this boot, so there is nothing to lay beside it"));
  CHECK(answeredBlock(after).find("the answer: http") == std::string::npos);
}

TEST_CASE("the report's `calls` section carries both blocks, and neither holds a byte that was sent") {
  AgentBench bench(nullptr, false);
  // The stall first, so the report has a failed call to show as well as the claim that worked.
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();
  theModemBehavesAgain(bench);
  bench.modemWorks();
  bench.server().failRoute("/observations", 503);
  theModemOfAGoodCall(bench);
  bench.server().queue(
      bench::ownJob(31002, agent::kReportType, "{\"sections\":[\"calls\"]}").json());
  (void)bench.oneWake();

  const std::string data = bench.lastData(31002);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(has(data, "\"name\":\"calls\""));
  CHECK(has(data, "\"cut\":false"));
  // The report is made in the middle of the wake, straight after the calls that worked - so the
  // claim's record is still in memory and the section carries it.
  CHECK(has(data, "what the modem said in it"));
  CHECK(has(data, "what the modem said in the latest call that was answered (claim)"));
  CHECK(has(data, "the answer: http 200, framed by a length, "));

  // **Nothing the server or this board sent is in either block** (spec/SYSTEM.md S-12): not the
  // token, not the route, not a brace of a body, and no header's value beyond the facts above.
  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  const std::string blocks = failedBlock(said);
  const std::string worked = answeredBlock(said);
  for (const std::string& block : {blocks, worked}) {
    REQUIRE_FALSE(block.empty());
    CHECK_FALSE(has(block, "Bearer"));
    CHECK_FALSE(has(block, bench.v().settings.serverToken()));
    CHECK_FALSE(has(block, "/agent/v1"));
    CHECK_FALSE(has(block, "{"));
    CHECK_FALSE(has(block, "\"jobs\""));
    CHECK_FALSE(has(block, "Server:"));
    CHECK_FALSE(has(block, "Content-Length:"));
  }
}
