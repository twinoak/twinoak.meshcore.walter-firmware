// A scripted meshmaster.dk for the wake bench (task p4-05): the agent routes of API §3 and the
// firmware routes of `docs/dev/unit-protocol.md` §4, answered out of the contract fixtures and
// out of what a test queued, behind the scripted server the fake TLS socket reaches
// (host_test/sim/scripted_server.h). It is the server as the Walter's wake sees it: a config, a
// heartbeat reply whose `pollIntervalS` is **what was accepted** of the unit's `nextCheckInS`
// (clamped to the server's 60..86 400, as the page says), a claim handed out from a queue of
// job JSON a test built with the library's own `fake::Job`, every result, extend and progress
// recorded by job id, an observation batch counted, and a release served as files with ranges.
//
// **It routes by the heartbeat's `jobTypes`, as the server does** (task p8-04): the server sends
// a unit only the types its last heartbeat advertised - "routing requires the type advertised"
// (agent/AgentLoop.cpp's `buildHeartbeat`, meshmaster.dk's task p3-53) - so a queued job of a type
// this unit did not offer is neither counted in `jobsWaiting` nor handed over by a claim; it stays
// queued and `jobs_not_routed` counts the times it was passed over. Before this the fake handed
// over whatever a test queued, which is how a wake that offered no mesh type could still be seen
// running a mesh job. `route_by_job_types = false` is the old behaviour, for a test that wants the
// server to misbehave.
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
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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

// What the server holds a job's lease at, as meshmaster.dk holds it today (task p14-04): a claim
// gives thirty minutes, and an extension sets the lease to **now plus what was asked** - even
// when it held more, which is how the thirty minutes a claim gave became the fifteen an apply
// asked for. More than the ceiling is refused as invalid.
constexpr int kMaxExtendSeconds = 1800;
constexpr int kClaimLeaseSeconds = 1800;

struct PostedResult {
  long long job_id = 0;
  std::string body;
  std::uint64_t at_ms = 0;
  // Where the lease stood when this result came in, and whether it had lapsed: the server fails a
  // job whose lease ran out, so a result posted after this is a person told the wrong thing.
  std::uint64_t lease_until_ms = 0;
  bool late = false;
};

// One post of a scanner's lines, as the server saw it (task p16-04): `POST
// /agent/v1/jobs/{id}/lines/{from}`, whose body is NDJSON and not JSON - the lines as the scanner
// sent them, each ended with `\n`.
struct SeenLines {
  long long job_id = 0;
  std::uint32_t from = 0;
  std::string body;
  std::uint64_t at_ms = 0;
};

