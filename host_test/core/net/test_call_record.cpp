// What the modem said in one call, on its receiving side (task p15-02).
//
// The fault these tests are about: at nearly every wake of the unit in service the observations
// call fails, the request having gone out whole and been stored by meshmaster.dk, the call lasting
// the modem library's 30-second command timeout, and nothing this firmware writes down saying what
// the modem did in those thirty seconds. `core/net/call_record.h` is that record, and the shape
// here is the 8 November shape: hundreds of asks answered *nothing buffered yet*, one command that
// goes unanswered, and a ring and the close arriving while it hangs.
//
// The asks themselves are the board's own receive loop (`components/modem/walter_tls.cpp`), which
// the host cannot run - so the bench's socket says the same things to the same record, through the
// four knobs `sim/fake_tls_socket.h` describes, and every call here goes through the firmware's own
// HTTP/1.1 and client.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/net/call_record.h"
#include "fakes/capture.h"
#include "fakes/sim_key_value.h"
#include "net_bench.h"

using bench::MemorySink;
using bench::NetBench;
using core::net::CallMoment;
using core::net::CallRecord;
using core::net::CallStep;
using core::net::Options;
using core::net::Result;
using interfaces::HttpOutcome;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// The record's lines, as `agent calls` and the report print them.
std::string wordsOf(const CallRecord& record, const char* what = "observations") {
  fakes::CaptureOut out;
  record.describe(out, what);
  return out.text;
}

// How many moments of this kind are in the record.
std::size_t countOf(const CallRecord& record, CallMoment what) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < record.steps(); ++i) {
    if (record.step(i).what == what) ++n;
  }
  return n;
}

// The first moment of this kind, or a kNothing one.
CallStep firstOf(const CallRecord& record, CallMoment what) {
  for (std::size_t i = 0; i < record.steps(); ++i) {
    if (record.step(i).what == what) return record.step(i);
  }
  return CallStep();
}

// The 8 November observations POST, as near as a host can be to it: a body of a few kB goes out,
// the modem answers *nothing buffered yet* to the polls, the answer the server really sent is
// never handed over, a ring announces 878 bytes and the close arrives while the one command hangs
// for the library's 30 seconds.
constexpr std::uint32_t kLibraryTimeoutMs = 30000;

Result theStall(NetBench& b, std::string* body_out) {
  b.server().serve("/agent/v1/observations", std::vector<std::uint8_t>(878, 'x'));
  b.socket().not_yet_receives = 4;
  b.socket().not_yet_ms = 50;             // the board's kReceivePollMs
  b.socket().receive_hangs_ms = kLibraryTimeoutMs;
  b.socket().receive_state = 2;           // WALTER_MODEM_STATE_TIMEOUT: nothing answered at all
  b.socket().receive_cme = -1;
  b.socket().ring_after_ms = 120;
  b.socket().ring_bytes = 878;
  b.socket().close_after_ms = 900;
  const std::string sent = "{\"observations\":[{\"id\":\"advert-of-a-repeater\"}]}";
  MemorySink sink;
  Options options;
  options.authorization = "Bearer a-token-nobody-may-print";
  options.sensitive = true;
  const Result r = b.client.post("https://server.example/agent/v1/observations",
                                 reinterpret_cast<const std::uint8_t*>(sent.data()), sent.size(),
                                 "application/json", options, sink);
  if (body_out != nullptr) *body_out = sent;
  return r;
}

}  // namespace

