#include "walter_agent/modem_uplink.h"

#include <cstdio>
#include <cstring>

#include <agent/Credentials.h>
#include <agent/Log.h>

#include "walter_agent/identity.h"
#include "walter_agent/json_scan.h"

namespace agent {

namespace {

void zeroBytes(void* at, std::size_t size) {
  auto* out = static_cast<volatile std::uint8_t*>(at);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = 0;
  }
}

std::uint32_t saturate(std::uint32_t value) {
  return value == 0xFFFFFFFFu ? value : value + 1;
}

// The reply, into the caller's buffer: what fits, NUL-terminated. The client's own bound
// (core/net/client.h, Options::bound) is what cuts a long body and names it kTooLarge; this
// refuses a piece only if that bound were ever wider than the buffer, which is a failed 2xx.
class ReplySink final : public interfaces::HttpBodySink {
 public:
  ReplySink(char* buffer, std::size_t capacity) : buffer_(buffer), capacity_(capacity) {}
  bool piece(const std::uint8_t* data, std::size_t size) override {
    if (size > capacity_ - length_) return false;
    std::memcpy(buffer_ + length_, data, size);
    length_ += size;
    buffer_[length_] = 0;
    return true;
  }
  std::size_t length() const { return length_; }

 private:
  char* buffer_;
  std::size_t capacity_;
  std::size_t length_ = 0;
};

// Retry-After as a count of seconds: digits as they stand, bounded where the client bounds its
// own reading (core/net/client.h, kRetryAfterCapS); **0** when the header is absent, is not all
// digits, or is an HTTP date - the rule docs/library.md §4 gives, because mm::retryAfterMs turns
// a 0 into the minute a date is honoured as. (The reference AgentClient answers 60 for a date,
// which is the same minute by another road.)
std::uint32_t retryAfterSeconds(const interfaces::HttpWantedHeader& header) {
  if (!header.present || header.truncated || header.value == nullptr || header.value[0] == 0) return 0;
  std::uint64_t value = 0;
  std::size_t digits = 0;
  for (const char* p = header.value; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return 0;
    value = value * 10 + static_cast<std::uint64_t>(*p - '0');
    if (++digits > 9) return core::net::kRetryAfterCapS;
  }
  return value > core::net::kRetryAfterCapS ? core::net::kRetryAfterCapS : static_cast<std::uint32_t>(value);
}

// X-RateLimit-Remaining: a whole number, or -1 when the answer did not carry one it could read.
int rateLimitRemaining(const interfaces::HttpWantedHeader& header) {
  if (!header.present || header.truncated || header.value == nullptr || header.value[0] == 0) return -1;
  long value = 0;
  std::size_t digits = 0;
  for (const char* p = header.value; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return -1;
    value = value * 10 + (*p - '0');
    if (++digits > 9) return -1;
  }
  return static_cast<int>(value);
}

bool isSuccess(std::uint16_t status) { return status >= 200 && status < 300; }
bool isRedirect(std::uint16_t status) { return status >= 300 && status < 400; }

}  // namespace

ModemUplink::ModemUplink(core::net::Client& http, core::settings::Settings& settings, interfaces::Clock& clock,
                         interfaces::WallClock& wall, const char* version)
    : http_(http), settings_(settings), clock_(clock), wall_(wall), version_(version != nullptr ? version : "") {}

// ---------------------------------------------------------------------------- an extension job's payload

