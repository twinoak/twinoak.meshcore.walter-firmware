// The sequence (task p3-01, core/uplink/policy.h): the good path, and every failure of the
// taxonomy's table reached by the script that causes it. The fake modem is the other half
// (host_test/sim/fake_modem.h) and it is a modem that costs time, so these tests are also where
// "a wake does not spend more than it was given" is checked.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "uplink_bench.h"

using bench::UplinkUnit;
namespace upl = core::uplink;
using upl::Failure;
using upl::Step;
using interfaces::TimeOrigin;
using interfaces::UplinkState;

TEST_CASE("the good path: modem, SIM, radio, attach, registration, data, time") {
  UplinkUnit unit;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.up);
  CHECK(r.failure == Failure::kNone);
  CHECK(r.reached == Step::kUp);
  CHECK_FALSE(r.abandoned);
  CHECK_FALSE(r.overran);
  CHECK_FALSE(r.modem_reset);
  CHECK(unit.policy->state() == UplinkState::kUp);
  // The steps, in order, each exactly once - no reset, because nothing was silent.
  CHECK(unit.modem.presents == 1);
  CHECK(unit.modem.resets == 0);
  CHECK(unit.modem.sims == 1);
  CHECK(unit.modem.unlocks == 0);  // a ready card is never sent a PIN
  CHECK(unit.modem.pdps == 1);
  CHECK(unit.modem.attaches == 1);
  CHECK(unit.modem.op_states >= 2);  // awake-no-radio, then the radio on
  CHECK(unit.modem.data_asks >= 1);
  CHECK(unit.modem.time_asks >= 1);
  // The time, and that it is marked as the network's.
  CHECK(r.time.origin == TimeOrigin::kNetwork);
  CHECK(r.time.epoch == unit.modem.time_epoch);
  // The backoff is empty again and the status says so.
  CHECK(unit.status().ever_up);
  CHECK(unit.status().failures == 0);
  CHECK(unit.status().interval_wakes == 1);
  CHECK(unit.policy->mayTry() == false);  // this wake's attempt was just made
  CHECK(unit.status().last_failure == Failure::kNone);
}

TEST_CASE("silence: the modem is reset once, and once only") {
  UplinkUnit unit;
  unit.modem.deaf = true;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK_FALSE(r.up);
  CHECK(r.failure == Failure::kModemSilent);
  CHECK(r.reached == Step::kModem);
  CHECK(r.modem_reset);
  CHECK(unit.modem.resets == 1);
  CHECK(unit.modem.presents == 2);  // before the reset and after it
  // Nothing beyond the first step was tried.
  CHECK(unit.modem.sims == 0);
  CHECK(unit.modem.attaches == 0);
  // And the modem was taken down on the way out, whatever it was doing.
  CHECK(unit.modem.take_downs >= 1);
  // The deadlines, exactly: the first ask, the reset, the second ask.
  CHECK(r.spent_ms == upl::kPresentDeadlineMs + upl::kModemResetDeadlineMs +
                          upl::kPresentDeadlineMs);
  CHECK_FALSE(r.overran);
}

TEST_CASE("silence that a reset cures: the uplink comes up in the same wake") {
  UplinkUnit unit;
  unit.modem.deaf_until_reset = true;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.up);
  CHECK(r.modem_reset);
  CHECK(unit.modem.resets == 1);
}

TEST_CASE("no SIM: the card is absent, or says nothing") {
  SUBCASE("absent") {
    UplinkUnit unit;
    unit.modem.sim_state = interfaces::SimState::kAbsent;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNoSim);
    CHECK(r.reached == Step::kSim);
    CHECK(unit.modem.unlocks == 0);
    CHECK(unit.modem.attaches == 0);
  }
  SUBCASE("it does not answer") {
    UplinkUnit unit;
    unit.modem.sim_answers = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNoSim);
    // The ask ran to its deadline rather than coming back early with a guess.
    CHECK(unit.modem.timeouts >= 1);
    CHECK_FALSE(r.overran);
  }
}

TEST_CASE("no-sim is tried at the next wake and not sooner, and never backs off further") {
  UplinkUnit unit;
  unit.modem.sim_state = interfaces::SimState::kAbsent;
  for (int i = 0; i < 6; ++i) {
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNoSim);
    CHECK(unit.status().interval_wakes == 1);
  }
  CHECK(unit.attempts == 6);
  CHECK(unit.wakes == 6);
}

TEST_CASE("sim-locked: a card that wants a PIN this firmware does not have") {
  UplinkUnit unit;
  unit.modem.sim_state = interfaces::SimState::kPinRequired;
  // settings.pin is empty, which is the firmware as built (no PIN is written down anywhere).
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.failure == Failure::kSimLocked);
  CHECK(r.reached == Step::kSim);
  // **Nothing was sent to the card.** A guess is how a card gets blocked.
  CHECK(unit.modem.unlocks == 0);
  // And it is never tried again by itself.
  CHECK(unit.status().retry == upl::Retry::kNever);
  CHECK_FALSE(unit.policy->mayTry());
  for (int i = 0; i < 20; ++i) CHECK_FALSE(unit.oneWake());
  CHECK(unit.modem.unlocks == 0);
}

