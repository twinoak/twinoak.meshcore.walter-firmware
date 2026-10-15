// The bound on a wake (task p3-04): **without a USB host, the Walter is never awake
// indefinitely.**
//
// Every step of a wake is bounded already - the link's three attempts, the uplink's budget, the
// HTTPS client's deadlines, the supervisor's waits - but a bound per step is not a bound on the
// whole, and the failure this exists for is the one no step owns: a wake that keeps finding one
// more thing to do, or a step that returns and is begun again. A Walter awake on a mast draws
// tens of milliamps instead of tens of microamps, and the battery it is spending is the
// repeater's.
//
// So: a wake is armed when it begins, `tick()` is called as the loop turns, and a wake that has
// not reached sleep within `kWakeBoundMs` is **ended by reset**. The next boot's record says who
// asked (`core::health::Restart::kWakeOverrun`), `core::cycle::WakeReason` reads that as a
// watchdog - which is what it is, this firmware's own - and the interval policy lengthens the
// next sleep rather than shortening it (`core/power/interval_policy.h`, kMaxFaultDoublings). A
// reset and not a straight sleep, because a wake that has overrun is a wake in an unknown state:
// a half-finished HTTPS download, a flasher mid-page, a modem that answered half a command. A
// boot is the one way back to a state this firmware understands, and the journal is what carries
// the unfinished work across it (`core/update/journal.h`).
//
// **Two things hold it off**, and only two:
//
//   a USB host   a person is at the console; a reset in the middle of a bench session would be
//                worse than useless. The grace period after the cable goes is `kUsbGraceMs`.
//   `stay`       the console's bench switch, which is itself bounded by `kStayBoundMs`: a person
//                who forgets to type `stay off` loses nothing but the rest of the day.
//
// Nothing else does. An update does not *hold it off*: an update journals every step and is
// resumed at the next boot by design (task p2-03), which is exactly why the reset is safe. What
// an update - and the supervisor's ladder - may do is **raise this wake's bound** to what that
// machine needs, which task p3-05 added and `allow()` below is: a longer bound, still a bound.
//
// Logic: no ESP-IDF include, no heap. The restart goes through `interfaces::Restart`, so a host
// test watches it being asked for instead of dying.
#pragma once

#include <cstdint>

#include "core/health/health.h"
#include "interfaces/clock.h"
#include "interfaces/restart.h"

namespace core {
namespace cycle {

// How long a wake may last. Five minutes, and the arithmetic behind it: the uplink's own budget
// is four (`core/uplink/policy.h`, kWakeBudgetMs), a download is budgeted inside that, and the
// rest of a wake - the supervisor's question, the clock, the power reading, one update step -
// is seconds. So five minutes is "the modem's whole budget and then some", and a wake that
// wants longer than that wants something nobody designed. A PLACEHOLDER in the same sense as
// the uplink's: when the bench says what an attach really costs, both numbers move together.
constexpr std::uint32_t kWakeBoundMs = 5u * 60 * 1000;

// **The bound a wake may raise itself to**, and the two things that need it: the supervisor's
// ladder and an update. A climb waits out thirty, sixty and a hundred and twenty seconds between
// its rungs (`core/supervisor/supervisor.h`) and the state of a climb does not survive a sleep,
// so a climb either finishes inside its wake or starts again at rung 1 in the next one; and a
// flash of a megabyte over a 100 kHz debug port, a boot window and a three-minute proving period
// (`core/update/radio_update.h`) is "several wakes' worth of time", which task p3-05's own words
// allow a wake to be held open for. So `allow()` raises this wake's bound to what the machine in
// hand needs, and `core/cycle/wake_cycle.h` is the only caller.
//
// A raised bound is still a bound: a wake that overruns *it* is reset, the work is in the
// journal, and the next boot resumes it (`core/update/journal.h`). That is the whole reason the
// reset is safe.
constexpr std::uint32_t kRaisedBoundMax = 30u * 60 * 1000;
static_assert(kRaisedBoundMax > kWakeBoundMs, "a raised bound is longer than the ordinary one");

// How long `stay` keeps the Walter awake without a cable. Twelve hours: a bench day, and not a
// week. `stay off` ends it sooner.
constexpr std::uint32_t kStayBoundMs = 12u * 60 * 60 * 1000;

// How long after a USB host goes away the Walter stays awake. A minute: long enough that
// unplugging to move a cable, or a laptop that suspends for a moment, is not a sleep; short
// enough that a unit left on a bench goes back to work by itself.
constexpr std::uint32_t kUsbGraceMs = 60000;

enum class Guard : std::uint8_t {
  kDisarmed = 0,  // nothing is running a wake: the bound does not apply
  kArmed = 1,     // a wake is running and is being timed
  kUsbHost = 2,   // a USB host is on the port (or inside the grace): a person is here
  kStaying = 3,   // `stay`: the bench, bounded by kStayBoundMs
  kOverrun = 4,   // the bound passed and the restart has been asked for
};

const char* describe(Guard state);

struct GuardStatus {
  Guard state = Guard::kDisarmed;
  std::uint32_t bound_ms = kWakeBoundMs;  // what this wake's bound is, raised or not
  std::uint32_t awake_ms = 0;        // how long this wake has been awake
  std::uint32_t left_ms = 0;         // until the bound, 0 when it does not apply or has passed
  bool usb_host = false;             // a host now
  std::uint32_t usb_gone_for_ms = 0;  // 0 while one is there
  bool staying = false;
  std::uint32_t stay_left_ms = 0;
  std::uint32_t overruns = 0;        // how many times this object has ended a wake
};

class WakeGuard {
 public:
  WakeGuard(interfaces::Clock& clock, interfaces::Restart& restart, health::Health& health);

  // A wake has begun and is to be timed from now, with `bound_ms` to reach sleep in. `main/`
  // calls it once a boot (task p3-05) and the wake cycle is what reaches sleep.
  void arm(std::uint32_t bound_ms = kWakeBoundMs);

  // Raise the bound of the wake in hand to `bound_ms`, **without moving when it began**: a step
  // that has turned out to need longer than an ordinary wake says so (see kRaisedBoundMax). It
  // never lowers a bound and never arms a disarmed guard, and it is capped at kRaisedBoundMax.
  void allow(std::uint32_t bound_ms);
  std::uint32_t bound() const { return status_.bound_ms; }
  // The wake reached sleep, or a person took over: the bound stops applying.
  void disarm();
  bool armed() const { return armed_; }

  // Told what the sleeper says about the port, every tick. The grace period is counted from the
  // first tick that found no host.
  void usbHostIs(bool present);

  // The console's `stay` and `stay off`.
  void stayAwake(bool on);
  bool staying() const { return staying_; }

  // As the loop turns. Asks for the restart when the bound has passed with nothing holding it
  // off, and answers what it did: true when the restart was asked for.
  bool tick();

  const GuardStatus& status() const { return status_; }

 private:
  void refresh();

  interfaces::Clock& clock_;
  interfaces::Restart& restart_;
  health::Health& health_;
  GuardStatus status_{};
  std::uint64_t armed_at_ms_ = 0;
  std::uint32_t bound_ms_ = kWakeBoundMs;
  std::uint64_t staying_since_ms_ = 0;
  std::uint64_t usb_gone_at_ms_ = 0;
  bool armed_ = false;
  bool staying_ = false;
  bool usb_host_ = false;
  bool usb_gone_known_ = false;  // a host has been seen to go: the grace is being counted
  bool overrun_ = false;
};

}  // namespace cycle
}  // namespace core
