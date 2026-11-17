// A failed receive is the end of the call again (task p15-05).
//
// Task p15-03 asked the modem once more when a receive failed with the connection's close already
// handed over, on the chance that it still held the answer the failed command never brought back.
// The unit in service has had that since the evening of 16 November 2026; on the morning of the
// 17th its report said *asked again after a failed receive: 35 since this unit was made, 0 of them
// answered*, and in both records read so far the second ask came back at once with
// `+CME ERROR: 4`. The bytes are gone by the time the first command gives up.
//
// It was not free either: a second command sent straight after one the modem did not answer can go
// unanswered too, and the first timer wake on that firmware was 68 seconds awake where the others
// are about 40. So the ask goes, and this file is what that means:
//
//   * a receive that failed with the close seen ends the call *closed* - after **one** command,
//     with no second one, even when the script would have answered a whole response to it;
//   * a receive that failed with no close seen is `kSocketFailed`, which never moved.
//
// The record of task p15-02 is untouched by all of it and is `test_call_record.cpp`'s; that a
// record written by the firmware of task p15-03 still prints its ask is
// `host_test/agent/test_the_second_ask_goes.cpp`.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/net/call_record.h"
#include "fakes/capture.h"
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

// The library's own command timeout, which is how long the one failing command lasts - and, since
// this task, the whole of what a failing call costs again.
constexpr std::uint32_t kLibraryTimeoutMs = 30000;

// The answer meshmaster.dk really sent and this firmware really lost: 878 bytes
// (tasks/phase-10/FOLLOWUPS.md).
constexpr std::size_t kAnswerBytes = 878;

// 16 November's shape. The ring and the close are handed over by the library's event task while
// the receive command is still out, which is the board's order; the scripted server is holding
// the whole 878-byte answer the whole time, so **anything that asked the socket a second time
// would be given it** - which is how "no second ask" is proved here rather than asserted.
void theStall(NetBench& b, bool the_close_is_seen = true) {
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
}

Result postIt(NetBench& b) {
  const std::string sent = "{\"observations\":[{\"id\":\"advert-of-a-repeater\"}]}";
  MemorySink sink;
  Options options;
  options.authorization = "Bearer a-token-nobody-may-print";
  options.sensitive = true;
  return b.client.post("https://server.example/agent/v1/observations",
                       reinterpret_cast<const std::uint8_t*>(sent.data()), sent.size(),
                       "application/json", options, sink);
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

}  // namespace

TEST_CASE("a receive that failed with the close seen: closed, after one command and no second") {
  NetBench b;
  theStall(b);
  const Result r = postIt(b);

  // The sentence of 8 November, which is what this firmware said before task p15-03 and says
  // again: the connection closed before the body was complete.
  CHECK(r.status == 0);
  CHECK(r.outcome == HttpOutcome::kClosed);
  CHECK(has(std::string(interfaces::describe(r.outcome)), "closed before the body was complete"));

  // **No second command.** The server's whole answer is sitting in the script and a second ask
  // would have been handed the first piece of it; not a byte of it was delivered, and the call
  // holds no moment of an ask.
  CHECK(b.socket().bytes_out == 0);
  CHECK(r.bytes == 0);
  const CallRecord& record = b.http.call_record;
  CHECK(countOf(record, CallMoment::kReadAgain) == 0);
  CHECK(countOf(record, CallMoment::kReceiveFailed) == 1);
  // ... and the failed receive is the last thing in the record: nothing happened after it.
  REQUIRE(record.steps() > 0);
  const CallStep last = record.step(record.steps() - 1);
  CHECK(last.what == CallMoment::kReceiveFailed);

  // **One command timeout, not two.** This is the cost the task takes back: a call that fails
  // this way spends the library's 30 s once, where between p15-03 and p15-05 a second ask that
  // hung as well could have made it 60.
  CHECK(r.elapsed_ms >= kLibraryTimeoutMs);
  CHECK(r.elapsed_ms < 2 * kLibraryTimeoutMs);

  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "a receive failed after 30000 ms"));
  CHECK(has(said, "it ended as: the connection closed before the body was complete"));
  CHECK_FALSE(has(said, "asked again"));
  // Not a byte of the answer, the token or the body is in those lines (S-12).
  CHECK_FALSE(has(said, "a-token-nobody-may-print"));
  CHECK_FALSE(has(said, "xxx"));
}

TEST_CASE("a receive that failed with no close seen: the socket failed, as it always has") {
  NetBench b;
  // The same stall, with the close never handed over. This is the half of the fault task p9-02
  // told apart from a close on purpose, and nothing of it has moved in this phase.
  theStall(b, false);
  const Result r = postIt(b);

  CHECK(r.status == 0);
  CHECK(r.outcome == HttpOutcome::kSocketFailed);
  CHECK(b.socket().bytes_out == 0);

  const CallRecord& record = b.http.call_record;
  CHECK(countOf(record, CallMoment::kReadAgain) == 0);
  CHECK(countOf(record, CallMoment::kClose) == 0);
  const std::string said = wordsOf(record);
  INFO(said);
  CHECK(has(said, "it ended as: the modem stopped answering for this socket"));
  CHECK_FALSE(has(said, "asked again"));
}

TEST_CASE("a call the modem answers normally is untouched by any of this") {
  NetBench b;
  b.server().serve("/agent/v1/heartbeat", {'o', 'k'});
  MemorySink sink;
  const Result r = b.client.get("https://server.example/agent/v1/heartbeat", Options{}, sink);
  CHECK(r.status == 200);

  const CallRecord& record = b.http.call_record;
  CHECK(countOf(record, CallMoment::kReadAgain) == 0);
  // A close seen with nothing failed is the end of the response, as it has always been.
  CHECK(has(wordsOf(record), "it ended as: ok"));
}
