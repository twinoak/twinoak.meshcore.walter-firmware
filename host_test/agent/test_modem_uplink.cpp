// The uplink over the modem (task p4-03): `agent::ModemUplink` - the library's IUplink as the
// seven calls of the agent API over phase 3's HTTPS client - against the scripted server behind
// the firmware's own HTTP/1.1 (sim/fake_http.h), answering with the contract fixtures' bytes
// (host_test/agent-core/tests/fixtures/contracts/, the copy of meshmaster.dk's).
//
// What is held: each call's route, method and headers on the wire, the body equal to what the
// library wrote, the reply parsed by the library's own parser into the record the fixture
// describes; every status the server can answer, as the `Call` record ServerPolicy reads it and
// as the action it then takes; **the token** read back as zeros from every buffer it crossed and
// found in no log line, no error text, no URL and no trace - on the wire exactly once per call;
// a body one byte over the bound; a redirect not followed; and the wake's deadline refusing a
// call it could not finish without touching the modem.
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <agent/Envelopes.h>
#include <agent/Iso8601.h>
#include <agent/Log.h>
#include <agent/ServerPolicy.h>

#include "core/net/client.h"
#include "core/settings/settings.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_wall_clock.h"
#include "fakes/sim_key_value.h"
#include "sim/fake_http.h"
#include "walter_agent/modem_uplink.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";
constexpr const char* kTokenBody = "zQ7x4LpR";
constexpr const char* kVersion = "0.4.0+p4test";
constexpr std::uint32_t kNow = 1790000000;