TEST_CASE("a blocked card wants a PUK, which is a person with a letter") {
  UplinkUnit unit;
  unit.modem.sim_state = interfaces::SimState::kBlocked;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  CHECK(r.failure == Failure::kSimLocked);
  CHECK(unit.modem.unlocks == 0);
}

TEST_CASE("not-registered: searching, denied, or nothing inside the deadline") {
  SUBCASE("it searches and never registers: the whole deadline is waited out") {
    UplinkUnit unit;
    unit.modem.never_registers = true;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNotRegistered);
    CHECK(r.reached == Step::kRegistration);
    CHECK_FALSE(r.abandoned);  // the budget was not the limit; the step's own deadline was
    // The wait is exact: the registration state is buffered, so this is the one step a deadline
    // really does bound (core/uplink/policy.h).
    CHECK(unit.modem.searching_ms >= upl::kRegistrationDeadlineMs);
    CHECK(unit.modem.searching_ms <= upl::kRegistrationDeadlineMs + 4u * upl::kRegistrationPollMs);
    CHECK(unit.modem.registrations > 100);  // it polled rather than slept through it
  }
  SUBCASE("denied is answered at once: waiting would not help") {
    UplinkUnit unit;
    unit.modem.denied = true;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNotRegistered);
    CHECK(r.reached == Step::kRegistration);
    // Seconds, not two minutes: the network has decided.
    CHECK(r.spent_ms < 2000);
    CHECK(unit.modem.registrations <= 2);
  }
  SUBCASE("the attach itself fails") {
    UplinkUnit unit;
    unit.modem.attachable = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNotRegistered);
    CHECK(r.reached == Step::kAttach);
  }
  SUBCASE("it registers and the network drops us again") {
    UplinkUnit unit;
    unit.modem.drops_after_registering = true;
    unit.modem.drops_after_ms = 0;  // registered and gone in the same instant
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNotRegistered);
    CHECK(r.reached == Step::kRegistration);
  }
}

TEST_CASE("no-data: registered, and the network gave us nothing to send on") {
  SUBCASE("no address, ever") {
    UplinkUnit unit;
    unit.modem.data_ever_ready = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNoData);
    CHECK(r.reached == Step::kData);
    // It asked more than once: the context is activated a moment after registration.
    CHECK(unit.modem.data_asks == static_cast<int>(upl::kDataTries));
  }
  SUBCASE("an address that is a little late is not a failure") {
    UplinkUnit unit;
    unit.modem.data_ready_after_ms = 3000;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(unit.modem.data_asks > 1);
  }
  SUBCASE("no context can be defined at all") {
    UplinkUnit unit;
    unit.modem.pdp_definable = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.failure == Failure::kNoData);
    CHECK(r.reached == Step::kContext);
    CHECK(unit.modem.attaches == 0);
  }
}

TEST_CASE("the time: 1970 refused, before the build date refused, a sane one accepted") {
  SUBCASE("no time at all is not a failure of the uplink") {
    UplinkUnit unit;
    unit.modem.time_answered = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(r.failure == Failure::kNone);
    CHECK(r.time.origin == TimeOrigin::kNone);
    CHECK(r.time.epoch == 0);
    CHECK(unit.modem.time_asks == static_cast<int>(upl::kTimeTries));
  }
  SUBCASE("1970") {
    UplinkUnit unit;
    unit.modem.time_epoch = 0;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(r.time.origin == TimeOrigin::kDefault);
    CHECK(r.time.epoch == 0);  // the epoch goes with the verdict: nothing can read 1970 out of it
  }
  SUBCASE("a day before this firmware was built") {
    UplinkUnit unit;
    unit.modem.time_epoch = upl::kEarliestEpoch - 86400u;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.time.origin == TimeOrigin::kDefault);
    CHECK(r.time.epoch == 0);
  }
  SUBCASE("absurdly far after it") {
    UplinkUnit unit;
    unit.modem.time_epoch = upl::kLatestEpoch + 86400u;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.time.origin == TimeOrigin::kDefault);
  }
  SUBCASE("a modem that has not heard NITZ yet answers 1970 twice and then the real time") {
    UplinkUnit unit;
    unit.modem.asks_before_time = 2;
    unit.modem.early_time_epoch = 0;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(r.time.origin == TimeOrigin::kNetwork);
    CHECK(r.time.epoch == unit.modem.time_epoch);
    CHECK(unit.modem.time_asks == 3);
  }
  SUBCASE("and a modem that never hears it is still up, with no time") {
    UplinkUnit unit;
    unit.modem.asks_before_time = 99;
    unit.modem.early_time_epoch = 0;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(r.time.origin == TimeOrigin::kDefault);
    CHECK(unit.modem.time_asks == static_cast<int>(upl::kTimeTries));
  }
}

