// The console's five uplink commands (task p3-01; docs/console.md, *The uplink*): `lte`, which
// only ever reads - looking at the uplink must not be able to switch a modem on - and `lte up`,
// `lte down`, `lte time` and `lte id`, which act.
//
// A console line is what a person on a bench has instead of a debugger, so what each one says is
// held to here in words: the failure's own name from the taxonomy, what it means, how far the
// sequence got, and when the next attempt is allowed.
#include <doctest.h>

#include <string>

#include "core/console/parser.h"
#include "uplink_console_rig.h"

using bench::has;
using bench::LteConsole;
namespace upl = core::uplink;

TEST_CASE("the parser takes the five lines and refuses the rest") {
  using core::console::parseLine;
  using core::console::Verb;
  CHECK(parseLine("lte").command.verb == Verb::kLte);
  CHECK(parseLine("lte up").command.verb == Verb::kLteUp);
  CHECK(parseLine("lte down").command.verb == Verb::kLteDown);
  CHECK(parseLine("lte time").command.verb == Verb::kLteTime);
  CHECK(parseLine("lte id").command.verb == Verb::kLteId);
  CHECK(parseLine("  lte  up \r\n").command.verb == Verb::kLteUp);

  // A modem has no slot: `lte` is not a `rak` line and nothing takes `@N`.
  CHECK_FALSE(parseLine("lte @1").ok);
  CHECK(has(parseLine("lte @1").error, "unknown lte subcommand"));
  CHECK_FALSE(parseLine("lte sideways").ok);
  CHECK(has(parseLine("lte sideways").error, "(lte, up, down, time, id): sideways"));
  CHECK_FALSE(parseLine("lte up now").ok);
  CHECK(has(parseLine("lte up now").error, "lte up takes no argument"));
  CHECK_FALSE(parseLine("lte id please").ok);
}

TEST_CASE("a build with no uplink says so in one line and does nothing") {
  LteConsole rig;
  rig.withoutUplink();
  for (const char* line : {"lte", "lte up", "lte down", "lte time", "lte id"}) {
    const std::string text = rig.run(line);
    CHECK(has(text, "error: this build has no uplink"));
  }
  CHECK(rig.modem.presents == 0);
}

TEST_CASE("lte before anything has been tried: the state, and that nothing was tried") {
  LteConsole rig;
  const std::string text = rig.run("lte");
  CHECK(has(text, "lte: down; it has not been up in this boot"));
  CHECK(has(text, "last attempt: none in this boot"));
  CHECK(has(text, "SIM PIN: none is set"));
  CHECK(has(text, "signal: not read in this boot"));
  CHECK(has(text, "power saving: asked for off (the modem is switched off between wakes)"));
  // **It read and nothing more**: the modem was not even started.
  CHECK(rig.modem.presents == 0);
  CHECK(rig.modem.op_states == 0);
  CHECK(rig.modem.take_downs == 0);
}

TEST_CASE("lte up brings it up and says what the network gave") {
  LteConsole rig;
  const std::string text = rig.run("lte up");
  CHECK(has(text, "lte up: bringing the uplink up"));
  CHECK(has(text, "The backoff would have allowed this wake"));
  CHECK(has(text, "lte up: up in "));
  CHECK(has(text, "RSRP -92 dBm"));
  CHECK(has(text, "RSRQ -10.5 dB"));
  CHECK(has(text, "RSSI -71 dBm"));
  CHECK(has(text, "operator TDC"));
  CHECK(has(text, "band 20"));
  CHECK(has(text, "LTE-M"));
  CHECK(has(text, "unix seconds, from the network"));
  CHECK(rig.policy->state() == interfaces::UplinkState::kUp);

  // And `lte` then has the same readings without asking the modem again.
  const int asked = rig.modem.signals;
  const std::string after = rig.run("lte");
  CHECK(has(after, "lte: up"));
  CHECK(has(after, "last attempt: none - the uplink is up"));
  CHECK(has(after, "operator TDC"));
  CHECK(rig.modem.signals == asked);
}

TEST_CASE("lte down takes the radio off") {
  LteConsole rig;
  rig.run("lte up");
  REQUIRE(rig.policy->state() == interfaces::UplinkState::kUp);
  const std::string text = rig.run("lte down");
  CHECK(has(text, "lte down: the modem's radio is off (down)"));
  CHECK(rig.policy->state() == interfaces::UplinkState::kDown);
}

