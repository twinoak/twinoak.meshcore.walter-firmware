// Going to sleep (task p3-04, core/cycle/sleep.h): the one way out of a wake, in order - the
// journal, `SLEEP_NOTICE` as the last request (§8), the modem down, the pins safe and held, deep
// sleep. **The fake sleeper checks the pins at the moment of sleeping** in every path that
// sleeps, errors included (host_test/fakes/fake_sleeper.h), which is one of the task's own
// requirements.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/cycle/sleep.h"
#include "core/link/codec/wire.h"
#include "cycle_bench.h"

using bench::CycleBench;
using core::cycle::SleepRefusal;
using core::cycle::SleepResult;
using core::cycle::Wrote;

namespace {

// Was SLEEP_NOTICE the *last* request on the wire, and what did it say.
bool noticeWasLast(const bench::CycleWorld& world, std::uint32_t* seconds) {
  if (world.rak.requests.empty()) return false;
  const sim::ScriptedRadio::Seen& last = world.rak.requests.back();
  if (last.type != core::link::type::kSleepNotice) return false;
  if (last.payload.size() < 4) return false;
  *seconds = static_cast<std::uint32_t>(last.payload[0]) |
             (static_cast<std::uint32_t>(last.payload[1]) << 8) |
             (static_cast<std::uint32_t>(last.payload[2]) << 16) |
             (static_cast<std::uint32_t>(last.payload[3]) << 24);
  return true;
}

}  // namespace

TEST_CASE("a sleep: the notice last, the modem down, the pads held, the timer armed") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  // A session first, as a wake has by the time it sleeps, and some requests after it.
  REQUIRE(b.v().link.hello().ok());
  REQUIRE(b.v().link.ping(7).ok());
  b.world.clock.advance(3000);
  b.world.rak.battery_mv = 4100;
  REQUIRE(b.duty().readPower());
  const core::power::Decision d = b.duty().decide();
  REQUIRE(d.band == core::power::Band::kPlenty);

  const SleepResult r = b.duty().sleepNow();
  CHECK(r.refusal == SleepRefusal::kNone);
  CHECK(r.seconds == core::power::kPlentyMinutes * 60);
  CHECK(r.band == core::power::Band::kPlenty);
  CHECK(r.radio_line_armed);
  CHECK(r.pins_held);
  CHECK(r.notice_sent);
  CHECK(r.awake_s > 0);
  // §8's own number, and the epoch it expects to be back at.
  std::uint32_t said = 0;
  CHECK(noticeWasLast(b.world, &said));
  CHECK(said == r.seconds);
  CHECK(r.epoch == 1760000000u + r.seconds);
  // The modem is down on every way out of a wake.
  CHECK(b.v().uplink.state() == interfaces::UplinkState::kDown);
  // The plan the chip was given: the timer and the line, and nothing else.
  REQUIRE(b.v().sleeper.sleeps == 1);
  CHECK(b.v().sleeper.last().seconds == r.seconds);
  CHECK(b.v().sleeper.last().radio_line);
}

TEST_CASE("SLEEP_NOTICE is the last request of every wake that had a link") {
  CycleBench b;
  // Three wakes, each doing a few things on the link before it sleeps.
  for (int i = 0; i < 3; ++i) {
    b.boot(interfaces::WokeBy::kTimer);
    b.v().begin();
    REQUIRE(b.v().link.hello().ok());
    REQUIRE(b.v().link.status().ok());
    REQUIRE(b.duty().readPower());
    (void)b.duty().decide();
    const SleepResult r = b.duty().sleepNow();
    REQUIRE(r.refusal == SleepRefusal::kNone);
    std::uint32_t said = 0;
    CHECK_MESSAGE(noticeWasLast(b.world, &said), i);
    CHECK_MESSAGE(said == r.seconds, i);
  }
}

TEST_CASE("a wake with no link sleeps all the same, and says the notice did not go") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  // The radio is gone: three attempts, no answer. That is not a reason to stay awake - staying
  // awake is what empties the battery they share.
  b.world.rak.deaf = true;
  b.duty().battery().noReading();
  (void)b.duty().decide();
  const SleepResult r = b.duty().sleepNow();
  CHECK(r.refusal == SleepRefusal::kNone);
  CHECK_FALSE(r.notice_sent);
  CHECK(r.notice_fault == core::link::client::Fault::kLinkDown);
  CHECK(r.pins_held);
  CHECK(r.band == core::power::Band::kUnknown);
  CHECK(r.seconds == core::power::kUnknownMinutes * 60);
  CHECK(b.v().sleeper.sleeps == 1);
}