void ModemUplink::keepPayloads(const char* claim, std::size_t len) {
  for (int i = 0; i < mm::kClaimMax; ++i) {
    payloads_[i].job_id = 0;
    zeroBytes(payloads_[i].text, sizeof payloads_[i].text);
  }
  payload_count_ = 0;
  const json::Value jobs = json::member(claim, len, "jobs");
  for (int i = 0; i < mm::kClaimMax; ++i) {
    const json::Value job = json::element(jobs, i);
    if (!job.found()) break;
    // Wide enough for the whole type as the claim carries it - `twinoak.power.cycle` is nineteen
    // characters, longer than the runner's own `kMaxJobTypeLen` buffer, which truncates it.
    char type[40] = {};
    long long id = 0;
    if (!json::text(json::member(job.at, job.len, "type"), type, sizeof type)) continue;
    if (!json::integer(json::member(job.at, job.len, "id"), &id)) continue;
    if (std::strncmp(type, kExtensionTypePrefix, sizeof kExtensionTypePrefix - 1) != 0) continue;
    KeptPayload& kept = payloads_[payload_count_++];
    kept.job_id = id;
    const json::Value payload = json::member(job.at, job.len, "payload");
    if (payload.found() && payload.len < sizeof kept.text) {
      std::memcpy(kept.text, payload.at, payload.len);
      kept.text[payload.len] = 0;
      counters_.payloads_kept = saturate(counters_.payloads_kept);
    } else if (payload.found()) {
      counters_.payloads_too_long = saturate(counters_.payloads_too_long);
      LOG_W("server: job %lld %s: a payload of %lu bytes is longer than the %lu this firmware keeps",
            id, type, static_cast<unsigned long>(payload.len),
            static_cast<unsigned long>(sizeof kept.text - 1));
    }
  }
}

