// The console's `rak swd id` and `rak swd mem` against the simulated nRF52840 (task p1-04): what
// they print on an open chip, a protected one, another part; a memory read of FICR and of
// nothing; the pins released and the power-up withdrawn on every exit path.
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/nrf52/nrf52.h"
#include "core/swd/dp.h"
#include "core/swd/wire.h"
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
    CHECK((target.ctrlStat() & core::swd::ctrl_stat::kPowerUpRequests) == 0);
    CHECK(target.errors().empty());
    CHECK(chip.errors().empty());
    CHECK(pins.violations.empty());
  }
};

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("rak swd id on the simulated chip: ports, part, variant, flash, device id, protection open, core running") {
  Rig rig;
  const std::string text = rig.run("rak swd id");
  CHECK(contains(text, "swd: 125 kHz: connect, power-up, control port, memory port, FICR"));
  CHECK(contains(text, "DPIDR 0x2ba01477 (the nRF52840's)"));
  CHECK(contains(text, "control access port: IDR 0x02880000 (Nordic); protection: open"));
  CHECK(contains(text, "memory access port: IDR 0x24770011 (Arm, class MEM-AP; the Cortex-M4's AHB-AP)"));
  CHECK(contains(text, "chip: nRF52840, variant AAF0, flash 1024 KB = 256 pages of 4096 bytes (INFO.FLASH 1024 KB)"));
  CHECK(contains(text, "device id 0badf00ddeadbeef"));
  CHECK(contains(text, "core: running"));
  CHECK(contains(text, "power-up request withdrawn; pins released;"));
  CHECK(contains(text, "0 WAIT retries, 0 faults, 0 parity errors"));
  CHECK(!contains(text, "error:"));
  rig.checkReleased();
  // Read-only: the chip's flash controller and debug registers were not written.
  CHECK(rig.chip.log().empty());
  CHECK(rig.chip.counters().dhcsr_writes == 0);
  CHECK(!rig.chip.coreHalted());
  // Twice is as clean as once.
  CHECK(contains(rig.run("rak swd id"), "core: running"));
  rig.checkReleased();
}

TEST_CASE("rak swd id on a protected chip: says so, says whose job it is, attempts no memory access") {
  Rig rig;
  rig.chip.approtect = true;
  const std::string text = rig.run("rak swd id");
  CHECK(contains(text, "control access port: IDR 0x02880000 (Nordic); protection: PROTECTED - memory access not attempted"));
  CHECK(contains(text, "bench procedure for a person (hardware repository, BENCH-BREADBOARD.md caveat 6)"));
  CHECK(contains(text, "error: rak swd id: protected: access-port protection is on, memory access not attempted"));
  CHECK(!contains(text, "memory access port:"));
  CHECK(!contains(text, "chip:"));
  CHECK(rig.chip.counters().protected_accesses == 0);
  rig.checkReleased();
}

TEST_CASE("rak swd id on another part: reported, and refused") {
  Rig rig;
  rig.chip.info_part = 0x52833;
  rig.chip.info_variant = 0x41414230;
  const std::string text = rig.run("rak swd id");
  CHECK(contains(text, "chip: part 0x52833, variant AAB0, 256 pages of 4096 bytes - not the nRF52840 with 4096-byte pages this firmware knows: refused"));
  CHECK(contains(text, "error: rak swd id: not an nRF52840 with 4096-byte pages: refused"));
  CHECK(!contains(text, "core:"));
  rig.checkReleased();
}

TEST_CASE("rak swd id with a halted core says halted") {
  Rig rig;
  // Halt it the way a debugger would have left it - a session of our own on the same lines,
  // closed before the console's command.
  {
    REQUIRE(rig.radio.swdAcquire() == core::board::Outcome::kOk);
    core::swd::Wire wire(rig.radio.swd(), rig.clock);
    core::swd::Dp dp(wire);
    core::nrf52::Nrf52 nrf(dp, rig.clock);
    REQUIRE(nrf.open().ok());
    REQUIRE(nrf.halt().ok());
    REQUIRE(nrf.close().ok());
    rig.radio.swdRelease();
  }
  CHECK(rig.chip.coreHalted());
  const std::string text = rig.run("rak swd id");
  CHECK(contains(text, "core: halted"));
  CHECK(rig.chip.coreHalted());  // id does not touch the core
  rig.checkReleased();
}

