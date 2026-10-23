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
#include "core/net/client.h"
#include "core/settings/settings.h"
#include "interfaces/clock.h"
#include "interfaces/http.h"
#include "interfaces/wall_clock.h"

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
  bool claimJobs(mm::ClaimedJob* jobs, int max, int& count) override;
  bool postJobResult(long long jobId, const char* body, std::size_t len) override;
  bool extendLease(long long jobId, int seconds, std::uint32_t& leaseUntil) override;
  bool postProgress(long long jobId, const char* body, std::size_t len) override;

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
  bool call(const char* endpoint, const std::uint8_t* body, std::size_t body_len, std::size_t bound,
            std::size_t& reply_len, Call& out);
  // A POST to /jobs/<id>/<action> (the result, the extend, the progress).
  bool callJob(long long jobId, const char* action, const std::uint8_t* body, std::size_t body_len,
               std::size_t& reply_len, Call& out);
  // The URL as the client is given it: url() and the endpoint. False when it does not fit.
  bool buildUrl(const char* endpoint);
  void offerServerTime(std::uint32_t epoch);
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
  core::cycle::AgentAttention* attention_ = nullptr;
  ResultGate* gate_ = nullptr;
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