TEST_CASE("the signal, the identity and what the network granted") {
  UplinkUnit unit;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  REQUIRE(r.up);

  interfaces::Signal s;
  REQUIRE(unit.policy->signal(&s));
  CHECK(s.rsrp_dbm == -92);
  CHECK(s.rsrq_tenth_db == -105);
  CHECK(s.rssi_dbm == -71);
  CHECK(std::string(s.operator_name) == "TDC");
  CHECK(s.band == 20);
  CHECK(unit.status().signal_known);

  interfaces::Identity id;
  REQUIRE(unit.policy->identity(&id));
  CHECK(std::string(id.imei) == "357999001112223");
  CHECK(std::string(id.iccid) == "8945020000000777666");
  CHECK(std::string(id.imsi) == "238010000012345");

  // Off between wakes is the default, so PSM was asked to be *off* and nothing was granted.
  CHECK(unit.modem.last_request.mode == interfaces::PowerSavingMode::kOff);
  CHECK(unit.status().power_saving.asked);
  CHECK_FALSE(unit.status().power_saving.granted);
}

TEST_CASE("PSM as a setting: the timers asked for, and the ones granted, in seconds") {
  UplinkUnit unit;
  unit.settings.power_saving = interfaces::PowerSavingMode::kPsm;
  unit.boot();
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  REQUIRE(r.up);
  // What was asked for is the firmware's two constants, encoded as 3GPP octets.
  CHECK(unit.modem.last_request.mode == interfaces::PowerSavingMode::kPsm);
  CHECK(upl::tauSeconds(unit.modem.last_request.tau_octet) == upl::kPsmTauSeconds);
  CHECK(upl::activeTimeSeconds(unit.modem.last_request.active_octet) == upl::kPsmActiveSeconds);
  // And what the network granted is reported as what it is, not as what was asked for.
  const interfaces::PowerSavingGrant& g = unit.status().power_saving;
  CHECK(g.asked);
  CHECK(g.granted);
  CHECK(upl::tauSeconds(g.tau_octet) == 3600u);
  CHECK(upl::activeTimeSeconds(g.active_octet) == 2u);
  // A network that says nothing is not a failure and is not reported as a grant.
  UplinkUnit quiet;
  quiet.settings.power_saving = interfaces::PowerSavingMode::kPsm;
  quiet.boot();
  quiet.modem.power_saving_granted = false;
  upl::Result r2;
  REQUIRE(quiet.oneWake(&r2));
  CHECK(r2.up);
  CHECK(quiet.status().power_saving.asked);
  CHECK_FALSE(quiet.status().power_saving.granted);
}

TEST_CASE("the RAT: LTE-M unless a setting says otherwise") {
  SUBCASE("already LTE-M: nothing is set and no reset is spent") {
    UplinkUnit unit;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(unit.modem.rats_read == 1);
    CHECK(unit.modem.rats_set == 0);
    CHECK(unit.modem.resets == 0);
  }
  SUBCASE("a modem left on NB-IoT is put back, and reset because a RAT change needs one") {
    UplinkUnit unit;
    unit.modem.rat_now = interfaces::Rat::kNbIot;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK(r.rat_changed);
    CHECK(unit.modem.rats_set == 1);
    CHECK(unit.modem.rat_now == interfaces::Rat::kLteM);
    CHECK(unit.modem.resets == 1);
  }
  SUBCASE("a modem that will not take it attaches anyway rather than staying off the air") {
    UplinkUnit unit;
    unit.modem.rat_now = interfaces::Rat::kNbIot;
    unit.modem.rat_settable = false;
    upl::Result r;
    REQUIRE(unit.oneWake(&r));
    CHECK(r.up);
    CHECK_FALSE(r.rat_changed);
    CHECK(unit.modem.rats_set == 1);
  }
}

TEST_CASE("take-down happens on every way out, including the failures") {
  UplinkUnit unit;
  upl::Result r;
  REQUIRE(unit.oneWake(&r));
  REQUIRE(r.up);
  const int after_success = unit.modem.take_downs;
  unit.policy->takeDown();
  CHECK(unit.modem.take_downs == after_success + 1);
  CHECK(unit.policy->state() == UplinkState::kDown);

  unit.modem.never_registers = true;
  REQUIRE(unit.oneWake(&r));
  CHECK_FALSE(r.up);
  CHECK(unit.modem.take_downs > after_success + 1);
  CHECK(unit.policy->state() == UplinkState::kDown);
}