TEST_CASE("rak swd id with nothing answering says no target, and releases") {
  Rig rig;
  rig.target.silent = true;
  const std::string text = rig.run("rak swd id");
  CHECK(contains(text, "error: rak swd id: no target: nothing answered on the line"));
  CHECK(contains(text, "pins released"));
  CHECK(!rig.radio.swdHeld());
}

TEST_CASE("rak swd mem reads FICR: the part and the variant, four words a line") {
  Rig rig;
  std::string text = rig.run("rak swd mem 0x10000100 2");
  CHECK(contains(text, "0x10000100: 00052840 41414630\n"));
  CHECK(!contains(text, "error:"));
  rig.checkReleased();
  text = rig.run("rak swd mem 0x10000010 6");
  CHECK(contains(text, "0x10000010: 00001000 00000100 ffffffff ffffffff\n0x10000020: ffffffff ffffffff\n"));
  // The application's vector table, blank on this chip.
  text = rig.run("rak swd mem 0x00026000 4");
  CHECK(contains(text, "0x00026000: ffffffff ffffffff ffffffff ffffffff\n"));
  // Decimal, one word, the default.
  text = rig.run("rak swd mem 268435712");
  CHECK(contains(text, "0x10000100: 00052840\n"));
  CHECK(rig.chip.log().empty());
  rig.checkReleased();
}

TEST_CASE("rak swd mem of nothing: the fault and its address; of UICR: read, never written") {
  Rig rig;
  std::string text = rig.run("rak swd mem 0x50000000 1");
  CHECK(contains(text, "error: rak swd mem: FAULT: a sticky error flag was set (read, then cleared) at 0x50000000"));
  rig.chip.clearErrors();  // the simulator records the read of nothing
  rig.checkReleased();
  text = rig.run("rak swd mem 0x10001208 1");
  CHECK(contains(text, "0x10001208: ffffff5a\n"));
  CHECK(rig.chip.counters().tripwire == 0);
  rig.checkReleased();
}

TEST_CASE("rak swd mem on a protected chip is refused, and on another part too") {
  Rig rig;
  rig.chip.approtect = true;
  CHECK(contains(rig.run("rak swd mem 0x10000100 2"), "error: rak swd mem: protected: access-port protection is on, memory access not attempted"));
  CHECK(rig.chip.counters().protected_accesses == 0);
  rig.checkReleased();
  Rig other;
  other.chip.info_part = 0x52833;
  CHECK(contains(other.run("rak swd mem 0x10000100 2"), "error: rak swd mem: not an nRF52840 with 4096-byte pages: refused"));
  other.checkReleased();
}

TEST_CASE("rak swd id and mem are refused while the rail is off, with no pin touched") {
  Rig rig;
  std::string text;
  bool done = false;
  rig.clock.on_delay = [&](std::uint32_t) {
    if (done || !rig.radio.killDriven()) return;
    done = true;
    text = rig.run("rak swd id");
    text += rig.run("rak swd mem 0x10000100");
  };
  rig.run("rak kill 1");
  CHECK(done);
  CHECK(contains(text, "error: rak swd id: refused: the radio's rail is off"));
  CHECK(contains(text, "error: rak swd mem: refused: the radio's rail is off"));
  CHECK(rig.target.counters().clocks == 0);
  CHECK(rig.pins.violations.empty());
}

TEST_CASE("help lists the two commands") {
  Rig rig;
  const std::string text = rig.run("help");
  CHECK(contains(text, "rak swd id          identify the radio's chip over SWD: part, variant, flash, protection, core"));
  CHECK(contains(text, "rak swd mem <addr> [words]"));
}