// A contract fixture, without its trailing newline: the bytes ContractsJson wrote.
std::string contract(const char* name) {
  const fs::path path = fs::path(TWINOAK_REPO_ROOT) / "host_test" / "agent-core" / "tests" / "fixtures" /
                        "contracts" / (std::string(name) + ".json");
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read " << path.string());
  std::ostringstream all;
  all << in.rdbuf();
  std::string text = all.str();
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

std::vector<std::uint8_t> bytesOf(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

// One top-level scalar or string member of a JSON object, without a parser (the fixtures' own
// values, to hold the parsed record to).
std::string member(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t found = json.find(needle);
  if (found == std::string::npos) return "";
  std::size_t at = found + needle.size();
  if (json[at] == '"') {
    const std::size_t end = json.find('"', at + 1);
    return json.substr(at + 1, end - at - 1);
  }
  std::size_t end = at;
  while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
  return json.substr(at, end - at);
}

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

bool holds(const void* object, std::size_t size, const char* needle) {
  const std::string image(static_cast<const char*>(object), size);
  return image.find(needle) != std::string::npos;
}

// Every line the library's logger formatted, captured: the sink is a function pointer
// (agent/Log.h), so the capture is one per process.
std::string g_log;
void captureLog(const char* line, std::size_t len) { g_log.append(line, len); }

struct Rig {
  fakes::FakeClock clock;
  sim::FakeHttp http{clock};
  core::net::Client client{http, clock};
  fakes::CaptureOut trace;
  fakes::SimKeyValue kv;
  core::settings::Settings settings{kv};
  fakes::FakeWallClock wall;
  agent::ModemUplink uplink{client, settings, clock, wall, kVersion};

  explicit Rig(const char* url = "https://server.example", bool withToken = true) {
    kv.put("srv_url", url);
    if (withToken) kv.put("srv_token", kToken);
    settings.begin();
    wall.give(kNow);
    uplink.setLinkUp(true);
    client.setTrace(&trace);
    g_log.clear();
    mm::logSetSink(&captureLog);
    mm::logSetLevel(mm::LogLevel::Debug);
  }
  ~Rig() { mm::logSetSink(nullptr); }

  sim::ScriptedServer& server() { return http.server; }
  sim::FakeTlsSocket& socket() { return http.socket; }
  void serve(const char* path, const std::string& body) { http.server.serve(path, bytesOf(body)); }
  const sim::SeenRequest& last() {
    REQUIRE(!http.server.requests.empty());
    return http.server.requests.back();
  }

  // The rail, after every call: the token is in no buffer of the uplink, the client or the
  // transport, in no log line, in no error text, in no URL and in no trace line; on the wire it
  // is in the Authorization header of every request and nowhere else in the request.
  void tokenNowhere(const char* where) {
    INFO(where);
    CHECK_FALSE(holds(&uplink, sizeof uplink, kTokenBody));
    CHECK_FALSE(holds(&client, sizeof client, kTokenBody));
    CHECK_FALSE(holds(&http.http, sizeof http.http, kTokenBody));
    CHECK_FALSE(has(g_log, kTokenBody));
    CHECK_FALSE(has(trace.text, kTokenBody));
    CHECK_FALSE(has(uplink.lastCall().error, kTokenBody));
    CHECK_FALSE(has(uplink.last().error, kTokenBody));
    for (const sim::SeenRequest& r : http.server.requests) {
      CHECK_FALSE(has(r.path, kTokenBody));
      CHECK_FALSE(has(r.body, kTokenBody));
      CHECK(r.header("Authorization") == std::string("Bearer ") + kToken);
      // Once in the whole request text: the header, and nothing else.
      std::size_t count = 0;
      for (const auto& h : r.headers) count += has(h.second, kTokenBody) ? 1 : 0;
      CHECK(count == 1);
    }
  }

  // What every request carries (docs/library.md §4): the token, the version, the accept.
  void headersOn(const sim::SeenRequest& r, bool post) {
    CHECK(r.header("Authorization") == std::string("Bearer ") + kToken);
    CHECK(r.header("X-Agent-Version") == kVersion);
    CHECK(r.header("Accept") == "application/json");
    CHECK(r.host == "server.example");
    if (post) {
      CHECK(r.method == "POST");
      CHECK(r.header("Content-Type") == "application/json");
    } else {
      CHECK(r.method == "GET");
      CHECK_FALSE(r.has("Content-Type"));
    }
    CHECK(has(r.header("User-Agent"), "twinoak-walter"));
  }
};

}  // namespace

// ---------------------------------------------------------------------------- the seven calls

TEST_CASE("GET /agent/v1/config: the route, the headers, the fixture's reply parsed into the config") {
  Rig rig;
  const std::string fixture = contract("AgentConfig");
  rig.serve("/agent/v1/config", fixture);
  CHECK(rig.uplink.fetchConfig());
  REQUIRE(rig.server().requests.size() == 1);
  const sim::SeenRequest& r = rig.last();
  CHECK(r.path == "/agent/v1/config");
  rig.headersOn(r, false);
  CHECK(r.body.empty());

  CHECK(rig.uplink.hasConfig());
  const mm::AgentConfig& cfg = rig.uplink.config();
  CHECK(std::to_string(cfg.agentId) == member(fixture, "agentId"));
  CHECK(std::string(cfg.label) == member(fixture, "label"));
  CHECK(std::string(cfg.transport) == member(fixture, "transport"));
  CHECK(std::to_string(cfg.pollIntervalS) == member(fixture, "pollIntervalS"));
  CHECK(std::string(cfg.serverTime) == member(fixture, "serverTime"));
  CHECK(cfg.budget.maxTransactionsPerMinute == 2);
  CHECK(cfg.budget.minGapSeconds == 20);

  const mm::IUplink::Call& c = rig.uplink.lastCall();
  CHECK(c.reachable);
  CHECK(c.ok);
  CHECK(c.httpStatus == 200);
  CHECK(c.retryAfterS == 0);
  CHECK(c.rateLimitRemaining == -1);
  CHECK(std::string(c.via) == "lte");
  CHECK(c.atEpoch == kNow);
  CHECK(std::string(c.error).empty());
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::Ok);
  const mm::IUplink::Outcome& o = rig.uplink.last();
  CHECK(o.attempted);
  CHECK(o.ok);
  CHECK(o.httpStatus == 200);
  CHECK(std::string(rig.uplink.lastEndpoint()) == "/agent/v1/config");
  CHECK(rig.uplink.lastReplyBytes() == fixture.size());
  CHECK(has(g_log, "server: /agent/v1/config -> http 200"));
  rig.tokenNowhere("after the config call");
}

TEST_CASE("POST /agent/v1/heartbeat: the body is what the library wrote, the reply parsed") {
  Rig rig;
  const std::string fixture = contract("HeartbeatReply");
  rig.serve("/agent/v1/heartbeat", fixture);
  mm::Heartbeat hb;
  hb.agentVersion = kVersion;
  hb.transport = "native";
  hb.at = kNow;
  hb.board = "walter";
  hb.hasNextCheckInS = true;
  hb.nextCheckInS = 3600;
  char expected[4096];
  const std::size_t expected_len = mm::writeHeartbeat(hb, expected, sizeof expected);
  REQUIRE(expected_len > 0);

  mm::HeartbeatReply reply;
  CHECK(rig.uplink.postHeartbeat(hb, reply));
  const sim::SeenRequest& r = rig.last();
  CHECK(r.path == "/agent/v1/heartbeat");
  rig.headersOn(r, true);
  CHECK_MESSAGE(r.body == std::string(expected, expected_len),
                "on the wire: " << r.body << " | the writer: " << std::string(expected, expected_len));
  CHECK(r.header("Content-Length") == std::to_string(expected_len));

  CHECK(std::to_string(reply.pollIntervalS) == member(fixture, "pollIntervalS"));
  CHECK(reply.jobsWaiting == (member(fixture, "jobsWaiting") == "true"));
  CHECK(std::string(reply.serverTime) == member(fixture, "serverTime"));
  std::uint32_t epoch = 0;
  CHECK(mm::parseUtc(member(fixture, "serverTime").c_str(), epoch));
  CHECK(reply.serverTimeUnix == epoch);
  CHECK(reply.budget.maxTransactionsPerMinute == 2);
  CHECK(reply.budget.minGapSeconds == 20);
  CHECK(rig.uplink.lastCall().ok);
  rig.tokenNowhere("after the heartbeat");
}

