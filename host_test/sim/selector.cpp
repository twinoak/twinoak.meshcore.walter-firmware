#include "sim/selector.h"

#include <doctest.h>

namespace sim {

using core::board::kNReset;
using core::board::kSwdClk;
using core::board::kSwdIo;
using interfaces::PinMode;

Selector::Selector(fakes::FakePins& pins, fakes::FakeClock& clock) : pins_(pins), clock_(clock) {
  // A selector on these wires: the fake pins stop calling a clock edge during a reset a broken
  // rule and start checking the programming window's length instead (fakes/fake_pins.h).
  pins_.selector_allowed = true;
}

void Selector::attach(core::board::Slot slot, SwdTarget* target) {
  REQUIRE(core::board::isSlot(slot.index));
  targets_[slot.index] = target;
}

// Where the SWD lines go now. The module DEBUG_SEL points at, or - with none attached - straight
// to the wires.
interfaces::Pins& Selector::swdPath() {
  SwdTarget* target = targets_[debug_sel_ ? 1 : 0];
  if (target == nullptr) return pins_;
  return *target;
}

bool Selector::setMode(std::uint8_t gpio, PinMode mode) {
  bool ok;
  if (isSwd(gpio) && (forward_stray_clocks || !pins_.resetAsserted())) {
    ok = swdPath().setMode(gpio, mode);
  } else {
    ok = pins_.setMode(gpio, mode);
  }
  if (ok) observe();
  return ok;
}

bool Selector::write(std::uint8_t gpio, bool high) {
  bool ok;
  if (isSwd(gpio) && (forward_stray_clocks || !pins_.resetAsserted())) {
    ok = swdPath().write(gpio, high);
  } else {
    ok = pins_.write(gpio, high);
  }
  if (ok) observe();
  return ok;
}

bool Selector::read(std::uint8_t gpio) { return pins_.read(gpio); }

bool Selector::holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) {
  return pins_.holdThroughSleep(gpios, count, on);
}

// The two flip-flops and the RC on the reset, after every change the Walter made to a pin.
void Selector::observe() {
  const bool reset_low = pins_.resetAsserted();

  // The gate: SHIFT_CLK = SWDCLK AND NOT nRESET (74LVC1G97). A rising edge of it shifts.
  const bool shift = pins_.wireLevel(kSwdClk.gpio) && reset_low;
  if (shift && !shift_was_) {
    uart_sel_ = debug_sel_;                      // FF2 <- FF1's old Q
    debug_sel_ = pins_.wireLevel(kSwdIo.gpio);   // FF1 <- SWDIO
    shifts_++;
  }
  shift_was_ = shift;

  // The reset toward the modules. The RC and the Schmitt buffer mean a module sees it only if it
  // lasted; the decision is made when nRESET is released, because nothing touches a pin while
  // the Walter is holding it down.
  if (reset_low && !reset_low_was_) {
    reset_start_us_ = clock_.nowUs();
  } else if (!reset_low && reset_low_was_) {
    const std::uint64_t held = clock_.nowUs() - reset_start_us_;
    last_reset_us_ = held;
    if (held >= kSelectorResetFilterUs) {
      const std::size_t which = debug_sel_ ? 1 : 0;
      resets_[which]++;
      if (on_reset[which]) on_reset[which]();
    } else {
      windows_++;  // filtered out entirely: no radio was reset
    }
  }
  reset_low_was_ = reset_low;
}

// ---- the UART's mux ---------------------------------------------------------------------------

bool UartMux::open() {
  // The Walter's peripheral takes its two pads; both modules are behind the mux either way.
  const bool a = radios_[0]->open();
  const bool b = radios_[1]->open();
  return a && b;
}

void UartMux::close() {
  radios_[0]->close();
  radios_[1]->close();
}

std::size_t UartMux::read(std::uint8_t* out, std::size_t max) {
  // The unselected module's TX "simply goes nowhere": drain it and drop it, so that a flip does
  // not deliver bytes the Walter was never wired to hear.
  ScriptedRadio& other = *radios_[selector_.uartSel() ? 0 : 1];
  std::uint8_t sink[64];
  for (;;) {
    const std::size_t n = other.read(sink, sizeof sink);
    if (n == 0) break;
    dropped_ += static_cast<std::uint32_t>(n);
  }
  return radios_[selector_.uartSel() ? 1 : 0]->read(out, max);
}

std::size_t UartMux::write(const std::uint8_t* data, std::size_t size) {
  return radios_[selector_.uartSel() ? 1 : 0]->write(data, size);
}

bool UartMux::flush(std::uint32_t timeout_ms) {
  return radios_[selector_.uartSel() ? 1 : 0]->flush(timeout_ms);
}

bool UartMux::idleFor(std::uint32_t ms) {
  return radios_[selector_.uartSel() ? 1 : 0]->idleFor(ms);
}

}  // namespace sim
