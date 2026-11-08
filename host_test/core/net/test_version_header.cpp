// **Who is asking, and what a refusal says** (task p10-05). meshmaster.dk refuses every
// `/agent/v1` request that does not carry `X-Agent-Version` with a 400 that names the header
// (`src/MeshMaster.Web/Agents/AgentV1.cs`, `RequireVersionHeaderAsync`, read at commit
// d7b9b3ea9deb). The agent's calls have always sent it; the release download never did, in any
// version up to v0.1.2, so job 22841 of 8 November 2026 failed in two seconds with *a status this
// firmware does not act on* - a sentence that named neither the 400 nor the manifest.
//
// What is held here: the header on every request the client makes - a GET, a POST, a redirect
// that leaves the host, and a caller that names it itself; the refusal's new sentence for each
// status class, with the server's own problem-details `detail` after it, one printable line, cut
// to fit and **dropped whole when it holds the request's token**; and the fake server's own half,
// which is meshmaster.dk's rule and not a knob of the script (sim/scripted_server.h).
#include <doctest.h>

#include <string>

#include "core/net/trust.h"
#include "core/version.h"
#include "net_bench.h"

using bench::has;
using bench::MemorySink;
using bench::NetBench;
using core::net::Options;
using core::net::Result;
using core::net::Verdict;

TEST_CASE("every request says which build made it: X-Agent-Version, on a GET, a POST and to another host") {
  // The client adds it, so that no path above it can be written that forgets - which is exactly
  // what happened to the download. It goes to every host: it is not a credential, and
  // `User-Agent` has carried the same product and version on every request since task p3-02.
  NetBench b;
  b.server().hosts.push_back(sim::ScriptedHost{"other.example", core::net::kRootIsrgX1});
  b.server().serve("/hello", {'h'});
  b.server().serve("/final", {'f'});
  b.server().redirects["/away"] = sim::Redirect{302, "https://other.example/final"};
  MemorySink body;
  Options o;
  o.authorization = "Bearer s3cr3t-t0ken-4242";
  CHECK(b.client.get("https://server.example/hello", o, body).verdict == Verdict::kOk);
  const std::uint8_t payload[2] = {'{', '}'};
  CHECK(b.client.post("https://server.example/hello", payload, 2, "application/json", o, body).verdict ==
        Verdict::kOk);
  CHECK(b.client.get("https://server.example/away", o, body).verdict == Verdict::kOk);
  REQUIRE(b.server().requests.size() == 4);
  for (const sim::SeenRequest& r : b.server().requests) {
    const std::string which = r.host + r.path;
    CHECK_MESSAGE(r.header("X-Agent-Version") == core::version(), which);
  }
  // The credential stayed behind at the redirect, as before; the version did not.
  const sim::SeenRequest& elsewhere = b.server().requests.back();
  CHECK(elsewhere.host == "other.example");
  CHECK_FALSE(elsewhere.has("Authorization"));
}

TEST_CASE("a caller that names the version header itself keeps its own, and is never given a second") {
  // The agent's uplink names it, from the version its owner gave it
  // (walter_agent/modem_uplink.h), and HTTP compares a header name without case.
  NetBench b;
  b.server().serve("/x", {'x'});
  MemorySink body;
  interfaces::HttpHeader mine[1] = {{"x-agent-version", "9.9.9+caller"}};
  Options o;
  o.headers = mine;
  o.header_count = 1;
  CHECK(b.client.get("https://server.example/x", o, body).verdict == Verdict::kOk);
  REQUIRE(b.server().requests.size() == 1);
  CHECK(b.server().requests[0].header("x-agent-version") == "9.9.9+caller");

  // An empty one is *not given* at the server (`AgentV1.VersionOf`), so the client's own goes.
  interfaces::HttpHeader empty[1] = {{"X-Agent-Version", ""}};
  o.headers = empty;
  CHECK(b.client.get("https://server.example/x", o, body).verdict == Verdict::kOk);
  REQUIRE(b.server().requests.size() == 2);
  CHECK(b.server().requests[1].header("x-agent-version") == core::version());
}