TEST_CASE("the heartbeat of a unit whose radio refuses carries radioIdentityLost, and no other does") {
  // Task p10-03. The library builds the heartbeat and writes its six counters whole at every
  // call (agent/AgentLoop.cpp's `doHeartbeat`), so this object is where a counter of the board's
  // can be added at all - and meshmaster.dk's `errors` is an open key space, which is why one
  // counter is enough and no field was added to the envelope.
  Rig rig;
  const std::string fixture = contract("HeartbeatReply");
  rig.serve("/agent/v1/heartbeat", fixture);
  mm::Heartbeat hb;
  hb.agentVersion = kVersion;
  hb.transport = "native";
  hb.at = kNow;
  hb.board = "walter";
  const mm::ErrorCounter six[] = {
      {"overheard", 41},   {"wifiReconnects", 0},       {"httpFailures", 2},
      {"observationsDropped", 0}, {"resultsDropped", 0}, {"reboots", 1},
  };
  hb.errors = six;
  hb.errorCount = 6;
  mm::HeartbeatReply reply;

  SUBCASE("a radio with its key, or one awaiting its first install: the library's six and no more") {
    CHECK_FALSE(rig.uplink.radioIdentityLost());
    REQUIRE(rig.uplink.postHeartbeat(hb, reply));
    const std::string body = rig.last().body;
    CHECK(body.find("\"overheard\":41") != std::string::npos);
    CHECK(body.find("radioIdentityLost") == std::string::npos);
  }

  SUBCASE("a radio that refuses: the seventh counter, behind the library's own") {
    rig.uplink.setRadioIdentityLost(true);
    REQUIRE(rig.uplink.postHeartbeat(hb, reply));
    const std::string body = rig.last().body;
    CHECK(body.find("\"radioIdentityLost\":1") != std::string::npos);
    // Behind them, and nothing of the library's lost: the key space is open and the order is the
    // writer's.
    CHECK(body.find("\"overheard\":41") != std::string::npos);
    CHECK(body.find("\"reboots\":1") != std::string::npos);
    CHECK(body.find("\"reboots\":1") < body.find("radioIdentityLost"));
    // And the heartbeat the caller handed over is untouched: the copy is this object's.
    CHECK(hb.errorCount == 6);
    CHECK(hb.errors == six);

    // The person at the radio types `erase` and `reboot`, the next wake reads a key, and the
    // counter is gone from the body - it is a state and not a tally.
    rig.uplink.setRadioIdentityLost(false);
    REQUIRE(rig.uplink.postHeartbeat(hb, reply));
    CHECK(rig.last().body.find("radioIdentityLost") == std::string::npos);
  }
}

TEST_CASE("POST /agent/v1/observations: the batch's bytes carried unchanged, accepted and duplicate read") {
  Rig rig;
  const std::string batch = contract("ObservationBatch");
  const std::string fixture = contract("ObservationsReply");
  rig.serve("/agent/v1/observations", fixture);
  int accepted = -1;
  bool duplicate = false;
  CHECK(rig.uplink.postObservations(batch.c_str(), batch.size(), accepted, duplicate));
  const sim::SeenRequest& r = rig.last();
  CHECK(r.path == "/agent/v1/observations");
  rig.headersOn(r, true);
  CHECK(r.body == batch);
  CHECK(std::to_string(accepted) == member(fixture, "accepted"));
  CHECK(duplicate == (member(fixture, "duplicate") == "true"));
  rig.tokenNowhere("after the observations");
}

