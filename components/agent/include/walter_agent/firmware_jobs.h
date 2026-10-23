// The four firmware jobs of a repeater-v2 unit (task p4-05): meshmaster.dk's `twinoak.fw.offer`,
// `twinoak.fw.apply`, `twinoak.fw.state` and `twinoak.power.cycle` as the copied library's
// `mm::IJobExtension` (agent/IJobExtension.h), mapped onto what phase 2 and phase 3 built - the
// image store, the fetch of a release, the radio's update machine, the Walter's own, the
// supervisor's record and the kill line. **Nothing here decides anything an update machine
// decides**: the server names a release and a slot, the manifest and the six checks choose the
// file, the machines flash, prove, commit or roll back, and this reports what they journalled.
// The page this is written against is meshmaster.dk's `docs/dev/unit-protocol.md` §5 (task 127
// of its pile 4.14) and the contract fixtures `tests/fixtures/contracts/Firmware*.json` and
// `PowerCycle*.json`, copied under host_test/agent-core/; the report of this task maps each step
// of that page to the code below.
//
//   twinoak.fw.offer    phase 3's Fetch (core/update/fetch.h) with the payload's `base` under
//                       `server.url` and the server's token: the manifest and its signature
//                       (checks 1-3), the file for the slot from `{base}/files/` in ranges into
//                       the candidate slot, resumable across wakes through the store's own
//                       progress record; progress every kProgressEveryBytes through the uplink's
//                       `postProgress`, the lease extended through `extendLease` before the first
//                       byte and every kExtendEveryMs after; `stored` with the image id, the hash
//                       and the bytes, or the failed check's number and the reason. A wake whose
//                       budget ends the download mid-file posts **no result**: the lease is left
//                       to expire and the next wake's claim of the same job continues the file
//                       (unit-protocol.md §5 says so; the report says how a library whose runner
//                       always posts was made to post nothing - ResultGate below). The Walter's
//                       own product goes into the other application slot through the same six
//                       checks (SelfUpdate as the transfer target).
//   twinoak.fw.apply    RadioUpdate run **in slices inside the wake**, the lease extended while
//                       it runs, the bound on the wake raised to what an update may hold it open
//                       for; the result's `outcome` and `journal` from the machine's own
//                       transitions, bounded to the shape's 40 lines and 2 000 characters here;
//                       every precondition the machine refuses mapped to the result's seven
//                       refusal words where one fits. The candidate's hash must be the payload's
//                       `expectedSha256`, else `refused`; `firstInstall` only when the payload
//                       says so. An apply claimed while the other MCU's update is unconfirmed, or
//                       this firmware is on trial, is `refused` with `inProgress` (S-7). The
//                       Walter's own apply is SelfUpdate's switch, which ends in a restart: the
//                       job is journalled as **owed** first (kind::kAgentJob), and the result is
//                       posted by the next boot's first wake from that entry and the self-update's
//                       own, after the trial - whose third condition is the agent's heartbeat
//                       (walter_agent/agent_uplink_check.h).
//   twinoak.fw.state    the store's three records, what HELLO says is running, the slot's last
//                       apply from this object's journal entries, the Walter's two slots and the
//                       supervisor's incidents, into the result shape. Nothing on the air.
//   twinoak.power.cycle RadioPins::killCycle within the board's bounds (core/board/radio_pins.h)
//                       and the band's permission (core/power/interval_policy.h), then HELLO
//                       within the boot window; `offMs`, `radioBack` and the new `boot_id`.
//
// Every result is bounded: the data is written into one buffer of this object by the library's
// own writer and a result that would not fit is a bug a test catches. The journal of an apply is
// truncated here, never left to the server.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object -
// which is a namespace-scope object in main/, never a local (a few kilobytes of buffers).
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IJobExtension.h>
#include <agent/JobEnvelopes.h>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/cycle/wake_guard.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/power/interval_policy.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/fetch.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "interfaces/backend.h"
#include "interfaces/clock.h"
#include "interfaces/wall_clock.h"
#include "walter_agent/modem_uplink.h"

