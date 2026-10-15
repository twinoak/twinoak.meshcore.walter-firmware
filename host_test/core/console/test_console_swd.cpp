// The console's `rak swd connect` against the simulated target (task p1-03): what it prints, and
// that the pins are released on every exit path - the happy one, no target, a pin the driver
// refuses - with the fake pins' rule checks underneath.
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/swd/dp.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
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

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  sim::SwdTarget target{pins};
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
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(kKillTiming.settle_ms);
    radio.safe();
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
    CHECK(!pins.driving(kSwdClk.gpio));
    CHECK(!pins.driving(kSwdIo.gpio));
  }
};

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("rak swd connect: DPIDR, the power-up, the request withdrawn, the pins released") {
  Rig rig;
  const std::string text = rig.run("rak swd connect");
  CHECK(contains(text, "swd: 125 kHz (half-period 4 us)"));
  CHECK(contains(text, "DPIDR 0x2ba01477: revision 2, part 0xba, DPv1, designer 0x23b (Arm) - the nRF52840's"));
  CHECK(contains(text, "power-up: debug and system domains acknowledged"));
  CHECK(contains(text, "power-up request withdrawn: the radio leaves debug interface mode"));
  CHECK(contains(text, "pins released"));
  CHECK(!contains(text, "error:"));
  rig.checkReleased();
  CHECK((rig.target.ctrlStat() & core::swd::ctrl_stat::kPowerUpRequests) == 0);
  CHECK(rig.target.errors().empty());
  CHECK(rig.pins.violations.empty());
  // And again: the second connect is as clean as the first.
  const std::string again = rig.run("rak swd connect");
  CHECK(contains(again, "DPIDR 0x2ba01477"));
  rig.checkReleased();
  CHECK(rig.target.errors().empty());
}

TEST_CASE("rak swd connect with nothing answering: no target, and the pins released") {
  Rig rig;
  rig.target.silent = true;
  const std::string text = rig.run("rak swd connect");
  CHECK(contains(text, "error: rak swd connect: no target: nothing answered on the line"));
  CHECK(contains(text, "pins released"));
  rig.checkReleased();
  CHECK(rig.target.errors().empty());
}

TEST_CASE("rak swd connect with a slow or absent power-up still says what it saw and releases") {
  Rig rig;
  rig.target.powers_up = false;
  const std::string text = rig.run("rak swd connect");
  CHECK(contains(text, "DPIDR 0x2ba01477"));
  CHECK(contains(text, "power-up: the power-up was not acknowledged in time (CTRL/STAT 0x50000000)"));
  CHECK(contains(text, "pins released"));
  rig.checkReleased();
}

TEST_CASE("rak swd connect when the pin driver refuses: the error, and the safe state") {
  Rig rig;
  rig.pins.fail_when = [](const fakes::Op& op) {
    return op.gpio == kSwdClk.gpio && op.kind == fakes::OpKind::kMode && op.mode == PinMode::kOutput;
  };
  const std::string text = rig.run("rak swd connect");
  CHECK(contains(text, "error: rak swd connect: the pin driver refused; pins are back in the safe state"));
  rig.checkReleased();
  CHECK(rig.target.counters().clocks == 0);
}

TEST_CASE("rak swd connect is refused while the rail is off, with no pin touched") {
  Rig rig;
  // Inside a kill the rail is off; the kill is synchronous, so act in the middle of its delay.
  std::string text;
  bool done = false;
  rig.clock.on_delay = [&](std::uint32_t) {
    if (done || !rig.radio.killDriven()) return;
    done = true;
    text = rig.run("rak swd connect");
  };
  rig.run("rak kill 1");
  CHECK(done);
  CHECK(contains(text, "error: rak swd connect: refused: the radio's rail is off"));
  rig.checkReleased();
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("a different DPIDR is printed as what it is") {
  Rig rig;
  rig.target.dpidr = 0x0BC11477;  // a Cortex-M0+ SW-DP, say
  const std::string text = rig.run("rak swd connect");
  CHECK(contains(text, "DPIDR 0x0bc11477: revision 0, part 0xbc, DPv1, designer 0x23b (Arm) - not the nRF52840's 0x2ba01477"));
  rig.checkReleased();
}

TEST_CASE("help lists rak swd connect") {
  Rig rig;
  const std::string text = rig.run("help");
  CHECK(contains(text, "rak swd connect"));
  CHECK(contains(text, "connect to the radio's debug port: DPIDR and the power-up, then the lines"));
}
