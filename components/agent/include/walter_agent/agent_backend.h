// The MeshMaster agent behind the backend seam (task p4-05): `interfaces::Backend`'s one
// `exchange()` as **one `mm::AgentLoop::wake()`** of the copied library (docs/ARCHITECTURE.md
// §10 step 8 and §13; spec/SYSTEM.md §8), over the three seams tasks p4-02 to p4-04 filled -
// `LinkMesh`, `ModemUplink`, `WalterPlatform` - the identity, the §7 drain, and the firmware jobs
// of this task (walter_agent/firmware_jobs.h). Nothing of the agent protocol is written here: the
// library heartbeats, claims, runs and reports; this object is what stands between it and the
// wake cycle, and it does five things:
//
//   before the wake   **the radio known before anything is claimed** (task p8-04): a session
//                     opened if there is none and the identity read again where it has to be (a
//                     radio that got its key between two wakes), so the local door is armed and
//                     `LinkMesh::supports()` can answer - without the key no mesh type is
//                     advertised and the server routes none, and one line says which of the two
//                     this wake was. Then the uplink told the modem is up and what the deadline
//                     is, the jobs told what this wake knows (the band's say, S-7's state), §7's
//                     ring drained into the loop's own, the intent and the budget built:
//                     `WakeIntent` from the band and the interval policy's own interval (task
//                     p4-04's table), `Budget` from the deadline the wake cycle gave and the
//                     bytes one wake may post;
//   the wake          `loop.wake(intent, budget)`;
//   after it          the trial's third condition told when the heartbeat was answered
//                     (walter_agent/agent_uplink_check.h), a trial that only waited for that
//                     stepped to its end so the result a Walter apply owes can go in the same
//                     wake, that result posted from the journal, and the loop's state saved;
//   the answer        what the jobs carried out (`did`, `carried`, `outside_ms`, for S-7 and the
//                     budget), and how long to sleep at least: the server's accepted interval
//                     **only when it is longer than the policy's own** - the battery wins - and
//                     a backoff or a Retry-After the loop asked for. Nothing is ever asked for:
//                     the closed set's `asks` stay empty, because the agent's jobs are the work;
//   across the sleep  the loop's `WakeState` (76 bytes, a POD) as hex in the key-value store
//                     (`agent.state` in NVS: RTC memory has 36 bytes free, tasks/phase-3/reports/
//                     05.md, and NVS is strings of 160) and restored before the next wake -
//                     **with how long the board was away** (task p10-01), which the library takes
//                     off a wait the server asked for, because the wait is a number of seconds of
//                     real time and the sleep is most of them. `core/cycle/wake_reason.h`'s
//                     `timeAwayMs()` is the measure and the rule it is measured under; and
//                     the kept image of results and adverts a wake could not post, through the
//                     library's own fourth seam over a flash window (walter_agent/region_store.h).
//
// What crosses the seam stays what it was: a plain structure in, a plain structure out, and the
// wake cycle's rails - one update step a wake, every wake ends in sleep, the bound on a wake -
// are the wake cycle's and hold around this object (core/cycle/wake_cycle.h). The console's
// `agent wake`, `agent jobs` and `agent state` are this object too, behind
// core/console/agent_loop_probe.h.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object
// or of the two library objects it is handed (`mm::JobRunner` is tens of kilobytes and
// `mm::AgentLoop` its 64-entry ring: both are namespace-scope objects in main/, never locals).
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/AgentLoop.h>
#include <agent/IRetained.h>
#include <agent/JobRunner.h>
#include <agent/KeptImage.h>
#include <agent/Observations.h>
#include <agent/ResultBuffer.h>

#include "core/console/agent_loop_probe.h"
#include "core/cycle/duty_cycle.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "interfaces/backend.h"
#include "interfaces/clock.h"
#include "interfaces/key_value.h"
#include "interfaces/text_out.h"
#include "walter_agent/agent_uplink_check.h"
#include "walter_agent/call_log.h"
#include "walter_agent/firmware_jobs.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/modem_uplink.h"
#include "walter_agent/observations.h"
#include "walter_agent/pending_batch.h"
#include "walter_agent/walter_platform.h"

