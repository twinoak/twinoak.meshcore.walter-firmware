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
// the same assertions, now through the framing. The 74 cases of test_loop.cpp are compiled a
// second time against this directory (host_test/CMakeLists.txt, `agent_core_loop_uplink_tests`),
// unchanged: the loop does not know which uplink it has.
//
// Not a subclass of the original: the original's script is private to it and is read by its own
// IUplink methods, which parse a reply they built themselves and never put on a wire. The
// semantics are copied from it line by line and said so beside each; the shape of a claim is
// the original's (the queue's oldest batch, `max` of it, the rest left queued), the cost of a
// call on the clock is the original's `callMs` (spent by the fake socket's connect, tied to the
// platform's clock), and the Call record is now ModemUplink's own.
//
// **One call of the seam has no road here**, and it is the one exception to the line above:
// `postUpdateResult` (upstream's task p1.15-61, `POST /agent/v1/updates/{id}/result`). The
// library gives it a body of its own so that a consumer which cannot be updated from the server
// implements nothing and is never asked, and this firmware is such a consumer - an update is
// never asked of a Walter, it is asked of an exit node running the exit-node firmware's own
// product, so `agent::ModemUplink` takes the library's default and nothing of this repository
// hands the loop an `mm::IUpdateProvider`. The suite's own update cases do hand it one, so this
// shim answers that one call itself, with the original's own `Call` record (`updateCall_`
// below), rather than putting a request the firmware never makes on the wire.
#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
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
  // What the server answered, where it was not 200 (the original's six).
  enum class Fault : uint8_t { None, Unreachable, Unauthorized, RateLimited, ServerError, Discard, NotFound };

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

  // A reply that carries an **ask for an update** (upstream's task p1.15-61), the original's
  // knob: the members of the contract fixture `tests/fixtures/contracts/HeartbeatReplyUpdate.json`
  // with this fake's own server time, and `jobsWaiting` as a test wants it. Copied from the
  // original line for line - the bytes go out over the wire here, which is the only difference.
  void askForAnUpdate(long long id, long long releaseId, bool jobsWaiting = false) {
    char iso[32];
    mm::formatUtc(serverEpoch_ != 0 ? serverEpoch_ : platform_.clock().now(), iso, sizeof iso);
    char json[640];
    std::snprintf(json, sizeof json,
                  "{\"serverTime\":\"%s\",\"budget\":{\"maxTransactionsPerMinute\":60,"
                  "\"minGapSeconds\":1},\"pollIntervalS\":30,\"jobsWaiting\":%s,"
                  "\"update\":{\"id\":%lld,\"releaseId\":%lld,"
                  "\"product\":\"twinoak-exitnode-firmware\",\"version\":\"0.8.0\","
                  "\"base\":\"/agent/v1/firmware/%lld\",\"expectedSha256\":"
                  "\"3b1f0c4e5d6a79880817a6b5c4d3e2f1a0b9c8d7e6f5041322314051607f8e9d\","
                  "\"size\":1432000}}",
                  iso, jobsWaiting ? "true" : "false", id, releaseId, releaseId);
    heartbeatReplyJson_ = json;
  }

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
    obsAnswerLost_ = 0;
  }
  void failResults(Fault fault, int calls) {
    resultFault_ = fault;
    resultFaultCalls_ = calls;
  }
  // The next `calls` posts of an update's result answer this instead of 200 (the original's
  // knob). `Fault::NotFound` is the one 404 - an id that is not this exit node's - which is an
  // answer and not a failure.
  void failUpdateResults(Fault fault, int calls) {
    updateFault_ = fault;
    updateFaultCalls_ = calls;
  }
  // The next `calls` **observation** posts are taken and stored by the server and the answer
  // never reaches the board - the Walter's failure of 16 November 2026, and the one a batch's
  // client id exists for (upstream's task p1.13-55). A later post under the same id is answered
  // *had it already*, with nothing stored twice. The original's knob, and here the answer is
  // lost by the TLS refusing after the request went: `begin()` below.
  void loseTheAnswerToObservations(int calls) { obsAnswerLost_ = calls; }
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
  // Every result of an update this fake was handed, whatever it answered (the original's record,
  // upstream's task p1.15-61).
  const std::vector<std::pair<long long, std::string>>& updateResults() const { return updateResults_; }
  // Every batch id this fake has been handed, whether its answer reached the board or not: a
  // post under an id that is in here is answered *had it already*, as the server answers one.
  const std::set<std::string>& seenBatches() const { return seenBatches_; }
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
    // An answer that was lost on the way back: the server has the batch, the board does not know
    // it (loseTheAnswerToObservations). It is remembered **before** the call fails, because the
    // server stored it and the next post of the same id is a duplicate to it. The server's own
    // half - the de-duplication on the id, and the 200 that says *had it already* - is in
    // `answer()` below, where every other route's answer is.
    if (obsAnswerLost_ != 0) seenBatches_.insert(clientIdOf(std::string(body, len)));
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

  // The one call with no road (the header says why): recorded and answered here, with the
  // original's own Call record rather than the modem's, because no request is made. The
  // original's `begin()` decided the fault and filled that record, so `begin()` below does the
  // first and this does the second.
  bool postUpdateResult(long long updateId, const char* body, size_t len, bool& notFound) override {
    updateResults_.emplace_back(updateId, std::string(body, len));
    notFound = false;
    const bool taken = begin("update-result");
    updateCall_ = Call();
    updateCall_.atMillis = platform_.millis();
    updateCall_.atEpoch = platform_.clock().now();
    updateCall_.via = "the shim's own answer";
    updateCall_.rateLimitRemaining = 119;
    ownsLastCall_ = true;
    if (taken) {
      updateCall_.reachable = true;
      updateCall_.ok = true;
      updateCall_.httpStatus = 200;
      return true;
    }
    switch (pending_) {
      case Fault::Unauthorized:
        updateCall_.reachable = true;
        updateCall_.httpStatus = 401;
        break;
      case Fault::RateLimited:
        updateCall_.reachable = true;
        updateCall_.httpStatus = 429;
        updateCall_.retryAfterS = retryAfterS_;
        break;
      case Fault::ServerError:
        updateCall_.reachable = true;
        updateCall_.httpStatus = 503;
        updateCall_.retryAfterS = retryAfterS_;
        break;
      case Fault::Discard:
        updateCall_.reachable = true;
        updateCall_.httpStatus = 422;
        break;
      case Fault::NotFound:
        updateCall_.reachable = true;
        updateCall_.httpStatus = 404;
        notFound = true;
        break;
      case Fault::Unreachable:
      case Fault::None:
      default:
        break;  // no reply at all, which is what `reachable = false` says
    }
    std::snprintf(updateCall_.error, sizeof updateCall_.error, "http %d", updateCall_.httpStatus);
    return false;
  }

  bool hasConfig() const override { return uplink_.hasConfig(); }
  const mm::AgentConfig& config() const override { return uplink_.config(); }
  const Outcome& last() const override { return uplink_.last(); }
  // The modem's, for every call that went over the wire; the shim's own for the one that did not.
  const Call& lastCall() const override { return ownsLastCall_ ? updateCall_ : uplink_.lastCall(); }
  const mm::RateLimiter& limiter() const override { return uplink_.limiter(); }

 private:
  // One call is about to be made: counted in the order the original counted it, and the fault
  // and the cost of this one decided before the wire is touched - the original's `begin()`,
  // less the Call record, which is the uplink's own now. True when the call is to be answered
  // 200, which only `postUpdateResult` (no wire, no Call record of the modem's) reads.
  bool begin(const char* what) {
    calls_.emplace_back(what);
    ownsLastCall_ = false;
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
    } else if (std::strcmp(what, "update-result") == 0 && updateFaultCalls_ != 0) {
      pending_ = updateFault_;
      if (updateFaultCalls_ > 0) updateFaultCalls_--;
    } else if (std::strcmp(what, "observations") == 0 && obsAnswerLost_ != 0) {
      // The original's `Fault::Unreachable`: nothing comes back. The script never sees this
      // request - the TLS is refused before it goes - so the batch is written into the server's
      // own memory by `postObservations` above instead, which is what *stored, and the answer
      // lost* has to mean on this road.
      pending_ = Fault::Unreachable;
      if (obsAnswerLost_ > 0) obsAnswerLost_--;
    } else if (faultCalls_ != 0) {
      pending_ = fault_;
      if (faultCalls_ > 0) faultCalls_--;
    }
    // Unreachable is no reply at all: the handshake fails, after the connect's cost on the
    // clock, and the uplink reads it as `reachable = false` (a TLS failure on the modem).
    http_.socket.refuse_tls = pending_ == Fault::Unreachable;
    return pending_ == Fault::None;
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
      case Fault::NotFound:
        reply->status = 404;
        reply->body = "{\"title\":\"Not Found\"}";
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
      // The server de-duplicates on the batch's client id (EXIT-NODES §6, and the original's own
      // rule since upstream's task p1.13-55). A batch whose id it has seen before is answered 200
      // with nothing stored and `duplicate` set - which is what the board gets when the answer to
      // the first post was lost on the way back.
      const std::string posted = clientIdOf(request.body);
      const bool duplicate = seenBatches_.count(posted) != 0;
      reply->body = "{\"accepted\":" + std::to_string(duplicate ? 0 : countItems(request.body)) +
                    ",\"duplicate\":" + (duplicate ? "true" : "false") + "}";
      seenBatches_.insert(posted);
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

  // The batch's `clientId`, which is what the server de-duplicates on (the original's helper).
  static std::string clientIdOf(const std::string& batch) {
    const size_t at = batch.find("\"clientId\":\"");
    if (at == std::string::npos) return "";
    const size_t from = at + 12;
    const size_t end = batch.find('"', from);
    return end == std::string::npos ? "" : batch.substr(from, end - from);
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
  Fault updateFault_ = Fault::None;
  int updateFaultCalls_ = 0;
  int obsAnswerLost_ = 0;
  Fault pending_ = Fault::None;
  uint32_t retryAfterS_ = 0;
  uint32_t callMs_ = 120;
  uint32_t slowCallMs_ = 0;

  std::vector<std::string> calls_;
  std::vector<std::string> heartbeats_;
  std::vector<PostedResult> results_;
  std::vector<std::string> observations_;
  std::set<std::string> seenBatches_;
  std::vector<std::pair<long long, std::string>> extends_;
  std::vector<std::pair<long long, std::string>> progress_;
  std::vector<std::pair<long long, std::string>> updateResults_;
  // The one call this shim answers itself, and whether the last call was it (the header says why).
  Call updateCall_;
  bool ownsLastCall_ = false;
  int configCalls_ = 0;
  int claimCalls_ = 0;
  std::vector<int> claimMaxes_;
  const mm::ClaimedJob* claimed_ = nullptr;
  int claimedCount_ = 0;
};

}  // namespace fake
