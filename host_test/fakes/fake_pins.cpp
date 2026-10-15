#include "fakes/fake_pins.h"

#include <doctest.h>

#include <string>

namespace fakes {

using core::board::kNReset;
using core::board::kPwrKill;
using core::board::kRadioPins;
using core::board::kResetPulseMs;
using core::board::kResetSpacingMs;
using core::board::kSwdClk;
using interfaces::PinMode;

namespace {

const char* modeName(PinMode mode) {
  switch (mode) {
    case PinMode::kInput: return "input";
    case PinMode::kInputPullUp: return "input-pull-up";
    case PinMode::kOutput: return "output";
    case PinMode::kOpenDrain: return "open-drain";
  }
  return "?";
}

const char* netOf(std::uint8_t gpio) {
  for (const core::board::PinDef& p : kRadioPins) {
    if (p.gpio == gpio) return p.net;
  }
  return "gpio";
}

}  // namespace

// The suite's own tally (task p3-06, rails R1 and R2): one object for the process, so that "the
// rules held" is a statement about every test that drove a pin and not about the one in hand.
PinTally& pinTally() {
  static PinTally tally;
  return tally;
}

FakePins::FakePins(FakeClock& clock, const core::board::KillTiming& timing)
    : clock_(clock), timing_(timing) {
  pins_[kNReset.gpio].external = true;  // the nRF52's own pull-up
  pinTally().instances++;
}

bool FakePins::allows(const char* rule) const {
  for (const std::string& allowed : allowed_) {
    if (allowed == rule) return true;
  }
  return false;
}

int FakePins::undeclaredViolations() const {
  int n = 0;
  for (const Violation& v : violations) {
    if (!allows(v.rule.c_str())) n++;
  }
  return n;
}

// The rules are checked at the transition (`strict`), and again here - because a test can turn
// `strict` off. A violation nobody declared with allow() fails the test at the end of it, which is
// the one thing `strict = false` must not be able to hide.
FakePins::~FakePins() {
  PinTally& tally = pinTally();
  tally.operations += static_cast<unsigned>(operations);
  tally.reset_pulses += static_cast<unsigned>(reset_pulses);
  tally.kill_cycles += static_cast<unsigned>(kill_cycles);
  for (const Violation& v : violations) {
    const bool declared = allows(v.rule.c_str());
    if (!report_undeclared && !declared) continue;  // the test that demonstrates this check
    tally.violations++;
    tally.by_rule[v.rule]++;
    if (declared) {
      tally.declared++;
      continue;
    }
    tally.undeclared++;
    tally.undeclared_by_rule[v.rule]++;
    FAIL_CHECK("a pin rule was broken and no test declared it: " << v.rule << " - " << v.detail
               << " at " << v.at_ms << " ms (fakes::FakePins::allow(\"" << v.rule
               << "\") is how a test says it wants to see one fire)");
  }
}

bool FakePins::isRadioPin(std::uint8_t gpio) const {
  for (const core::board::PinDef& p : kRadioPins) {
    if (p.gpio == gpio) return true;
  }
  return false;
}

bool FakePins::driving(std::uint8_t gpio) const {
  const Pin& p = pins_[gpio];
  if (p.mode == PinMode::kOutput) return true;
  if (p.mode == PinMode::kOpenDrain) return !p.level;
  return false;
}

bool FakePins::pulling(std::uint8_t gpio) const { return pins_[gpio].mode == PinMode::kInputPullUp; }

bool FakePins::wireLevel(std::uint8_t gpio) const {
  const Pin& p = pins_[gpio];
  if (driving(gpio)) return p.level;
  if (p.mode == PinMode::kInputPullUp) return true;
  return p.external;
}

bool FakePins::killDriven() const {
  return pins_[kPwrKill.gpio].mode == PinMode::kOutput && pins_[kPwrKill.gpio].level;
}

bool FakePins::refused(const Op& op) {
  operations++;
  return fail_when && fail_when(op);
}

void FakePins::record(const Op& op, bool changed) {
  if (record_log) log.push_back(Transition{clock_.now_ms, op, changed});
}

void FakePins::violate(const char* rule, const std::string& detail) {
  violations.push_back(Violation{clock_.now_ms, rule, detail});
  if (strict) {
    // std::string, not the const char*: doctest's message stream would print the pointer as a bool.
    FAIL_CHECK("pin rule broken: " << std::string(rule) << " - " << detail << " at "
                                   << clock_.now_ms << " ms");
  }
}

// A pin number past the end fails the test (a FAIL throws, as REQUIRE would). A plain comparison
// rather than REQUIRE on these three: they run once per SWD clock edge, and a flash is millions
// of edges - a doctest assertion each was most of the host suite's time (task p1-05).
#define TWINOAK_GPIO_IN_RANGE(g) \
  if ((g) >= kGpioCount) FAIL("fake pins: GPIO " << static_cast<unsigned>(g) << " is not a pin of the ESP32-S3")

bool FakePins::setMode(std::uint8_t gpio, PinMode mode) {
  TWINOAK_GPIO_IN_RANGE(gpio);
  const Op op{OpKind::kMode, gpio, mode, false, false};
  if (refused(op)) return false;
  const bool was_reset = resetAsserted();
  const bool was_kill = killDriven();
  const bool clk_wire = wireLevel(kSwdClk.gpio);
  const bool changed = pins_[gpio].mode != mode;
  pins_[gpio].mode = mode;
  record(op, changed);
  if (changed) afterChange(gpio, was_reset, was_kill, clk_wire);
  return true;
}

bool FakePins::write(std::uint8_t gpio, bool high) {
  TWINOAK_GPIO_IN_RANGE(gpio);
  const Op op{OpKind::kLevel, gpio, PinMode::kInput, high, false};
  if (refused(op)) return false;
  const bool was_reset = resetAsserted();
  const bool was_kill = killDriven();
  const bool clk_wire = wireLevel(kSwdClk.gpio);
  const bool changed = pins_[gpio].level != high;
  pins_[gpio].level = high;
  record(op, changed);
  // Rule 2, the strict reading: the reset pin is never even asked to go high.
  if (gpio == kNReset.gpio && high) {
    violate("reset-driven-high", "nRESET written high (" + std::string(modeName(pins_[gpio].mode)) + ")");
  }
  if (changed) afterChange(gpio, was_reset, was_kill, clk_wire);
  return true;
}

bool FakePins::read(std::uint8_t gpio) {
  TWINOAK_GPIO_IN_RANGE(gpio);
  return wireLevel(gpio);
}

bool FakePins::holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) {
  for (std::size_t i = 0; i < count; ++i) {
    REQUIRE(gpios[i] < kGpioCount);
    const Op op{OpKind::kHold, gpios[i], PinMode::kInput, false, on};
    const bool changed = pins_[gpios[i]].held != on;
    pins_[gpios[i]].held = on;
    record(op, changed);
  }
  return true;
}

