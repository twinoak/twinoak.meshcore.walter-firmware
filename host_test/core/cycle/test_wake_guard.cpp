// The bound on a wake (task p3-04, core/cycle/wake_guard.h): **without a USB host, the Walter is
// never awake indefinitely.** A wake that hangs is ended by a reset, the next boot's record says
// who asked, and the wake after it reads that as a watchdog. A USB host and `stay` hold it off,
// and `stay` runs out on its own.
#include <doctest.h>

#include <string>

#include "core/cycle/wake_cycle.h"
#include "core/cycle/wake_guard.h"
#include "core/health/health.h"
#include "cycle_bench.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"

using core::cycle::Guard;
using core::cycle::kStayBoundMs;
using core::cycle::kUsbGraceMs;
using core::cycle::kWakeBoundMs;
using core::cycle::WakeGuard;

namespace {

struct GuardBench {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  fakes::FakeRestart restart;
  core::health::Health health{retained, clock};
  WakeGuard guard{clock, restart, health};

  GuardBench() {
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
  }

  // The loop turning for `ms`, a tick every hundred milliseconds as main's does.
  bool run(std::uint32_t ms) {
    for (std::uint32_t at = 0; at < ms; at += 100) {
      if (guard.tick()) return true;
      clock.advance(100);
    }
    return guard.tick();
  }
};

}  // namespace

TEST_CASE("a disarmed guard never ends anything") {
  GuardBench b;
  CHECK(b.guard.status().state == Guard::kDisarmed);
  CHECK_FALSE(b.run(kWakeBoundMs * 3));
  CHECK(b.restart.asked == 0);
  for (std::uint8_t s = 0; s <= static_cast<std::uint8_t>(Guard::kOverrun); ++s) {
    CHECK(std::string(core::cycle::describe(static_cast<Guard>(s))) != "?");
  }
}

TEST_CASE("a wake that hangs is ended by a reset, and the next boot knows who asked") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(false);
  CHECK(b.guard.status().state == Guard::kArmed);
  // Just inside the bound: nothing happens, and the guard says how long is left.
  CHECK_FALSE(b.run(kWakeBoundMs - 1000));
  CHECK(b.restart.asked == 0);
  CHECK(b.guard.status().left_ms > 0);
  CHECK(b.guard.status().awake_ms >= kWakeBoundMs - 1100);
  // Past it: the restart is asked for, once.
  CHECK(b.run(2000));
  CHECK(b.restart.asked == 1);
  CHECK(b.guard.status().state == Guard::kOverrun);
  CHECK(b.guard.status().overruns == 1);
  // And it does not ask again and again: a board that did not restart is not helped by asking
  // sixty times a minute.
  CHECK_FALSE(b.guard.tick());
  CHECK(b.restart.asked == 1);
  // The next boot's record says who asked, which is what core/cycle/wake_reason.h reads as a
  // watchdog.
  CHECK(b.health.thisBoot().requested == core::health::Restart::kNotRequested);  // this boot's
  core::health::Health next(b.retained, b.clock);
  next.begin(core::health::ResetReason::kSoftware, core::health::WakeCause::kNotFromSleep);
  CHECK(next.thisBoot().requested == core::health::Restart::kWakeOverrun);
  CHECK(std::string(core::health::name(core::health::Restart::kWakeOverrun)) != "?");
}

TEST_CASE("a USB host holds the bound off, and the grace period ends a minute after the cable") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(true);
  CHECK(b.guard.status().state == Guard::kUsbHost);
  CHECK_FALSE(b.run(kWakeBoundMs * 3));
  CHECK(b.restart.asked == 0);
  // The cable goes: still awake, for the grace period.
  b.guard.usbHostIs(false);
  CHECK(b.guard.status().state == Guard::kUsbHost);
  CHECK_FALSE(b.run(kUsbGraceMs - 1000));
  CHECK(b.restart.asked == 0);
  // After the grace the bound applies again - and the wake has long overrun, so the very next
  // tick ends it.
  b.clock.advance(2000);
  CHECK(b.guard.tick());
  CHECK(b.restart.asked == 1);
}

TEST_CASE("a host that was never there starts no grace period") {
  GuardBench b;
  b.guard.arm();
  // Every tick says "no host", which is a Walter on a mast. The bound applies from the start.
  b.guard.usbHostIs(false);
  CHECK(b.guard.status().state == Guard::kArmed);
  CHECK(b.guard.status().usb_gone_for_ms == 0);
  CHECK(b.run(kWakeBoundMs + 1000));
  CHECK(b.restart.asked == 1);
}

