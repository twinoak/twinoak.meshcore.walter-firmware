// The server as the agent loop and the job runner see it (task p1.5-15): the calls of API §3,
// each answering a `Call` record that ServerPolicy.h reads, and the state of the link they go
// over.
//
// `AgentClient` (AgentClient.h) is the WiFi implementation - HTTPS with the chain verified, the
// token read from NVS for the call and wiped after - and stays board-side; the Walter of the
// repeater-v2 design speaks the same seven calls through its modem. `TlsRoots.h` and the rail
// "verified, never bypassed" apply to both.
//
// Two of the seven the reference agent never makes but the protocol has (API §3):
// `POST /agent/v1/jobs/{id}/extend` and `POST /agent/v1/jobs/{id}/progress`, for a job that runs
// past its lease and wants to say so. Nothing on the Heltec calls them yet.
//
// **One call has a body here and is not pure interface**: `postUpdateResult`, the result of an
// update the server asked for (task p1.15-61). A consumer that cannot be updated from the server
// implements nothing, compiles unchanged and is never asked - the loop makes the call only for a
// consumer that gave it an `IUpdateProvider`. It is the one way to add a call to this seam without
// breaking every tree that pins it (docs/library.md §10).

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/ConfigReply.h>
#include <agent/Envelopes.h>
#include <agent/JobEnvelopes.h>
#include <agent/RateLimit.h>

namespace mm {

class IUplink {
 public:
  // What one request came back with, whatever it asked for. `via` is one word for how it went
  // (the WiFi client names the root the chain verified against); it is a static string, never
  // a buffer of the call's own.
  struct Call {
    bool reachable = false;   // a reply arrived, whatever its status
    bool ok = false;          // 2xx and the body parsed
    int httpStatus = 0;
    uint32_t retryAfterS = 0;  // the 429's Retry-After, in seconds
    int rateLimitRemaining = -1;
    char error[128] = {0};
    uint32_t atEpoch = 0;
    uint32_t atMillis = 0;
    const char* via = "";
  };

  // The config call keeps a little more, because `status` reports it.
  struct Outcome {
    bool attempted = false;
    bool ok = false;
    int httpStatus = 0;
    char error[128] = {0};
    uint32_t atMillis = 0;
    uint32_t atEpoch = 0;
    const char* via = "";
    uint32_t heapAfter = 0;  // free heap right after the call, the number later tasks budget from
  };

  // The link the calls go over. The loop gates on `up`, counts `connects - 1` as the heartbeat's
  // `wifiReconnects`, and prints all three on the console.
  struct LinkState {
    bool up = false;
    uint32_t connects = 0;  // links since boot
    uint32_t drops = 0;     // of those, the ones that fell
  };

  virtual ~IUplink() = default;

  // A URL and a token: whether a call can be made at all.
  virtual bool configured() const = 0;
  virtual LinkState link() const = 0;

  // GET /agent/v1/config, once. Blocking. The reply is kept and config() reports it.
  virtual bool fetchConfig() = 0;

  // POST /agent/v1/heartbeat. True when the server answered 200 and the reply parsed.
  virtual bool postHeartbeat(const Heartbeat& hb, HeartbeatReply& reply) = 0;

  // POST /agent/v1/observations. `body` is what writeObservationBatch() made.
  virtual bool postObservations(const char* body, size_t len, int& accepted, bool& duplicate) = 0;