namespace agent {

namespace lc = core::link::client;

// The four types, as meshmaster.dk's `JobTypes` spells them, and the two products.
constexpr const char kOfferType[] = "twinoak.fw.offer";
constexpr const char kApplyType[] = "twinoak.fw.apply";
constexpr const char kStateType[] = "twinoak.fw.state";
constexpr const char kPowerCycleType[] = "twinoak.power.cycle";
constexpr int kFirmwareJobTypes = 4;
constexpr const char kProductRepeater[] = "twinoak-repeater-firmware";
constexpr const char kProductWalter[] = "twinoak-walter-firmware";
// Where a release's files are under its base (unit-protocol.md §4).
constexpr const char kFilesDir[] = "files";

// The lease: an offer and an apply ask for this much when they begin and again every
// kExtendEveryMs while they run - well inside the server's thirty-minute ceiling on an extension
// and well ahead of a lease that is at least two minutes (meshmaster.dk's JobDefaults), so that a
// download or a proving period never runs past what the server believes this unit holds.
constexpr int kLeaseExtendSeconds = 900;
constexpr std::uint32_t kExtendEveryMs = 300000;

// A progress report every two of the download's 64 KB ranges: the job's page prints *412 kB of
// 1.2 MB*, and a report per range would be a request per range on a metered link.
constexpr std::uint32_t kProgressEveryBytes = 2 * 64 * 1024;

// The result's journal, as the server bounds it (meshmaster.dk's FirmwareJournalBounds): the unit
// truncates to this, and the server flags a row that carried more.
constexpr int kJournalMaxEntries = 40;
constexpr std::size_t kJournalMaxChars = 2000;
constexpr std::size_t kJournalNoteBytes = 72;

// The result's data buffer: a state result is some 700 bytes and an apply's journal at its bound
// a little over 2 kB with its envelope; 4 kB holds both with room, inside the library's own 8 kB.
constexpr std::size_t kDataBytes = 4096;

// After a power cycle the radio has the update machine's boot window to answer HELLO in.
constexpr std::uint32_t kPowerCycleHelloWindowMs = core::update::kBootWindowMs;
constexpr std::uint32_t kPowerCycleHelloIntervalMs = core::update::kBootHelloIntervalMs;

// The slices an apply is driven in: the machine's own idle, and the bound a wake may be held
// open to for it (core/cycle/wake_guard.h: thirty minutes, the most a wake may ask for, because
// an apply inside an exchange sits behind a heartbeat and a claim that have already spent some).
constexpr std::uint32_t kApplyIdleMs = core::update::kStepIdleMs;
constexpr std::uint32_t kApplyBoundMs = core::cycle::kRaisedBoundMax;

// -- the words of the result shapes --------------------------------------------------------------

// `FirmwareApplyOutcomes`: the four ways an apply ends.
enum class ApplyOutcome : std::uint8_t { kNone = 0, kCommitted = 1, kRolledBack = 2, kRefused = 3, kStuck = 4 };
const char* name(ApplyOutcome outcome);

// `FirmwareRefusals`: the seven preconditions the server has a word for. A precondition without
// one (no candidate, the wrong role or board, an unreadable image, the candidate another file than
// the one offered) is `refused` with no word and the reason in the journal's `check` line.
enum class Refusal : std::uint8_t {
  kNone = 0,
  kBattery = 1,
  kLink = 2,
  kClock = 3,
  kInProgress = 4,
  kNoGolden = 5,
  kBadList = 6,
  kOrder = 7,
};
const char* name(Refusal refusal);
// The machines' own reasons, in the server's words where there is one.
Refusal refusalFor(core::update::Reason reason);
Refusal refusalFor(core::update::SelfReason reason);
// The radio update's states as the journal's `step` words (unit-protocol.md §5: `check`,
// `flashing-candidate`, `booting`, `proving`, `committing`, `rolling-back`, `restored`, `done`).
const char* stepWord(core::update::State state);

// -- the journal entry: kind::kAgentJob, 48 bytes ------------------------------------------------
//
//    0  job_id      i64   the server's job id
//    8  state       u8    kOwed: a Walter apply switched and the result is owed to the next boot;
//                         kPosted: the owed result went; kEnded: an apply ended, for `state`'s
//                         lastUpdate
//    9  outcome     u8    an ApplyOutcome (kEnded and kPosted)
//   10  product     u8    0 the radio's, 1 the Walter's
//   11  slot        u8
//   12  at_epoch    u32   0 when the unit had no time of day
//   16  hash16      16 B  the first sixteen bytes of the image's SHA-256
//   32  to_version  12 B  `X.Y.Z` the slot ran afterwards (core/update/self_update.h's short form)
//   44  spare       4 B
enum class JobEntryState : std::uint8_t { kNone = 0, kOwed = 1, kPosted = 2, kEnded = 3 };

struct JobEntry {
  long long job_id = 0;
  JobEntryState state = JobEntryState::kNone;
  ApplyOutcome outcome = ApplyOutcome::kNone;
  std::uint8_t product = 0;
  std::uint8_t slot = 0;
  std::uint32_t at_epoch = 0;
  std::uint8_t hash16[core::update::kHash16] = {};
  char to_version[core::update::kShortVersion] = {};
};

constexpr std::size_t kJobEntryBytes = 48;
static_assert(kJobEntryBytes <= core::update::kPayloadMax, "the entry has to fit a journal payload");
void encode(const JobEntry& entry, std::uint8_t out[kJobEntryBytes]);
bool decode(const core::update::Entry& entry, JobEntry* out);  // false unless it is an agent job entry

// -- what one wake tells the jobs ----------------------------------------------------------------

struct WakeContext {
  bool uplink_up = false;
  core::power::Band band = core::power::Band::kUnknown;
  core::power::Allowance may{};
  bool on_trial = false;        // this firmware is proving itself (S-6): no apply
  bool update_running = false;  // an update of either MCU is mid-run (S-7): no apply
  bool radio_silent = false;    // the supervisor has classified the link as down
  // The battery as this wake read it (the median of the wake's readings), for the power cycle's
  // own floor - the supervisor's rung 3's (core/supervisor/supervisor.h, kPowerCycleMinMv).
  bool battery_known = false;
  std::uint16_t battery_mv = 0;
  std::uint64_t deadline_ms = 0;
  // The most an offer's download may fetch this wake, in bytes: an LTE plan is metered in bytes,
  // and a wake that would pass it stops at a resumable range boundary and the next wake continues
  // (core/net/download.h). 0 is the whole file. The library prices a job by transactions and a
  // download is minutes, so the wake's **time** cannot be what stops a half-done download - it
  // would stop the library starting the job at all - which is why the cap is bytes.
  std::uint64_t download_bytes = 0;
};

// What the jobs of one wake did, for the seam's answer (interfaces/backend.h): the bits of the
// closed set they carried out, the one update step that went, and the time the machines took.
struct Did {
  std::uint8_t asks = 0;
  interfaces::Ask carried = interfaces::Ask::kNothing;
  std::uint32_t outside_ms = 0;
};

struct FirmwareJobCounters {
  std::uint32_t offers = 0;
  std::uint32_t offers_stored = 0;
  std::uint32_t offers_refused = 0;      // a check, a bad URL, a denial
  std::uint32_t offers_left_leased = 0;  // the budget ended the download: no result, the lease expires
  std::uint32_t applies = 0;
  std::uint32_t committed = 0;
  std::uint32_t rolled_back = 0;
  std::uint32_t refused = 0;
  std::uint32_t stuck = 0;
  std::uint32_t switches = 0;            // the Walter's own: a switch asked for
  std::uint32_t owed_posted = 0;         // results posted after a boot, from the journal
  std::uint32_t states = 0;
  std::uint32_t power_cycles = 0;
  std::uint32_t power_cycles_refused = 0;
  std::uint32_t progress_posted = 0;
  std::uint32_t extends = 0;
  std::uint64_t bytes_downloaded = 0;
  std::uint32_t malformed = 0;           // a payload this firmware could not read
};

class FirmwareJobs final : public mm::IJobExtension, public ResultGate {
 public:
  // What the jobs map onto. The pointers may be null in a build that has no store, no journal,
  // no machine: the job then answers `failed` in one sentence. `guard` raises the bound on the
  // wake for an apply; null in a test that drives the clock itself.
  struct Parts {
    core::image::Store* store = nullptr;
    core::update::RadioUpdate* update = nullptr;
    core::update::SelfUpdate* self = nullptr;
    core::update::Fetch* fetch = nullptr;
    core::update::Journal* journal = nullptr;
    core::supervisor::Supervisor* supervisor = nullptr;
    core::cycle::WakeGuard* guard = nullptr;
  };

