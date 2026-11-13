// The last eight calls to the server that did not come back 2xx (task p13-02).
//
// On 13 November 2026 meshmaster.dk showed *httpFailures 512* for the unit and nobody could say
// which calls had failed, with what, or when. The counter is a total with no start date - the
// library's own (`mm::AgentCounters`, agent-core/include/agent/AgentLoop.h), which leaves 401 and
// 429 out of it - and the reason for each failure was one `LOG_W` line on a port nobody was
// reading. A deep sleep forgot the line; the counter outlived it and said nothing.
//
// So this keeps the failures themselves. Eight of them, newest first, each with:
//
//   which call   config, heartbeat, claim, a result, observations, progress, a lease - the seven
//                of `mm::IUplink` as `agent::ModemUplink` makes them - and a **download range**,
//                which is the eighth call this firmware makes to that same server and is not one
//                of the seven (a `twinoak.fw.offer`'s fetch, walter_agent/firmware_jobs.h).
//   what came back  the HTTP status; or, when nothing answered, **how far the uplink had got**
//                (`interfaces::HttpOutcome`: no uplink, the connection, the handshake, the
//                request, a timeout, the socket). That pair is the whole of what a fault-finder
//                asks first, and the two halves need different things done about them.
//   the wake     the wake's number as the duty cycle counts it, so a failure lines up with
//                `wake`'s list and with a journal entry.
//   when         the epoch, **when the clock was set**. A unit that never attached has none and
//                the entry says so rather than printing a 0 that reads as 1970.
//
// **401 and 429 are in this list.** The library leaves them out of `httpFailures` because they
// are not the server being unwell; the question here is *what came back*, and a run of 401s is
// the single most useful thing this list can show a person.
//
// -- where it is kept ---------------------------------------------------------------------------
//
// One value of the settings store (`interfaces::KeyValue`, NVS on the board), as hex, under
// `agent_calls` - the shape `agent_state` has used since task p4-05 and for the same reason: RTC
// memory has no slot left (tasks/phase-3/reports/05.md) and the list has to survive a **power
// cut**, which RTC memory does not. A store's value is 160 characters, so the entry is nine bytes
// packed and the whole ring is 146 characters of hex with room to spare.
//
// **The flash is not spent on it** (the task's fourth deliverable): the value is written only
// when the ring moved, which is only when a call failed. A wake in which nothing failed makes no
// write here at all - not a shorter one, none - and `save()` is what holds that true.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/http.h"
#include "interfaces/key_value.h"

