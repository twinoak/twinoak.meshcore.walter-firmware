// The failure taxonomy as a table (task p3-01, core/uplink/failure.h), the backoff's arithmetic,
// the 3GPP power-saving timers (core/uplink/psm.h) and the window a time has to be inside before
// this firmware believes the network gave it (core/uplink/clock_window.h). All four are pure
// functions, so these are the cheapest tests in the phase and the ones everything else rests on.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "core/uplink/clock_window.h"
#include "core/uplink/failure.h"
#include "core/uplink/policy.h"
#include "core/uplink/psm.h"

namespace upl = core::uplink;
using upl::Failure;
using upl::Retry;

TEST_CASE("every failure has a row, a word and a retry rule") {
  const Failure all[] = {Failure::kNone,          Failure::kModemSilent, Failure::kNoSim,
                         Failure::kSimLocked,     Failure::kNotRegistered, Failure::kNoData};
  for (Failure f : all) {
    const upl::Row& r = upl::row(f);
    CHECK(r.failure == f);
    CHECK(std::string(r.name).size() > 2);
    CHECK(std::string(r.means).size() > 10);
    CHECK(std::string(upl::describe(f)) == r.name);
    CHECK(std::string(upl::means(f)) == r.means);
    CHECK(std::string(upl::describe(r.retry)).size() > 3);
  }
  // The task's own spelling, because the console and the report use these words.
  CHECK(std::string(upl::describe(Failure::kModemSilent)) == "modem-silent");
  CHECK(std::string(upl::describe(Failure::kNoSim)) == "no-sim");
  CHECK(std::string(upl::describe(Failure::kSimLocked)) == "sim-locked");
  CHECK(std::string(upl::describe(Failure::kNotRegistered)) == "not-registered");
  CHECK(std::string(upl::describe(Failure::kNoData)) == "no-data");
  // And the retry column of the task's table, each one.
  CHECK(upl::row(Failure::kModemSilent).retry == Retry::kAfterModemReset);
  CHECK(upl::row(Failure::kNoSim).retry == Retry::kNextWake);
  CHECK(upl::row(Failure::kSimLocked).retry == Retry::kNever);
  CHECK(upl::row(Failure::kNotRegistered).retry == Retry::kGrowing);
  CHECK(upl::row(Failure::kNoData).retry == Retry::kGrowing);
  // A value that is not in the enum reads as "none" rather than off the end of the table.
  CHECK(upl::row(static_cast<Failure>(200)).failure == Failure::kNone);
}

TEST_CASE("the backoff doubles the interval and stops at the cap") {
  // Never means never: no number of failures turns it into a wait.
  for (std::uint32_t n = 0; n < 40; ++n) CHECK(upl::intervalAfter(Retry::kNever, n) == 0);
  // The other fixed rules are "every wake", whatever the count.
  for (std::uint32_t n = 0; n < 40; ++n) {
    CHECK(upl::intervalAfter(Retry::kNextWake, n) == 1);
    CHECK(upl::intervalAfter(Retry::kAfterModemReset, n) == 1);
    CHECK(upl::intervalAfter(Retry::kNotNeeded, n) == 1);
  }
  // Growing: the next wake, then every second, fourth, eighth ... up to the cap.
  CHECK(upl::intervalAfter(Retry::kGrowing, 0) == 1);
  CHECK(upl::intervalAfter(Retry::kGrowing, 1) == 1);
  CHECK(upl::intervalAfter(Retry::kGrowing, 2) == 2);
  CHECK(upl::intervalAfter(Retry::kGrowing, 3) == 4);
  CHECK(upl::intervalAfter(Retry::kGrowing, 4) == 8);
  CHECK(upl::intervalAfter(Retry::kGrowing, 5) == 16);
  // And it never grows past the cap, however long the run of failures - which is the whole
  // point: a unit with no coverage must still look now and then.
  for (std::uint32_t n = 5; n < 1000; ++n) {
    CHECK(upl::intervalAfter(Retry::kGrowing, n) <= upl::kBackoffCapWakes);
  }
  CHECK(upl::intervalAfter(Retry::kGrowing, 999) == upl::kBackoffCapWakes);
}