TEST_CASE("POST /agent/v1/jobs/claim: {max} from the library's writer, the claim parsed in place, the reply wiped") {
  Rig rig;
  const std::string request = contract("ClaimRequest");
  const std::string fixture = contract("ClaimResponse");
  rig.serve("/agent/v1/jobs/claim", fixture);
  const int max = std::stoi(member(request, "max"));
  mm::ClaimedJob jobs[4];
  int count = -1;
  CHECK(rig.uplink.claimJobs(jobs, max, count));
  const sim::SeenRequest& r = rig.last();
  CHECK(r.path == "/agent/v1/jobs/claim");
  rig.headersOn(r, true);
  CHECK(r.body == request);
  CHECK(count == 1);
  CHECK(std::to_string(jobs[0].id) == member(fixture, "id"));
  CHECK(std::string(jobs[0].type) == "status");
  CHECK(jobs[0].credentialClass == mm::CredentialClass::Guest);
  CHECK(jobs[0].credential.hasGuestPassword);
  CHECK(std::string(jobs[0].credential.guestPassword) == "guest-pw-sample");
  CHECK(jobs[0].hasPath);
  CHECK(jobs[0].pathCount == 2);
  // The claim carried a password: it is in the job's own credential and in no buffer of the
  // uplink afterwards (the reply is wiped whether or not the parse took).
  CHECK_FALSE(holds(&rig.uplink, sizeof rig.uplink, "guest-pw-sample"));
  CHECK_FALSE(has(g_log, "guest-pw-sample"));
  rig.tokenNowhere("after the claim");
}

TEST_CASE("POST /agent/v1/jobs/{id}/result, /extend and /progress: the per-job routes and their replies") {
  Rig rig;
  const std::string report = contract("JobResultReport");
  rig.serve("/agent/v1/jobs/12345/result", contract("JobResultReply"));
  CHECK(rig.uplink.postJobResult(12345, report.c_str(), report.size()));
  CHECK(rig.last().path == "/agent/v1/jobs/12345/result");
  rig.headersOn(rig.last(), true);
  CHECK(rig.last().body == report);
  rig.tokenNowhere("after the result");

  const std::string extend = contract("ExtendRequest");
  const std::string extendReply = contract("ExtendReply");
  rig.serve("/agent/v1/jobs/12345/extend", extendReply);
  std::uint32_t leaseUntil = 0;
  CHECK(rig.uplink.extendLease(12345, std::stoi(member(extend, "seconds")), leaseUntil));
  CHECK(rig.last().path == "/agent/v1/jobs/12345/extend");
  rig.headersOn(rig.last(), true);
  CHECK(rig.last().body == extend);
  std::uint32_t epoch = 0;
  CHECK(mm::parseUtc(member(extendReply, "leaseUntil").c_str(), epoch));
  CHECK(leaseUntil == epoch);
  rig.tokenNowhere("after the extend");

  const std::string progress = contract("ProgressReport");
  rig.serve("/agent/v1/jobs/12345/progress", contract("ProgressReply"));
  CHECK(rig.uplink.postProgress(12345, progress.c_str(), progress.size()));
  CHECK(rig.last().path == "/agent/v1/jobs/12345/progress");
  rig.headersOn(rig.last(), true);
  CHECK(rig.last().body == progress);
  rig.tokenNowhere("after the progress");

  // Seven routes, three requests here: one request per call, never a retry inside the uplink.
  CHECK(rig.server().requests.size() == 3);
  CHECK(rig.client.requests() == 3);
  CHECK(rig.uplink.uplinkCounters().calls == 3);
  CHECK(rig.uplink.uplinkCounters().ok == 3);
}

// ---------------------------------------------------------------------------- what a status means

TEST_CASE("401: reachable, not ok, the status in the record, and the policy halts") {
  Rig rig;
  rig.server().statuses["/agent/v1/config"] = sim::StatusRule{401, "", "{\"title\":\"Unauthorized\"}"};
  CHECK_FALSE(rig.uplink.fetchConfig());
  const mm::IUplink::Call& c = rig.uplink.lastCall();
  CHECK(c.reachable);
  CHECK_FALSE(c.ok);
  CHECK(c.httpStatus == 401);
  CHECK(has(c.error, "http 401"));
  CHECK(has(c.error, "the token was refused"));
  CHECK_FALSE(has(c.error, "Unauthorized"));  // never a byte of the body
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::Halt);
  CHECK_FALSE(rig.uplink.hasConfig());
  CHECK(rig.uplink.last().httpStatus == 401);
  CHECK(rig.uplink.uplinkCounters().denied == 1);
  rig.tokenNowhere("after a 401");
}

