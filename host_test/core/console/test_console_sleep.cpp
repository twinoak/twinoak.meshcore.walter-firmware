// The console's five duty-cycle commands (task p3-04): `wake`, `power`, `sleep now`,
// `sleep <seconds>` and `stay` / `stay off` - their grammar, their refusals, and what they say.
// The rig is the cycle bench's world with the console over it, because what these commands print
// is about a wake and not about a radio.
#include <doctest.h>

#include <memory>
#include <string>

#include "core/console/console.h"
#include "core/console/parser.h"
#include "core/cycle/duty_cycle.h"
#include "core/cycle/cycle_bench.h"

using core::console::Verb;

namespace {

bool has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

struct SleepConsole {
  bench::CycleBench b;
  std::unique_ptr<core::console::Console> console;

  SleepConsole() { wakeAs(interfaces::WokeBy::kTimer); }

  // A wake, and a console over the Walter it built. The console keeps references into the
  // Walter, so it is built after the boot and never before.
  void wakeAs(interfaces::WokeBy woke) {
    b.boot(woke);
    b.v().begin();
    build();
  }

  void build() {
    console.reset(new core::console::Console(
        b.v().out, b.v().board, b.v().health, b.world.clock, b.v().restart, b.v().link,
        b.v().slots, nullptr, nullptr, nullptr, &b.v().journal, nullptr, b.v().supervisor.get(),
        &b.v().uplink, nullptr, nullptr, nullptr, &b.v().settings, nullptr, &b.duty()));
  }

  // A console with no duty cycle, as a build without one has.
  void withoutDuty() {
    console.reset(new core::console::Console(b.v().out, b.v().board, b.v().health, b.world.clock,
                                             b.v().restart, b.v().link, b.v().slots, nullptr,
                                             nullptr, nullptr, &b.v().journal, nullptr,
                                             b.v().supervisor.get(), &b.v().uplink, nullptr,
                                             nullptr, nullptr, &b.v().settings));
  }

  std::string run(const char* line) {
    b.v().out.clear();
    console->runLine(line);
    return b.v().out.text;
  }

  std::string begin() {
    b.v().out.clear();
    console->begin();
    return b.v().out.text;
  }
};

}  // namespace

TEST_CASE("the grammar: what the five commands take and what they refuse") {
  struct Row {
    const char* line;
    bool ok;
    Verb verb;
  };
  const Row rows[] = {
      {"wake", true, Verb::kWake},
      {"power", true, Verb::kPower},
      {"sleep now", true, Verb::kSleepNow},
      {"sleep 900", true, Verb::kSleepFor},
      {"sleep 1", true, Verb::kSleepFor},
      {"stay", true, Verb::kStay},
      {"stay off", true, Verb::kStay},
      {"wake up", false, Verb::kEmpty},
      {"power on", false, Verb::kEmpty},
      {"sleep", false, Verb::kEmpty},
      {"sleep soon", false, Verb::kEmpty},
      {"sleep 0", false, Verb::kEmpty},
      {"sleep -5", false, Verb::kEmpty},
      {"sleep 604801", false, Verb::kEmpty},
      {"sleep now please", false, Verb::kEmpty},
      {"stay on", false, Verb::kEmpty},
      {"stay forever", false, Verb::kEmpty},
  };
  for (const Row& row : rows) {
    const core::console::Parsed p = core::console::parseLine(row.line);
    const std::string line = row.line;
    CHECK_MESSAGE(p.ok == row.ok, line << ": " << p.error);
    if (!row.ok) {
      const std::string why = p.error;
      CHECK_MESSAGE(!why.empty(), line);
      CHECK_MESSAGE(why.find("unknown") == std::string::npos, line << ": " << why);
      continue;
    }
    CHECK_MESSAGE(p.command.verb == row.verb, line);
  }
  // The number, and the `on`/`off` of `stay`.
  CHECK(core::console::parseLine("sleep 900").command.seconds == 900);
  CHECK(core::console::parseLine("sleep 604800").command.seconds == 604800);
  CHECK(core::console::parseLine("stay").command.on);
  CHECK_FALSE(core::console::parseLine("stay off").command.on);
  // The ceiling is said in the refusal, so a person does not have to guess it.
  CHECK(has(core::console::parseLine("sleep 999999").error, "604800"));
  // `power` says which command reads the three channels, because `rak power` is the other one.
  CHECK(has(core::console::parseLine("power on").error, "rak power"));
}