// One `extend`, as the server saw it.
struct SeenExtend {
  int seconds = 0;
  std::uint64_t at_ms = 0;
  std::uint64_t until_ms = 0;  // where the lease stood afterwards; 0 when the ask was refused
  bool refused = false;        // more than the ceiling: 400, and the lease is left where it was
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
  void answerAgain() {
    fail_status_ = 0;
    next_fail_count_ = 0;
    route_status_.clear();
    silent_routes_.clear();
    claim_answer_lost_ = 0;
  }
  // **One route**, from now on: a status for it while the rest of the API answers normally (the
  // path's suffix under `/agent/v1`, as the uplink builds it - `/heartbeat`, `/jobs/claim`).
  // Task p13-02: *the heartbeat came back 503 and the claim never came back* is one wake, and it
  // is the wake a list of failed calls has to be able to show.
  void failRoute(const std::string& suffix, int status, const std::string& retry_after = "") {
    route_status_[suffix] = std::make_pair(status, retry_after);
  }
  // The same, for a route that is taken and never answered: the call ends at its deadline with
  // no status at all.
  void silenceRoute(const std::string& suffix) { silent_routes_.push_back(suffix); }
  // The **next `count` requests to one route** answer `status`, and the route is itself again
  // after them: a deploy, a gateway that was restarting, an attach that was slow once. Task
  // p14-05 needs it - a trial whose first heartbeat fails and whose second is answered is one
  // wake, so the server cannot be told to stop failing in between.
  void failNextOn(const std::string& suffix, int status, int count, const std::string& retry_after = "") {
    next_fail_route_ = suffix;
    next_fail_status_ = status;
    next_fail_count_ = count;
    next_fail_retry_after_ = retry_after;
  }
  // **The next `count` observation posts are stored and their answers lost** (task p15-04): the
  // batch reaches this server and goes into its own memory, and then the TLS refuses before the
  // reply - which is the failure of 16 November 2026 as the unit met it (*the connection closed
  // before the body was complete*, `tasks/phase-15/reports/02.md`). The knob and its name are
  // upstream's own `FakeUplink::loseTheAnswerToObservations`, so that a case here reads as the
  // library's own case for the same thing.
  void loseTheAnswerToObservations(int calls) { obs_answer_lost_ = calls; }
  // **The next `count` claims are leased and their answers lost** (the library's `v0.7.6`,
  // upstream's task p1.20-70): the jobs leave this server's queue - it has leased them to this
  // unit - and the call ends with no answer at all, which is the evening of 21 November 2026 at
  // Bellevue. The lease is remembered here and **handed back to the first claim that says it does
  // not hold that job** (`holding`, meshmaster.dk's task p4.47-254), which is this server's own
  // half of the bump and the half a unit cannot be tested against without. The knob's name is
  // upstream's own `FakeUplink::loseTheAnswerToClaims`.
  void loseTheAnswerToClaims(int count) { claim_answer_lost_ = count; }
  // The jobs this server has leased out and whose answers were lost, still waiting to be handed
  // back.
  std::size_t leasedAndLost() const { return lost_.size(); }
  // The next `count` result posts answer `status` (a server that takes everything but results).
  void failResults(int status, int count) {
    result_fail_status_ = status;
    result_fail_count_ = count;
  }
  // The next `count` posts of a scanner's lines are **taken and never answered** (task p16-04):
  // the call ends at its deadline with no status at all, which is what the contract's *a post
  // whose answer never comes* is. The route carries `{from}` in its path, so it cannot be named
  // to `silenceRoute` once and for all.
  void silenceNextLines(int count) { silent_lines_ = count; }
  // How many lines of one job this server has room for; 0 is as many as come. Past it the reply
  // says `full` and the unit posts no more.
  void roomForLines(std::uint32_t lines) { lines_room_ = lines; }
  std::uint32_t linesHeld(long long job_id) const {
    const auto at = lines_held_.find(job_id);
    return at == lines_held_.end() ? 0u : at->second;
  }
  // **The airtime budget the heartbeat's reply hands out** (task p16-06). The unit's own is the
  // default, 2 a minute with a 20 s gap; a server that gives a tighter one is what makes a job of
  // a few transactions take the better part of an hour, which is the only way a test can reach
  // the ceiling on a raised bound without an hour of simulated airtime.
  void budgetIs(int per_minute, int gap_seconds) {
    max_per_minute_ = per_minute;
    gap_seconds_ = gap_seconds;
  }

  // Called with the path of every `/agent/v1` request this server answers, before it answers.
  std::function<void(const std::string& path)> on_call;

  // Whether a claim is held to the types the last heartbeat advertised. True is the server.
  bool route_by_job_types = true;
  // The types the last heartbeat offered, in order.
  const std::vector<std::string>& advertised() const { return advertised_; }
  bool offers(const std::string& type) const {
    if (!route_by_job_types) return true;
    if (advertised_.empty()) return true;  // no heartbeat yet: nothing to route by
    for (const std::string& one : advertised_) {
      if (one == type) return true;
    }
    return false;
  }

  // -- what happened -----------------------------------------------------------------------------

