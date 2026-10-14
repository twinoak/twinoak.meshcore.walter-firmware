// The backoff (task p3-01, core/uplink/failure.h and policy.h): a month of wakes with no
// coverage, the attempts counted and the seconds spent searching counted, then coverage returns
// and the next allowed attempt succeeds and the backoff empties.
//
// This is the test the battery cares about. A unit on a mast with no signal that searched at
// every wake would be flat in days; the whole of the arithmetic exists to keep it from doing
// that, and the whole of this file is the arithmetic measured rather than asserted.
#include <doctest.h>

#include <cstdint>

#include "uplink_bench.h"

using bench::UplinkUnit;
namespace upl = core::uplink;
using upl::Failure;

namespace {

// Thirty days of half-hourly wakes.
constexpr int kMonthOfWakes = 30 * 24 * 2;

}  // namespace

TEST_CASE("a month of wakes with no coverage: the attempts thin out and stop at the cap") {
  UplinkUnit unit;
  unit.modem.never_registers = true;

  for (int i = 0; i < kMonthOfWakes; ++i) unit.oneWake();

  CHECK(unit.wakes == kMonthOfWakes);
  // Every attempt costs the registration deadline of radio, so the attempts are the bill. With
  // the cap at 16 wakes and a wake every half hour that is one search every eight hours;
  // trying at every wake would have been 1440 of them.
  CHECK(unit.attempts > 10);
  CHECK(unit.attempts < kMonthOfWakes / 10);
  // It stopped growing rather than going quiet for ever.
  CHECK(unit.status().interval_wakes == upl::kBackoffCapWakes);
  CHECK(unit.status().last_failure == Failure::kNotRegistered);
  CHECK(unit.status().failures == static_cast<std::uint32_t>(unit.attempts));

  // The seconds spent searching: every attempt waits out the registration deadline, and nothing
  // else of the sequence costs radio time at all.
  const std::uint64_t per_attempt = upl::kRegistrationDeadlineMs;
  CHECK(unit.modem.searching_ms >= static_cast<std::uint64_t>(unit.attempts) * per_attempt);
  CHECK(unit.modem.searching_ms <=
        static_cast<std::uint64_t>(unit.attempts) * (per_attempt + 4u * upl::kRegistrationPollMs));
  // And what that would have been without a backoff.
  const std::uint64_t without = static_cast<std::uint64_t>(kMonthOfWakes) * per_attempt;
  CHECK(unit.modem.searching_ms < without / 10u);
}

TEST_CASE("the first failures are not punished: the next wake still tries") {
  UplinkUnit unit;
  unit.modem.never_registers = true;
  upl::Result r;
  // Wake 1 tries (nothing has failed yet), wake 2 tries (one failure is "the next wake"), and
  // only from the second failure does it begin to skip.
  REQUIRE(unit.oneWake(&r));
  CHECK(unit.status().interval_wakes == 1);
  REQUIRE(unit.oneWake(&r));
  CHECK(unit.status().interval_wakes == 2);
  CHECK_FALSE(unit.oneWake());  // skipped
  REQUIRE(unit.oneWake(&r));    // the second wake after the attempt
  CHECK(unit.status().interval_wakes == 4);
  CHECK_FALSE(unit.oneWake());
  CHECK_FALSE(unit.oneWake());
  CHECK_FALSE(unit.oneWake());
  REQUIRE(unit.oneWake(&r));
  CHECK(unit.status().interval_wakes == 8);
}

TEST_CASE("coverage returns: the next allowed attempt succeeds and the backoff empties") {
  UplinkUnit unit;
  unit.modem.never_registers = true;
  for (int i = 0; i < 40; ++i) unit.oneWake();
  REQUIRE(unit.status().failures > 4);
  const std::uint32_t grown = unit.status().interval_wakes;
  CHECK(grown > 1);

  // The tower comes back. The backoff does not know that, so the wakes until the next attempt
  // are still skipped - which is the price of not searching for nothing.
  unit.modem.never_registers = false;
  upl::Result r;
  const int waited = unit.wakeUntilTried(&r);
  REQUIRE(waited > 0);
  CHECK(waited <= static_cast<int>(grown));
  CHECK(r.up);
  // And then it is empty: the next wake may try at once.
  CHECK(unit.status().failures == 0);
  CHECK(unit.status().interval_wakes == 1);
  CHECK(unit.status().last_failure == Failure::kNone);
  unit.modem.never_registers = true;
  CHECK(unit.oneWake(&r));
}

TEST_CASE("the backoff survives a restart of the Walter and not a power cut") {
  UplinkUnit unit;
  unit.modem.never_registers = true;
  for (int i = 0; i < 10; ++i) unit.oneWake();
  const std::uint32_t failures = unit.status().failures;
  const std::uint32_t interval = unit.status().interval_wakes;
  REQUIRE(interval > 1);

  // A reset, a panic, a watchdog, a wake from deep sleep: RTC memory holds, so the backoff is
  // exactly where it was and a unit that reboots itself does not get a free search each time.
  unit.boot();
  CHECK(unit.status().failures == failures);
  CHECK(unit.status().interval_wakes == interval);
  CHECK_FALSE(unit.status().record_lost);

  // A power cut takes RTC memory with it. The count begins again - the forgiving side, and the
  // status says which happened so that a console can too.
  unit.powerCut();
  CHECK(unit.status().failures == 0);
  CHECK(unit.status().interval_wakes == 1);
  CHECK(unit.status().record_lost);
}

TEST_CASE("a changed failure starts the count again rather than inheriting the wait") {
  UplinkUnit unit;
  unit.modem.never_registers = true;
  for (int i = 0; i < 20; ++i) unit.oneWake();
  REQUIRE(unit.status().interval_wakes > 2);

  // The card falls out. That is a different failure with a different rule - "next wake; not
  // sooner" - and inheriting a sixteen-wake wait from the coverage problem would hide it.
  unit.modem.sim_state = interfaces::SimState::kAbsent;
  upl::Result r;
  REQUIRE(unit.wakeUntilTried(&r) > 0);
  CHECK(r.failure == Failure::kNoSim);
  CHECK(unit.status().failures == 1);
  CHECK(unit.status().interval_wakes == 1);
  CHECK(unit.oneWake(&r));
  CHECK(r.failure == Failure::kNoSim);
}

TEST_CASE("a person can forget the backoff; nothing automatic can") {
  UplinkUnit unit;
  unit.modem.never_registers = true;
  for (int i = 0; i < 20; ++i) unit.oneWake();
  REQUIRE_FALSE(unit.policy->mayTry());
  unit.policy->forgetBackoff();
  CHECK(unit.policy->mayTry());
  CHECK(unit.status().failures == 0);
  CHECK(unit.status().interval_wakes == 1);
}

TEST_CASE("every wake is counted, including the ones in which nothing is tried") {
  UplinkUnit unit;
  unit.modem.never_registers = true;
  for (int i = 0; i < 50; ++i) unit.oneWake();
  // The count the backoff is made of: a wake that was skipped is still a wake, or "every fourth
  // wake" would mean nothing at all.
  CHECK(unit.status().wakes == 50u);
  CHECK(unit.status().tries == static_cast<std::uint32_t>(unit.attempts));
  CHECK(unit.status().tries < unit.status().wakes);
}
