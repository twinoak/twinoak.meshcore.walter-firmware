// The console against the fake pins (task p0-03): what each command prints and does, the echo
// and the line ending, and the property test - a few thousand random command sequences, none of
// which may break a pin rule (the fake fails the test the moment one does).
#include <doctest.h>

#include <random>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/version.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/scripted_radio.h"

using core::board::kKillTiming;
using core::board::kNReset;
using core::board::kPwrKill;
using core::board::RadioPins;
using core::console::Console;
using core::health::Health;
using core::health::ResetReason;
using core::health::WakeCause;
using interfaces::PinMode;

namespace {

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  RadioPins radio{pins, clock, retained};
  Health health{retained, clock};
  // The radio on the other end of the UART, and the client that speaks to it (task p1-02): the
  // console's `rak hello` and the rest go through these.
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  // The slot seam (task p1-06): a single-radio unit, as the firmware builds it.
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, clock, wall, slots};
  Console console{out, radio, health, clock, restart, link, slots};

  Rig() {
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(kKillTiming.settle_ms);
    radio.safe();
  }

  std::string run(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }

  std::string type(const std::string& bytes) {
    out.clear();
    for (char c : bytes) console.feed(c);
    return out.text;
  }
};

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

int countLines(const std::string& text) {
  int n = 0;
  for (char c : text) n += c == '\n';
  return n;
}

}  // namespace

TEST_CASE("begin prints the boot's reason and a prompt") {
  Rig rig;
  rig.out.clear();
  rig.console.begin();
  CHECK(contains(rig.out.text, "boot 1: power-on reset\n"));
  CHECK(contains(rig.out.text, "the boot record starts here"));
  CHECK(contains(rig.out.text, "type 'help' for the commands\n"));
  CHECK(rig.out.text.substr(rig.out.text.size() - 2) == "> ");
}

TEST_CASE("help lists every command") {
  Rig rig;
  const std::string text = rig.run("help");
  for (const char* cmd : {"help", "status", "reboots", "pins", "rak reset", "rak kill <seconds>",
                          "reboot", "rak hello", "rak ping", "rak status", "rak id", "rak time",
                          "rak power", "rak env", "rak cli <line>", "rak events", "rak text",
                          "trust"}) {
    CHECK_MESSAGE(contains(text, cmd), cmd);
  }
  CHECK(contains(text, "(1..30)"));
}

TEST_CASE("trust says this build accepts no update, because keys/release-keys.txt names no key (task p2-01)") {
  Rig rig;
  const std::string text = rig.run("trust");
  CHECK(contains(text, "ed25519 self-test (RFC 8032 test 1, and one bit changed): passed, "));
  CHECK(contains(text, "trust: none: no update will be accepted (keys/release-keys.txt names no key;\n"));
  CHECK(contains(text, "spec/SYSTEM.md section 10 item 1"));
  CHECK_FALSE(contains(text, "TEST KEY"));
  CHECK(rig.run("trust me") == "error: trust takes no argument\n");
  // Not a radio verb: no slot, no hold, nothing on the pins.
  CHECK(rig.run("rak trust") == "error: unknown rak subcommand (try 'help'): trust\n");
}

TEST_CASE("status: version, uptime, why it booted, the pins and the radio's power") {
  Rig rig;
  rig.clock.advance(12345);
  const std::string text = rig.run("status");
  CHECK(contains(text, core::banner()));
  CHECK(contains(text, "uptime 12 s, boot 1"));
  CHECK(contains(text, "booted: power-on reset\n"));
  CHECK(contains(text, "radio power: on\n"));
  CHECK(contains(text, "uart: released; swd: released; nRESET: released; kill line: not driven\n"));
  CHECK(contains(text, "next reset: allowed now\n"));
  CHECK(contains(text, "LORA_nRESET=input/high"));
  CHECK(contains(text, "WALTER_TX=input pull-up/high"));
  CHECK(contains(text, "LORA_PWR_KILL=input/low"));
}

TEST_CASE("status says how long until a reset is allowed") {
  Rig rig;  // constructed at 0, safe() at 200: a reset is assumed to have ended at 0
  const std::string text = rig.run("status");
  CHECK(contains(text, "next reset: in 800 ms (two resets are a second apart)\n"));
}