  FirmwareJobs(const Parts& parts, core::board::RadioPins& pins, core::board::SlotLock& slots,
               lc::Client& radio, ModemUplink& server, core::settings::Settings& settings,
               interfaces::Clock& clock, interfaces::WallClock& wall);

  // Once a wake, before the loop runs: what this wake knows (the band's say, S-7's state).
  void beginWake(const WakeContext& context);
  // What this wake's jobs did, since beginWake().
  const Did& did() const { return did_; }

  // After the loop's wake, when the heartbeat was answered: the result owed for a Walter apply
  // whose trial has ended since, posted from the journal. True when one went.
  bool postOwedResult();
  // Whether a result is owed at all (a Walter apply switched and not yet reported).
  bool resultOwed(JobEntry* out = nullptr) const;
  // The last apply that ended, for `state`'s lastUpdate; false when none has.
  bool lastEnded(JobEntry* out) const;

  // ------------------------------------------------------------------ mm::IJobExtension
  int typeCount() const override { return kFirmwareJobTypes; }
  const char* type(int i) const override;
  bool run(const mm::ClaimedJob& job, mm::JobContext& ctx, mm::ResultWriter& out) override;

  // ------------------------------------------------------------------ ResultGate
  bool withhold(long long jobId) override;

  // ------------------------------------------------------------------ what the console reads
  const FirmwareJobCounters& counters() const { return counters_; }
  // The last apply's journal lines, as the result carried them.
  int journalLines() const { return line_count_; }