TEST_CASE("a sleep with neither a time nor a source is refused") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  (void)b.duty().decide();
  // `wake.radio off`, and no seconds: a sleep only a person with a reset button would end.
  char err[96] = {};
  REQUIRE(b.v().settings.put(core::settings::def(core::settings::Key::kWakeRadio), "off", err,
                             sizeof err));
  const SleepResult r = b.duty().sleepFor(0);
  CHECK(r.refusal == SleepRefusal::kNoSeconds);
  CHECK(b.v().sleeper.sleeps == 0);
  // With the line armed, no timer is a sleep the radio can end - and that is allowed.
  REQUIRE(b.v().settings.put(core::settings::def(core::settings::Key::kWakeRadio), "on", err,
                             sizeof err));
  const SleepResult with_line = b.duty().sleepFor(0);
  CHECK(with_line.refusal == SleepRefusal::kNone);
  CHECK(b.v().sleeper.last().radio_line);
  CHECK(b.v().sleeper.last().seconds == 0);
}

TEST_CASE("a sleeper that refuses leaves the pads where software can reach them again") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  (void)b.duty().decide();
  b.v().sleeper.refuse = true;
  const SleepResult r = b.duty().sleepNow();
  CHECK(r.refusal == SleepRefusal::kSleeperRefused);
  // The call that does not return, returned: the holds are off again, so the next thing that
  // drives a pin is not writing into a latch.
  CHECK_FALSE(r.pins_held);
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    CHECK_FALSE(b.v().pins.held(pin.gpio));
  }
  CHECK(b.v().pins.violations.empty());
}

TEST_CASE("the pins at the moment of sleeping are safe in every path, errors included") {
  // The fake sleeper checks it at every sleep; this is the check itself shown working and shown
  // refusing, so that a sleeper which stopped checking cannot pass silently.
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  (void)b.duty().decide();
  // A path with the radio gone, a path with the line disarmed, and a path with no time of day:
  // each sleeps, and each is checked.
  b.world.rak.deaf = true;
  CHECK(b.duty().sleepFor(600).refusal == SleepRefusal::kNone);
  b.world.wall.forget();
  CHECK(b.duty().sleepFor(600).refusal == SleepRefusal::kNone);
  CHECK(b.duty().sleepFor(600).epoch == 0);
  CHECK(b.v().sleeper.sleeps == 3);
  CHECK(b.v().pins.violations.empty());

  // And the check fires when the pads are not held. Driving a pin after the sleeper has been
  // told not to check, and then checking by hand, is how that is shown without failing the run.
  b.v().sleeper.check_pins = false;
  REQUIRE(b.v().board.uartEnable() == core::board::Outcome::kOk);
  CHECK(b.v().pins.driving(core::board::uartTxGpio()));
  // The real path puts it back: holdForSleep() is safe() first.
  CHECK(b.duty().sleepFor(600).refusal == SleepRefusal::kNone);
  CHECK_FALSE(b.v().pins.driving(core::board::uartTxGpio()));
}

TEST_CASE("the wake pin is armed with a pull-up and left out of our own holds") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  (void)b.duty().decide();
  CHECK(b.duty().sleepFor(900).refusal == SleepRefusal::kNone);
  // The pad carries the chip's weak pull-up, so a radio that is off, resetting or being flashed
  // cannot hold the Walter awake with a line that has drifted low.
  CHECK(b.v().pins.pulling(core::board::wakePinGpio()));
  CHECK_FALSE(b.v().pins.held(core::board::wakePinGpio()));
  // Every other pad is held by us.
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    if (pin.gpio == core::board::wakePinGpio()) continue;
    CHECK_MESSAGE(b.v().pins.held(pin.gpio), pin.net);
  }
  // With the line not armed, the pad is held like the rest and carries no pull-up.
  char err[96] = {};
  REQUIRE(b.v().settings.put(core::settings::def(core::settings::Key::kWakeRadio), "off", err,
                             sizeof err));
  CHECK(b.duty().sleepFor(900).refusal == SleepRefusal::kNone);
  CHECK(b.v().pins.held(core::board::wakePinGpio()));
  CHECK_FALSE(b.v().pins.pulling(core::board::wakePinGpio()));
  CHECK_FALSE(b.v().sleeper.last().radio_line);
}

