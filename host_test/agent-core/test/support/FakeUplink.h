// meshmaster.dk behind the uplink seam, on a host (task p1.5-17).
//
// `FakeUplink` is `mm::IUplink` (agent/IUplink.h) with a scripted server behind it: a config, a
// heartbeat reply that may say work is waiting, a claim queue of jobs **built from the contract
// fixtures** (`tests/fixtures/contracts/*.json` through `mm::parseClaimResponse`, which is the
// firmware's own parser and not a second reading of the envelope), and every result, observation
// batch, extend and progress call captured as the bytes the library made.
//
// Nothing here is a reading of the server's *code*: the server's side of this protocol is
// meshmaster.dk's and the contract is the fixtures, so a claim this fake hands over is a fixture
// run through the parser and a heartbeat it captures is what `mm::writeHeartbeat` wrote. The
// faults are API §3's answers - unreachable, 401, 429 with Retry-After, 5xx, a slow call - and
// what each of them *means* is `agent/ServerPolicy.h`'s, which test/test_server_policy already
// holds code by code.
//
// Every call costs time on the fake clock (support/FakePlatform.h), so a heartbeat inside a job's
// wait and a backoff are both measured rather than slept.

#pragma once

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

#include <support/FakePlatform.h>
#include <support/Fixtures.h>

namespace fake {

// One result the server was handed.
struct PostedResult {
  long long jobId = 0;
  std::string body;
  uint32_t atMs = 0;
};

class FakeUplink : public mm::IUplink {
 public:
  // What the server answered, where it was not 200.
  enum class Fault : uint8_t { None, Unreachable, Unauthorized, RateLimited, ServerError, Discard };

  explicit FakeUplink(FakePlatform& platform) : platform_(platform) {
    configJson_ = contract("AgentConfig");
    heartbeatReplyJson_ = contract("HeartbeatReply");
  }

  // ------------------------------------------------------------------ the server, as set up

  void setConfigured(bool on) { configured_ = on; }
  void setLinkUp(bool up) {
    if (up && !link_.up) link_.connects++;
    if (!up && link_.up) link_.drops++;
    link_.up = up;
  }
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

  // One claim's worth of jobs, each a ClaimedJob object as the server writes one. The next
  // `claimJobs` hands over as many of them as its `max` allows, **oldest first, and leaves the
  // rest queued** - which is the server's own rule for what a claim does not hand out: it stays
  // queued and bound to nothing, for the next claim or another exit node (EXIT-NODES §6 as built
  // in meshmaster.dk's task p4.14-121).
  void queue(std::vector<std::string> jobs) { queued_.push_back(std::move(jobs)); }
  void queue(const std::string& job) { queue(std::vector<std::string>{job}); }
  bool anythingQueued() const { return !queued_.empty(); }
  // Everything a claim has not taken yet, forgotten: a test that runs round after round against
  // one fake server starts each of them with an empty queue.
  void forgetQueued() { queued_.clear(); }
  // How many jobs are still waiting across every batch: what a wake's cap left behind.
  size_t queuedJobs() const {
    size_t n = 0;
    for (const std::vector<std::string>& batch : queued_) n += batch.size();
    return n;
  }

  void setServerTime(uint32_t epoch) { serverEpoch_ = epoch; }

  // ------------------------------------------------------------------ the faults, on cue

  // The next `calls` calls answer this instead of 200.
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
  // The next `calls` **result** posts answer this, whatever the other calls do: the offline
  // buffer is what a server that went away under a running job fills, and the claim that handed
  // the job over had to be answered for the job to run at all.
  void failResults(Fault fault, int calls) {
    resultFault_ = fault;
    resultFaultCalls_ = calls;
  }
  void setRetryAfter(uint32_t seconds) { retryAfterS_ = seconds; }
  void setCallMs(uint32_t ms) { callMs_ = ms; }
  // A slow call: this long on the clock, once.
  void beSlowOnce(uint32_t ms) { slowCallMs_ = ms; }

