// A scripted meshmaster.dk for the wake bench (task p4-05): the agent routes of API §3 and the
// firmware routes of `docs/dev/unit-protocol.md` §4, answered out of the contract fixtures and
// out of what a test queued, behind the scripted server the fake TLS socket reaches
// (host_test/sim/scripted_server.h). It is the server as the Walter's wake sees it: a config, a
// heartbeat reply whose `pollIntervalS` is **what was accepted** of the unit's `nextCheckInS`
// (clamped to the server's 60..86 400, as the page says), a claim handed out from a queue of
// job JSON a test built with the library's own `fake::Job`, every result, extend and progress
// recorded by job id, an observation batch counted, and a release served as files with ranges.
//
// What a test turns: `away(true)` is a server nobody reaches (every handshake fails, which is
// what a week without coverage or a deploy looks like from an LTE-M modem); `accept(seconds)`
// pins the interval the next heartbeat reply accepts; `queue(job)` is one claim's worth. What it
// records: `heartbeats`, `claims`, `results[job id]`, `extends[job id]`, `progress[job id]`,
// `batches`, and every request the scripted server saw.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <agent/Iso8601.h>

#include "sim/fake_http.h"

namespace bench {

// The contract fixtures, read at run time and never transcribed (FIXTURES_DIR is the copied set).
inline std::string contractFixture(const char* name) {
  const std::string path = std::string(FIXTURES_DIR) + "/" + name + ".json";
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

struct PostedResult {
  long long job_id = 0;
  std::string body;
  std::uint64_t at_ms = 0;
};

class MeshMasterScript {
 public:
  MeshMasterScript(sim::FakeHttp& http, fakes::FakeClock& clock) : http_(http), clock_(clock) {
    config_ = contractFixture("AgentConfig");
    http_.server.on_request = [this](const sim::SeenRequest& request, sim::ScriptedReply* reply) {
      return answer(request, reply);
    };
  }

  // -- the script ------------------------------------------------------------------------------

  // Nobody home: the handshake fails on every request.
  void away(bool gone) { http_.socket.refuse_tls = gone; }
  bool isAway() const { return http_.socket.refuse_tls; }
  // What the next heartbeat replies accept, in seconds; 0 is "what the unit proposed, clamped".
  void accept(std::uint32_t seconds) { accepted_ = seconds; }
  // One claim's worth of jobs (the library's own `fake::Job` builds the JSON).
  void queue(const std::vector<std::string>& jobs) { queued_.push_back(jobs); }
  void queue(const std::string& job) { queue(std::vector<std::string>{job}); }
  std::size_t queuedJobs() const {
    std::size_t n = 0;
    for (const std::vector<std::string>& batch : queued_) n += batch.size();
    return n;
  }
  void forgetQueued() { queued_.clear(); }
  // The server's own clock, as its replies stamp it; 0 is the fixtures' own stamps.
  void setServerTime(std::uint32_t epoch) { server_epoch_ = epoch; }
  // A status for every request from now on (a 401, a 503 with a Retry-After...).
  void failWith(int status, const std::string& retry_after = "") {
    fail_status_ = status;
    fail_retry_after_ = retry_after;
  }
  void answerAgain() { fail_status_ = 0; }
  // The next `count` result posts answer `status` (a server that takes everything but results).
  void failResults(int status, int count) {
    result_fail_status_ = status;
    result_fail_count_ = count;
  }

  // -- what happened -----------------------------------------------------------------------------

  std::vector<std::string> heartbeats;            // the bodies, in order
  std::vector<int> claim_maxes;                   // every claim's `max`
  std::map<long long, std::vector<PostedResult>> results;  // by job id, in order
  std::map<long long, std::vector<std::string>> extends;
  std::map<long long, std::vector<std::string>> progress;
  std::vector<std::string> batches;               // observation batches, the bodies
  std::vector<std::string> calls;                 // every agent route reached, in order
  int configs = 0;
  int claims = 0;
  int withheld_claims = 0;  // claims answered with nothing queued
  int results_refused = 0;  // result posts answered with the scripted failure

  // The last `nextCheckInS` a heartbeat carried, and what was accepted of it.
  std::uint32_t lastProposed() const { return last_proposed_; }
  std::uint32_t lastAccepted() const { return last_accepted_; }
  const sim::SeenRequest* lastRequestTo(const std::string& path) const {
    const sim::SeenRequest* found = nullptr;
    for (const sim::SeenRequest& r : http_.server.requests) {
      if (r.path == path) found = &r;
    }
    return found;
  }
  int requestsTo(const std::string& path) const {
    int n = 0;
    for (const sim::SeenRequest& r : http_.server.requests) n += r.path == path;
    return n;
  }
  // Whether any request's path or body carries `needle` (a token that must be in no URL or body).
  bool anyRequestCarries(const std::string& needle) const {
    for (const sim::SeenRequest& r : http_.server.requests) {
      if (r.path.find(needle) != std::string::npos || r.body.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  // How many requests carried the token in the Authorization header, and in any other header.
  void tokenHeaders(const std::string& token, int* in_authorization, int* elsewhere) const {
    *in_authorization = 0;
    *elsewhere = 0;
    for (const sim::SeenRequest& r : http_.server.requests) {
      for (const auto& h : r.headers) {
        if (h.second.find(token) == std::string::npos) continue;
        if (h.first == "authorization") ++*in_authorization;
        else ++*elsewhere;
      }
    }
  }
  std::size_t resultCount() const {
    std::size_t n = 0;
    for (const auto& entry : results) n += entry.second.size();
    return n;
  }

 private:
  static std::string iso(std::uint32_t epoch) {
    char out[24];
    mm::formatUtc(epoch, out, sizeof out);
    return out;
  }
  static std::uint32_t numberAfter(const std::string& body, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = body.find(needle);
    if (at == std::string::npos) return 0;
    return static_cast<std::uint32_t>(std::strtoul(body.c_str() + at + needle.size(), nullptr, 10));
  }
  static int countItems(const std::string& batch) {
    int n = 0;
    std::size_t at = 0;
    while ((at = batch.find("\"publicKey\":", at)) != std::string::npos) {
      ++n;
      at += 12;
    }
    return n;
  }

  bool answer(const sim::SeenRequest& request, sim::ScriptedReply* reply) {
    const std::string& path = request.path;
    if (path.rfind("/agent/v1/", 0) != 0) return false;
    // The firmware routes are files the test served: the scripted server's own range logic.
    if (path.rfind("/agent/v1/firmware/", 0) == 0) return false;
    calls.push_back(path);
    reply->headers.push_back("X-RateLimit-Remaining: 119");
    reply->headers.push_back("Content-Type: application/json");
    if (fail_status_ != 0) {
      reply->status = fail_status_;
      if (!fail_retry_after_.empty()) reply->headers.push_back("Retry-After: " + fail_retry_after_);
      reply->body = "{\"title\":\"scripted\"}";
      return true;
    }
    reply->status = 200;
    const std::uint32_t server_time = server_epoch_ != 0 ? server_epoch_ : 1791936000u;
    if (path == "/agent/v1/config") {
      ++configs;
      reply->body = config_;
      return true;
    }
    if (path == "/agent/v1/heartbeat") {
      heartbeats.push_back(request.body);
      last_proposed_ = numberAfter(request.body, "nextCheckInS");
      // What was accepted: the unit's own proposal clamped to 60..86 400, or what the test pinned.
      std::uint32_t accepted = accepted_ != 0 ? accepted_ : last_proposed_;
      if (accepted == 0) accepted = 120;
      if (accepted < 60) accepted = 60;
      if (accepted > 86400) accepted = 86400;
      last_accepted_ = accepted;
      char json[256];
      std::snprintf(json, sizeof json,
                    "{\"serverTime\":\"%s\",\"budget\":{\"maxTransactionsPerMinute\":%d,\"minGapSeconds\":%d},"
                    "\"pollIntervalS\":%lu,\"jobsWaiting\":%s}",
                    iso(server_time).c_str(), max_per_minute_, gap_seconds_,
                    static_cast<unsigned long>(accepted), queued_.empty() ? "false" : "true");
      reply->body = json;
      return true;
    }
    if (path == "/agent/v1/observations") {
      batches.push_back(request.body);
      reply->body = "{\"accepted\":" + std::to_string(countItems(request.body)) + ",\"duplicate\":false}";
      return true;
    }
    if (path == "/agent/v1/jobs/claim") {
      ++claims;
      int max = 0;
      std::sscanf(request.body.c_str(), "{\"max\":%d}", &max);
      claim_maxes.push_back(max);
      std::vector<std::string> batch;
      if (!queued_.empty()) {
        std::vector<std::string>& front = queued_.front();
        const std::size_t take = static_cast<std::size_t>(max) < front.size() ? static_cast<std::size_t>(max) : front.size();
        batch.assign(front.begin(), front.begin() + static_cast<long>(take));
        front.erase(front.begin(), front.begin() + static_cast<long>(take));
        if (front.empty()) queued_.erase(queued_.begin());
      } else {
        ++withheld_claims;
      }
      std::string response = "{\"serverTime\":\"" + iso(server_time) + "\",\"jobs\":[";
      for (std::size_t i = 0; i < batch.size(); ++i) {
        if (i > 0) response += ",";
        response += batch[i];
      }
      response += "]}";
      reply->body = response;
      return true;
    }
    long long job_id = 0;
    char action[16] = {0};
    if (std::sscanf(path.c_str(), "/agent/v1/jobs/%lld/%15s", &job_id, action) == 2) {
      const std::string what(action);
      if (what == "result") {
        if (result_fail_count_ != 0) {
          --result_fail_count_;
          ++results_refused;
          reply->status = result_fail_status_;
          reply->body = "{\"title\":\"scripted\"}";
          return true;
        }
        results[job_id].push_back(PostedResult{job_id, request.body, clock_.nowMs()});
        reply->body = "{\"jobId\":" + std::to_string(job_id) + ",\"state\":\"succeeded\",\"duplicate\":false,\"late\":false}";
        return true;
      }
      if (what == "extend") {
        extends[job_id].push_back(request.body);
        reply->body = "{\"jobId\":" + std::to_string(job_id) + ",\"leaseUntil\":\"" + iso(server_time + 900) + "\"}";
        return true;
      }
      if (what == "progress") {
        progress[job_id].push_back(request.body);
        reply->body = "{\"jobId\":" + std::to_string(job_id) + ",\"recordedAt\":\"" + iso(server_time) + "\"}";
        return true;
      }
    }
    reply->status = 404;
    reply->body = "{\"title\":\"There is no such resource.\"}";
    return true;
  }

  sim::FakeHttp& http_;
  fakes::FakeClock& clock_;
  std::string config_;
  std::vector<std::vector<std::string>> queued_;
  std::uint32_t accepted_ = 0;
  std::uint32_t last_proposed_ = 0;
  std::uint32_t last_accepted_ = 0;
  std::uint32_t server_epoch_ = 0;
  int max_per_minute_ = 2;
  int gap_seconds_ = 20;
  int fail_status_ = 0;
  std::string fail_retry_after_;
  int result_fail_status_ = 503;
  int result_fail_count_ = 0;
};

}  // namespace bench
