// What a wake has room for, worked out in one place (task p16-06, `core/cycle/wake_room.h`).
//
// `host_test/agent-backend/test_a_wake_outlasts_what_it_took_on.cpp` is this arithmetic in whole
// wakes with a repeater on the air. This is the arithmetic itself, case by case, including the two
// a whole wake cannot reach: a lease the ceiling cuts into the past, and a claim made before this
// firmware knew what day it was. It also holds the words - the journal's wake entry and `wake`'s
// own line - because those are what a person reads a month later.
#include <doctest.h>

#include <string>

#include "core/cycle/wake_cycle.h"
#include "core/cycle/wake_guard.h"
#include "core/cycle/wake_room.h"
#include "core/health/health.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"

using core::cycle::Cover;
using core::cycle::kRaisedBoundMax;
using core::cycle::kWakeBoundMs;
using core::cycle::WakeGuard;
using core::cycle::WakeRoom;

namespace {

// The epoch a claim is made at, in the tests below: any Unix second this firmware would accept.
constexpr std::uint32_t kNow = 1795000000u;
// The modem's command timeout, which is what one call after the jobs is given room for.
constexpr std::uint32_t kCallRoomS = core::cycle::kCallRoomMs / 1000u;

struct RoomBench {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  fakes::FakeRestart restart;
  core::health::Health health{retained, clock};
  WakeGuard guard{clock, restart, health};

  RoomBench() {
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
  }

  WakeRoom room() { return WakeRoom(&guard, clock); }
  // A wake armed and `awake_ms` into itself.
  void aWake(std::uint32_t awake_ms = 0) {
    guard.arm();
    clock.advance(awake_ms);
  }
};

}  // namespace

TEST_CASE("the room for the calls after the jobs is one for each and one for the observations") {
  CHECK(core::cycle::roomForCallsAfter(0) == core::cycle::kCallRoomMs);
  CHECK(core::cycle::roomForCallsAfter(1) == 2 * core::cycle::kCallRoomMs);
  CHECK(core::cycle::roomForCallsAfter(2) == 3 * core::cycle::kCallRoomMs);
  // A negative count is no jobs, which is what a library that answered one would mean by it.
  CHECK(core::cycle::roomForCallsAfter(-1) == core::cycle::kCallRoomMs);
}

TEST_CASE("the bound is raised to the end of the latest lease and the calls after it") {
  RoomBench b;
  b.aWake(30000);

  const Cover cover = b.room().tookOn(1, kNow + 240, kNow);

  CHECK(cover.jobs == 1);
  CHECK(cover.room_ms == 2 * core::cycle::kCallRoomMs);
  // The guard counts from the boot and the lease from now, so the thirty seconds the wake has
  // already spent are in it.
  CHECK(cover.bound_ms == 30000 + 240000 + cover.room_ms);
  CHECK(b.guard.bound() == cover.bound_ms);
  CHECK(cover.covered_s == 240 + 2 * kCallRoomS);
  // The lease fits, so it is not cut.
  CHECK_FALSE(cover.cut);
  CHECK(cover.lease_until_s == kNow + 240);
}

TEST_CASE("of two leases the later governs, and the room is for both results") {
  RoomBench b;
  b.aWake();

  const Cover cover = b.room().tookOn(2, kNow + 660, kNow);

  CHECK(cover.jobs == 2);
  CHECK(cover.room_ms == 3 * core::cycle::kCallRoomMs);
  CHECK(cover.covered_s == 660 + 3 * kCallRoomS);
  CHECK_FALSE(cover.cut);
}

TEST_CASE("a wake that claims nothing keeps its five minutes") {
  RoomBench b;
  b.aWake(30000);

  const Cover cover = b.room().tookOn(0, 0, kNow);

  CHECK(cover.jobs == 0);
  CHECK(cover.bound_ms == kWakeBoundMs);
  CHECK(b.guard.bound() == kWakeBoundMs);
  CHECK(cover.covered_s == 0);
  CHECK(cover.lease_until_s == 0);
  CHECK_FALSE(cover.cut);
}

TEST_CASE("a claim with no lease, and one made before the unit knew what day it was") {
  // Neither is anything to cover: there is no end to reach and no clock to cut against, so the
  // wake keeps the bound it had and the rail is what ends one the job outlasts.
  RoomBench with_no_lease;
  with_no_lease.aWake();
  const Cover no_lease = with_no_lease.room().tookOn(1, 0, kNow);
  CHECK(no_lease.bound_ms == kWakeBoundMs);
  CHECK(no_lease.lease_until_s == 0);
  CHECK_FALSE(no_lease.cut);

  RoomBench with_no_clock;
  with_no_clock.aWake();
  const Cover no_clock = with_no_clock.room().tookOn(1, kNow + 240, 0);
  CHECK(no_clock.bound_ms == kWakeBoundMs);
  CHECK(no_clock.lease_until_s == 0);
  CHECK_FALSE(no_clock.cut);
}

