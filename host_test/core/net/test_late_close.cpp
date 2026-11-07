// The fault the first unit showed on 6 November 2026, end to end (task p8-03): the second HTTPS
// request of a wake failed because the *first* connection's close arrived after the second had
// been opened. The fake socket is the modem's socket layer now (sim/fake_tls_socket.h):
// `close_event_lateness` is how late the library is with a close, counted in connections, and
// `as_v0_1_0` puts back the rule the released firmware had - one socket, one flag, cleared by
// every open. Every case below is run both ways, so the test fails before the fix and the
// failure is the board's, in the board's words.
#include <doctest.h>

#include <string>

#include "core/net/trust.h"
#include "net_bench.h"


using bench::MemorySink;
using bench::NetBench;
using core::net::Options;
using core::net::Result;
using core::net::Verdict;
using interfaces::HttpOutcome;

namespace {

// Two requests to the server, as a wake's heartbeat and observations are: back to back, each one
// connection, each ending on its Content-Length.
struct TwoRequests {
  Result first;
  Result second;
  std::string first_body;
  std::string second_body;
};

TwoRequests twoRequests(NetBench& b) {
  TwoRequests out;
  MemorySink one;
  out.first = b.client.get("https://server.example/heartbeat", Options{}, one);
  out.first_body = one.text();
  MemorySink two;
  out.second = b.client.get("https://server.example/observations", Options{}, two);
  out.second_body = two.text();
  return out;
}

void serveBoth(NetBench& b) {
  b.server().serve("/heartbeat", {'o', 'k'});
  b.server().serve("/observations", {'t', 'w', 'o'});
  b.server().serve("/claim", {'j', 'o', 'b'});
}

}  // namespace

TEST_CASE("v0.1.0's rule: a close one connection late stops the second request going out") {
  NetBench b;
  serveBoth(b);
  b.socket().as_v0_1_0 = true;
  b.socket().close_event_lateness = 1;

  const TwoRequests r = twoRequests(b);
  // The first request of every wake worked: it ends on its Content-Length and never waits for
  // the close.
  CHECK(r.first.verdict == Verdict::kOk);
  CHECK(r.first.status == 200);
  CHECK(r.first_body == "ok");
  // The second: the late close set the one flag after open() had cleared it, and send() stopped.
  // `/agent/v1/observations -> no reply: the request did not go out after 444 ms`, 6 November.
  CHECK(r.second.verdict == Verdict::kFailed);
  CHECK(r.second.outcome == HttpOutcome::kSendFailed);
  CHECK(std::string(r.second.why) == "the request did not go out");
  CHECK(r.second_body.empty());
}

TEST_CASE("one connection, one socket: both requests go through with a close one connection late") {
  NetBench b;
  serveBoth(b);
  b.socket().close_event_lateness = 1;

  const TwoRequests r = twoRequests(b);
  CHECK(r.first.verdict == Verdict::kOk);
  CHECK(r.first.status == 200);
  CHECK(r.first_body == "ok");
  CHECK(r.second.verdict == Verdict::kOk);
  CHECK(r.second.status == 200);
  CHECK(r.second_body == "two");

  // The two connections were on different sockets, and the late close was discounted rather
  // than believed.
  REQUIRE(b.socket().socket_ids.size() == 2);
  CHECK(b.socket().socket_ids[0] != b.socket().socket_ids[1]);
  CHECK(b.socket().late_closes == 1);
  CHECK(b.socket().rotation().discountedCloses() == 1);
  CHECK(b.socket().rotation().owedWaits() == 0);  // the other id owed nothing: no waiting
}

TEST_CASE("v0.1.0's rule: a close two connections late breaks the third request") {
  NetBench b;
  serveBoth(b);
  b.socket().as_v0_1_0 = true;
  b.socket().close_event_lateness = 2;

  const TwoRequests r = twoRequests(b);
  CHECK(r.first.verdict == Verdict::kOk);
  CHECK(r.second.verdict == Verdict::kOk);  // nothing was due yet
  MemorySink third;
  const Result t = b.client.get("https://server.example/claim", Options{}, third);
  CHECK(t.verdict == Verdict::kFailed);
  CHECK(t.outcome == HttpOutcome::kSendFailed);
  CHECK(std::string(t.why) == "the request did not go out");
  CHECK(third.text().empty());
}