  // ------------------------------------------------------------------ what a test reads back

  const std::vector<std::string>& heartbeats() const { return heartbeats_; }
  const std::vector<PostedResult>& results() const { return results_; }
  const std::vector<std::string>& observations() const { return observations_; }
  const std::vector<std::pair<long long, std::string>>& extends() const { return extends_; }
  const std::vector<std::pair<long long, std::string>>& progress() const { return progress_; }
  // The order of the calls, as words: "config", "heartbeat", "result", "claim", "observations",
  // "extend", "progress". What EXIT-NODES §5's order inside one pass is asserted against.
  const std::vector<std::string>& calls() const { return calls_; }
  int configCalls() const { return configCalls_; }
  int claimCalls() const { return claimCalls_; }
  // The `max` of every claim, in order: what a wake's transaction cap asked the server for.
  const std::vector<int>& claimMaxes() const { return claimMaxes_; }
  // The claim's own buffer, as the runner handed it in: how a test reads a credential back after
  // the job that used it has ended.
  const mm::ClaimedJob* claimed() const { return claimed_; }
  int claimedCount() const { return claimedCount_; }
  void forgetCalls() {
    calls_.clear();
    heartbeats_.clear();
    results_.clear();
    observations_.clear();
  }

  // ------------------------------------------------------------------ IUplink

  bool configured() const override { return configured_; }
  LinkState link() const override { return link_; }

  bool fetchConfig() override {
    configCalls_++;
    if (!begin("config")) {
      outcome_ = Outcome();
      outcome_.attempted = true;
      outcome_.httpStatus = call_.httpStatus;
      std::snprintf(outcome_.error, sizeof outcome_.error, "%s", call_.error);
      outcome_.atMillis = call_.atMillis;
      outcome_.atEpoch = call_.atEpoch;
      return false;
    }
    char err[96] = {0};
    if (!mm::parseAgentConfig(configJson_.c_str(), configJson_.size(), config_, err, sizeof err)) return false;
    hasConfig_ = true;
    outcome_ = Outcome();
    outcome_.attempted = true;
    outcome_.ok = true;
    outcome_.httpStatus = 200;
    outcome_.atMillis = call_.atMillis;
    outcome_.atEpoch = call_.atEpoch;
    outcome_.via = "the fake server";
    return true;
  }

  bool postHeartbeat(const mm::Heartbeat& hb, mm::HeartbeatReply& reply) override {
    // What the board would have put on the wire, from the struct the loop filled.
    char body[4096];
    const size_t len = mm::writeHeartbeat(hb, body, sizeof body);
    heartbeats_.emplace_back(body, len);
    if (!begin("heartbeat")) return false;
    char err[96] = {0};
    return mm::parseHeartbeatReply(heartbeatReplyJson_.c_str(), heartbeatReplyJson_.size(), reply, err, sizeof err);
  }

  bool postObservations(const char* body, size_t len, int& accepted, bool& duplicate) override {
    observations_.emplace_back(body, len);
    if (!begin("observations")) return false;
    accepted = countItems(std::string(body, len));
    duplicate = false;
    return true;
  }

  bool claimJobs(mm::ClaimedJob* jobs, int max, int& count) override {
    claimCalls_++;
    claimMaxes_.push_back(max);
    claimed_ = jobs;
    claimedCount_ = 0;
    count = 0;
    if (!begin("claim")) return false;
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
    uint32_t serverTime = 0;
    char err[96] = {0};
    const bool ok =
        mm::parseClaimResponse(&response[0], response.size(), jobs, max, count, serverTime, err, sizeof err);
    EXPECT_TRUE(ok) << "the claim this fake built did not parse: " << err;
    claimedCount_ = count;
    return ok;
  }

  bool postJobResult(long long jobId, const char* body, size_t len) override {
    if (!begin("result")) return false;
    results_.push_back(PostedResult{jobId, std::string(body, len), platform_.millis()});
    return true;
  }

