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
//                     05.md, and NVS is strings of 160) and restored before the next wake; and
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

#include "core/console/agent_loop_probe.h"
#include "core/cycle/duty_cycle.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "interfaces/backend.h"
#include "interfaces/clock.h"
#include "interfaces/key_value.h"
#include "interfaces/text_out.h"
#include "walter_agent/agent_uplink_check.h"
#include "walter_agent/firmware_jobs.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/modem_uplink.h"
#include "walter_agent/observations.h"
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

// The key the loop's state is kept under, and the hex it is kept as: two characters a byte and a
// terminator, inside what the store takes (interfaces/key_value.h).
constexpr const char kStateKey[] = "agent_state";
constexpr std::size_t kStateHexBytes = 2 * sizeof(mm::WakeState) + 1;
static_assert(kStateHexBytes <= interfaces::kKeyValueMax, "the loop's state has to fit one value of the store");

// The scratch a kept image is built in (agent/IRetained.h): the whole of what the loop may keep,
// `ResultBuffer::kCapacityBytes` of results and the ring's adverts with their headers.
constexpr std::size_t kKeptScratchBytes = 20480;

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
};

class AgentBackend final : public interfaces::Backend, public core::console::AgentLoopProbe {
 public:
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
    mm::IRetained* kept = nullptr;
    std::uint8_t* kept_scratch = nullptr;
    std::size_t kept_scratch_len = 0;
    core::cycle::DutyCycle* duty = nullptr;
    core::update::SelfUpdate* self = nullptr;
    core::update::RadioUpdate* update = nullptr;
  };

  AgentBackend(const Parts& parts, interfaces::Clock& clock, interfaces::WallClock& wall);

  // Once a boot, before the first wake: the runner and the loop begun over the seams, the
  // identity handed over, the extension set, the state restored, the kept image read back.
  // `pump` is what the runner's idle waits run (docs/library.md §6 step 3): the console's.
  void begin(void (*pump)());

  // ------------------------------------------------------------------ interfaces::Backend
  interfaces::Exchanged exchange(const interfaces::BackendKnowledge& known, std::uint64_t deadline_ms,
                                 interfaces::BackendAnswer* out) override;

  // ------------------------------------------------------------------ core::console::AgentLoopProbe
  bool wake(interfaces::TextOut& out) override;
  void jobs(interfaces::TextOut& out) override;
  void state(interfaces::TextOut& out) override;

  // ------------------------------------------------------------------ what a test and the console read
  const mm::WakeReport& lastReport() const { return report_; }
  const AgentBackendCounters& counters() const { return counters_; }
  bool stateSurvived() const { return state_survived_; }
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
  void printReport(interfaces::TextOut& out) const;
  // `agent jobs`'s own two blocks about the local door (task p9-04): the door's counters, and
  // the last ten jobs by the repeater they named - which the library's report has no field for.
  void sayLocalDoor(interfaces::TextOut& out) const;

  Parts p_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;
  void (*pump_)() = nullptr;
  bool begun_ = false;
  bool state_survived_ = false;
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
