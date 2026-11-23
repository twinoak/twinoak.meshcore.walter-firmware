// What the chip says about its reset pin, and what a pulse does to a chip that has none (task
// p18-04). UICR.PSELRESET[0] and [1] are the two registers a pin reset is configured by, and this
// firmware **reads** them and writes nothing: a module without the pin reset enabled has no
// nRESET whatever the wire does, and enabling one is a person's act of provisioning.
//
// From the Product Specification (v1.11, 4.5.1.62 and 4.5.1.63, "Mapping of the nRESET
// function"), which is what the reader is written from:
//
//   PIN     bits [4:0]  "GPIO pin number onto which nRESET is exposed"
//   PORT    bit 5       "Port number onto which nRESET is exposed"
//   CONNECT bit 31      "Connected 0 Connect, Disconnected 1 Disconnect"
//   reset value 0xFFFFFFFF, which is Disconnected
//
// and the rule that makes the two one answer: "All PSELRESET registers have to contain the same
// value for a pin mapping to be valid. If values are not the same, there will be no nRESET
// function exposed on a GPIO. As a result, the device will always start independently of the
// levels present on any of the GPIOs" (5.3.6.2: "Pin reset is configured via both registers
// PSELRESET[n]").
#include <doctest.h>

#include "sim/nrf52_bench.h"

namespace regs = core::nrf52;
using core::nrf52::ResetPin;

TEST_CASE("UICR.PSELRESET read: a provisioned RAK4630 has nRESET on P0.18") {
  sim::Nrf52Bench b;
  REQUIRE(b.open().ok());
  ResetPin pin;
  REQUIRE(b.nrf.readResetPin(&pin).ok());
  CHECK(pin.known);
  CHECK(pin.enabled);
  CHECK(pin.pin == 18);
  CHECK(pin.port == 0);
  CHECK(pin.raw0 == 18);
  CHECK(pin.raw1 == 18);
  // Read, and nothing else: no UICR word was written and the tripwire did not move.
  CHECK(b.chip.counters().uicr_writes == 0);
  CHECK(b.chip.counters().tripwire == 0);
  MESSAGE("PSELRESET reads " << pin.raw0 << " and " << pin.raw1 << ": nRESET on P"
                             << static_cast<unsigned>(pin.port) << "."
                             << static_cast<unsigned>(pin.pin));
  b.close();
}

TEST_CASE("UICR.PSELRESET read: an erased pair is Disconnected, and that is no pin reset") {
  sim::Nrf52Bench b;
  b.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kNotEnabled);
  REQUIRE(b.open().ok());
  ResetPin pin;
  REQUIRE(b.nrf.readResetPin(&pin).ok());
  CHECK(pin.known);
  CHECK_FALSE(pin.enabled);
  CHECK(pin.raw0 == 0xFFFFFFFFu);
  CHECK(pin.raw1 == 0xFFFFFFFFu);
  CHECK((pin.raw0 & regs::uicr::kPselResetConnectBit) != 0);  // Disconnected
  CHECK(b.chip.counters().uicr_writes == 0);
  b.close();
}

TEST_CASE("UICR.PSELRESET read: two registers that do not agree are no pin reset either") {
  sim::Nrf52Bench b;
  // The document's own case: "If values are not the same, there will be no nRESET function
  // exposed on a GPIO." Written straight into the simulated UICR, which is a thing a test may do
  // and this firmware may not.
  b.chip.setUicrWord(regs::uicr::kPselReset0, 18);
  b.chip.setUicrWord(regs::uicr::kPselReset1, 21);
  REQUIRE(b.open().ok());
  ResetPin pin;
  REQUIRE(b.nrf.readResetPin(&pin).ok());
  CHECK(pin.known);
  CHECK_FALSE(pin.enabled);
  CHECK(pin.raw0 == 18);
  CHECK(pin.raw1 == 21);
  b.close();
}

