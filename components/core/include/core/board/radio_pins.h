// RadioPins: the only object that touches the seven pins shared with the radio (task p0-03), and
// the one place every rule of docs/ARCHITECTURE.md §2 is enforced:
//
//   1. the safe state - nRESET, SWDCLK, SWDIO and KILL not driving, TX pulled up (not driven) while
//      the radio is believed powered and floating when it is not - at construction, whenever there
//      is nothing to say, and after every operation that fails part-way;
//   2. a reset is a pulse of output-low (open-drain) of 100 ms, then input; two are at least one
//      second apart, waited for rather than violated, across a deep sleep too; and there is no
//      way to drive the reset pin high - the only handle on it (ResetLine) has no such operation;
//   3. before the kill line goes high every other pin is a plain input (no pull-up either), and
//      after it is released the rail is given time to settle before anything drives again;
//   4. a kill is bounded below and above, and the kill line is driven high or not at all - never
//      low (it floats to "on" by hardware when the Walter resets);
//   5. the pads can be held through deep sleep in the safe state (the holds; sleeping itself is
//      phase 3's).
//
// And two rules of the task: nothing acquires SWD while the rail is believed off, and the SWD
// clock cannot toggle while reset is asserted - except in the one operation that exists to do
// exactly that, `selectorProgram()` (task p1-06): the dual adapter's selector is addressed by
// clocking bits in while nRESET is held low, and this is the only place in the firmware that may.
// A single-radio unit never calls it (nothing constructs board/dual_target.h).
//
// All of it is logic: no ESP-IDF include, no heap, and every pin operation goes through
// interfaces::Pins so that host_test/ runs it against fake pins that record every transition and
// check these rules themselves (host_test/fakes/fake_pins.h).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/pin_map.h"
#include "interfaces/clock.h"
#include "interfaces/pins.h"
#include "interfaces/retained.h"

namespace core {
namespace board {

// What this firmware believes about the LoRa 3.3 V rail. kOff while it is driving the kill line;
// kUnknown for the settle time after the kill line was last released - and for the settle time
// after boot, because a Walter that reset in the middle of a kill left the line to float back on
// just before (ARCHITECTURE §2 rule 4); kOn after that. Read by the supervisor (phase 2).
enum class RadioPower : std::uint8_t { kUnknown, kOn, kOff };

enum class Outcome : std::uint8_t {
  kOk,
  kKillTooShort,    // below KillTiming::min_ms: the rail would not have collapsed; nothing done
  kKillTooLong,     // above KillTiming::max_ms; nothing done
  kSwdHeld,         // a reset while SWD is acquired: the holder releases it first
  kSwdAlreadyHeld,  // swdAcquire() twice
  kResetAsserted,   // SWD cannot be acquired, nor its clock toggled, while nRESET is low
  kRailOff,         // nothing is enabled or acquired while the kill line is driven
  kPinRefused,      // the pin driver refused an operation; everything is back in the safe state
};

const char* describe(Outcome outcome);
const char* describe(RadioPower power);

// The reset pulse and spacing (ARCHITECTURE §2 rule 2; BENCH-BREADBOARD.md §6 caveat 4: two resets
// under about half a second apart put the Adafruit bootloader into UF2 mode, so the spacing is a
// second, from the end of one pulse to the start of the next).
constexpr std::uint32_t kResetPulseMs = 100;
constexpr std::uint32_t kResetSpacingMs = 1000;

// The dual adapter's selector, programmed by clocking two bits in on SWDIO while nRESET is held
// low (the hardware repository's DUAL-RAK4630-FEASIBILITY.md §3, at commit b798aab). The window
// numbers are that section's:
//
//   "The pulse must exceed the ~1 µs bulkhead RC, so a 100–500 µs programming window is the
//    sweet spot" - below 100 µs the series resistors and the two bulkhead filters swallow it,
//    above 500 µs it starts to approach the adapter's own ~2–5 ms RC + Schmitt buffer, which is
//    what keeps this reset away from the radios. "A real reset = Walter holds nRESET ≥ 50 ms".
//
// A half-period of 20 µs is 25 kHz: twenty times the bulkhead's time constant, and a tenth of
// the ~300 kHz the same lines carry SWD at. Two bits plus the setup and the trailing edge make a
// 120 µs window, inside the document's range with room at both ends.
constexpr std::uint32_t kSelectorHalfUs = 20;
constexpr std::uint32_t kSelectorBits = 2;
constexpr std::uint32_t kSelectorWindowMinUs = 100;
constexpr std::uint32_t kSelectorWindowMaxUs = 500;
// What selectorProgram() spends inside the window: setup + 2 bits × 2 half-periods + the trailing
// half-period before nRESET is released.
constexpr std::uint32_t kSelectorWindowUs = kSelectorHalfUs * (2 + 2 * kSelectorBits);
static_assert(kSelectorWindowUs >= kSelectorWindowMinUs && kSelectorWindowUs <= kSelectorWindowMaxUs,
              "the selector's programming window is outside DUAL-RAK4630-FEASIBILITY.md §3's 100-500 us");

// The kill's bounds and the settle time after it (ARCHITECTURE §2 rules 3 and 4). PLACEHOLDERS
// for the bench: no hardware document gives the rail's fall or settle time (the TPS63001's output
// capacitance and the RAK's two 100 µF on the rail discharge through whatever the radio draws, so
// the fall is load-dependent). One second is long enough for the rail to collapse at any plausible
// draw and short enough to be a console argument; thirty seconds bounds how long a bug can keep
// the radio off per call; 200 ms is generous for a buck-boost soft-start. docs/bench.md §2 asks for
// the measured numbers; replace them here and nowhere else.
struct KillTiming {
  std::uint32_t min_ms;
  std::uint32_t max_ms;
  std::uint32_t settle_ms;
};
constexpr KillTiming kKillTiming{1000, 30000, 200};

// What `pins` and `status` print: one line per pin.
struct PinState {
  const char* net;
  std::uint8_t dsub;
  std::uint8_t gpio;
  interfaces::PinMode mode;  // as this firmware last configured it
  bool level;                // read from the pad now
  const char* use;           // "released", "uart", "swd", "reset", "kill"
};

class RadioPins;

// The SWD clock and data lines, handed out by RadioPins::swdAcquire() and dead after
// swdRelease() or a kill: every call returns false and touches nothing unless the lines are held
// and nRESET is released. Phase 1's wire protocol is written against this.
class SwdLines {
 public:
  bool clock(bool high);    // SWDCLK; idle is low
  bool driveIo(bool high);  // SWDIO as output at this level
  bool releaseIo();         // SWDIO as input: the target's turn to drive
  bool readIo();            // SWDIO's level (false when not held)