TEST_CASE("each failure of the table reaches the console with its own name and its own wait") {
  SUBCASE("modem-silent") {
    LteConsole rig;
    rig.modem.deaf = true;
    const std::string text = rig.run("lte up");
    CHECK(has(text, "lte up: modem-silent - no answer to the first command"));
    CHECK(has(text, "reached 'is the modem there'"));
    CHECK(has(text, "the modem was reset"));
    CHECK(has(text, "try again: after a modem reset, once per wake"));
  }
  SUBCASE("no-sim") {
    LteConsole rig;
    rig.modem.sim_state = interfaces::SimState::kAbsent;
    const std::string text = rig.run("lte up");
    CHECK(has(text, "lte up: no-sim - no card, or it does not answer"));
    CHECK(has(text, "reached 'the SIM'"));
    CHECK(has(text, "try again: next wake; not sooner (a person has to act)"));
  }
  SUBCASE("sim-locked") {
    LteConsole rig;
    rig.modem.sim_state = interfaces::SimState::kPinRequired;
    const std::string text = rig.run("lte up");
    CHECK(has(text, "lte up: sim-locked - a PIN is wanted and none is set"));
    CHECK(has(text, "try again: never automatically - a person has to act"));
    CHECK(rig.modem.unlocks == 0);
    // And `lte` says the same, with the PIN's own line.
    CHECK(has(rig.run("lte"), "SIM PIN: none is set"));
  }
  SUBCASE("not-registered") {
    LteConsole rig;
    rig.modem.never_registers = true;
    const std::string text = rig.run("lte up");
    CHECK(has(text, "lte up: not-registered"));
    CHECK(has(text, "reached 'waiting for the network'"));
    CHECK(has(text, "try again: growing"));
  }
  SUBCASE("no-data") {
    LteConsole rig;
    rig.modem.data_ever_ready = false;
    const std::string text = rig.run("lte up");
    CHECK(has(text, "lte up: no-data - registered, no packet data"));
    CHECK(has(text, "reached 'packet data'"));
  }
}

TEST_CASE("lte says when the next attempt is allowed, and lte up goes ahead anyway") {
  LteConsole rig;
  rig.modem.never_registers = true;
  // Four failures in a row: the growing backoff is past "every wake" by now.
  for (int i = 0; i < 4; ++i) {
    rig.policy->beginWake();
    rig.policy->bringUp();
  }
  REQUIRE(rig.policy->status().interval_wakes > 1);
  const std::string text = rig.run("lte");
  CHECK(has(text, "failures in a row"));
  CHECK(has(text, "until the next is allowed"));

  // A person at a console is not the backoff's business - but it is told what the backoff
  // thought, so that "why did it not try by itself" has an answer on the same screen.
  rig.modem.never_registers = false;
  const std::string forced = rig.run("lte up");
  CHECK(has(forced, "The backoff would have waited"));
  CHECK(has(forced, "a person asking goes ahead anyway"));
  CHECK(has(forced, "lte up: up in "));
  // And the success emptied the backoff.
  CHECK(rig.policy->status().failures == 0);
}

TEST_CASE("lte time: the network's time, and the refusals that are not a time") {
  SUBCASE("the network's") {
    LteConsole rig;
    rig.run("lte up");
    const std::string text = rig.run("lte time");
    CHECK(has(text, "lte time: "));
    CHECK(has(text, "unix seconds, from the network"));
  }
  SUBCASE("the modem's own default is refused, in words") {
    LteConsole rig;
    rig.modem.time_epoch = 0;  // 1970
    rig.run("lte up");
    const std::string text = rig.run("lte time");
    CHECK(has(text, "error: lte time: the modem answered with its own default"));
    CHECK(has(text, "a wrong clock is worse than none"));
    CHECK_FALSE(has(text, "1970"));
  }
  SUBCASE("no answer at all") {
    LteConsole rig;
    rig.modem.time_answered = false;
    rig.run("lte up");
    CHECK(has(rig.run("lte time"), "error: lte time: no answer"));
  }
}

TEST_CASE("lte up on a modem that comes up without a time says the uplink is up all the same") {
  LteConsole rig;
  rig.modem.time_answered = false;
  const std::string text = rig.run("lte up");
  CHECK(has(text, "lte up: up in "));
  CHECK(has(text, "time: none - the uplink is up all the same"));
}

TEST_CASE("lte id prints the three numbers and says why it is the only place they appear") {
  LteConsole rig;
  rig.run("lte up");
  const std::string text = rig.run("lte id");
  CHECK(has(text, "lte id: IMEI 357999001112223"));
  CHECK(has(text, "ICCID 8945020000000777666"));
  CHECK(has(text, "IMSI 238010000012345"));
  CHECK(has(text, "they are printed because a person asked"));

  // A modem that will not say so answers with one line and no blanks pretending to be numbers.
  LteConsole quiet;
  quiet.modem.identity_answered = false;
  const std::string refused = quiet.run("lte id");
  CHECK(has(refused, "error: lte id: the modem would not say"));
  CHECK_FALSE(has(refused, "IMEI"));
}

TEST_CASE("the first boot says the backoff's record was not there") {
  LteConsole rig;
  CHECK(has(rig.run("lte"), "the backoff's record was gone at this boot"));
  rig.run("lte up");
  // A restart with RTC memory intact: the record is there and the line is not printed.
  rig.build();
  CHECK_FALSE(has(rig.run("lte"), "the backoff's record was gone"));
}

TEST_CASE("help names the five lines") {
  LteConsole rig;
  const std::string text = rig.run("help");
  CHECK(has(text, "the uplink: state, operator, band, signal"));
  CHECK(has(text, "lte up              bring the uplink up now"));
  CHECK(has(text, "lte down            the modem's radio off"));
  CHECK(has(text, "lte time            the network's time"));
  CHECK(has(text, "lte id              the modem's IMEI"));
}
