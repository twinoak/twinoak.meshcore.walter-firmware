// The console against the fake pins (task p0-03): what each command prints and does, the echo
// and the line ending, and the property test - a few thousand random command sequences, none of
// which may break a pin rule (the fake fails the test the moment one does).
#include <doctest.h>

#include <random>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/version.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"

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
  Console console{out, radio, health, clock, restart};

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
  for (const char* cmd : {"help", "status", "reboots", "pins", "rak reset", "rak kill <seconds>", "reboot"}) {
    CHECK_MESSAGE(contains(text, cmd), cmd);
  }
  CHECK(contains(text, "(1..30)"));
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
  Console console(rig.out, rig.radio, next, rig.clock, rig.restart);
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
  CHECK(rig.run("rak") == "error: rak needs a subcommand: rak reset, rak kill <seconds>\n");
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
                          "rak kill 31", "rak reset rak reset", "RAK RESET", "\x01", "\xff"};
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
      // After every line the board is quiet: nothing driven, nothing asserted, nothing held.
      CHECK(!rig.radio.killDriven());
      CHECK(!rig.radio.resetAsserted());
      CHECK(!rig.radio.swdHeld());
      CHECK(!rig.radio.uartEnabled());
      for (const core::board::PinDef& pin : core::board::kRadioPins) {
        CHECK_MESSAGE(!rig.pins.driving(pin.gpio), pin.net << " driven after '" << line << "'");
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
  CHECK(resets > 100);
  CHECK(kills > 100);
  MESSAGE(sequences << " sequences, " << lines << " lines, " << resets << " resets, " << kills
                    << " kills, " << rig.restart.asked << " reboots, no rule broken");
}
