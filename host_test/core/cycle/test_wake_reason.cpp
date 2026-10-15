// What a wake means (task p3-04, core/cycle/wake_reason.h), against the real scripted radio: a
// timer wake, a wake on the radio's line with a valid EV_ATTENTION on the 1st, the 4th and the
// 10th repeat, one with noise only, one with a scanner's text, the spurious rate limit that
// disarms the line and the timer wake that arms it again, the power-up that reset the radio with
// us, a crash and a watchdog, and the wake log that crosses a sleep and not a power cut.
#include <doctest.h>

#include <string>

#include "core/cycle/wake_reason.h"
#include "core/link/codec/wire.h"
#include "cycle_bench.h"

using bench::CycleBench;
using core::cycle::Wake;

namespace {

// How long the attention window is, in the clock the bench owns.
constexpr std::uint32_t kWindow = core::cycle::kAttentionWindowMs;

}  // namespace

TEST_CASE("a timer wake is the check-in") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kTimer);
  CHECK_FALSE(b.wake().radio_may_be_booting);
  CHECK(b.wake().listened_ms == 0);  // nothing is listened for on a timer wake
  // One, not two: the bench's first Walter is built but never asked why it is awake, as a
  // Walter whose main() had not reached the duty cycle yet would not be.
  CHECK(b.wake().wakes == 1);
  CHECK(std::string(core::cycle::name(Wake::kTimer)) == "timer");
  for (std::uint8_t w = 0; w < static_cast<std::uint8_t>(Wake::kCount); ++w) {
    CHECK(std::string(core::cycle::name(static_cast<Wake>(w))) != "?");
    CHECK(std::string(core::cycle::describe(static_cast<Wake>(w))) != "?");
  }
}

TEST_CASE("a wake on the radio's line: a valid attention on the 1st, the 4th and the 10th repeat") {
  // §8: the radio sends 0x00 0x00, waits 300 ms and repeats the event every second, ten times.
  // The first bytes are what woke us and are gone; a Walter needs a few hundred milliseconds
  // before its UART listens, "which is why the event is repeated and why losing the first ones
  // is expected". So each of the three is a Walter that was in time for a different repeat.
  const int repeats[] = {1, 4, 10};
  for (int which : repeats) {
    CycleBench b;
    b.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, which, which - 1);
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    const std::string where = "repeat " + std::to_string(which);
    CHECK_MESSAGE(b.wake().wake == Wake::kAttention, where);
    CHECK_MESSAGE(b.wake().attention.reason == core::link::attention::kPersonAsked, where);
    CHECK_MESSAGE(b.wake().attention.repeat == which - 1, where);
    // It stopped listening as soon as it had one, well inside the window.
    CHECK_MESSAGE(b.wake().listened_ms < kWindow, where);
    CHECK_MESSAGE(b.wake().spurious_in_window == 0, where);
    CHECK_MESSAGE(b.duty().lineWillArm(), where);
  }
}

TEST_CASE("an attention whose reason is the battery carries its millivolts to the policy") {
  CycleBench b;
  b.world.rak.raiseAttention(core::link::attention::kBatteryCritical, 3380, 3);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  REQUIRE(b.wake().wake == Wake::kAttention);
  CHECK(b.wake().attention.reason == core::link::attention::kBatteryCritical);
  CHECK(b.wake().attention.detail == 3380);
  // And the band follows it at once, without a reading of our own.
  b.world.rak.power_valid = 0;  // channel 2 does not read
  (void)b.duty().readPower();
  const core::power::Decision d = b.duty().decide();
  CHECK(d.band == core::power::Band::kCritical);
  CHECK_FALSE(d.may.modem);
}

TEST_CASE("a wake on the line with nothing valid on it is spurious, and counted") {
  CycleBench b;
  // Noise: the zeros that wake a Walter, and then bytes that are not a frame.
  b.world.rak.sendBytes({0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, 0);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kSpurious);
  CHECK(b.wake().spurious_in_window == 1);
  CHECK(b.wake().spurious_total == 1);
  // The whole window was listened to before it gave up: §8's repeats could still have arrived.
  CHECK(b.wake().listened_ms >= kWindow);
  // One is not enough to disarm the line.
  CHECK(b.duty().lineWillArm());
  CHECK_FALSE(b.wake().line_disarmed);
}

