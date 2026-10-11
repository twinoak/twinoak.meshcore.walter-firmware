#include "core/board/radio_pins.h"

namespace core {
namespace board {

using interfaces::PinMode;

const char* describe(Outcome outcome) {
  switch (outcome) {
    case Outcome::kOk: return "ok";
    case Outcome::kKillTooShort: return "too short for the rail to collapse";
    case Outcome::kKillTooLong: return "longer than the kill limit";
    case Outcome::kSwdHeld: return "refused: the SWD lines are acquired; release them first";
    case Outcome::kSwdAlreadyHeld: return "refused: the SWD lines are already acquired";
    case Outcome::kResetAsserted: return "refused: nRESET is asserted";
    case Outcome::kRailOff: return "refused: the radio's rail is off (the kill line is driven)";
    case Outcome::kPinRefused: return "the pin driver refused; pins are back in the safe state";
  }
  return "?";
}

const char* describe(RadioPower power) {
  switch (power) {
    case RadioPower::kUnknown: return "unknown (the rail is settling)";
    case RadioPower::kOn: return "on";
    case RadioPower::kOff: return "off (the kill line is driven)";
  }
  return "?";
}

// ---- the reset line ---------------------------------------------------------------------------

// Level first, open-drain second: an open-drain pin written false drives low the moment it is
// enabled, and an open-drain pin can never drive high (interfaces/pins.h). The nRF52 has its
// own pull-up (INTERCONNECT.md, BENCH-BREADBOARD.md §6 caveat 2).
bool RadioPins::ResetLine::assertLow() {
  if (!pins_.write(gpio_, false)) return false;
  return pins_.setMode(gpio_, PinMode::kOpenDrain);
}

bool RadioPins::ResetLine::release() { return pins_.setMode(gpio_, PinMode::kInput); }

// ---- the SWD lines ----------------------------------------------------------------------------

bool SwdLines::clock(bool high) {
  if (!owner_.swd_held_ || owner_.reset_asserted_) return false;
  return owner_.pins_.write(kSwdClk.gpio, high);
}

bool SwdLines::driveIo(bool high) {
  if (!owner_.swd_held_ || owner_.reset_asserted_) return false;
  if (!owner_.pins_.write(kSwdIo.gpio, high)) return false;
  if (!owner_.pins_.setMode(kSwdIo.gpio, PinMode::kOutput)) return false;
  owner_.swdio_driven_ = true;
  return true;
}

bool SwdLines::releaseIo() {
  if (!owner_.swd_held_) return false;
  owner_.swdio_driven_ = false;
  return owner_.pins_.setMode(kSwdIo.gpio, PinMode::kInput);
}

bool SwdLines::readIo() {
  if (!owner_.swd_held_) return false;
  return owner_.pins_.read(kSwdIo.gpio);
}

// ---- RadioPins --------------------------------------------------------------------------------

RadioPins::RadioPins(interfaces::Pins& pins, interfaces::Clock& clock,
                     interfaces::Retained& retained, const KillTiming& timing)
    : pins_(pins),
      clock_(clock),
      retained_(retained),
      timing_(timing),
      reset_(pins, kNReset.gpio),
      swd_(*this) {
  const std::uint64_t now = clock_.nowMs();
  // The rail: the kill line floats to "on" by hardware, and this firmware has not driven it yet;
  // but a Walter that reset in the middle of a kill released it only a boot ago, so the rail is
  // "settling" for the first settle_ms (rule 4).
  rail_on_at_ms_ = now + timing_.settle_ms;
  // The last reset. A record from before this boot is honoured when the clock still makes sense
  // of it. Without one - or with one from a clock epoch that ended (a deep sleep today, see
  // interfaces/clock.h) - the worst is assumed: a reset ended just now. That covers the one the
  // ESP32-S3's own power-up glitch gave the radio (BENCH-BREADBOARD.md §6 caveat 3), so the first
  // pulse after a power-up is never the second tap of a double-tap (caveat 4).
  Record record{};
  if (retained_.load(interfaces::RetainedSlot::kResetRecord, &record, sizeof record) &&
      record.last_reset_end_ms <= now) {
    last_reset_end_ms_ = record.last_reset_end_ms;
  } else {
    last_reset_end_ms_ = now;
  }
  last_reset_known_ = true;
  safe();
}

bool RadioPins::input(std::uint8_t gpio) { return pins_.setMode(gpio, PinMode::kInput); }

// TX in the safe state: pulled up (idle high, not driven) while the radio is believed powered, so
// that its receiver sees a quiet line and not a floating one; floating otherwise, so that nothing
// - not even a weak pull-up - feeds a dead rail (rule 3: "make it zero").
bool RadioPins::applyTx() {
  return pins_.setMode(kWalterTx.gpio, power() == RadioPower::kOn ? PinMode::kInputPullUp
                                                                  : PinMode::kInput);
}

void RadioPins::safe() {
  // Order: the kill line first, so that if it was driven the rail is coming back while the rest
  // is released; then the reset line, so that it is never left asserted; then everything else.
  // Results are not checked here: this is the fallback, and every pin is tried regardless.
  if (kill_driven_) {
    kill_driven_ = false;
    rail_on_at_ms_ = clock_.nowMs() + timing_.settle_ms;
  }
  input(kPwrKill.gpio);
  reset_.release();
  reset_asserted_ = false;
  swd_held_ = false;
  swdio_driven_ = false;
  uart_enabled_ = false;
  input(kSwdClk.gpio);
  input(kSwdIo.gpio);
  input(kSpare.gpio);
  input(kWalterRx.gpio);
  applyTx();
}

Outcome RadioPins::failed() {
  safe();
  return Outcome::kPinRefused;
}

RadioPower RadioPins::power() {
  if (kill_driven_) return RadioPower::kOff;
  if (clock_.nowMs() < rail_on_at_ms_) return RadioPower::kUnknown;
  return RadioPower::kOn;
}

// Bounded by settle_ms: rail_on_at_ms_ is never further ahead than that.
void RadioPins::waitForRail() {
  const std::uint64_t now = clock_.nowMs();
  if (now < rail_on_at_ms_) clock_.delayMs(static_cast<std::uint32_t>(rail_on_at_ms_ - now));
}

std::uint32_t RadioPins::msUntilResetAllowed() {
  const std::uint64_t now = clock_.nowMs();
  if (!last_reset_known_ || last_reset_end_ms_ > now) return kResetSpacingMs;
  const std::uint64_t since = now - last_reset_end_ms_;
  return since >= kResetSpacingMs ? 0 : static_cast<std::uint32_t>(kResetSpacingMs - since);
}

void RadioPins::recordResetEnd() {
  last_reset_end_ms_ = clock_.nowMs();
  last_reset_known_ = true;
  const Record record{last_reset_end_ms_};
  retained_.store(interfaces::RetainedSlot::kResetRecord, &record, sizeof record);
}

void RadioPins::noteRadioReset() { recordResetEnd(); }

Outcome RadioPins::resetPulse() {
  if (swd_held_) return Outcome::kSwdHeld;
  if (kill_driven_) return Outcome::kRailOff;
  waitForRail();
  // Rule 2: wait, never violate. A clock that went backwards (last_reset_end_ms_ > now) is a new
  // epoch and gets the full spacing.
  if (last_reset_end_ms_ > clock_.nowMs()) last_reset_end_ms_ = clock_.nowMs();
  const std::uint32_t wait = msUntilResetAllowed();
  if (wait > 0) clock_.delayMs(wait);

  reset_asserted_ = true;
  if (!reset_.assertLow()) return failed();
  clock_.delayMs(kResetPulseMs);
  if (!reset_.release()) return failed();
  reset_asserted_ = false;
  recordResetEnd();
  return Outcome::kOk;
}

Outcome RadioPins::killCycle(std::uint32_t off_ms) {
  if (off_ms < timing_.min_ms) return Outcome::kKillTooShort;
  if (off_ms > timing_.max_ms) return Outcome::kKillTooLong;

  // Rule 3: before the kill line goes high, every other radio-facing pin is a plain input - the
  // UART's, the SWD lines, the reset line, the spare - with no pull-up. The UART and SWD are
  // taken back from whoever held them: their operations return false from here on.
  uart_enabled_ = false;
  swd_held_ = false;
  swdio_driven_ = false;
  if (!reset_.release()) return failed();
  reset_asserted_ = false;
  for (const PinDef& pin : kRadioPins) {
    if (pin.gpio == kPwrKill.gpio || pin.gpio == kNReset.gpio) continue;
    if (!input(pin.gpio)) return failed();
  }

  // Rule 4: the kill line is driven high - level first, then enabled - and never low.
  if (!pins_.write(kPwrKill.gpio, true)) return failed();
  kill_driven_ = true;
  if (!pins_.setMode(kPwrKill.gpio, PinMode::kOutput)) return failed();
  clock_.delayMs(off_ms);

  // Stop driving it: input, never output-low. The rail comes back by hardware.
  kill_driven_ = false;
  rail_on_at_ms_ = clock_.nowMs() + timing_.settle_ms;
  if (!input(kPwrKill.gpio)) return failed();
  clock_.delayMs(timing_.settle_ms);

  // Only now may anything face the radio again: the safe state puts TX's pull-up back, and the
  // caller may enable the UART.
  safe();
  return Outcome::kOk;
}

Outcome RadioPins::uartEnable() {
  if (kill_driven_) return Outcome::kRailOff;
  waitForRail();
  if (!pins_.write(kWalterTx.gpio, true)) return failed();
  if (!pins_.setMode(kWalterTx.gpio, PinMode::kOutput)) return failed();
  if (!input(kWalterRx.gpio)) return failed();
  uart_enabled_ = true;
  return Outcome::kOk;
}

void RadioPins::uartRelease() {
  uart_enabled_ = false;
  applyTx();
  input(kWalterRx.gpio);
}

Outcome RadioPins::swdAcquire() {
  if (swd_held_) return Outcome::kSwdAlreadyHeld;
  if (reset_asserted_) return Outcome::kResetAsserted;
  if (kill_driven_) return Outcome::kRailOff;
  waitForRail();
  // SWDCLK idle low and driven; SWDIO an input until a transfer drives it (ARCHITECTURE §2).
  if (!pins_.write(kSwdClk.gpio, false)) return failed();
  if (!pins_.setMode(kSwdClk.gpio, PinMode::kOutput)) return failed();
  if (!input(kSwdIo.gpio)) return failed();
  swd_held_ = true;
  return Outcome::kOk;
}

void RadioPins::swdRelease() {
  swd_held_ = false;
  swdio_driven_ = false;
  input(kSwdClk.gpio);
  input(kSwdIo.gpio);
}

bool RadioPins::holdForSleep() {
  safe();
  std::uint8_t gpios[kPinCount];
  for (std::size_t i = 0; i < kPinCount; ++i) gpios[i] = kRadioPins[i].gpio;
  return pins_.holdThroughSleep(gpios, kPinCount, true);
}

bool RadioPins::releaseHolds() {
  // The same state the holds were put on in, configured again before they come off, so the pad
  // sees no change (interfaces/pins.h; ESP-IDF's gpio_hold_dis documentation says the same).
  safe();
  std::uint8_t gpios[kPinCount];
  for (std::size_t i = 0; i < kPinCount; ++i) gpios[i] = kRadioPins[i].gpio;
  return pins_.holdThroughSleep(gpios, kPinCount, false);
}

PinState RadioPins::state(std::size_t index) {
  if (index >= kPinCount) index = kPinCount - 1;
  const PinDef& pin = kRadioPins[index];
  PinState s{pin.net, pin.dsub, pin.gpio, PinMode::kInput, pins_.read(pin.gpio), "released"};
  if (pin.gpio == kNReset.gpio) {
    s.mode = reset_asserted_ ? PinMode::kOpenDrain : PinMode::kInput;
    s.use = reset_asserted_ ? "reset" : "released";
  } else if (pin.gpio == kPwrKill.gpio) {
    s.mode = kill_driven_ ? PinMode::kOutput : PinMode::kInput;
    s.use = kill_driven_ ? "kill" : "released";
  } else if (pin.gpio == kSwdClk.gpio) {
    s.mode = swd_held_ ? PinMode::kOutput : PinMode::kInput;
    s.use = swd_held_ ? "swd" : "released";
  } else if (pin.gpio == kSwdIo.gpio) {
    s.mode = swdio_driven_ ? PinMode::kOutput : PinMode::kInput;
    s.use = swd_held_ ? "swd" : "released";
  } else if (pin.gpio == kWalterTx.gpio) {
    s.mode = uart_enabled_ ? PinMode::kOutput
             : (power() == RadioPower::kOn ? PinMode::kInputPullUp : PinMode::kInput);
    s.use = uart_enabled_ ? "uart" : "released";
  } else if (pin.gpio == kWalterRx.gpio) {
    s.use = uart_enabled_ ? "uart" : "released";
  }
  return s;
}

}  // namespace board
}  // namespace core
