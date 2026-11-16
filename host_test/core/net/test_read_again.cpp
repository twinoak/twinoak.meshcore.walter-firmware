// One more ask before a call is given up (task p15-03).
//
// The fault: at nearly every wake of the unit in service the observations call fails. The request
// goes out whole and meshmaster.dk answers `200` in under a second; one `AT+SQNSRECV` then goes
// unanswered for the modem library's whole 30-second command timeout, and because the server's
// close has usually been handed over by then - the answer comes under `Connection: close` - the
// socket layer read the pair as *the other end hung up* and the call was over. **The modem had
// never been asked a second time.**
//
// `core/net/read_again.h` is the rule that asks it once more, with no modem in it. These tests
// drive the rule through the firmware's own HTTP/1.1 and client over the bench's socket
// (`sim/fake_tls_socket.h`), which asks the same rule object the board's socket asks and whose
// `second_ask` knob is what the modem is scripted to answer:
//
//   * the modem still holds the answer: the call is answered, the body is whole, the count is one;
//   * it holds nothing: *closed*, as before this rule, after one ask and no more;
//   * the ask fails too: *closed*, and no third;
//   * no close seen when the receive failed: `kSocketFailed`, and no ask at all;
//   * a response already half read: the second ask's bytes complete it;
//   * and the record of task p15-02 says the ask was made and what it returned.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/net/call_record.h"
#include "core/net/read_again.h"
#include "fakes/capture.h"
#include "fakes/sim_key_value.h"
#include "net_bench.h"

using bench::MemorySink;
using bench::NetBench;
using core::net::AfterFailure;
using core::net::CallMoment;
using core::net::CallRecord;
using core::net::CallStep;
using core::net::Options;
using core::net::ReadAgain;
using core::net::ReadAgainFound;
using core::net::Result;
using interfaces::HttpOutcome;
using sim::SecondAsk;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// The library's own command timeout, which is how long the one failing command lasts.
constexpr std::uint32_t kLibraryTimeoutMs = 30000;

// The answer meshmaster.dk really sent and this firmware really lost: 878 bytes
// (tasks/phase-10/FOLLOWUPS.md).
constexpr std::size_t kAnswerBytes = 878;

// 16 November's shape, with one knob for what the one more ask then finds. `close_after_ms` is
// inside the hang, which is the board's order: the ring and the close are handed over by the
// library's event task while the receive command is still out.
void theStall(NetBench& b, SecondAsk second, bool the_close_is_seen = true) {
  b.server().serve("/agent/v1/observations", std::vector<std::uint8_t>(kAnswerBytes, 'x'));
  b.socket().not_yet_receives = 3;
  b.socket().not_yet_ms = 50;  // the board's kReceivePollMs
  b.socket().receive_hangs_ms = kLibraryTimeoutMs;
  b.socket().receive_state = 2;  // WALTER_MODEM_STATE_TIMEOUT: nothing answered at all
  b.socket().receive_cme = -1;
  b.socket().ring_after_ms = 120;
  b.socket().ring_bytes = static_cast<std::uint16_t>(kAnswerBytes);
  if (the_close_is_seen) {
    b.socket().close_after_ms = 900;
  } else {
    // The other half of the fault: the close is never handed over at all, which the library's
    // eight-deep event queue really does (core/net/socket_rotation.h) and which a close the
    // rotation discounted as an older connection's debt looks exactly like from here.
    b.socket().close_after_ms = 0;
    b.socket().close_event_lost = true;
  }
  b.socket().second_ask = second;
}

Result postIt(NetBench& b) {
  const std::string sent = "{\"observations\":[{\"id\":\"advert-of-a-repeater\"}]}";
  MemorySink sink;
  Options options;
  options.authorization = "Bearer a-token-nobody-may-print";
  options.sensitive = true;
  const Result r = b.client.post("https://server.example/agent/v1/observations",
                                 reinterpret_cast<const std::uint8_t*>(sent.data()), sent.size(),
                                 "application/json", options, sink);
  return r;
}

// The one more ask in the record, or a kNothing moment.
CallStep theAsk(const CallRecord& record) {
  for (std::size_t i = 0; i < record.steps(); ++i) {
    if (record.step(i).what == CallMoment::kReadAgain) return record.step(i);
  }
  return CallStep();
}

std::size_t countOf(const CallRecord& record, CallMoment what) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < record.steps(); ++i) {
    if (record.step(i).what == what) ++n;
  }
  return n;
}

std::string wordsOf(const CallRecord& record) {
  fakes::CaptureOut out;
  record.describe(out, "observations");
  return out.text;
}

std::string wordsOf(const ReadAgain& rule) {
  fakes::CaptureOut out;
  rule.describe(out);
  return out.text;
}

}  // namespace

// ---------------------------------------------------------------------------------- the rule itself

TEST_CASE("the rule is three answers and nothing else") {
  // No close seen: the socket itself failed, whatever has been asked before. Nothing of this
  // moved in task p15-03, and that is deliberate (the task's second deliverable).
  CHECK(core::net::after(false, false) == AfterFailure::kFailed);
  CHECK(core::net::after(false, true) == AfterFailure::kFailed);
  // The close seen: one ask, and then *closed*.
  CHECK(core::net::after(true, false) == AfterFailure::kAskAgain);
  CHECK(core::net::after(true, true) == AfterFailure::kClosed);
}

