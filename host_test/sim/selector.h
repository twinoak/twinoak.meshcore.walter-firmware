// The dual adapter's target selector, simulated (task p1-06): the two flip-flops as the hardware
// repository's `hardware/pcb/DUAL-RAK4630-FEASIBILITY.md` §3 draws them, at commit
// b798aab043281fa4c9b17802a164fa94be12e206. It is only as true as that document; every rule below
// is a quotation from it.
//
//                         ┌──────────────────────────────┐
//    nRESET (pin 3) ──┬──►│ 1G97: CLK_EN = ~nRESET       │
//    SWDCLK (pin 4) ──┼──►│ SHIFT_CLK = SWDCLK & ~nRESET │
//                     │   └──────────────┬───────────────┘
//                     │                  ▼
//    SWDIO (pin 5) ─── D ──►[FF1]──►[FF2]        RC on CLR: power-on = 0,0 → module A
//                            │ Q=DEBUG_SEL │ Q=UART_SEL
//                     └──►[RC ~2-5 ms + Schmitt]──► nRESET to the *selected* module
//
//   - the shift clock is `SWDCLK AND NOT nRESET`, so nothing shifts while nRESET is released;
//   - a rising shift clock samples SWDIO into FF1 and FF1's old Q into FF2, so the first bit the
//     Walter clocks in ends up in FF2 (UART_SEL) and the second in FF1 (DEBUG_SEL);
//   - DEBUG_SEL steers SWDIO, SWDCLK *and* nRESET; UART_SEL steers the UART's two lines;
//   - the reset toward the modules passes an RC of about 2-5 ms and a Schmitt buffer, so the
//     programming window (100-500 µs) "is filtered out entirely" and "flipping the UART from A to
//     B does not reboot anything"; a real reset is nRESET held for 50 ms or more;
//   - the power-on state is 0,0 - module A - "so a fresh unit behaves exactly like the single-RAK
//     design until told otherwise";
//   - during the programming window "the 2-3 SWDCLK pulses do reach the previously-selected
//     module's SWD pins, but a valid SWD transaction needs a full 8-bit header": this object
//     passes them on, to the module selected at that instant, and counts them.
//
// It stands between RadioPins and the fake pins as an interfaces::Pins of its own, exactly as
// task p1-03's SwdTarget does, and hands the SWD lines on to whichever of two SwdTargets
// DEBUG_SEL points at - the other one's pins "float safely" and it sees nothing. The UART's two
// radios are behind UartMux below, which is the analog mux of §2.3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "core/board/pin_map.h"
#include "core/board/slot.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "interfaces/byte_stream.h"
#include "interfaces/pins.h"
#include "sim/scripted_radio.h"
#include "sim/swd_target.h"

namespace sim {

// The adapter's RC and Schmitt buffer on the reset toward the modules (§3: "~2-5 ms"). An
// assertion shorter than this reaches no radio. The low end of the document's range is the strict
// reading: the shortest filter is the one most likely to pass something on.
constexpr std::uint64_t kSelectorResetFilterUs = 2000;

class Selector final : public interfaces::Pins {
 public:
  Selector(fakes::FakePins& pins, fakes::FakeClock& clock);

  // interfaces::Pins. The SWD lines go to the selected module's SwdTarget (which forwards them to
  // the fake pins and watches them, as it always did); everything else goes straight to the fake
  // pins, which check the wire rules underneath as before.
  bool setMode(std::uint8_t gpio, interfaces::PinMode mode) override;
  bool write(std::uint8_t gpio, bool high) override;
  bool read(std::uint8_t gpio) override;
  bool holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) override;

  // The module behind each position. Without one, the SWD lines go to the fake pins directly.
  void attach(core::board::Slot slot, SwdTarget* target);

  // ---- the two flip-flops --------------------------------------------------------------------
  bool debugSel() const { return debug_sel_; }  // FF1's Q: SWDIO, SWDCLK and nRESET
  bool uartSel() const { return uart_sel_; }    // FF2's Q: the UART's two lines
  core::board::Slot debugSlot() const { return debug_sel_ ? core::board::kSlot1 : core::board::kSlot0; }
  core::board::Slot uartSlot() const { return uart_sel_ ? core::board::kSlot1 : core::board::kSlot0; }

  // ---- what a test reads ---------------------------------------------------------------------
  unsigned shifts() const { return shifts_; }            // rising shift clocks seen in all
  unsigned windows() const { return windows_; }           // assertions of nRESET filtered out
  unsigned resets(core::board::Slot slot) const { return resets_[slot.index & 1]; }
  unsigned resetsDelivered() const { return resets_[0] + resets_[1]; }
  std::uint64_t lastResetUs() const { return last_reset_us_; }
  // Told when a real reset pulse reaches that module, after the adapter's filter.
  std::function<void()> on_reset[2];

  // "a couple of stray clocks cannot form" a header: true (the default) passes the programming
  // window's edges on to the module selected at that instant, as the hardware does.
  bool forward_stray_clocks = true;

 private:
  bool isSwd(std::uint8_t gpio) const {
    return gpio == core::board::kSwdClk.gpio || gpio == core::board::kSwdIo.gpio;
  }
  interfaces::Pins& swdPath();
  void observe();

  fakes::FakePins& pins_;
  fakes::FakeClock& clock_;
  SwdTarget* targets_[2] = {nullptr, nullptr};

  bool debug_sel_ = false;  // the RC clear: module A
  bool uart_sel_ = false;
  bool shift_was_ = false;
  bool reset_low_was_ = false;
  std::uint64_t reset_start_us_ = 0;
  std::uint64_t last_reset_us_ = 0;
  unsigned shifts_ = 0;
  unsigned windows_ = 0;
  unsigned resets_[2] = {0, 0};
};

// §2.3's analog mux on the UART: "Walter's TX is routed to one module's RX, and that module's TX
// is routed back. The unselected module's RX is pulled up to idle-high; its TX simply goes
// nowhere." open() and close() are the Walter's own peripheral and reach both; everything else
// goes to the module UART_SEL points at, and whatever the other one emitted is dropped, because
// that is where its TX goes.
class UartMux final : public interfaces::ByteStream {
 public:
  UartMux(const Selector& selector, ScriptedRadio& a, ScriptedRadio& b)
      : selector_(selector), radios_{&a, &b} {}

  bool open() override;
  void close() override;
  std::size_t read(std::uint8_t* out, std::size_t max) override;
  std::size_t write(const std::uint8_t* data, std::size_t size) override;
  bool flush(std::uint32_t timeout_ms) override;
  bool idleFor(std::uint32_t ms) override;

  ScriptedRadio& selected() { return *radios_[selector_.uartSel() ? 1 : 0]; }
  std::uint32_t droppedBytes() const { return dropped_; }

 private:
  const Selector& selector_;
  ScriptedRadio* radios_[2];
  std::uint32_t dropped_ = 0;
};

}  // namespace sim