TEST_CASE("wake says why this Walter is awake, and the last ten with their durations") {
  SleepConsole c;
  const std::string text = c.run("wake");
  INFO(text);
  CHECK(has(text, "this wake: the timer: the check-in"));
  CHECK(has(text, "the radio's line: armed for the next sleep"));
  CHECK(has(text, "wakes counted:"));
  // No line of it runs off a terminal.
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? end : end - at);
    CHECK_MESSAGE(line.size() <= 100, line);
    if (end == std::string::npos) break;
    at = end + 1;
  }
}

TEST_CASE("wake says what an attention was, and what a spurious wake was") {
  SleepConsole c;
  c.b.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 3);
  c.wakeAs(interfaces::WokeBy::kRadioLine);
  std::string text = c.run("wake");
  INFO(text);
  CHECK(has(text, "the radio's line: it raised attention"));
  CHECK(has(text, "a person asked"));

  // §8: the radio keeps repeating an attention "until any valid request arrives", and working
  // out why we woke sends nothing. So the rest of the repeats are still on the wire; a bench
  // takes them off it before the next wake, as the wake cycle's first request would.
  c.b.world.rak.forgetPending();
  c.b.world.rak.sendBytes({0x00, 0x00, 0x42}, 0);
  c.wakeAs(interfaces::WokeBy::kRadioLine);
  text = c.run("wake");
  INFO(text);
  CHECK(has(text, "a spurious wake"));
  CHECK(has(text, "spurious wake(s) since the last timer wake"));
}

TEST_CASE("power says the band, the readings behind it, and the next interval") {
  SleepConsole c;
  c.b.world.rak.battery_mv = 3500;
  c.b.world.rak.battery_ma = -30;
  const std::string text = c.run("power");
  INFO(text);
  CHECK(has(text, "band: saving"));
  CHECK(has(text, "3500 mV"));
  CHECK(has(text, "discharging"));
  CHECK(has(text, "PLACEHOLDERS, docs/power.md"));
  CHECK(has(text, "this wake may:"));
  CHECK(has(text, "next wake: 360 min"));
  // Every threshold is named, so a person at a bench can read them off the port.
  CHECK(has(text, "upper 4000 mV"));
  CHECK(has(text, "lower 3600 mV"));
  CHECK(has(text, "floor 3400 mV"));
}

TEST_CASE("power with no reading says so and does not pretend to a band") {
  SleepConsole c;
  c.b.world.rak.deaf = true;
  const std::string text = c.run("power");
  INFO(text);
  CHECK(has(text, "band: no-reading"));
  CHECK(has(text, "did not answer"));
  CHECK(has(text, "reading: none"));
  CHECK(has(text, "next wake: 60 min"));
}

TEST_CASE("sleep now and sleep <seconds> go, and say what they did") {
  SleepConsole c;
  c.b.world.rak.battery_mv = 4100;
  const std::string said = c.run("sleep now");
  INFO(said);
  CHECK(has(said, "SLEEP_NOTICE to the radio"));
  // On a board nothing after the sleep is printed: the call does not return. The fake's does,
  // which is how the line below can be read at all.
  CHECK(has(said, "asleep: 900 s, band plenty"));
  CHECK(has(said, "the pads held in the safe state"));
  CHECK(has(said, "expected back at epoch"));
  CHECK(c.b.v().sleeper.sleeps == 1);

  const std::string for_a_minute = c.run("sleep 60");
  INFO(for_a_minute);
  CHECK(has(for_a_minute, "sleeping for 60 s"));
  CHECK(has(for_a_minute, "asleep: 60 s"));
  CHECK(c.b.v().sleeper.sleeps == 2);
  CHECK(c.b.v().sleeper.last().seconds == 60);
}

TEST_CASE("a sleep the duty cycle refuses is one error line, and the sleeper is not called") {
  SleepConsole c;
  // `wake.radio off` and no seconds: a sleep only a reset would end.
  char err[96] = {};
  REQUIRE(c.b.v().settings.put(core::settings::def(core::settings::Key::kWakeRadio), "off", err,
                               sizeof err));
  // The parser refuses `sleep 0` before the console sees it, so the refusal that is tested here
  // is the one the duty cycle itself gives - reached through the interval, which `wake.interval`
  // cannot make zero either. So it is checked where it can be: the sleeper's own refusal.
  c.b.v().sleeper.refuse = true;
  const std::string text = c.run("sleep 300");
  INFO(text);
  CHECK(has(text, "error: sleep refused:"));
  CHECK(has(text, "would not take the sleep plan"));
}