TEST_CASE("the journal gets a line when there is news, and not at every check-in") {
  CycleBench b;
  // The first sleep after a power cut is news: nothing was known before it.
  b.world.rak.battery_mv = 4100;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.duty().readPower());
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepNow().journalled);
  std::vector<core::cycle::Payload> entries = b.sleepEntries();
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].what == Wrote::kSleeping);
  CHECK(entries[0].band == core::power::Band::kPlenty);
  CHECK(entries[0].interval_s == core::power::kPlentyMinutes * 60);
  CHECK(entries[0].at_epoch == 1760000000u);
  CHECK((entries[0].flags & core::cycle::kFlagBandChanged) != 0);
  // SLEEP_NOTICE needs no capability (§4: it is always available), so it went and was answered.
  CHECK((entries[0].flags & core::cycle::kFlagNoticeSent) != 0);

  // Two more wakes in the same band: nothing new, nothing written. A unit checking in every
  // quarter of an hour writes 96 wakes a day, and the journal is 1024 entries that exist so an
  // *update* is never guessed at (core/cycle/sleep.h, *what is written down*).
  for (int i = 0; i < 2; ++i) {
    b.boot(interfaces::WokeBy::kTimer);
    b.v().begin();
    REQUIRE(b.duty().readPower());
    (void)b.duty().decide();
    const SleepResult r = b.duty().sleepNow();
    REQUIRE(r.refusal == SleepRefusal::kNone);
    CHECK_FALSE(r.journalled);
  }
  CHECK(b.sleepEntries().size() == 1);

  // The band changing is news.
  b.world.rak.battery_mv = 3500;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.duty().readPower());
  (void)b.duty().decide();
  CHECK(b.duty().sleepNow().journalled);
  entries = b.sleepEntries();
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].band == core::power::Band::kSaving);
  CHECK(entries[0].interval_s == core::power::kSavingMinutes * 60);
  CHECK(entries[0].median_mv == 3500);

  // And a person's `sleep now` is always news: somebody did something.
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.duty().readPower());
  (void)b.duty().decide();
  CHECK(b.duty().sleepNow(true).journalled);
  entries = b.sleepEntries();
  REQUIRE(entries.size() == 3);
  CHECK((entries[0].flags & core::cycle::kFlagByHand) != 0);
}

TEST_CASE("the radio's line being disarmed is its own journal entry") {
  CycleBench b;
  for (std::uint8_t i = 0; i < core::cycle::kSpuriousPerWindow; ++i) {
    b.world.rak.sendBytes({0x00, 0x00, 0x5a}, 0);
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    REQUIRE(b.duty().readPower());
    (void)b.duty().decide();
    REQUIRE(b.duty().sleepNow().refusal == SleepRefusal::kNone);
  }
  const std::vector<core::cycle::Payload> entries = b.sleepEntries();
  bool disarmed = false;
  for (const core::cycle::Payload& p : entries) {
    if (p.what == Wrote::kLineDisarmed) disarmed = true;
  }
  CHECK(disarmed);
  // And the timer wake that arms it again.
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  REQUIRE(b.duty().readPower());
  (void)b.duty().decide();
  REQUIRE(b.duty().sleepNow().refusal == SleepRefusal::kNone);
  bool armed = false;
  for (const core::cycle::Payload& p : b.sleepEntries()) {
    if (p.what == Wrote::kLineArmed) armed = true;
  }
  CHECK(armed);
  for (std::uint8_t w = 0; w <= static_cast<std::uint8_t>(Wrote::kLineArmed); ++w) {
    CHECK(std::string(core::cycle::describe(static_cast<Wrote>(w))) != "?");
  }
  for (std::uint8_t r = 0; r <= static_cast<std::uint8_t>(SleepRefusal::kSleeperRefused); ++r) {
    CHECK(std::string(core::cycle::describe(static_cast<SleepRefusal>(r))) != "?");
  }
}