TEST_CASE("429 with Retry-After in seconds and as a date, 503 with and without, 500, 403: the record and the action") {
  Rig rig;
  rig.server().statuses["/agent/v1/jobs/1/result"] = sim::StatusRule{429, "45", "later"};
  rig.server().statuses["/agent/v1/jobs/2/result"] = sim::StatusRule{429, "Wed, 21 Oct 2026 07:28:00 GMT", ""};
  rig.server().statuses["/agent/v1/jobs/3/result"] = sim::StatusRule{503, "120", "deploying"};
  rig.server().statuses["/agent/v1/jobs/4/result"] = sim::StatusRule{503, "", ""};
  rig.server().statuses["/agent/v1/jobs/5/result"] = sim::StatusRule{500, "", "boom"};
  rig.server().statuses["/agent/v1/jobs/6/result"] = sim::StatusRule{403, "", "paused"};
  rig.server().statuses["/agent/v1/jobs/7/result"] = sim::StatusRule{429, "86400", ""};
  const std::string body = "{}";

  CHECK_FALSE(rig.uplink.postJobResult(1, body.c_str(), body.size()));
  mm::IUplink::Call c = rig.uplink.lastCall();
  CHECK(c.reachable);
  CHECK(c.httpStatus == 429);
  CHECK(c.retryAfterS == 45);
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::WaitRetryAfter);
  CHECK(mm::retryAfterMs(c.retryAfterS) == 45000);
  CHECK(has(c.error, "http 429"));

  CHECK_FALSE(rig.uplink.postJobResult(2, body.c_str(), body.size()));
  c = rig.uplink.lastCall();
  CHECK(c.httpStatus == 429);
  CHECK(c.retryAfterS == 0);  // an HTTP date: the library's rule, and retryAfterMs makes it a minute
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::WaitRetryAfter);
  CHECK(mm::retryAfterMs(c.retryAfterS) == mm::kRetryAfterWhenUnstatedMs);

  CHECK_FALSE(rig.uplink.postJobResult(3, body.c_str(), body.size()));
  c = rig.uplink.lastCall();
  CHECK(c.httpStatus == 503);
  CHECK(c.retryAfterS == 120);
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::WaitRetryAfter);

  CHECK_FALSE(rig.uplink.postJobResult(4, body.c_str(), body.size()));
  c = rig.uplink.lastCall();
  CHECK(c.httpStatus == 503);
  CHECK(c.retryAfterS == 0);
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::BackOff);

  CHECK_FALSE(rig.uplink.postJobResult(5, body.c_str(), body.size()));
  c = rig.uplink.lastCall();
  CHECK(c.httpStatus == 500);
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::BackOff);
  CHECK_FALSE(has(c.error, "boom"));

  CHECK_FALSE(rig.uplink.postJobResult(6, body.c_str(), body.size()));
  c = rig.uplink.lastCall();
  CHECK(c.httpStatus == 403);
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::Discard);

  // A day's Retry-After is believed for an hour, where the client bounds its own reading too.
  CHECK_FALSE(rig.uplink.postJobResult(7, body.c_str(), body.size()));
  c = rig.uplink.lastCall();
  CHECK(c.retryAfterS == core::net::kRetryAfterCapS);

  CHECK(rig.server().requests.size() == 7);  // one request per call, no retry of its own
  CHECK(rig.uplink.uplinkCounters().later == 6);
  CHECK(rig.uplink.uplinkCounters().denied == 1);
  rig.tokenNowhere("after the statuses");
}

TEST_CASE("X-RateLimit-Remaining is read into the record; Retry-After on a 200 is not acted on") {
  Rig rig;
  const std::string fixture = contract("AgentConfig");
  rig.server().on_request = [&](const sim::SeenRequest&, sim::ScriptedReply* reply) {
    reply->status = 200;
    reply->headers = {"Content-Type: application/json", "X-RateLimit-Remaining: 119", "X-RateLimit-Limit: 120"};
    reply->body = fixture;
    return true;
  };
  CHECK(rig.uplink.fetchConfig());
  CHECK(rig.uplink.lastCall().rateLimitRemaining == 119);
  CHECK(rig.uplink.lastCall().ok);
  rig.server().on_request = [&](const sim::SeenRequest&, sim::ScriptedReply* reply) {
    reply->status = 200;
    reply->headers = {"X-RateLimit-Remaining: lots"};
    reply->body = fixture;
    return true;
  };
  CHECK(rig.uplink.fetchConfig());
  CHECK(rig.uplink.lastCall().rateLimitRemaining == -1);
}