TEST_CASE("stay keeps it awake without a cable, and runs out on its own") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(false);
  b.guard.stayAwake(true);
  CHECK(b.guard.status().state == Guard::kStaying);
  CHECK(b.guard.status().stay_left_ms > 0);
  CHECK_FALSE(b.run(kWakeBoundMs * 4));
  CHECK(b.restart.asked == 0);
  // `stay off` puts the bound back, and the wake has overrun by now.
  b.guard.stayAwake(false);
  CHECK(b.guard.tick());
  CHECK(b.restart.asked == 1);
}

TEST_CASE("stay is bounded: a person who forgets 'stay off' loses the rest of the day") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(false);
  b.guard.stayAwake(true);
  // Twelve hours of ticking at ten a second would be half a million calls; the clock is advanced
  // instead, which is what a fake clock is for.
  b.clock.advance(kStayBoundMs - 1000);
  CHECK_FALSE(b.guard.tick());
  CHECK(b.guard.staying());
  b.clock.advance(2000);
  CHECK(b.guard.tick());
  CHECK_FALSE(b.guard.staying());
  CHECK(b.restart.asked == 1);
}

TEST_CASE("disarming is what a sleep does: the bound stops applying") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(false);
  b.clock.advance(kWakeBoundMs - 1000);
  b.guard.disarm();
  CHECK(b.guard.status().state == Guard::kDisarmed);
  CHECK_FALSE(b.run(kWakeBoundMs * 2));
  CHECK(b.restart.asked == 0);
  // Armed again, it times from now and not from the last arming.
  b.guard.arm();
  CHECK(b.guard.status().awake_ms == 0);
  CHECK_FALSE(b.run(kWakeBoundMs - 1000));
  CHECK(b.restart.asked == 0);
}

TEST_CASE("the bound is a number somebody chose, and it is longer than a wake's modem budget") {
  // core/uplink/policy.h's kWakeBudgetMs is what one wake may spend on the modem; a bound on the
  // whole wake that was shorter than that would end every wake that had to attach.
  CHECK(kWakeBoundMs > core::uplink::kWakeBudgetMs);
  CHECK(kWakeBoundMs >= 60u * 1000);
  CHECK(kWakeBoundMs <= 30u * 60 * 1000);
  CHECK(kStayBoundMs > kWakeBoundMs);
  CHECK(kUsbGraceMs >= 10u * 1000);
}

TEST_CASE("the duty cycle hands the sleeper's answer to the guard, and nothing arms it") {
  bench::CycleBench b;
  b.world.usb = true;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  // main/ does not arm it: nothing in this firmware reaches sleep by itself yet, so a bound that
  // reset the Walter would be a boot loop (tasks/phase-3/reports/04.md).
  CHECK_FALSE(b.duty().guard().armed());
  CHECK(b.duty().guard().status().state == Guard::kUsbHost);
  for (int i = 0; i < 50; ++i) {
    CHECK_FALSE(b.duty().tick());
    b.world.clock.advance(100);
  }
  CHECK(b.v().restart.asked == 0);
  CHECK(b.v().sleeper.usb_reads > 50);
  // The cable goes. The grace period starts at the tick that noticed it, not before, so the
  // next tick is still inside it.
  b.world.usb = false;
  b.v().sleeper.usb = false;
  b.duty().guard().arm();
  CHECK_FALSE(b.duty().tick());
  CHECK(b.duty().guard().status().state == Guard::kUsbHost);
  // Past the grace and past the bound: the wake is ended.
  b.world.clock.advance(kUsbGraceMs + kWakeBoundMs + 1000);
  CHECK(b.duty().tick());
  CHECK(b.v().restart.asked == 1);
}

// -- a bound raised for one command, and put back when it returns (task p9-04) -------------------