TEST_CASE("a scanner's text is not an attention: the wake is spurious") {
  CycleBench b;
  // §9: NoiseScope speaks NDJSON lines on this same UART. A line is not a frame, and a Walter
  // woken by one has been woken for nothing.
  b.world.rak.scanner = true;
  b.world.rak.sendLine("{\"t\":\"stat\",\"rssi\":-91}", 0);
  b.world.rak.sendLine("{\"t\":\"stat\",\"rssi\":-93}", 1000);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kSpurious);
  CHECK(b.wake().spurious_in_window == 1);
}

TEST_CASE("spurious wakes disarm the line, and the next timer wake arms it again") {
  CycleBench b;
  for (std::uint8_t i = 1; i <= core::cycle::kSpuriousPerWindow; ++i) {
    b.world.rak.sendBytes({0x00, 0x00, 0x7f, 0x7f}, 0);
    b.boot(interfaces::WokeBy::kRadioLine);
    b.v().begin();
    REQUIRE(b.wake().wake == Wake::kSpurious);
    CHECK(b.wake().spurious_in_window == i);
    const bool at_the_limit = i >= core::cycle::kSpuriousPerWindow;
    CHECK(b.wake().line_disarmed == at_the_limit);
    CHECK(b.wake().line_just_disarmed == at_the_limit);
    CHECK(b.duty().lineWillArm() == !at_the_limit);
  }
  // Disarmed, and it stays disarmed over another line wake (which the chip could still deliver:
  // the pin was armed when the *last* sleep began).
  b.world.rak.sendBytes({0x00, 0x00, 0x7f}, 0);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  CHECK(b.wake().line_disarmed);
  CHECK_FALSE(b.wake().line_just_disarmed);  // it was already disarmed; this is not news
  CHECK_FALSE(b.duty().lineWillArm());

  // A timer wake arms it again, and says so.
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kTimer);
  CHECK_FALSE(b.wake().line_disarmed);
  CHECK(b.wake().line_rearmed);
  CHECK(b.duty().lineWillArm());
  CHECK(b.wake().spurious_in_window == 0);
  CHECK(b.wake().spurious_total >= core::cycle::kSpuriousPerWindow);

  // And `wake.radio off` disarms it whatever the count says.
  char err[96] = {};
  REQUIRE(b.v().settings.put(core::settings::def(core::settings::Key::kWakeRadio), "off", err,
                             sizeof err));
  CHECK_FALSE(b.duty().lineWillArm());
}

TEST_CASE("a valid attention is not chatter: it empties the spurious window") {
  CycleBench b;
  b.world.rak.sendBytes({0x00, 0x00, 0x01}, 0);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  REQUIRE(b.wake().spurious_in_window == 1);
  b.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 2);
  b.boot(interfaces::WokeBy::kRadioLine);
  b.v().begin();
  REQUIRE(b.wake().wake == Wake::kAttention);
  CHECK(b.wake().spurious_in_window == 0);
}

TEST_CASE("a power-up reset the radio with us, and a reset of ours did not") {
  // docs/ARCHITECTURE.md §2 rule 5: a power-up of the ESP32-S3 glitches GPIO1-14 low for about
  // 60 us and that reaches nRESET. The three reasons are the supervisor's own (classify()).
  CycleBench b;
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kPowerOn);
  CHECK(b.wake().radio_may_be_booting);

  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kBrownout);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kCrash);
  CHECK(b.wake().radio_may_be_booting);

  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kResetPin);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kReset);
  CHECK_FALSE(b.wake().radio_may_be_booting);
}

TEST_CASE("a crash and a watchdog are counted, and a person's reboot is not") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kCrash);
  CHECK(b.wake().faults_in_a_row == 1);

  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kWatchdog);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kWatchdog);
  CHECK(b.wake().faults_in_a_row == 2);

  // This firmware's own bound on a wake reads as a watchdog, which is what it is.
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware,
         core::health::Restart::kWakeOverrun);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kWatchdog);
  CHECK(b.wake().faults_in_a_row == 3);

  // A person's `reboot` is a software reset too, and is not a fault.
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware,
         core::health::Restart::kConsole);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kReset);
  CHECK(b.wake().faults_in_a_row == 0);
}

