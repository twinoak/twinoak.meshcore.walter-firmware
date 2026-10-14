// Privacy (task p3-01, *Privacy*; core/uplink/policy.h): the IMEI, the ICCID and the IMSI
// identify a subscription. `lte id` prints them because a person asked; **nothing else this
// firmware says or sends may contain them**.
//
// The task asks for a test that greps the log sink. This firmware has one sink - the console's
// interfaces::TextOut (main/main.cpp gives it the USB port; here it is a string) - so the test
// is: run a whole wake and every line of the console through it, with the fake modem answering
// with three obviously invented numbers, and grep everything that came out. Then run `lte id`
// and check the numbers *are* there, so that a grep which has stopped matching anything cannot
// pass by accident.
#include <doctest.h>

#include <string>

#include "uplink_console_rig.h"

using bench::has;
using bench::LteConsole;
namespace upl = core::uplink;

namespace {

// The fake's numbers (host_test/sim/fake_modem.cpp, makeIdentity).
const char* const kIdentifying[] = {"357999001112223", "8945020000000777666", "238010000012345"};

void checkNothingIdentifying(const std::string& text, const char* what) {
  for (const char* number : kIdentifying) {
    // A std::string, not the pointer: doctest prints a const char* variable as an address
    // (filldata<T*> in doctest.h), and a message that cannot name the number is no use.
    CHECK_MESSAGE(text.find(number) == std::string::npos,
                  what << " contains something that identifies the subscription: "
                       << std::string(number));
  }
}

}  // namespace

TEST_CASE("a whole wake, and every console line but `lte id`, says nothing identifying") {
  LteConsole rig;

  // The uplink up, the questions asked, the readings taken, the modem down again: everything a
  // wake does with the modem (task p3-05 will do exactly this, in this order).
  rig.policy->beginWake();
  const upl::Result r = rig.policy->bringUp();
  REQUIRE(r.up);
  interfaces::Signal signal;
  REQUIRE(rig.policy->signal(&signal));
  interfaces::NetworkTime when;
  rig.policy->networkTime(&when);
  interfaces::PowerSavingGrant grant;
  rig.policy->powerSaving(&grant);

  // And the policy was asked for the numbers too, so that this test covers the case where they
  // were in the house rather than only the case where nobody fetched them.
  interfaces::Identity id;
  REQUIRE(rig.policy->identity(&id));
  CHECK(std::string(id.imei) == "357999001112223");

  rig.policy->takeDown();

  // Every line of the console, `lte id` excepted.
  for (const char* line : {"lte", "status", "help", "pins", "reboots", "lte up", "lte time",
                           "lte down", "lte"}) {
    checkNothingIdentifying(rig.run(line), line);
  }
  // And the whole transcript of this test, in one go: a number that leaked into any line of it -
  // a boot line, a failure line, a status line - fails here.
  checkNothingIdentifying(rig.everything, "the console's whole transcript");

  // What `lte` prints is Status, and Status is where a number would most easily be left behind:
  // the readings are in it (so the line is a real line) and the identity is not.
  CHECK(rig.policy->status().signal_known);
}

TEST_CASE("a failed wake says nothing identifying either") {
  // The failure lines name the taxonomy and how far the sequence got; a modem that answered
  // `AT+CGSN` before it failed to attach must not get its IMEI into one of them.
  for (int which = 0; which < 4; ++which) {
    LteConsole rig;
    switch (which) {
      case 0: rig.modem.deaf = true; break;
      case 1: rig.modem.sim_state = interfaces::SimState::kAbsent; break;
      case 2: rig.modem.never_registers = true; break;
      default: rig.modem.data_ever_ready = false; break;
    }
    rig.run("lte up");
    rig.run("lte");
    checkNothingIdentifying(rig.everything, "a failed wake's console lines");
  }
}

TEST_CASE("lte id does print them: the grep is looking for the right numbers") {
  LteConsole rig;
  rig.run("lte up");
  const std::string text = rig.run("lte id");
  for (const char* number : kIdentifying) {
    CHECK_MESSAGE(text.find(number) != std::string::npos,
                  "lte id did not print " << std::string(number));
  }
}