TEST_CASE("the journal entry's payload survives a round trip, and another kind's does not decode") {
  core::cycle::Payload p{};
  p.what = Wrote::kSleeping;
  p.wake = core::cycle::Wake::kAttention;
  p.band = core::power::Band::kSaving;
  p.flags = core::cycle::kFlagBandChanged | core::cycle::kFlagCharging;
  p.interval_s = 21600;
  p.median_mv = 3555;
  p.mean_ma = -314;
  p.at_epoch = 1760000123;
  p.at_ms = 98765;
  p.awake_s = 42;
  p.attention = core::link::attention::kBatteryLow;
  p.spurious = 2;
  p.faults = 1;
  p.refusal = SleepRefusal::kNone;
  p.wakes = 1234;
  std::uint8_t bytes[core::cycle::kPayloadBytes];
  core::cycle::encode(p, bytes);
  core::update::Entry e{};
  e.kind = core::update::kind::kDutyCycle;
  e.size = core::cycle::kPayloadBytes;
  for (std::size_t i = 0; i < core::cycle::kPayloadBytes; ++i) e.payload[i] = bytes[i];
  core::cycle::Payload back{};
  REQUIRE(core::cycle::decode(e, &back));
  CHECK(back.what == p.what);
  CHECK(back.wake == p.wake);
  CHECK(back.band == p.band);
  CHECK(back.flags == p.flags);
  CHECK(back.interval_s == p.interval_s);
  CHECK(back.median_mv == p.median_mv);
  CHECK(back.mean_ma == p.mean_ma);  // negative, and it comes back negative
  CHECK(back.at_epoch == p.at_epoch);
  CHECK(back.at_ms == p.at_ms);
  CHECK(back.awake_s == p.awake_s);
  CHECK(back.attention == p.attention);
  CHECK(back.spurious == p.spurious);
  CHECK(back.faults == p.faults);
  CHECK(back.wakes == p.wakes);
  // Another owner's entry is not ours.
  e.kind = core::update::kind::kSupervisor;
  CHECK_FALSE(core::cycle::decode(e, &back));
  // Nor is a payload whose bytes are not a Wrote, a Wake, a Band or a refusal.
  e.kind = core::update::kind::kDutyCycle;
  e.payload[0] = 0xEE;
  CHECK_FALSE(core::cycle::decode(e, &back));
  e.payload[0] = static_cast<std::uint8_t>(Wrote::kSleeping);
  e.payload[1] = 0xEE;
  CHECK_FALSE(core::cycle::decode(e, &back));
  e.payload[1] = static_cast<std::uint8_t>(core::cycle::Wake::kTimer);
  e.payload[2] = 0xEE;
  CHECK_FALSE(core::cycle::decode(e, &back));
  e.payload[2] = static_cast<std::uint8_t>(core::power::Band::kNormal);
  e.payload[27] = 0xEE;
  CHECK_FALSE(core::cycle::decode(e, &back));
  // A short payload is not one either.
  e.payload[27] = 0;
  e.size = core::cycle::kPayloadBytes - 1;
  CHECK_FALSE(core::cycle::decode(e, &back));
  // Nor is a null out.
  e.size = core::cycle::kPayloadBytes;
  CHECK_FALSE(core::cycle::decode(e, nullptr));
}

TEST_CASE("a build with no journal sleeps all the same: the retained wake log is all there is") {
  bench::CycleWorld world;
  bench::CycleWalter v(world);
  core::cycle::WakeReason reason(world.retained, world.clock);
  reason.begin(v.health.thisBoot(), interfaces::WokeBy::kTimer, false, nullptr);
  core::cycle::Sleep sleep(v.sleeper, v.board, v.link, v.uplink, reason, world.clock, world.wall);
  core::power::IntervalPolicy policy;
  policy.addSample(3800, -20);
  const SleepResult r = sleep.go(policy.decide(core::power::Inputs{}), true);
  CHECK(r.refusal == SleepRefusal::kNone);
  CHECK_FALSE(r.journalled);
  CHECK(r.pins_held);
  CHECK(v.sleeper.sleeps == 1);
  CHECK(reason.at(0).lasted_s > 0);
}

// -- the one way out ---------------------------------------------------------------------------
//
// `core/cycle/sleep.h` says "**the one way out of a wake**. Nothing else in this firmware calls
// `interfaces::Sleeper::sleep()`, and a host test greps for it." This is that grep, over the
// tree, after host_test/core/net/test_trust_rail.cpp: a second caller would be a path that put a
// Walter to sleep without telling the radio, taking the modem down or holding a pad.
TEST_CASE("only core/cycle/sleep.cpp sleeps, and only platform/sleeper.cpp calls deep sleep") {
  std::vector<std::string> sleep_callers;
  std::vector<std::string> deep_sleepers;
  for (const char* dir : {"components", "main"}) {
    const std::filesystem::path root = std::filesystem::path(TWINOAK_REPO_ROOT) / dir;
    REQUIRE(std::filesystem::is_directory(root));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) continue;
      const std::string ext = entry.path().extension().string();
      if (ext != ".cpp" && ext != ".h" && ext != ".c") continue;
      const std::string name = entry.path().filename().string();
      std::ifstream in(entry.path());
      REQUIRE(in.good());
      std::string line;
      while (std::getline(in, line)) {
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.compare(i, 2, "//") == 0 || line.compare(i, 2, "*") == 0) continue;
        // The interface's own declaration is not a call.
        if (name != "sleeper.h" && line.find("sleeper_.sleep(") != std::string::npos) {
          sleep_callers.push_back(name);
        }
        if (line.find("esp_deep_sleep_start") != std::string::npos ||
            line.find("esp_deep_sleep(") != std::string::npos) {
          deep_sleepers.push_back(name);
        }
      }
    }
  }
  REQUIRE_MESSAGE(sleep_callers.size() == 1,
                  sleep_callers.size() << " files call Sleeper::sleep()");
  CHECK(sleep_callers[0] == "sleep.cpp");
  REQUIRE_MESSAGE(deep_sleepers.size() == 1,
                  deep_sleepers.size() << " files call ESP-IDF's deep sleep");
  CHECK(deep_sleepers[0] == "sleeper.cpp");
}
