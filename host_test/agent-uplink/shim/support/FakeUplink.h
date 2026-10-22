// The library's scripted meshmaster.dk behind the modem (task p4-03): the loop's scenarios,
// again, through HTTPS.
//
// The agent library's test_loop suite drives `fake::FakeUplink` - its scripted server behind the
// uplink seam, which answers a config, a heartbeat reply, a queue of claims built from the
// contract fixtures, and the faults of API §3 on cue - and asserts on what it recorded. This
// header shadows that class, the way host_test/shim/support/FakePlatform.h shadows the platform
// and host_test/agent-link/shim/support/FakeMesh.h the mesh, with one that keeps every knob and
// every record of the original (`queue`, `failWith`, `answerHeartbeat`, `calls()`, `results()`,
// ...) and puts the agent's ModemUplink, phase 3's HTTPS client and the scripted server behind
// the firmware's own HTTP/1.1 between the loop and the script:
//
//     AgentLoop -> mm::IUplink = ModemUplink -> core::net::Client -> HttpOverTls
//               -> FakeTlsSocket -> ScriptedServer::on_request = this script
//
// So a scenario's `server.queue(job)` is a claim answered over the wire with that job's bytes,
// its `failWith(Unauthorized, 1)` a real 401 the uplink reads into the Call record, and
// `server.results()` what reached the server as the body of `POST /agent/v1/jobs/{id}/result` -
// the same assertions, now through the framing. The 47 cases of test_loop.cpp are compiled a
// second time against this directory (host_test/CMakeLists.txt, `agent_core_loop_uplink_tests`),
// unchanged: the loop does not know which uplink it has.
//
// Not a subclass of the original: the original's script is private to it and is read by its own
// IUplink methods, which parse a reply they built themselves and never put on a wire. The
// semantics are copied from it line by line and said so beside each; the shape of a claim is
// the original's (the queue's oldest batch, `max` of it, the rest left queued), the cost of a
// call on the clock is the original's `callMs` (spent by the fake socket's connect, tied to the
// platform's clock), and the Call record is now ModemUplink's own.
#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <agent/ConfigReply.h>
#include <agent/Envelopes.h>
#include <agent/IUplink.h>
#include <agent/Iso8601.h>
#include <agent/JobEnvelopes.h>

#include <support/FakePlatform.h>  // the shim's: fake::FakePlatform carries the link's clock
#include <support/Fixtures.h>

// The original, under another name: `PostedResult` and the fixtures it reads are reused; the
// class itself is compiled and never constructed here.
#define FakeUplink FakeUplinkUpstream
#include_next <support/FakeUplink.h>
#undef FakeUplink

#include "core/net/client.h"
#include "core/settings/settings.h"
#include "fakes/sim_key_value.h"
#include "interfaces/wall_clock.h"
#include "sim/fake_http.h"
#include "walter_agent/modem_uplink.h"

namespace fake {

// interfaces::WallClock over the platform's own wall clock: what ModemUplink stamps `atEpoch`
// from, as the Walter's NetworkClock will.
class WallOverPlatform final : public interfaces::WallClock {
 public:
  explicit WallOverPlatform(FakePlatform& platform) : platform_(platform) {}
  bool now(interfaces::WallTime* out) override {
    if (!platform_.clock().hasTime() || out == nullptr) return false;
    out->epoch = platform_.clock().now();
    out->source = 2;  // the server's
    return true;
  }

 private:
  FakePlatform& platform_;
};

class FakeUplink : public mm::IUplink {
 public:
  // What the server answered, where it was not 200 (the original's five).
  enum class Fault : uint8_t { None, Unreachable, Unauthorized, RateLimited, ServerError, Discard };

  explicit FakeUplink(FakePlatform& platform)
      : platform_(platform),
        settings_(kv_),
        wall_(platform),
        http_(platform.linkClock),
        client_(http_, platform.linkClock),
        uplink_(client_, settings_, platform.linkClock, wall_, "0.2.0") {
    configJson_ = contract("AgentConfig");
    heartbeatReplyJson_ = contract("HeartbeatReply");
    setConfigured(true);
    // One call costs `callMs` on the clock, as the original's `begin()` charged it: the fake
    // socket's connect is the whole of it, and a piece costs nothing.
    http_.socket.connect_ms = callMs_;
    http_.socket.piece_ms = 0;
    http_.server.on_request = [this](const sim::SeenRequest& request, sim::ScriptedReply* reply) {
      return answer(request, reply);
    };
  }

