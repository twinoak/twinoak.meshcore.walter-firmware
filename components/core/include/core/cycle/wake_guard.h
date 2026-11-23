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
// flash, a boot window and a three-minute proving period (`core/update/radio_update.h`) is
// "several wakes' worth of time", which task p3-05's own words allow a wake to be held open for.
// So `allow()` raises this wake's bound to what the machine in hand needs, and
// `core/cycle/wake_cycle.h` and the agent's apply are the callers.
//
// **Thirty minutes was read off a flash of a megabyte over a 100 kHz debug port, and the unit has
// since said what a flash really costs** (task p14-04): 14 min 44 s on 13 November and 14 min
// 51 s on 14 November, and a run that rolls back is a flash and a proof **twice** -
// `core::update::kWholeRunMs`, 38 min 40 s. At thirty minutes a rollback was cut off in its second
// flash: no result could be posted, the lease was left to lapse, and the server failed a job whose
// radio was in fact being put back. So the ceiling is three quarters of an hour, which is that run
// with the rest of a wake - the radio's question, the modem's attach, a heartbeat and a claim -
// around it. It is not an allowance anything spends by default: `arm()` is five minutes and only a
// machine that says what it needs raises it.
//
// **And then a session of the radio as a scanner** (task p16-03, `core/update/scanner_session.h`):
// the scanner image flashed and proved, a program of steps, and the golden image flashed and
// proved - two passes of that machine *and* the twenty-minute ceiling the steps are held to, which
// is 58 min 40 s and did not fit three quarters of an hour. So the ceiling was **one hour**, which
// is that session with the rest of a wake around it. Nothing else moved: an apply still asks for
// `core::update::kWholeRunMs` and an ordinary wake is still five minutes.
//
// **And an hour was 20 s more than the session it was set from** (task p18-04), which is no
// headroom at all for a task that gives a pass anything. That task gives it two things, both for
// a unit whose reset line does not reach its radio: a second boot window at every flash, after
// the look that finds the pulse reached nothing (`core::update::kStartAgainWindowMs`), and - once
// a session, before the golden image's proof - the watchdog the scanner left running
// (`kScannerWatchdogWaitMs`). A session is 60 min 35 s with them and the exchange's bound a
// minute more, so the ceiling is **sixty-five minutes**: that session with three and a half
// minutes beside it. It is still an allowance and not a spend - `arm()` is five minutes, and only
// a machine that says what it needs raises it - and a wake that overruns it is still reset with
// its work in the journal.
//
// **And then the second try at a flash that wrote nothing** (task p18-05, deliverable 9). A pass
// of the radio's update machine may now spend `core::update::kSecondTryMs` - a first try that
// wrote nothing (`kFlashWroteNothingMs`, three minutes: the scan is the only long way out before
// the first write), the pause between the two tries (`kFlashAgainPauseMs`, five seconds) and the
// boot window the radio gets to answer in where the first try had halted it - and then the flash
// itself. A pass is 23 min 05 s with it, a whole run 46 min 10 s, a session 67 min 25 s and the
// exchange's bound a minute more: **68 min 25 s**, which did not fit sixty-five. So the ceiling is
// **seventy minutes**, which is that session with a minute and a half beside it. Nothing else
// moved, and the static assertions in `core/cycle/wake_cycle.h` are what hold it to the arithmetic
// rather than to this comment.
//
// A raised bound is still a bound: a wake that overruns *it* is reset, the work is in the
// journal, and the next boot resumes it (`core/update/journal.h`). That is the whole reason the
// reset is safe - and it is why raising this number costs nothing but the worst case of a wake
// that has something unfinished in hand: `arm()` is five minutes and only a machine that says
// what it needs raises it.
constexpr std::uint32_t kRaisedBoundMax = 70u * 60 * 1000;
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

  // **A bound raised for a piece of work that ends**, and put back when it returns (task p9-04).
  // `allow()` above only ever raises, and the bound is counted from the boot's `arm()`, so a
  // person's `agent wake` - a command that returns in a minute or two - used to leave the rest
  // of the boot bounded at twenty minutes from the boot. On 23 October and again on 7 November
  // that was a bench session after which the cable came out and the drop from about 41 mA to
  // 8 mA did not come: the Walter stayed awake until the raised bound had passed, restarted, and
  // only then slept. Nothing was broken - the work was journalled, the bound was still a bound -
  // but it was invisible, and it is the battery of the repeater that pays for it
  // (tasks/phase-6/FOLLOWUPS.md).
  //
  // So: `raiseFor()` raises as `allow()` does **and answers the bound it found**, which the
  // caller hands straight back to `putBack()` when its work returns. `putBack()` is the only
  // thing in this object that lowers a bound, it lowers it no further than the ordinary one, and
  // it does nothing at all while disarmed - so a wake that reached sleep in the meantime is not
  // re-bounded by a command that was still on the stack.
  std::uint32_t raiseFor(std::uint32_t bound_ms);
  void putBack(std::uint32_t bound_ms);

  // How long this wake has been awake **now**, and 0 while disarmed. `status().awake_ms` is only
  // as new as the last `tick()`; a caller that raises the bound for a machine of its own has to
  // add what the wake has already spent to what the machine needs, because the bound is counted
  // from `arm()` and the machine is not (task p14-04, the agent's apply).
  std::uint32_t awakeMs() const;

  // **Whether anything ends this wake at all**, and what is left of the bound if it does (task
  // p16-06). `status().left_ms` says 0 for both *nothing ends this wake* and *the bound has
  // passed*, and it is only as new as the last `tick()`; the arithmetic of what a wake has room
  // for cannot be worked out from that (`core/cycle/wake_room.h`), so these two read the clock
  // now and tell the two apart. `false` is a disarmed guard, a person on the port and `stay` -
  // the states a bench session is in, and the reason none of this touches one.
  bool boundApplies() const { return status_.state == Guard::kArmed; }
  std::uint32_t leftOfBoundMs() const;

  // How long after the cable goes **now** this wake would last, in milliseconds: the later of
  // the USB grace and what is left of the bound. It is what the console tells a person who is
  // about to pull the cable, and 0 means nothing would end this wake - `stay` is on, or no wake
  // is being timed.
  std::uint32_t endsAfterCableMs() const;
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
