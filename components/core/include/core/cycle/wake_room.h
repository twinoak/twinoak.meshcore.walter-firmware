// **What a wake has room for, in one place** (task p16-06).
//
// In the night of 17 November 2026 this unit took the hourly `status` job of its own repeater at
// 01:29:53 UTC and was next heard at 01:34:56 as a new boot; the same at 02:05:24 and 02:10:28.
// The job was leased for four minutes, was taken at both wakes and answered at neither, and
// meshmaster.dk's page of job 32763 says the rest: *taken twice by Risskov LTE, and no answer came
// before it expired*. Nothing was measured for over two hours, because each reset was read as a
// watchdog and followed by half an hour of sleep.
//
// The arithmetic that let it happen was two numbers that did not know about each other:
//
//   the exchange   step 8 gives `exchange()` a deadline of `kBackendBoundMs` - the longest machine
//                  it may run inside itself and a minute - because *the agent's jobs are minutes*.
//                  Every call to the server takes what is left of that deadline as its own
//                  (`walter_agent/modem_uplink.h`), and the library is given it as its wake's
//                  budget.
//   the wake       the bound on the wake is `kWakeBoundMs`, five minutes, and a wake that has not
//                  reached sleep by then is **ended by reset** (`core/cycle/wake_guard.h`). Step 8
//                  raised it only *after* the exchange had returned, and only when an update
//                  machine had run inside it.
//
// So an exchange in a wake of the cycle's own was promised the better part of an hour and reset at
// five minutes. A job leased for four fits only when everything else in the wake takes under one:
// the radio's question, the modem's attach, the heartbeat, the claim, a result's call, and an
// observations call that stalls for the modem's thirty-second command timeout at nearly every
// wake.
//
// This is the one place the two are worked out, and the rule is:
//
//   * **the exchange's deadline is a ceiling, not a promise.** It is what it asked for, or the
//     most a wake may ever be raised to (`kRaisedBoundMax`) if that is sooner. The library's
//     budget comes off it, so a result a long job made is still posted rather than kept.
//   * **the bound is what actually ends a call.** Every call takes what is left of the *sooner*
//     of the two, so a call is bounded by the wake it is in and the wake is never reset in the
//     middle of one.
//   * **a claim raises the bound to cover what it took on**: the end of the latest lease among the
//     jobs, and the calls that come after them - a result for each job and the observations, each
//     at the modem's command timeout. Never past `kRaisedBoundMax`.
//   * **where that ceiling cuts the cover short of a lease, the lease is cut instead**, to where
//     the room for those calls begins. The copied library then ends the job `timeout` by its own
//     rule (`agent/JobRunner.cpp`, `leaseLapsed`) and the result is posted - which is what the
//     server is owed and what the night of 17 November never got.
//
// A raised bound is still a bound, and the rail is untouched: a wake that hangs past it is reset,
// the journal carries the unfinished work, and the next boot takes it up.
//
// Logic: no ESP-IDF include, no heap, no state of its own. It is a value built on the stack from
// the guard and the clock wherever the arithmetic is needed - the wake cycle's step 8, and the
// uplink's call and claim - because a member of an object that lives for the whole boot costs
// bytes of the heap the agent's scratch is carved out of (tools/heap_at_start.py).
#pragma once

#include <cstdint>

#include "core/cycle/wake_guard.h"
#include "core/uplink/policy.h"
#include "interfaces/clock.h"