namespace agent {

// The bytes of body one wake may post - every result and every observation batch, against the
// library's one ledger (agent/UplinkMeter.h). An apply's result at its journal's bound is some
// 3 kB, a twenty-advert batch 6 kB; 32 kB is two jobs and three batches with room, and a wake
// that has more keeps the rest for the next one rather than posting past it. TO BE SET ON THE
// BENCH against the LTE plan's bytes (docs/bench.md §18).
constexpr std::size_t kWakePostBytes = 32 * 1024;

// The most an offer's download may fetch in one wake, in bytes (walter_agent/firmware_jobs.h,
// WakeContext::download_bytes): an LTE plan is metered, and a radio image is a few hundred KB, so
// a wake that cannot pay for the whole of it stops at a resumable range boundary and the next
// wake continues. A megabyte lets a radio image in one wake when the plan can pay; a unit on a
// tight plan is given less. TO BE SET ON THE BENCH against what a wake may spend
// (docs/bench.md §18).
constexpr std::uint64_t kWakeDownloadBytes = 1024u * 1024u;

// How many steps of a trial the exchange drives itself once the heartbeat has met its third
// condition: the confirmation is one step, and a few more cover a HELLO the trial was still
// waiting on. A trial that does not end in them is step 9's, as before.
constexpr int kTrialStepsAfterHeartbeat = 50;

// -- what a trial's extra heartbeat may spend (task p14-05) ------------------------------------
//
// `beat()` is *one heartbeat and nothing else*, and these two numbers are how that is said to a
// library whose `wake()` runs every step it has. A transaction cap of one is below the price of
// the cheapest job (`mm::kTransactionsPerJobAtLeast` is two), so no claim is made; a body cap of
// one byte stops every result and every observation batch, and stops neither the heartbeat nor
// the config call, whose bodies the library does not count (agent/AgentLoop.h, `Budget`). What
// the trial asks about is whether this image can reach the server, and the work of the wake is
// the next wake's - which will not be long, because the trial is about to end either way.
constexpr int kTrialBeatTransactions = 1;
constexpr std::size_t kTrialBeatPostBytes = 1;

// The key the loop's state is kept under, and the hex it is kept as: two characters a byte and a
// terminator, inside what the store takes (interfaces/key_value.h).
constexpr const char kStateKey[] = "agent_state";
constexpr std::size_t kStateHexBytes = 2 * sizeof(mm::WakeState) + 1;
static_assert(kStateHexBytes <= interfaces::kKeyValueMax, "the loop's state has to fit one value of the store");

// The scratch a kept image is built in (agent/IRetained.h): the whole of what the loop may keep,
// `ResultBuffer::kCapacityBytes` of results and the ring's adverts with their headers.
//
// **19 456 and not 20 480** (task p14-01). This allocation is the one the heap margin is measured
// against: it is the last big block taken before `app_main` and it lands in what is left of the
// internal heap, so `tools/heap_at_start.py`'s *tightest* figure is the free block there less
// this number, and 117 more bytes of `.data` or `.bss` used to move it into another heap - which
// is what v0.1.3 died of (rail R22, phase 11's task 01). Phase 13's follow-up says the next task
// that adds static storage **makes room here first**, and link 1.3 adds a 31-byte scope name and
// a little state beside it.
//
// The room is free, because the number was never the bound: 20 480 was a round figure and the
// largest image `agent/KeptImage.h` can ever write is the static assert below - 19 368 bytes at
// agent-core 0.7.3 - so a scratch of 19 456 still holds **everything the loop may keep**, with 88
// bytes over, and nothing is dropped that was not dropped before. 19 456 and not 19 368 because the allocator
// rounds a block of this size up to a multiple of 1 024 (TLSF's `mapping_search`, 16
// subdivisions of a power of two): anything from 18 433 to 19 456 takes the same block, so the
// largest of them is the one to ask for.
constexpr std::size_t kKeptScratchBytes = 19456;

// The largest image the library can write, **as the library says it itself**: the header, the
// result headers over the buffer's bytes, the ring full of adverts, and the client id of a batch
// that is still waiting for an answer (`mm::kMaxKeptImageLen`, agent/KeptImage.h).
//
// It was a sum written out here, and that is the one thing the bump to 0.7.3 nearly let past: the
// batch's 40 bytes are new in the library's figure (its task p1.13-55), and a sum of this
// repository's own would have gone on asserting 19 328 while a full image had become 19 368 -
// which is not a compile error anywhere and would have shown up as a sleep dropping the last of
// what it kept. The library made it a constant so that it moves when the library moves; this
// reads that constant, and the assert below is what a copy that grows it again stops at.
constexpr std::size_t kLargestKeptImage = mm::kMaxKeptImageLen;
static_assert(kLargestKeptImage == 19368, "agent-core's kept image changed size: see above");
static_assert(kKeptScratchBytes >= kLargestKeptImage,
              "the scratch must hold the largest image the loop can write, or a sleep drops "
              "adverts a bigger one would have kept");

struct AgentBackendCounters {
  std::uint32_t exchanges = 0;
  std::uint32_t wakes = 0;             // loop.wake() calls that ran their steps
  std::uint32_t not_run = 0;           // ... and ones that did nothing (not configured, halted, a hold)
  std::uint32_t heartbeats_ok = 0;
  std::uint32_t heartbeats_failed = 0;
  std::uint32_t jobs_run = 0;
  std::uint32_t jobs_not_run = 0;
  std::uint32_t results_kept = 0;
  std::uint32_t batches = 0;
  std::uint32_t observations_posted = 0;
  std::uint32_t observations_drained = 0;  // §7 records into the ring, over every wake
  std::uint32_t state_saved = 0;
  std::uint32_t state_restored = 0;
  std::uint32_t state_refused = 0;     // a stored state of another version or shape
  std::uint32_t kept_written = 0;      // wakes that wrote a kept image
  std::uint32_t kept_restored = 0;     // boots that read one back
  std::uint32_t trials_confirmed = 0;  // trials this object stepped to their end
  std::uint32_t longer_sleeps = 0;     // wakes where the server's interval was the longer one
  std::uint32_t holds = 0;             // wakes that carried a backoff or a Retry-After out
  // The trial's extra heartbeats (task p14-05): the ones that went out, and the ones that were
  // not sent because a wait of the library's own was still running. **Two 16-bit counters and
  // not two 32-bit ones**, in one four-byte slot: a trial is five minutes and cannot ask more
  // than a few hundred times, and this structure is a member of an object in static storage
  // whose every four bytes move the heap (tools/heap_at_start.py).
  std::uint16_t trial_beats = 0;
  std::uint16_t trial_beats_held = 0;
};

class AgentBackend final : public interfaces::Backend, public core::console::AgentLoopProbe {
 public:
  // Why a wake laid the saved wait aside instead of putting it back (tasks p14-05 and p14-06).
  // Each of them has one thing to say that an older image's failed call must not answer for.
  enum class LaidAside : std::uint8_t {
    kNone = 0,        // the wait was put back, less the time away: every ordinary wake
    kOnTrial = 1,     // this image is proving itself and the heartbeat is the proof
    kResultOwed = 2,  // the old image is back and the server is waiting for the apply's result
    // **A person pressed *Wake it up*** (task p14-06; §8's attention, reason 4). The other two
    // are boots after a restart; this one is any wake at all, because §8's attention reaches
    // this board either on the line - which wakes it - or as a reason still pending in step 4's
    // STATUS, which a timer wake finds. A person who asks for this unit is told something, and
    // a wait a failed call left behind is not an answer to them. The bound on how often is the
    // radio's and not this board's: §8 allows one new attention per reason an hour.
    kPersonAsked = 3,
  };