TEST_CASE("a TLS failure, no route, no uplink: reachable is false, the status not read, the policy backs off") {
  SUBCASE("the chain does not verify") {
    Rig rig;
    rig.serve("/agent/v1/config", contract("AgentConfig"));
    rig.socket().refuse_tls = true;
    CHECK_FALSE(rig.uplink.fetchConfig());
    const mm::IUplink::Call& c = rig.uplink.lastCall();
    CHECK_FALSE(c.reachable);
    CHECK_FALSE(c.ok);
    CHECK(c.httpStatus == 0);
    CHECK(has(c.error, "no reply"));
    CHECK(has(c.error, "TLS handshake failed"));
    CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::BackOff);
    CHECK(rig.socket().tls_failures == 1);
    CHECK(rig.server().requests.empty());
    CHECK(rig.uplink.uplinkCounters().unreachable == 1);
    // Nothing of the token went anywhere: no request was written to the wire, and the buffers
    // are clean all the same.
    CHECK_FALSE(holds(&rig.uplink, sizeof rig.uplink, kTokenBody));
    CHECK_FALSE(holds(&rig.http.http, sizeof rig.http.http, kTokenBody));
    CHECK_FALSE(has(g_log, kTokenBody));
  }
  SUBCASE("no route to the host") {
    Rig rig("https://nowhere.example");
    CHECK_FALSE(rig.uplink.fetchConfig());
    const mm::IUplink::Call& c = rig.uplink.lastCall();
    CHECK_FALSE(c.reachable);
    CHECK(has(c.error, "could not be reached"));
    CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::BackOff);
    CHECK_FALSE(holds(&rig.uplink, sizeof rig.uplink, kTokenBody));
  }
  SUBCASE("the modem is not up") {
    Rig rig;
    rig.socket().no_uplink = true;
    CHECK_FALSE(rig.uplink.fetchConfig());
    CHECK_FALSE(rig.uplink.lastCall().reachable);
    CHECK(has(rig.uplink.lastCall().error, "the uplink is not up"));
  }
}

TEST_CASE("a body one byte over the bound is cut and the call is not ok, with the reason") {
  Rig rig;
  // A config reply of exactly the bound parses (the parser is given what fit); one byte more is
  // cut by the client at the bound and refused here, whatever it would have parsed to.
  std::string padded = contract("AgentConfig");
  padded.pop_back();  // the closing brace, put back after the padding
  padded += ",\"padding\":\"";
  while (padded.size() < agent::kSmallReplyBytes + 1 - 2) padded += 'x';
  padded += "\"}";
  REQUIRE(padded.size() == agent::kSmallReplyBytes + 1);
  rig.serve("/agent/v1/config", padded);
  CHECK_FALSE(rig.uplink.fetchConfig());
  const mm::IUplink::Call& c = rig.uplink.lastCall();
  CHECK(c.reachable);
  CHECK_FALSE(c.ok);
  CHECK(c.httpStatus == 200);
  CHECK(has(c.error, "longer than 2048 bytes and was cut"));
  CHECK_FALSE(rig.uplink.hasConfig());
  CHECK(rig.uplink.uplinkCounters().cut == 1);
  CHECK(rig.socket().isOpen() == false);

  // The same body one byte shorter fits, and parses.
  padded.erase(padded.size() - 3, 1);
  REQUIRE(padded.size() == agent::kSmallReplyBytes);
  rig.serve("/agent/v1/config", padded);
  CHECK(rig.uplink.fetchConfig());
  CHECK(rig.uplink.lastCall().ok);

  // And the claim's bound is the wider one.
  std::string claim = contract("ClaimResponse");
  claim.pop_back();
  claim += ",\"padding\":\"";
  while (claim.size() < agent::kClaimReplyBytes - 2) claim += 'y';
  claim += "\"}";
  REQUIRE(claim.size() == agent::kClaimReplyBytes);
  rig.serve("/agent/v1/jobs/claim", claim);
  mm::ClaimedJob jobs[2];
  int count = 0;
  CHECK(rig.uplink.claimJobs(jobs, 2, count));
  CHECK(count == 1);
  rig.tokenNowhere("after the bound");
}

TEST_CASE("a 200 whose body the library's parser refuses is not ok, with the parser's reason and no byte of the body") {
  Rig rig;
  rig.serve("/agent/v1/config", "{\"agentId\":\"not a number\",\"secret-looking\":\"zzz\"}");
  CHECK_FALSE(rig.uplink.fetchConfig());
  const mm::IUplink::Call& c = rig.uplink.lastCall();
  CHECK(c.reachable);
  CHECK_FALSE(c.ok);
  CHECK(c.httpStatus == 200);
  CHECK(has(c.error, "http 200 but the reply did not parse"));
  CHECK_FALSE(has(c.error, "zzz"));
  CHECK_FALSE(rig.uplink.hasConfig());
  CHECK(has(rig.uplink.last().error, "did not parse"));
  CHECK(rig.uplink.uplinkCounters().unparsed == 1);
  rig.tokenNowhere("after an unparsed reply");
}