TEST_CASE("the 30-second stall, written down: the not-yets, the failed receive, the ring, the close") {
  NetBench b;
  std::string sent;
  const Result r = theStall(b, &sent);

  // What the console said on the board, and says here: the modem stopped answering for this
  // socket before the body was complete (task p9-02 told this apart from a close).
  CHECK(r.status == 0);
  CHECK(r.outcome == HttpOutcome::kSocketFailed);

  const CallRecord& record = b.http.call_record;
  REQUIRE(record.held());

  // Once for the call: the socket it was on, the bytes that went out, and how it ended.
  CHECK(record.socketId() == b.socket().socket_ids.back());
  CHECK(record.socketId() != 0);
  CHECK(record.bytesSent() > sent.size());  // the head and the body
  CHECK(record.outcome() == HttpOutcome::kSocketFailed);

  // The asks that said *nothing buffered yet*: counted, not listed.
  CHECK(record.notYets() == 4);
  CHECK(countOf(record, CallMoment::kData) == 0);

  // The ring, with the length the modem announced and when it came.
  const CallStep ring = firstOf(record, CallMoment::kRing);
  REQUIRE(ring.what == CallMoment::kRing);
  CHECK(ring.value == 878);
  CHECK(ring.at_ms >= 120);
  CHECK(ring.at_ms < 400);

  // The receive that failed, with its duration and with *nothing answered* rather than a code.
  const CallStep failed = firstOf(record, CallMoment::kReceiveFailed);
  REQUIRE(failed.what == CallMoment::kReceiveFailed);
  CHECK_FALSE(failed.had_cme);
  CHECK(failed.state == core::net::kCallStateNoAnswer);
  CHECK(failed.value >= kLibraryTimeoutMs);
  CHECK(failed.at_ms >= kLibraryTimeoutMs);

  // The close, with its time: it arrived *while* the receive was hanging, which is the whole
  // reason the record exists - nothing of this firmware could say so before.
  const CallStep closed = firstOf(record, CallMoment::kClose);
  REQUIRE(closed.what == CallMoment::kClose);
  CHECK(closed.at_ms >= 900);
  CHECK(closed.at_ms < kLibraryTimeoutMs);
  const bool close_before_the_failure = closed.at_ms < failed.at_ms;
  CHECK(close_before_the_failure);

  // And in words, for `agent calls`.
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "what the modem said in it (observations)"));
  CHECK(has(said, "4 receives answered \"nothing buffered yet\""));
  CHECK(has(said, "a ring: the modem announced 878 bytes"));
  CHECK(has(said, "a receive failed after 30"));
  CHECK(has(said, "nothing answered, the library gave up on the command"));
  CHECK(has(said, "the other end closed"));
  CHECK(has(said, "the modem stopped answering for this socket"));
}

TEST_CASE("no byte of a body, a header or a token is in the record or in its lines") {
  NetBench b;
  std::string sent;
  (void)theStall(b, &sent);
  CallRecord& record = b.http.call_record;
  const std::string said = wordsOf(record);
  INFO(said);
  // The token, the host, the path and the body: S-12, and what the whole list beside this one
  // was written to hold to (walter_agent/call_log.h).
  CHECK_FALSE(has(said, "a-token-nobody-may-print"));
  CHECK_FALSE(has(said, "Bearer"));
  CHECK_FALSE(has(said, "advert-of-a-repeater"));
  CHECK_FALSE(has(said, "server.example"));
  CHECK_FALSE(has(said, "observations\":"));
  CHECK_FALSE(has(said, "xxx"));  // the answer's own bytes
  // Not only the lines: the bytes it is stored as carry none of it either.
  fakes::SimKeyValue kv;
  CHECK(record.keep(3, &kv));
  const std::string stored = kv.raw(core::net::kCallRecordKey);
  CHECK(stored.size() == core::net::kCallRecordHexBytes - 1);
  CHECK_FALSE(has(stored, "a-token"));
  for (std::size_t i = 0; i < stored.size(); ++i) {
    const char c = stored[i];
    const bool a_hex_digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    CHECK(a_hex_digit);
  }
}

TEST_CASE("a call with more moments than the record holds: the first dozen and a count of the rest") {
  NetBench b;
  // A body in one-byte pieces, so that a receive returns one byte at a time and the moments run
  // past the dozen the record keeps.
  b.server().serve("/many", std::vector<std::uint8_t>(60, 'a'));
  b.socket().piece_bytes = 1;
  MemorySink sink;
  const Result r = b.client.get("https://server.example/many", Options{}, sink);
  CHECK(r.status == 200);

  const CallRecord& record = b.http.call_record;
  REQUIRE(record.held());
  // Exactly a dozen kept ...
  CHECK(record.steps() == core::net::kCallMoments);
  // ... and the rest counted, up to the cap the nibble holds.
  CHECK(record.dropped() == core::net::kCallDroppedAtMost);
  const std::string said = wordsOf(record, "a download range");
  INFO(said);
  CHECK(has(said, "more there was no room for"));
  // The first dozen and not the last: the beginning of a call is what a stall is read from.
  CHECK(record.step(0).what == CallMoment::kData);
  CHECK(record.step(0).value == 1);
  CHECK(record.step(core::net::kCallMoments - 1).what == CallMoment::kData);
}