TEST_CASE("the 3GPP timers: the units of TS 24.008 and back again") {
  // T3412 extended, §10.5.7.4a: bits 8-6 the unit, bits 5-1 the value.
  CHECK(upl::tauSeconds(0x06) == 60u * 60u);        // unit 000 (10 min) x 6
  CHECK(upl::tauSeconds(0x21) == 3600u);            // unit 001 (1 hour) x 1
  CHECK(upl::tauSeconds(0x42) == 2u * 36000u);      // unit 010 (10 hours) x 2
  CHECK(upl::tauSeconds(0x61) == 2u);               // unit 011 (2 s) x 1
  CHECK(upl::tauSeconds(0x82) == 60u);              // unit 100 (30 s) x 2
  CHECK(upl::tauSeconds(0xA5) == 300u);             // unit 101 (1 min) x 5
  CHECK(upl::tauSeconds(0xC1) == 1152000u);         // unit 110 (320 hours) x 1
  CHECK(upl::tauSeconds(upl::kTimerDeactivated) == 0u);  // unit 111: deactivated
  CHECK(upl::tauSeconds(0xFF) == 0u);

  // T3324, §10.5.7.3: 2 s, 1 minute, decihours, and "other values ... multiples of 1 minute".
  CHECK(upl::activeTimeSeconds(0x01) == 2u);
  CHECK(upl::activeTimeSeconds(0x2A) == 10u * 60u);
  CHECK(upl::activeTimeSeconds(0x41) == 360u);
  CHECK(upl::activeTimeSeconds(0x61) == 60u);
  CHECK(upl::activeTimeSeconds(upl::kTimerDeactivated) == 0u);

  // Asking for a duration gives an octet that means at most that duration, and the arithmetic
  // round-trips - which is what the library's own durationToTAU() gets wrong at 1.5.0
  // (core/uplink/psm.h says how).
  for (std::uint32_t want : {2u, 30u, 60u, 120u, 600u, 1800u, 3600u, 7200u, 86400u, 1000000u}) {
    std::uint32_t actual = 0;
    const std::uint8_t octet = upl::tauOctet(want, &actual);
    CHECK(actual == upl::tauSeconds(octet));
    CHECK(actual <= want);
    CHECK(actual > 0u);
  }
  // A request below everything the element can say gets the smallest it can say, not nothing.
  std::uint32_t actual = 0;
  CHECK(upl::tauSeconds(upl::tauOctet(1, &actual)) == 2u);
  CHECK(actual == 2u);
  CHECK(upl::activeTimeSeconds(upl::activeTimeOctet(1)) == 2u);
  // An hour and a minute, the two the firmware's own constants ask for.
  CHECK(upl::tauSeconds(upl::tauOctet(upl::kPsmTauSeconds)) == upl::kPsmTauSeconds);
  CHECK(upl::activeTimeSeconds(upl::activeTimeOctet(upl::kPsmActiveSeconds)) ==
        upl::kPsmActiveSeconds);
  for (std::uint32_t want : {2u, 60u, 120u, 360u, 600u, 3600u}) {
    std::uint32_t got = 0;
    const std::uint8_t octet = upl::activeTimeOctet(want, &got);
    CHECK(got == upl::activeTimeSeconds(octet));
    CHECK(got <= want);
  }
}