TEST_CASE("one ask per call: spent by the first failure, and given back by the next call") {
  ReadAgain rule;
  CHECK_FALSE(rule.spent());
  CHECK(rule.asks() == 0);

  // A failure with no close seen spends nothing: the ask is still there for a later failure of
  // the same call, which is the one that may have an answer behind it.
  CHECK(rule.onFailedReceive(false) == AfterFailure::kFailed);
  CHECK_FALSE(rule.spent());
  CHECK(rule.asks() == 0);

  CHECK(rule.onFailedReceive(true) == AfterFailure::kAskAgain);
  CHECK(rule.spent());
  CHECK(rule.asks() == 1);
  // ... and no second ask in this call, however many more receives fail.
  CHECK(rule.onFailedReceive(true) == AfterFailure::kClosed);
  CHECK(rule.onFailedReceive(true) == AfterFailure::kClosed);
  CHECK(rule.asks() == 1);

  // Only bytes count as an answer.
  rule.found(ReadAgainFound::kNothing);
  rule.found(ReadAgainFound::kFailed);
  CHECK(rule.answers() == 0);
  rule.found(ReadAgainFound::kBytes);
  CHECK(rule.answers() == 1);

  // The next call has its own ask, and the totals are the unit's.
  rule.newCall();
  CHECK_FALSE(rule.spent());
  CHECK(rule.asks() == 1);
  CHECK(rule.answers() == 1);
}

// ------------------------------------------------------------- the call, through the real transport

TEST_CASE("the modem still held the answer: the call is answered, the body is whole, the count is one") {
  NetBench b;
  theStall(b, SecondAsk::kHeld);
  const Result r = postIt(b);

  // The whole cure for the data: the call ends well. It is still thirty seconds late.
  CHECK(r.status == 200);
  CHECK(r.outcome == HttpOutcome::kOk);
  CHECK(r.bytes == kAnswerBytes);
  CHECK(r.elapsed_ms >= kLibraryTimeoutMs);

  const CallRecord& record = b.http.call_record;
  CHECK(record.again().asks() == 1);
  CHECK(record.again().answers() == 1);

  // And the record says the ask was made and what it came back with (the task's fourth
  // deliverable, and the first thing a person reads on a unit with no cable).
  const CallStep ask = theAsk(record);
  REQUIRE(ask.what == CallMoment::kReadAgain);
  CHECK(ask.state == static_cast<std::uint8_t>(ReadAgainFound::kBytes));
  CHECK(ask.value > 0);
  CHECK(ask.at_ms >= kLibraryTimeoutMs);
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "a receive failed after 30000 ms"));
  CHECK(has(said, "asked again after that: the modem still held "));
  CHECK(has(said, "it ended as: ok"));
  // Not a byte of the answer, the token or the body is in those lines (S-12), as for the record.
  CHECK_FALSE(has(said, "a-token-nobody-may-print"));
  CHECK_FALSE(has(said, "xxx"));
}

TEST_CASE("nothing at the second ask: closed, as before the rule, after one ask and no more") {
  NetBench b;
  theStall(b, SecondAsk::kNothing);
  const Result r = postIt(b);

  // The sentence of 8 November, unchanged: the connection closed before the body was complete.
  CHECK(r.status == 0);
  CHECK(r.outcome == HttpOutcome::kClosed);
  CHECK(has(std::string(interfaces::describe(r.outcome)), "closed before the body was complete"));

  const CallRecord& record = b.http.call_record;
  // One ask, and no answer by it. **And no second ask**: the record holds exactly one.
  CHECK(record.again().asks() == 1);
  CHECK(record.again().answers() == 0);
  CHECK(countOf(record, CallMoment::kReadAgain) == 1);
  const CallStep ask = theAsk(record);
  REQUIRE(ask.what == CallMoment::kReadAgain);
  CHECK(ask.state == static_cast<std::uint8_t>(ReadAgainFound::kNothing));
  CHECK(ask.value == 0);
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "asked again after that: the modem held nothing"));
  CHECK(has(said, "it ended as: the connection closed before the body was complete"));
}

TEST_CASE("the second ask failing too: closed, and no third") {
  NetBench b;
  theStall(b, SecondAsk::kFailed);
  b.socket().receive_cme = 4;  // the code the modem answers before the first byte is there
  const Result r = postIt(b);

  CHECK(r.status == 0);
  CHECK(r.outcome == HttpOutcome::kClosed);

  const CallRecord& record = b.http.call_record;
  CHECK(record.again().asks() == 1);
  CHECK(record.again().answers() == 0);
  CHECK(countOf(record, CallMoment::kReadAgain) == 1);
  const CallStep ask = theAsk(record);
  REQUIRE(ask.what == CallMoment::kReadAgain);
  CHECK(ask.state == static_cast<std::uint8_t>(ReadAgainFound::kFailed));
  CHECK(ask.had_cme);
  CHECK(ask.cme == 4);
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "asked again after that: it failed too, +CME ERROR: 4"));
}