TEST_CASE("one connection, one socket: a close two connections late is waited for, then discounted") {
  NetBench b;
  serveBoth(b);
  b.socket().close_event_lateness = 2;

  const TwoRequests r = twoRequests(b);
  CHECK(r.first.verdict == Verdict::kOk);
  CHECK(r.second.verdict == Verdict::kOk);
  MemorySink third;
  const Result t = b.client.get("https://server.example/claim", Options{}, third);
  CHECK(t.verdict == Verdict::kOk);
  CHECK(t.status == 200);
  CHECK(third.text() == "job");

  // The third connection is the first id coming round again with its close still in the air: it
  // waited the bounded time, gave up and discounted the close when it came.
  REQUIRE(b.socket().socket_ids.size() == 3);
  CHECK(b.socket().socket_ids[0] == b.socket().socket_ids[2]);
  CHECK(b.socket().socket_ids[0] != b.socket().socket_ids[1]);
  CHECK(b.socket().rotation().owedWaits() == 1);
  CHECK(b.socket().rotation().owedTimeouts() == 1);
  CHECK(b.socket().rotation().discountedCloses() == 1);
}

TEST_CASE("a wake's worth of requests, with every close a connection late") {
  NetBench b;
  b.server().serve("/config", {'c'});
  b.server().serve("/heartbeat", {'h'});
  b.server().serve("/claim", {'j'});
  b.server().serve("/observations", {'o'});
  b.socket().close_event_lateness = 1;

  const char* const paths[4] = {"/config", "/heartbeat", "/claim", "/observations"};
  for (int i = 0; i < 4; ++i) {
    MemorySink body;
    const std::string url = std::string("https://server.example") + paths[i];
    const Result r = b.client.get(url.c_str(), Options{}, body);
    CHECK_MESSAGE(r.verdict == Verdict::kOk, paths[i]);
    CHECK_MESSAGE(r.status == 200, paths[i]);
    CHECK(body.bytes.size() == 1);
  }
  // Four connections, alternating, and no connection ever told it was closed by mistake.
  REQUIRE(b.socket().socket_ids.size() == 4);
  CHECK(b.socket().socket_ids[0] == b.socket().socket_ids[2]);
  CHECK(b.socket().socket_ids[1] == b.socket().socket_ids[3]);
  CHECK(b.socket().socket_ids[0] != b.socket().socket_ids[1]);
  CHECK(b.socket().rotation().discountedCloses() == 3);
  CHECK(b.socket().rotation().owedWaits() == 0);
}

TEST_CASE("one socket id and nothing else: the debt rule alone carries a wake") {
  // What the modem layer falls back to when the library will not hand out a second socket
  // (components/modem/walter_tls.h): every connection on the one id, the owed close waited for.
  NetBench b;
  serveBoth(b);
  b.socket().rotation().useIds(sim::kFirstSocketId, 0);
  b.socket().close_event_lateness = 1;

  const TwoRequests r = twoRequests(b);
  CHECK(r.first.verdict == Verdict::kOk);
  CHECK(r.second.verdict == Verdict::kOk);
  CHECK(r.second_body == "two");
  REQUIRE(b.socket().socket_ids.size() == 2);
  CHECK(b.socket().socket_ids[0] == b.socket().socket_ids[1]);
  CHECK(b.socket().rotation().owedWaits() == 1);
  CHECK(b.socket().rotation().owedTimeouts() == 1);
  CHECK(b.socket().rotation().discountedCloses() == 1);
}

TEST_CASE("a genuine early close is still an early close, with the rotation in play") {
  NetBench b;
  b.server().serve("/big", bench::patternBytes(4000));
  // The connection drops after 500 bytes of the body of the first request.
  b.socket().drop_at_request = 0;
  b.socket().drop_after_body_bytes = 500;

  MemorySink body;
  const Result r = b.client.get("https://server.example/big", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kClosed);
  CHECK(std::string(r.why) == "the connection closed before the body was complete");
  CHECK(body.bytes.size() == 500);
  // It was this connection's own close, so nothing was discounted and nothing is owed.
  CHECK(b.socket().rotation().discountedCloses() == 0);
  CHECK_FALSE(b.socket().rotation().owes(b.socket().socket_ids[0]));

  // And the retry, on the other id, gets the whole file.
  MemorySink again;
  const Result second = b.client.get("https://server.example/big", Options{}, again);
  CHECK(second.verdict == Verdict::kOk);
  CHECK(again.bytes.size() == 4000);
  CHECK(b.socket().socket_ids[0] != b.socket().socket_ids[1]);
}

TEST_CASE("a drop in the middle is reported even when every other close is late") {
  NetBench b;
  b.server().serve("/heartbeat", {'o', 'k'});
  b.server().serve("/big", bench::patternBytes(4000));
  b.socket().close_event_lateness = 2;
  // The second request of this socket's life drops half way through its body.
  b.socket().drop_at_request = 1;
  b.socket().drop_after_body_bytes = 200;

  MemorySink first;
  CHECK(b.client.get("https://server.example/heartbeat", Options{}, first).verdict == Verdict::kOk);
  MemorySink body;
  const Result r = b.client.get("https://server.example/big", Options{}, body);
  CHECK(r.outcome == HttpOutcome::kClosed);
  CHECK(body.bytes.size() == 200);
}
