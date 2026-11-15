// The three counts of a flooded transaction, kept (task p14-02).
//
// Since link 1.3 the agent library applies its rule on this board: the first flood of a
// transaction goes out in the radio's default scope and the second without it (agent/FloodScope.h;
// meshmaster.dk's D-47). Whether that second try still finds repeaters the first one misses is a
// number, and the number decides the day the try is switched off - so a companion exit node sends
// it in every heartbeat, as `flood: { scope, unscopedRetry, answeredScoped, answeredUnscoped,
// unanswered }`.
//
// **The library's counts are since boot, and this board boots at every wake.** `mm::FloodPolicy`
// counts one of the three per flooded transaction (`JobRunner::floodCounts()`), it cannot be
// seeded, and it is part of a hash-checked copy of upstream that is not changed here. Sent as
// they are, the three would read 0 / 0 / 0 on the exit node's page with a 1 now and then, which
// is worth nothing to the question they exist to answer. So the totals are this board's, beside
// the library's, and a wake **adds what the library counted in that wake** to them.
//
// **Kept as `httpFailures` is**, which is the deliverable's own words: a running total across a
// deep sleep, a restart and a power cut. The library keeps its own counters inside `mm::WakeState`,
// which goes to the settings store as hex under `agent_state` (walter_agent/agent_backend.h) -
// and that value is 152 of the 160 characters a value may be, so these three go under a key of
// their own, `agent_flood`, in the same shape and for the same reasons the four wake counters
// have one (`core/cycle/wake_counters.h`: RTC memory has no slot left and a power cut takes it).
// A key of its own is also what makes the state of an older release readable without a thought:
// a unit that wakes on this firmware with everything v0.1.6 saved keeps every counter it had,
// because nothing of what v0.1.6 wrote has changed shape, and its three totals start at 0.
//
// **The delta, and why a wake can lose its counts but never count them twice.** `take()` keeps
// what it has already added in this boot and adds only the difference, so it may be called twice
// in a wake and move the totals once. The store is the only thing that crosses a boot: a wake
// that dies between a transaction and `save()` leaves the store holding the totals as they were
// before that wake - its own counts are lost, which is at most one wake's - and the next boot
// starts with the library's counter back at 0 and with `taken_` at 0 beside it, so nothing is
// added a second time.
//
// **What it costs the flash.** One value of the store a wake, and only when a total moved - so a
// wake in which nothing flooded writes nothing here at all. An exit node whose repeaters all have
// paths never writes this value in its life.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/FloodScope.h>

#include "interfaces/key_value.h"

namespace agent {

// The key, and what is kept under it: a version byte and the three totals, little-endian - the
// shape `agent_wakes` has, read and written the same way.
constexpr const char kFloodTotalsKey[] = "agent_flood";
constexpr std::uint8_t kFloodTotalsVersion = 1;
constexpr std::size_t kFloodTotalsBytes = 1 + 3 * sizeof(std::uint32_t);
constexpr std::size_t kFloodTotalsHexBytes = 2 * kFloodTotalsBytes + 1;
static_assert(kFloodTotalsHexBytes - 1 <= interfaces::kKeyValueMax,
              "the flood totals have to fit one value of the store");

// The three the heartbeat's `flood` object carries, as totals: `mm::FloodCounts` is the library's
// own since this boot, and these are the same three since the unit was made.
struct FloodTotals {
  std::uint32_t answered_scoped = 0;    // a flooded transaction whose scoped flood was answered
  std::uint32_t answered_unscoped = 0;  // ... whose unscoped one was: the number the retry is judged by
  std::uint32_t unanswered = 0;         // ... that flooded and got nothing back, in either scope
};

// Saturating 16-bit counters, for the reason `core::cycle::WakeCounterWrites` gives: this object
// is in static storage and the margin the main task's stack is found in is counted in tens of
// bytes (tools/heap_at_start.py).
struct FloodTotalWrites {
  std::uint16_t writes = 0;   // values written to the store
  std::uint16_t refused = 0;  // a stored value of another version or shape
};

class FloodCounters {
 public:
  // `store` may be null, or not ready: a build or a test without a settings store counts for this
  // boot alone, which is what every total of this firmware does without one.
  explicit FloodCounters(interfaces::KeyValue* store);

  // Once a boot, before the first wake: the totals read back from the store.
  void begin();
  bool begun() const { return begun_; }
  // Whether stored totals were read back: false is a first boot, a store that would not open, or
  // a value of another shape.
  bool survived() const { return survived_; }

  // What the library has counted **in this boot**, added once: the difference against what this
  // object has already taken from it. Called at the end of a wake, where the loop's own state is
  // saved; a second call in the same wake adds nothing.
  void take(const mm::FloodCounts& counts);

  // The totals to the store, **only when one moved**. True when there was nothing to write or the
  // write went through.
  bool save();

  const FloodTotals& totals() const { return totals_; }
  const FloodTotalWrites& writes() const { return writes_; }

  // Everything forgotten and the key erased: the bench, and a test.
  void forget();

 private:
  void decode(const char* hex);

  interfaces::KeyValue* store_;
  FloodTotals totals_;
  // What `take()` has already added out of the library's own counters in this boot. Not a total
  // and never saved: the library's counter starts at 0 at every boot, and so does this.
  mm::FloodCounts taken_;
  bool begun_ = false;
  bool survived_ = false;
  bool dirty_ = false;
  FloodTotalWrites writes_;
};

}  // namespace agent
