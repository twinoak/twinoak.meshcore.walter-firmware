// The server as the agent's loop and runner see it, over the modem (task p4-03): `mm::IUplink`
// (components/agent-core, agent/IUplink.h) as the seven HTTPS calls of meshmaster.dk's agent API
// (API §3), each **one request** of phase 3's client (core/net/client.h) over the modem's TLS,
// and each leaving behind the `Call` record the library's ServerPolicy reads. The reference
// implementation is the exit-node firmware's `AgentClient` (WiFi and HTTPClient); this is the
// same seven calls with the same answers, over the client this firmware already has. The report
// of this task maps each call to its route and each status to the policy's action.
//
// What is here and nowhere else:
//
//   - **the token's path.** `server.token` (core/settings/keys.h) is read into one buffer of
//     this object at the start of a call, `Bearer ` in front of it, handed to the client as the
//     Authorization value - a pointer, which the client copies into the request head alone -
//     and wiped the moment the client returns. The request is flagged *sensitive*
//     (core/net/client.h, Options::sensitive), so the transport wipes the head it wrote once it
//     is on the wire. The token is in no log line, no URL, no `Call::error` and no reply buffer;
//     host_test/agent/test_modem_uplink.cpp reads every one of those back.
//   - **the headers.** `Authorization: Bearer <token>`, `X-Agent-Version: <this firmware's
//     version>` (core::version(): the Walter's, not the library's), `Accept: application/json`,
//     and `Content-Type: application/json` on a POST. The body is the bytes the library wrote
//     (mm::writeHeartbeat, writeClaimRequest, writeExtendRequest; the runner's own for a result,
//     an observation batch and a progress report), carried unchanged. The reply is parsed by the
//     library's own parser and by nothing else (docs/library.md §4).
//   - **the `Call` record.** `reachable` is a reply with a status, whatever it said; `ok` is a
//     2xx whose body fit and parsed; `retryAfterS` is Retry-After as a count of seconds, 0 when
//     absent, unparseable or an HTTP date (the library's rule: mm::retryAfterMs turns a 0 into a
//     minute); `rateLimitRemaining` is X-RateLimit-Remaining or -1; `error` is one sentence made
//     of static text and numbers - never a byte of a body; `atMillis`/`atEpoch` are this
//     firmware's clocks; `via` is "lte".
//   - **the bounds.** A reply is read into one buffer of this object, 2 kB for the config, the
//     heartbeat, the observations, the result, the extend and the progress and 6 kB for a claim
//     (the figures docs/library.md §4 gives); a longer one is cut at the bound by the client and
//     the call is `ok = false` with a reason. The buffer is wiped after every call: a claim
//     carries a repeater's password, parsed in place into the job's own credential.
//   - **the deadline.** The owner of a wake gives this object the wake's uplink deadline
//     (setDeadline, on interfaces::Clock's milliseconds; the Walter's step 8 budget,
//     docs/ARCHITECTURE.md §10). Every call takes what is left of it as its total deadline; a
//     call that could not finish - less than kLeastCallMs left - is `reachable = false` with
//     *budget* in its error and touches the modem not at all. No deadline is the transport's
//     default (core/net/http_over_tls.h).
//   - **no redirect is followed.** A 3xx on an agent route is `ok = false` with the status: the
//     unit's server is one host and API §3 says the routes never redirect. `https://` only is
//     the client's rule and the settings table's (core/settings/keys.h, Kind::kUrl).
//   - **the ceiling on this side.** The library's RateLimiter (120 a minute, agent/RateLimit.h)
//     is taken before every call, as docs/library.md §4.2 asks; a call it holds back is not made.
//
// What is not here: when to call (task p4-05's loop behind the backend seam), what a heartbeat
// says (task p4-04's identity), the heap figure of an Outcome (the platform's, task p4-04) and
// the link's state as the modem knows it - the owner says `setLinkUp` when the uplink policy
// brought the modem up, and `link()` counts the ups and the downs it was told of.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object -
// which is why it is a namespace-scope object in main/ and never a local (about 9 kB of buffers).
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/ConfigReply.h>
#include <agent/Envelopes.h>
#include <agent/IPlatform.h>
#include <agent/IUplink.h>
#include <agent/JobEnvelopes.h>
#include <agent/RateLimit.h>