  bool extendLease(long long jobId, int seconds, uint32_t& leaseUntil) override {
    // The body the WiFi client writes for this call, so what the fake captures is bytes and not
    // an integer (`mm::writeExtendRequest`, held against ExtendRequest.json).
    char body[64];
    const size_t len = mm::writeExtendRequest(seconds, body, sizeof body);
    extends_.emplace_back(jobId, std::string(body, len));
    if (!begin("extend")) return false;
    const std::string json = contract("ExtendReply");
    mm::ExtendReply reply;
    char err[96] = {0};
    if (!mm::parseExtendReply(json.c_str(), json.size(), reply, err, sizeof err)) return false;
    leaseUntil = reply.leaseUntilUnix;
    return true;
  }

  bool postProgress(long long jobId, const char* body, size_t len) override {
    progress_.emplace_back(jobId, std::string(body, len));
    if (!begin("progress")) return false;
    const std::string json = contract("ProgressReply");
    mm::ProgressReply reply;
    char err[96] = {0};
    return mm::parseProgressReply(json.c_str(), json.size(), reply, err, sizeof err);
  }

  bool hasConfig() const override { return hasConfig_; }
  const mm::AgentConfig& config() const override { return config_; }
  const Outcome& last() const override { return outcome_; }
  const Call& lastCall() const override { return call_; }
  const mm::RateLimiter& limiter() const override { return limiter_; }

 private:
  // One call: the clock moves, the limiter counts it, and the Call record says what came back.
  bool begin(const char* what) {
    calls_.emplace_back(what);
    uint32_t cost = callMs_;
    if (slowCallMs_ > 0) {
      cost = slowCallMs_;
      slowCallMs_ = 0;
    }
    platform_.advance(cost);
    limiter_.take(platform_.millis());

    call_ = Call();
    call_.atMillis = platform_.millis();
    call_.atEpoch = platform_.clock().now();
    call_.via = "the fake server";
    call_.rateLimitRemaining = 119;

    Fault fault = Fault::None;
    if (std::strcmp(what, "result") == 0 && resultFaultCalls_ != 0) {
      fault = resultFault_;
      if (resultFaultCalls_ > 0) resultFaultCalls_--;
    } else if (faultCalls_ != 0) {
      fault = fault_;
      if (faultCalls_ > 0) faultCalls_--;
    }
    switch (fault) {
      case Fault::Unreachable:
        std::snprintf(call_.error, sizeof call_.error, "no reply");
        return false;
      case Fault::Unauthorized:
        call_.reachable = true;
        call_.httpStatus = 401;
        std::snprintf(call_.error, sizeof call_.error, "http 401");
        return false;
      case Fault::RateLimited:
        call_.reachable = true;
        call_.httpStatus = 429;
        call_.retryAfterS = retryAfterS_;
        std::snprintf(call_.error, sizeof call_.error, "http 429");
        return false;
      case Fault::ServerError:
        call_.reachable = true;
        call_.httpStatus = 503;
        call_.retryAfterS = retryAfterS_;
        std::snprintf(call_.error, sizeof call_.error, "http 503");
        return false;
      case Fault::Discard:
        call_.reachable = true;
        call_.httpStatus = 422;
        std::snprintf(call_.error, sizeof call_.error, "http 422");
        return false;
      case Fault::None:
      default:
        break;
    }
    call_.reachable = true;
    call_.ok = true;
    call_.httpStatus = 200;
    return true;
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

  bool configured_ = true;
  LinkState link_;
  std::string configJson_;
  std::string heartbeatReplyJson_;
  std::vector<std::vector<std::string>> queued_;
  uint32_t serverEpoch_ = 0;

  Fault fault_ = Fault::None;
  int faultCalls_ = 0;
  Fault resultFault_ = Fault::None;
  int resultFaultCalls_ = 0;
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

  bool hasConfig_ = false;
  mm::AgentConfig config_;
  Outcome outcome_;
  Call call_;
  mm::RateLimiter limiter_;
};

}  // namespace fake