  // Everything the loop runs over. `kept` and `state_store` may be null: a build with neither
  // keeps nothing across a sleep, which is the library's own behaviour on a board that does not
  // sleep. `duty` is where the band and the wake's guard are; null in a test that has neither.
  struct Parts {
    LinkMesh* mesh = nullptr;
    ModemUplink* server = nullptr;
    WalterPlatform* platform = nullptr;
    Identity* identity = nullptr;
    Observations* observations = nullptr;
    mm::JobRunner* jobs = nullptr;
    mm::AgentLoop* loop = nullptr;
    FirmwareJobs* firmware = nullptr;
    AgentUplinkCheck* trial = nullptr;
    interfaces::KeyValue* state_store = nullptr;
    // The last eight failed calls (task p13-02). Null keeps none; the uplink is given it here so
    // that `call()` is the one place a failure is written down.
    CallLog* calls = nullptr;
    // The three counts of a flooded transaction, kept across a sleep (task p14-02). Null keeps
    // none, and then the heartbeat carries the library's own counts since boot - which on a board
    // that boots at every wake is what this task exists to stop sending.
    FloodCounters* floods = nullptr;
    // Since which wake a batch has been waiting for its answer (task p15-04). The batch itself is
    // the library's, in the image a sleep keeps; the wake is this board's, and null keeps none -
    // `agent obs` then says a batch is waiting and not since when.
    PendingBatch* batch = nullptr;
    mm::IRetained* kept = nullptr;
    std::uint8_t* kept_scratch = nullptr;
    std::size_t kept_scratch_len = 0;
    core::cycle::DutyCycle* duty = nullptr;
    core::update::SelfUpdate* self = nullptr;