TEST_CASE("pins: one line per pin with net, D-sub, GPIO, mode, level and use") {
  Rig rig;
  const std::string text = rig.run("pins");
  CHECK(countLines(text) == 1 + 7 + 1);
  CHECK(contains(text, "net            d-sub  gpio  mode           level  use\n"));
  CHECK(contains(text, "LORA_nRESET        3     1  input          high   released\n"));
  CHECK(contains(text, "LORA_SWDCLK        4     2  input          low    released\n"));
  CHECK(contains(text, "LORA_SWDIO         5     4  input          low    released\n"));
  CHECK(contains(text, "LORA_PWR_KILL      6     5  input          low    released\n"));
  CHECK(contains(text, "spare              7     6  input          low    released\n"));
  CHECK(contains(text, "WALTER_TX          8     7  input pull-up  high   released\n"));
  CHECK(contains(text, "WALTER_RX          9     8  input          low    released\n"));
  CHECK(contains(text, "radio power: on\n"));
}

TEST_CASE("rak reset pulses once, saying how long it waited") {
  Rig rig;
  std::string text = rig.run("rak reset");
  CHECK(contains(text, "waiting 800 ms: two resets are a second apart\n"));
  CHECK(contains(text, "ok: nRESET low for 100 ms, then released\n"));
  CHECK(rig.pins.transitionsOf(kNReset.gpio).size() == 2);
  text = rig.run("rak reset");
  CHECK(contains(text, "waiting 1000 ms"));
  CHECK(contains(text, "ok: nRESET"));
  CHECK(rig.pins.transitionsOf(kNReset.gpio).size() == 4);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("rak kill: within bounds it cycles the rail, outside it is refused with the bounds") {
  Rig rig;
  std::string text = rig.run("rak kill 0");
  CHECK(text == "error: rak kill 0: too short for the rail to collapse (1..30 seconds)\n");
  text = rig.run("rak kill 31");
  CHECK(text == "error: rak kill 31: longer than the kill limit (1..30 seconds)\n");
  text = rig.run("rak kill 999999999");
  CHECK(contains(text, "longer than the kill limit"));
  CHECK(rig.pins.transitionsOf(kPwrKill.gpio).empty());
  text = rig.run("rak kill 2");
  CHECK(text == "ok: rail off 2 s, released, settled 200 ms; radio power on\n");
  CHECK(rig.pins.transitionsOf(kPwrKill.gpio).size() == 3);  // high, output, input
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("reboots: this boot and the ones before") {
  Rig rig;
  std::string text = rig.run("reboots");
  CHECK(contains(text, "boots since the record began: 1 (the record started at this boot"));
  CHECK(contains(text, "  1. power-on - this boot\n"));

  // A reboot, then the next boot's console.
  rig.clock.advance(4000);
  text = rig.run("reboot");
  CHECK(text == "rebooting\n");
  CHECK(rig.restart.asked == 1);
  Health next(rig.retained, rig.clock);
  next.begin(ResetReason::kSoftware, WakeCause::kNotFromSleep);
  Console console(rig.out, rig.radio, next, rig.clock, rig.restart, rig.link, rig.slots);
  rig.out.clear();
  console.runLine("reboots");
  CHECK(contains(rig.out.text, "boots since the record began: 2\n"));
  CHECK(contains(rig.out.text, "  1. software (asked for on the console) - this boot; the run before it lasted 4 s\n"));
  CHECK(contains(rig.out.text, "  2. power-on\n"));
  rig.out.clear();
  console.begin();
  CHECK(contains(rig.out.text, "boot 2: software reset, asked for on the console\n"));
}

TEST_CASE("unknown commands and bad arguments get one error line") {
  Rig rig;
  CHECK(rig.run("foo") == "error: unknown command (try 'help'): foo\n");
  CHECK(rig.run("rak kill x") == "error: rak kill: not a whole number of seconds: x\n");
  CHECK(rig.run("rak") == "error: rak needs a subcommand (try 'help')\n");
  CHECK(rig.run("") == "");
  const std::string long_line(core::console::kMaxLineLength + 5, 'a');
  CHECK(rig.run(long_line.c_str()) == "error: line too long (200 characters at most)\n");
}

TEST_CASE("typed bytes are echoed, backspace works, CR LF is one line ending") {
  Rig rig;
  std::string text = rig.type("staxx\b\x7ftus\r\n");
  // "staxx", two erasures (backspace and DEL), "tus", the line ending, then status's output and
  // a prompt
  CHECK(text.rfind("staxx\b \b\b \btus\n", 0) == 0);
  CHECK(contains(text, "radio power: on"));
  CHECK(text.substr(text.size() - 2) == "> ");
  CHECK(countLines(text) > 5);
  // The LF of the CR LF did not make a second, empty command (which would print a second prompt).
  CHECK(text.find("> ") == text.rfind("> "));
  // A bare CR and a bare LF each end a line.
  text = rig.type("help\r");
  CHECK(contains(text, "commands:"));
  text = rig.type("help\n");
  CHECK(contains(text, "commands:"));
  // Backspace on an empty line does nothing.
  CHECK(rig.type("\b") == "");
}

TEST_CASE("an over-long typed line is refused when it ends, and the next line is clean") {
  Rig rig;
  const std::string typed = std::string(core::console::kMaxLineLength + 20, 'q') + "\n";
  std::string text = rig.type(typed);
  CHECK(contains(text, "error: line too long (200 characters at most)\n"));
  text = rig.type("pins\n");
  CHECK(contains(text, "LORA_nRESET"));
}

// A few thousand random command sequences against the fake pins. The fake is strict: a broken
// rule fails the test at the transition that broke it, with the rule's name and the time.
TEST_CASE("property: no sequence of console commands breaks a pin rule") {
  const char* tokens[] = {"help", "status", "reboots", "pins", "rak", "reset", "kill", "reboot",
                          "0", "1", "2", "5", "29", "30", "31", "100", "-1", "1.5", "x", "", "  ",
                          "rak reset", "rak kill 1", "rak kill 2", "rak kill 30", "rak kill 0",
                          "rak kill 31", "rak reset rak reset", "RAK RESET", "\x01", "\xff",
                          // the link (task p1-02): every subcommand, and the words they are made
                          // of, so that half-typed ones are in the bag too
                          "hello", "ping", "id", "time", "set", "power", "env", "cli", "events",
                          "text", "rak hello", "rak ping", "rak status", "rak id", "rak time",
                          "rak time set 1791719000", "rak time set x", "rak power", "rak env",
                          "rak cli get radio", "rak cli", "rak events", "rak text",
                          // the debug port (task p1-03): no target behind these fake pins, so
                          // every connect fails and must still release the lines
                          "swd", "connect", "rak swd", "rak swd connect", "rak swd connect now",
                          // the nRF52 (task p1-04): the same, nothing answers, every session
                          // must still release the lines
                          "mem", "rak swd id", "rak swd mem", "rak swd mem 0x10000100 2",
                          "rak swd mem 0x1 2", "rak swd mem 0x20000000 32", "0x10000100",
                          // the flasher (task p1-05): nothing answers, the lines are released
                          "flash-test", "rak flash-test", "rak flash-test now",
                          // the trust table (task p2-01): no pin, no slot
                          "trust", "trust me", "rak trust",
                          // the radio's update (task p2-03): this rig has no update machine, so
                          // every one is refused in one line and touches nothing
                          "update", "role", "scanner", "repeater", "--first-install",
                          "rak update", "rak update status", "rak update --first-install",
                          "rak role scanner", "rak role repeater"};
  const std::size_t token_count = sizeof tokens / sizeof tokens[0];
  std::mt19937 rng(20261011u);
  std::uniform_int_distribution<std::size_t> pick(0, token_count - 1);
  std::uniform_int_distribution<int> lines_per_sequence(1, 6);
  std::uniform_int_distribution<int> tokens_per_line(1, 4);
  std::uniform_int_distribution<int> pause_ms(0, 1500);
  std::uniform_int_distribution<int> coin(0, 9);

  Rig rig;
  int sequences = 0, lines = 0, resets = 0, kills = 0;
  for (; sequences < 3000; ++sequences) {
    const int n = lines_per_sequence(rng);
    for (int i = 0; i < n; ++i) {
      std::string line;
      const int t = tokens_per_line(rng);
      for (int j = 0; j < t; ++j) {
        if (j > 0) line += ' ';
        line += tokens[pick(rng)];
      }
      if (coin(rng) == 0) line += "\r\n";
      const std::size_t before = rig.pins.transitionsOf(kNReset.gpio).size();
      const std::size_t kill_before = rig.pins.transitionsOf(kPwrKill.gpio).size();
      rig.console.runLine(line.c_str());
      lines++;
      resets += rig.pins.transitionsOf(kNReset.gpio).size() > before;
      kills += rig.pins.transitionsOf(kPwrKill.gpio).size() > kill_before;
      // After every line the board is quiet: the kill line not driven, nRESET not asserted, SWD
      // not held, and nothing driving a pin - except WALTER_TX while the link client holds the
      // UART, which is the one pin that is *meant* to be driven (ARCHITECTURE §2 rule 1: TX is
      // driven only between uartEnable() and uartRelease(), and the client keeps the port open so
      // that the radio's events are heard). Task p0-03's version of this test had no link in it
      // and could ask for silence on all seven.
      CHECK(!rig.radio.killDriven());
      CHECK(!rig.radio.resetAsserted());
      CHECK(!rig.radio.swdHeld());
      for (const core::board::PinDef& pin : core::board::kRadioPins) {
        const bool may_drive = pin.gpio == core::board::kWalterTx.gpio && rig.radio.uartEnabled();
        const bool quiet = may_drive ? true : !rig.pins.driving(pin.gpio);
        CHECK_MESSAGE(quiet, pin.net << " driven after '" << line << "'");
      }
      if (rig.pins.violations.size() != 0) {
        FAIL("rule " << rig.pins.violations[0].rule << " broken by '" << line << "': "
                     << rig.pins.violations[0].detail);
      }
      rig.clock.advance(static_cast<std::uint64_t>(pause_ms(rng)));
      rig.out.clear();  // the output is not the point here
      if (rig.pins.log.size() > 100000) rig.pins.clearLog();
    }
  }
  CHECK(rig.pins.violations.empty());
  CHECK(sequences == 3000);
  CHECK(lines > 3000);
  // Fewer of each than task p0-03's version of this test saw: the bag of tokens grew by the
  // link's twenty-odd (task p1-02), the SWD commands' dozen (tasks p1-03, p1-04) and the update's
  // ten (task p2-03), so a given line is less often one of these two. The run is seeded, and at
  // this seed it is 30 resets and 98 kills in some 10 000 lines.
  CHECK(resets > 25);
  CHECK(kills > 90);
  MESSAGE(sequences << " sequences, " << lines << " lines, " << resets << " resets, " << kills
                    << " kills, " << rig.restart.asked << " reboots, no rule broken");
}

// ---------------------------------------------------------------------------- the link (p1-02)
//
// The answers come from spec/fixtures/link/link-v1.json through the scripted radio, so what the
// console prints here is the specification's own numbers.

TEST_CASE("rak hello prints the version, role, slot and capabilities") {
  Rig rig;
  const std::string text = rig.run("rak hello");
  CHECK(contains(text, "hello: protocol 1.0, role 1 (repeater), slot 0, max payload 240"));
  CHECK(contains(text, "firmware 0.1.0+abc1234, board rak4630-single, "
                       "upstream repeater-v1.17.1+d9296435\n"));
  CHECK(contains(text, "capabilities 0x3f: cli, power, env, mesh-client, observations, attention"));
  // §5.4: a boot_id this end has not seen means the radio's clock wants setting.
  CHECK(contains(text, "its clock has not been set since it booted"));
  CHECK(contains(rig.run("status"), "link: frames (protocol 1.0); a session is up"));
}

TEST_CASE("rak ping, rak status, rak id, rak time, rak power, rak env in words") {
  Rig rig;
  REQUIRE(contains(rig.run("rak hello"), "hello: protocol 1.0"));

  CHECK(contains(rig.run("rak ping"), "ping: the nonce came back"));
  // The radio answers the fixture's PING response, whose nonce is the fixture's and not ours.
  rig.rak.answerWith("ping response");
  CHECK(contains(rig.run("rak ping"), "a different nonce came back, uptime 86400 s"));

  std::string text = rig.run("rak status");
  CHECK(contains(text, "radio status: uptime 3600 s, booted: 2 (reset pin)\n"));
  CHECK(contains(text, "clock 1791720000, time state 2 (set over this link since boot)\n"));
  CHECK(contains(text, "flags 0x10: the radio initialised\n"));
  CHECK(contains(text, "observations 7 pending, 0 dropped; link errors 1; attention 0 (none)\n"));

  text = rig.run("rak id");
  CHECK(contains(text, "identity: public key "
                       "7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c\n"));
  CHECK(contains(text, "name TwinOak Risskov\n"));

  CHECK(contains(rig.run("rak time"), "radio clock: 1791720042, time state 2 (set over this"));
  text = rig.run("rak time set 1791720000");
  CHECK(contains(text, "radio clock: was 1715770400, now 1791720000, time state 2 ("));

  text = rig.run("rak power");
  CHECK(contains(text, "power (valid 0x7):\n"));
  CHECK(contains(text, "  panel   18100 mV,    120 mA\n"));
  CHECK(contains(text, "  battery  4012 mV,    -52 mA\n"));
  CHECK(contains(text, "  load     3980 mV,     85 mA\n"));

  // §5.8's fixture is below freezing, which is why it is the fixture: -3.50 C.
  CHECK(contains(rig.run("rak env"), "env (valid 0x7): -3.50 C, 81.25 %rh, 101325 Pa\n"));
}

TEST_CASE("rak cli runs a line and prints the reply; silence after one is expected") {
  Rig rig;
  REQUIRE(rig.link.hello().ok());
  const std::string text = rig.run("rak cli get radio");
  CHECK(contains(text, "reply: > 869.618,62.5,8,8\n"));
  REQUIRE(rig.rak.requests.size() >= 2);
  const sim::ScriptedRadio::Seen& seen = rig.rak.requests.back();
  CHECK(seen.type == core::link::type::kCli);
  // §5.7: the request is the line itself - 1 to 160 bytes of UTF-8, no length and no terminator.
  CHECK(std::string(seen.payload.begin(), seen.payload.end()) == "get radio");

  // §5.7: `reboot` restarts the node and never answers. Three attempts, then a line that says so.
  rig.rak.deaf = true;
  const std::string quiet = rig.run("rak cli reboot");
  CHECK(contains(quiet, "error: rak cli: no answer to three attempts: the link is down\n"));
  CHECK(contains(quiet, "some lines (reboot) restart the radio and never answer"));
}

TEST_CASE("rak events prints what the radio sent, decoded") {
  Rig rig;
  REQUIRE(rig.link.hello().ok());
  CHECK(contains(rig.run("rak events"), "no events queued (0 seen, 0 dropped, 0 gaps)\n"));

  rig.rak.sendEvent("attention: the battery is low");
  rig.rak.sendEvent("mesh reply: a cli reply");
  const std::string text = rig.run("rak events");
  CHECK(contains(text, "2 events queued (2 seen, 0 dropped, 0 gaps):\n"));
  CHECK(contains(text, "1. attention (seq 0): 2 (battery low), detail 3450, repeat 0\n"));
  CHECK(contains(text, "2. mesh reply (seq 1): a text reply, tag 0x00000000, snr 31/4 dB, "
                       "rssi -80 dBm, 2890 ms\n"));
  // Taken out of the queue by printing them.
  CHECK(contains(rig.run("rak events"), "no events queued (2 seen"));
}

TEST_CASE("rak text prints a scanner's lines, and says so when there are none") {
  Rig rig;
  CHECK(contains(rig.run("rak text"), "no scanner lines (0 seen, 0 dropped; the link is unknown"));
  rig.rak.scanner = true;
  CHECK(contains(rig.run("rak hello"), "the radio is running the scanner: scanner (text)"));
  rig.rak.sendLine("{\"t\":\"stat\",\"up\":12}");
  const std::string text = rig.run("rak text");
  CHECK(contains(text, "kept of "));
  CHECK(contains(text, "{\"t\":\"stat\",\"up\":12}\n"));
  // §9: a frame is never sent in a text session - every rak command says so instead.
  CHECK(rig.run("rak ping") ==
        "error: rak ping: the radio is running the scanner: no frames are sent\n");
}

TEST_CASE("a radio that says nothing, and one that cannot do what was asked") {
  Rig rig;
  rig.rak.deaf = true;
  CHECK(rig.run("rak hello") ==
        "error: rak hello: no answer to three attempts: the link is down\n");
  CHECK(rig.rak.requests.size() == 3);  // §3: three attempts, the same seq
  CHECK(rig.run("rak status") ==
        "error: rak status: no answer to three attempts: the link is down\n");

  // §4: POWER_GET needs the POWER capability. Without a session there is nothing to check it
  // against, and with one that lacks it nothing is sent at all.
  Rig other;
  CHECK(other.run("rak power") == "error: rak power: no session: say hello first\n");
  other.rak.caps = core::link::cap::kCli;
  REQUIRE(other.link.hello().ok());
  const std::size_t sent = other.rak.requests.size();
  CHECK(other.run("rak power") ==
        "error: rak power: the radio does not advertise the capability\n");
  CHECK(other.rak.requests.size() == sent);
}

TEST_CASE("rak reset and rak kill tell the client the radio is starting over") {
  Rig rig;
  REQUIRE(rig.link.hello().ok());
  REQUIRE(rig.link.session().up);
  CHECK(contains(rig.run("rak reset"), "ok: nRESET low for 100 ms"));
  CHECK_FALSE(rig.link.session().up);
  CHECK(rig.link.mode() == core::link::client::Mode::kUnknown);

  REQUIRE(rig.link.hello().ok());
  CHECK(contains(rig.run("rak kill 2"), "ok: rail off 2 s"));
  CHECK_FALSE(rig.link.session().up);
  // The kill took the UART from under the port (ARCHITECTURE §2 rule 3): the port noticed and
  // let the stream go with it, rather than keeping a peripheral on a pad it no longer owns.
  CHECK_FALSE(rig.link.port().isOpen());
  CHECK(rig.link.port().lost() == 1);
  CHECK(rig.pins.violations.empty());
}

// ------------------------------------------------------------------ the slot (task p1-06)

TEST_CASE("every rak command takes @0 without saying so, and @1 is refused on this unit") {
  Rig rig;
  // @0 is what a line without a slot means: the same output, to the byte.
  const std::string plain = rig.run("rak hello");
  const std::string at_zero = rig.run("rak @0 hello");
  CHECK(plain == at_zero);

  const std::string refused = rig.run("rak @1 hello");
  CHECK(refused == "error: slot 1: this adapter has 1 radio, so slot 0 is the only one\n");
  // Every one of them, and nothing reached a pin or the radio.
  rig.rak.writes.clear();
  for (const char* line : {"rak @1 reset", "rak @1 kill 2", "rak @1 ping", "rak @1 status",
                           "rak @1 id", "rak @1 time", "rak @1 power", "rak @1 env",
                           "rak @1 cli get radio", "rak @1 events", "rak @1 text",
                           "rak @1 swd connect", "rak @1 swd id", "rak @1 swd mem 0x10000100",
                           "rak @1 flash-test"}) {
    CHECK(rig.run(line) == "error: slot 1: this adapter has 1 radio, so slot 0 is the only one\n");
  }
  CHECK(rig.rak.writes.empty());
  CHECK(rig.pins.selector_clocks == 0);
  CHECK(rig.pins.violations.empty());
  // A number that is not a position of any adapter is the parser's refusal, not the console's.
  CHECK(rig.run("rak @2 hello") == "error: rak: not a slot (@0 or @1): @2\n");
  // And help says where the slot goes.
  CHECK(rig.run("help").find("rak @1 hello") != std::string::npos);
}