#include "core/cycle/agent_attention.h"
#include "core/cycle/wake_progress.h"
#include "core/cycle/wake_room.h"
#include "core/cycle/wake_counters.h"
#include "core/net/client.h"
#include "core/settings/settings.h"
#include "interfaces/backend.h"
#include "interfaces/clock.h"
#include "interfaces/http.h"
#include "interfaces/wall_clock.h"
#include "walter_agent/call_log.h"
#include "walter_agent/flood_counters.h"

namespace agent {

// The reply buffers' sizes, docs/library.md §4's: the config, heartbeat, observations, extend and
// progress answers are a few hundred bytes; a claim is two jobs with their credentials, paths and
// timeouts, or one config.apply whose plan is forty lines.
constexpr std::size_t kSmallReplyBytes = 2048;
constexpr std::size_t kClaimReplyBytes = 6144;

// The heartbeat's body, written here from the struct the loop filled: the reference agent's is
// 1024 and the contract fixture is 544 bytes; a unit with thirteen job types, a board, the two
// optional fields and a few error counters fits with room.
constexpr std::size_t kHeartbeatBodyBytes = 1536;

// How many `errors` entries one heartbeat may carry through here: the library's six and room for
// a few of this board's own behind them (task p10-03's `radioIdentityLost` is the first, and the
// five wake counters of tasks p13-02 and p15-06 are the next). A heartbeat with more than this
// keeps the ones the library wrote, because a counter is worth less than the heartbeat it would
// cost.
constexpr std::size_t kHeartbeatErrorsMax = 12;
static_assert(kHeartbeatErrorsMax >= 6 + 1 + core::cycle::kWakeCounterCount,
              "the library's six, the radio's refusal and the wake counters have to fit");

// The least that may be left of the wake's uplink deadline for a call to be begun: a connect and
// a TLS handshake over LTE-M are seconds each before a byte of the request goes, and a call begun
// with less than this ends in a timeout that cost the modem for nothing. TO BE SET ON THE BENCH
// (docs/bench.md §17): the first `agent probe` on a real network says what a call takes.
constexpr std::uint32_t kLeastCallMs = 5000;

// The route every call is under, and the longest path under it (`/agent/v1/jobs/<id>/progress`
// with a 19-digit id).
constexpr const char* kAgentRoute = "/agent/v1";
constexpr std::size_t kMaxEndpointLen = 48;

// `Bearer ` and the longest token the settings table takes.
constexpr std::size_t kBearerPrefixLen = 7;
constexpr std::size_t kBearerBytes = kBearerPrefixLen + core::settings::kMaxValueLen + 1;

// -- an extension job's payload (task p4-05) ---------------------------------------------------
//
// The copied library reads a claim into `mm::ClaimedJob`, which has named fields for the payloads
// of its own thirteen types and **no raw payload**: a `twinoak.fw.offer`'s `base`, an apply's
// `expectedSha256`, a power cycle's `offSeconds` would be gone the moment the claim is parsed
// (agent/JobEnvelopes.h; reported upstream in tasks/phase-4/reports/05.md). So before the claim's
// bytes go to the library's parser, the text of each `twinoak.*` job's `payload` object is kept
// here, by job id, for the extension to read (walter_agent/json_scan.h). The payloads are a few
// hundred bytes - `FirmwareOfferPayload.json` is 193 - and carry no credential: all four types are
// credential class `none`, and the claim's `credential` object is a sibling of the payload, never
// inside it. Wiped with the next claim.
constexpr std::size_t kExtensionPayloadBytes = 384;
constexpr const char kExtensionTypePrefix[] = "twinoak.";

struct KeptPayload {
  long long job_id = 0;
  char text[kExtensionPayloadBytes] = {};
};

// Whether a job's result is to be posted at all (task p4-05). The library's runner posts a result
// for every job it ran and has no way to leave one leased and silent; meshmaster.dk's
// unit-protocol.md §5 says a wake that ends in the middle of an offer's download posts **no**
// result and lets the lease expire, which re-queues the job without spending an attempt. The
// extension names such a job here and the uplink answers the runner's post with "taken" while
// sending nothing - the one honest reading of a library API that has no "nothing" (the report
// says so, and asks upstream for the word).
class ResultGate {
 public:
  virtual ~ResultGate() = default;
  virtual bool withhold(long long jobId) = 0;