TEST_CASE("stay and stay off say what they change") {
  SleepConsole c;
  const std::string on = c.run("stay");
  INFO(on);
  CHECK(has(on, "stay: this Walter will not be put to sleep"));
  CHECK(has(on, "'stay off' ends it"));
  CHECK(c.b.duty().guard().staying());
  const std::string off = c.run("stay off");
  INFO(off);
  CHECK(has(off, "the bound on a wake applies again"));
  CHECK_FALSE(c.b.duty().guard().staying());
  // And `wake` says it while it is on.
  c.run("stay");
  CHECK(has(c.run("wake"), "'stay' is on"));
}

TEST_CASE("a build with no duty cycle answers in one line") {
  SleepConsole c;
  c.withoutDuty();
  for (const char* line : {"wake", "power", "sleep now", "sleep 60", "stay", "stay off"}) {
    const std::string text = c.run(line);
    INFO(text);
    CHECK_MESSAGE(has(text, "error: this build has no duty cycle"), std::string(line));
  }
  // And nothing slept.
  CHECK(c.b.v().sleeper.sleeps == 0);
}

TEST_CASE("status says this wake and the next one, and stays one screen") {
  SleepConsole c;
  c.b.world.rak.battery_mv = 3800;
  (void)c.b.duty().readPower();
  (void)c.b.duty().decide();
  const std::string text = c.run("status");
  INFO(text);
  CHECK(has(text, "this wake: timer"));
  CHECK(has(text, "next wake: 60 min, band normal"));
  CHECK(has(text, "line armed ('power', 'wake')"));
  std::size_t lines = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? end : end - at);
    if (!line.empty()) {
      CHECK_MESSAGE(line.size() <= 100, line);
      ++lines;
    }
    if (end == std::string::npos) break;
    at = end + 1;
  }
  CHECK(lines >= 10);
  CHECK(lines <= 16);
}

TEST_CASE("the boot says what this wake means, and that a USB host keeps it awake") {
  {
    SleepConsole c;
    const std::string text = c.begin();
    INFO(text);
    CHECK(has(text, "wake: the timer: the check-in"));
    CHECK_FALSE(has(text, "a USB host is on the port"));
  }
  {
    SleepConsole c;
    c.b.world.usb = true;
    c.wakeAs(interfaces::WokeBy::kTimer);
    const std::string text = c.begin();
    INFO(text);
    CHECK(has(text, "a USB host is on the port"));
    CHECK(has(text, "stays awake while you are here"));
    // The prompt itself is `> ` and nothing else: docs/provisioning.md and every tool that
    // reads this port expect exactly that (tasks/phase-3/reports/04.md, *Deviations*).
    CHECK(text.find("usb> ") == std::string::npos);
    CHECK(has(text, "> "));
  }
}

TEST_CASE("journal prints a duty-cycle entry in words") {
  SleepConsole c;
  c.b.world.rak.battery_mv = 3500;
  REQUIRE(has(c.run("sleep now"), "asleep:"));
  const std::string text = c.run("journal");
  INFO(text);
  CHECK(has(text, "duty cycle, sleeping for 21600 s, band saving"));
  CHECK(has(text, "woke by timer"));
  CHECK(has(text, "3500 mV"));
  CHECK(has(text, "a person's command"));
}

TEST_CASE("set wake.interval and set wake.radio say what they reach") {
  SleepConsole c;
  const std::string interval = c.run("set wake.interval 30");
  INFO(interval);
  CHECK(has(interval, "ok wake.interval"));
  CHECK(has(interval, "the next time the interval is chosen"));
  c.b.world.rak.battery_mv = 4100;
  CHECK(has(c.run("power"), "next wake: 30 min"));
  CHECK(has(c.run("power"), "from wake.interval and not the band's 15 min"));

  const std::string radio = c.run("set wake.radio off");
  INFO(radio);
  CHECK(has(radio, "ok wake.radio"));
  CHECK(has(c.run("wake"), "not armed for the next sleep"));
  CHECK(has(c.run("sleep now"), "the radio's line not armed"));
  CHECK_FALSE(c.b.v().sleeper.last().radio_line);
}