    core::update::RadioUpdate* update = nullptr;
  };

  AgentBackend(const Parts& parts, interfaces::Clock& clock, interfaces::WallClock& wall);
  // The library holds a plain function pointer to this object's pump (task p15-06), so the owner
  // that pointer finds has to go when this object does.
  ~AgentBackend() override;

  // Once a boot, before the first wake: the runner and the loop begun over the seams, the
  // identity handed over, the extension set, the state restored, the kept image read back.
  // `pump` is what the runner's idle waits run (docs/library.md §6 step 3): the console's.
  //
  // **The pump the library is given is this object's and not the caller's** (task p15-06): it
  // writes down which job is being run before it runs the caller's. That is the one hook the
  // copied library gives a board inside a job - `IPlatform::delayMs` and the runner's own waits
  // call it - and `mm::JobRunner::openJob()` is where the job's type comes from, so a wake ended
  // inside a job leaves the job's type and its requests of the radio without a line of the
  // library changing.
  void begin(void (*pump)());

  // Where this wake is, from what the runner and the mesh say now: the job being run and how many
  // requests of the radio or the mesh it has made, or that none is. Called by the pump above at
  // every wait the library takes, and at the two edges of an exchange. Cheap and idempotent -
  // `core::cycle::WakeProgress` writes nothing unless something moved.
  void noteProgress();

  // ------------------------------------------------------------------ interfaces::Backend
  interfaces::Exchanged exchange(const interfaces::BackendKnowledge& known, std::uint64_t deadline_ms,
                                 interfaces::BackendAnswer* out) override;
  interfaces::Beat beat(std::uint64_t deadline_ms) override;
  // What this wake's claim did to the bound on it (task p16-06): the uplink's own three bytes,
  // which is where they are already (`walter_agent/modem_uplink.h`, `WakesRoom`).
  interfaces::WakesRoom roomTookOn() const override;

  // ------------------------------------------------------------------ core::console::AgentLoopProbe
  bool wake(interfaces::TextOut& out) override;
  void jobs(interfaces::TextOut& out) override;
  void state(interfaces::TextOut& out) override;
  void calls(interfaces::TextOut& out) override;

  // ------------------------------------------------------------------ what a test and the console read
  const mm::WakeReport& lastReport() const { return report_; }
  const AgentBackendCounters& counters() const { return counters_; }
  bool stateSurvived() const { return state_survived_; }
  // What `restore()` was told this boot and what it was told it about (task p10-01): how long the
  // board was away, as `core/cycle/wake_reason.h` measured it, and how much of a wait the state
  // in the store still owed before that was taken off. Both 0 when no state was restored.
  std::uint32_t timeAwayMs() const { return away_ms_; }
  std::uint32_t holdOwedBeforeSleepMs() const { return owed_before_ms_; }
  // **How much of that wait was laid aside, and why** (tasks p14-05 and p14-06). 0 when none
  // was, which is every ordinary wake.
  //
  // Worked out from the two figures above rather than kept as a third, because every four bytes
  // of this object move the heap the kept scratch is carved out of (tools/heap_at_start.py) -
  // and the two cases are not the same arithmetic:
  //
  //   a trial, a result owed   the wait is dropped **before** it is handed to the library, so
  //                            what was laid aside is the whole of what the store held;
  //   a person asked           the wait was handed over and the time away taken off it first, so
  //                            what was laid aside is what this boot still owed - the remainder,
  //                            bar the seconds the wake itself spent reaching the exchange.
  std::uint32_t holdLaidAsideMs() const {
    switch (laid_aside_) {
      case LaidAside::kNone: return 0;
      case LaidAside::kOnTrial:
      case LaidAside::kResultOwed: return owed_before_ms_;
      case LaidAside::kPersonAsked:
        return owed_before_ms_ > away_ms_ ? owed_before_ms_ - away_ms_ : 0;
    }
    return 0;
  }
  const char* holdLaidAsideWhy() const {
    switch (laid_aside_) {
      case LaidAside::kNone: return "";
      case LaidAside::kOnTrial: return "this image is on trial";
      case LaidAside::kResultOwed: return "a rollback's result is owed";
      case LaidAside::kPersonAsked: return "a person asked for this Walter";
    }
    return "";
  }
  bool begun() const { return begun_; }
  // The most an offer may download in one wake (kWakeDownloadBytes by default; a test sets a
  // small one to see a wake stop mid-download and the next resume).
  void setDownloadBytesPerWake(std::uint64_t bytes) { download_bytes_ = bytes; }