const char* ModemUplink::payloadOf(long long jobId) const {
  for (int i = 0; i < payload_count_; ++i) {
    if (payloads_[i].job_id == jobId) return payloads_[i].text;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------- what the owner tells it

void ModemUplink::setLinkUp(bool up) {
  if (up && !link_.up) link_.connects = saturate(link_.connects);
  if (!up && link_.up) link_.drops = saturate(link_.drops);
  link_.up = up;
}

// ---------------------------------------------------------------------------- the URL

const char* ModemUplink::url() {
  // The stored URL, read at every call rather than kept: `set server.url` at the console changes
  // it at once (core/settings/keys.h, the key's `effect`), and the store's own buffer is what is
  // read. A trailing slash is not part of the origin.
  const char* stored = settings_.serverUrl();
  std::size_t n = std::strlen(stored);
  if (n > core::settings::kMaxUrlLen) n = core::settings::kMaxUrlLen;
  std::memcpy(url_, stored, n);
  url_[n] = 0;
  while (n > 0 && url_[n - 1] == '/') url_[--n] = 0;
  return url_;
}

bool ModemUplink::buildUrl(const char* endpoint) {
  const char* base = url();
  const int n = std::snprintf(full_url_, sizeof full_url_, "%s%s%s", base, kAgentRoute, endpoint);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof full_url_) {
    full_url_[0] = 0;
    return false;
  }
  std::snprintf(endpoint_, sizeof endpoint_, "%s%s", kAgentRoute, endpoint);
  return true;
}

bool ModemUplink::configured() const {
  return settings_.serverUrl()[0] != 0 && settings_.isSet(core::settings::Key::kServerToken);
}

// ---------------------------------------------------------------------------- the secret, and the reply

void ModemUplink::wipeSecret() { zeroBytes(secret_, sizeof secret_); }

void ModemUplink::wipeReply() {
  zeroBytes(reply_, sizeof reply_);
  zeroBytes(request_, sizeof request_);
}

void ModemUplink::offerServerTime(std::uint32_t epoch) {
  if (agent_clock_ == nullptr || epoch == 0) return;
  std::int32_t difference = 0;
  (void)agent_clock_->takeServerTime(epoch, difference);
}

// ---------------------------------------------------------------------------- one call

bool ModemUplink::call(const char* endpoint, const std::uint8_t* body, std::size_t body_len, std::size_t bound,
                       std::size_t& reply_len, Call& out) {
  reply_len = 0;
  // The reply buffer alone: the heartbeat's body is in request_ and goes out from there.
  zeroBytes(reply_, sizeof reply_);
  counters_.calls = saturate(counters_.calls);

  out = Call();
  out.via = "lte";
  out.atMillis = static_cast<std::uint32_t>(clock_.nowMs());
  interfaces::WallTime now{};
  out.atEpoch = wall_.now(&now) ? now.epoch : 0;

  if (!configured()) {
    counters_.not_configured = saturate(counters_.not_configured);
    std::snprintf(out.error, sizeof out.error, "not configured: server.url and server.token are needed");
    last_call_ = out;
    return false;
  }
  if (!buildUrl(endpoint)) {
    std::snprintf(out.error, sizeof out.error, "the URL does not fit: server.url is too long for this route");
    last_call_ = out;
    return false;
  }
  if (!limiter_.take(out.atMillis)) {
    counters_.held = saturate(counters_.held);
    const std::uint32_t wait = limiter_.waitMs(out.atMillis);
    std::snprintf(out.error, sizeof out.error, "held back by the 120-a-minute ceiling for %lu s",
                  static_cast<unsigned long>((wait + 999) / 1000));
    LOG_W("server: %s", out.error);
    last_call_ = out;
    return false;
  }

  // The wake's deadline: what is left of it is this call's whole, and too little is no call.
  std::uint32_t total_ms = 0;
  if (deadline_ms_ != 0) {
    const std::uint64_t now_ms = clock_.nowMs();
    const std::uint64_t left = now_ms < deadline_ms_ ? deadline_ms_ - now_ms : 0;
    if (left < kLeastCallMs) {
      counters_.budget = saturate(counters_.budget);
      std::snprintf(out.error, sizeof out.error,
                    "budget: %lu ms left of the wake's uplink deadline, and a call needs %lu to begin",
                    static_cast<unsigned long>(left), static_cast<unsigned long>(kLeastCallMs));
      LOG_W("server: %s %s", endpoint_, out.error);
      last_call_ = out;
      return false;
    }
    total_ms = static_cast<std::uint32_t>(left);
  }

  // The token: the store -> this one buffer -> the request head, and wiped. The pointer the
  // client is given is this buffer's, and the client copies it into the head and nowhere else.
  std::snprintf(secret_, sizeof secret_, "Bearer %s", settings_.serverToken());

  interfaces::HttpHeader headers[2] = {};
  headers[0] = interfaces::HttpHeader{"X-Agent-Version", version_};
  headers[1] = interfaces::HttpHeader{"Accept", "application/json"};
  char retry_after[32] = {};
  char remaining[16] = {};
  interfaces::HttpWantedHeader wanted[2] = {};
  wanted[0].name = "Retry-After";
  wanted[0].value = retry_after;
  wanted[0].capacity = sizeof retry_after;
  wanted[1].name = "X-RateLimit-Remaining";
  wanted[1].value = remaining;
  wanted[1].capacity = sizeof remaining;

  core::net::Options options;
  options.authorization = secret_;
  options.sensitive = true;
  options.headers = headers;
  options.header_count = 2;
  options.wanted = wanted;
  options.wanted_count = 2;
  options.bound = bound;
  options.total_ms = total_ms;
  options.follow_redirects = false;

  ReplySink sink(reply_, sizeof reply_ - 1);
  counters_.requests = saturate(counters_.requests);
  const core::net::Result r = body == nullptr
                                  ? http_.get(full_url_, options, sink)
                                  : http_.post(full_url_, body, body_len, "application/json", options, sink);
  wipeSecret();

  reply_len = sink.length();
  last_elapsed_ms_ = r.elapsed_ms;
  last_reply_bytes_ = static_cast<std::uint32_t>(reply_len);

  if (r.verdict == core::net::Verdict::kRefusedUrl) {
    counters_.unreachable = saturate(counters_.unreachable);
    std::snprintf(out.error, sizeof out.error, "the URL was refused before anything was sent: %.70s", r.why);
    LOG_E("server: %s %s", endpoint_, out.error);
    last_call_ = out;
    return false;
  }
  if (r.status == 0) {
    // No status line came back: DNS, TCP, the handshake, a timeout, no uplink. The transport's
    // sentence names which, and never a byte of anything the server said.
    counters_.unreachable = saturate(counters_.unreachable);
    std::snprintf(out.error, sizeof out.error, "no reply: %.90s%s", interfaces::describe(r.outcome),
                  r.outcome == interfaces::HttpOutcome::kTimeout && total_ms != 0 ? " (the wake's budget)" : "");
    LOG_W("server: %s -> %s after %lu ms", endpoint_, out.error, static_cast<unsigned long>(r.elapsed_ms));
    last_call_ = out;
    return false;
  }

  counters_.replies = saturate(counters_.replies);
  out.reachable = true;
  out.httpStatus = r.status;
  out.retryAfterS = retryAfterSeconds(wanted[0]);
  out.rateLimitRemaining = rateLimitRemaining(wanted[1]);

  if (isSuccess(r.status)) {
    if (r.verdict == core::net::Verdict::kTooLarge) {
      counters_.cut = saturate(counters_.cut);
      std::snprintf(out.error, sizeof out.error, "http %u but the reply is longer than %lu bytes and was cut there",
                    static_cast<unsigned>(r.status), static_cast<unsigned long>(bound));
    } else if (r.verdict != core::net::Verdict::kOk) {
      std::snprintf(out.error, sizeof out.error, "http %u but the body did not arrive whole: %.70s",
                    static_cast<unsigned>(r.status), r.why);
    } else {
      out.ok = true;
      counters_.ok = saturate(counters_.ok);
    }
  } else if (r.status == 401) {
    counters_.denied = saturate(counters_.denied);
    std::snprintf(out.error, sizeof out.error, "http 401: the token was refused");
    // The Walter's own attention (task p4-04): the library halts the loop on a 401 and nothing
    // more is sent until a person types `set server.token`, so without this the only trace of it
    // would be a log line the next deep sleep forgets. Nothing of the radio is touched
    // (core/cycle/agent_attention.h).
    if (attention_ != nullptr) attention_->raiseTokenRefused();
  } else if (r.status == 403) {
    counters_.denied = saturate(counters_.denied);
    std::snprintf(out.error, sizeof out.error, "http 403: this unit is refused (paused, or not its route)");
  } else if (r.status == 429) {
    counters_.later = saturate(counters_.later);
    std::snprintf(out.error, sizeof out.error, "http 429: rate limited; Retry-After %lu s",
                  static_cast<unsigned long>(out.retryAfterS));
  } else if (r.status >= 500) {
    counters_.later = saturate(counters_.later);
    std::snprintf(out.error, sizeof out.error, "http %u: the server is not well; Retry-After %lu s",
                  static_cast<unsigned>(r.status), static_cast<unsigned long>(out.retryAfterS));
  } else if (isRedirect(r.status)) {
    counters_.redirected = saturate(counters_.redirected);
    std::snprintf(out.error, sizeof out.error, "http %u: a redirect, which an agent route never answers and "
                  "this unit never follows", static_cast<unsigned>(r.status));
  } else {
    // Any other status: the client's own sentence, which names the status, the route's file and
    // what the server said about it in problem details (task p10-05, core/net/client.h). It
    // carries no token: a `detail` that held one is dropped there.
    std::snprintf(out.error, sizeof out.error, "%.120s", r.why);
  }
  if (out.ok) {
    LOG_I("server: %s -> http %u, %lu bytes, %lu ms", endpoint_, static_cast<unsigned>(r.status),
          static_cast<unsigned long>(reply_len), static_cast<unsigned long>(r.elapsed_ms));
  } else {
    LOG_W("server: %s -> %s (%lu ms)", endpoint_, out.error, static_cast<unsigned long>(r.elapsed_ms));
  }
  last_call_ = out;
  return true;
}

bool ModemUplink::callJob(long long jobId, const char* action, const std::uint8_t* body, std::size_t body_len,
                          std::size_t& reply_len, Call& out) {
  char endpoint[kMaxEndpointLen];
  std::snprintf(endpoint, sizeof endpoint, "/jobs/%lld/%s", jobId, action);
  return call(endpoint, body, body_len, kSmallReplyBytes, reply_len, out);
}

// ---------------------------------------------------------------------------- the seven calls

bool ModemUplink::fetchConfig() {
  std::size_t len = 0;
  Call c;
  const bool reachable = call("/config", nullptr, 0, kSmallReplyBytes, len, c);

  Outcome o;
  o.attempted = true;
  o.atMillis = c.atMillis;
  o.atEpoch = c.atEpoch;
  o.via = c.via;
  o.httpStatus = c.httpStatus;
  std::snprintf(o.error, sizeof o.error, "%s", c.error);
  if (!reachable || !c.ok) {
    last_ = o;
    wipeReply();
    return false;
  }

  mm::AgentConfig cfg;
  char err[64] = {};
  if (!mm::parseAgentConfig(reply_, len, cfg, err, sizeof err)) {
    counters_.unparsed = saturate(counters_.unparsed);
    last_call_.ok = false;
    std::snprintf(last_call_.error, sizeof last_call_.error, "http %d but the reply did not parse: %s",
                  c.httpStatus, err);
    std::snprintf(o.error, sizeof o.error, "%s", last_call_.error);
    LOG_E("server: config: %s", last_call_.error);
    last_ = o;
    wipeReply();
    return false;
  }
  offerServerTime(cfg.serverTimeUnix);
  o.ok = true;
  config_ = cfg;
  has_config_ = true;
  last_ = o;
  wipeReply();
  LOG_I("server: config ok: agent %lld, transport %s, budget %d/min gap %d s, poll %d s", cfg.agentId,
        cfg.transport, cfg.budget.maxTransactionsPerMinute, cfg.budget.minGapSeconds, cfg.pollIntervalS);
  return true;
}

bool ModemUplink::postHeartbeat(const mm::Heartbeat& hb, mm::HeartbeatReply& reply) {
  wipeReply();
  // This board's own counter behind the library's (task p10-03): a copy of the heartbeat with one
  // more `errors` entry while the radio refuses, and the heartbeat itself untouched otherwise -
  // the library owns `hb.errors` and writes the array whole at every call, so this is where a
  // counter of ours can be added at all. `addRadioIdentityLost` is shared with the console's
  // `agent heartbeat`, so the printed body and the posted one carry the same key.
  mm::Heartbeat sending = hb;
  mm::ErrorCounter errors[kHeartbeatErrorsMax];
  if (radio_identity_lost_ && hb.errors != nullptr && hb.errorCount >= 0 &&
      hb.errorCount < static_cast<int>(kHeartbeatErrorsMax)) {
    for (int i = 0; i < hb.errorCount; ++i) errors[i] = hb.errors[i];
    sending.errors = errors;
    sending.errorCount = addRadioIdentityLost(errors, hb.errorCount,
                                              static_cast<int>(kHeartbeatErrorsMax), true);
  }
  const std::size_t body_len = mm::writeHeartbeat(sending, request_, sizeof request_);
  if (body_len == 0) {
    last_call_ = Call();
    last_call_.via = "lte";
    std::snprintf(last_call_.error, sizeof last_call_.error, "the heartbeat body did not fit in %lu bytes",
                  static_cast<unsigned long>(sizeof request_));
    LOG_E("server: %s", last_call_.error);
    return false;
  }
  std::size_t len = 0;
  Call c;
  if (!call("/heartbeat", reinterpret_cast<const std::uint8_t*>(request_), body_len, kSmallReplyBytes, len, c) ||
      !c.ok) {
    wipeReply();
    return false;
  }
  char err[64] = {};
  if (!mm::parseHeartbeatReply(reply_, len, reply, err, sizeof err)) {
    counters_.unparsed = saturate(counters_.unparsed);
    last_call_.ok = false;
    std::snprintf(last_call_.error, sizeof last_call_.error, "http %d but the reply did not parse: %s",
                  c.httpStatus, err);
    LOG_E("server: heartbeat: %s", last_call_.error);
    wipeReply();
    return false;
  }
  wipeReply();
  return true;
}

bool ModemUplink::postObservations(const char* body, std::size_t len, int& accepted, bool& duplicate) {
  accepted = 0;
  duplicate = false;
  std::size_t reply_len = 0;
  Call c;
  if (!call("/observations", reinterpret_cast<const std::uint8_t*>(body), len, kSmallReplyBytes, reply_len, c) ||
      !c.ok) {
    wipeReply();
    return false;
  }
  mm::ObservationsReply r;
  char err[64] = {};
  if (!mm::parseObservationsReply(reply_, reply_len, r, err, sizeof err)) {
    counters_.unparsed = saturate(counters_.unparsed);
    last_call_.ok = false;
    std::snprintf(last_call_.error, sizeof last_call_.error, "http %d but the reply did not parse: %s",
                  c.httpStatus, err);
    LOG_E("server: observations: %s", last_call_.error);
    wipeReply();
    return false;
  }
  accepted = r.accepted;
  duplicate = r.duplicate;
  wipeReply();
  return true;
}

bool ModemUplink::claimJobs(mm::ClaimedJob* jobs, int max, int& count) {
  count = 0;
  char request[32] = {};
  const std::size_t request_len = mm::writeClaimRequest(max, request, sizeof request);
  std::size_t len = 0;
  Call c;
  if (!call("/jobs/claim", reinterpret_cast<const std::uint8_t*>(request), request_len, kClaimReplyBytes, len, c) ||
      !c.ok) {
    wipeReply();
    return false;
  }
  // The `twinoak.*` jobs' payloads first, out of the bytes as they arrived (task p4-05): the
  // library's parser keeps none of an extension type's payload, and it parses in place.
  keepPayloads(reply_, len);
  // The claim carries a repeater's password: parsed in place, the job keeps the one copy it
  // needs, and the reply's bytes are wiped here whether the parse took or not.
  std::uint32_t server_time = 0;
  char err[64] = {};
  const bool parsed = mm::parseClaimResponse(reply_, len, jobs, max, count, server_time, err, sizeof err);
  wipeReply();
  if (!parsed) {
    for (int i = 0; i < max; ++i) jobs[i].credential.wipe();
    count = 0;
    counters_.unparsed = saturate(counters_.unparsed);
    last_call_.ok = false;
    std::snprintf(last_call_.error, sizeof last_call_.error, "http %d but the claim did not parse: %s",
                  c.httpStatus, err);
    LOG_E("server: claim: %s", last_call_.error);
    return false;
  }
  offerServerTime(server_time);
  return true;
}

bool ModemUplink::postJobResult(long long jobId, const char* body, std::size_t len) {
  if (gate_ != nullptr && gate_->withhold(jobId)) {
    // The extension left this job to its lease (walter_agent/modem_uplink.h, ResultGate): nothing
    // goes, and the runner is told the result is taken so that it neither keeps nor re-posts it.
    counters_.withheld = saturate(counters_.withheld);
    LOG_I("server: job %lld: no result posted on purpose - the lease is left to expire and the "
          "next wake's claim continues the job (unit-protocol.md)", jobId);
    return true;
  }
  std::size_t reply_len = 0;
  Call c;
  const bool ok = callJob(jobId, "result", reinterpret_cast<const std::uint8_t*>(body), len, reply_len, c) && c.ok;
  wipeReply();
  return ok;
}

bool ModemUplink::extendLease(long long jobId, int seconds, std::uint32_t& leaseUntil) {
  leaseUntil = 0;
  char request[32] = {};
  const std::size_t request_len = mm::writeExtendRequest(seconds, request, sizeof request);
  std::size_t len = 0;
  Call c;
  if (!callJob(jobId, "extend", reinterpret_cast<const std::uint8_t*>(request), request_len, len, c) || !c.ok) {
    wipeReply();
    return false;
  }
  mm::ExtendReply r;
  char err[64] = {};
  if (!mm::parseExtendReply(reply_, len, r, err, sizeof err)) {
    counters_.unparsed = saturate(counters_.unparsed);
    last_call_.ok = false;
    std::snprintf(last_call_.error, sizeof last_call_.error, "http %d but the reply did not parse: %s",
                  c.httpStatus, err);
    LOG_E("server: extend: %s", last_call_.error);
    wipeReply();
    return false;
  }
  leaseUntil = r.leaseUntilUnix;
  wipeReply();
  return true;
}

bool ModemUplink::postProgress(long long jobId, const char* body, std::size_t len) {
  std::size_t reply_len = 0;
  Call c;
  if (!callJob(jobId, "progress", reinterpret_cast<const std::uint8_t*>(body), len, reply_len, c) || !c.ok) {
    wipeReply();
    return false;
  }
  mm::ProgressReply r;
  char err[64] = {};
  if (!mm::parseProgressReply(reply_, reply_len, r, err, sizeof err)) {
    counters_.unparsed = saturate(counters_.unparsed);
    last_call_.ok = false;
    std::snprintf(last_call_.error, sizeof last_call_.error, "http %d but the reply did not parse: %s",
                  c.httpStatus, err);
    LOG_E("server: progress: %s", last_call_.error);
    wipeReply();
    return false;
  }
  wipeReply();
  return true;
}

}  // namespace agent