TEST_CASE("the record round-trips through one value of the store, and a value of another shape is refused") {
  NetBench b;
  (void)theStall(b, nullptr);
  CallRecord& made = b.http.call_record;

  fakes::SimKeyValue kv;
  REQUIRE(made.keep(3, &kv));
  CHECK(kv.writes == 1);

  CallRecord read;
  CHECK_FALSE(read.held());
  REQUIRE(read.load(&kv));
  CHECK(read.held());
  CHECK(read.callWas() == 3);
  CHECK(read.socketId() == made.socketId());
  CHECK(read.bytesSent() == made.bytesSent());
  CHECK(read.notYets() == made.notYets());
  CHECK(read.dropped() == made.dropped());
  CHECK(read.outcome() == made.outcome());
  REQUIRE(read.steps() == made.steps());
  for (std::size_t i = 0; i < made.steps(); ++i) {
    CHECK(read.step(i).what == made.step(i).what);
    CHECK(read.step(i).at_ms == made.step(i).at_ms);
    CHECK(read.step(i).value == made.step(i).value);
    CHECK(read.step(i).state == made.step(i).state);
    CHECK(read.step(i).had_cme == made.step(i).had_cme);
  }
  CHECK(wordsOf(read) == wordsOf(made));

  // A value of another shape - another firmware's, a flipped character, a version this build does
  // not know - is refused whole rather than read as half a record.
  kv.put(core::net::kCallRecordKey, "not hex at all");
  CallRecord refused;
  CHECK_FALSE(refused.load(&kv));
  CHECK_FALSE(refused.held());
  CHECK(has(wordsOf(refused), "no call has been written down yet"));

  // And forgotten is forgotten.
  CHECK(CallRecord::forget(&kv));
  CallRecord gone;
  CHECK_FALSE(gone.load(&kv));
}

TEST_CASE("a receive the modem answered with a code keeps the code, and a ring with no answer behind it") {
  NetBench b;
  b.server().serve("/agent/v1/heartbeat", {'o', 'k'});
  b.socket().not_yet_receives = 2;
  b.socket().receive_hangs_ms = 180;
  b.socket().receive_state = 1;   // WALTER_MODEM_STATE_ERROR
  b.socket().receive_cme = 4;     // `+CME ERROR: 4`, the one this firmware reads as *not yet*
  MemorySink sink;
  const Result r = b.client.get("https://server.example/agent/v1/heartbeat", Options{}, sink);
  CHECK(r.outcome == HttpOutcome::kSocketFailed);

  const CallRecord& record = b.http.call_record;
  const CallStep failed = firstOf(record, CallMoment::kReceiveFailed);
  REQUIRE(failed.what == CallMoment::kReceiveFailed);
  CHECK(failed.had_cme);
  CHECK(failed.cme == 4);
  CHECK(failed.state == 1);
  CHECK(failed.value >= 180);
  const std::string said = wordsOf(record, "heartbeat");
  INFO(said);
  CHECK(has(said, "+CME ERROR: 4"));
  CHECK_FALSE(has(said, "nothing answered"));

  // `+CME ERROR: 0` is a code of its own (equipment failure), which is why the byte it is kept in
  // is biased by one: a 0 there has to mean *the modem answered no code at all*.
  CallRecord by_hand;
  by_hand.begin();
  by_hand.wentOut(1000, 40);
  by_hand.receiveFailed(1100, 100, 1, 0);
  by_hand.ended(HttpOutcome::kSocketFailed);
  REQUIRE(by_hand.steps() == 1);
  CHECK(by_hand.step(0).had_cme);
  CHECK(by_hand.step(0).cme == 0);
  CHECK(has(wordsOf(by_hand), "+CME ERROR: 0"));
}

TEST_CASE("a call that never got its request out is still the record, with no moment in it") {
  NetBench b;
  // No such host in the script: the dial is refused before a byte goes out.
  MemorySink sink;
  const Result r = b.client.get("https://nowhere.example/agent/v1/config", Options{}, sink);
  CHECK(r.status == 0);

  const CallRecord& record = b.http.call_record;
  // Held, so that the record kept beside `agent calls`'s newest entry is always that call's and
  // never an older call's.
  REQUIRE(record.held());
  CHECK(record.steps() == 0);
  CHECK(record.bytesSent() == 0);
  CHECK(record.notYets() == 0);
  CHECK(record.outcome() == HttpOutcome::kConnectFailed);
  const std::string said = wordsOf(record, "config");
  INFO(said);
  CHECK(has(said, "0 bytes sent"));
  CHECK(has(said, "on no socket"));
  CHECK(has(said, "the modem said nothing at all after that"));
}

TEST_CASE("every figure the record caps says so rather than reading as exact") {
  CallRecord record;
  record.begin();
  record.onSocket(6);
  record.wentOut(0, 200000);                 // a request longer than the field holds
  for (int i = 0; i < 3; ++i) record.notYet();
  record.rang(70000, 900);                   // past 65 535 ms after the request went out
  record.ended(HttpOutcome::kTimeout);
  CHECK(record.bytesSent() == core::net::kCallBytesAtMost);
  REQUIRE(record.steps() == 1);
  CHECK(record.step(0).at_ms == core::net::kCallMomentAtMost);
  const std::string said = wordsOf(record, "a lease");
  INFO(said);
  CHECK(has(said, "65535 or more bytes sent"));
  CHECK(has(said, "+65535 ms or more"));
  CHECK(has(said, "3 receives answered"));
}