  // ------------------------------------------------------------------ the road

  agent::ModemUplink& modem() { return uplink_; }
  sim::FakeHttp& http() { return http_; }
  core::settings::Settings& settings() { return settings_; }

  // ------------------------------------------------------------------ the server, as set up

  // `configured()` is a URL and a token in the settings (core/settings/keys.h): on puts both,
  // off forgets every key, which is what a unit nobody provisioned looks like.
  void setConfigured(bool on) {
    char err[96] = {0};
    if (on) {
      kv_.put("srv_url", "https://server.example");
      kv_.put("srv_token", "mma_shim0000000000000000000000000000000000");
      settings_.begin();
    } else {
      settings_.eraseAll(err, sizeof err);
    }
  }
  void setLinkUp(bool up) { uplink_.setLinkUp(up); }
  void setConfigJson(std::string json) { configJson_ = std::move(json); }
  void setHeartbeatReplyJson(std::string json) { heartbeatReplyJson_ = std::move(json); }
  // The budget and the poll interval the next heartbeat reply carries, as JSON of its own.
  void answerHeartbeat(int maxPerMinute, int gapSeconds, int pollIntervalS, bool jobsWaiting, uint32_t serverEpoch) {
    char iso[32];
    mm::formatUtc(serverEpoch, iso, sizeof iso);
    char json[256];
    std::snprintf(json, sizeof json,
                  "{\"serverTime\":\"%s\",\"budget\":{\"maxTransactionsPerMinute\":%d,\"minGapSeconds\":%d},"
                  "\"pollIntervalS\":%d,\"jobsWaiting\":%s}",
                  iso, maxPerMinute, gapSeconds, pollIntervalS, jobsWaiting ? "true" : "false");
    heartbeatReplyJson_ = json;
  }

  // One claim's worth of jobs, oldest batch first, `max` of it handed over and the rest left
  // queued (the original's rule, which is the server's: EXIT-NODES §6 as built in task 121).
  void queue(std::vector<std::string> jobs) { queued_.push_back(std::move(jobs)); }
  void queue(const std::string& job) { queue(std::vector<std::string>{job}); }
  bool anythingQueued() const { return !queued_.empty(); }
  void forgetQueued() { queued_.clear(); }
  size_t queuedJobs() const {
    size_t n = 0;
    for (const std::vector<std::string>& batch : queued_) n += batch.size();
    return n;
  }

  void setServerTime(uint32_t epoch) { serverEpoch_ = epoch; }

  // ------------------------------------------------------------------ the faults, on cue

  void failWith(Fault fault, int calls) {
    fault_ = fault;
    faultCalls_ = calls;
  }
  void failAlways(Fault fault) {
    fault_ = fault;
    faultCalls_ = -1;
  }
  void answerAgain() {
    fault_ = Fault::None;
    faultCalls_ = 0;
  }
  void failResults(Fault fault, int calls) {
    resultFault_ = fault;
    resultFaultCalls_ = calls;
  }
  void setRetryAfter(uint32_t seconds) { retryAfterS_ = seconds; }
  void setCallMs(uint32_t ms) {
    callMs_ = ms;
    http_.socket.connect_ms = ms;
  }
  void beSlowOnce(uint32_t ms) { slowCallMs_ = ms; }

  // ------------------------------------------------------------------ what a test reads back

  const std::vector<std::string>& heartbeats() const { return heartbeats_; }
  const std::vector<PostedResult>& results() const { return results_; }
  const std::vector<std::string>& observations() const { return observations_; }
  const std::vector<std::pair<long long, std::string>>& extends() const { return extends_; }
  const std::vector<std::pair<long long, std::string>>& progress() const { return progress_; }
  const std::vector<std::string>& calls() const { return calls_; }
  int configCalls() const { return configCalls_; }
  int claimCalls() const { return claimCalls_; }
  const std::vector<int>& claimMaxes() const { return claimMaxes_; }
  const mm::ClaimedJob* claimed() const { return claimed_; }
  int claimedCount() const { return claimedCount_; }
  void forgetCalls() {
    calls_.clear();
    heartbeats_.clear();
    results_.clear();
    observations_.clear();
  }

  // ------------------------------------------------------------------ IUplink, over the modem

  bool configured() const override { return uplink_.configured(); }
  LinkState link() const override { return uplink_.link(); }

  bool fetchConfig() override {
    configCalls_++;
    begin("config");
    return uplink_.fetchConfig();
  }