TEST_CASE("UICR.PSELRESET read: the port is bit 5 and the pin is bits [4:0]") {
  sim::Nrf52Bench b;
  b.chip.setUicrWord(regs::uicr::kPselReset0, regs::uicr::kPselResetPortBit | 3u);
  b.chip.setUicrWord(regs::uicr::kPselReset1, regs::uicr::kPselResetPortBit | 3u);
  REQUIRE(b.open().ok());
  ResetPin pin;
  REQUIRE(b.nrf.readResetPin(&pin).ok());
  CHECK(pin.enabled);
  CHECK(pin.pin == 3);
  CHECK(pin.port == 1);
  b.close();
}

TEST_CASE("UICR.PSELRESET: a closed port answers nothing, and `known` stays false") {
  sim::Nrf52Bench b;
  b.chip.approtect = true;  // a protected module: memory access is never attempted
  CHECK_FALSE(b.open().ok());
  ResetPin pin;
  const core::nrf52::Result r = b.nrf.readResetPin(&pin);
  CHECK_FALSE(r.ok());
  CHECK_FALSE(pin.known);
  CHECK(pin.raw0 == 0);
  b.close();
}

TEST_CASE("a pulse on a line that reaches nothing leaves the chip exactly as it was") {
  sim::Nrf52Bench b;
  b.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kEnabledNotWired);
  REQUIRE(b.open().ok());
  REQUIRE(b.nrf.halt().ok());
  CHECK(b.chip.coreHalted());
  const unsigned resets = b.chip.counters().resets;

  // The wire that is not there. UICR still says the pin reset is enabled - this is the unit at
  // Bellevue as 21 and 22 November read it - so the firmware would pulse, and the chip never
  // hears it.
  ResetPin pin;
  REQUIRE(b.nrf.readResetPin(&pin).ok());
  CHECK(pin.enabled);
  b.chip.pinReset();
  CHECK(b.chip.resets().pin == 0);
  CHECK(b.chip.resets().pin_lost == 1);
  CHECK(b.chip.counters().resets == resets);
  CHECK(b.chip.coreHalted());  // still halted where the flash left it: the look's whole subject

  // And a module with no reset pin at all is the same, with UICR saying so.
  sim::Nrf52Bench none;
  none.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kNotEnabled);
  REQUIRE(none.open().ok());
  REQUIRE(none.nrf.halt().ok());
  none.chip.pinReset();
  CHECK(none.chip.resets().pin == 0);
  CHECK(none.chip.resets().pin_lost == 1);
  CHECK(none.chip.coreHalted());

  // A wired one, for the contrast: the pulse is a pin reset, and the core runs again.
  sim::Nrf52Bench wired;
  REQUIRE(wired.open().ok());
  REQUIRE(wired.nrf.halt().ok());
  wired.chip.pinReset();
  CHECK(wired.chip.resets().pin == 1);
  CHECK(wired.chip.resets().pin_lost == 0);
  CHECK_FALSE(wired.chip.coreHalted());

  // The two whose pulse was lost still have a debug port to close; the wired one's pin reset took
  // the debug port with it (PS "Reset behavior": a pin reset resets the debug components), so
  // there is nothing left there to say anything to - the lines are simply let go.
  b.close();
  none.close();
  wired.release();
}

TEST_CASE("the debug port starts a halted core where the line cannot: SYSRESETREQ, and it runs") {
  sim::Nrf52Bench b;
  b.chip.resetPinIs(sim::Nrf52Chip::ResetPinIs::kNotEnabled);
  REQUIRE(b.open().ok());
  REQUIRE(b.nrf.halt().ok());
  CHECK(b.chip.coreHalted());
  REQUIRE(b.nrf.systemReset().ok());
  CHECK_FALSE(b.chip.coreHalted());
  CHECK(b.chip.counters().aircr_resets == 1);
  CHECK(b.chip.resets().pin == 0);
  // And what that costs, which is the whole of task p18-01's finding: a soft reset does not clear
  // a watchdog the image in flash started (PS 5.3.6.8's "Reset behavior" table).
  b.chip.startWatchdog(60000);
  CHECK(b.chip.watchdogRunning());
  REQUIRE(b.nrf.halt().ok());
  REQUIRE(b.nrf.systemReset().ok());
  CHECK(b.chip.watchdogRunning());
  b.close();
}