TEST_CASE("a redirect on an agent route is not followed: ok is false with the status, one request made") {
  Rig rig;
  rig.server().redirects["/agent/v1/config"] = sim::Redirect{302, "https://elsewhere.example/agent/v1/config"};
  rig.server().hosts.push_back(sim::ScriptedHost{"elsewhere.example", core::net::kRootIsrgX1});
  rig.serve("/agent/v1/config", contract("AgentConfig"));
  CHECK_FALSE(rig.uplink.fetchConfig());
  const mm::IUplink::Call& c = rig.uplink.lastCall();
  CHECK(c.reachable);
  CHECK_FALSE(c.ok);
  CHECK(c.httpStatus == 302);
  CHECK(has(c.error, "a redirect"));
  CHECK(rig.server().requests.size() == 1);
  CHECK(rig.server().requests[0].host == "server.example");
  CHECK(rig.uplink.uplinkCounters().redirected == 1);
  CHECK(mm::actionFor(c.reachable, c.httpStatus, c.retryAfterS) == mm::ServerAction::BackOff);
  rig.tokenNowhere("after a redirect");
}

// ---------------------------------------------------------------------------- the deadline

TEST_CASE("a deadline that cannot be met: reachable is false with *budget*, and the modem is not touched") {
  Rig rig;
  rig.serve("/agent/v1/config", contract("AgentConfig"));
  rig.clock.advance(10000);
  rig.uplink.setDeadline(rig.clock.now_ms + agent::kLeastCallMs - 1);
  CHECK_FALSE(rig.uplink.fetchConfig());
  const mm::IUplink::Call& c = rig.uplink.lastCall();
  CHECK_FALSE(c.reachable);
  CHECK(has(c.error, "budget"));
  CHECK(rig.socket().opens == 0);
  CHECK(rig.server().requests.empty());
  CHECK(rig.uplink.uplinkCounters().budget == 1);
  CHECK(rig.uplink.uplinkCounters().requests == 0);
  CHECK_FALSE(holds(&rig.uplink, sizeof rig.uplink, kTokenBody));

  // A deadline that has passed altogether is the same answer.
  rig.uplink.setDeadline(rig.clock.now_ms - 1);
  CHECK_FALSE(rig.uplink.fetchConfig());
  CHECK(has(rig.uplink.lastCall().error, "budget: 0 ms left"));
  CHECK(rig.socket().opens == 0);

  // With enough left the call is made, and what is left is its whole deadline: a server that
  // never answers costs exactly the rest of the budget - ten seconds here, sooner than the
  // transport's own fifteen for a piece - then the socket is closed, and the caller takes the
  // modem down cleanly after that.
  rig.uplink.setDeadline(rig.clock.now_ms + 10000);
  rig.socket().stall = true;
  const std::uint64_t before = rig.clock.now_ms;
  CHECK_FALSE(rig.uplink.fetchConfig());
  CHECK_FALSE(rig.uplink.lastCall().reachable);
  CHECK(has(rig.uplink.lastCall().error, "the deadline passed"));
  CHECK(has(rig.uplink.lastCall().error, "the wake's budget"));
  CHECK(rig.clock.now_ms - before == 10000);
  CHECK(rig.socket().opens == 1);
  CHECK(rig.socket().isOpen() == false);
  CHECK(rig.socket().closes == 1);
  rig.tokenNowhere("after the deadline");

  // No deadline: the transport's own defaults bound the call (core/net/http_over_tls.h) - here
  // the wait for a piece that never comes, after the connect and the send.
  rig.uplink.setDeadline(0);
  const std::uint64_t again = rig.clock.now_ms;
  CHECK_FALSE(rig.uplink.fetchConfig());
  CHECK(rig.clock.now_ms - again == rig.socket().connect_ms + rig.socket().piece_ms + core::net::kDefaultPieceMs);
  CHECK(has(rig.uplink.lastCall().error, "the deadline passed"));
  CHECK_FALSE(has(rig.uplink.lastCall().error, "the wake's budget"));
}

// ---------------------------------------------------------------------------- the settings

