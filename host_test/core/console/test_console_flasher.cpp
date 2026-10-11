// The console's `rak flash-test` against the simulated RAK4630 (task p1-05): the pattern written
// at the top of the region, verified, erased again; the application and the settings page
// untouched; refused when the pages are not blank, on a protected chip, with nothing answering;
// the pins released on every exit path; the help and the parser.
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/console/parser.h"
#include "core/flasher/flasher.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/nrf52_chip.h"
#include "sim/scripted_radio.h"
#include "sim/swd_target.h"

using core::board::kKillTiming;
using core::board::kSwdClk;
using core::board::kSwdIo;
using core::board::RadioPins;
using core::console::Console;
using core::health::Health;
using core::health::ResetReason;
using core::health::WakeCause;
using interfaces::PinMode;

namespace {

constexpr std::uint32_t kTop = 0xED000 - 2 * 4096;  // the pattern's two pages

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  sim::SwdTarget target{pins};
  sim::Nrf52Chip chip{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  RadioPins radio{target, clock, retained};
  Health health{retained, clock};
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  // The slot seam (task p1-06): a single-radio unit, as the firmware builds it.
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, clock, wall, slots};
  Console console{out, radio, health, clock, restart, link, slots};

  Rig() {
    chip.attachTo(target);
    chip.on_power_loss = [this] { target.silent = true; };
    chip.installSoftDevice();
    pins.record_log = false;
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(kKillTiming.settle_ms);
    radio.safe();
    // An application and its settings, which the command must leave alone.
    chip.setFlashWord(0x26000, 0x20040000);
    chip.setFlashWord(0x26004, 0x00026101);
    chip.setFlashWord(0xFF000, 0x00000001);
  }

  std::string run(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }

  void checkReleased() {
    CHECK(!radio.swdHeld());
    CHECK(pins.modeOf(kSwdClk.gpio) == PinMode::kInput);
    CHECK(pins.modeOf(kSwdIo.gpio) == PinMode::kInput);
    CHECK((target.ctrlStat() & core::swd::ctrl_stat::kPowerUpRequests) == 0);
    CHECK(target.errors().empty());
    CHECK(chip.errors().empty());
    CHECK(pins.violations.empty());
  }
  void checkApplicationUntouched() {
    CHECK(chip.flashWord(0x26000) == 0x20040000);
    CHECK(chip.flashWord(0x26004) == 0x00026101);
    CHECK(chip.flashWord(0xFF000) == 0x00000001);
    CHECK(chip.logInside(kTop, 0xED000));
  }
};

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("rak flash-test on blank top pages: written, verified, erased again, the radio reset, nothing else touched") {
  Rig rig;
  const std::string text = rig.run("rak flash-test");
  CHECK(contains(text, "flash-test: slot 0, rak4630-single, two pages at 0x000eb000..0x000ed000"));
  CHECK(contains(text, "reading the pages the image covers: page 1/2"));
  CHECK(contains(text, "writing and verifying the first page: page 2/2"));
  CHECK(contains(text, "reading the whole image back: page 2/2"));
  CHECK(contains(text, "rak flash-test: done in "));
  CHECK(contains(text, "2 pages (0 skipped, 2 written in place, 0 erased, 0 retries), 2 erases"));
  CHECK(contains(text, "the radio was reset through the debug port; the next reset pulse waits out the spacing"));
  CHECK(contains(text, "pins released"));
  CHECK(!contains(text, "error:"));
  // The two pages are blank again; the pattern was there in between (two erases in the log).
  for (std::uint32_t a = kTop; a < 0xED000; a += 4) REQUIRE(rig.chip.flashWord(a) == 0xFFFFFFFF);
  CHECK(rig.chip.countLog(sim::FlashEvent::Kind::kErasePage) == 2);
  CHECK(rig.chip.countLog(sim::FlashEvent::Kind::kWrite) == 2 * 1024);
  CHECK(rig.chip.counters().aircr_resets == 1);
  CHECK(!rig.chip.coreHalted());
  CHECK(rig.radio.msUntilResetAllowed() == core::board::kResetSpacingMs);
  rig.checkApplicationUntouched();
  rig.checkReleased();
  // Again: the same.
  CHECK(contains(rig.run("rak flash-test"), "rak flash-test: done"));
  rig.checkApplicationUntouched();
  rig.checkReleased();
}

TEST_CASE("rak flash-test refuses when the pages are not blank, with nothing written and the core never halted") {
  Rig rig;
  rig.chip.setFlashWord(0xECFFC, 0x12345678);
  const std::string text = rig.run("rak flash-test");
  CHECK(contains(text, "error: rak flash-test: refused while checking the image against the region: the last two pages of the application region are not blank: refused (0x000ecffc)"));
  CHECK(rig.chip.countLog(sim::FlashEvent::Kind::kErasePage) == 0);
  CHECK(rig.chip.countLog(sim::FlashEvent::Kind::kWrite) == 0);
  CHECK(rig.chip.countLog(sim::FlashEvent::Kind::kConfig) == 0);
  CHECK(rig.chip.counters().dhcsr_writes == 0);
  CHECK(rig.chip.counters().aircr_resets == 0);
  CHECK(rig.chip.flashWord(0xECFFC) == 0x12345678);
  rig.checkReleased();
}

TEST_CASE("rak flash-test on a protected chip, on another part, with nothing answering: refused and released") {
  Rig rig;
  rig.chip.approtect = true;
  std::string text = rig.run("rak flash-test");
  CHECK(contains(text, "error: rak flash-test: protected: access-port protection is on, memory access not attempted - failed while the chip is protected"));
  CHECK(rig.chip.counters().protected_accesses == 0);
  rig.checkReleased();

  Rig other;
  other.chip.info_part = 0x52833;
  text = other.run("rak flash-test");
  CHECK(contains(text, "error: rak flash-test: not an nRF52840 with 4096-byte pages: refused - failed while identifying the chip"));
  CHECK(other.chip.log().empty());
  other.checkReleased();

  Rig silent;
  silent.target.silent = true;
  text = silent.run("rak flash-test");
  CHECK(contains(text, "error: rak flash-test: no target: nothing answered on the line - failed while connecting to the debug port"));
  CHECK(contains(text, "pins released"));
  CHECK(!silent.radio.swdHeld());
}

TEST_CASE("rak flash-test is refused while the rail is off, with no pin touched") {
  Rig rig;
  std::string text;
  bool done = false;
  rig.clock.on_delay = [&](std::uint32_t) {
    if (done || !rig.radio.killDriven()) return;
    done = true;
    text = rig.run("rak flash-test");
  };
  rig.run("rak kill 1");
  CHECK(done);
  CHECK(contains(text, "error: rak flash-test: refused while acquiring the SWD lines: refused: the radio's rail is off"));
  CHECK(rig.target.counters().clocks == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("help lists rak flash-test; the parser takes no argument with it") {
  Rig rig;
  const std::string text = rig.run("help");
  CHECK(contains(text, "rak flash-test      write, verify and erase a 2-page pattern at the top of the application"));
  CHECK(core::console::parseLine("rak flash-test").ok);
  CHECK(core::console::parseLine("rak flash-test").command.verb == core::console::Verb::kRakFlashTest);
  const core::console::Parsed p = core::console::parseLine("rak flash-test now");
  CHECK(!p.ok);
  CHECK(std::string(p.error) == "rak flash-test takes no argument");
  CHECK(!core::console::parseLine("rak flash").ok);
  CHECK(contains(rig.run("rak flash"), "error: unknown rak subcommand (try 'help'): flash"));
}