  // POST /agent/v1/jobs/claim `{"max":N}`, the body `writeClaimRequest` makes. True when the
  // server answered 200 and the reply parsed; `count` is how many jobs it handed over, which may
  // be none.
  //
  // `transactionsLeft` is what the wake has left of the `maxTransactionsThisWake` its heartbeat
  // stated, so that the server hands out no job this wake cannot pay for (task p1.20-69).
  // **Below 0 is not stated**: the member is left out, which is what `loop()` sends and what the
  // first claim of a wake sends.
  //
  // `holding` and `holdingCount` are the ids of the jobs this exit node is running or keeps a
  // result for, oldest first (task p1.20-70): the claim's `holding`, on which the server hands
  // back a lease whose answer was lost. The library fills it for **every** claim it makes -
  // `[]` when it holds nothing, which says *nothing* where an absent member says *this agent
  // does not say* - so pass both straight on to
  // `writeClaimRequest(max, out, cap, transactionsLeft, holding, holdingCount)` and write
  // neither member yourself.
  //
  // **A lost answer is told apart from a refusal by `lastCall()`** (agent/ServerPolicy.h's
  // `answerWasLost`), so an implementation must leave `lastCall()` saying what really came back:
  // `reachable` false and `httpStatus` 0 for no reply, and `httpStatus` 200 with `ok` false for a
  // 200 whose body did not parse. A wake makes such a claim once more; it makes no second claim
  // after a refusal.
  virtual bool claimJobs(ClaimedJob* jobs, int max, int& count, int transactionsLeft, const long long* holding,
                         int holdingCount) = 0;

  // POST /agent/v1/jobs/{id}/result. `body` is what writeJobResult() made. A late result is
  // taken all the same and a duplicate clientId stores nothing twice, so this is safe to retry.
  //
  // `jobsWaiting` is the answer's own `jobsWaiting` (API §3, task p1.20-69): whether a claim by
  // this exit node now would be handed a job. **Read from the body this call already has** -
  // `mm::resultReplySaysJobsWaiting` is the reader, so the reply is parsed once, here, and the
  // library is handed an answer rather than a second copy of the bytes. False when the member was
  // not there, which is every answer from a server that has never written it, and false whenever
  // this returns false: a call that got no answer said nothing. A wake claims again on it
  // (`AgentLoop::WakeIntent::maxClaimsThisWake`); `loop()` does nothing with it.
  virtual bool postJobResult(long long jobId, const char* body, size_t len, bool& jobsWaiting) = 0;

  // POST /agent/v1/jobs/{id}/extend `{"seconds":N}` (1-1800): the lease holder asks for longer.
  // True when the server answered 200; `leaseUntil` is then the new end of the lease (UNIX
  // seconds). A 409 is "not the lease holder any more", and false.
  virtual bool extendLease(long long jobId, int seconds, uint32_t& leaseUntil) = 0;

  // POST /agent/v1/jobs/{id}/progress. `body` is what writeProgressReport() made: an
  // intermediate `data` and a note, kept on the job and replaced by the next; not a result.
  virtual bool postProgress(long long jobId, const char* body, size_t len) = 0;

  // POST /agent/v1/updates/{id}/result (task p1.15-61): the one result of an update the server
  // asked for, as `writeUpdateResult` wrote it. True when the server answered 200, which it also
  // does for a result it already has - the first one stands - so this is safe to retry, and the
  // loop does retry it at every heartbeat's turn until it is taken.
  //
  // `notFound` is the one 404: an id that is not this exit node's, or is no ask at all. It is the
  // other way a result stops being posted, and it is **not** a failure to back off from.
  //
  // **Defaulted on purpose**: a consumer with no `IUpdateProvider` is never asked to make this
  // call, so it need not implement it and its tree compiles unchanged.
  virtual bool postUpdateResult(long long updateId, const char* body, size_t len, bool& notFound) {
    (void)updateId;
    (void)body;
    (void)len;
    notFound = false;
    return false;
  }

  virtual bool hasConfig() const = 0;
  virtual const AgentConfig& config() const = 0;
  virtual const Outcome& last() const = 0;
  virtual const Call& lastCall() const = 0;
  // The 120-a-minute ceiling of the server's `agent` rate-limit policy, kept on this side too
  // (RateLimit.h): a call that would break it is not made.
  virtual const RateLimiter& limiter() const = 0;
};

}  // namespace mm