TEST_CASE("a refused request names its status and its file, and what the server said about it") {
  NetBench b;
  const std::string problem =
      "{\"type\":\"bad-request\",\"title\":\"Request not accepted\",\"status\":400,"
      "\"detail\":\"Every request carries X-Agent-Version: the agent's build.\"}";
  b.server().statuses["/r/manifest.json"] = sim::StatusRule{400, "", problem};
  b.server().statuses["/r/missing.json"] = sim::StatusRule{404, "", "not here"};
  b.server().statuses["/r/files/image.bin"] = sim::StatusRule{416, "", ""};
  b.server().statuses["/r/teapot"] = sim::StatusRule{418, "", "{\"detail\":\"I am a teapot\"}"};
  b.server().statuses["/r/no"] = sim::StatusRule{403, "", "{\"detail\":\"This exit node is paused.\"}"};
  b.server().statuses["/r/wait"] = sim::StatusRule{429, "30", "{\"detail\":\"Slow down.\"}"};
  b.server().statuses["/r/ill"] = sim::StatusRule{503, "", "{\"detail\":\"Deploying.\"}"};
  b.server().statuses["/r/query.json"] = sim::StatusRule{400, "", "no details at all"};
  b.server().serve("/r/ok", {'o'});
  MemorySink body;

  // The sentence job 22841 should have come back with.
  Result r = b.client.get("https://server.example/r/manifest.json", Options{}, body);
  CHECK(r.verdict == Verdict::kFailed);
  CHECK(std::string(r.why) ==
        "http 400 for manifest.json: Every request carries X-Agent-Version: the agent's build.");
  r = b.client.get("https://server.example/r/missing.json", Options{}, body);
  CHECK(std::string(r.why) == "http 404 for missing.json");
  r = b.client.get("https://server.example/r/files/image.bin", Options{}, body);
  CHECK(std::string(r.why) == "http 416 for image.bin");
  r = b.client.get("https://server.example/r/teapot", Options{}, body);
  CHECK(std::string(r.why) == "http 418 for teapot: I am a teapot");

  // The clauses this firmware has of its own come first, the server's `detail` after them.
  r = b.client.get("https://server.example/r/no", Options{}, body);
  CHECK(r.verdict == Verdict::kDenied);
  CHECK(std::string(r.why) == "http 403 for no: the server refused this unit; This exit node is paused.");
  r = b.client.get("https://server.example/r/wait", Options{}, body);
  CHECK(r.verdict == Verdict::kLater);
  CHECK(r.retry_after_s == 30);
  CHECK(std::string(r.why) == "http 429 for wait: too many requests; later; Slow down.");
  r = b.client.get("https://server.example/r/ill", Options{}, body);
  CHECK(r.verdict == Verdict::kLater);
  CHECK(std::string(r.why) == "http 503 for ill: the server is not well; later; Deploying.");

  // A query is in no message - the file alone - and a body with no `detail` adds nothing.
  r = b.client.get("https://server.example/r/query.json?product=twinoak-walter", Options{}, body);
  CHECK(std::string(r.why) == "http 400 for query.json");

  // A 2xx and a transport failure are untouched: those sentences were never about a status.
  CHECK(std::string(b.client.get("https://server.example/r/ok", Options{}, body).why) == "ok");
  b.socket().no_uplink = true;
  r = b.client.get("https://server.example/r/manifest.json", Options{}, body);
  CHECK(r.status == 0);
  CHECK(std::string(r.why) == std::string(interfaces::describe(interfaces::HttpOutcome::kNoUplink)));
}