namespace core {
namespace cycle {

// What one call after the jobs may take: **the modem's command timeout** (`core/uplink/policy.h`,
// `kModemCallCeilingMs`). Not the deadline a call is given - that is what is left of the wake -
// but the longest one blocking call of the modem library can take whatever it was given, which is
// the figure a *reservation* has to be made at: the 30 s a stalled observations post costs is the
// shape this unit has.
constexpr std::uint32_t kCallRoomMs = uplink::kModemCallCeilingMs;

// The room the calls after a claim's jobs need: a result for each job, and the observations. The
// observations go at every wake and are the call this unit has actually seen stall.
constexpr std::uint32_t roomForCallsAfter(int jobs) {
  const std::uint32_t n = jobs > 0 ? static_cast<std::uint32_t>(jobs) : 0u;
  return (n + 1u) * kCallRoomMs;
}

// *No limit at all*: what `leftForCallMs` answers when neither a deadline nor a bound applies,
// which is the console's bench and a test that makes an exchange by hand.
constexpr std::uint32_t kNoLimitMs = 0xFFFFFFFFu;

// What a claim did to the room of the wake it was made in.
struct Cover {
  std::uint8_t jobs = 0;           // how many jobs were taken on
  std::uint32_t bound_ms = kWakeBoundMs;  // the wake's bound afterwards, from the boot
  std::uint32_t covered_s = 0;     // how far past the claim that bound reaches
  std::uint32_t room_ms = 0;       // what of it is kept for the calls after the jobs
  // Where a lease may end at the latest, in Unix seconds, and 0 for *no ceiling*: a wake nothing
  // ends, or a claim made before this firmware knew what day it was. A job leased past this is
  // handed this instead, so that the library ends it `timeout` while there is still room to post
  // the result.
  std::uint32_t lease_until_s = 0;
  bool cut = false;                // a lease was longer than the wake could be made to cover
};

class WakeRoom {
 public:
  // The guard by pointer, because the caller that has none - a build with no duty cycle, which is
  // every test that makes an exchange by hand - then has nothing bounding its wake, and that is
  // an answer this object can give rather than a branch every caller has to write.
  WakeRoom(WakeGuard* guard, interfaces::Clock& clock) : guard_(guard), clock_(clock) {}

  // **The deadline a piece of work inside this wake is given**: `want_ms` from now, or the most
  // this wake could ever be raised to if that is sooner. A ceiling: what ends a call is
  // `leftMs()`, the bound as it stands.
  std::uint64_t deadlineFor(std::uint32_t want_ms) const;

  // Whether anything ends this wake at all, and what is left of the bound if it does. `false` is
  // a disarmed guard, a person on the port, `stay`: the bench, where none of this applies.
  bool bounded() const { return guard_ != nullptr && guard_->boundApplies(); }
  std::uint32_t leftMs() const { return guard_ == nullptr ? 0 : guard_->leftOfBoundMs(); }

  // What is left of the **sooner** of the two for a call that is about to be made: `deadline_ms`
  // is what the exchange was given (0 is none), the answer is 0 when the bound has no room left
  // at all, and `kNoLimitMs` is *neither applies*. `bounded_by_the_wake` says which of the two
  // the figure came from, for the line a call that is not begun writes.
  std::uint32_t leftForCallMs(std::uint64_t deadline_ms, bool* bounded_by_the_wake) const;

  // **Could this wake be made to hold another piece of work at all?** (task p18-02) The most a
  // wake may ever be raised to (`kRaisedBoundMax`), against where this one stands and what the
  // work would need. A wake nothing is timing could hold anything.
  //
  // It is asked **before** a claim and not after: a wake may make several claims now, and one
  // made where the bound cannot be raised far enough to hold a job's lease and the calls after it
  // would take on work this wake cannot finish - a lease cut to this second, a job reported
  // `timeout` at no transaction, a job the server leased for nothing. The answer to that is to
  // ask for no more work, which is *no room* and not a failure.
  bool couldHold(std::uint32_t want_ms) const;

  // **The wake has taken jobs on.** Raises the bound to the end of the latest of their leases and
  // the room for the calls after them, never past `kRaisedBoundMax`, and answers what that came
  // to. `latest_lease_s` and `now_epoch` are Unix seconds, either of them 0 meaning *no lease to
  // cover* - and then nothing is raised and nothing is cut, because there is nothing to cover and
  // no clock to cut against. Raises only: `WakeGuard::allow()` never lowers a bound, so an apply,
  // a session, a climb and the console's `agent wake` keep theirs.
  Cover tookOn(int jobs, std::uint32_t latest_lease_s, std::uint32_t now_epoch) const;

 private:
  WakeGuard* guard_;
  interfaces::Clock& clock_;
};

}  // namespace cycle
}  // namespace core