  std::vector<std::string> heartbeats;            // the bodies, in order
  std::vector<int> claim_maxes;                   // every claim's `max`
  // And every claim's **body**, in order, as this server received it: `{"max":N}` with the two
  // optional members the library's `v0.7.6` added after it - `transactionsLeft` (upstream's task
  // p1.20-69) and `holding` (p1.20-70), which this unit's library states in every claim.
  std::vector<std::string> claim_bodies;
  int claims_handed_back = 0;  // jobs handed over again because a claim said it did not hold them
  std::map<long long, std::vector<PostedResult>> results;  // by job id, in order
  std::map<long long, std::vector<std::string>> extends;
  std::map<long long, std::vector<SeenExtend>> extensions;    // the same calls, read (task p14-04)
  std::map<long long, std::uint64_t> lease_until_ms;          // what the server holds now
  std::map<long long, std::vector<std::string>> progress;
  std::map<long long, std::vector<std::uint64_t>> progress_at_ms;
  std::map<long long, std::vector<SeenLines>> lines;  // the posts of a scanner's lines, in order
  int lines_unanswered = 0;                           // ... and the ones this script never answered
  std::vector<std::string> batches;               // observation batches, the bodies
  // **What this server stored, de-duplicated on the batch's `clientId`**, which is what
  // meshmaster.dk does with one (task p15-04): the ids it has seen, and how many adverts came in
  // under an id it had not seen before. A batch posted again under its own id adds nothing to
  // either - so `storedAdverts()` is the count of distinct adverts a unit's rows would show, and
  // it is the number that was too high before the library kept a batch's id over a sleep.
  const std::set<std::string>& seenBatches() const { return seen_batches_; }
  std::size_t storedAdverts() const { return stored_adverts_; }
  // The `clientId` of a batch body, as the library writes it.
  static std::string clientIdOf(const std::string& batch) {
    const std::size_t at = batch.find("\"clientId\":\"");
    if (at == std::string::npos) return "";
    const std::size_t begin = at + 12;
    const std::size_t end = batch.find('"', begin);
    return end == std::string::npos ? std::string() : batch.substr(begin, end - begin);
  }
  std::vector<std::string> calls;                 // every agent route reached, in order
  int configs = 0;
  int claims = 0;
  int withheld_claims = 0;  // claims answered with nothing queued
  int results_refused = 0;  // result posts answered with the scripted failure
  int jobs_not_routed = 0;  // queued jobs a claim passed over: the unit does not offer the type

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
  // The job types the heartbeat offered: the strings of its `jobTypes` array, in order.
  static std::vector<std::string> jobTypesOf(const std::string& body) {
    std::vector<std::string> types;
    const std::size_t at = body.find("\"jobTypes\":[");
    if (at == std::string::npos) return types;
    std::size_t i = at + 12;
    while (i < body.size() && body[i] != ']') {
      if (body[i] != '"') {
        ++i;
        continue;
      }
      const std::size_t end = body.find('"', i + 1);
      if (end == std::string::npos) break;
      types.push_back(body.substr(i + 1, end - i - 1));
      i = end + 1;
    }
    return types;
  }
  // One job's `id`, as the claim's JSON carries it.
  static long long idOf(const std::string& job) {
    const std::size_t at = job.find("\"id\":");
    return at == std::string::npos ? 0 : std::strtoll(job.c_str() + at + 5, nullptr, 10);
  }
  // The claim's `holding`, as the body carries it (the library's `v0.7.6`): the ids this unit says
  // it keeps a result for, and `*stated` false when the member is not there at all - which says
  // *this unit does not say* where `[]` says *nothing*, and is handed nothing back.
  static std::vector<long long> holdingOf(const std::string& body, bool* stated) {
    std::vector<long long> ids;
    const std::size_t at = body.find("\"holding\":[");
    *stated = at != std::string::npos;
    if (!*stated) return ids;
    for (std::size_t i = at + 11; i < body.size() && body[i] != ']';) {
      if (body[i] < '0' || body[i] > '9') {
        ++i;
        continue;
      }
      ids.push_back(std::strtoll(body.c_str() + i, nullptr, 10));
      while (i < body.size() && body[i] >= '0' && body[i] <= '9') ++i;
    }
    return ids;
  }
  // One job's `type`, as the claim's JSON carries it.
  static std::string typeOf(const std::string& job) {
    const std::size_t at = job.find("\"type\":\"");
    if (at == std::string::npos) return "";
    const std::size_t begin = at + 8;
    const std::size_t end = job.find('"', begin);
    return end == std::string::npos ? std::string() : job.substr(begin, end - begin);
  }
  // How many queued jobs this unit offers a type for: what `jobsWaiting` means to the server.
  int routableQueued() const {
    int n = 0;
    for (const std::vector<std::string>& batch : queued_) {
      for (const std::string& job : batch) n += offers(typeOf(job)) ? 1 : 0;
    }
    return n;
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
    // **Something for a test to check at every call** (task p18-03): the one hook a rule of the
    // shape *nothing of this end goes out while X* can be held by, because it is told about every
    // request of the API and not only the ones a test went looking for.
    if (on_call) on_call(path);
    reply->headers.push_back("X-RateLimit-Remaining: 119");
    reply->headers.push_back("Content-Type: application/json");
    const std::string suffix = path.substr(9);  // past "/agent/v1"
    for (const std::string& silent : silent_routes_) {
      if (suffix == silent) {
        reply->no_response = true;
        return true;
      }
    }
    const auto scripted = route_status_.find(suffix);
    if (scripted != route_status_.end()) {
      reply->status = scripted->second.first;
      if (!scripted->second.second.empty()) {
        reply->headers.push_back("Retry-After: " + scripted->second.second);
      }
      reply->body = "{\"title\":\"scripted\"}";
      return true;
    }
    if (fail_status_ != 0) {
      reply->status = fail_status_;
      if (!fail_retry_after_.empty()) reply->headers.push_back("Retry-After: " + fail_retry_after_);
      reply->body = "{\"title\":\"scripted\"}";
      return true;
    }
    if (next_fail_count_ > 0 && suffix == next_fail_route_) {
      --next_fail_count_;
      reply->status = next_fail_status_;
      if (!next_fail_retry_after_.empty()) {
        reply->headers.push_back("Retry-After: " + next_fail_retry_after_);
      }
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
      advertised_ = jobTypesOf(request.body);
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
                    static_cast<unsigned long>(accepted), routableQueued() > 0 ? "true" : "false");
      reply->body = json;
      return true;
    }
    if (path == "/agent/v1/observations") {
      batches.push_back(request.body);
      // The server de-duplicates on the batch's own `clientId` (task p15-04): a batch it has
      // already stored is answered *had it already* and stored nothing twice. The store happens
      // **before** the answer can be lost, because that is the failure the library's kept batch
      // id exists for - the rows are on the server and the unit does not know it.
      const std::string id = clientIdOf(request.body);
      const bool had_it = !id.empty() && seen_batches_.count(id) != 0;
      if (!had_it) {
        if (!id.empty()) seen_batches_.insert(id);
        stored_adverts_ += static_cast<std::size_t>(countItems(request.body));
      }
      if (obs_answer_lost_ != 0) {
        --obs_answer_lost_;
        reply->no_response = true;
        return true;
      }
      reply->body = "{\"accepted\":" + std::to_string(had_it ? 0 : countItems(request.body)) +
                    ",\"duplicate\":" + (had_it ? "true" : "false") + "}";
      return true;
    }
    if (path == "/agent/v1/jobs/claim") {
      ++claims;
      int max = 0;
      // `{"max":N}` and, since the library's `v0.7.6`, `transactionsLeft` and `holding` after it.
      // `max` is first in all of them, so the one assignment reads the same number it always did.
      std::sscanf(request.body.c_str(), "{\"max\":%d", &max);
      claim_maxes.push_back(max);
      claim_bodies.push_back(request.body);
      std::vector<std::string> batch;
      // **What was leased to this unit and whose answer was lost, first and before anything new**
      // (meshmaster.dk's task p4.47-254): handed back to a claim that states `holding` without
      // that id. A claim that states no such member is handed nothing back - a unit that says
      // nothing may be running the job.
      bool stated = false;
      const std::vector<long long> held = holdingOf(request.body, &stated);
      if (stated) {
        for (std::size_t i = 0; i < lost_.size();) {
          if (static_cast<int>(batch.size()) >= max) break;
          bool theirs = false;
          for (long long id : held) theirs = theirs || id == lost_[i].first;
          if (theirs) {
            ++i;
            continue;
          }
          batch.push_back(lost_[i].second);
          ++claims_handed_back;
          lost_.erase(lost_.begin() + static_cast<long>(i));
        }
      }
      if (static_cast<int>(batch.size()) < max && !queued_.empty()) {
        // Only the types the last heartbeat offered, up to `max`; the rest stay queued, which is
        // the server keeping a job for a unit that cannot do it yet rather than leasing it out.
        std::vector<std::string>& front = queued_.front();
        for (std::size_t i = 0; i < front.size();) {
          if (static_cast<int>(batch.size()) >= max) break;
          if (!offers(typeOf(front[i]))) {
            ++jobs_not_routed;
            ++i;
            continue;
          }
          batch.push_back(front[i]);
          front.erase(front.begin() + static_cast<long>(i));
        }
        if (front.empty()) queued_.erase(queued_.begin());
      } else if (queued_.empty() && batch.empty()) {
        ++withheld_claims;
      }
      // **The answer this unit never gets** (loseTheAnswerToClaims): the server has leased these
      // jobs - the lease begins the moment it answers, and it did - and the call ends with nothing
      // at all. They are remembered here, out of the queue, and the next claim that says it does
      // not hold them is handed them back.
      if (claim_answer_lost_ != 0) {
        if (claim_answer_lost_ > 0) --claim_answer_lost_;
        for (const std::string& job : batch) {
          lease_until_ms[idOf(job)] = clock_.nowMs() + static_cast<std::uint64_t>(kClaimLeaseSeconds) * 1000u;
          lost_.push_back(std::make_pair(idOf(job), job));
        }
        reply->no_response = true;
        return true;
      }
      std::string response = "{\"serverTime\":\"" + iso(server_time) + "\",\"jobs\":[";
      for (std::size_t i = 0; i < batch.size(); ++i) {
        if (i > 0) response += ",";
        response += batch[i];
        // The lease the claim gives, which is the one an extension replaces rather than adds to.
        lease_until_ms[idOf(batch[i])] =
            clock_.nowMs() + static_cast<std::uint64_t>(kClaimLeaseSeconds) * 1000u;
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
        const std::uint64_t now = clock_.nowMs();
        const std::uint64_t lease = lease_until_ms.count(job_id) != 0 ? lease_until_ms[job_id] : 0;
        const bool late = lease != 0 && now > lease;
        results[job_id].push_back(PostedResult{job_id, request.body, now, lease, late});
        reply->body = "{\"jobId\":" + std::to_string(job_id) + ",\"state\":\"succeeded\",\"duplicate\":false,\"late\":" +
                      (late ? "true" : "false") + "}";
        return true;
      }
      if (what == "extend") {
        extends[job_id].push_back(request.body);
        const int seconds = static_cast<int>(numberAfter(request.body, "seconds"));
        SeenExtend seen;
        seen.seconds = seconds;
        seen.at_ms = clock_.nowMs();
        if (seconds < 1 || seconds > kMaxExtendSeconds) {
          // `MaxExtendSeconds`: more than the ceiling is not a longer lease, it is a bad request.
          seen.refused = true;
          extensions[job_id].push_back(seen);
          reply->status = 400;
          reply->body = "{\"title\":\"seconds must be between 1 and 1800.\"}";
          return true;
        }
        // **Now plus what was asked**, even when the lease held more.
        const std::uint64_t until = clock_.nowMs() + static_cast<std::uint64_t>(seconds) * 1000u;
        lease_until_ms[job_id] = until;
        seen.until_ms = until;
        extensions[job_id].push_back(seen);
        reply->body = "{\"jobId\":" + std::to_string(job_id) + ",\"leaseUntil\":\"" +
                      iso(server_time + static_cast<std::uint32_t>(seconds)) + "\"}";
        return true;
      }
      if (what == "progress") {
        progress[job_id].push_back(request.body);
        progress_at_ms[job_id].push_back(clock_.nowMs());
        reply->body = "{\"jobId\":" + std::to_string(job_id) + ",\"recordedAt\":\"" + iso(server_time) + "\"}";
        return true;
      }
      // **A scanner's lines** (task p16-04): `{from}` is in the path, the body is NDJSON, and the
      // reply says how many lines of this job are held and whether the room is used up. The rule
      // the contract gives this end - *a line it already holds is left alone, a `from` past what
      // it holds is accepted and the gap counted as lost* - is `held = max(held, from + count)`.
      if (what.rfind("lines/", 0) == 0) {
        const std::uint32_t from =
            static_cast<std::uint32_t>(std::strtoul(what.c_str() + 6, nullptr, 10));
        lines[job_id].push_back(SeenLines{job_id, from, request.body, clock_.nowMs()});
        if (silent_lines_ > 0) {
          --silent_lines_;
          ++lines_unanswered;
          reply->no_response = true;
          return true;
        }
        std::uint32_t count = 0;
        for (char c : request.body) count += c == '\n' ? 1 : 0;
        const std::uint32_t held = from + count;
        std::uint32_t& have = lines_held_[job_id];
        if (held > have) have = held;
        const bool full = lines_room_ != 0 && have >= lines_room_;
        reply->body = "{\"held\":" + std::to_string(have) + ",\"full\":" +
                      (full ? "true" : "false") + "}";
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
  std::vector<std::string> advertised_;  // the last heartbeat's `jobTypes`
  std::uint32_t accepted_ = 0;
  std::uint32_t last_proposed_ = 0;
  std::uint32_t last_accepted_ = 0;
  std::uint32_t server_epoch_ = 0;
  int max_per_minute_ = 2;
  int gap_seconds_ = 20;
  int fail_status_ = 0;
  std::string fail_retry_after_;
  // failNext(): a run of failures with an end to it.
  std::string next_fail_route_;
  int next_fail_status_ = 0;
  int next_fail_count_ = 0;
  std::string next_fail_retry_after_;
  // One route's scripted answer, by the path's suffix under /agent/v1 (task p13-02).
  std::map<std::string, std::pair<int, std::string>> route_status_;
  std::vector<std::string> silent_routes_;
  int result_fail_status_ = 503;
  int result_fail_count_ = 0;
  // Task p15-04: how many more observation posts are stored and then left unanswered, and what
  // this server has stored.
  int obs_answer_lost_ = 0;
  // The library's `v0.7.6`: how many more claims are leased with their answers lost, and the jobs
  // this server has leased out that way - (id, the job's JSON) - waiting to be handed back.
  int claim_answer_lost_ = 0;
  std::vector<std::pair<long long, std::string>> lost_;
  std::set<std::string> seen_batches_;
  std::size_t stored_adverts_ = 0;
  // A scanner's lines (task p16-04): how many posts to leave unanswered, the room one job's lines
  // have, and how many this server holds of each.
  int silent_lines_ = 0;
  std::uint32_t lines_room_ = 0;
  std::map<long long, std::uint32_t> lines_held_;
};

}  // namespace bench
