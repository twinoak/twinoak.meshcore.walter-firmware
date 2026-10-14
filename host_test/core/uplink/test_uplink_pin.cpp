// The SIM's PIN (task p3-01): the one failure of the taxonomy whose retry rule is *never*, and
// the only thing in this phase that can be made permanently worse by trying again. Three
// refusals block a card; opening it again needs the operator's PUK and a person at the mast.
//
// So what is tested here is a count: how many times a PIN reaches the card. The rule
// (core/uplink/policy.h, *The PIN*) is that a refused PIN is never sent a second time - a card
// can therefore lose at most one of its three tries to this firmware - and that a PIN which is
// *accepted* is sent again whenever the card asks, because a modem that was switched off between
// wakes comes back with a locked card and a unit that refused to unlock it twice would be off
// the air for ever.
//
// The other half of the rule is not in core/ at all: the library retransmits a command that
// answered `+CME ERROR` up to CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS times, so one send of a
// wrong PIN would be three refusals and a blocked card. `sdkconfig.defaults` sets that to 1 and
// says why; components/modem/include/modem/walter_uplink.h holds the build to it with a
// static_assert.
#include <doctest.h>

#include <cstdint>

#include "uplink_bench.h"

using bench::UplinkUnit;
namespace upl = core::uplink;
using upl::Failure;
using upl::Step;

namespace {

// A unit whose card wants a PIN, with `pin` set in its settings.
UplinkUnit* withPin(UplinkUnit* unit, const char* pin, std::uint32_t epoch = 1) {
  unit->modem.sim_state = interfaces::SimState::kPinRequired;
  unit->settings.pin = pin;
  unit->settings.pin_epoch = epoch;
  unit->boot();
  return unit;
}

}  // namespace

TEST_CASE("the right PIN is sent once in the wake, and the card opens") {
  UplinkUnit unit;
  withPin(&unit, "1234");
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.up);
  CHECK(unit.modem.unlocks == 1);
  CHECK(unit.status().pin_set);
  CHECK_FALSE(unit.status().pin_refused);
}

TEST_CASE("a refused PIN is never sent again") {
  UplinkUnit unit;
  withPin(&unit, "9999");  // the fake's card wants 1234
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK_FALSE(r.up);
  CHECK(r.failure == Failure::kSimLocked);
  CHECK(r.reached == Step::kSim);
  // One of the card's three tries, and only one.
  CHECK(unit.modem.unlocks == 1);
  CHECK(unit.status().pin_refused);
  CHECK(unit.status().retry == upl::Retry::kNever);

  // A month of wakes: the backoff never allows another attempt, so nothing reaches the card.
  for (int i = 0; i < 48; ++i) CHECK_FALSE(unit.oneWake());
  CHECK(unit.modem.unlocks == 1);

  // And not even a person's `lte up` sends it a second time: forgetBackoff() is about coverage,
  // not about the card, so the attempt happens and stops at the SIM without a send.
  unit.policy->forgetBackoff();
  const upl::Result forced = unit.forceWake();
  CHECK(forced.failure == Failure::kSimLocked);
  CHECK(unit.modem.unlocks == 1);
}

TEST_CASE("the refusal survives a restart of the Walter") {
  UplinkUnit unit;
  withPin(&unit, "9999");
  REQUIRE(unit.oneWake());
  REQUIRE(unit.modem.unlocks == 1);

  // A reset, or a wake from deep sleep: RTC memory is kept, so the refusal is still known.
  unit.boot();
  CHECK(unit.status().pin_refused);
  for (int i = 0; i < 8; ++i) CHECK_FALSE(unit.oneWake());
  CHECK(unit.modem.unlocks == 1);
}

TEST_CASE("a power cut forgets it, and the card loses one more try - deliberately") {
  // The lesser of the two evils available here (core/uplink/policy.h, *The PIN*): retained
  // storage does not survive a power cut, and the alternative - a refusal kept somewhere a
  // power cut cannot reach - would be a unit whose SIM can never be unlocked again after a flat
  // battery. Task p3-03's settings store in NVS is the durable answer; this test is here so
  // that the behaviour is a decision and not a surprise.
  UplinkUnit unit;
  withPin(&unit, "9999");
  REQUIRE(unit.oneWake());
  REQUIRE(unit.modem.unlocks == 1);

  unit.powerCut();
  CHECK(unit.status().record_lost);
  CHECK_FALSE(unit.status().pin_refused);
  REQUIRE(unit.oneWake());
  CHECK(unit.modem.unlocks == 2);  // one try per power cut, never more
  for (int i = 0; i < 8; ++i) CHECK_FALSE(unit.oneWake());
  CHECK(unit.modem.unlocks == 2);
}

TEST_CASE("a changed PIN is a new PIN: the refusal is forgotten and one try is allowed") {
  UplinkUnit unit;
  withPin(&unit, "9999");
  REQUIRE(unit.oneWake());
  REQUIRE(unit.status().pin_refused);
  REQUIRE_FALSE(unit.policy->mayTry());

  // What task p3-03 does when a person provisions a different PIN.
  unit.policy->pinChanged("1234", 2);
  CHECK_FALSE(unit.status().pin_refused);
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.up);
  CHECK(unit.modem.unlocks == 2);

  // And the same PIN provisioned again is not a new one: a refusal stays a refusal.
  UplinkUnit again;
  withPin(&again, "9999");
  REQUIRE(again.oneWake());
  REQUIRE(again.status().pin_refused);
  again.policy->pinChanged("9999", 1);  // the same epoch: the same PIN
  CHECK(again.status().pin_refused);
  for (int i = 0; i < 4; ++i) CHECK_FALSE(again.oneWake());
  CHECK(again.modem.unlocks == 1);
}

TEST_CASE("a card that asks for a PIN this firmware does not have is left alone for ever") {
  UplinkUnit unit;
  unit.modem.sim_state = interfaces::SimState::kPinRequired;  // settings.pin is empty
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.failure == Failure::kSimLocked);
  CHECK(unit.modem.unlocks == 0);
  CHECK_FALSE(unit.status().pin_set);
  CHECK_FALSE(unit.status().pin_refused);  // nothing was refused: nothing was sent
  for (int i = 0; i < 48; ++i) CHECK_FALSE(unit.oneWake());
  CHECK(unit.modem.unlocks == 0);
}

TEST_CASE("a PIN command that never answers costs the card nothing and is tried again") {
  UplinkUnit unit;
  withPin(&unit, "1234");
  unit.modem.unlock_answers = false;  // `AT+CPIN=` itself times out: the card said nothing
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK_FALSE(r.up);
  // Reported as no-sim - "it does not answer" - and not as sim-locked, because nothing of the
  // card's three tries was spent and the next wake may try again.
  CHECK(r.failure == Failure::kNoSim);
  CHECK(unit.status().retry == upl::Retry::kNextWake);
  CHECK_FALSE(unit.status().pin_refused);
  CHECK(unit.policy->wakesUntilNextTry() == 1);

  // The next wake does try, and when the command gets through the card opens.
  unit.modem.unlock_answers = true;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.up);
}