namespace agent {

// Which call it was. The seven of `mm::IUplink` in the order `docs/library.md` §4 lists them, and
// the download range behind them. The numbers are stored, so they are not reordered: a value this
// build has no name for reads back as `kUnknown`.
enum class CallKind : std::uint8_t {
  kUnknown = 0,
  kConfig = 1,
  kHeartbeat = 2,
  kObservations = 3,
  kClaim = 4,
  kResult = 5,
  kLease = 6,
  kProgress = 7,
  kDownload = 8,
  kCount,
};

// One word, for the console and the report.
const char* callKindName(CallKind what);

// How many are kept. Eight, as the task asks: a wake makes at most a handful of calls, so eight
// covers the last wake whole and the one before it, which is the window a person reads a failure
// in. A ninth pushes the oldest out.
constexpr std::size_t kCallsKept = 8;

// One entry, packed, as it is stored: the kind and the outcome in one byte, then the status, the
// wake and the epoch, little-endian.
//
//   0  kind     u8 low nibble (CallKind), outcome u8 high nibble (interfaces::HttpOutcome)
//   1  status   u16  the HTTP status; 0 when nothing came back
//   3  wake     u16  the wake's number, clamped
//   5  epoch    u32  when the clock was set; 0 when it was not
constexpr std::size_t kCallEntryBytes = 9;

// The key, and the hex it is kept as: a version byte, then the entries newest first.
constexpr const char kCallsKey[] = "agent_calls";
constexpr std::uint8_t kCallsVersion = 1;
constexpr std::size_t kCallsBytes = 1 + kCallsKept * kCallEntryBytes;
constexpr std::size_t kCallsHexBytes = 2 * kCallsBytes + 1;
static_assert(kCallsHexBytes - 1 <= interfaces::kKeyValueMax,
              "the failed calls have to fit one value of the store");

struct FailedCall {
  CallKind what = CallKind::kUnknown;
  // What came back. `status` 0 means nothing did, and then `outcome` says how far it got; a
  // status means `outcome` is not read.
  std::uint16_t status = 0;
  interfaces::HttpOutcome outcome = interfaces::HttpOutcome::kOk;
  std::uint16_t wake = 0;
  std::uint32_t at_epoch = 0;
};

// The wake's number as this list keeps it. A wake count is the duty cycle's `wakes`, which starts
// again at a power cut and would take two years at the shortest interval to pass this; a number
// above it is recorded as this one rather than wrapping to a small number that reads as recent.
constexpr std::uint32_t kWakeAtMost = 0xFFFFu;

// Saturating 16-bit counters, and not 32: this object is in static storage and the main task's
// stack has to be found around every byte of it afterwards (docs/ARCHITECTURE.md §14, rail R22).
// A unit that has recorded 65 535 failed calls has said everything these four have to say.
struct CallLogCounters {
  std::uint16_t recorded = 0;    // calls put in the list
  std::uint16_t pushed_out = 0;  // ... and the oldest ones they pushed out
  std::uint16_t writes = 0;      // values written to the store
  std::uint16_t refused = 0;     // a stored value of another version or shape
};

class CallLog {
 public:
  // `store` may be null: a build or a test with no settings store keeps the list for this boot
  // and loses it at the sleep, which is what this firmware did before this task.
  explicit CallLog(interfaces::KeyValue* store);

  // Once a boot, before anything else: the list read back from the store.
  void begin();
  bool begun() const { return begun_; }
  // Whether a stored list was read back at begin(): false is a first boot, a store that would not
  // open, or a value of another shape.
  bool survived() const { return survived_; }

  // The wake's number the next records are stamped with. Called at every wake.
  void atWake(std::uint32_t wakes);

  // One call that did not come back 2xx. `status` 0 is "nothing answered", and then `outcome` is
  // how far the uplink had got. `at_epoch` 0 is a unit with no clock yet.
  void record(CallKind what, std::uint16_t status, interfaces::HttpOutcome outcome,
              std::uint32_t at_epoch);

  // The list to the store, **only when it moved**. True when there was nothing to write or the
  // write went through.
  bool save();

  std::size_t count() const { return count_; }
  // 0 is the newest. **By value**: the ring is held in the nine bytes an entry is stored as and
  // not as eight `FailedCall`s, which would be 96 bytes of static storage for 72 bytes of facts -
  // and the margin the main task's stack is found in is counted in tens of bytes
  // (docs/ARCHITECTURE.md §14, rail R22).
  FailedCall at(std::size_t index) const;

  // Everything forgotten and the key erased: the bench, and a test.
  void forget();

  const CallLogCounters& counters() const { return counters_; }

 private:
  void decode(const char* hex);
  // One entry into and out of the nine bytes it is stored as.
  static void pack(std::uint8_t* at, const FailedCall& call);
  static FailedCall unpack(const std::uint8_t* at);

  interfaces::KeyValue* store_;
  // The ring, newest first, in the form it is written in: the value's bytes without its version
  // byte. A record is a memmove of this and nine bytes written over the front, and saving it is
  // the hex of one byte and this.
  std::uint8_t items_[kCallsKept * kCallEntryBytes] = {};
  std::uint8_t count_ = 0;
  std::uint16_t wake_ = 0;
  bool begun_ = false;
  bool survived_ = false;
  bool dirty_ = false;
  CallLogCounters counters_;
};

}  // namespace agent
