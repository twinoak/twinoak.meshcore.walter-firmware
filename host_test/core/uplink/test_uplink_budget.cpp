// The deadlines and the wake's budget (task p3-01, core/uplink/policy.h). Two promises:
//
//   every call returns by its deadline, against a modem that never answers;
//   a bring-up that would overrun the wake's budget is abandoned, cleanly, with the modem down.
//
// The fake modem is what makes the first one checkable: a call that cannot finish advances the
// clock *to* its deadline and fails there, so a policy that asked for more than it was allowed
// would show up as an overrun (host_test/sim/fake_modem.h).
#include <doctest.h>

#include <cstdint>

#include "uplink_bench.h"

using bench::UplinkUnit;
namespace upl = core::uplink;
using upl::Failure;
using upl::Step;

TEST_CASE("against a modem that never answers, every call comes back by its deadline") {
  UplinkUnit unit;
  unit.modem.deaf = true;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK_FALSE(r.up);
  CHECK_FALSE(r.overran);
  CHECK(unit.modem.overruns == 0);
  // Exactly the three deadlines the sequence is allowed to spend on a silent modem: the first
  // ask, the one reset, the ask after it. Not a millisecond more.
  CHECK(r.spent_ms == upl::kPresentDeadlineMs + upl::kModemResetDeadlineMs +
                          upl::kPresentDeadlineMs);
  CHECK(unit.modem.timeouts == 3);
}

TEST_CASE("a deaf SIM, a deaf attach and a deaf everything each stop at their own deadline") {
  // Each one is a call that runs to its deadline and no further, and each is reported as the
  // failure of the step it belongs to.
  SUBCASE("the SIM") {
    UplinkUnit unit;
    unit.modem.sim_answers = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNoSim);
    CHECK(r.reached == Step::kSim);
    CHECK_FALSE(r.overran);
    CHECK(r.spent_ms <= upl::kPresentDeadlineMs + upl::kOpStateDeadlineMs +
                            upl::kRatDeadlineMs * 2 + upl::kSimDeadlineMs + 1000);
  }
  SUBCASE("the attach") {
    UplinkUnit unit;
    unit.modem.attachable = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNotRegistered);
    CHECK(r.reached == Step::kAttach);
    CHECK_FALSE(r.overran);
  }
}

TEST_CASE("the wake's budget is never exceeded, whatever it is") {
  // A modem that answers but a network that takes a minute: the sequence wants more than most
  // of these budgets allow, and must stop inside every one of them.
  for (std::uint32_t budget : {1u, 100u, 500u, 1000u, 5000u, 15000u, 30000u, 60000u, 90000u,
                               120000u, 180000u, 240000u}) {
    UplinkUnit unit;
    unit.modem.registers_after_ms = 60000;
    upl::Result r;
    REQUIRE(unit.oneWake(&r, budget));
    CHECK(r.spent_ms <= budget);
    // Whether it got up or not, the modem is not left transmitting.
    if (!r.up) {
      CHECK(unit.modem.take_downs >= 1);
      CHECK(unit.policy->state() == interfaces::UplinkState::kDown);
    }
    CHECK_FALSE(r.overran);
  }
}

TEST_CASE("a bring-up that would overrun is abandoned, cleanly, and says so") {
  UplinkUnit unit;
  unit.modem.registers_after_ms = 60000;  // a minute to register
  upl::Result r;
  REQUIRE(unit.oneWake(&r, 20000));       // twenty seconds to spend
  CHECK_FALSE(r.up);
  CHECK(r.abandoned);
  CHECK(r.failure == Failure::kNotRegistered);
  CHECK(r.reached == Step::kRegistration);
  CHECK(r.spent_ms <= 20000);
  CHECK(unit.modem.take_downs >= 1);
  CHECK(unit.policy->state() == interfaces::UplinkState::kDown);
  // Abandoning is not a different failure: the backoff treats it as the not-registered it was.
  CHECK(unit.status().last_abandoned);
  CHECK(unit.status().failures == 1);
}

TEST_CASE("a budget too small to begin anything stops at the first step") {
  UplinkUnit unit;
  upl::Result r;
  REQUIRE(unit.oneWake(&r, 1));
  CHECK_FALSE(r.up);
  CHECK(r.spent_ms <= 1);
  CHECK(r.abandoned);
  CHECK(r.reached == Step::kModem);
  CHECK(unit.modem.presents == 0);  // not even the first `AT` was begun
}

TEST_CASE("the questions after the bring-up spend from the same budget") {
  UplinkUnit unit;
  upl::Result r;
  // Twenty seconds: enough that the registration wait can be begun at all (it asks for room for
  // a tenth of its own two minutes before it starts), and far less than the whole budget.
  REQUIRE(unit.oneWake(&r, 20000));
  REQUIRE(r.up);
  const std::uint32_t left = unit.policy->budgetLeftMs();
  CHECK(left > 0);
  CHECK(left < 20000);
  interfaces::Signal s;
  CHECK(unit.policy->signal(&s));
  CHECK(unit.policy->budgetLeftMs() < left);

  // And when there is nothing left, they refuse rather than overrun it.
  unit.clock.advance(left + 1);
  CHECK_FALSE(unit.policy->signal(&s));
  interfaces::Identity id;
  CHECK_FALSE(unit.policy->identity(&id));
  interfaces::NetworkTime when;
  CHECK_FALSE(unit.policy->networkTime(&when));
  CHECK(unit.policy->budgetLeftMs() == 0);
}