  // **What this firmware holds that the library's buffer knows nothing of** (task p18-02): the
  // ids, into `out` and at most `max` of them, and how many were written.
  //
  // The claim's `holding` is filled by `JobRunner::claim` from the runner's own offline buffer -
  // results it made and has not had taken - and meshmaster.dk hands a claim that states the
  // member **every** job it has leased to this exit node, whose lease is still running, that has
  // no result, and whose id the claim did not name (its task p4.47-254,
  // `Transport/HandedBack.cs`). The server keeps no record of an answer that did not arrive, so
  // *whose answer was lost* is not a question it can ask. Three kinds of work of this firmware's
  // are leased and in hand and in no buffer of the library's - a job the result gate left to its
  // lease, a session the bound cut, an apply whose result waits for the next boot - and a wake
  // that claims more than once would be handed each of them back, mid-flight, by its own next
  // claim. Naming them here is how they are not.
  //
  // A default of none, so that a gate which only withholds - every one of them but the firmware
  // jobs - is unchanged.
  virtual int holdsOutsideTheBuffer(long long* out, int max) const {
    (void)out;
    (void)max;
    return 0;
  }
};

// What this object did, for the console and the tests. Saturating.
struct UplinkCounters {
  std::uint32_t calls = 0;           // every call the library asked for
  std::uint32_t not_configured = 0;  // refused: no server.url or no server.token
  std::uint32_t held = 0;            // refused: the 120-a-minute ceiling on this side
  std::uint32_t budget = 0;          // refused: too little of the wake's deadline left
  std::uint32_t requests = 0;        // reached the client: one request each
  std::uint32_t unreachable = 0;     // no status came back: DNS, TCP, TLS, a timeout, no uplink
  std::uint32_t replies = 0;         // a status came back
  std::uint32_t ok = 0;              // 2xx, the body whole and parsed
  std::uint32_t denied = 0;          // 401, 403
  std::uint32_t later = 0;           // 429, 5xx
  std::uint32_t redirected = 0;      // a 3xx, which an agent route never answers
  std::uint32_t cut = 0;             // a 2xx whose body passed the bound
  std::uint32_t unparsed = 0;        // a 2xx the library's parser refused
  std::uint32_t payloads_kept = 0;   // extension payloads kept out of claims (task p4-05)
  std::uint32_t payloads_too_long = 0;  // ... and ones that did not fit kExtensionPayloadBytes
  std::uint32_t withheld = 0;        // results not posted because the extension left the job leased
};

class ModemUplink final : public mm::IUplink {
 public:
  // `version` is what X-Agent-Version carries: core::version(), a static string.
  ModemUplink(core::net::Client& http, core::settings::Settings& settings, interfaces::Clock& clock,
              interfaces::WallClock& wall, const char* version);

  // ------------------------------------------------------------------ what the owner tells it