TEST_CASE("a boot that finds a USB host is a person - except a crash, which is still a crash") {
  CycleBench b;
  b.world.usb = true;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kUsb);
  CHECK(b.wake().usb_host);
  // The reason the chip gave is kept beside it, so nothing is lost by the relabelling.
  CHECK(b.wake().reset == core::health::ResetReason::kDeepSleepWake);
  CHECK(b.wake().woke_by == interfaces::WokeBy::kTimer);

  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic);
  b.v().begin();
  CHECK(b.wake().wake == Wake::kCrash);
  CHECK(b.wake().usb_host);
  CHECK(b.wake().faults_in_a_row == 1);
}

TEST_CASE("the wake log: the last ten, their durations, and what a power cut does to it") {
  CycleBench b;
  // Ten wakes, each sleeping for a minute: the log fills and then keeps the newest ten.
  for (int i = 0; i < 12; ++i) {
    b.boot(interfaces::WokeBy::kTimer);
    b.v().begin();
    b.world.clock.advance(5000);  // five seconds awake
    (void)b.duty().readPower();
    (void)b.duty().decide();
    const core::cycle::SleepResult r = b.duty().sleepFor(60);
    REQUIRE(r.refusal == core::cycle::SleepRefusal::kNone);
  }
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  CHECK(b.duty().reason().count() == core::cycle::kWakesRemembered);
  CHECK(b.duty().reason().recordSurvived());
  // 0 is this wake, and it has not reached sleep yet.
  CHECK(b.duty().reason().at(0).wake == Wake::kTimer);
  CHECK(b.duty().reason().at(0).lasted_s == 0);
  // The ones before it did, and lasted about the five seconds they were awake.
  for (std::size_t i = 1; i < b.duty().reason().count(); ++i) {
    const core::cycle::WakeEntry& e = b.duty().reason().at(i);
    CHECK_MESSAGE(e.wake == Wake::kTimer, i);
    CHECK_MESSAGE(e.lasted_s > 0, i);
  }
  // An index past the end is an empty entry and not a read past the array.
  CHECK(b.duty().reason().at(99).wake == Wake::kUnknown);

  // A power cut clears RTC memory: the log starts over, and says so.
  b.world.powerCut();
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  b.v().begin();
  CHECK_FALSE(b.duty().reason().recordSurvived());
  CHECK(b.duty().reason().count() == 1);
  CHECK(b.wake().wakes == 1);
  CHECK(b.wake().spurious_total == 0);
}

TEST_CASE("a wake that never reached sleep is recorded as such") {
  CycleBench b;
  b.boot(interfaces::WokeBy::kTimer);
  b.v().begin();
  b.world.clock.advance(30000);
  // No sleep: the board was reset instead (a crash, the bound, a person).
  b.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPanic);
  b.v().begin();
  REQUIRE(b.duty().reason().count() >= 2);
  CHECK(b.duty().reason().at(1).wake == Wake::kTimer);
  CHECK(b.duty().reason().at(1).lasted_s == 0);  // 0 means "it never reached sleep"
}

TEST_CASE("a wake with no supervisor cannot hear an attention, and says so by being spurious") {
  // A build with no `rakfw` or no `journal` partition hands the duty cycle no supervisor
  // (main/main.cpp), and the supervisor is the one place an EV_ATTENTION is decoded. Listening
  // to nothing for eleven seconds would be worse than useless, so the window is skipped.
  bench::CycleWorld world;
  world.woke = interfaces::WokeBy::kRadioLine;
  world.reset = core::health::ResetReason::kDeepSleepWake;
  bench::CycleWalter v(world);
  world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 10);
  core::cycle::WakeReason reason(world.retained, world.clock);
  const std::uint64_t before = world.clock.nowMs();
  reason.begin(v.health.thisBoot(), interfaces::WokeBy::kRadioLine, false, nullptr);
  CHECK(reason.info().wake == Wake::kSpurious);
  CHECK(reason.info().listened_ms == 0);
  CHECK(world.clock.nowMs() == before);
}