 private:
  interfaces::Exchanged run(const interfaces::BackendKnowledge& known, std::uint64_t deadline_ms,
                            interfaces::BackendAnswer* out, interfaces::TextOut* say);
  void refreshIdentity();
  // The radio known before a wake claims anything (task p8-04): a session if there is none, then
  // the identity, then the local door armed and the mesh's job types with it.
  bool knowTheRadio();
  void handOverIdentity();
  void sayRadio(bool radio_known, bool radio_up) const;
  bool saveState();
  bool loadState();
  // §8's reason 4, wherever it was read: the wait still owed is laid aside and nothing else of
  // the library's state moves (task p14-06).
  void layAsideForPerson();
  // What the library counted in this wake, added to the kept totals and written down (task
  // p14-02): called where the loop's own state is saved, which is the end of every wake this
  // object runs. The addition is the difference against what has already been taken in this boot,
  // so a wake adds its counts once (walter_agent/flood_counters.h).
  void takeFloodCounts();
  // `agent state`'s flood line: the scope and the rule in the **library's** words
  // (`JobRunner::describeFlood`) and the three **totals** where that line has the wake's counts.
  void sayFlood(interfaces::TextOut& out) const;
  // Whether this wake was answered, and when it was not, why nothing was sent (task p10-02):
  // the seam's `interfaces::Reached`, named from the library's own chain. Reads `report_`, which
  // is the wake just run.
  interfaces::Reached reachedThisWake(const interfaces::BackendKnowledge& known) const;
  void printReport(interfaces::TextOut& out) const;
  // `agent jobs`'s own two blocks about the local door (task p9-04): the door's counters, and
  // the last ten jobs by the repeater they named - which the library's report has no field for.
  void sayLocalDoor(interfaces::TextOut& out) const;
  // `agent calls`'s second block (task p15-07): what the modem said in the latest call of this
  // boot that the server **answered**, which the one that failed is read against. From RAM, and
  // from this boot only; one line when nothing has been answered yet.
  void sayAnsweredCall(interfaces::TextOut& out, const CallLog& log) const;

  // The pump the library is given (task p15-06): this writes down where the wake is and then
  // runs the caller's. A plain function and a file-static owner, because that is what the library
  // takes - the same arrangement `WalterPlatform`'s log sink uses, and a backend constructed
  // later takes it, which is what a test with two lives of the Walter wants.
  static void pumpAndNote();

  Parts p_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;
  void (*pump_)() = nullptr;
  // The job the pump last wrote down, and what the mesh's request counter stood at when it began:
  // the record carries the requests **this job** made, and `LinkMeshCounters::requests` counts
  // from the boot. The id's low 32 bits and not the whole of it: every byte of this object is
  // static storage (`tools/heap_at_start.py`), and what this is asked is *is it the same job*,
  // which two meshmaster.dk ids four thousand million apart in one wake would not disturb.
  std::uint32_t progress_job_ = 0;
  std::uint32_t progress_requests_at_ = 0;
  bool begun_ = false;
  bool state_survived_ = false;
  // Beside the two bools and not beside the two figures it belongs to: it costs nothing in the
  // padding here and four bytes there, and four bytes of .bss move the heap the 20 kB scratch
  // is carved out of (tools/heap_at_start.py, `the tightest is N bytes`).
  LaidAside laid_aside_ = LaidAside::kNone;
  std::uint32_t away_ms_ = 0;         // what restore() was told the sleep lasted
  std::uint32_t owed_before_ms_ = 0;  // ... and the hold the stored state still owed before it
  mm::WakeReport report_;
  AgentBackendCounters counters_;
  std::uint64_t download_bytes_ = kWakeDownloadBytes;
  char state_hex_[kStateHexBytes] = {};
  char last_hex_[kStateHexBytes] = {};  // what the store holds, so an unchanged state is not written again
  // `agent wake`'s own knowledge, built from what the console has.
  interfaces::BackendKnowledge console_known_;
  interfaces::BackendAnswer console_answer_;
};

}  // namespace agent
