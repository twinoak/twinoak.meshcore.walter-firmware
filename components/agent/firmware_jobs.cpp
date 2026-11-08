#include "walter_agent/firmware_jobs.h"

#include <cstdio>
#include <cstring>

#include <agent/Iso8601.h>
#include <agent/JsonWriter.h>
#include <agent/Log.h>

#include "core/image/descriptor.h"
#include "core/link/client/words.h"
#include "core/version.h"
#include "walter_agent/json_scan.h"

namespace agent {

namespace upd = core::update;
namespace img = core::image;

namespace {

std::uint32_t saturate(std::uint32_t value) { return value == 0xFFFFFFFFu ? value : value + 1; }

void zeroBytes(void* at, std::size_t size) {
  volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(at);
  for (std::size_t i = 0; i < size; ++i) p[i] = 0;
}

void copyText(char* out, std::size_t cap, const char* text) {
  if (cap == 0) return;
  out[0] = 0;
  if (text == nullptr) return;
  std::size_t i = 0;
  for (; i + 1 < cap && text[i] != 0; ++i) out[i] = text[i];
  out[i] = 0;
}

void hexOf(const std::uint8_t* bytes, std::size_t n, char* out) {
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = digits[bytes[i] >> 4];
    out[2 * i + 1] = digits[bytes[i] & 0x0F];
  }
  out[2 * n] = 0;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// `n` bytes of hex into `out`; false for anything that is not 2n hex digits.
bool bytesOf(const char* hex, std::size_t n, std::uint8_t* out) {
  if (hex == nullptr || std::strlen(hex) != 2 * n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

void put32(std::uint8_t* at, std::uint32_t value) {
  at[0] = static_cast<std::uint8_t>(value);
  at[1] = static_cast<std::uint8_t>(value >> 8);
  at[2] = static_cast<std::uint8_t>(value >> 16);
  at[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t get32(const std::uint8_t* at) {
  return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
         (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

// The `X.Y.Z` of a version, as the self-update's entries keep it.
void shortOf(const char* version, char out[upd::kShortVersion]) { upd::shortVersion(version, out); }

// Whether a claimed job's type is `full`. The library's `mm::ClaimedJob::type` is at most
// `kMaxJobTypeLen` (16) bytes, so `twinoak.power.cycle` (19) arrives truncated to
// `twinoak.power.cy`; the heartbeat advertises the whole word (type() below) and the server
// routes by it, but a claim carries the short form. The first sixteen bytes tell the four apart.
bool isType(const char* claimed, const char* full) {
  return std::strncmp(claimed, full, mm::kMaxJobTypeLen) == 0 && std::strlen(claimed) <= mm::kMaxJobTypeLen;
}

// A UNIX time as the fixtures write one, 0 being the epoch's own start - the honest value when
// this unit had no time of day (the server stores the journal verbatim and reads only the
// outcome, the refusal and the two versions).
void isoOf(std::uint32_t epoch, char out[24]) { mm::formatUtc(epoch, out, 24); }

// An image's record as the result shapes want it: `{"version":"...","sha256":"..."}`.
void heldImage(mm::JsonWriter& w, const char* key, const img::Record& record) {
  if (!record.present) return;
  char hex[2 * img::kSha256Size + 1];
  hexOf(record.sha256, img::kSha256Size, hex);
  w.key(key);
  w.beginObject();
  w.key("version");
  w.string(record.version);
  w.key("sha256");
  w.string(hex);
  w.endObject();
}

}  // namespace

// ---------------------------------------------------------------------------- the words

const char* name(ApplyOutcome outcome) {
  switch (outcome) {
    case ApplyOutcome::kNone: return "";
    case ApplyOutcome::kCommitted: return "committed";
    case ApplyOutcome::kRolledBack: return "rolledBack";
    case ApplyOutcome::kRefused: return "refused";
    case ApplyOutcome::kStuck: return "stuck";
  }
  return "";
}

const char* name(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone: return "";
    case Refusal::kBattery: return "battery";
    case Refusal::kLink: return "link";
    case Refusal::kClock: return "clock";
    case Refusal::kInProgress: return "inProgress";
    case Refusal::kNoGolden: return "noGolden";
    case Refusal::kBadList: return "badList";
    case Refusal::kOrder: return "order";
  }
  return "";
}

Refusal refusalFor(upd::Reason reason) {
  switch (reason) {
    case upd::Reason::kBatteryUnknown:
    case upd::Reason::kBatteryLow:
    case upd::Reason::kBatteryFalling: return Refusal::kBattery;
    case upd::Reason::kLinkDown: return Refusal::kLink;
    case upd::Reason::kRadioUpdateInProgress:
    case upd::Reason::kSelfUpdateUnconfirmed:
    case upd::Reason::kBusy: return Refusal::kInProgress;
    case upd::Reason::kNoGolden: return Refusal::kNoGolden;
    case upd::Reason::kOnBadList: return Refusal::kBadList;
    default: return Refusal::kNone;
  }
}

Refusal refusalFor(upd::SelfReason reason) {
  switch (reason) {
    case upd::SelfReason::kBatteryUnknown:
    case upd::SelfReason::kBatteryLow:
    case upd::SelfReason::kBatteryFalling: return Refusal::kBattery;
    case upd::SelfReason::kLinkDown: return Refusal::kLink;
    case upd::SelfReason::kRadioUpdateInProgress:
    case upd::SelfReason::kRunningUnconfirmed:
    case upd::SelfReason::kTransferRunning: return Refusal::kInProgress;
    case upd::SelfReason::kOnBadList: return Refusal::kBadList;
    case upd::SelfReason::kLinkMajor: return Refusal::kOrder;
    default: return Refusal::kNone;
  }
}

const char* stepWord(upd::State state) {
  switch (state) {
    case upd::State::kIdle: return "idle";
    case upd::State::kCheck: return "check";
    case upd::State::kFlashing: return "flashing-candidate";
    case upd::State::kBooting: return "booting";
    case upd::State::kProving: return "proving";
    case upd::State::kCommitting: return "committing";
    case upd::State::kDone: return "done";
    case upd::State::kRollingBack: return "rolling-back";
    case upd::State::kRollbackBooting: return "rollback-booting";
    case upd::State::kRollbackProving: return "rollback-proving";
    case upd::State::kRestored: return "restored";
    case upd::State::kStuck: return "stuck";
    case upd::State::kRefused: return "refused";
  }
  return "?";
}

// ---------------------------------------------------------------------------- the journal entry

void encode(const JobEntry& e, std::uint8_t out[kJobEntryBytes]) {
  for (std::size_t i = 0; i < kJobEntryBytes; ++i) out[i] = 0;
  const std::uint64_t id = static_cast<std::uint64_t>(e.job_id);
  put32(out, static_cast<std::uint32_t>(id));
  put32(out + 4, static_cast<std::uint32_t>(id >> 32));
  out[8] = static_cast<std::uint8_t>(e.state);
  out[9] = static_cast<std::uint8_t>(e.outcome);
  out[10] = e.product;
  out[11] = e.slot;
  put32(out + 12, e.at_epoch);
  std::memcpy(out + 16, e.hash16, upd::kHash16);
  std::memcpy(out + 32, e.to_version, upd::kShortVersion);
  out[43] = 0;
}

bool decode(const upd::Entry& entry, JobEntry* out) {
  if (entry.kind != upd::kind::kAgentJob || entry.size < kJobEntryBytes || out == nullptr) return false;
  const std::uint8_t* p = entry.payload;
  if (p[8] == 0 || p[8] > static_cast<std::uint8_t>(JobEntryState::kEnded)) return false;
  if (p[9] > static_cast<std::uint8_t>(ApplyOutcome::kStuck)) return false;
  out->job_id = static_cast<long long>(static_cast<std::uint64_t>(get32(p)) |
                                       (static_cast<std::uint64_t>(get32(p + 4)) << 32));
  out->state = static_cast<JobEntryState>(p[8]);
  out->outcome = static_cast<ApplyOutcome>(p[9]);
  out->product = p[10];
  out->slot = p[11];
  out->at_epoch = get32(p + 12);
  std::memcpy(out->hash16, p + 16, upd::kHash16);
  std::memcpy(out->to_version, p + 32, upd::kShortVersion);
  out->to_version[upd::kShortVersion - 1] = 0;
  return true;
}

// ---------------------------------------------------------------------------- the object

FirmwareJobs::FirmwareJobs(const Parts& parts, core::board::RadioPins& pins, core::board::SlotLock& slots,
                           lc::Client& radio, ModemUplink& server, core::settings::Settings& settings,
                           interfaces::Clock& clock, interfaces::WallClock& wall)
    : parts_(parts), pins_(pins), slots_(slots), radio_(radio), server_(server), settings_(settings),
      clock_(clock), wall_(wall) {}

void FirmwareJobs::beginWake(const WakeContext& context) {
  context_ = context;
  did_ = Did{};
  left_leased_ = 0;
}

const char* FirmwareJobs::type(int i) const {
  switch (i) {
    case 0: return kOfferType;
    case 1: return kApplyType;
    case 2: return kStateType;
    case 3: return kPowerCycleType;
    default: return "";
  }
}

bool FirmwareJobs::withhold(long long jobId) { return left_leased_ != 0 && jobId == left_leased_; }

std::uint32_t FirmwareJobs::nowEpoch() const {
  interfaces::WallTime now{};
  return wall_.now(&now) ? now.epoch : 0;
}

bool FirmwareJobs::journal(const JobEntry& entry) {
  if (parts_.journal == nullptr) return false;
  std::uint8_t bytes[kJobEntryBytes];
  encode(entry, bytes);
  return parts_.journal->append(upd::kind::kAgentJob, bytes, kJobEntryBytes) == upd::JournalError::kNone;
}

const char* FirmwareJobs::bearer() {
  std::snprintf(secret_, sizeof secret_, "Bearer %s", settings_.serverToken());
  return secret_;
}

void FirmwareJobs::wipeBearer() { zeroBytes(secret_, sizeof secret_); }

// -- the lease and the progress ------------------------------------------------------------------

void FirmwareJobs::extendLease(long long job_id, mm::JobContext& ctx) {
  std::uint32_t until = 0;
  counters_.extends = saturate(counters_.extends);
  if (server_.extendLease(job_id, kLeaseExtendSeconds, until)) {
    ctx.log("lease extended");
  } else {
    // A 409 is "not the lease holder any more": the job goes on all the same - the machine it
    // runs is not something to stop halfway for a server's bookkeeping - and the result is late,
    // which the server takes (EXIT-NODES §5).
    ctx.log("the lease could not be extended; the result may be late");
  }
  extend_due_ms_ = clock_.nowMs() + kExtendEveryMs;
}

// -- the dispatch --------------------------------------------------------------------------------

bool FirmwareJobs::run(const mm::ClaimedJob& job, mm::JobContext& ctx, mm::ResultWriter& out) {
  const char* payload = server_.payloadOf(job.id);
  if (isType(job.type, kStateType)) return runState(job, ctx, out);  // no payload
  if (payload == nullptr) {
    counters_.malformed = saturate(counters_.malformed);
    ctx.log("the claim's payload was not kept: nothing to run the job from");
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  if (isType(job.type, kOfferType)) return runOffer(job, payload, ctx, out);
  if (isType(job.type, kApplyType)) return runApply(job, payload, ctx, out);
  if (isType(job.type, kPowerCycleType)) return runPowerCycle(job, payload, ctx, out);
  return false;  // not ours: the runner answers `unsupported`
}

// -- twinoak.fw.offer ------------------------------------------------------------------------------

namespace {

// The download's listener: the progress report every kProgressEveryBytes and the lease every
// kExtendEveryMs, between two ranges - the one moment the modem's socket is free.
class OfferListener final : public core::net::RangeListener {
 public:
  OfferListener(ModemUplink& server, interfaces::Clock& clock, long long job_id, std::uint32_t of,
                std::uint32_t held, std::uint64_t* extend_due_ms, FirmwareJobCounters& counters)
      : server_(server), clock_(clock), job_id_(job_id), of_(of), held_(held),
        next_(((held / kProgressEveryBytes) + 1) * kProgressEveryBytes), extend_due_ms_(extend_due_ms),
        counters_(counters) {}

  void range(const core::net::RangeReport& r) override {
    if (!r.ok) return;
    held_ = r.offset + r.length;
    if (held_ >= next_ && held_ < of_) {
      next_ = held_ + kProgressEveryBytes;
      char data[64];
      std::snprintf(data, sizeof data, "{\"bytes\":%lu,\"of\":%lu}", static_cast<unsigned long>(held_),
                    static_cast<unsigned long>(of_));
      char note[48];
      std::snprintf(note, sizeof note, "%lu kB of %lu kB", static_cast<unsigned long>(held_ / 1024u),
                    static_cast<unsigned long>(of_ / 1024u));
      char report[160];
      const std::size_t n = mm::writeProgressReport(data, note, report, sizeof report);
      if (n != 0) {
        counters_.progress_posted = saturate(counters_.progress_posted);
        (void)server_.postProgress(job_id_, report, n);
      }
    }
    if (clock_.nowMs() >= *extend_due_ms_) {
      std::uint32_t until = 0;
      counters_.extends = saturate(counters_.extends);
      (void)server_.extendLease(job_id_, kLeaseExtendSeconds, until);
      *extend_due_ms_ = clock_.nowMs() + kExtendEveryMs;
    }
  }

 private:
  ModemUplink& server_;
  interfaces::Clock& clock_;
  long long job_id_;
  std::uint32_t of_;
  std::uint32_t held_;
  std::uint32_t next_;
  std::uint64_t* extend_due_ms_;
  FirmwareJobCounters& counters_;
};

}  // namespace

bool FirmwareJobs::runOffer(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx,
                            mm::ResultWriter& out) {
  counters_.offers = saturate(counters_.offers);
  const std::size_t plen = std::strlen(payload);
  char product[40] = {};
  char base[96] = {};
  char expected_hex[2 * img::kSha256Size + 1] = {};
  long long size = 0;
  long long slot = 0;
  (void)json::text(json::member(payload, plen, "product"), product, sizeof product);
  (void)json::text(json::member(payload, plen, "expectedSha256"), expected_hex, sizeof expected_hex);
  (void)json::integer(json::member(payload, plen, "size"), &size);
  (void)json::integer(json::member(payload, plen, "slot"), &slot);
  if (!json::text(json::member(payload, plen, "base"), base, sizeof base) || base[0] != '/') {
    counters_.malformed = saturate(counters_.malformed);
    const std::size_t n = writeOfferResult(false, nullptr, nullptr, 0, 0, "the payload names no base route", false);
    if (n != 0) out.data(data_, n);
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  const bool walter = std::strcmp(product, kProductWalter) == 0;
  if (!walter && std::strcmp(product, kProductRepeater) != 0) {
    counters_.offers_refused = saturate(counters_.offers_refused);
    const std::size_t n = writeOfferResult(false, nullptr, nullptr, 0, 0, "a product this unit is not", false);
    if (n != 0) out.data(data_, n);
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  if (parts_.fetch == nullptr || parts_.store == nullptr || (walter && parts_.self == nullptr)) {
    const std::size_t n = writeOfferResult(false, nullptr, nullptr, 0, 0, "this build has no image store to fetch into", false);
    if (n != 0) out.data(data_, n);
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  if (!context_.uplink_up) {
    const std::size_t n = writeOfferResult(false, nullptr, nullptr, 0, 0, "the uplink is down", false);
    if (n != 0) out.data(data_, n);
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  if (walter) {
    const upd::SelfReason can = parts_.self->canReceive();
    if (can != upd::SelfReason::kNone) {
      counters_.offers_refused = saturate(counters_.offers_refused);
      const std::size_t n = writeOfferResult(false, nullptr, nullptr, 0, 0, upd::describe(can), false);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Failed);
      return true;
    }
  }

  // The base under `server.url`: `{server.url}{base}`, the manifest at `{base}/manifest.json` and
  // the files at `{base}/files/{name}` (unit-protocol.md §4).
  const int built = std::snprintf(base_url_, sizeof base_url_, "%s%s", server_.url(), base);
  if (built <= 0 || static_cast<std::size_t>(built) >= sizeof base_url_) {
    const std::size_t n = writeOfferResult(false, nullptr, nullptr, 0, 0, "the base route does not fit a URL", false);
    if (n != 0) out.data(data_, n);
    out.outcome(mm::Outcome::Failed);
    return true;
  }

  // What this wake may spend on it: the time to the wake's deadline, and the whole of a radio
  // image in bytes. The lease first: a download is long.
  core::net::Budget budget;
  const std::uint64_t now = clock_.nowMs();
  budget.seconds = context_.deadline_ms > now ? static_cast<std::uint32_t>((context_.deadline_ms - now) / 1000u) : 0;
  if (context_.download_bytes != 0) budget.bytes = context_.download_bytes;
  extendLease(job.id, ctx);

  // Resumed: the store (or the slot) holds bytes of this very file from an earlier wake.
  std::uint32_t held_before = 0;
  if (!walter) {
    img::Record record{};
    if (parts_.store->read(img::Which::kCandidate, radio_.slot(), &record) == img::StoreError::kNone &&
        !record.present && record.transfer) {
      held_before = record.transferred;
    }
  }
  const std::uint32_t of = static_cast<std::uint32_t>(size > 0 ? size : 0);
  OfferListener listener(server_, clock_, job.id, of, held_before, &extend_due_ms_, counters_);
  const std::uint64_t began = clock_.nowMs();
  const char* token = bearer();
  const upd::FetchResult r =
      walter ? parts_.fetch->runInto(base_url_, *parts_.self, budget, token, kFilesDir, &listener)
             : parts_.fetch->run(base_url_, img::Which::kCandidate, radio_.slot(), budget, token, kFilesDir,
                                 &listener);
  wipeBearer();
  counters_.bytes_downloaded += r.bytes;
  did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);
  did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kFetchRelease));
  if (did_.carried == interfaces::Ask::kNothing) did_.carried = interfaces::Ask::kFetchRelease;
  const bool resumed = r.resumed_from != 0 || held_before != 0;
  const std::uint64_t bytes = static_cast<std::uint64_t>(r.resumed_from) + r.bytes;

  char line_text[160];
  switch (r.verdict) {
    case upd::FetchVerdict::kStored: {
      counters_.offers_stored = saturate(counters_.offers_stored);
      char hex[2 * img::kSha256Size + 1];
      hexOf(r.sha256, img::kSha256Size, hex);
      if (expected_hex[0] != 0 && std::strcmp(expected_hex, hex) != 0) {
        ctx.log("the file the manifest chose is not the one the server expected; a different choice is not a refusal");
      }
      std::snprintf(line_text, sizeof line_text, "stored %s %s as the %s, %lu bytes%s", r.image_id, r.version,
                    walter ? "other slot's image" : "candidate", static_cast<unsigned long>(bytes),
                    resumed ? " (resumed)" : "");
      ctx.log(line_text);
      const std::size_t n = writeOfferResult(true, r.image_id, hex, bytes, 0, nullptr, resumed);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Ok);
      return true;
    }
    case upd::FetchVerdict::kBudget:
      // The agreed behaviour (unit-protocol.md §5): no result, the lease expires, the next wake's
      // claim of the same job continues the file from the store's progress record.
      counters_.offers_left_leased = saturate(counters_.offers_left_leased);
      left_leased_ = job.id;
      std::snprintf(line_text, sizeof line_text,
                    "the wake's budget ended the download at %lu of %lu bytes: no result, the lease is left to "
                    "expire and the next wake continues it",
                    static_cast<unsigned long>(r.held), static_cast<unsigned long>(r.expected));
      ctx.log(line_text);
      LOG_I("jobs: %s", line_text);
      {
        const std::size_t n = writeOfferResult(false, nullptr, nullptr, bytes, 0,
                                               "the wake's budget ended the download; the next wake resumes it",
                                               resumed);
        if (n != 0) out.data(data_, n);
      }
      out.outcome(mm::Outcome::Timeout);
      return true;
    case upd::FetchVerdict::kRefused: {
      counters_.offers_refused = saturate(counters_.offers_refused);
      const std::size_t n = writeOfferResult(false, nullptr, nullptr, bytes, static_cast<int>(r.check), r.why, resumed);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Failed);
      return true;
    }
    default: {
      counters_.offers_refused = saturate(counters_.offers_refused);
      const std::size_t n = writeOfferResult(false, nullptr, nullptr, bytes, 0, r.why, resumed);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Failed);
      return true;
    }
  }
}

std::size_t FirmwareJobs::writeOfferResult(bool stored, const char* image_id, const char* sha256_hex,
                                           std::uint64_t bytes, int check, const char* reason, bool resumed) {
  mm::JsonWriter w(data_, sizeof data_);
  w.beginObject();
  w.key("stored");
  w.boolean(stored);
  if (image_id != nullptr && image_id[0] != 0) {
    w.key("imageId");
    w.string(image_id);
  }
  if (sha256_hex != nullptr && sha256_hex[0] != 0) {
    w.key("sha256");
    w.string(sha256_hex);
  }
  if (stored || bytes != 0) {
    w.key("bytes");
    w.integer(static_cast<long long>(bytes));
  }
  if (check != 0) {
    w.key("check");
    w.integer(check);
  }
  if (reason != nullptr && reason[0] != 0) {
    w.key("reason");
    w.string(reason);
  }
  w.key("resumed");
  w.boolean(resumed);
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

// -- twinoak.fw.apply ------------------------------------------------------------------------------

void FirmwareJobs::line(const char* step, const char* note) {
  const std::size_t cost = std::strlen(step) + (note != nullptr ? std::strlen(note) : 0);
  if (line_count_ >= kJournalMaxEntries || line_chars_ + cost > kJournalMaxChars) {
    // The shape's bound: the last line kept is replaced, so that the end of the story is always
    // in it (a `restored` or a `done` is what a reader wants most).
    if (line_count_ == 0) return;
    line_chars_ -= std::strlen(lines_[line_count_ - 1].step) + std::strlen(lines_[line_count_ - 1].note);
    --line_count_;
    if (line_chars_ + cost > kJournalMaxChars) return;
  }
  Line& l = lines_[line_count_++];
  l.at_epoch = nowEpoch();
  l.step = step;
  copyText(l.note, sizeof l.note, note);
  line_chars_ += std::strlen(step) + std::strlen(l.note);
}

std::size_t FirmwareJobs::writeApplyResult(ApplyOutcome outcome, const char* from, const char* to, Refusal refusal) {
  mm::JsonWriter w(data_, sizeof data_);
  w.beginObject();
  w.key("outcome");
  w.string(name(outcome));
  if (from != nullptr && from[0] != 0) {
    w.key("fromVersion");
    w.string(from);
  }
  if (to != nullptr && to[0] != 0) {
    w.key("toVersion");
    w.string(to);
  }
  if (line_count_ > 0) {
    w.key("journal");
    w.beginArray();
    for (int i = 0; i < line_count_; ++i) {
      char at[24];
      isoOf(lines_[i].at_epoch, at);
      w.comma();
      w.beginObject();
      w.key("at");
      w.string(at);
      w.key("step");
      w.string(lines_[i].step);
      if (lines_[i].note[0] != 0) {
        w.key("note");
        w.string(lines_[i].note);
      }
      w.endObject();
    }
    w.endArray();
  }
  if (refusal != Refusal::kNone) {
    w.key("refusal");
    w.string(name(refusal));
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

bool FirmwareJobs::refuseApply(mm::ResultWriter& out, Refusal refusal, const char* note, const char* from) {
  counters_.refused = saturate(counters_.refused);
  line("check", note);
  const std::size_t n = writeApplyResult(ApplyOutcome::kRefused, from, from, refusal);
  if (n != 0) out.data(data_, n);
  out.outcome(mm::Outcome::Failed);
  return true;
}

bool FirmwareJobs::runApply(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx,
                            mm::ResultWriter& out) {
  counters_.applies = saturate(counters_.applies);
  clearLines();
  const std::size_t plen = std::strlen(payload);
  char product[40] = {};
  char expected_hex[2 * img::kSha256Size + 1] = {};
  bool first_install = false;
  (void)json::text(json::member(payload, plen, "product"), product, sizeof product);
  (void)json::text(json::member(payload, plen, "expectedSha256"), expected_hex, sizeof expected_hex);
  (void)json::boolean(json::member(payload, plen, "firstInstall"), &first_install);
  const bool walter = std::strcmp(product, kProductWalter) == 0;

  // S-7, as the wake knows it: this firmware on trial, or an update of either MCU mid-run.
  if (context_.on_trial) return refuseApply(out, Refusal::kInProgress, "this firmware is on trial (S-7)", nullptr);
  if (context_.update_running) {
    return refuseApply(out, Refusal::kInProgress, "an update is journalled as in progress (S-7)", nullptr);
  }
  if (walter) return runSelfApply(job, expected_hex, ctx, out);
  if (std::strcmp(product, kProductRepeater) != 0) {
    return refuseApply(out, Refusal::kNone, "a product this unit is not", nullptr);
  }
  if (parts_.update == nullptr || parts_.store == nullptr) {
    return refuseApply(out, Refusal::kNone, "this build has no update machine", nullptr);
  }

  // The versions either side, and the candidate against what the server believes is held.
  char from[img::kVersionSize] = {};
  img::Record golden{};
  if (parts_.store->read(img::Which::kGolden, radio_.slot(), &golden) == img::StoreError::kNone && golden.present) {
    copyText(from, sizeof from, golden.version);
  } else if (radio_.session().up) {
    copyText(from, sizeof from, radio_.session().fw_version);
  }
  img::Record candidate{};
  const bool have_candidate =
      parts_.store->read(img::Which::kCandidate, radio_.slot(), &candidate) == img::StoreError::kNone &&
      candidate.present;
  if (expected_hex[0] != 0) {
    std::uint8_t expected[img::kSha256Size];
    if (!bytesOf(expected_hex, img::kSha256Size, expected)) {
      return refuseApply(out, Refusal::kNone, "expectedSha256 is not 64 hex digits", from);
    }
    if (!have_candidate || !img::sha256Equal(expected, candidate.sha256)) {
      // The one precondition that is this firmware's and not the machine's: the candidate held
      // is another file than the one the offer stored, or none. The bad list has its own word,
      // asked of the store before the machine is.
      if (parts_.store->isBad(expected)) {
        return refuseApply(out, Refusal::kBadList, "the image the server named is on the bad list", from);
      }
      return refuseApply(out, Refusal::kNone,
                         have_candidate ? "the candidate held is not the file the server named"
                                        : "no candidate is held", from);
    }
  }
  if (!context_.may.update) {
    return refuseApply(out, Refusal::kBattery, "the battery's band does not allow an update to be started", from);
  }

  upd::Request request{};
  request.kind = first_install ? upd::Kind::kFirstInstall : upd::Kind::kUpdate;
  request.slot = radio_.slot();
  // The link's refusal and the battery's below it read **the supervisor's classification** and
  // nothing else (task p9-03): a scanner slot and a slot awaiting its first install have no frame
  // session either, and neither is a radio this end may flash past the two of them.
  request.link_known_down = core::supervisor::linkKnownDown(radioHealth());
  if (!parts_.update->start(request)) {
    char note[kJournalNoteBytes];
    std::snprintf(note, sizeof note, "%.70s", upd::describe(parts_.update->reason()));
    return refuseApply(out, refusalFor(parts_.update->reason()), note, from);
  }

  // The machine runs to its end inside this job: the lease ahead of it, the bound on the wake
  // raised to what an update may hold it open for, the uplink's deadline with it.
  extendLease(job.id, ctx);
  if (parts_.guard != nullptr) parts_.guard->allow(kApplyBoundMs);
  const std::uint64_t began = clock_.nowMs();
  const std::uint64_t bound = began + kApplyBoundMs;
  server_.setDeadline(bound);
  did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kUpdateRadio));
  did_.carried = interfaces::Ask::kUpdateRadio;
  line("check", "the machine's preconditions hold");
  bool bounded = false;
  while (parts_.update->active()) {
    const upd::StepResult s = parts_.update->step();
    if (s.changed) {
      char note[kJournalNoteBytes];
      if (s.after == upd::State::kRollingBack) {
        std::snprintf(note, sizeof note, "%.40s at %.24s", upd::describe(s.reason), upd::describe(s.step));
      } else if (s.after == upd::State::kDone || s.after == upd::State::kRestored || s.after == upd::State::kStuck) {
        std::snprintf(note, sizeof note, "%.70s", s.note);
      } else {
        std::snprintf(note, sizeof note, "%.70s", s.note);
      }
      line(stepWord(s.after), note);
    }
    if (!parts_.update->active()) break;
    if (clock_.nowMs() >= bound) {
      bounded = true;
      break;
    }
    if (clock_.nowMs() >= extend_due_ms_) extendLease(job.id, ctx);
    if (s.waited) ctx.platform().delayMs(kApplyIdleMs);
  }
  did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);

  if (bounded) {
    // The bound a wake may be held open to passed with the machine mid-run: the journal carries
    // it to the next boot and the wake's own guard ends this wake. No result can say what the
    // machine has not decided; the lease is left, and the report says so.
    left_leased_ = job.id;
    ctx.log("the apply ran past the bound on this wake: the journal carries it; no result yet");
    LOG_W("jobs: apply %lld: past the wake's bound with the machine at %s", job.id,
          upd::describe(parts_.update->state()));
    out.outcome(mm::Outcome::Timeout);
    return true;
  }

  ApplyOutcome outcome = ApplyOutcome::kStuck;
  Refusal refusal = Refusal::kNone;
  switch (parts_.update->state()) {
    case upd::State::kDone:
      outcome = ApplyOutcome::kCommitted;
      counters_.committed = saturate(counters_.committed);
      break;
    case upd::State::kRestored:
      outcome = ApplyOutcome::kRolledBack;
      counters_.rolled_back = saturate(counters_.rolled_back);
      break;
    case upd::State::kRefused:
      outcome = ApplyOutcome::kRefused;
      refusal = refusalFor(parts_.update->reason());
      counters_.refused = saturate(counters_.refused);
      break;
    default:
      outcome = ApplyOutcome::kStuck;
      counters_.stuck = saturate(counters_.stuck);
      break;
  }
  // What the slot runs afterwards: the golden image's version now.
  char to[img::kVersionSize] = {};
  img::Record after{};
  if (parts_.store->read(img::Which::kGolden, radio_.slot(), &after) == img::StoreError::kNone && after.present) {
    copyText(to, sizeof to, after.version);
  } else {
    copyText(to, sizeof to, from);
  }

  JobEntry ended;
  ended.job_id = job.id;
  ended.state = JobEntryState::kEnded;
  ended.outcome = outcome;
  ended.product = 0;
  ended.slot = radio_.slot().index;
  ended.at_epoch = nowEpoch();
  std::memcpy(ended.hash16, candidate.sha256, upd::kHash16);
  shortOf(to, ended.to_version);
  (void)journal(ended);

  const std::size_t n = writeApplyResult(outcome, from, to, refusal);
  if (n != 0) out.data(data_, n);
  out.outcome(outcome == ApplyOutcome::kCommitted || outcome == ApplyOutcome::kRolledBack ? mm::Outcome::Ok
                                                                                           : mm::Outcome::Failed);
  return true;
}

bool FirmwareJobs::runSelfApply(const mm::ClaimedJob& job, const char* expected_hex, mm::JobContext& ctx,
                                mm::ResultWriter& out) {
  if (parts_.self == nullptr) return refuseApply(out, Refusal::kNone, "this build has no self-update machine", nullptr);
  char from[img::kVersionSize] = {};
  copyText(from, sizeof from, core::version());
  upd::SelfPayload received;
  if (!parts_.self->received(&received)) {
    return refuseApply(out, Refusal::kNone, "nothing is received into the other slot", from);
  }
  if (expected_hex[0] != 0) {
    std::uint8_t expected[img::kSha256Size];
    if (!bytesOf(expected_hex, img::kSha256Size, expected)) {
      return refuseApply(out, Refusal::kNone, "expectedSha256 is not 64 hex digits", from);
    }
    if (std::memcmp(expected, received.hash16, upd::kHash16) != 0) {
      if (parts_.store != nullptr && parts_.store->isBad(expected)) {
        return refuseApply(out, Refusal::kBadList, "the image the server named is on the bad list", from);
      }
      return refuseApply(out, Refusal::kNone, "the other slot holds another file than the one the server named", from);
    }
  }
  if (!context_.may.update) {
    return refuseApply(out, Refusal::kBattery, "the battery's band does not allow an update to be started", from);
  }

  // The result is owed: the switch restarts this board, and the boot after the trial posts it
  // from this entry and the self-update's own. Written **before** the switch, which on a board
  // does not return.
  JobEntry owed;
  owed.job_id = job.id;
  owed.state = JobEntryState::kOwed;
  owed.product = 1;
  owed.slot = received.slot;
  owed.at_epoch = nowEpoch();
  std::memcpy(owed.hash16, received.hash16, upd::kHash16);
  std::memcpy(owed.to_version, received.to, upd::kShortVersion);
  if (!journal(owed)) {
    return refuseApply(out, Refusal::kNone, "the journal could not be written: nothing is switched without it", from);
  }
  extendLease(job.id, ctx);
  counters_.switches = saturate(counters_.switches);
  did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kUpdateSelf));
  did_.carried = interfaces::Ask::kUpdateSelf;
  line("check", "the received image is the one the server named");
  upd::SwitchRequest request{};
  // The trial's second condition: only a radio the supervisor has classified may be waived
  // (task p9-03). A wake that classified nothing waives nothing, and the switch is refused on a
  // radio that does not answer - which is what keeps a good Walter image from being condemned by
  // a `HELLO` nobody had decided about.
  request.radio_known_down = core::supervisor::radioKnownDown(radioHealth());
  const std::uint64_t began = clock_.nowMs();
  const upd::SelfReason why = parts_.self->switchNow(request);
  did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);
  if (why != upd::SelfReason::kNone) {
    // Refused before the boot pointer moved: the owed entry is settled here, with the refusal.
    JobEntry ended = owed;
    ended.state = JobEntryState::kEnded;
    ended.outcome = ApplyOutcome::kRefused;
    shortOf(from, ended.to_version);
    (void)journal(ended);
    char note[kJournalNoteBytes];
    std::snprintf(note, sizeof note, "%.70s", upd::describe(why));
    return refuseApply(out, refusalFor(why), note, from);
  }
  // On a board this line is not reached: switchNow() restarted it. On the host the restart is
  // recorded, and nothing is posted now - the trial has not happened.
  left_leased_ = job.id;
  ctx.log("the other slot is the next to boot: the result follows the trial, from the next boot");
  out.outcome(mm::Outcome::Timeout);
  return true;
}

bool FirmwareJobs::resultOwed(JobEntry* out) const {
  if (parts_.journal == nullptr) return false;
  upd::Entry e;
  JobEntry j;
  if (!parts_.journal->lastOf(upd::kind::kAgentJob, &e) || !decode(e, &j)) return false;
  if (j.state != JobEntryState::kOwed) return false;
  if (out != nullptr) *out = j;
  return true;
}

bool FirmwareJobs::lastEnded(JobEntry* out) const {
  if (parts_.journal == nullptr || out == nullptr) return false;
  upd::Cursor c;
  upd::Entry e;
  bool more = parts_.journal->first(&c, &e);
  int steps = 0;
  while (more && steps++ < 1024) {
    JobEntry j;
    if (decode(e, &j) && j.state == JobEntryState::kEnded) {
      *out = j;
      return true;
    }
    more = parts_.journal->previous(&c, &e);
  }
  return false;
}

bool FirmwareJobs::postOwedResult() {
  JobEntry owed;
  if (!resultOwed(&owed) || parts_.journal == nullptr) return false;
  // The self-update's story since the switch: the entries newer than the owed one, oldest first.
  // The newest says how the trial ended; `switching` and `on trial` alone are a trial still
  // running in this boot, and nothing is posted yet.
  upd::Entry owed_entry;
  (void)parts_.journal->lastOf(upd::kind::kAgentJob, &owed_entry);
  upd::SelfPayload story[8];
  int count = 0;
  upd::Cursor c;
  upd::Entry e;
  bool more = parts_.journal->first(&c, &e);
  int steps = 0;
  while (more && steps++ < 1024 && e.seq > owed_entry.seq) {
    upd::SelfPayload p;
    if (upd::decode(e, &p) && count < 8) {
      // Newest first off the cursor: shifted so that the array ends oldest first.
      for (int i = count; i > 0; --i) story[i] = story[i - 1];
      story[0] = p;
      ++count;
    }
    more = parts_.journal->previous(&c, &e);
  }
  if (count == 0) return false;
  const upd::SelfPayload& last = story[count - 1];
  ApplyOutcome outcome = ApplyOutcome::kNone;
  if (last.state == upd::SelfState::kConfirmed) outcome = ApplyOutcome::kCommitted;
  if (last.state == upd::SelfState::kRolledBack) outcome = ApplyOutcome::kRolledBack;
  if (outcome == ApplyOutcome::kNone) return false;  // the trial has not ended

  clearLines();
  for (int i = 0; i < count; ++i) {
    const upd::SelfPayload& p = story[i];
    const char* step = p.state == upd::SelfState::kSwitching    ? "switching"
                       : p.state == upd::SelfState::kOnTrial    ? "on-trial"
                       : p.state == upd::SelfState::kConfirmed  ? "confirmed"
                       : p.state == upd::SelfState::kRolledBack ? "rolled-back"
                                                                : "received";
    line(step, p.reason != upd::SelfReason::kNone ? upd::describe(p.reason) : "");
  }
  // The versions as the trial's entries carry them: `from` is the image that was running, `to`
  // the one switched to when confirmed and the one that ran before when rolled back.
  const char* to = outcome == ApplyOutcome::kCommitted ? last.to : last.from;
  const std::size_t n = writeApplyResult(outcome, last.from, to, Refusal::kNone);
  if (n == 0) return false;

  char client_id[48];
  char hash_hex[2 * upd::kHash16 + 1];
  hexOf(owed.hash16, upd::kHash16, hash_hex);
  std::snprintf(client_id, sizeof client_id, "%lld-apply-%s", owed.job_id, hash_hex);
  mm::JobReport report;
  report.clientId = client_id;
  report.outcome = mm::Outcome::Ok;
  report.startedAt = owed.at_epoch;
  report.finishedAt = nowEpoch();
  report.transactions = 0;
  report.data = data_;
  report.log = "the Walter's own apply: the trial's outcome, posted from the journal after the restart";
  const std::size_t len = mm::writeJobResult(report, body_, sizeof body_);
  if (len == 0) return false;
  if (!server_.postJobResult(owed.job_id, body_, len)) return false;
  counters_.owed_posted = saturate(counters_.owed_posted);

  JobEntry posted = owed;
  posted.state = JobEntryState::kPosted;
  posted.outcome = outcome;
  posted.at_epoch = nowEpoch();
  std::memcpy(posted.to_version, outcome == ApplyOutcome::kCommitted ? last.to : last.from, upd::kShortVersion);
  (void)journal(posted);
  JobEntry ended = posted;
  ended.state = JobEntryState::kEnded;
  (void)journal(ended);
  LOG_I("jobs: the Walter's own apply %lld: %s, posted after the restart", owed.job_id, name(outcome));
  return true;
}

// -- twinoak.fw.state ------------------------------------------------------------------------------

bool FirmwareJobs::runState(const mm::ClaimedJob& job, mm::JobContext& ctx, mm::ResultWriter& out) {
  (void)job;
  counters_.states = saturate(counters_.states);
  const std::size_t n = writeStateResult();
  if (n == 0) {
    ctx.log("the state did not fit its buffer");
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  out.data(data_, n);
  out.outcome(mm::Outcome::Ok);
  return true;
}

std::size_t FirmwareJobs::writeStateResult() {
  mm::JsonWriter w(data_, sizeof data_);
  const core::board::Slot slot = radio_.slot();
  const lc::Session& session = radio_.session();
  img::Record golden{};
  img::Record candidate{};
  img::Record scanner{};
  if (parts_.store != nullptr) {
    (void)parts_.store->read(img::Which::kGolden, slot, &golden);
    (void)parts_.store->read(img::Which::kCandidate, slot, &candidate);
    (void)parts_.store->read(img::Which::kScanner, slot, &scanner);
  }
  w.beginObject();
  w.key("slots");
  w.beginArray();
  w.comma();
  w.beginObject();
  w.key("slot");
  w.integer(slot.index);
  // The role, the board and the band: the golden image's descriptor, which is what the running
  // image was flashed from; HELLO's words when there is none yet.
  if (golden.present) {
    w.key("role");
    w.string(img::role_name(golden.descriptor.role));
    w.key("board");
    w.string(golden.descriptor.board);
    w.key("band");
    w.string(golden.descriptor.band);
  } else if (session.up) {
    w.key("role");
    w.string(lc::roleName(session.role));
    w.key("board");
    w.string(session.board);
    // The band too, when the radio said one: §5.1's `band`, which link 1.1 added, and which
    // `FirmwareRollout.PickImage` needs with the other two before it can pick a release's image
    // (the 6 November fault, phase-6/FOLLOWUPS.md). A 1.0 radio says none, and then the key is
    // left out rather than written empty: *absent* is what the slot has never been told.
    if (session.band[0] != 0) {
      w.key("band");
      w.string(session.band);
    }
  }
  if (session.up) {
    w.key("running");
    w.beginObject();
    w.key("version");
    w.string(session.fw_version);
    w.key("bootId");
    w.integer(static_cast<long long>(session.boot_id));
    w.endObject();
  }
  heldImage(w, "golden", golden);
  heldImage(w, "candidate", candidate);
  heldImage(w, "scanner", scanner);
  JobEntry last;
  if (lastEnded(&last) && last.product == 0) {
    char at[24];
    isoOf(last.at_epoch, at);
    w.key("lastUpdate");
    w.beginObject();
    w.key("at");
    w.string(at);
    w.key("outcome");
    w.string(name(last.outcome));
    if (last.to_version[0] != 0) {
      w.key("toVersion");
      w.string(last.to_version);
    }
    w.endObject();
  }
  w.endObject();
  w.endArray();

  // The Walter's own halves.
  w.key("walter");
  w.beginObject();
  w.key("version");
  w.string(core::version());
  if (parts_.self != nullptr) {
    interfaces::OwnSlot running{};
    interfaces::OwnSlot other{};
    if (parts_.self->runningSlot(&running) && running.known) {
      char label[8];
      std::snprintf(label, sizeof label, "ota_%u", static_cast<unsigned>(running.index));
      w.key("runningSlot");
      w.string(label);
      w.key("confirmed");
      w.boolean(running.state != interfaces::OwnImageState::kPendingVerify &&
                running.state != interfaces::OwnImageState::kNew);
    }
    if (parts_.self->otherSlot(&other) && other.known && other.version[0] != 0) {
      w.key("otherSlot");
      w.beginObject();
      w.key("version");
      w.string(other.version);
      w.endObject();
    }
  }
  w.endObject();

  // The supervisor: what it keeps across a sleep (the journal keeps the rest).
  if (parts_.supervisor != nullptr) {
    w.key("supervisor");
    w.beginObject();
    w.key("incidents");
    w.integer(static_cast<long long>(parts_.supervisor->incidentCount()));
    if (parts_.supervisor->incidentCount() > 0) {
      const core::supervisor::Incident& newest = parts_.supervisor->incidentAt(0);
      if (newest.began_epoch != 0) {
        char at[24];
        isoOf(newest.began_epoch, at);
        w.key("lastIncidentAt");
        w.string(at);
      }
    }
    w.endObject();
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

// -- twinoak.power.cycle ---------------------------------------------------------------------------

std::size_t FirmwareJobs::writePowerCycleResult(std::uint32_t off_ms, bool radio_back, bool has_boot_id,
                                                std::uint32_t boot_id) {
  mm::JsonWriter w(data_, sizeof data_);
  w.beginObject();
  w.key("offMs");
  w.integer(static_cast<long long>(off_ms));
  w.key("radioBack");
  w.boolean(radio_back);
  if (has_boot_id) {
    w.key("bootId");
    w.integer(static_cast<long long>(boot_id));
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

bool FirmwareJobs::runPowerCycle(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx,
                                 mm::ResultWriter& out) {
  (void)job;
  counters_.power_cycles = saturate(counters_.power_cycles);
  long long off_seconds = 5;  // PowerCyclePayload.DefaultOffSeconds
  (void)json::integer(json::member(payload, std::strlen(payload), "offSeconds"), &off_seconds);
  const core::board::KillTiming& timing = pins_.timing();
  char note[160];
  // The server's bounds (2..60 s) and the board's own (core/board/radio_pins.h): outside either
  // the rail is not touched.
  if (off_seconds < 2 || off_seconds > 60 || static_cast<std::uint64_t>(off_seconds) * 1000u < timing.min_ms ||
      static_cast<std::uint64_t>(off_seconds) * 1000u > timing.max_ms) {
    counters_.power_cycles_refused = saturate(counters_.power_cycles_refused);
    std::snprintf(note, sizeof note, "%lld s is outside what this board's kill line allows (%lu..%lu s)",
                  off_seconds, static_cast<unsigned long>(timing.min_ms / 1000u),
                  static_cast<unsigned long>(timing.max_ms / 1000u));
    ctx.log(note);
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  // The battery's permission, as phase 2 has it: the band's say (`critical` takes the rail away
  // from nobody), and the supervisor's own floor for its rung 3 over the reading this wake took.
  // An unknown reading refuses nothing, as the supervisor's rung does not: a radio that is there
  // to be asked is what a power cycle is for.
  if (!context_.may.power_cycle) {
    counters_.power_cycles_refused = saturate(counters_.power_cycles_refused);
    ctx.log("the battery's band does not allow the rail to be taken away");
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  if (context_.battery_known && context_.battery_mv < core::supervisor::kPowerCycleMinMv) {
    counters_.power_cycles_refused = saturate(counters_.power_cycles_refused);
    std::snprintf(note, sizeof note, "the battery is at %u mV, below the %u a power cycle needs",
                  static_cast<unsigned>(context_.battery_mv), static_cast<unsigned>(core::supervisor::kPowerCycleMinMv));
    ctx.log(note);
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  const core::board::SlotHold hold(slots_, radio_.slot());
  if (!hold.ok()) {
    counters_.power_cycles_refused = saturate(counters_.power_cycles_refused);
    ctx.log(core::board::describe(hold.result()));
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  const std::uint32_t off_ms = static_cast<std::uint32_t>(off_seconds) * 1000u;
  const std::uint64_t began = clock_.nowMs();
  const core::board::Outcome r = pins_.killCycle(off_ms);
  if (r != core::board::Outcome::kOk) {
    counters_.power_cycles_refused = saturate(counters_.power_cycles_refused);
    did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);
    ctx.log(core::board::describe(r));
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  radio_.radioWasReset();  // the rail went down: a new boot over there, and the UART was taken
  // HELLO inside the boot window: the radio is back when it answers, with its new boot id.
  bool back = false;
  const std::uint64_t until = clock_.nowMs() + kPowerCycleHelloWindowMs;
  for (;;) {
    if (radio_.hello().ok()) {
      back = true;
      break;
    }
    if (clock_.nowMs() >= until) break;
    ctx.platform().delayMs(kPowerCycleHelloIntervalMs);
  }
  // The rail's time is outside the wake's budget, as the ladder's rung is; a power cycle is none
  // of the closed set's asks, so it leaves no bit in the record beyond the job itself.
  did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);
  std::snprintf(note, sizeof note, "rail off %lu ms; the radio %s", static_cast<unsigned long>(off_ms),
                back ? "answered HELLO" : "did not answer inside the boot window");
  ctx.log(note);
  const std::size_t n = writePowerCycleResult(off_ms, back, back, radio_.session().boot_id);
  if (n != 0) out.data(data_, n);
  out.outcome(mm::Outcome::Ok);
  return true;
}

}  // namespace agent