  // The modem's state, as whoever drives it says (the uplink policy's outcome at a wake's step
  // 6, or `lte up` on the console). Counted: connects on every down-to-up, drops on up-to-down.
  void setLinkUp(bool up);
  // The wake's uplink deadline on interfaces::Clock's milliseconds; 0 is none. Every call from
  // here on takes what is left of it as its total deadline.
  void setDeadline(std::uint64_t deadline_ms) { deadline_ms_ = deadline_ms; }
  std::uint64_t deadline() const { return deadline_ms_; }
  // **Whether the radio has lost its identity** (task p10-03), as the identity read before this
  // wake found it (walter_agent/identity.h, `identityLost()`). The library builds the heartbeat
  // and its six counters itself (agent/AgentLoop.cpp's `doHeartbeat` writes `hb.errors` whole,
  // so a board cannot add one there), and this is the seam the bytes pass through: while it is
  // set, the body carries `radioIdentityLost: 1` behind the library's own six and the exit
  // node's page shows it among its *Counters*. Set at every wake by whoever read the identity;
  // nothing else changes about the body.
  void setRadioIdentityLost(bool lost) { radio_identity_lost_ = lost; }
  bool radioIdentityLost() const { return radio_identity_lost_; }
  // **What woke this unit** (task p13-02), as core/cycle/wake_counters.h has counted it. The same
  // seam as the line above and for the same reason: the library writes `hb.errors` whole, so this
  // is the one place a counter of ours can be added to the body that goes out. Held by pointer -
  // the object outlives this one - and null is a build with no settings store, whose heartbeat is
  // byte for byte what it was.
  void countWakesFrom(const core::cycle::WakeTotals* wakes) { wakes_ = wakes; }
  // **The three counts of a flooded transaction** (task p14-02, walter_agent/flood_counters.h).
  // Not an `errors` entry but the heartbeat's own `flood` object, and the same arrangement for a
  // different reason: the library builds that object out of the mesh's scope and its own
  // *since-boot* counts (agent/AgentLoop.cpp's `buildHeartbeat`), and this board boots at every
  // wake - so this is where the kept totals are swapped in, leaving the member, its order and
  // the other two fields exactly as the library wrote them. Held by pointer, so that a heartbeat
  // carries the totals as they stand when it is built; null leaves the library's own counts,
  // which is a build with nothing keeping them.
  void countFloodsFrom(const FloodTotals* floods) { floods_ = floods; }
  // **Where a call that did not come back 2xx is written down** (task p13-02,
  // walter_agent/call_log.h). Every call the library asks for passes through `call()` below, so
  // this is the one place the list can be kept from. Null keeps none.
  void recordFailuresIn(CallLog* calls) { calls_ = calls; }
  CallLog* callLog() const { return calls_; }
  // **Where the call that is out is written down while it is out** (task p15-06,
  // core/cycle/wake_progress.h). The same one place as the line above, and for the same reason:
  // every call the library asks for passes through `call()` below. A wake ended inside one - the
  // 30-second command timeout of a failing observations post is the shape this unit has - leaves
  // the call's name in RTC memory, and the boot after says it. Null writes nothing down.
  void writeProgressTo(core::cycle::WakeProgress* progress) { progress_ = progress; }
  // Where a 401 is raised (task p4-04): the Walter's own attention, for the next wake record and
  // for `status`. A null one is a build with no duty cycle, and then a 401 is a log line and the
  // `Call` record and nothing more.
  void raiseAttentionThrough(core::cycle::AgentAttention* attention) { attention_ = attention; }
  // The library's wall clock, when the platform has one (task p4-04): the server time a config
  // reply and a claim carry is offered to it, as the reference agent does. The heartbeat's is the
  // loop's own business. nullptr is "nobody to tell".
  void setClock(mm::IClock* clock) { agent_clock_ = clock; }
  // Who may withhold a result (task p4-05): the firmware jobs. nullptr posts every result.
  void setResultGate(ResultGate* gate) { gate_ = gate; }

  // **The bound on the wake these calls are being made in** (task p16-06,
  // `core/cycle/wake_room.h`). Two things come of it, and `core/cycle/wake_room.h` says why:
  //
  //   every call takes what is left of the **sooner** of the deadline above and the bound, so a
  //   call is never begun that the wake has no room to finish and the wake is never reset in the
  //   middle of one;
  //   a claim raises the bound to cover the leases it took on, and cuts a lease the ceiling
  //   cannot cover down to where the room for the result's call begins.
  //
  // The guard and not a room object: the arithmetic is a value built on the stack where it is
  // needed, and this is the one pointer that costs anything. Null is a build with no duty cycle -
  // every test that makes an exchange by hand - and then nothing of this applies and a call takes
  // what is left of the deadline, as it did before this task.
  void boundedBy(core::cycle::WakeGuard* guard) { guard_ = guard; }

  // **What this wake's claim did to its bound, and what it then had no room for** (task p16-06),
  // for the record and for nothing this object decides. **Three bytes**, and not a whole
  // `core::cycle::Cover`: every four bytes of this object are four of the heap the agent's kept
  // scratch is carved out of (tools/heap_at_start.py), and these are the only figures anything
  // reads after the claim. *How far the bound reaches* is the bound itself, which the wake record
  // already carries, and *where a lease was cut to* is said in the log line that cuts it.
  //
  // **The fourth byte is why this wake stopped claiming, where this end stopped it** (task
  // p18-02): it sits in the padding the three above left, so it costs nothing.
  struct WakesRoom {
    std::uint8_t jobs = 0;     // claimed jobs the bound was raised to cover; 0: nothing was
    std::uint8_t no_room = 0;  // calls not begun for want of room, saturating
    bool cut = false;          // a lease was longer than the wake could be made to cover
    interfaces::Claims stop = interfaces::Claims::kUnsaid;  // `stopClaiming` below
  };
  const WakesRoom& wakesRoom() const { return room_; }
  // At the top of every wake, so that the figures of the wake before are not reported as this
  // one's - and the latch below with them, which is why it is cleared here and nowhere else.
  void forgetTheWakesRoom() { room_ = WakesRoom{}; }