TEST_CASE("the timers as the modem writes them: eight characters of 0 and 1") {
  std::uint8_t octet = 0;
  CHECK(upl::parseTimerBits("00000110", &octet));
  CHECK(octet == 0x06);
  CHECK(upl::parseTimerBits("11100000", &octet));
  CHECK(octet == upl::kTimerDeactivated);
  CHECK_FALSE(upl::parseTimerBits("0000011", &octet));    // seven
  CHECK_FALSE(upl::parseTimerBits("000001100", &octet));  // nine
  CHECK_FALSE(upl::parseTimerBits("0000011x", &octet));
  CHECK_FALSE(upl::parseTimerBits("", &octet));
  CHECK_FALSE(upl::parseTimerBits(nullptr, &octet));
  char bits[9] = {};
  upl::timerBits(0x06, bits);
  CHECK(std::string(bits) == "00000110");
  upl::timerBits(0xA5, bits);
  CHECK(std::string(bits) == "10100101");
  // Round-trip over every octet there is.
  for (int i = 0; i < 256; ++i) {
    upl::timerBits(static_cast<std::uint8_t>(i), bits);
    std::uint8_t back = 0;
    REQUIRE(upl::parseTimerBits(bits, &back));
    CHECK(back == static_cast<std::uint8_t>(i));
  }
}

TEST_CASE("a time is the network's only inside the window") {
  using interfaces::TimeOrigin;
  CHECK(upl::judgeTime(false, 0) == TimeOrigin::kNone);
  CHECK(upl::judgeTime(false, upl::kEarliestEpoch) == TimeOrigin::kNone);
  // 1970 and the modem's own defaults.
  CHECK(upl::judgeTime(true, 0) == TimeOrigin::kDefault);
  CHECK(upl::judgeTime(true, 1) == TimeOrigin::kDefault);
  CHECK(upl::judgeTime(true, 1672531200u) == TimeOrigin::kDefault);  // the library's own floor
  CHECK(upl::judgeTime(true, upl::kEarliestEpoch - 1) == TimeOrigin::kDefault);
  // Inside it.
  CHECK(upl::judgeTime(true, upl::kEarliestEpoch) == TimeOrigin::kNetwork);
  CHECK(upl::judgeTime(true, upl::kEarliestEpoch + 86400u) == TimeOrigin::kNetwork);
  CHECK(upl::judgeTime(true, upl::kLatestEpoch) == TimeOrigin::kNetwork);
  // Absurdly far after.
  CHECK(upl::judgeTime(true, upl::kLatestEpoch + 1) == TimeOrigin::kDefault);
  CHECK(upl::judgeTime(true, 0xFFFFFFFFu) == TimeOrigin::kDefault);
}

TEST_CASE("every deadline and the wake's budget is a number somebody chose") {
  // Not a tautology: it is the guard on the constants of core/uplink/policy.h, which the report
  // lists as "to be set on the bench". A zero or a budget smaller than one worst-case modem call
  // would make the whole of the budget's promise empty.
  CHECK(upl::kPresentDeadlineMs > 0);
  CHECK(upl::kModemResetDeadlineMs > upl::kPresentDeadlineMs);
  CHECK(upl::kRegistrationDeadlineMs > upl::kAttachDeadlineMs);
  CHECK(upl::kRegistrationPollMs > 0);
  CHECK(upl::kRegistrationPollMs < upl::kRegistrationDeadlineMs);
  CHECK(upl::kWakeBudgetMs > upl::kRegistrationDeadlineMs);
  CHECK(upl::kWakeBudgetMs > upl::kModemCallCeilingMs);
  CHECK(upl::kDataTries > 1);
  CHECK(upl::kTimeTries > 1);
  CHECK(upl::kBackoffCapWakes > 1);
  // No PIN and no APN are written down in this firmware (tasks/README.md: secrets never in the
  // repo; an APN is not a secret but a guess at one is a unit that cannot attach).
  CHECK(std::string(upl::kSimPin).empty());
  CHECK(std::string(upl::kApn).empty());
  // Off between wakes is the default and PSM is a setting (spec/SYSTEM.md §10 item 5).
  CHECK(upl::kPowerSaving == interfaces::PowerSavingMode::kOff);
}