 private:
  friend class RadioPins;
  explicit SwdLines(RadioPins& owner) : owner_(owner) {}
  RadioPins& owner_;
};

class RadioPins {
 public:
  // Puts the pins in the safe state before returning: constructing this object is the first thing
  // app_main does. The retained record (when the last reset pulse ended) is read here.
  RadioPins(interfaces::Pins& pins, interfaces::Clock& clock, interfaces::Retained& retained,
            const KillTiming& timing = kKillTiming);

  // Rule 1. Releases the UART and SWD too; what remains driven is nothing (TX is pulled up, not
  // driven, while the radio is believed powered).
  void safe();

  // Rule 2. Waits out the spacing, then output-low (open-drain) for kResetPulseMs, then input.
  Outcome resetPulse();

  // Rules 3 and 4. Releases everything, drives KILL high for off_ms, stops driving it, waits for
  // the rail to settle, and returns to the safe state - after which the UART may be enabled
  // again. The caller is blocked for off_ms plus the settle time.
  Outcome killCycle(std::uint32_t off_ms);

  // TX driven (idle high) and RX an input, for the link client (phase 1). Waits for the rail if it
  // is still settling. Refused while the kill line is driven.
  Outcome uartEnable();
  void uartRelease();

  // SWDCLK driven low and SWDIO an input, for the wire protocol (phase 1). Waits for the rail if
  // it is still settling. Refused while the kill line is driven, while nRESET is asserted, and
  // when already held.
  Outcome swdAcquire();
  void swdRelease();
  SwdLines& swd() { return swd_; }

  // The dual adapter's selector, and the one sanctioned "the clock toggles while reset is
  // asserted" in this firmware (task p1-06; DUAL-RAK4630-FEASIBILITY.md §3). With nRESET held
  // low - which is what enables the adapter's shift clock, `SWDCLK AND NOT nRESET` - two bits
  // are clocked in on SWDIO, FF2's first: `uart_sel` reaches the second flip-flop (which radio
  // the UART reaches) and `debug_sel` stays in the first (which radio the debug lines and the
  // reset reach). false = module A, the position a single-radio unit's radio sits in; the
  // flip-flops' RC clear makes that the power-on state.
  //
  // The reset this implies is filtered out by the adapter's own RC and Schmitt buffer and never
  // reaches a radio, but rule 2's spacing is applied to it all the same: it is recorded like the
  // end of a pulse and waited out before the next one, because nothing here can measure the
  // adapter's filter and a bootloader in UF2 mode is how a radio is lost. Refused while the SWD
  // lines are acquired (their holder releases first), while nRESET is already asserted, and while
  // the rail is off.
  //
  // Only board/dual_target.h calls it, and this firmware does not construct that
  // (host_test/core/board/test_slot_single.cpp holds it).
  Outcome selectorProgram(bool uart_sel, bool debug_sel);
  std::uint32_t selectorPrograms() const { return selector_programs_; }

