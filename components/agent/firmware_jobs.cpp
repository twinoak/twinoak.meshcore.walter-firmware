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

// `out` = a + b + c + d, bounded and always terminated. By hand and not with snprintf: the build
// treats -Wformat-truncation as an error in our own components, and what these four strings hold
// is not known at compile time (report p11-01's note about an snprintf append).
void joinText(char* out, std::size_t cap, const char* a, const char* b, const char* c, const char* d) {
  if (cap == 0) return;
  const char* const parts[4] = {a, b, c, d};
  std::size_t at = 0;
  for (const char* part : parts) {
    for (std::size_t i = 0; part != nullptr && part[i] != 0 && at + 1 < cap; ++i) out[at++] = part[i];
  }
  out[at] = 0;
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

const char* name(SessionOutcome outcome) {
  switch (outcome) {
    case SessionOutcome::kNone: return "";
    case SessionOutcome::kRestored: return "restored";
    case SessionOutcome::kRefused: return "refused";
    case SessionOutcome::kStuck: return "stuck";
  }
  return "";
}

// The five endings are the machine's own words; the sixth - a step the session ended before - is
// this end's, because the machine has no word for a step that never ran.
const char* endWord(upd::StepEnd end) {
  return end == upd::StepEnd::kNone ? kStepNotRun : upd::name(end);
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
    // Not one of the eight words unit-protocol.md §5 lists, and the page says "the step words are
    // yours": the server stores the journal verbatim and reads only the outcome (task p10-03).
    case upd::State::kNodeFault: return "node-fault";
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
  // The first of the four spare bytes (task p16-04): how many steps the session's payload had, so
  // that a result posted after a restart still carries a row for every one of them. 0 on an
  // apply's entry, which is what every entry written before this task holds there.
  out[44] = e.steps;
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
  out->steps = p[44] <= core::update::kStepsMax ? p[44] : 0;
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
    case 4: return kReportType;
    case 5: return kSessionType;
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

bool FirmwareJobs::quietInAStep() {
  // **Asked of the machine and never of a copy.** A step's lines are handed over from inside
  // step(), so a flag this end cached at the top of the loop would still say *in a step* at the
  // one moment the lines are meant to go out. The machine knows: the step has ended before its
  // sink is called.
  if (parts_.session == nullptr || !parts_.session->listening()) return false;
  // Unreachable by the loop's shape, which is exactly why it is counted rather than trusted: on
  // 21 November a progress call fell between a step's command and its lines and took nine of
  // sixteen with it, and the loop that did it was doing nothing wrong at the time.
  counters_.calls_in_step = saturate(counters_.calls_in_step);
  return true;
}

void FirmwareJobs::extendLease(long long job_id, mm::JobContext& ctx, int seconds) {
  if (quietInAStep()) return;
  std::uint32_t until = 0;
  counters_.extends = saturate(counters_.extends);
  // What the server grants is *now plus what was asked*, whatever it held before (task p14-04),
  // so this is the lease whether the call was answered or not: a 409 means this end is not the
  // holder any more, and then no number here is worth anything anyway.
  lease_until_ms_ = clock_.nowMs() + static_cast<std::uint64_t>(seconds) * 1000u;
  if (server_.extendLease(job_id, seconds, until)) {
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
  if (isType(job.type, kReportType)) return runReport(job, payload, ctx, out);
  if (isType(job.type, kSessionType)) return runSession(job, payload, ctx, out);
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

  // **Which of the slot's images the file goes into** (task p16-02): `into`, the one member the
  // payload gained. Left out it is the candidate, so an offer of today is byte for byte what it
  // was; `"scanner"` sends the same fetch into the scanner slot, where the manifest's image whose
  // role is *scanner* is the one chosen for the board and the band (core/image/receive.cpp's
  // `roleFor`). Everything else about the offer is unchanged: the manifest and its signature, the
  // six checks, the ranges, the progress, the lease, and a download taken up again at the next
  // wake. A unit on a mast has no console, and `fetch scanner` is the console's: without this
  // member such a unit can never hold a scanner image at all.
  //
  // `json::text` answers false for a word longer than the buffer as well as for a member that is
  // not a string, so the member is asked for first and the word only then - otherwise
  // `"into":"something-very-long"` would read as *absent* and quietly fill the candidate.
  img::Which into = img::Which::kCandidate;
  const json::Value into_value = json::member(payload, plen, "into");
  if (into_value.found()) {
    char word[16] = {};
    const bool read = json::text(into_value, word, sizeof word) && img::whichFromName(word, &into);
    // `whichFromName` knows all four, and only two of them are an offer's: the golden image is
    // reached by a promotion and the base by `rak provision`, never by a download.
    if (!read || (into != img::Which::kCandidate && into != img::Which::kScanner)) {
      counters_.offers_refused = saturate(counters_.offers_refused);
      const std::size_t n = writeOfferResult(
          false, nullptr, nullptr, 0, 0,
          "`into` names neither the candidate nor the scanner image of the slot", false);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Failed);
      return true;
    }
    if (into == img::Which::kScanner && walter) {
      counters_.offers_refused = saturate(counters_.offers_refused);
      const std::size_t n = writeOfferResult(
          false, nullptr, nullptr, 0, 0,
          "the scanner image is the radio's; this unit's own firmware has no scanner slot", false);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Failed);
      return true;
    }
  }
  const bool scanner = into == img::Which::kScanner;
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

  // **Stored already** (task p16-02). The slot may hold this very file: a result that never
  // reached the server, a release offered to the unit twice, or - the case this task makes
  // common - the scanner image, which meshmaster.dk offers to every unit that advertises a
  // session and which a unit that has one would otherwise fetch again at every offer. A few
  // hundred kilobytes over a metered link, for bytes that are already in the flash.
  //
  // What it is keyed on is `expectedSha256`, the one thing in the payload that names a **file**
  // rather than a release, together with the record's own: a record is written only by commit(),
  // which takes the token of all six checks, so a present record whose hash is the server's is
  // that file, stored and judged. `verify()` reads the area back and hashes it before the
  // promise is made, so a bit that has rotted since is a download and not a lie. A payload that
  // names no hash skips nothing and is fetched as it always was.
  if (!walter && expected_hex[0] != 0) {
    img::Record held{};
    char hex[2 * img::kSha256Size + 1];
    if (parts_.store->read(into, radio_.slot(), &held) == img::StoreError::kNone && held.present) {
      hexOf(held.sha256, img::kSha256Size, hex);
      const bool same = std::strcmp(expected_hex, hex) == 0;
      if (same && parts_.store->verify(into, radio_.slot()) == img::StoreError::kNone) {
        counters_.offers_stored = saturate(counters_.offers_stored);
        // `<role>.<board>.<band>`, which is what a manifest's own `id` is
        // (spec/FIRMWARE-MANIFEST.md §4) - the record keeps the three words and not the id.
        char image_id[64];
        const char* role = img::role_name(held.descriptor.role);
        std::snprintf(image_id, sizeof image_id, "%s.%s.%s", role != nullptr ? role : "unknown",
                      held.descriptor.board, held.descriptor.band);
        char line[200];
        std::snprintf(line, sizeof line, "the %s slot already holds %s %s, %lu bytes: nothing fetched",
                      img::name(into), image_id, held.version, static_cast<unsigned long>(held.size));
        ctx.log(line);
        // The ask was carried out - the release the server named is in the slot - and it took no
        // time outside, which is the whole point of the answer.
        did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kFetchRelease));
        if (did_.carried == interfaces::Ask::kNothing) did_.carried = interfaces::Ask::kFetchRelease;
        const std::size_t n = writeOfferResult(true, image_id, hex, held.size, 0, nullptr, false);
        if (n != 0) out.data(data_, n);
        out.outcome(mm::Outcome::Ok);
        return true;
      }
    }
  }

  // What this wake may spend on it: the time to the wake's deadline, and the whole of a radio
  // image in bytes. The lease first: a download is long.
  core::net::Budget budget;
  const std::uint64_t now = clock_.nowMs();
  budget.seconds = context_.deadline_ms > now ? static_cast<std::uint32_t>((context_.deadline_ms - now) / 1000u) : 0;
  if (context_.download_bytes != 0) budget.bytes = context_.download_bytes;
  extendLease(job.id, ctx);

  // Resumed: the store (or the slot) holds bytes of this very file from an earlier wake. The
  // scanner slot keeps its progress the way the candidate's does - they are two areas of the same
  // store - so this reads the slot the offer is for and nothing else (task p16-02).
  std::uint32_t held_before = 0;
  if (!walter) {
    img::Record record{};
    if (parts_.store->read(into, radio_.slot(), &record) == img::StoreError::kNone &&
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
             : parts_.fetch->run(base_url_, into, radio_.slot(), budget, token, kFilesDir, &listener);
  wipeBearer();
  // **The eighth call this unit makes to that server**, into the same list as the seven of the
  // uplink (task p13-02, walter_agent/call_log.h). It is recorded here and not in `core/net/` for
  // the reason the list exists at all: a list has to say *which call* it was, and only the caller
  // knows. Every verdict but a stored image is a range that did not come back 2xx - a 401, a 429,
  // a 5xx, a connection that never opened - and the budget ending a download is the one exception,
  // because that is this firmware stopping, not the server refusing.
  //
  // What the modem said in it (task p15-02) goes with it, because `CallLog::record` writes both
  // values in the same moment. A fetch is many range requests and there is one record, so the
  // record kept for a download range is **the last range of the fetch** - which is the one that
  // failed, since a fetch stops at the range that did not come back.
  if (r.verdict != upd::FetchVerdict::kStored && r.verdict != upd::FetchVerdict::kBudget) {
    CallLog* calls = server_.callLog();
    if (calls != nullptr) {
      interfaces::WallTime at{};
      calls->record(CallKind::kDownload, r.status, r.outcome, wall_.now(&at) ? at.epoch : 0);
    }
  }
  counters_.bytes_downloaded += r.bytes;
  did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);
  did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kFetchRelease));
  if (did_.carried == interfaces::Ask::kNothing) did_.carried = interfaces::Ask::kFetchRelease;
  const bool resumed = r.resumed_from != 0 || held_before != 0;
  const std::uint64_t bytes = static_cast<std::uint64_t>(r.resumed_from) + r.bytes;

  // 200, not 160: the budget line names the two products' two different next wakes (task p10-04),
  // and at 160 the longer of them would be cut mid-sentence once the two byte counts are long.
  // It stays well inside the console's own 200-byte line (task p9-04) through LOG_I's prefix.
  char line_text[200];
  switch (r.verdict) {
    case upd::FetchVerdict::kStored: {
      counters_.offers_stored = saturate(counters_.offers_stored);
      char hex[2 * img::kSha256Size + 1];
      hexOf(r.sha256, img::kSha256Size, hex);
      if (expected_hex[0] != 0 && std::strcmp(expected_hex, hex) != 0) {
        ctx.log("the file the manifest chose is not the one the server expected; a different choice is not a refusal");
      }
      std::snprintf(line_text, sizeof line_text, "stored %s %s as the %s, %lu bytes%s", r.image_id, r.version,
                    walter ? "other slot's image" : (scanner ? "scanner image" : "candidate"),
                    static_cast<unsigned long>(bytes), resumed ? " (resumed)" : "");
      ctx.log(line_text);
      const std::size_t n = writeOfferResult(true, r.image_id, hex, bytes, 0, nullptr, resumed);
      if (n != 0) out.data(data_, n);
      out.outcome(mm::Outcome::Ok);
      return true;
    }
    case upd::FetchVerdict::kBudget:
      // The agreed behaviour (unit-protocol.md §5): no result, the lease expires, the next wake's
      // claim of the same job takes the download up again. What the next wake then does with the
      // bytes already in flash is not the same for the two products, and both messages say which
      // (task p10-04, tasks/phase-9/FOLLOWUPS.md): a **radio** image continues from the image
      // store's progress record, while the **Walter's own** other application slot is erased when
      // a write opens on it (`SelfUpdate::beginWrite`), so the file starts again from zero and the
      // plan pays for these bytes a second time. Making it resume is not this task's
      // (`esp_ota_resume` and a journalled byte count); saying so truthfully is.
      counters_.offers_left_leased = saturate(counters_.offers_left_leased);
      left_leased_ = job.id;
      std::snprintf(line_text, sizeof line_text,
                    "the wake's budget ended the download at %lu of %lu bytes: no result, the lease is left to "
                    "expire and the next wake %s",
                    static_cast<unsigned long>(r.held), static_cast<unsigned long>(r.expected),
                    walter ? "starts the file again from zero" : "continues it");
      ctx.log(line_text);
      LOG_I("jobs: %s", line_text);
      {
        const std::size_t n = writeOfferResult(
            false, nullptr, nullptr, bytes, 0,
            walter ? "the wake's budget ended the download; this slot keeps no progress, so the next wake "
                     "starts the file again from zero"
                   : "the wake's budget ended the download; the next wake resumes it",
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
  mm::JsonWriter w(data_, kDataBytes);
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

// The `journal` array, in one place: the result carries it at the end of a run and a progress
// report carries it as it is written (task p14-04), and the two must be the same shape or a reader
// who followed the job live would see the story change when it finished. A **free** function over
// an array of lines (task p16-04), so that an apply's journal, a session's and a test's fixture
// values all go through the one writer.
void writeJournalLines(mm::JsonWriter& w, const JobJournalLine* lines, int count) {
  if (lines == nullptr || count <= 0) return;
  w.key("journal");
  w.beginArray();
  for (int i = 0; i < count; ++i) {
    char at[24];
    isoOf(lines[i].at_epoch, at);
    w.comma();
    w.beginObject();
    w.key("at");
    w.string(at);
    w.key("step");
    w.string(lines[i].step);
    if (lines[i].note[0] != 0) {
      w.key("note");
      w.string(lines[i].note);
    }
    w.endObject();
  }
  w.endArray();
}

void FirmwareJobs::writeJournal(mm::JsonWriter& w) { writeJournalLines(w, lines_, line_count_); }

// `{"data":{"journal":[...]},"note":"<the newest line's step>"}`, straight into the envelope half
// of this object's one buffer. A session's journal goes out through this too (task p16-04): the
// shape is the same and the reason is the same, which is that a job's page must not say *nothing
// has come back yet* while a radio is being flashed for a quarter of an hour.
// of this object's one buffer - which is free for the whole of an apply: `data_` is not written
// until the result is, and an owed result is posted after the heartbeat, outside any job. The
// whole body is written by one JsonWriter rather than through `mm::writeProgressReport`, which
// would want a second buffer the size of the first (kResultBufferBytes is counted to the byte;
// rail R22).
void FirmwareJobs::postApplyProgress(long long job_id) {
  if (line_count_ == 0 || quietInAStep()) return;
  mm::JsonWriter w(body_, kEnvelopeBytes);
  w.beginObject();
  w.key("data");
  w.beginObject();
  writeJournal(w);
  w.endObject();
  w.key("note");
  w.string(lines_[line_count_ - 1].step);
  w.endObject();
  if (w.overflowed()) {
    // The journal at its bound would not fit the envelope. Nothing about the apply changes: the
    // result is written into its own buffer at the end and is the one that matters.
    counters_.progress_refused = saturate(counters_.progress_refused);
    return;
  }
  if (server_.postProgress(job_id, body_, w.length())) {
    counters_.progress_posted = saturate(counters_.progress_posted);
  } else {
    // A 4xx, a 5xx or a call that never came back. Counted, and that is all: an update machine is
    // not something to stop halfway for a server's bookkeeping.
    counters_.progress_refused = saturate(counters_.progress_refused);
  }
}

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

// The `lastTrial` object: what ended the trial, in the one shape both an apply's result and
// `twinoak.fw.state` carry (task p11-03).
void FirmwareJobs::writeTrial(mm::JsonWriter& w, const core::update::SelfUpdate::LastTrial& trial) {
  w.key("lastTrial");
  w.beginObject();
  w.key("reset");
  w.string(core::health::name(trial.reason));
  w.key("dump");
  w.string(core::health::describe(trial.dump));
  w.key("attributed");
  w.boolean(trial.attributed);
  if (trial.attributed) {
    if (trial.panic[0] != 0) {
      w.key("panic");
      w.string(trial.panic);
      w.key("panicCut");
      w.boolean(trial.panic_cut);
    }
    if (trial.task[0] != 0) {
      w.key("task");
      w.string(trial.task);
    }
    w.key("dumpHeld");
    w.boolean(trial.dump_now);
  }
  w.endObject();
}

std::size_t FirmwareJobs::writeApplyResult(ApplyOutcome outcome, const char* from, const char* to,
                                           Refusal refusal, const char* reason,
                                           const core::update::SelfUpdate::LastTrial* trial) {
  mm::JsonWriter w(data_, kDataBytes);
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
  writeJournal(w);
  if (refusal != Refusal::kNone) {
    w.key("refusal");
    w.string(name(refusal));
  }
  // After the contract's own keys (tests/fixtures/contracts/FirmwareApplyResult.json), so that
  // every reader that knew this shape still reads it: the sentence a person sees for a rolled-back
  // trial, and what ended it field by field (task p11-03).
  if (reason != nullptr && reason[0] != 0) {
    w.key("reason");
    w.string(reason);
  }
  if (trial != nullptr) writeTrial(w, *trial);
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
  // raised to what a whole run of the machine may hold it open for, the uplink's deadline with it.
  // The guard counts from the **boot** and this machine from here, so what the wake has already
  // spent is asked for too (`allow()` caps it at kRaisedBoundMax and never lowers a bound).
  extendLease(job.id, ctx);
  const std::uint64_t began = clock_.nowMs();
  const std::uint64_t bound = began + kApplyBoundMs;
  if (parts_.guard != nullptr) parts_.guard->allow(parts_.guard->awakeMs() + kApplyBoundMs);
  server_.setDeadline(bound);
  did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kUpdateRadio));
  did_.carried = interfaces::Ask::kUpdateRadio;
  line("check", "the machine's preconditions hold");
  postApplyProgress(job.id);
  bool bounded = false;
  while (parts_.update->active()) {
    // **Before the step, not after it** (task p14-04). A flash is one call of the flasher and a
    // quarter of an hour: the machine cannot come back to ask for more lease in the middle of it,
    // so what it may need is asked for here, whatever the five-minute timer says. `kFlashing` is
    // the candidate's, `kRollingBack` the golden image's on a rollback - the two steps that
    // cannot stop.
    const upd::State at = parts_.update->state();
    if (at == upd::State::kFlashing || at == upd::State::kRollingBack) {
      extendLease(job.id, ctx, kLeaseFlashSeconds);
    } else if (clock_.nowMs() >= extend_due_ms_) {
      extendLease(job.id, ctx);
    }
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
      // The line is written: the job's page gets it now rather than in a quarter of an hour.
      // Between two steps of the machine and never inside one.
      postApplyProgress(job.id);
    }
    if (!parts_.update->active()) break;
    if (clock_.nowMs() >= bound) {
      bounded = true;
      break;
    }
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
    case upd::State::kNodeFault:
      // **`stuck`, and the journal says why** (task p10-03). The server's vocabulary is four
      // words (`FirmwareApplyOutcomes`, unit-protocol.md §5) and a node fault is none of them:
      // not `committed` (nothing was promoted), not `rolledBack` (no rollback was spent, on
      // purpose), and not `refused` (the flash happened). `stuck` is the one that means *a
      // person's business now*, which this is - and the journal's last line names the refusal
      // and the two words that end it. A fifth outcome is the server's to add, not ours.
      outcome = ApplyOutcome::kStuck;
      counters_.stuck = saturate(counters_.stuck);
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

bool FirmwareJobs::owedResultDue() const {
  if (parts_.journal == nullptr) return false;
  JobEntry owed;
  if (!resultOwed(&owed)) return false;
  // **A session's owed result is due as soon as the session is not running** (task p16-04). There
  // is no trial to wait for: step 3 of this wake has either put the golden image back or left the
  // session open for the next wake to try again, and either of those is an answer the server is
  // waiting for. A session still in hand is not one: its own run will post the result.
  if (owed.product == kProductForSession) {
    return parts_.session == nullptr || !parts_.session->active();
  }
  upd::Entry owed_entry;
  if (!parts_.journal->lastOf(upd::kind::kAgentJob, &owed_entry)) return false;
  // The self-update's own last word since the switch. `postOwedResult` walks the whole story to
  // build the lines; this only asks the one question the wait turns on, which is whether there is
  // an outcome to report at all.
  upd::Entry e;
  upd::SelfPayload p;
  if (!parts_.journal->lastOf(upd::kind::kSelfUpdate, &e) || !upd::decode(e, &p)) return false;
  if (e.seq <= owed_entry.seq) return false;  // nothing has happened since the apply was journalled
  if (p.state == upd::SelfState::kConfirmed || p.state == upd::SelfState::kRolledBack) return true;
  // **The trial is over and nobody has written it down yet.** The journal says a new image was in
  // the air and *this boot is not it*: the bootloader gave up on the trial and the old image is
  // back, which `SelfUpdate::begin()` is about to journal as a rollback. This is asked before
  // that happens (main/main.cpp's boot order; task p14-05), so it makes the same finding rather
  // than waiting a wake for it.
  interfaces::OwnSlot running;
  if (parts_.self == nullptr || !parts_.self->runningSlot(&running)) return false;
  return p.slot != running.index;
}

bool FirmwareJobs::lastEnded(JobEntry* out, std::uint8_t product) const {
  if (parts_.journal == nullptr || out == nullptr) return false;
  upd::Cursor c;
  upd::Entry e;
  bool more = parts_.journal->first(&c, &e);
  int steps = 0;
  while (more && steps++ < 1024) {
    JobEntry j;
    if (decode(e, &j) && j.state == JobEntryState::kEnded && j.product == product) {
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
  // A session this board restarted in the middle of (task p16-04): another journal, another
  // shape, the same rule - the result is owed and the next answered heartbeat carries it.
  if (owed.product == kProductForSession) return postOwedSessionResult(owed);
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
  // What ended the trial, when it was rolled back (task p11-03): the reset reason of the boot
  // that found it and, when the `coredump` partition held the trial image's own dump, the panic
  // line that killed it. On 9 November this was all the server could be told: *the image never
  // reached its main loop (the bootloader gave up on it)*, and the panic line sat on the board.
  core::update::SelfUpdate::LastTrial trial;
  bool have_trial = false;
  char reason[256] = {};
  if (outcome == ApplyOutcome::kRolledBack && parts_.self != nullptr) {
    have_trial = parts_.self->lastTrial(&trial);
  }
  // **How hard the backend was asked**, when that is what the trial was rolled back for (task
  // p14-05). The sentence v0.1.6 was condemned with on 15 November 2026 - *the backend was not
  // reached inside the trial window* - was true of a trial that never sent one heartbeat, and
  // read exactly like a unit that cannot attach. These are the figures that tell the two apart,
  // and they are written by the image that rejected itself (core/update/self_update.h,
  // kind::kTrialUplink). No entry means this rollback was for another reason.
  upd::TrialUplink tried;
  bool have_tried = false;
  if (outcome == ApplyOutcome::kRolledBack && parts_.self != nullptr) {
    have_tried = parts_.self->lastUplink(&tried);
  }
  char asked[128] = {};
  if (have_tried) {
    if (tried.nothingAsked()) {
      std::snprintf(asked, sizeof asked, "no heartbeat was sent in the %lu s the window lasted",
                    static_cast<unsigned long>(tried.elapsed_ms / 1000u));
    } else {
      std::snprintf(asked, sizeof asked, "%lu heartbeat(s) tried in %lu s; the last: %s",
                    static_cast<unsigned long>(tried.tries),
                    static_cast<unsigned long>(tried.elapsed_ms / 1000u), upd::describe(tried.last));
    }
    line("uplink", asked);
  }
  // The sentence a person reads on the job page, one clause at a time: the rollback's reason,
  // then what ended the trial and how hard the backend was asked - whichever of the two the
  // journal kept.
  if (have_trial || have_tried) {
    joinText(reason, sizeof reason, "rolled back: ", upd::describe(last.reason), "", "");
  }
  if (have_trial) {
    char with_reset[sizeof reason];
    joinText(with_reset, sizeof with_reset, reason, " - the reset that ended it: ",
             core::health::name(trial.reason), "");
    copyText(reason, sizeof reason, with_reset);
    if (trial.attributed && trial.panic[0] != 0) {
      char with_panic[sizeof reason];
      joinText(with_panic, sizeof with_panic, reason, " - panic: ", trial.panic,
               trial.panic_cut ? " [cut]" : "");
      copyText(reason, sizeof reason, with_panic);
      if (trial.task[0] != 0) {
        joinText(with_panic, sizeof with_panic, reason, " - in task ", trial.task, "");
        copyText(reason, sizeof reason, with_panic);
      }
    }
    // And as journal lines, for a job page that renders the story and nothing else.
    line("reset", core::health::name(trial.reason));
    if (trial.attributed) {
      if (trial.panic[0] != 0) line("panic", trial.panic);
      if (trial.task[0] != 0) line("task", trial.task);
    }
  }
  if (have_tried) {
    char with_asked[sizeof reason];
    joinText(with_asked, sizeof with_asked, reason, " - ", asked, "");
    copyText(reason, sizeof reason, with_asked);
  }
  // The versions as the trial's entries carry them: `from` is the image that was running, `to`
  // the one switched to when confirmed and the one that ran before when rolled back.
  const char* to = outcome == ApplyOutcome::kCommitted ? last.to : last.from;
  const std::size_t n = writeApplyResult(outcome, last.from, to, Refusal::kNone,
                                         reason[0] != 0 ? reason : nullptr,
                                         have_trial ? &trial : nullptr);
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
  const std::size_t len = mm::writeJobResult(report, body_, kEnvelopeBytes);
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
  mm::JsonWriter w(data_, kDataBytes);
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
    // What ended the last trial, while it is the newest word in the story (task p11-03): the
    // reset reason the boot that found the rollback recorded, and whether a core dump was
    // attributed to the trial image, with its panic line. This is the half of the 9 November
    // fault the server could not see.
    core::update::SelfUpdate::LastTrial trial;
    if (parts_.self->lastTrial(&trial)) writeTrial(w, trial);
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
  mm::JsonWriter w(data_, kDataBytes);
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

// -- twinoak.report (task p13-01) ------------------------------------------------------------------
//
// What the unit says about itself, for whoever has to find a fault without the cable. On
// 13 November a wake request reached the unit's radio, the radio answered *ok*, and the unit did
// not check in; `wake`, `agent state` and `lte` say why, and they are behind a USB port on a unit
// meant to sit on a mast. This job returns those lines.
//
// **The lines are the console's own.** Nothing here renders anything: a section is a console
// command (core/console/report_source.h), run with its output going into this buffer instead of
// into the port, so that a line added to a command is a line the report gains with it and no
// wording of this firmware exists twice.
//
// **It changes nothing.** The sections are the read-only commands; `radio` asks this unit's own
// radio over the box link and nothing goes on the air. The one thing a report leaves behind is
// the record of the job itself - the library's `last ten`, and the lines the library logged,
// which are in the ring `dump` prints as every other line of this port is.
//
// **It fits.** The whole data object is kReportDataBytes; sections are written in the order asked;
// one that does not fit whole is cut at a line end and marked, and the ones after it are named in
// `left`. Every list this console prints is newest first, so a cut at the end loses the oldest,
// which is the end to lose.

namespace {

// What became of one name the payload asked for.
enum class Fate : std::uint8_t {
  kWritten,   // the section, whole
  kCut,       // the section, cut at a line end: the result was full
  kLeft,      // not started: the result was full
  kUnknown,   // this firmware has no section of that name
  kNotAName,  // not 1 to kReportNameMax characters of a-z: not answered at all
};

bool nameOk(const char* name) {
  const std::size_t n = std::strlen(name);
  if (n == 0 || n > kReportNameMax) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (name[i] < 'a' || name[i] > 'z') return false;
  }
  return true;
}

// One section's lines, escaped into the result as the console prints them.
//
// A section that does not fit is cut **at a line end**, so every call is split at every '\n' and
// the end of the last whole line is remembered. The escaping is `mm::JsonWriter`'s - the one JSON
// writer this firmware sends anything with, and the only thing here that has seen a stranger's
// bytes - done **in place**: the writer is given the room that is left and writes the line into
// it as a JSON string, quotes and all, and the two quotes are then taken off. There is no second
// buffer, which is what a board whose tightest heap arrangement has a few hundred bytes of margin
// can afford (task p11-01, rail R22).
class ReportText final : public interfaces::TextOut {
 public:
  ReportText(char* buffer, std::size_t at, std::size_t limit)
      : buffer_(buffer), pos_(at), text_end_(at), limit_(limit) {}

  void write(const char* text, std::size_t length) override {
    if (text == nullptr || cut_) return;
    std::size_t i = 0;
    while (i < length) {
      std::size_t end = i;
      while (end < length && text[end] != '\n') ++end;
      const bool whole = end < length;  // the '\n' is in this call
      const std::size_t n = whole ? end + 1 - i : length - i;
      if (!append(text + i, n)) {
        cut_ = true;
        return;
      }
      ends_with_newline_ = whole;
      if (whole) text_end_ = pos_ - kNewlineEscaped;
      i += n;
    }
  }

  // Not text. Nothing of a section writes bytes: a frame of the image transfer is the only thing
  // in this console that does, and it is not a section.
  bool writeBytes(const std::uint8_t*, std::size_t) override { return false; }

  bool cut() const { return cut_; }
  // Where the text ends: with no '\n' at the end, and at a line end when it was cut.
  std::size_t end() const { return (cut_ || ends_with_newline_) ? text_end_ : pos_; }

 private:
  // A '\n' is written as `\n`: two characters, which is what comes off the end.
  static constexpr std::size_t kNewlineEscaped = 2;

  bool append(const char* text, std::size_t n) {
    if (n == 0) return true;
    if (pos_ >= limit_) return false;
    mm::JsonWriter w(buffer_ + pos_, limit_ - pos_ + 1);
    w.string(text, n);
    if (w.overflowed() || w.length() < 2) return false;
    const std::size_t inner = w.length() - 2;  // without the writer's two quotes
    std::memmove(buffer_ + pos_, buffer_ + pos_ + 1, inner);
    pos_ += inner;
    return true;
  }

  char* buffer_;
  std::size_t pos_;
  std::size_t text_end_;
  std::size_t limit_;
  bool cut_ = false;
  bool ends_with_newline_ = false;
};

// The document around the sections: bounded, and written by hand rather than with mm::JsonWriter
// because a report is cut at a line end and a forward-only writer cannot be rewound. **Nothing
// written through this needs escaping**: it is this firmware's own punctuation and the section
// names, which nameOk() has held to a-z. The only text in the document is a section's lines, and
// those go through ReportText above, which is mm::JsonWriter.
struct Doc {
  char* out;
  std::size_t at;
  std::size_t cap;
  bool full;

  void raw(const char* text) {
    const std::size_t n = std::strlen(text);
    if (full || at + n >= cap) {
      full = true;
      return;
    }
    std::memcpy(out + at, text, n);
    at += n;
  }
  void number(std::uint32_t value) {
    char digits[12];
    std::snprintf(digits, sizeof digits, "%lu", static_cast<unsigned long>(value));
    raw(digits);
  }
};

}  // namespace

bool FirmwareJobs::runReport(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx,
                             mm::ResultWriter& out) {
  (void)job;
  counters_.reports = saturate(counters_.reports);
  const std::size_t plen = std::strlen(payload);
  const json::Value asked = json::member(payload, plen, "sections");
  if (!json::element(asked, 0).found()) {
    counters_.malformed = saturate(counters_.malformed);
    ctx.log("the payload names no sections");
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  if (parts_.report == nullptr) {
    ctx.log("this build has no console to read the unit out of");
    out.outcome(mm::Outcome::Failed);
    return true;
  }

  int count = 0;
  while (count < kReportSectionsMax && json::element(asked, count).found()) ++count;
  Fate fate[kReportSectionsMax] = {};

  Doc doc{buffer_, 0, kReportDataBytes, false};
  doc.raw("{\"at\":");
  doc.number(nowEpoch());
  doc.raw(",\"sections\":[");
  bool first = true;
  bool full = false;
  int written = 0;
  int left = 0;
  int unknown = 0;
  int dropped = 0;  // not 1 to kReportNameMax characters of a-z: the contract says they are
  char name[kReportNameMax + 1] = {};
  for (int i = 0; i < count; ++i) {
    if (!json::text(json::element(asked, i), name, sizeof name) || !nameOk(name)) {
      fate[i] = Fate::kNotAName;
      ++dropped;
      continue;
    }
    bool have = false;
    for (int s = 0; s < parts_.report->sectionCount() && !have; ++s) {
      have = std::strcmp(name, parts_.report->sectionName(s)) == 0;
    }
    if (!have) {
      fate[i] = Fate::kUnknown;
      ++unknown;
      continue;
    }
    // A section is not begun with less room than its entry, a line or two of text and the two
    // lists at the end need: naming it in `left` says more than an entry that holds nothing.
    if (full || doc.at + kReportEntryBytes + kReportLeastText + kReportTailBytes > kReportDataBytes) {
      full = true;
      fate[i] = Fate::kLeft;
      ++left;
      continue;
    }
    doc.raw(first ? "{\"name\":\"" : ",{\"name\":\"");
    first = false;
    doc.raw(name);
    doc.raw("\",\"text\":\"");
    // The room the text may have: everything but the two lists at the end and this entry's own
    // closing `","cut":false}`.
    ReportText text(buffer_, doc.at, kReportDataBytes - kReportTailBytes - kReportEntryBytes);
    (void)parts_.report->writeSection(name, text);
    doc.at = text.end();
    doc.raw(text.cut() ? "\",\"cut\":true}" : "\",\"cut\":false}");
    fate[i] = text.cut() ? Fate::kCut : Fate::kWritten;
    ++written;
    if (text.cut()) {
      full = true;
      counters_.reports_cut = saturate(counters_.reports_cut);
    }
  }
  doc.raw("]");

  // The names asked for that this firmware has and did not start, and the ones it has no section
  // for: read back out of the payload, because nothing of them was kept.
  for (int pass = 0; pass < 2; ++pass) {
    doc.raw(pass == 0 ? ",\"left\":[" : ",\"unknown\":[");
    const Fate wanted = pass == 0 ? Fate::kLeft : Fate::kUnknown;
    bool first_name = true;
    for (int i = 0; i < count; ++i) {
      if (fate[i] != wanted) continue;
      if (!json::text(json::element(asked, i), name, sizeof name)) continue;
      doc.raw(first_name ? "\"" : ",\"");
      first_name = false;
      doc.raw(name);
      doc.raw("\"");
    }
    doc.raw("]");
  }
  doc.raw("}");

  if (doc.full) {
    // The reserves above are what stop this; it is here because a result that is not whole JSON
    // is worse than a job that says it failed.
    ctx.log("the report did not fit its buffer");
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  buffer_[doc.at] = 0;
  char note[128];
  std::snprintf(note, sizeof note,
                "%d section(s) written, %d left, %d unknown, %d not a name; %lu bytes", written, left,
                unknown, dropped, static_cast<unsigned long>(doc.at));
  ctx.log(note);
  out.data(buffer_, doc.at);
  out.outcome(mm::Outcome::Ok);
  return true;
}

// -- twinoak.noisescope.session (task p16-04) ------------------------------------------------------
//
// A session of the radio as a scanner, begun by the server instead of by a person at a port. The
// machine is task p16-03's and is **not** reimplemented here: the program comes out of the
// payload and is judged by that task's own rules, `start()` runs the whole check, `step()` is
// driven in slices inside this wake exactly as an apply's machine is, and what this file adds is
// the three things a mast needs and a cable does not - the lease, the lines on their way to the
// server, and a result that outlives a restart.
//
// **S-12 is the rail this job is written against.** A scanner's line goes into a post and nowhere
// else: not the log, not the journal, not a report, not the job's `log`. The room is read where it
// lies and nothing of it is copied anywhere by this end, and every line this file writes is
// counts and words of its own.

namespace {

// The journal's word for a step, as the contract's example writes them. Static text, because a
// journal line keeps a **pointer** to its step word.
const char* const kSessionStepWords[core::update::kStepsMax] = {"step-1", "step-2", "step-3",
                                                                "step-4", "step-5", "step-6"};

std::uint32_t saturateBy(std::uint32_t value, std::uint32_t more) {
  return value + more < value ? 0xFFFFFFFFu : value + more;
}

}  // namespace

std::size_t writeSessionResult(const SessionResult& result, const JobJournalLine* journal,
                               int journal_count, char* out, std::size_t cap) {
  mm::JsonWriter w(out, cap);
  w.beginObject();
  w.key("outcome");
  w.string(name(result.outcome));
  if (result.scanner != nullptr && result.scanner[0] != 0) {
    w.key("scanner");
    w.string(result.scanner);
  }
  w.key("steps");
  w.beginArray();
  for (std::size_t i = 0; i < result.steps && i < core::update::kStepsMax; ++i) {
    w.comma();
    w.beginObject();
    w.key("ended");
    w.string(result.step[i].ended != nullptr ? result.step[i].ended : kStepNotRun);
    w.key("lines");
    w.integer(static_cast<long long>(result.step[i].lines));
    w.key("bytes");
    w.integer(static_cast<long long>(result.step[i].bytes));
    w.endObject();
  }
  w.endArray();
  w.key("lines");
  w.integer(static_cast<long long>(result.lines));
  w.key("linesUnanswered");
  w.integer(static_cast<long long>(result.lines_unanswered));
  writeJournalLines(w, journal, journal_count);
  if (result.refusal != nullptr && result.refusal[0] != 0) {
    w.key("refusal");
    w.string(result.refusal);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

bool readSessionPayload(const char* payload, std::size_t len, core::update::Program* program,
                        long long* slot, const char** why) {
  const char* reason = "";
  if (program != nullptr) *program = core::update::Program{};
  if (program == nullptr) {
    reason = "this build has nowhere to read a program into";
  } else if (payload == nullptr || payload[0] == 0 || len == 0) {
    // An empty text is a payload the claim could not keep: `kExtensionPayloadBytes` is what this
    // firmware holds of one, and the contract's own ceiling is the same figure.
    reason = "the payload did not fit the bytes a claim keeps";
  } else {
    long long which = 0;
    if (!json::integer(json::member(payload, len, "slot"), &which)) {
      reason = "the payload names no slot";
    } else {
      if (slot != nullptr) *slot = which;
      const json::Value steps = json::member(payload, len, "steps");
      std::size_t count = 0;
      for (int i = 0; i <= static_cast<int>(core::update::kStepsMax); ++i) {
        const json::Value one = json::element(steps, i);
        if (!one.found()) break;
        if (count >= core::update::kStepsMax) {
          // One more than the machine runs: the program is refused as a whole, with the rule
          // that was broken, rather than run as its first six steps.
          reason = "a program is at most six steps";
          break;
        }
        core::update::ProgramStep& step = program->step[count];
        const json::Value command = json::member(one.at, one.len, "command");
        if (!command.found()) {
          reason = "a step names no command";
          break;
        }
        // `json::text` answers false for a word that does not fit as well as for one that is not
        // a string, and the buffer is exactly the machine's own 40 characters and a terminator -
        // so a longer command is refused here in the words the machine would have used.
        if (!json::text(command, step.command, sizeof step.command)) {
          reason = "a step's command is at most 40 characters";
          break;
        }
        long long seconds = 0;
        if (!json::integer(json::member(one.at, one.len, "seconds"), &seconds)) {
          reason = "a step names no seconds";
          break;
        }
        step.seconds = seconds > 0 && seconds <= 0xFFFFFFFF ? static_cast<std::uint32_t>(seconds) : 0;
        ++count;
      }
      if (reason[0] == 0) {
        program->count = count;
        // **Task p16-03's own pass over the program** (deliverable 2): a person at a console and
        // a payload from the server are refused in the same words, because it is the same code.
        const char* broken = core::update::whyProgram(*program);
        if (broken != nullptr) reason = broken;
      }
    }
  }
  if (why != nullptr) *why = reason;
  return reason[0] == 0;
}

// -- the lines on their way to the server ----------------------------------------------------------

std::size_t FirmwareJobs::postSomeLines(long long job_id, const char* lines, std::size_t len,
                                        std::uint32_t from, std::uint32_t* count, bool* answered,
                                        bool* full) {
  *count = 0;
  *answered = false;
  *full = false;
  if (quietInAStep()) return 0;
  // **As many whole lines as fit one body** (deliverable 6): the body is cut at a line's end, so
  // every line the server holds is a line the scanner sent whole. A line longer than a body on
  // its own cannot be cut at all and goes in a post of its own rather than being split or
  // dropped: nothing a scanner sends is that long (§9 bounds a line at 7 600 bytes against a
  // body of 8 192), and losing a measurement would be worse than one body over the bound.
  std::size_t take = 0;
  std::uint32_t whole = 0;
  for (std::size_t at = 0; at < len;) {
    std::size_t end = at;
    while (end < len && lines[end] != '\n') ++end;
    const std::size_t one = (end < len ? end + 1 : len) - at;
    if (take != 0 && take + one > kLinesPostBytes) break;
    take += one;
    ++whole;
    at += one;
    if (take >= kLinesPostBytes) break;
  }
  if (take == 0 || whole == 0) return 0;

  ModemUplink::LinesReply reply;
  bool ok = server_.postLines(job_id, from, lines, take, &reply);
  if (!ok) {
    // **Once more, with the same `{from}`** (deliverable 6). The server leaves a line it already
    // holds alone, so a post that arrived and whose answer did not is harmless to repeat; after
    // this the lines of that post are counted as unanswered - they may have arrived - and this
    // end goes on to the next ones rather than holding the radio for a server.
    ok = server_.postLines(job_id, from, lines, take, &reply);
  }
  counters_.lines_posted = saturateBy(counters_.lines_posted, whole);
  *count = whole;
  *answered = ok;
  if (ok) {
    *full = reply.full;
  } else {
    counters_.lines_unanswered = saturateBy(counters_.lines_unanswered, whole);
    LOG_W("jobs: job %lld: %lu line(s) from %lu went unanswered twice; they may have arrived",
          job_id, static_cast<unsigned long>(whole), static_cast<unsigned long>(from));
  }
  return take;
}

void FirmwareJobs::SessionLines::onStepLines(const core::update::StepReport& report) {
  const std::size_t which = report.index > 0 && report.index <= core::update::kStepsMax
                                ? report.index - 1
                                : 0;
  // **A step that lost bytes at the port says so** (task p18-03), as a journal line of its own and
  // before its lines go out: the machine has written the same thing into the flash, and this is
  // the copy that reaches the server with the result and with the next progress post. It is said
  // whatever the server's room, because *nine lines are missing* is the one thing a reader of
  // that step cannot work out for themselves.
  if (report.losses != 0) {
    jobs_.counters_.steps_lossy = saturate(jobs_.counters_.steps_lossy);
    char note[kJournalNoteBytes];
    std::snprintf(note, sizeof note, "the port lost bytes %lu time(s): lines are missing",
                  static_cast<unsigned long>(report.losses));
    jobs_.line(kSessionStepWords[which], note);
  }
  // The server said its room for this job is used up: the step still ran and still ended, and
  // nothing more of it is posted (deliverable 6).
  if (full) return;
  std::size_t at = 0;
  while (at < report.used) {
    std::uint32_t count = 0;
    bool answered = false;
    bool is_full = false;
    const std::size_t took = jobs_.postSomeLines(job_id_, report.lines + at, report.used - at,
                                                 handed, &count, &answered, &is_full);
    if (took == 0) break;
    handed = saturateBy(handed, count);
    lines[which] = saturateBy(lines[which], count);
    bytes[which] = saturateBy(bytes[which], static_cast<std::uint32_t>(took));
    if (!answered) unanswered = saturateBy(unanswered, count);
    at += took;
    if (is_full) {
      full = true;
      break;
    }
  }
}

// -- the journal, and the one owed across a restart ------------------------------------------------

void FirmwareJobs::readSessionJournal() {
  if (parts_.journal == nullptr) return;
  clearLines();
  upd::Cursor cursor;
  upd::Entry entry;
  std::uint32_t looked = 0;
  bool more = parts_.journal->first(&cursor, &entry);
  // Backwards from the newest to the entry that **opened** the session, which is the `check` one,
  // and no further than a session may ever write (core/update/scanner_session.h). The lines are
  // put at the front as they are read, so that lines_ ends oldest first as a result's journal is.
  // The newest `check` is this session's: an open session is ended and never begun again, so two
  // of them cannot overlap.
  while (more && looked++ < upd::kEntriesPerSessionMax) {
    upd::SessionPayload p;
    if (entry.kind == upd::kind::kScannerSession && upd::decode(entry, &p)) {
      char note[kJournalNoteBytes];
      note[0] = 0;
      const char* word = upd::name(p.stage);
      if (p.stage == upd::Stage::kStep) {
        const std::size_t which = p.step > 0 && p.step <= upd::kStepsMax ? p.step - 1u : 0u;
        word = kSessionStepWords[which];
        if ((p.flags & upd::kSessionFlagLost) != 0) {
          // Task p18-03's entry: this one is the step's **end**, and what it says is that lines
          // the scanner numbered are not here. Read back after a restart exactly as it was said
          // while the session ran.
          std::snprintf(note, sizeof note, "the port lost bytes %lu time(s): lines are missing",
                        static_cast<unsigned long>(p.losses));
        } else {
          copyText(note, sizeof note, p.command);
        }
      } else if (p.stage == upd::Stage::kRoom) {
        (void)upd::roomLine(p.room, note, sizeof note);
      }
      // **The journal stamps no time of its own** (core/update/journal.h: an entry is a sequence
      // number, a kind and a payload), so the epoch every line of a session read back out of
      // flash carries is the one the session was opened at. It is the honest value this end has
      // after a restart; the lines of a session posted as it ran carry the moment each was
      // written.
      lineAtFront(word, note, p.at_epoch);
      if (p.stage == upd::Stage::kCheck) break;  // the session's first entry
    }
    more = parts_.journal->previous(&cursor, &entry);
  }
}

void FirmwareJobs::lineAtFront(const char* step, const char* note, std::uint32_t at_epoch) {
  const std::size_t cost = std::strlen(step) + (note != nullptr ? std::strlen(note) : 0);
  if (line_count_ >= kJournalMaxEntries || line_chars_ + cost > kJournalMaxChars) return;
  for (int i = line_count_; i > 0; --i) lines_[i] = lines_[i - 1];
  ++line_count_;
  Line& l = lines_[0];
  l.at_epoch = at_epoch;
  l.step = step;
  copyText(l.note, sizeof l.note, note);
  line_chars_ += std::strlen(step) + std::strlen(l.note);
}

bool FirmwareJobs::postOwedSessionResult(const JobEntry& owed) {
  if (parts_.journal == nullptr) return false;
  // A session still in hand is one whose own run posts the result: this is for the session a
  // restart left behind, which step 3 of this wake has already ended one way or the other.
  if (parts_.session != nullptr && parts_.session->active()) return false;
  upd::Mark mark;
  if (!upd::readMark(*parts_.journal, &mark) || !mark.found) return false;
  readSessionJournal();

  SessionResult result;
  // **Open means `stuck`** (the contract): the golden image did not come back - this wake tried
  // and the next one will try again - and the server is told so rather than waiting. Closed means
  // the repeater is back and proved, whatever became of the program.
  result.outcome = mark.open ? SessionOutcome::kStuck : SessionOutcome::kRestored;
  result.scanner = owed.to_version[0] != 0 ? owed.to_version : nullptr;
  result.steps = owed.steps;
  // `notRun` for every step, and counts of nought: the payload is gone with the RAM that held it,
  // and what this end can vouch for is what the journal says (deliverable 9).
  const std::size_t n = writeSessionResult(result, lines_, line_count_, data_, kDataBytes);
  if (n == 0) return false;

  char client_id[64];
  char hash_hex[2 * upd::kHash16 + 1];
  hexOf(owed.hash16, upd::kHash16, hash_hex);
  std::snprintf(client_id, sizeof client_id, "%lld-session-%s", owed.job_id, hash_hex);
  mm::JobReport report;
  report.clientId = client_id;
  report.outcome = result.outcome == SessionOutcome::kRestored ? mm::Outcome::Ok : mm::Outcome::Failed;
  report.startedAt = owed.at_epoch;
  report.finishedAt = nowEpoch();
  report.transactions = 0;
  report.data = data_;
  report.log = "a session of the radio as a scanner: how it ended, posted from the journal after "
               "the restart";
  const std::size_t len = mm::writeJobResult(report, body_, kEnvelopeBytes);
  if (len == 0) return false;
  if (!server_.postJobResult(owed.job_id, body_, len)) return false;
  counters_.owed_posted = saturate(counters_.owed_posted);

  JobEntry posted = owed;
  posted.state = JobEntryState::kPosted;
  posted.outcome = result.outcome == SessionOutcome::kRestored ? ApplyOutcome::kCommitted
                                                               : ApplyOutcome::kStuck;
  posted.at_epoch = nowEpoch();
  (void)journal(posted);
  JobEntry ended = posted;
  ended.state = JobEntryState::kEnded;
  (void)journal(ended);
  LOG_I("jobs: the session of job %lld: %s, posted from the journal after the restart", owed.job_id,
        name(result.outcome));
  return true;
}

// -- the job ---------------------------------------------------------------------------------------

bool FirmwareJobs::refuseSession(mm::ResultWriter& out, upd::Refusal refusal, const char* note,
                                 std::size_t steps) {
  counters_.sessions_refused = saturate(counters_.sessions_refused);
  line("check", note);
  SessionResult result;
  result.outcome = SessionOutcome::kRefused;
  result.steps = steps;
  // **`journal` is not one of the contract's eight words** (task p16-03's own ninth refusal): a
  // journal that will not take the entry that opens a session is not a precondition a server can
  // do anything about, so the outcome is `refused` with no word and the reason is the journal's
  // `check` line - which is what an apply does with a precondition the server has no word for.
  result.refusal = refusal == upd::Refusal::kJournal ? nullptr : upd::word(refusal);
  const std::size_t n = writeSessionResult(result, lines_, line_count_, data_, kDataBytes);
  if (n != 0) out.data(data_, n);
  out.outcome(mm::Outcome::Failed);
  return true;
}

bool FirmwareJobs::runSession(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx,
                              mm::ResultWriter& out) {
  counters_.sessions = saturate(counters_.sessions);
  clearLines();
  // **The program is this call's own** (core/update/scanner_session.h, Program): the machine keeps
  // a pointer to it for the length of the run and not a copy, and the run ends inside this call.
  // A few hundred bytes of this frame, where six commands as .bss would be six commands the main
  // task's stack has to be found around (rail R22).
  upd::Program program;
  long long slot = 0;
  const char* why = "";
  if (!readSessionPayload(payload, std::strlen(payload), &program, &slot, &why)) {
    counters_.malformed = saturate(counters_.malformed);
    ctx.log(why);
    return refuseSession(out, upd::Refusal::kProgram, why, 0);
  }
  const std::size_t rows = program.count;
  if (parts_.session == nullptr || parts_.store == nullptr) {
    ctx.log("this build has no session machine (it needs the image store, the journal and room to "
            "borrow)");
    out.outcome(mm::Outcome::Failed);
    return true;
  }
  // S-7 as the wake knows it, before the machine's own check, and in the same words an apply
  // refuses it with: this firmware on trial, or an update of either MCU mid-run.
  if (context_.on_trial) {
    return refuseSession(out, upd::Refusal::kInProgress, "this firmware is on trial (S-7)", rows);
  }
  if (context_.update_running) {
    return refuseSession(out, upd::Refusal::kInProgress,
                         "an update is journalled as in progress (S-7)", rows);
  }
  // The slot the payload names has to be one this board has: a single adapter has slot 0 and
  // nothing else, and a program for a radio that is not there is not a program this unit runs.
  const core::board::Slot at{static_cast<std::uint8_t>(slot >= 0 && slot < 256 ? slot : 255)};
  if (slot < 0 || !slots_.exists(at)) {
    return refuseSession(out, upd::Refusal::kProgram,
                         "the payload names a slot this board has not", rows);
  }

  // **The whole check, and the session open in the journal before anything is flashed** - task
  // p16-03's `start()`, which is the one door a job may open a session by (rail R20). A refusal
  // leaves the radio, the store and the heap exactly as they were.
  SessionLines lines(*this, job.id);
  if (!parts_.session->start(program, at, &lines)) {
    const upd::Refusal refusal = parts_.session->refusal();
    char note[kJournalNoteBytes];
    std::snprintf(note, sizeof note, "%.70s", upd::describe(refusal));
    parts_.session->clear();
    return refuseSession(out, refusal, note, rows);
  }

  // **The lease before the first byte** (deliverable 4), and the wake held open for a whole
  // session: two passes of the radio's update machine with the twenty-minute ceiling between
  // them. The guard counts from the **boot** and this machine from here, so what the wake has
  // already spent is asked for too (`allow()` caps it at kRaisedBoundMax and never lowers a
  // bound), and the uplink's own deadline goes with it - every post of a line and of the journal
  // takes what is left of it and no more, which is what keeps a server from holding the radio.
  extendLease(job.id, ctx);
  const std::uint64_t began = clock_.nowMs();
  const std::uint64_t bound = began + kSessionBoundMs;
  if (parts_.guard != nullptr) parts_.guard->allow(parts_.guard->awakeMs() + kSessionBoundMs);
  server_.setDeadline(bound);
  // S-7: a session is two passes of the radio's update machine, so the wake is told its one
  // update step went (task p16-03's rail 7, `core/cycle/wake_cycle.h`'s step 9).
  did_.asks = static_cast<std::uint8_t>(did_.asks | interfaces::askBit(interfaces::Ask::kUpdateRadio));
  did_.carried = interfaces::Ask::kUpdateRadio;

  // The scanner image this session is about to run, for the result's `scanner` and for the entry
  // the result is owed by.
  img::Record scanner{};
  (void)parts_.store->read(img::Which::kScanner, at, &scanner);

  // **The result is owed from here on** (deliverable 9). The scanner is about to go on, and a
  // board that restarts between the two switches must still tell the server how it ended: the
  // entry says which job, which slot, how many steps its payload had and which image ran, and
  // the next boot that reaches the server posts `restored` or `stuck` from it and the session's
  // own journal.
  JobEntry owed;
  owed.job_id = job.id;
  owed.state = JobEntryState::kOwed;
  owed.product = kProductForSession;
  owed.slot = at.index;
  owed.at_epoch = nowEpoch();
  owed.steps = static_cast<std::uint8_t>(rows);
  std::memcpy(owed.hash16, scanner.sha256, upd::kHash16);
  shortOf(scanner.version, owed.to_version);
  if (!journal(owed)) {
    // The session itself is journalled as open - `start()` would not have returned true otherwise
    // - so the repeater comes back whatever happens next. What a journal that will take no more
    // entries costs is this one result, and the server hears that from the line below and from
    // the job's own page when the result goes.
    ctx.log("the journal would not take the entry a result is owed by: a restart from here would "
            "leave this session's result untold");
  }

  // The journal's first line, and the first progress post with it (deliverable 5).
  char note[kJournalNoteBytes];
  if (context_.battery_known) {
    std::snprintf(note, sizeof note, "battery %u mV, link up, clock set",
                  static_cast<unsigned>(context_.battery_mv));
  } else {
    copyText(note, sizeof note, "link up, clock set");
  }
  line("check", note);
  postApplyProgress(job.id);

  bool bounded = false;
  bool scanner_ran = false;
  while (parts_.session->active()) {
    // **The lease before the step, never inside one** (deliverable 4, and task p18-03's second):
    // a flash is one call of the flasher and a quarter of an hour, and the machine cannot come
    // back to ask for more lease in the middle of it. `kFlashing` is the scanner image's and the
    // golden image's alike - whichever of the two kinds the machine is running - and
    // `kRollingBack` is the scanner run putting the golden image back by itself.
    //
    // **And never inside a step either** (task p18-03). `in_step_` is the machine's own answer:
    // the command is said and the step has not ended, so the port is the only thing being
    // listened to and a second spent on LTE-M is a second of lines lost. The timer below cannot
    // fire there; what takes its place is `stepPending()` - the seconds of the step whose command
    // is the next thing to happen - and a lease that would not cover the step, the posting of its
    // lines and the calls after them is extended now, while the scanner is still silent.
    if (!parts_.session->listening()) {
      const upd::State state =
          parts_.update != nullptr ? parts_.update->state() : upd::State::kIdle;
      const std::uint32_t pending = parts_.session->stepPending();
      const std::uint64_t needed = pending != 0
                                       ? clock_.nowMs() +
                                             static_cast<std::uint64_t>(pending) * 1000u +
                                             kLeaseAfterStepMs
                                       : 0;
      if (state == upd::State::kFlashing || state == upd::State::kRollingBack) {
        extendLease(job.id, ctx, kLeaseFlashSeconds);
      } else if (needed > lease_until_ms_) {
        extendLease(job.id, ctx);
      } else if (clock_.nowMs() >= extend_due_ms_) {
        extendLease(job.id, ctx);
      }
    }
    const upd::SessionStep s = parts_.session->step();
    if (s.changed) {
      const upd::SessionStatus& status = parts_.session->status();
      const char* word = upd::name(s.after);
      note[0] = 0;
      if (s.after == upd::Stage::kStep) {
        const std::size_t which =
            status.step > 0 && status.step <= upd::kStepsMax ? status.step - 1 : 0;
        word = kSessionStepWords[which];
        copyText(note, sizeof note, program.step[which].command);
      } else if (s.after == upd::Stage::kRoom) {
        // **The room's five figures, out to the server with every session** (deliverable 8): it
        // is how 8 192 bytes gets raised from what units say rather than from a guess.
        (void)upd::roomLine(status.room, note, sizeof note);
      } else {
        std::snprintf(note, sizeof note, "%.70s", s.note);
      }
      line(word, note);
      postApplyProgress(job.id);
      if (s.after == upd::Stage::kScanner) scanner_ran = true;
    }
    if (s.stalled) break;  // the journal would take no entry: nothing more can be done here
    if (!parts_.session->active()) break;
    if (clock_.nowMs() >= bound) {
      bounded = true;
      break;
    }
    if (s.waited) ctx.platform().delayMs(kSessionIdleMs);
  }
  did_.outside_ms += static_cast<std::uint32_t>(clock_.nowMs() - began);

  if (bounded) {
    // The bound a wake may be held open to passed with the session mid-run. No result can say
    // what has not happened; the journal says a session is open, step 3 of the next wake ends it
    // (task p16-03's rail 2), and the entry above is what makes that wake post the result.
    left_leased_ = job.id;
    ctx.log("the session ran past the bound on this wake: the journal carries it, and the result "
            "follows from the next wake that reaches the server");
    LOG_W("jobs: session %lld: past the wake's bound at %s", job.id,
          upd::name(parts_.session->stage()));
    out.outcome(mm::Outcome::Timeout);
    return true;
  }

  const upd::SessionStatus& status = parts_.session->status();
  SessionResult result;
  result.outcome = status.ending == upd::Ending::kRestored ? SessionOutcome::kRestored
                                                           : SessionOutcome::kStuck;
  result.scanner = scanner_ran && scanner.present ? scanner.version : nullptr;
  result.steps = rows;
  for (std::size_t i = 0; i < rows && i < upd::kStepsMax; ++i) {
    // How a step ended is the **machine's** record of it and not this end's; what it handed to
    // its posts is this end's.
    result.step[i].ended = endWord(status.step_end[i]);
    result.step[i].lines = lines.lines[i];
    result.step[i].bytes = lines.bytes[i];
  }
  result.lines = lines.handed;
  result.lines_unanswered = lines.unanswered;

  JobEntry ended = owed;
  ended.state = JobEntryState::kEnded;
  ended.outcome = result.outcome == SessionOutcome::kRestored ? ApplyOutcome::kCommitted
                                                              : ApplyOutcome::kStuck;
  ended.at_epoch = nowEpoch();
  (void)journal(ended);

  char line_text[200];
  std::snprintf(line_text, sizeof line_text,
                "the session ended %s: %lu line(s) posted in all, %lu of them unanswered%s",
                name(result.outcome), static_cast<unsigned long>(result.lines),
                static_cast<unsigned long>(result.lines_unanswered),
                lines.full ? "; the server's room for this job was used up" : "");
  ctx.log(line_text);
  const std::size_t n = writeSessionResult(result, lines_, line_count_, data_, kDataBytes);
  if (n != 0) out.data(data_, n);
  out.outcome(result.outcome == SessionOutcome::kRestored ? mm::Outcome::Ok : mm::Outcome::Failed);
  parts_.session->clear();
  return true;
}

}  // namespace agent
