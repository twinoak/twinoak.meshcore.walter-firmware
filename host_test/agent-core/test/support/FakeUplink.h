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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
  enum class Fault : uint8_t { None, Unreachable, Unauthorized, RateLimited, ServerError, Discard, NotFound };

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

  // A reply that carries an **ask for an update** (task p1.15-61): the members of the contract
  // fixture `tests/fixtures/contracts/HeartbeatReplyUpdate.json`, with this fake's own
  // server time so that nothing of the clock moves, and `jobsWaiting` as a test wants it - the
  // real server sends an exit node no job while an ask is out, and the board's own hold is what
  // these tests are for.
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

  // The next `calls` posts of an update's result answer this instead of 200. `Fault::NotFound` is
  // the one 404 - an id that is not this exit node's - which is an answer and not a failure.
  void failUpdateResults(Fault fault, int calls) {
    updateFault_ = fault;
    updateFaultCalls_ = calls;
  }

  // Every batch id this fake has been handed, whether its answer reached the board or not: a
  // post under an id that is in here is answered *had it already*, as the server answers one.
  const std::set<std::string>& seenBatches() const { return seenBatches_; }

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
    obsAnswerLost_ = 0;
    claimAnswerLost_ = 0;
    claimFaultCalls_ = 0;
  }
  // The next `calls` **result** posts answer this, whatever the other calls do: the offline
  // buffer is what a server that went away under a running job fills, and the claim that handed
  // the job over had to be answered for the job to run at all.
  void failResults(Fault fault, int calls) {
    resultFault_ = fault;
    resultFaultCalls_ = calls;
  }
  // The next `calls` **claims** answer this, whatever the other calls do: a claim the server
  // **refused** - a 4xx, a 5xx, a 429 - which is the server having spoken, and which a wake does
  // not ask again (task p1.20-70).
  void failClaims(Fault fault, int calls) {
    claimFault_ = fault;
    claimFaultCalls_ = calls;
  }
  // The next `calls` **claims** are leased by the server and the answer never reaches the board:
  // the evening of 21 November 2026 at Bellevue, where the claim answered *http 200* without its
  // body and the server had leased the unit a download and a status read (task p1.20-70,
  // meshmaster.dk's `LostLease.cs`). The jobs leave the queue - the server has leased them - and
  // are **handed back to the next claim that says it does not hold them**, first and before
  // anything new, which is meshmaster.dk's task p4.47-254 and the half this fake stands in for.
  // A claim with no `holding` member is handed nothing back, because an agent that says nothing
  // may be running the job.
  void loseTheAnswerToClaims(int calls) { claimAnswerLost_ = calls; }
  // The next `calls` **observation** posts are taken and stored by the server and the answer
  // never reaches the board - the Walter's failure of 16 November 2026, and the one a batch's
  // client id exists for (task p1.13-55). A later post under the same id is answered *had it
  // already*, with nothing stored twice.
  void loseTheAnswerToObservations(int calls) { obsAnswerLost_ = calls; }
  // What the answers to results say about more work waiting (API §3's `jobsWaiting`, task
  // p1.20-69): `results` of them carry it, and -1 is every one. The default is a server that
  // says it at no result at all - which is what one that has never heard of the member answers,
  // and what every test written before that task gets.
  void sayMoreWaitsAfterAResult(int results) { moreWaits_ = results; }
  void sayMoreWaitsAfterEveryResult() { moreWaits_ = -1; }
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
  // Every result of an update this fake was handed, whatever it answered (task p1.15-61).
  const std::vector<std::pair<long long, std::string>>& updateResults() const { return updateResults_; }
  // The order of the calls, as words: "config", "heartbeat", "result", "claim", "observations",
  // "extend", "progress", "update-result". What EXIT-NODES §5's order inside one pass is
  // asserted against.
  const std::vector<std::string>& calls() const { return calls_; }
  int configCalls() const { return configCalls_; }
  int claimCalls() const { return claimCalls_; }
  // The `max` of every claim, in order: what a wake's transaction cap asked the server for.
  const std::vector<int>& claimMaxes() const { return claimMaxes_; }
  // And the **bytes** of every claim, in order (task p1.20-69): `{"max":N}` for the first claim
  // of a wake and for every claim a board that does not sleep makes, with `transactionsLeft`
  // after `max` in a wake's later ones.
  const std::vector<std::string>& claimRequests() const { return claimRequests_; }
  // The `holding` of every claim, in order, as the library handed it in: the job ids this exit
  // node said it was running or keeping a result for (task p1.20-70). An empty vector is `[]` on
  // the wire - *nothing held* - because this library states the member in every claim.
  const std::vector<std::vector<long long>>& claimHoldings() const { return claimHoldings_; }
  // Jobs this fake leased out and whose answer was lost, still waiting to be handed back.
  size_t leasedAndLost() const { return lost_.size(); }
  // Every answer this fake gave a result, in order: `JobResultReply`'s bytes, with `jobsWaiting`
  // last where this server had more for the exit node.
  const std::vector<std::string>& resultReplies() const { return resultReplies_; }
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
    const std::string posted = clientIdOf(std::string(body, len));
    // An answer that was lost on the way back: the server has the batch, the board does not know
    // it (loseTheAnswerToObservations). It is remembered **before** the call fails, because the
    // server stored it and the next post of the same id is a duplicate to it.
    if (obsAnswerLost_ != 0) seenBatches_.insert(posted);
    if (!begin("observations")) return false;
    // The server de-duplicates on the batch's client id (EXIT-NODES section 6). A batch whose id
    // it has seen before is answered 200 with nothing stored and `duplicate` set - which is what
    // the board gets when the answer to the first post was lost on the way back (task p1.13-55).
    duplicate = seenBatches_.count(posted) != 0;
    accepted = duplicate ? 0 : countItems(std::string(body, len));
    seenBatches_.insert(posted);
    return true;
  }

  bool claimJobs(mm::ClaimedJob* jobs, int max, int& count, int transactionsLeft, const long long* holding,
                 int holdingCount) override {
    claimCalls_++;
    claimMaxes_.push_back(max);
    // The bytes the board would have put on the wire for this claim, from what it handed in: what
    // a test reads to see the optional `transactionsLeft` of a wake's later claim (task p1.20-69)
    // and the `holding` of every claim (task p1.20-70), to the byte.
    char request[mm::kMaxClaimRequestLen];
    const size_t requestLen =
        mm::writeClaimRequest(max, request, sizeof request, transactionsLeft, holding, holdingCount);
    claimRequests_.emplace_back(request, requestLen);
    std::vector<long long> said;
    for (int i = 0; i < holdingCount && holding != nullptr; i++) said.push_back(holding[i]);
    claimHoldings_.push_back(said);
    claimed_ = jobs;
    claimedCount_ = 0;
    count = 0;
    // What was leased to this exit node and whose answer never reached it, handed back **first
    // and before anything new** to a claim that says it does not hold it (meshmaster.dk's task
    // p4.47-254). A claim that states no `holding` is handed nothing back: an agent that says
    // nothing may be running the job.
    std::vector<std::string> handBack;
    if (holdingCount >= 0) {
      for (size_t i = 0; i < lost_.size();) {
        if (std::find(said.begin(), said.end(), lost_[i].first) != said.end()) {
          i++;
          continue;
        }
        handBack.push_back(lost_[i].second);
        lost_.erase(lost_.begin() + static_cast<long>(i));
      }
    }
    if (handBack.size() > static_cast<size_t>(max)) handBack.resize(static_cast<size_t>(max));
    const bool lose = claimAnswerLost_ != 0;
    if (!begin("claim")) {
      // The call failed before anything was leased: what was about to be handed back goes back to
      // the front of the lost list, in the order it was leased, for the next claim to find.
      for (size_t i = 0; i < handBack.size(); i++) {
        lost_.insert(lost_.begin() + static_cast<long>(i), {idOf(handBack[i]), handBack[i]});
      }
      return false;
    }
    std::vector<std::string> batch = handBack;
    if (batch.size() < static_cast<size_t>(max) && !queued_.empty()) {
      std::vector<std::string>& front = queued_.front();
      const size_t room = static_cast<size_t>(max) - batch.size();
      const size_t take = room < front.size() ? room : front.size();
      batch.insert(batch.end(), front.begin(), front.begin() + static_cast<long>(take));
      front.erase(front.begin(), front.begin() + static_cast<long>(take));
      if (front.empty()) queued_.erase(queued_.begin());
    }
    // The answer that was lost (loseTheAnswerToClaims): the server **leased** these jobs and the
    // board never learned of them, so they are gone from the queue and remembered here. The call
    // answers 200 with no body, which is what the modem at Bellevue gave the unit.
    if (lose) {
      if (claimAnswerLost_ > 0) claimAnswerLost_--;
      for (const std::string& job : batch) lost_.push_back({idOf(job), job});
      call_.ok = false;
      std::snprintf(call_.error, sizeof call_.error, "http 200 but the claim did not parse: the body was empty");
      return false;
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

  bool postJobResult(long long jobId, const char* body, size_t len, bool& jobsWaiting) override {
    jobsWaiting = false;
    if (!begin("result")) return false;
    results_.push_back(PostedResult{jobId, std::string(body, len), platform_.millis()});
    // The answer, as the server writes one: `JobResultReply.json`'s members, and `jobsWaiting`
    // last when this server has more for the exit node - the line of task p1.20-69, which is the
    // waiting fixture `JobResultReplyWaiting.json`. What the board learns from it is read by the
    // **firmware's own reader** and not by a second reading here, so a test of a wake that claims
    // again is a test against those bytes.
    const bool more = moreWaits_ != 0;
    if (moreWaits_ > 0) moreWaits_--;
    char reply[160];
    const int written =
        std::snprintf(reply, sizeof reply, "{\"jobId\":%lld,\"state\":\"succeeded\",\"duplicate\":false,\"late\":false%s}",
                      jobId, more ? ",\"jobsWaiting\":true" : "");
    resultReplies_.emplace_back(reply);
    jobsWaiting = mm::resultReplySaysJobsWaiting(reply, static_cast<size_t>(written));
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

  bool postUpdateResult(long long updateId, const char* body, size_t len, bool& notFound) override {
    updateResults_.emplace_back(updateId, std::string(body, len));
    notFound = false;
    if (!begin("update-result")) {
      notFound = call_.httpStatus == 404;
      return false;
    }
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
    } else if (std::strcmp(what, "claim") == 0 && claimFaultCalls_ != 0) {
      fault = claimFault_;
      if (claimFaultCalls_ > 0) claimFaultCalls_--;
    } else if (std::strcmp(what, "update-result") == 0 && updateFaultCalls_ != 0) {
      fault = updateFault_;
      if (updateFaultCalls_ > 0) updateFaultCalls_--;
    } else if (std::strcmp(what, "observations") == 0 && obsAnswerLost_ != 0) {
      fault = Fault::Unreachable;
      if (obsAnswerLost_ > 0) obsAnswerLost_--;
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
      case Fault::NotFound:
        call_.reachable = true;
        call_.httpStatus = 404;
        std::snprintf(call_.error, sizeof call_.error, "http 404");
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

  // A claimed job's own id, read out of the JSON this fake was queued with: what `holding` is
  // compared against and what a leased-and-lost job is remembered by.
  static long long idOf(const std::string& job) {
    const size_t at = job.find("\"id\":");
    if (at == std::string::npos) return 0;
    return std::strtoll(job.c_str() + at + 5, nullptr, 10);
  }

  // The batch's `clientId`, which is what the server de-duplicates on.
  static std::string clientIdOf(const std::string& batch) {
    const size_t at = batch.find("\"clientId\":\"");
    if (at == std::string::npos) return "";
    const size_t from = at + 12;
    const size_t end = batch.find('"', from);
    return end == std::string::npos ? "" : batch.substr(from, end - from);
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
  int obsAnswerLost_ = 0;
  int claimAnswerLost_ = 0;
  Fault claimFault_ = Fault::None;
  int claimFaultCalls_ = 0;
  // Leased to this exit node, answer lost: (job id, the job's JSON), in the order they were
  // leased. Handed back to the first claim that says it does not hold them.
  std::vector<std::pair<long long, std::string>> lost_;
  int resultFaultCalls_ = 0;
  Fault updateFault_ = Fault::None;
  int updateFaultCalls_ = 0;
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
  int configCalls_ = 0;
  int claimCalls_ = 0;
  std::vector<int> claimMaxes_;
  std::vector<std::vector<long long>> claimHoldings_;
  std::vector<std::string> claimRequests_;
  std::vector<std::string> resultReplies_;
  int moreWaits_ = 0;
  const mm::ClaimedJob* claimed_ = nullptr;
  int claimedCount_ = 0;

  bool hasConfig_ = false;
  mm::AgentConfig config_;
  Outcome outcome_;
  Call call_;
  mm::RateLimiter limiter_;
};

}  // namespace fake
