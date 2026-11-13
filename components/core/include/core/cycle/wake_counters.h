// What woke this unit, counted and kept (task p13-02).
//
// On 13 November 2026 a wake request reached the unit's radio, the radio answered *ok*, and the
// unit did not check in. From meshmaster.dk there was no way to tell apart **the line never
// moved**, **the line moved and nothing valid was on it** and **the unit woke and sent
// nothing** - and those three are a radio fault, a wiring fault and a firmware fault. The wake
// record (core/cycle/wake_reason.h) knows all three and holds ten wakes of them in RTC memory,
// which a power cut takes; nothing crossed to the server at all.
//
// So four running totals, beside the ones a heartbeat already carries:
//
//   timer      the check-in: the sleep timer came round.
//   radio      the radio's line, with a valid EV_ATTENTION inside §8's window - a wake somebody
//              asked for.
//   spurious   the radio's line, and nothing valid inside it.
//   silent     a wake that **sent nothing**, whatever woke it.
//
// A reason that is none of the first three - a person's cable, a power-on, a reset, a crash, a
// watchdog - moves none of them: these count the three a server cannot see, not every wake there
// is, and the heartbeat's own record carries the total of those.
//
// **Kept as `httpFailures` is**: a running total, across a deep sleep and a power cut. The
// library keeps its own inside `mm::WakeState`, which goes to the settings store as hex under
// `agent_state` (walter_agent/agent_backend.h); that value is 152 of the 160 characters a value
// may be, so these go under a key of their own, `agent_wakes`, in the same shape. RTC memory was
// not an option for either: it has no slot left (tasks/phase-3/reports/05.md) and a power cut
// takes it, and *how many times has this unit woken on its radio's line* is exactly the question
// a person asks after the power came back.
//
// **It is core/'s and not agent/'s** although only a heartbeat reads it: the wake cycle is what
// knows why a wake happened and whether it sent anything, and it cannot name components/agent/
// (agent/ is built on core/, not the other way round). What agent/ takes is the four numbers.
//
// **What it costs the flash.** One value of the store a wake, and only when a total moved - so a
// wake that is none of the four writes nothing here at all. An ordinary timer wake moves `timer`
// and costs one write of 34 characters beside the one `agent_state` already made.
//
// Logic: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/cycle/wake_reason.h"
#include "interfaces/backend.h"
#include "interfaces/key_value.h"

namespace core {
namespace cycle {

// The key, and what is kept under it: a version byte and the four totals, little-endian.
constexpr const char kWakeCountersKey[] = "agent_wakes";
constexpr std::uint8_t kWakeCountersVersion = 1;
constexpr std::size_t kWakeCountersBytes = 1 + 4 * sizeof(std::uint32_t);
constexpr std::size_t kWakeCountersHexBytes = 2 * kWakeCountersBytes + 1;
static_assert(kWakeCountersHexBytes - 1 <= interfaces::kKeyValueMax,
              "the wake counters have to fit one value of the store");

// How many `errors` entries these four take in a heartbeat. Named here so that the array can be
// sized from it rather than from a number somebody has to keep in step.
constexpr int kWakeCounterCount = 4;

struct WakeTotals {
  std::uint32_t timer = 0;
  std::uint32_t radio = 0;
  std::uint32_t spurious = 0;
  std::uint32_t silent = 0;
};

// Saturating 16-bit counters, for the reason `agent::CallLogCounters` gives: this object is in
// static storage and the margin the main task's stack is found in is counted in tens of bytes.
struct WakeCounterWrites {
  std::uint16_t writes = 0;   // values written to the store
  std::uint16_t refused = 0;  // a stored value of another version or shape
};

class WakeCounters {
 public:
  // `store` may be null: a build or a test with no settings store counts for this boot alone,
  // which is what this firmware did before this task.
  explicit WakeCounters(interfaces::KeyValue* store);

  // Once a boot, before anything else: the totals read back from the store.
  void begin();
  bool begun() const { return begun_; }
  // Whether stored totals were read back: false is a first boot, a store that would not open, or
  // a value of another shape.
  bool survived() const { return survived_; }

  // One wake, counted by what woke it. Called once a wake, by the step that worked it out; a
  // second call in the same wake counts nothing.
  void countWake(Wake wake);

  // ... and whether it sent anything. `reached` is what the wake settled on
  // (core/cycle/wake_cycle.h), and a wake is **silent** when that reason is one of the *nothing
  // was sent* ones - a wait still owed, the uplink's backoff, no uplink, nothing configured, a
  // halt, nothing to send. *The backend answered* is not silent, and neither is *it did not say*,
  // which since task p13-02 means one thing only: the exchange failed, which is a wake that did
  // try. It is the same line the record's `nothing sent: ` clause is drawn at. Called at the end
  // of a wake, and a second call in the same wake counts nothing.
  void countSent(interfaces::Reached reached);

  // The totals to the store, **only when one moved**. True when there was nothing to write or
  // the write went through.
  bool save();

  const WakeTotals& totals() const { return totals_; }
  const WakeCounterWrites& writes() const { return writes_; }

  // Everything forgotten and the key erased: the bench, and a test.
  void forget();

 private:
  void decode(const char* hex);

  interfaces::KeyValue* store_;
  WakeTotals totals_;
  bool begun_ = false;
  bool survived_ = false;
  bool dirty_ = false;
  bool wake_counted_ = false;
  bool sent_counted_ = false;
  WakeCounterWrites writes_;
};

}  // namespace cycle
}  // namespace core