 private:
  struct Line {
    std::uint32_t at_epoch = 0;
    const char* step = "";
    char note[kJournalNoteBytes] = {};
  };

  bool runOffer(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx, mm::ResultWriter& out);
  bool runApply(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx, mm::ResultWriter& out);
  bool runSelfApply(const mm::ClaimedJob& job, const char* expected_hex, mm::JobContext& ctx,
                    mm::ResultWriter& out);
  bool runState(const mm::ClaimedJob& job, mm::JobContext& ctx, mm::ResultWriter& out);
  bool runPowerCycle(const mm::ClaimedJob& job, const char* payload, mm::JobContext& ctx, mm::ResultWriter& out);

  // The result shapes, into data_; the length, or 0 when it did not fit.
  std::size_t writeOfferResult(bool stored, const char* image_id, const char* sha256_hex, std::uint64_t bytes,
                               int check, const char* reason, bool resumed);
  std::size_t writeApplyResult(ApplyOutcome outcome, const char* from, const char* to, Refusal refusal);
  std::size_t writePowerCycleResult(std::uint32_t off_ms, bool radio_back, bool has_boot_id, std::uint32_t boot_id);
  std::size_t writeStateResult();
  // One line of the apply's journal, bounded to the shape.
  void line(const char* step, const char* note);
  void clearLines() { line_count_ = 0; line_chars_ = 0; }
  // A `refused` apply with its one `check` line, written and reported.
  bool refuseApply(mm::ResultWriter& out, Refusal refusal, const char* note, const char* from);

  // The lease and the token (the progress is the download listener's, in the .cpp).
  void extendLease(long long job_id, mm::JobContext& ctx);
  const char* bearer();
  void wipeBearer();
  // The journal entries this object writes.
  bool journal(const JobEntry& entry);
  std::uint32_t nowEpoch() const;

  Parts parts_;
  core::board::RadioPins& pins_;
  core::board::SlotLock& slots_;
  lc::Client& radio_;
  ModemUplink& server_;
  core::settings::Settings& settings_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;

  WakeContext context_;
  Did did_;
  FirmwareJobCounters counters_;

  // The job whose result is not to be posted this wake (ResultGate), 0 for none.
  long long left_leased_ = 0;
  // When the lease is next extended, on the clock.
  std::uint64_t extend_due_ms_ = 0;

  Line lines_[kJournalMaxEntries];
  int line_count_ = 0;
  std::size_t line_chars_ = 0;
  char data_[kDataBytes] = {};
  char body_[kDataBytes + 512] = {};  // an owed result's whole envelope
  char secret_[kBearerBytes] = {};
  char base_url_[core::settings::kMaxUrlLen + 128] = {};
};

}  // namespace agent