  // **No more claims this wake, and why** (task p18-02). The library asks for another claim when
  // the answer to the last result said more work waits (`jobsWaiting`, upstream's task p1.20-69),
  // and this firmware writes that bit: so the one place a board can end the asking without
  // failing a call is here, where that answer is read. The first reason latches - what ended the
  // claiming is what ended it first - and `forgetTheWakesRoom()` clears it at the top of a wake.
  //
  // Three callers, and all three are *this wake cannot take on more work*, never *the server said
  // so*: the result gate leaving a job to its lease for want of budget (the firmware jobs), a job
  // that took the radio away and did not give it back (the same), and the bound that cannot be
  // raised far enough to cover another claim (this object, below). None of them is a failure: no
  // call is made, nothing backs off, and the wake runs its remaining steps and sleeps.
  void stopClaiming(interfaces::Claims why) {
    if (room_.stop == interfaces::Claims::kUnsaid) room_.stop = why;
  }
  interfaces::Claims claimsStopped() const { return room_.stop; }

  // The payload text of a `twinoak.*` job of the last claim, by id, or nullptr: what the
  // extension reads its fields from (task p4-05). Empty text is a payload that did not fit.
  const char* payloadOf(long long jobId) const;
  int payloadsKept() const { return payload_count_; }

  // ------------------------------------------------------------------ what the console reads

  // `server.url` as it is used: the stored value with any trailing slash off; "" when unset.
  const char* url();
  const char* version() const { return version_; }
  bool tokenSet() const { return settings_.isSet(core::settings::Key::kServerToken); }
  const UplinkCounters& uplinkCounters() const { return counters_; }
  // The last call's endpoint (the path under the URL, no query, no token) and its length.
  const char* lastEndpoint() const { return endpoint_; }
  std::uint32_t lastElapsedMs() const { return last_elapsed_ms_; }
  std::uint32_t lastReplyBytes() const { return last_reply_bytes_; }

  // ------------------------------------------------------------------ mm::IUplink

  bool configured() const override;
  LinkState link() const override { return link_; }

  bool fetchConfig() override;
  bool postHeartbeat(const mm::Heartbeat& hb, mm::HeartbeatReply& reply) override;
  bool postObservations(const char* body, std::size_t len, int& accepted, bool& duplicate) override;
  bool claimJobs(mm::ClaimedJob* jobs, int max, int& count, int transactionsLeft,
                 const long long* holding, int holdingCount) override;
  bool postJobResult(long long jobId, const char* body, std::size_t len, bool& jobsWaiting) override;
  bool extendLease(long long jobId, int seconds, std::uint32_t& leaseUntil) override;
  bool postProgress(long long jobId, const char* body, std::size_t len) override;

  // ------------------------------------------------------------------ and one call of our own

  // **A scanner's lines on their way to the server** (task p16-04): `POST
  // /agent/v1/jobs/{id}/lines/{from}`. It is not one of `mm::IUplink`'s seven - the copied library
  // has no notion of it and is not changed - so it is declared here and goes out through the same
  // `call()` as every other one: the same token path, the same wake deadline, the same
  // 120-a-minute ceiling, the same `Call` record, the same line in the failed-call list and the
  // same name written down while it is out (`core/cycle/wake_progress.h`).
  //
  // `body` is what the scanner said, its lines as they were sent and each ended with `\n`. It is
  // read where it lies - the session's room - and nothing of it is kept here or logged (S-12).
  // True when the server answered 2xx and the reply parsed; `out` is then what it said.
  struct LinesReply {
    std::uint32_t held = 0;  // how many lines of this job the server holds now
    bool full = false;       // the job's room is used up: the unit posts no more and goes on
  };
  bool postLines(long long jobId, std::uint32_t from, const char* body, std::size_t len,
                 LinesReply* out);

  bool hasConfig() const override { return has_config_; }
  const mm::AgentConfig& config() const override { return config_; }
  const Outcome& last() const override { return last_; }
  const Call& lastCall() const override { return last_call_; }
  const mm::RateLimiter& limiter() const override { return limiter_; }