TEST_CASE("not configured without a URL or a token: no call is made and the record says so") {
  SUBCASE("no token") {
    Rig rig("https://server.example", false);
    CHECK_FALSE(rig.uplink.configured());
    CHECK_FALSE(rig.uplink.tokenSet());
    CHECK_FALSE(rig.uplink.fetchConfig());
    CHECK(has(rig.uplink.lastCall().error, "not configured"));
    CHECK(rig.socket().opens == 0);
    CHECK(rig.uplink.uplinkCounters().not_configured == 1);
  }
  SUBCASE("no URL") {
    Rig rig("", true);
    CHECK_FALSE(rig.uplink.configured());
    CHECK(rig.uplink.tokenSet());
    CHECK(std::string(rig.uplink.url()).empty());
    mm::HeartbeatReply reply;
    mm::Heartbeat hb;
    CHECK_FALSE(rig.uplink.postHeartbeat(hb, reply));
    CHECK(has(rig.uplink.lastCall().error, "not configured"));
    CHECK(rig.socket().opens == 0);
  }
}

TEST_CASE("https only: a server.url with another scheme is refused by the settings and nothing is sent") {
  Rig rig("http://server.example");
  CHECK(rig.settings.rejected() == 1);
  CHECK(std::string(rig.settings.firstRejected()) == "server.url");
  CHECK_FALSE(rig.uplink.configured());
  CHECK_FALSE(rig.uplink.fetchConfig());
  CHECK(has(rig.uplink.lastCall().error, "not configured"));
  CHECK(rig.socket().opens == 0);
  // The client's own rule stands behind it: a URL that is not https is refused before the modem
  // is touched (host_test/core/net/test_client.cpp).
}

TEST_CASE("the URL as stored: a trailing slash off, a path kept, the route appended") {
  Rig rig("https://server.example:8443/mm/");
  rig.server().hosts.push_back(sim::ScriptedHost{"server.example", core::net::kRootIsrgX1});
  rig.serve("/mm/agent/v1/config", contract("AgentConfig"));
  CHECK(std::string(rig.uplink.url()) == "https://server.example:8443/mm");
  CHECK(rig.uplink.fetchConfig());
  CHECK(rig.last().path == "/mm/agent/v1/config");
  CHECK(rig.last().host == "server.example:8443");
  CHECK(std::string(rig.uplink.lastEndpoint()) == "/agent/v1/config");
}

TEST_CASE("the 120-a-minute ceiling is kept on this side: the 121st call in a minute is not made") {
  Rig rig;
  rig.serve("/agent/v1/jobs/1/result", contract("JobResultReply"));
  rig.socket().connect_ms = 1;
  rig.socket().piece_ms = 0;
  const std::string body = "{}";
  for (int i = 0; i < 120; ++i) {
    CHECK(rig.uplink.postJobResult(1, body.c_str(), body.size()));
  }
  CHECK(rig.server().requests.size() == 120);
  CHECK_FALSE(rig.uplink.postJobResult(1, body.c_str(), body.size()));
  CHECK(rig.server().requests.size() == 120);
  CHECK_FALSE(rig.uplink.lastCall().reachable);
  CHECK(has(rig.uplink.lastCall().error, "held back by the 120-a-minute ceiling"));
  CHECK(rig.uplink.uplinkCounters().held == 1);
  CHECK(rig.uplink.limiter().held() == 1);
  rig.clock.advance(60001);
  CHECK(rig.uplink.postJobResult(1, body.c_str(), body.size()));
}

TEST_CASE("link(): the ups and downs the owner says, counted") {
  Rig rig;
  mm::IUplink::LinkState s = rig.uplink.link();
  CHECK(s.up);
  CHECK(s.connects == 1);
  CHECK(s.drops == 0);
  rig.uplink.setLinkUp(true);  // already up: nothing counted
  CHECK(rig.uplink.link().connects == 1);
  rig.uplink.setLinkUp(false);
  CHECK_FALSE(rig.uplink.link().up);
  CHECK(rig.uplink.link().drops == 1);
  rig.uplink.setLinkUp(true);
  CHECK(rig.uplink.link().connects == 2);
}

TEST_CASE("the heartbeat's body is bounded: one that does not fit is refused before any call") {
  Rig rig;
  rig.serve("/agent/v1/heartbeat", contract("HeartbeatReply"));
  mm::Heartbeat hb;
  hb.agentVersion = kVersion;
  // `errors` is an open key space (agent/Envelopes.h): enough counters overflow the buffer.
  std::vector<std::string> names;
  for (int i = 0; i < 200; ++i) names.push_back("counterWithALongName" + std::to_string(i));
  std::vector<mm::ErrorCounter> errors;
  for (const std::string& name : names) errors.push_back(mm::ErrorCounter{name.c_str(), 1});
  hb.errors = errors.data();
  hb.errorCount = static_cast<int>(errors.size());
  mm::HeartbeatReply reply;
  CHECK_FALSE(rig.uplink.postHeartbeat(hb, reply));
  CHECK(has(rig.uplink.lastCall().error, "the heartbeat body did not fit"));
  CHECK(rig.server().requests.empty());
}
