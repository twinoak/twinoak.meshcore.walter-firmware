// The HTTPS client (task p3-02) against the scripted server through the firmware's own HTTP/1.1:
// a small GET and a small POST; each status class; Retry-After honoured and capped; a body over
// the bound cut off; redirects - one, three, four (refused), to http:// (refused), to another host
// with an Authorization header (followed without it); a chain that does not end at the host's
// root (refused as a TLS failure); and the token a request carried in no line of the trace.
#include <doctest.h>

#include <string>

#include "core/net/trust.h"
#include "net_bench.h"

using bench::has;
using bench::MemorySink;
using bench::NetBench;
using core::net::Options;
using core::net::Result;
using core::net::Verdict;
using interfaces::HttpOutcome;

TEST_CASE("a small GET: 200, the body, the headers asked for, the trace line") {
  NetBench b;
  b.server().serve("/hello", {'h', 'i', '!'}, "\"v1\"", "Wed, 30 Sep 2026 02:39:56 GMT");
  char etag[32] = {};
  char type[32] = {};
  interfaces::HttpWantedHeader wanted[2] = {};
  wanted[0].name = "ETag";
  wanted[0].value = etag;
  wanted[0].capacity = sizeof etag;
  wanted[1].name = "Content-Type";
  wanted[1].value = type;
  wanted[1].capacity = sizeof type;
  Options o;
  o.wanted = wanted;
  o.wanted_count = 2;
  MemorySink body;
  b.client.setTrace(&b.trace);
  const Result r = b.client.get("https://server.example/hello", o, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(r.status == 200);
  CHECK(r.outcome == HttpOutcome::kOk);
  CHECK(body.text() == "hi!");
  CHECK(r.bytes == 3);
  CHECK(r.has_content_length);
  CHECK(r.content_length == 3);
  CHECK(wanted[0].present);
  CHECK(std::string(etag) == "\"v1\"");
  CHECK(std::string(type) == "application/octet-stream");
  CHECK(r.redirects == 0);
  CHECK(std::string(r.final_host) == "server.example");
  CHECK(has(b.trace.text, "GET server.example/hello -> 200 ok, 3 bytes"));
  // What went over the wire: the firmware's own request head.
  REQUIRE(b.server().requests.size() == 1);
  const sim::SeenRequest& seen = b.server().requests[0];
  CHECK(seen.method == "GET");
  CHECK(seen.path == "/hello");
  CHECK(seen.host == "server.example");
  CHECK(seen.header("Connection") == "close");
  CHECK(has(seen.header("User-Agent"), "twinoak-walter"));
  CHECK_FALSE(seen.has("Authorization"));
  // The connection was held to the host's root, and closed afterwards.
  CHECK(b.socket().last_root == core::net::kRootIsrgX1);
  CHECK(b.socket().closes == 1);
  CHECK(b.socket().isOpen() == false);
}

TEST_CASE("a small POST: the body goes out with its Content-Type and Content-Length") {
  NetBench b;
  b.server().serve("/post", {'o', 'k'});
  const std::string payload = "{\"a\":1}";
  MemorySink body;
  const Result r = b.client.post("https://server.example/post", reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 payload.size(), "application/json", Options{}, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(body.text() == "ok");
  REQUIRE(b.server().requests.size() == 1);
  const sim::SeenRequest& seen = b.server().requests[0];
  CHECK(seen.method == "POST");
  CHECK(seen.header("Content-Type") == "application/json");
  CHECK(seen.header("Content-Length") == "7");
  CHECK(seen.body == payload);
}

TEST_CASE("each status class: 2xx ok, 401 and 403 denied, 429 and 5xx later with Retry-After capped, the rest failed") {
  NetBench b;
  b.server().serve("/ok", {'o'});
  b.server().statuses["/denied"] = sim::StatusRule{401, "", "who are you"};
  b.server().statuses["/forbidden"] = sim::StatusRule{403, "", "no"};
  b.server().statuses["/busy"] = sim::StatusRule{429, "120", "later"};
  b.server().statuses["/down"] = sim::StatusRule{503, "86400", "maintenance"};
  b.server().statuses["/date"] = sim::StatusRule{500, "Wed, 21 Oct 2026 07:28:00 GMT", ""};
  b.server().statuses["/silent"] = sim::StatusRule{502, "", ""};
  b.server().statuses["/teapot"] = sim::StatusRule{418, "", ""};
  MemorySink body;
  CHECK(b.client.get("https://server.example/ok", Options{}, body).verdict == Verdict::kOk);
  Result r = b.client.get("https://server.example/denied", Options{}, body);
  CHECK(r.verdict == Verdict::kDenied);
  CHECK(r.status == 401);
  CHECK(b.client.get("https://server.example/forbidden", Options{}, body).verdict == Verdict::kDenied);
  r = b.client.get("https://server.example/busy", Options{}, body);
  CHECK(r.verdict == Verdict::kLater);
  CHECK(r.status == 429);
  CHECK(r.retry_after_s == 120);
  r = b.client.get("https://server.example/down", Options{}, body);
  CHECK(r.verdict == Verdict::kLater);
  CHECK(r.retry_after_s == core::net::kRetryAfterCapS);  // a day, honoured for an hour
  r = b.client.get("https://server.example/date", Options{}, body);
  CHECK(r.verdict == Verdict::kLater);
  CHECK(r.retry_after_s == core::net::kRetryAfterCapS);  // an HTTP-date: the cap
  r = b.client.get("https://server.example/silent", Options{}, body);
  CHECK(r.verdict == Verdict::kLater);
  CHECK(r.retry_after_s == 0);  // the server did not say
  r = b.client.get("https://server.example/teapot", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.status == 418);
  r = b.client.get("https://server.example/missing", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.status == 404);
  // Nothing was retried by the client itself: one request per call.
  CHECK(b.server().requests.size() == 9);
}

TEST_CASE("a body over the bound is cut off and reported, not buffered") {
  NetBench b;
  b.server().serve("/big", bench::patternBytes(10000));
  Options o;
  o.bound = 1000;
  MemorySink body;
  const Result r = b.client.get("https://server.example/big", o, body);
  CHECK(r.verdict == Verdict::kTooLarge);
  CHECK(r.status == 200);
  CHECK(r.outcome == HttpOutcome::kSinkStopped);
  CHECK(body.bytes.size() == 1000);  // exactly the bound reached the sink
  CHECK(r.has_content_length);
  CHECK(r.content_length == 10000);
  // The connection was closed early: far fewer than 10 000 bytes were read off the socket.
  CHECK(b.socket().bytes_out < 4000);
  CHECK(b.socket().isOpen() == false);
}

TEST_CASE("redirects: one and three followed, four refused, http:// refused, no Location refused, 303 turns a POST into a GET") {
  NetBench b;
  b.server().serve("/final", {'f'});
  b.server().redirects["/one"] = sim::Redirect{302, "https://server.example/final"};
  b.server().redirects["/a"] = sim::Redirect{301, "/b"};
  b.server().redirects["/b"] = sim::Redirect{307, "https://server.example/c"};
  b.server().redirects["/c"] = sim::Redirect{308, "/final"};
  b.server().redirects["/d"] = sim::Redirect{302, "/a"};  // four hops to /final
  b.server().redirects["/insecure"] = sim::Redirect{302, "http://server.example/final"};
  b.server().redirects["/scheme-relative"] = sim::Redirect{302, "//server.example/final"};
  b.server().redirects["/relative"] = sim::Redirect{302, "final"};
  b.server().raw_response = "";
  b.server().statuses["/nowhere"] = sim::StatusRule{302, "", ""};
  b.server().redirects["/see-other"] = sim::Redirect{303, "/final"};
  MemorySink body;
  Result r = b.client.get("https://server.example/one", Options{}, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(r.redirects == 1);
  CHECK(body.text() == "f");

  body.bytes.clear();
  r = b.client.get("https://server.example/a", Options{}, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(r.redirects == 3);
  CHECK(body.text() == "f");

  r = b.client.get("https://server.example/d", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(has(r.why, "more redirects"));
  CHECK(r.redirects == 3);

  r = b.client.get("https://server.example/insecure", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.url_error == core::net::UrlError::kNotHttps);
  CHECK(has(r.why, "other than https"));
  r = b.client.get("https://server.example/scheme-relative", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  r = b.client.get("https://server.example/relative", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  r = b.client.get("https://server.example/nowhere", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(has(r.why, "without a Location"));
  // Nothing of an http:// redirect was requested: the server saw no request to it.
  CHECK(b.requestsTo("/final") == 2);

  // 303 after a POST: a GET without the body.
  const std::uint8_t payload[3] = {1, 2, 3};
  body.bytes.clear();
  r = b.client.post("https://server.example/see-other", payload, 3, "application/octet-stream", Options{}, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(body.text() == "f");
  const sim::SeenRequest& last = b.server().requests.back();
  CHECK(last.method == "GET");
  CHECK_FALSE(last.has("Content-Length"));

  // A request told not to follow sees the 3xx itself.
  Options nofollow;
  nofollow.follow_redirects = false;
  r = b.client.get("https://server.example/one", nofollow, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.status == 302);
}

TEST_CASE("an Authorization header reaches the first host and is never carried to another") {
  NetBench b;
  b.server().hosts.push_back(sim::ScriptedHost{"other.example", core::net::kRootIsrgX1});
  b.server().serve("/final", {'f'});
  b.server().redirects["/same"] = sim::Redirect{302, "https://server.example/final"};
  b.server().redirects["/away"] = sim::Redirect{302, "https://other.example/final"};
  Options o;
  o.authorization = "Bearer s3cr3t-t0ken-4242";
  MemorySink body;
  Result r = b.client.get("https://server.example/same", o, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK_FALSE(r.authorization_dropped);
  REQUIRE(b.server().requests.size() == 2);
  CHECK(b.server().requests[0].header("Authorization") == o.authorization);
  CHECK(b.server().requests[1].header("Authorization") == o.authorization);  // the same host

  r = b.client.get("https://server.example/away", o, body);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(r.authorization_dropped);
  CHECK(std::string(r.final_host) == "other.example");
  REQUIRE(b.server().requests.size() == 4);
  CHECK(b.server().requests[2].header("Authorization") == o.authorization);
  CHECK_FALSE(b.server().requests[3].has("Authorization"));  // another host: nothing
  CHECK(b.server().requests[3].host == "other.example");
}

TEST_CASE("the token a request carried is in no line of the trace") {
  NetBench b;
  b.server().hosts.push_back(sim::ScriptedHost{"other.example", core::net::kRootIsrgX1});
  b.server().serve("/final", {'f'});
  b.server().redirects["/away"] = sim::Redirect{302, "https://other.example/final?sig=public"};
  b.server().statuses["/denied"] = sim::StatusRule{401, "", "no"};
  Options o;
  o.authorization = "Bearer s3cr3t-t0ken-4242";
  MemorySink body;
  b.client.setTrace(&b.trace);
  b.client.get("https://server.example/away", o, body);
  b.client.get("https://server.example/denied", o, body);
  b.client.get("https://server.example/final", o, body);
  CHECK(b.trace.text.find("s3cr3t") == std::string::npos);
  CHECK(b.trace.text.find("Bearer") == std::string::npos);
  CHECK(b.trace.text.find("Authorization") == std::string::npos);
  // ... and the trace did say what happened, so a grep that matches nothing is not what passed.
  CHECK(has(b.trace.text, "-> 302"));
  CHECK(has(b.trace.text, "-> 401"));
  CHECK(has(b.trace.text, "-> 200"));
}

TEST_CASE("a sensitive request's credential is wiped from the transport's head once it is on the wire") {
  // The one copy below the caller's that this firmware can reach is the request head the
  // transport writes (core/net/http_over_tls.h, head_): with `sensitive` the head is zeroed
  // after the send and on every way out; without it (the control) the bytes are still there,
  // because a fetch's requests carry nothing worth the cost.
  NetBench b;
  b.server().serve("/final", {'f'});
  b.server().statuses["/denied"] = sim::StatusRule{401, "", "no"};
  constexpr const char* kSecret = "s3cr3t-t0ken-4242";
  auto transportHolds = [&](const char* needle) {
    const std::string image(reinterpret_cast<const char*>(&b.http.http), sizeof b.http.http);
    return image.find(needle) != std::string::npos;
  };
  auto clientHolds = [&](const char* needle) {
    const std::string image(reinterpret_cast<const char*>(&b.client), sizeof b.client);
    return image.find(needle) != std::string::npos;
  };
  MemorySink body;

  Options control;
  control.authorization = "Bearer s3cr3t-t0ken-4242";
  CHECK(b.client.get("https://server.example/final", control, body).verdict == Verdict::kOk);
  CHECK(transportHolds(kSecret));  // the control: the grep finds what is there

  Options sensitive;
  sensitive.authorization = "Bearer s3cr3t-t0ken-4242";
  sensitive.sensitive = true;
  CHECK(b.client.get("https://server.example/final", sensitive, body).verdict == Verdict::kOk);
  CHECK_FALSE(transportHolds(kSecret));
  CHECK_FALSE(transportHolds("Bearer"));
  CHECK_FALSE(clientHolds(kSecret));
  // On the wire once, as the header: the request itself was whole.
  REQUIRE(b.server().requests.size() == 2);
  CHECK(b.server().requests[1].header("Authorization") == "Bearer s3cr3t-t0ken-4242");

  // Every way out: a refused status, a failed handshake, a request that could not be written.
  CHECK(b.client.get("https://server.example/denied", sensitive, body).verdict == Verdict::kDenied);
  CHECK_FALSE(transportHolds(kSecret));
  b.socket().refuse_tls = true;
  CHECK(b.client.get("https://server.example/final", sensitive, body).verdict == Verdict::kFailed);
  CHECK_FALSE(transportHolds(kSecret));
  b.socket().refuse_tls = false;
  Options unwritable = sensitive;
  interfaces::HttpHeader bad[1] = {{"X-Bad", "line\r\nbreak"}};
  unwritable.headers = bad;
  unwritable.header_count = 1;
  const Result r = b.client.get("https://server.example/final", unwritable, body);
  CHECK(r.outcome == HttpOutcome::kBadRequest);
  CHECK_FALSE(transportHolds(kSecret));
}

TEST_CASE("a URL that is not https is refused before the modem is touched") {
  NetBench b;
  MemorySink body;
  for (const char* url : {"http://server.example/x", "ftp://server.example/x", "server.example/x", ""}) {
    const Result r = b.client.get(url, Options{}, body);
    CHECK_MESSAGE(r.verdict == Verdict::kRefusedUrl, url);
  }
  CHECK(b.socket().opens == 0);
  CHECK(b.http.http.requests() == 0);
}

TEST_CASE("a chain that does not end at the host's root is a TLS failure, and nothing is sent") {
  NetBench b;
  // github.com is held to USERTrust ECC (core/net/trust.h); a server that presents an ISRG chain
  // under that name is somebody else, and the handshake fails.
  b.server().hosts.push_back(sim::ScriptedHost{"github.com", core::net::kRootIsrgX1});
  b.server().serve("/x", {'x'});
  MemorySink body;
  Result r = b.client.get("https://github.com/x", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kTlsFailed);
  CHECK(b.socket().last_root == core::net::kRootUserTrustEcc);
  CHECK(b.socket().tls_failures == 1);
  CHECK(b.server().requests.empty());  // nothing went over the wire
  // The same server under its right root is fine.
  b.server().hosts.back().root = core::net::kRootUserTrustEcc;
  r = b.client.get("https://github.com/x", Options{}, body);
  CHECK(r.verdict == Verdict::kOk);
  // A host nobody answers for: unreachable, not a TLS failure.
  r = b.client.get("https://nobody.example/x", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kConnectFailed);
  // The modem's library cannot tell the two apart; the fake can say so too.
  b.socket().dial_opaque = true;
  r = b.client.get("https://nobody.example/x", Options{}, body);
  CHECK(r.outcome == HttpOutcome::kDialRefused);
}

TEST_CASE("the transport's failures reach the verdict: no uplink, a stall, a drop mid-body, a malformed response") {
  NetBench b;
  b.server().serve("/x", bench::patternBytes(5000));
  MemorySink body;
  b.socket().no_uplink = true;
  Result r = b.client.get("https://server.example/x", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kNoUplink);
  b.socket().no_uplink = false;

  b.socket().stall = true;
  Options quick;
  quick.total_ms = 5000;
  const std::uint64_t t0 = b.clock.nowMs();
  r = b.client.get("https://server.example/x", quick, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kTimeout);
  CHECK(b.clock.nowMs() - t0 <= 5000 + 10);  // the deadline, and not a millisecond longer
  b.socket().stall = false;

  b.socket().drop_at_request = static_cast<int>(b.socket().requests);
  b.socket().drop_after_body_bytes = 1234;
  body.bytes.clear();
  r = b.client.get("https://server.example/x", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kClosed);
  CHECK(body.bytes.size() == 1234);
  b.socket().drop_at_request = -1;

  b.server().raw_response = "HTTP/9.9 what\r\n\r\n";
  r = b.client.get("https://server.example/x", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(r.outcome == HttpOutcome::kMalformed);
}

TEST_CASE("a chunked body and a body with no length both arrive whole, in one-byte pieces too") {
  NetBench b;
  sim::ServedFile chunked;
  chunked.bytes = bench::patternBytes(777);
  chunked.chunked = true;
  chunked.chunk_size = 50;
  b.server().files["/chunked"] = chunked;
  sim::ServedFile until_close;
  until_close.bytes = bench::patternBytes(333, 2);
  until_close.no_length = true;
  b.server().files["/until-close"] = until_close;
  for (std::size_t piece : {1u, 7u, 1500u}) {
    b.socket().piece_bytes = piece;
    MemorySink body;
    Result r = b.client.get("https://server.example/chunked", Options{}, body);
    CHECK(r.verdict == Verdict::kOk);
    CHECK(r.chunked);
    CHECK(body.bytes == chunked.bytes);
    MemorySink body2;
    r = b.client.get("https://server.example/until-close", Options{}, body2);
    CHECK(r.verdict == Verdict::kOk);
    CHECK_FALSE(r.has_content_length);
    CHECK(body2.bytes == until_close.bytes);
  }
}