// Two hours and not one: since task p18-04 a wake may be raised to sixty-five minutes, so an
// hour-long lease is one a wake can cover whole and no longer shows the cut.
TEST_CASE("a two-hour lease is cut to where the room for the calls after it begins") {
  RoomBench b;
  b.aWake(30000);

  const Cover cover = b.room().tookOn(1, kNow + 7200, kNow);

  // The ceiling, and no further.
  CHECK(cover.bound_ms == kRaisedBoundMax);
  CHECK(b.guard.bound() == kRaisedBoundMax);
  CHECK(cover.cut);
  // What is left of the wake, less the room for the result and the observations.
  const std::uint32_t left_s = (kRaisedBoundMax - 30000) / 1000u;
  CHECK(cover.covered_s == left_s);
  CHECK(cover.lease_until_s == kNow + left_s - 2 * kCallRoomS);
  // The library's own rule then ends the job `timeout` there, with the room still to post it.
  CHECK(cover.lease_until_s < kNow + 7200);
}

TEST_CASE("a wake with no room left at all hands a lease that has already run out") {
  RoomBench b;
  b.guard.arm();
  b.guard.allow(kRaisedBoundMax);  // a session or an apply raised it, and spent it
  b.clock.advance(kRaisedBoundMax - 20000);

  const Cover cover = b.room().tookOn(1, kNow + 3600, kNow);

  // Twenty seconds left and the calls after one job want sixty, so there is nothing for the job:
  // the lease it is handed is this second, `mm::leaseLapsed` is true at once, and the library
  // reports it `timeout` at no transaction rather than putting a packet on the air for a lease
  // this wake cannot hold.
  CHECK(cover.cut);
  CHECK(cover.lease_until_s == kNow);
}

// **Whether a wake could be made to hold another piece of work at all** (task p18-02): the
// question asked before a claim, now that a wake may make several. What a claim needs is its own
// call and the calls after its jobs - a result for each and the observations - and the jobs' own
// time is not in it, because a lease the ceiling cannot cover is cut and the library ends that
// job `timeout` with the room still there to post the result (the case above).
TEST_CASE("another claim is held to what the wake could still be raised to") {
  // What a claim of two jobs needs at the least: the call that makes it, and a result for each
  // job and the observations behind it.
  const std::uint32_t needs = 5000u + core::cycle::roomForCallsAfter(2);

  SUBCASE("a fresh wake could hold it, and so could one half an hour in") {
    RoomBench b;
    b.aWake();
    CHECK(b.room().couldHold(needs));
    b.clock.advance(30u * 60 * 1000);
    CHECK(b.room().couldHold(needs));
  }

  SUBCASE("a wake within a minute of the ceiling could not") {
    RoomBench b;
    b.aWake(kRaisedBoundMax - 60000u);
    CHECK_FALSE(b.room().couldHold(needs));
    // It could still hold one call, which is why the result of the job in hand still goes: this
    // is a wake with no room for *more work*, not one with no room for anything.
    CHECK(b.room().couldHold(5000u));
  }

  SUBCASE("the ceiling exactly, and a wake past it") {
    RoomBench b;
    b.aWake(kRaisedBoundMax - needs);
    CHECK(b.room().couldHold(needs));
    b.clock.advance(1);
    CHECK_FALSE(b.room().couldHold(needs));
    // A wake already past the ceiling - the next tick's reset - holds nothing, and the
    // arithmetic does not wrap round to *plenty of room*.
    b.clock.advance(kRaisedBoundMax);
    CHECK_FALSE(b.room().couldHold(needs));
    CHECK_FALSE(b.room().couldHold(0xFFFFFFFFu));
  }

  SUBCASE("a wake nothing is timing could hold anything") {
    RoomBench b;  // disarmed: the bench, a person on the port
    CHECK(b.room().couldHold(needs));
    CHECK(b.room().couldHold(0xFFFFFFFFu));
    b.aWake();
    b.guard.usbHostIs(true);
    CHECK(b.room().couldHold(0xFFFFFFFFu));
  }
}

TEST_CASE("nothing ends a wake nobody is timing, and nothing of this applies to it") {
  RoomBench b;  // disarmed: no wake is being timed
  const WakeRoom room = b.room();
  CHECK_FALSE(room.bounded());
  CHECK(room.leftMs() == 0);

  const Cover cover = room.tookOn(1, kNow + 240, kNow);
  CHECK(cover.bound_ms == kWakeBoundMs);
  CHECK(cover.lease_until_s == 0);
  CHECK_FALSE(cover.cut);

  // A call then takes what is left of the deadline it was given, as it did before this task...
  bool by_the_wake = true;
  CHECK(room.leftForCallMs(b.clock.nowMs() + 9000, &by_the_wake) == 9000);
  CHECK_FALSE(by_the_wake);
  // ... and with no deadline either, nothing bounds it at all.
  CHECK(room.leftForCallMs(0, &by_the_wake) == core::cycle::kNoLimitMs);
}

