// What the modem said in the newest failed call, on the unit (task p15-02).
//
// `host_test/core/net/test_call_record.cpp` is the record itself - what it holds, how it is packed,
// how it is printed. This is the other half: the record on a whole Walter, through a wake whose
// call fails the way the unit in service fails one, so that
//
//   * `agent calls` prints it under its list, and the report's `calls` section with it;
//   * it is still there after a sleep, after a restart and after a power cut - one value of the
//     settings store, as the list beside it is;
//   * a call answered 2xx afterwards leaves the kept record the failed one's;
//   * and no byte the server or this board sent is in any line of it (spec/SYSTEM.md S-12).
//
// The shape scripted here is 8 November's: the request goes out whole, the modem answers *nothing
// buffered yet* to the polls, a ring announces the answer, the close arrives, and one command goes
// unanswered for the library's whole 30 seconds (`sim/fake_tls_socket.h`, and
// `tasks/phase-10/FOLLOWUPS.md` for the readings this is built from).
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

// The record's own lines of `agent calls`: everything from the line that names it onwards. The
// lines above it count what *this boot* recorded and wrote, and a boot that recorded nothing is
// what two of the cases below are about.
std::string receivingOf(const std::string& said) {
  const std::size_t at = said.find("  what the modem said");
  return at == std::string::npos ? std::string() : said.substr(at);
}

// The modem of 8 November, on every connection this socket makes.
void theStallFromNowOn(AgentBench& bench) {
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.not_yet_receives = 3;
  socket.not_yet_ms = 50;            // the board's kReceivePollMs
  socket.receive_hangs_ms = 30000;   // the library's command timeout
  socket.receive_state = 2;          // WALTER_MODEM_STATE_TIMEOUT: nothing came back at all
  socket.receive_cme = -1;
  socket.ring_after_ms = 100;
  socket.ring_bytes = 878;           // the length every lost answer had at the server
  socket.close_after_ms = 500;
  // And what the one more ask of task p15-03 then finds: nothing, which is 8 November's own
  // shape - the answer was lost. The rule's own cases are host_test/core/net/test_read_again.cpp.
  socket.second_ask = sim::SecondAsk::kNothing;
}

void theModemBehavesAgain(AgentBench& bench) {
  sim::FakeTlsSocket& socket = bench.world.http.socket;
  socket.not_yet_receives = 0;
  socket.receive_hangs_ms = 0;
  socket.ring_after_ms = 0;
  socket.ring_bytes = 0;
  socket.close_after_ms = 0;
}

}  // namespace

TEST_CASE("agent calls says what the modem said in the newest failed call") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();

  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  // The list is still the list: a call that did not come back 2xx, with nothing answered.
  CHECK(has(said, "did not come back 2xx"));
  CHECK(has(said, "nothing answered, the connection closed before the body was complete"));
  // And under it, what the modem said in it - which nothing of this firmware could say before.
  const std::string record = receivingOf(said);
  REQUIRE_FALSE(record.empty());
  CHECK(has(record, "what the modem said in it"));
  CHECK(has(record, "on socket "));
  CHECK(has(record, "bytes sent"));
  CHECK(has(record, "3 receives answered \"nothing buffered yet\""));
  CHECK(has(record, "a ring: the modem announced 878 bytes"));
  CHECK(has(record, "a receive failed after 30000 ms"));
  CHECK(has(record, "nothing answered, the library gave up on the command"));
  CHECK(has(record, "the other end closed"));
  CHECK(has(record, "asked again after that: the modem held nothing"));

  // Not a byte of what the server or this board sent: the token, the host, the route, the body.
  CHECK_FALSE(has(record, "Bearer"));
  CHECK_FALSE(has(record, bench.v().settings.serverToken()));
  CHECK_FALSE(has(record, "/agent/v1/"));
  CHECK_FALSE(has(record, "{"));
  CHECK_FALSE(has(record, "\"ok\""));

  // One value of the store, under a key of its own.
  CHECK(bench.world.kv.has(core::net::kCallRecordKey));

  // **A sleep.** The next boot reads it back out of the store, with nothing of its own recorded.
  bench.boot();
  bench.v().begin();
  const std::string after_sleep = receivingOf(bench.v().runLine("agent calls"));
  CHECK(after_sleep == record);

  // **A restart**, which is a boot that did not come from sleep.
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware);
  bench.v().begin();
  CHECK(receivingOf(bench.v().runLine("agent calls")) == record);

  // **A power cut**, which takes RTC memory and leaves the settings store - which is why this is
  // not in RTC memory, and RTC memory has no slot left for it either.
  bench.world.powerCut();
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  bench.v().begin();
  CHECK(receivingOf(bench.v().runLine("agent calls")) == record);
}

TEST_CASE("a call answered 2xx after a failed one leaves the kept record the failed one's") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();
  const std::string failed = receivingOf(bench.v().runLine("agent calls"));
  REQUIRE_FALSE(failed.empty());
  const std::string stored_then = bench.world.kv.raw(core::net::kCallRecordKey);
  REQUIRE_FALSE(stored_then.empty());

  // The modem answers again, and a wake goes through.
  theModemBehavesAgain(bench);
  (void)bench.oneWake();
  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  // Nothing of the good wake is in the record: it is the failed call's, to the character.
  CHECK(receivingOf(said) == failed);
  CHECK(has(failed, "a receive failed after 30000 ms"));
  // And the value itself was not rewritten: byte for byte what the failed call left there, so a
  // wake whose calls all came back 2xx spent no flash on this key.
  CHECK(bench.world.kv.raw(core::net::kCallRecordKey) == stored_then);
}

TEST_CASE("the report's `calls` section carries the record, inside the section's bytes") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();
  // The modem answers again, so that the wake which claims the report gets through.
  theModemBehavesAgain(bench);
  bench.server().queue(
      bench::ownJob(28399, agent::kReportType, "{\"sections\":[\"calls\"]}").json());
  (void)bench.oneWake();

  const std::string data = bench.lastData(28399);
  REQUIRE_FALSE(data.empty());
  INFO(data);
  CHECK(has(data, "\"name\":\"calls\""));
  CHECK(has(data, "\"unknown\":[]"));
  // The section is the lines of `agent calls`, the record among them - and whole: a section cut
  // because the result was full is named `cut`, and this one is not.
  CHECK(has(data, "agent calls: "));
  CHECK(has(data, "what the modem said in it"));
  CHECK(has(data, "a ring: the modem announced 878 bytes"));
  CHECK(has(data, "a receive failed after 30000 ms"));
  CHECK(has(data, "\"cut\":false"));
  CHECK_FALSE(has(data, "Bearer"));
}

TEST_CASE("a unit whose calls have all come back 2xx says there is no record yet") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  (void)bench.oneWake();
  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK(has(said, "no call has failed since the list began"));
  CHECK_FALSE(bench.world.kv.has(core::net::kCallRecordKey));
  // `agent calls` stops at the list's own line in that case: there is nothing to read the record
  // against, and a line about a record nobody has is noise.
  CHECK(receivingOf(said).empty());
}

TEST_CASE("forgetting the list forgets the record with it") {
  AgentBench bench(nullptr, false);
  bench.modemWorks();
  theStallFromNowOn(bench);
  (void)bench.oneWake();
  REQUIRE(bench.world.kv.has(core::net::kCallRecordKey));

  bench.half().calls.forget();
  CHECK_FALSE(bench.world.kv.has(core::net::kCallRecordKey));
  const std::string said = bench.v().runLine("agent calls");
  INFO(said);
  CHECK(has(said, "no call has failed since the list began"));
}