  bool postHeartbeat(const mm::Heartbeat& hb, mm::HeartbeatReply& reply) override {
    // What the board puts on the wire, from the struct the loop filled - recorded here as the
    // original recorded it, whatever the server then answers. That these bytes are the ones the
    // server receives is held by host_test/agent/test_modem_uplink.cpp.
    char body[4096];
    const size_t len = mm::writeHeartbeat(hb, body, sizeof body);
    heartbeats_.emplace_back(body, len);
    begin("heartbeat");
    return uplink_.postHeartbeat(hb, reply);
  }

  bool postObservations(const char* body, size_t len, int& accepted, bool& duplicate) override {
    observations_.emplace_back(body, len);
    begin("observations");
    return uplink_.postObservations(body, len, accepted, duplicate);
  }

  bool claimJobs(mm::ClaimedJob* jobs, int max, int& count) override {
    claimCalls_++;
    claimMaxes_.push_back(max);
    claimed_ = jobs;
    claimedCount_ = 0;
    count = 0;
    begin("claim");
    const bool ok = uplink_.claimJobs(jobs, max, count);
    claimedCount_ = count;
    return ok;
  }

  bool postJobResult(long long jobId, const char* body, size_t len) override {
    // Recorded when the server took it (answer()), as the original did only on a 200.
    begin("result");
    return uplink_.postJobResult(jobId, body, len);
  }

  bool extendLease(long long jobId, int seconds, uint32_t& leaseUntil) override {
    char body[64];
    const size_t len = mm::writeExtendRequest(seconds, body, sizeof body);
    extends_.emplace_back(jobId, std::string(body, len));
    begin("extend");
    return uplink_.extendLease(jobId, seconds, leaseUntil);
  }

  bool postProgress(long long jobId, const char* body, size_t len) override {
    progress_.emplace_back(jobId, std::string(body, len));
    begin("progress");
    return uplink_.postProgress(jobId, body, len);
  }

  bool hasConfig() const override { return uplink_.hasConfig(); }
  const mm::AgentConfig& config() const override { return uplink_.config(); }
  const Outcome& last() const override { return uplink_.last(); }
  const Call& lastCall() const override { return uplink_.lastCall(); }
  const mm::RateLimiter& limiter() const override { return uplink_.limiter(); }

 private:
  // One call is about to be made: counted in the order the original counted it, and the fault
  // and the cost of this one decided before the wire is touched - the original's `begin()`,
  // less the Call record, which is the uplink's own now.
  void begin(const char* what) {
    calls_.emplace_back(what);
    uint32_t cost = callMs_;
    if (slowCallMs_ > 0) {
      cost = slowCallMs_;
      slowCallMs_ = 0;
    }
    http_.socket.connect_ms = cost;

    pending_ = Fault::None;
    if (std::strcmp(what, "result") == 0 && resultFaultCalls_ != 0) {
      pending_ = resultFault_;
      if (resultFaultCalls_ > 0) resultFaultCalls_--;
    } else if (faultCalls_ != 0) {
      pending_ = fault_;
      if (faultCalls_ > 0) faultCalls_--;
    }
    // Unreachable is no reply at all: the handshake fails, after the connect's cost on the
    // clock, and the uplink reads it as `reachable = false` (a TLS failure on the modem).
    http_.socket.refuse_tls = pending_ == Fault::Unreachable;
  }