TEST_CASE("a bench a person is on is a wake nothing ends") {
  RoomBench b;
  b.aWake();
  CHECK(b.room().bounded());
  // `stay`, and a USB host: the two things that hold the bound off (core/cycle/wake_guard.h).
  b.guard.stayAwake(true);
  CHECK_FALSE(b.room().bounded());
  b.guard.stayAwake(false);
  CHECK(b.room().bounded());
  b.guard.usbHostIs(true);
  CHECK_FALSE(b.room().bounded());
}

TEST_CASE("every call takes what is left of the sooner of the two") {
  RoomBench b;
  b.aWake(60000);  // a minute into a five-minute wake: 240 s of bound left
  const WakeRoom room = b.room();
  bool by_the_wake = false;

  // The exchange's deadline is the better part of an hour away, so the bound is the sooner.
  const std::uint64_t far = b.clock.nowMs() + 3580000;
  CHECK(room.leftForCallMs(far, &by_the_wake) == 240000);
  CHECK(by_the_wake);

  // A nearer deadline - an apply's own, for instance - governs instead.
  const std::uint64_t near_by = b.clock.nowMs() + 10000;
  CHECK(room.leftForCallMs(near_by, &by_the_wake) == 10000);
  CHECK_FALSE(by_the_wake);

  // And a bound that has passed leaves nothing: no call is begun, which is the rule the uplink
  // already has for the deadline (`walter_agent/modem_uplink.h`, kLeastCallMs).
  b.clock.advance(kWakeBoundMs);
  CHECK(room.leftForCallMs(far, &by_the_wake) == 0);
  CHECK(by_the_wake);
}

TEST_CASE("the deadline an exchange is given is a ceiling and not a promise") {
  RoomBench b;
  b.aWake(60000);
  const WakeRoom room = b.room();
  const std::uint64_t now = b.clock.nowMs();

  // What it asked for, when the wake could be raised that far.
  CHECK(room.deadlineFor(600000) == now + 600000);
  // ... and the most a wake may ever be raised to, when it could not.
  CHECK(room.deadlineFor(kRaisedBoundMax) == now + kRaisedBoundMax - 60000);
  // The exchange's own number is `kBackendBoundMs`, and since task p18-04 the ceiling is three and
  // a half minutes above it, so a wake a minute old still has the whole of it - where before that
  // task the ceiling was twenty seconds above and this was the ceiling less the minute.
  CHECK(core::cycle::kBackendBoundMs + 60000 <= kRaisedBoundMax);
  CHECK(room.deadlineFor(core::cycle::kBackendBoundMs) == now + core::cycle::kBackendBoundMs);

  // A wake nothing is timing gets what it asked for, which is what the console's `agent wake` and
  // a test that makes an exchange by hand have always had.
  b.guard.disarm();
  CHECK(b.room().deadlineFor(core::cycle::kBackendBoundMs) ==
        b.clock.nowMs() + core::cycle::kBackendBoundMs);
}

// -- the words ----------------------------------------------------------------------------------

TEST_CASE("the journal's wake entry carries the room the wake took on, and an older one does not") {
  core::cycle::WakePayload p{};
  p.wake = core::cycle::Wake::kTimer;
  p.jobs_covered = 2;
  p.bound_s = 750;
  p.calls_no_room = 1;
  std::uint8_t bytes[core::cycle::kWakePayloadBytes];
  core::cycle::encode(p, bytes);

  core::update::Entry entry{};
  entry.kind = core::update::kind::kWakeCycle;
  entry.size = core::cycle::kWakePayloadBytes;
  for (std::size_t i = 0; i < core::cycle::kWakePayloadBytes; ++i) entry.payload[i] = bytes[i];
  core::cycle::WakePayload read{};
  REQUIRE(core::cycle::decode(entry, &read));
  CHECK(read.jobs_covered == 2);
  CHECK(read.bound_s == 750);
  CHECK(read.calls_no_room == 1);

  // The entries a unit's flash already holds are still a page a person can read, and what they
  // say about the room is *nothing*, which is what they honestly know.
  entry.size = core::cycle::kWakePayloadBytesV2;
  core::cycle::WakePayload older{};
  REQUIRE(core::cycle::decode(entry, &older));
  CHECK(older.jobs_covered == 0);
  CHECK(older.bound_s == 0);
  CHECK(older.calls_no_room == 0);
  CHECK(older.reached == p.reached);  // ... and v2's own two fields still read
}