TEST_CASE("a receive that failed with no close seen: the socket failed, and nothing was asked again") {
  NetBench b;
  // The same stall, with the close never handed over. This is the half of the fault task p9-02
  // told apart from a close on purpose, and the rule leaves it exactly where it was.
  theStall(b, SecondAsk::kHeld, false);
  const Result r = postIt(b);

  CHECK(r.status == 0);
  CHECK(r.outcome == HttpOutcome::kSocketFailed);

  const CallRecord& record = b.http.call_record;
  CHECK(record.again().asks() == 0);
  CHECK(record.again().answers() == 0);
  CHECK(countOf(record, CallMoment::kReadAgain) == 0);
  CHECK(countOf(record, CallMoment::kClose) == 0);
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK_FALSE(has(said, "asked again"));
  CHECK(has(said, "it ended as: the modem stopped answering for this socket"));
}

TEST_CASE("a response half read when the receive fails: the second ask's bytes complete it") {
  NetBench b;
  theStall(b, SecondAsk::kHeld);
  // Pieces of 200 bytes, and the command fails once 200 of them have been handed over: the
  // parser has the status line and the headers and part of the body when the modem stops
  // answering, and what the one more ask brings is the rest.
  b.socket().piece_bytes = 200;
  b.socket().hang_after_bytes = 200;
  b.socket().not_yet_receives = 1;
  const Result r = postIt(b);

  CHECK(r.status == 200);
  CHECK(r.outcome == HttpOutcome::kOk);
  CHECK(r.bytes == kAnswerBytes);

  const CallRecord& record = b.http.call_record;
  CHECK(record.again().asks() == 1);
  CHECK(record.again().answers() == 1);
  // Bytes came back before the failure as well as after it, which is what *half read* means.
  CHECK(countOf(record, CallMoment::kData) >= 1);
  const CallStep ask = theAsk(record);
  REQUIRE(ask.what == CallMoment::kReadAgain);
  CHECK(ask.value == 200);
}

TEST_CASE("a call the modem answers normally asks nothing again and writes nothing down") {
  NetBench b;
  b.server().serve("/agent/v1/heartbeat", {'o', 'k'});
  MemorySink sink;
  const Result r = b.client.get("https://server.example/agent/v1/heartbeat", Options{}, sink);
  CHECK(r.status == 200);

  const CallRecord& record = b.http.call_record;
  CHECK(record.again().asks() == 0);
  CHECK_FALSE(record.again().moved());
  CHECK(countOf(record, CallMoment::kReadAgain) == 0);
  // A close seen with nothing failed is the end of the response, as it was before the rule.
  CHECK(has(wordsOf(record), "it ended as: ok"));
}

// -------------------------------------------------------------------------------- kept over a sleep

TEST_CASE("the two totals round-trip through one value of the store, and another shape is refused") {
  fakes::SimKeyValue kv;
  ReadAgain made;
  made.begin(&kv);
  CHECK_FALSE(made.survived());
  // Nothing moved, nothing is written: a unit whose calls behave spends no flash on this key.
  CHECK(made.save(&kv));
  CHECK(kv.writes == 0);
  CHECK_FALSE(kv.has(core::net::kReadAgainKey));

  CHECK(made.onFailedReceive(true) == AfterFailure::kAskAgain);
  made.found(ReadAgainFound::kBytes);
  made.newCall();
  CHECK(made.onFailedReceive(true) == AfterFailure::kAskAgain);
  made.found(ReadAgainFound::kNothing);
  CHECK(made.moved());
  CHECK(made.save(&kv));
  CHECK(kv.writes == 1);
  // ... and a second save with nothing new writes nothing.
  CHECK(made.save(&kv));
  CHECK(kv.writes == 1);

  ReadAgain read;
  read.begin(&kv);
  CHECK(read.survived());
  CHECK(read.asks() == 2);
  CHECK(read.answers() == 1);
  CHECK(wordsOf(read) == wordsOf(made));
  CHECK(has(wordsOf(read), "asked again after a failed receive: 2 since this unit was made, "
                           "1 of them answered"));

  // A value of another shape - another firmware's, a flipped character, a version this build
  // does not know - is refused whole and counted, not read as half a figure.
  kv.put(core::net::kReadAgainKey, "not hex at all");
  ReadAgain refused;
  refused.begin(&kv);
  CHECK_FALSE(refused.survived());
  CHECK(refused.refused() == 1);
  CHECK(refused.asks() == 0);
  CHECK(has(wordsOf(refused), "never yet"));

  // And forgotten is forgotten.
  read.forget(&kv);
  CHECK_FALSE(kv.has(core::net::kReadAgainKey));
  CHECK(read.asks() == 0);

  // No store at all: counted for this boot and lost at the sleep, which is what every total of
  // this firmware does without one.
  ReadAgain alone;
  alone.begin(nullptr);
  CHECK(alone.onFailedReceive(true) == AfterFailure::kAskAgain);
  CHECK(alone.asks() == 1);
  CHECK_FALSE(alone.save(nullptr));
}