  // The server's side of one request: the fault decided in begin(), else the script's answer
  // for the route. Every answer carries X-RateLimit-Remaining, as meshmaster.dk's do.
  bool answer(const sim::SeenRequest& request, sim::ScriptedReply* reply) {
    reply->headers.push_back("X-RateLimit-Remaining: 119");
    reply->headers.push_back("Content-Type: application/json");
    switch (pending_) {
      case Fault::Unreachable:
        ADD_FAILURE() << "a request reached the server during an unreachable fault";
        return false;
      case Fault::Unauthorized:
        reply->status = 401;
        reply->body = "{\"title\":\"Unauthorized\"}";
        return true;
      case Fault::RateLimited:
        reply->status = 429;
        if (retryAfterS_ != 0) reply->headers.push_back("Retry-After: " + std::to_string(retryAfterS_));
        reply->body = "{\"title\":\"Too Many Requests\"}";
        return true;
      case Fault::ServerError:
        reply->status = 503;
        if (retryAfterS_ != 0) reply->headers.push_back("Retry-After: " + std::to_string(retryAfterS_));
        reply->body = "{\"title\":\"Service Unavailable\"}";
        return true;
      case Fault::Discard:
        reply->status = 422;
        reply->body = "{\"title\":\"Unprocessable\"}";
        return true;
      case Fault::None:
      default:
        break;
    }
    reply->status = 200;
    const std::string& path = request.path;
    if (path == "/agent/v1/config") {
      reply->body = configJson_;
      return true;
    }
    if (path == "/agent/v1/heartbeat") {
      reply->body = heartbeatReplyJson_;
      return true;
    }
    if (path == "/agent/v1/observations") {
      // The original: every item taken, and never a duplicate.
      reply->body = "{\"accepted\":" + std::to_string(countItems(request.body)) + ",\"duplicate\":false}";
      return true;
    }
    if (path == "/agent/v1/jobs/claim") {
      reply->body = claimReply(maxOf(request.body));
      return true;
    }
    long long jobId = 0;
    char action[16] = {0};
    if (std::sscanf(path.c_str(), "/agent/v1/jobs/%lld/%15s", &jobId, action) == 2) {
      if (std::strcmp(action, "result") == 0) {
        results_.push_back(PostedResult{jobId, request.body, platform_.millis()});
        reply->body = "{\"jobId\":" + std::to_string(jobId) + ",\"state\":\"succeeded\",\"duplicate\":false,\"late\":false}";
        return true;
      }
      if (std::strcmp(action, "extend") == 0) {
        reply->body = contract("ExtendReply");
        return true;
      }
      if (std::strcmp(action, "progress") == 0) {
        reply->body = contract("ProgressReply");
        return true;
      }
    }
    ADD_FAILURE() << "a request for a route this script does not have: " << request.method << " " << path;
    reply->status = 404;
    reply->body = "{\"title\":\"Not Found\"}";
    return true;
  }

  // The claim's answer: the queue's oldest batch, `max` of it, the rest left queued, as the
  // original built and parsed it.
  std::string claimReply(int max) {
    std::vector<std::string> batch;
    if (!queued_.empty()) {
      std::vector<std::string>& front = queued_.front();
      const size_t take = static_cast<size_t>(max) < front.size() ? static_cast<size_t>(max) : front.size();
      batch.assign(front.begin(), front.begin() + static_cast<long>(take));
      front.erase(front.begin(), front.begin() + static_cast<long>(take));
      if (front.empty()) queued_.erase(queued_.begin());
    }
    std::string response = "{\"serverTime\":\"";
    char iso[32];
    mm::formatUtc(serverEpoch_ != 0 ? serverEpoch_ : platform_.clock().now(), iso, sizeof iso);
    response += iso;
    response += "\",\"jobs\":[";
    for (size_t i = 0; i < batch.size(); i++) {
      if (i > 0) response += ",";
      response += batch[i];
    }
    response += "]}";
    return response;
  }

  static int maxOf(const std::string& body) {
    int max = 0;
    if (std::sscanf(body.c_str(), "{\"max\":%d}", &max) != 1) {
      ADD_FAILURE() << "a claim whose body is not {\"max\":N}: " << body;
      return 0;
    }
    return max;
  }

  static int countItems(const std::string& batch) {
    int n = 0;
    size_t at = 0;
    while ((at = batch.find("\"publicKey\":", at)) != std::string::npos) {
      n++;
      at += 12;
    }
    return n;
  }

  FakePlatform& platform_;
  fakes::SimKeyValue kv_;
  core::settings::Settings settings_;
  WallOverPlatform wall_;
  sim::FakeHttp http_;
  core::net::Client client_;
  agent::ModemUplink uplink_;

  std::string configJson_;
  std::string heartbeatReplyJson_;
  std::vector<std::vector<std::string>> queued_;
  uint32_t serverEpoch_ = 0;

  Fault fault_ = Fault::None;
  int faultCalls_ = 0;
  Fault resultFault_ = Fault::None;
  int resultFaultCalls_ = 0;
  Fault pending_ = Fault::None;
  uint32_t retryAfterS_ = 0;
  uint32_t callMs_ = 120;
  uint32_t slowCallMs_ = 0;

  std::vector<std::string> calls_;
  std::vector<std::string> heartbeats_;
  std::vector<PostedResult> results_;
  std::vector<std::string> observations_;
  std::vector<std::pair<long long, std::string>> extends_;
  std::vector<std::pair<long long, std::string>> progress_;
  int configCalls_ = 0;
  int claimCalls_ = 0;
  std::vector<int> claimMaxes_;
  const mm::ClaimedJob* claimed_ = nullptr;
  int claimedCount_ = 0;
};

}  // namespace fake