  // Rule 5. safe(), then the seven pads held as they are, for the sleeper (task p3-04); and the
  // reverse at wake: the pads configured to the same state first, then the holds released.
  //
  // `arm_radio_line` is a sleep the radio's start bit may wake (docs/ARCHITECTURE.md §10): the
  // radio's RX pin is given the chip's weak pull-up first - so that a radio which is off,
  // resetting or being flashed cannot hold the Walter awake with a line that has drifted low -
  // and then **that one pad is left out of the holds here**, because the platform's wake
  // preparation routes it to the RTC mux, enables its input and holds it itself as part of
  // arming the source, and a hold put on before that would block those writes (ESP-IDF v5.5.5,
  // components/esp_hw_support/sleep_modes.c, `ext1_wakeup_prepare()`; and
  // docs/en/api-reference/system/sleep_modes.rst, *External Wakeup (ext1)*: "please call
  // rtc_gpio_hold_dis to disable the hold function to perform any pin re-configuration"). The
  // pad is held either way, in the same state - by us, or by the chip arming it.
  //
  // The pull-up is dropped again by safe(), which every kill calls first (rule 3), so it cannot
  // survive into a moment when the rail is off.
  bool holdForSleep(bool arm_radio_line = false);
  bool releaseHolds();

  RadioPower power();
  bool uartEnabled() const { return uart_enabled_; }
  bool swdHeld() const { return swd_held_; }
  bool resetAsserted() const { return reset_asserted_; }
  bool killDriven() const { return kill_driven_; }
  // How long resetPulse() would wait before pulsing, now. 0: at once.
  std::uint32_t msUntilResetAllowed();
  // Whether a reset pulse is on record (this boot or an earlier one), and when it ended.
  bool lastResetKnown() const { return last_reset_known_; }
  std::uint64_t lastResetEndMs() const { return last_reset_end_ms_; }

  // The radio was reset by other means than nRESET - the flasher's system reset through the
  // debug port (task p1-05). Recorded like the end of a pulse, so that the next resetPulse()
  // waits out the spacing (rule 2): the bootloader cannot tell our reset from a tap, and two
  // inside half a second would put it into UF2 mode.
  void noteRadioReset();

  static constexpr std::size_t kPinCount = kRadioPinCount;
  PinState state(std::size_t index);

  const KillTiming& timing() const { return timing_; }

 private:
  friend class SwdLines;

  // The one handle on nRESET. It can assert (open-drain low) and release (input) and nothing
  // else: there is no member that drives the pin high, and nothing else in RadioPins holds the
  // reset pin's number (ARCHITECTURE §2 rule 2: "never output-high").
  class ResetLine {
   public:
    ResetLine(interfaces::Pins& pins, std::uint8_t gpio) : pins_(pins), gpio_(gpio) {}
    bool assertLow();
    bool release();
    std::uint8_t gpio() const { return gpio_; }

   private:
    interfaces::Pins& pins_;
    const std::uint8_t gpio_;
  };

  struct Record {  // retained across resets and deep sleep: interfaces::RetainedSlot::kResetRecord
    std::uint64_t last_reset_end_ms;
  };

  bool input(std::uint8_t gpio);
  bool applyTx();
  Outcome failed();
  void waitForRail();
  void recordResetEnd();

  interfaces::Pins& pins_;
  interfaces::Clock& clock_;
  interfaces::Retained& retained_;
  const KillTiming timing_;
  ResetLine reset_;
  SwdLines swd_;

  bool uart_enabled_ = false;
  bool swd_held_ = false;
  bool swdio_driven_ = false;
  bool reset_asserted_ = false;
  bool kill_driven_ = false;
  bool last_reset_known_ = false;
  std::uint32_t selector_programs_ = 0;
  std::uint64_t last_reset_end_ms_ = 0;
  std::uint64_t rail_on_at_ms_ = 0;  // the rail is believed on from this time
};

}  // namespace board
}  // namespace core