 private:
  // One call: the checks before anything is sent, the token into the header and out of it, the
  // request, and the Call record. `endpoint` is the path under kAgentRoute; `body` nullptr is a
  // GET. True when a status came back (`out.reachable`); the reply's bytes are in reply_,
  // `reply_len` of them, NUL-terminated.
  // `what` is which of the eight this is, for the failed-call list (task p13-02): it is passed in
  // rather than worked out from `endpoint`, because a list that names the wrong call is worse than
  // no list and a path is not a name.
  bool call(CallKind what, const char* endpoint, const std::uint8_t* body, std::size_t body_len,
            std::size_t bound, std::size_t& reply_len, Call& out);
  // A POST to /jobs/<id>/<action> (the result, the extend, the progress).
  bool callJob(CallKind what, long long jobId, const char* action, const std::uint8_t* body,
               std::size_t body_len, std::size_t& reply_len, Call& out);
  // The URL as the client is given it: url() and the endpoint. False when it does not fit.
  bool buildUrl(const char* endpoint);
  // One failed call into the list, when this object was given one.
  void noteFailedCall(CallKind what, std::uint16_t status, interfaces::HttpOutcome outcome,
                      std::uint32_t at_epoch);
  // And one that came back 2xx whole, into the record the list keeps beside it in RAM (task
  // p15-07): the other half of the line above, in the one place every call passes through.
  void noteAnsweredCall(CallKind what);
  void offerServerTime(std::uint32_t epoch);
  // The wake's bound raised to cover a claim's leases, and a lease cut where it cannot be (task
  // p16-06). Called by `claimJobs` with the jobs as the library parsed them, before the runner
  // sees one.
  void coverTheLeases(mm::ClaimedJob* jobs, int count);
  // **Whether this wake could cover another claim at all** (task p18-02): the claim's own call
  // and the calls after its jobs - a result for each and the observations - inside the most a
  // wake may ever be raised to. The jobs' own time is not in it: a lease the ceiling cannot cover
  // is cut and the library ends that job `timeout` with the room still there to post the result
  // (`core/cycle/wake_room.h`), which is a job answered and not a job lost. What this refuses is
  // a claim the wake could not even post the results of.
  bool roomForAnotherClaim() const;
  // The claim's `holding` as it goes on the wire: the library's own list, then the ids this
  // firmware holds outside its buffer that are not already in it (`ResultGate` above). `out` is
  // `mm::kClaimHoldingMax` long, which is as many as the library's writer will write.
  int holdingForTheClaim(const long long* holding, int holdingCount, long long* out) const;
  void wipeSecret();
  void wipeReply();
  // The `twinoak.*` jobs' payloads out of a claim's bytes, before the library parses them in
  // place (task p4-05).
  void keepPayloads(const char* claim, std::size_t len);

  core::net::Client& http_;
  core::settings::Settings& settings_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;
  const char* version_;
  mm::IClock* agent_clock_ = nullptr;

  LinkState link_;
  std::uint64_t deadline_ms_ = 0;
  bool radio_identity_lost_ = false;
  // **Here, and the guard below with the other pointers** (task p16-06). Every four bytes of this
  // object are four of the heap the agent's kept scratch is carved out of
  // (tools/heap_at_start.py), and these two cost none: three bytes of the padding behind that
  // bool, and the four the eight-byte alignment of `payloads_` left behind the pointers.
  WakesRoom room_{};
  const core::cycle::WakeTotals* wakes_ = nullptr;
  const FloodTotals* floods_ = nullptr;
  CallLog* calls_ = nullptr;
  core::cycle::WakeProgress* progress_ = nullptr;
  core::cycle::AgentAttention* attention_ = nullptr;
  ResultGate* gate_ = nullptr;
  core::cycle::WakeGuard* guard_ = nullptr;
  KeptPayload payloads_[mm::kClaimMax];
  int payload_count_ = 0;

  // The one buffer of this object the token is copied into: `Bearer <token>`, wiped before a
  // call returns. A member and not a stack buffer, so that a test can read it back - and because
  // the main task's stack is small (CONFIG_ESP_MAIN_TASK_STACK_SIZE 3584).
  char secret_[kBearerBytes] = {};
  char url_[core::settings::kMaxUrlLen + 1] = {};
  char full_url_[core::settings::kMaxUrlLen + kMaxEndpointLen + 1] = {};
  char endpoint_[kMaxEndpointLen + 1] = {};
  char request_[kHeartbeatBodyBytes] = {};
  char reply_[kClaimReplyBytes + 1] = {};

  mm::RateLimiter limiter_;
  mm::AgentConfig config_;
  bool has_config_ = false;
  Outcome last_;
  Call last_call_;
  UplinkCounters counters_;
  std::uint32_t last_elapsed_ms_ = 0;
  std::uint32_t last_reply_bytes_ = 0;
};

}  // namespace agent
