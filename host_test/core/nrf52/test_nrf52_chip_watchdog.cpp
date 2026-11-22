// The simulated chip's hardware watchdog (task p18-01), against the nRF52840 Product
// Specification v1.11: this is the peripheral that ended the first NoiseScope session `stuck`,
// and these are the four of its rules that made that happen.
//
//   6.36.3  "After it is started, the watchdog's configuration registers ... will be blocked for
//           further configuration" - nothing stops it and nothing changes its timeout.
//   6.36.2  CONFIG.HALT: "Pause watchdog while the CPU is halted by the debugger" - which is
//           what the scanner sets, and why a flash of any length does not end in a reset.
//   5.3.6.8 the "Reset behavior" table: the WDT column is marked for a watchdog, a pin, a
//           brownout and a power-on reset, and **not** for a soft reset - so a watchdog counts
//           on through the boot a SYSRESETREQ begins.
//   6.36.3  "A TIMEOUT event will automatically lead to a watchdog reset."
//
// Nothing of this firmware touches the WDT (the radio's firmware starts it); the simulator has it
// so that host_test/core/update/test_scanner_session_watchdog.cpp can show the fault and its cure.
#include <doctest.h>

#include "sim/nrf52_bench.h"

namespace {
constexpr std::uint32_t kScannerTimeoutMs = 60000;  // the repeater firmware's WDT_TIMEOUT_S 60
}  // namespace

TEST_CASE("the watchdog counts down while the core runs and is fed back to its timeout") {
  sim::Nrf52Bench b;
  CHECK(!b.chip.watchdogRunning());
  b.chip.startWatchdog(kScannerTimeoutMs);
  CHECK(b.chip.watchdogRunning());
  CHECK(b.chip.watchdogLeftMs() == kScannerTimeoutMs);
  b.clock.advance(20000);
  CHECK(b.chip.watchdogLeftMs() == 40000);
  b.chip.feedWatchdog();
  CHECK(b.chip.watchdogLeftMs() == kScannerTimeoutMs);
  b.clock.advance(59999);
  CHECK(b.chip.watchdogRunning());
  CHECK(b.chip.resets().watchdog == 0);
}

TEST_CASE("a watchdog that is never fed resets the chip at its timeout, and that reset clears it") {
  sim::Nrf52Bench b;
  b.chip.startWatchdog(kScannerTimeoutMs);
  b.clock.advance(kScannerTimeoutMs);
  b.chip.serviceWatchdog();
  CHECK(b.chip.resets().watchdog == 1);
  CHECK(!b.chip.watchdogRunning());
  CHECK(b.chip.counters().resets == 1);
  // And nothing fires a second time: the watchdog is gone with the reset.
  b.clock.advance(10 * kScannerTimeoutMs);
  b.chip.serviceWatchdog();
  CHECK(b.chip.resets().watchdog == 1);
}

TEST_CASE("a started watchdog cannot be stopped or given another timeout") {
  sim::Nrf52Bench b;
  b.chip.startWatchdog(kScannerTimeoutMs);
  b.clock.advance(30000);
  b.chip.startWatchdog(1000);  // TASKS_START again, with another CRV: blocked
  CHECK(b.chip.watchdogLeftMs() == 30000);
  b.clock.advance(5000);
  CHECK(b.chip.watchdogRunning());
  CHECK(b.chip.resets().watchdog == 0);
}

TEST_CASE("CONFIG.HALT: the watchdog stands still while the debugger holds the core halted") {
  sim::Nrf52Bench b;
  REQUIRE(b.open().ok());
  b.chip.startWatchdog(kScannerTimeoutMs);
  REQUIRE(b.nrf.halt().ok());
  REQUIRE(b.chip.coreHalted());
  b.clock.advance(10 * kScannerTimeoutMs);  // a flash of ten minutes
  b.chip.serviceWatchdog();
  CHECK(b.chip.resets().watchdog == 0);
  CHECK(b.chip.watchdogLeftMs() == kScannerTimeoutMs);
  b.close();
}

TEST_CASE("a soft reset does not clear the watchdog: it counts on into the next boot (21 November)") {
  sim::Nrf52Bench b;
  REQUIRE(b.open().ok());
  b.chip.startWatchdog(kScannerTimeoutMs);
  REQUIRE(b.nrf.halt().ok());
  b.clock.advance(300000);  // the flash: five minutes with the core halted, the watchdog paused
  REQUIRE(b.nrf.systemReset().ok());
  CHECK(b.chip.resets().sysresetreq == 1);
  CHECK(b.chip.watchdogRunning());
  // All but the few milliseconds the session took before the core was halted: the five minutes
  // of flashing cost it nothing, and the soft reset cost it nothing either.
  const std::uint32_t left = b.chip.watchdogLeftMs();
  CHECK_MESSAGE(left > kScannerTimeoutMs - 100, "the watchdog has " << left << " ms left of 60 000");
  b.close();
  // The image that boot starts feeds nothing - the repeater has no watchdog of its own - so 60 s
  // later the radio restarts, which is what a proving period reads as a failure.
  b.clock.advance(kScannerTimeoutMs);
  b.chip.serviceWatchdog();
  CHECK(b.chip.resets().watchdog == 1);
  CHECK(!b.chip.watchdogRunning());
}

TEST_CASE("a pin reset clears the watchdog, and so does a power-on") {
  sim::Nrf52Bench b;
  REQUIRE(b.open().ok());
  b.chip.startWatchdog(kScannerTimeoutMs);
  REQUIRE(b.nrf.halt().ok());
  b.close();
  // The reset line, through RadioPins: the one the flasher leaves by since this task.
  REQUIRE(b.board.resetPulse() == core::board::Outcome::kOk);
  CHECK(b.chip.resets().pin == 1);
  CHECK(!b.chip.watchdogRunning());
  b.clock.advance(10 * kScannerTimeoutMs);
  b.chip.serviceWatchdog();
  CHECK(b.chip.resets().watchdog == 0);

  b.chip.startWatchdog(kScannerTimeoutMs);
  b.chip.powerOn();
  CHECK(!b.chip.watchdogRunning());
  b.clock.advance(10 * kScannerTimeoutMs);
  b.chip.serviceWatchdog();
  CHECK(b.chip.resets().watchdog == 0);
}