// The rules, checked after a pin's state changed.
void FakePins::afterChange(std::uint8_t gpio, bool was_reset_asserted, bool was_kill_driven,
                           bool clk_wire_was) {
  const Pin& p = pins_[gpio];
  const std::uint64_t now = clock_.now_ms;

  if (gpio == kNReset.gpio) {
    if (p.mode == PinMode::kOutput) violate("reset-driven-high", "nRESET configured push-pull");
    const bool asserted = resetAsserted();
    if (asserted && !was_reset_asserted) {
      if (have_last_reset_end_ && now - last_reset_end_ms_ < kResetSpacingMs) {
        violate("reset-spacing", "nRESET asserted " + std::to_string(now - last_reset_end_ms_) +
                                     " ms after the previous pulse ended");
      }
      reset_start_ms_ = now;
      reset_start_us_ = clock_.nowUs();
      clk_moved_in_reset_ = false;
    } else if (!asserted && was_reset_asserted) {
      if (clk_moved_in_reset_) {
        // The dual adapter's selector being programmed, not a reset: SWDCLK moved inside the
        // window, which is what the adapter's gate turns into a shift clock
        // (DUAL-RAK4630-FEASIBILITY.md section 3). Its length is a microsecond figure, and the
        // radios never see it, so it does not start the reset spacing.
        const std::uint64_t held_us = clock_.nowUs() - reset_start_us_;
        selector_windows++;
        last_selector_window_us = held_us;
        if (held_us < core::board::kSelectorWindowMinUs || held_us > core::board::kSelectorWindowMaxUs) {
          violate("selector-window", "the selector's programming window lasted " +
                                         std::to_string(held_us) + " us, outside 100-500 us");
        }
      } else {
        const std::uint64_t held = now - reset_start_ms_;
        if (held < kResetPulseMs || held > kResetPulseMs + 50) {
          violate("reset-pulse-length", "nRESET held low for " + std::to_string(held) + " ms");
        }
        have_last_reset_end_ = true;
        last_reset_end_ms_ = now;
        ++reset_pulses;
        if (on_reset_pulse) on_reset_pulse();
      }
      clk_moved_in_reset_ = false;
    }
  }

  if (gpio == kSwdClk.gpio && resetAsserted() && was_reset_asserted &&
      wireLevel(kSwdClk.gpio) != clk_wire_was) {
    // Either the one sanctioned use of it - the selector - or the rule it would otherwise break.
    clk_moved_in_reset_ = true;
    selector_clocks++;
    if (!selector_allowed) {
      violate("swdclk-while-reset", "SWDCLK changed level while nRESET is asserted");
    }
  }

  if (gpio == kPwrKill.gpio) {
    if (p.mode == PinMode::kOutput && !p.level) violate("kill-driven-low", "KILL driven low");
    if (p.mode == PinMode::kOpenDrain) violate("kill-driven-low", "KILL configured open-drain");
    const bool driven = killDriven();
    if (driven && !was_kill_driven) {
      kill_start_ms_ = now;
      for (const core::board::PinDef& other : kRadioPins) {
        if (other.gpio == kPwrKill.gpio) continue;
        if (driving(other.gpio) || pulling(other.gpio)) {
          violate("driven-while-killed", std::string(other.net) + " is " + modeName(pins_[other.gpio].mode) +
                                             " as KILL goes high");
        }
      }
    } else if (!driven && was_kill_driven) {
      const std::uint64_t off = now - kill_start_ms_;
      if (off < timing_.min_ms || off > timing_.max_ms) {
        violate("kill-bounds", "KILL driven high for " + std::to_string(off) + " ms");
      }
      ++kill_cycles;
      if (on_kill_cycle) on_kill_cycle();
    }
  } else if (isRadioPin(gpio) && killDriven() && (driving(gpio) || pulling(gpio))) {
    violate("driven-while-killed", std::string(netOf(gpio)) + " became " + modeName(p.mode) +
                                       " while KILL is high");
  }
}

std::vector<Transition> FakePins::transitionsOf(std::uint8_t gpio) const {
  std::vector<Transition> out;
  for (const Transition& t : log) {
    if (t.op.gpio == gpio && t.changed) out.push_back(t);
  }
  return out;
}

int FakePins::indexOf(std::uint8_t gpio, OpKind kind, PinMode mode, bool level, std::size_t from) const {
  for (std::size_t i = from; i < log.size(); ++i) {
    const Transition& t = log[i];
    if (!t.changed || t.op.gpio != gpio || t.op.kind != kind) continue;
    if (kind == OpKind::kMode && t.op.mode != mode) continue;
    if (kind == OpKind::kLevel && t.op.level != level) continue;
    return static_cast<int>(i);
  }
  return -1;
}

int FakePins::countViolations(const char* rule) const {
  int n = 0;
  for (const Violation& v : violations) {
    if (v.rule == rule) n++;
  }
  return n;
}

}  // namespace fakes