TEST_CASE("a problem detail is one printable line, cut to fit, and dropped whole when it holds the token") {
  NetBench b;
  b.server().statuses["/a"] = sim::StatusRule{400, "", "{\"detail\":\"two\\r\\nlines\\tand a \\u00e6\"}"};
  std::string very_long = "{\"detail\":\"";
  for (int i = 0; i < 40; ++i) very_long += "0123456789";
  very_long += "\"}";
  b.server().statuses["/b"] = sim::StatusRule{400, "", very_long};
  b.server().statuses["/c"] = sim::StatusRule{400, "", "{\"detail\":\"the header was Bearer s3cr3t-t0ken-4242\"}"};
  b.server().statuses["/d"] = sim::StatusRule{400, "", "{\"detail\":\"your token s3cr3t-t0ken-4242 is stale\"}"};
  b.server().statuses["/e"] = sim::StatusRule{400, "", "{\"title\":\"no detail here\"}"};
  MemorySink body;
  Options o;
  o.authorization = "Bearer s3cr3t-t0ken-4242";

  // Nothing a server writes can end a console line or begin one of its own.
  Result r = b.client.get("https://server.example/a", o, body);
  CHECK(std::string(r.why) == "http 400 for a: two  lines and a ?");

  // Longer than the sentence holds: cut where it fits, and no further.
  r = b.client.get("https://server.example/b", o, body);
  const std::string cut(r.why);
  CHECK(cut.size() == core::net::kMaxRefusalLength);
  CHECK(cut.compare(0, 20, "http 400 for b: 0123") == 0);

  // The credential, whole and without its scheme word: the detail goes, the sentence stays.
  for (const char* path : {"/c", "/d"}) {
    r = b.client.get((std::string("https://server.example") + path).c_str(), o, body);
    const std::string why(r.why);
    CHECK_MESSAGE(why == std::string("http 400 for ") + (path + 1), why);
    const bool token_in_it = why.find("s3cr3t") != std::string::npos;
    CHECK_MESSAGE(!token_in_it, why);
  }
  r = b.client.get("https://server.example/e", o, body);
  CHECK(std::string(r.why) == "http 400 for e");

  // And nothing of any of those bodies outlives the sentence inside the client.
  const std::string image(reinterpret_cast<const char*>(&b.client), sizeof b.client);
  CHECK(image.find("s3cr3t") == std::string::npos);
  CHECK(image.find("is stale") == std::string::npos);
}

TEST_CASE("the fake server holds meshmaster.dk's own rule: an /agent/v1 request without the header is its 400") {
  // The rule the download failed on, in the fake, so that this mismatch cannot pass a host test
  // again (sim/scripted_server.h, `agent_version_required`). The two requests below are made at
  // the transport, under the client, because the client cannot be asked to leave the header out.
  NetBench b;
  b.server().serve("/agent/v1/firmware/3/manifest.json", {'m'});
  interfaces::HttpRequest bare;
  bare.host = "server.example";
  bare.path = "/agent/v1/firmware/3/manifest.json";
  bare.root = core::net::rootForHost("server.example");
  interfaces::HttpResponse seen;
  MemorySink refused;
  b.http.request(bare, nullptr, 0, &seen, refused);
  CHECK(seen.status == 400);
  CHECK(b.server().unversioned_refused == 1);
  CHECK(has(refused.text(), "Every request carries X-Agent-Version"));

  // A header longer than `AgentLimits.Version` (64) reads as *not given* there, and here.
  const std::string too_long(65, '9');
  bare.headers[0] = interfaces::HttpHeader{"X-Agent-Version", too_long.c_str()};
  bare.header_count = 1;
  MemorySink also_refused;
  b.http.request(bare, nullptr, 0, &seen, also_refused);
  CHECK(seen.status == 400);
  CHECK(b.server().unversioned_refused == 2);

  // The client's own request carries it, so the same file is served.
  MemorySink got;
  const Result r = b.client.get("https://server.example/agent/v1/firmware/3/manifest.json", Options{}, got);
  CHECK(r.verdict == Verdict::kOk);
  CHECK(got.text() == "m");
  CHECK(b.server().unversioned_refused == 2);
}