TEST_CASE("a bound raised for a command is put back when it returns, and the cable then ends the wake") {
  // 23 October and again 7 November: after a bench session of `agent wake`s the cable came out
  // and the drop from about 41 mA to 8 mA did not come, because `allow()` had raised the bound
  // to twenty minutes **from the boot** and nothing ever lowered it again
  // (tasks/phase-6/FOLLOWUPS.md, *[04] A person's `agent wake` keeps the 20-minute bound*).
  using core::cycle::kBackendBoundMs;
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(true);  // a person is at the console
  CHECK(b.guard.bound() == kWakeBoundMs);
  // Eight minutes of bench session before the command is typed, so that the ordinary bound has
  // passed already - which is a bench session's normal state, and 7 November's.
  b.clock.advance(8u * 60 * 1000);
  CHECK_FALSE(b.guard.tick());

  // The command raises the bound and is told what it found.
  const std::uint32_t was = b.guard.raiseFor(kBackendBoundMs);
  CHECK(was == kWakeBoundMs);
  CHECK(b.guard.bound() == kBackendBoundMs);
  // It takes two minutes, as a wake of the loop does, and nothing ends it: the cable is in.
  b.clock.advance(2u * 60 * 1000);
  CHECK_FALSE(b.guard.tick());
  // And returns.
  b.guard.putBack(was);
  CHECK(b.guard.bound() == kWakeBoundMs);

  // The wake has been awake far longer than the ordinary bound, so what is left of it is nothing
  // and the USB grace is the figure the console names.
  CHECK(b.guard.endsAfterCableMs() == kUsbGraceMs);
  // The cable comes out. The grace, and then the reset that ends this wake - where before the
  // Walter would have stayed awake until twenty minutes after the boot.
  b.guard.usbHostIs(false);
  CHECK(b.guard.status().state == Guard::kUsbHost);
  CHECK_FALSE(b.run(kUsbGraceMs - 1000));
  CHECK(b.restart.asked == 0);
  b.clock.advance(2000);
  CHECK(b.guard.tick());
  CHECK(b.restart.asked == 1);
}

TEST_CASE("without the putting back, the same session stays awake to the raised bound") {
  // The behaviour of 7 November, kept as a test so that the difference is a measured one and not
  // a claim: the same two minutes of work, the bound left raised, and the cable out.
  using core::cycle::kBackendBoundMs;
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(true);
  b.clock.advance(8u * 60 * 1000);
  b.guard.allow(kBackendBoundMs);
  b.clock.advance(2u * 60 * 1000);
  b.guard.usbHostIs(false);
  // Past the grace, and still awake: the raised bound has not passed.
  CHECK_FALSE(b.run(kUsbGraceMs + 1000));
  CHECK(b.restart.asked == 0);
  CHECK(b.guard.endsAfterCableMs() > kUsbGraceMs);
  // It ends, as it always did - at the raised bound, counted from the boot.
  CHECK(b.run(kBackendBoundMs));
  CHECK(b.restart.asked == 1);
}

TEST_CASE("putting back lowers no further than the ordinary bound, and does nothing while disarmed") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(false);
  CHECK(b.guard.raiseFor(core::cycle::kRaisedBoundMax) == kWakeBoundMs);
  // A bound of nothing is not a bound: the ordinary one is the floor.
  b.guard.putBack(0);
  CHECK(b.guard.bound() == kWakeBoundMs);
  b.guard.putBack(core::cycle::kRaisedBoundMax * 4);
  CHECK(b.guard.bound() == core::cycle::kRaisedBoundMax);
  // A wake that reached sleep while the command was still on the stack is not re-bounded by it.
  b.guard.disarm();
  CHECK(b.guard.bound() == kWakeBoundMs);
  b.guard.putBack(core::cycle::kRaisedBoundMax);
  CHECK(b.guard.bound() == kWakeBoundMs);
  CHECK(b.guard.status().state == Guard::kDisarmed);
  // And a disarmed guard names no time at all: nothing of this object would end that wake.
  CHECK(b.guard.endsAfterCableMs() == 0);
  CHECK(b.guard.raiseFor(core::cycle::kRaisedBoundMax) == kWakeBoundMs);
  CHECK(b.guard.bound() == kWakeBoundMs);  // allow() never arms a disarmed guard
}

TEST_CASE("the time the console names after a cable: the later of the grace and what is left") {
  GuardBench b;
  b.guard.arm();
  b.guard.usbHostIs(true);
  // Early in a wake, what is left of the bound outlasts the grace, and that is the figure.
  b.clock.advance(1000);
  CHECK(b.guard.endsAfterCableMs() == kWakeBoundMs - 1000);
  CHECK(b.guard.endsAfterCableMs() > kUsbGraceMs);
  // Inside the last minute of the bound, the grace is the longer one.
  b.clock.advance(kWakeBoundMs - kUsbGraceMs);
  CHECK(b.guard.endsAfterCableMs() == kUsbGraceMs);
  // `stay` is the one thing that answers "nothing here would end it".
  b.guard.stayAwake(true);
  CHECK(b.guard.endsAfterCableMs() == 0);
  b.guard.stayAwake(false);
  CHECK(b.guard.endsAfterCableMs() == kUsbGraceMs);
}
