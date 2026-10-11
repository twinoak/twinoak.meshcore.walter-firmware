// Fake pins that record every transition with its time and check the rules of
// docs/ARCHITECTURE.md §2 themselves (task p0-03). They know the pin map (core/board/pin_map.h)
// and nothing of RadioPins: the rules are about what happens on the wires, whoever drives them.
//
// The rules, each named so that a test can say which one it expects to fire:
//
//   reset-driven-high     nRESET configured push-pull, or written high in any mode
//   reset-spacing         nRESET asserted less than kResetSpacingMs after the previous pulse ended
//   reset-pulse-length    nRESET held low for less than kResetPulseMs or for much longer
//   swdclk-while-reset    SWDCLK's level on the wire changed while nRESET is asserted
//   driven-while-killed   while KILL is driven high, any other radio pin drives or pulls up
//   kill-driven-low       KILL configured push-pull with a low level, or open-drain
//   kill-bounds           KILL driven high for less than min_ms or more than max_ms
//
// A violation is recorded in `violations`; while `strict` (the default) it also fails the running
// test at once with doctest's FAIL_CHECK. A test that wants to see a rule fire sets strict false.
//
// Failures can be injected: `fail_when` is asked before every setMode/write and, when it says so,
// the call returns false without changing anything - what a refused GPIO call does.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "fakes/fake_clock.h"
#include "interfaces/pins.h"

namespace fakes {

enum class OpKind { kMode, kLevel, kHold };

struct Op {
  OpKind kind;
  std::uint8_t gpio;
  interfaces::PinMode mode;  // kMode
  bool level;                // kLevel
  bool hold;                 // kHold
};

struct Transition {
  std::uint64_t at_ms;
  Op op;
  bool changed;  // false: the call repeated the state the pin was already in
};

struct Violation {
  std::uint64_t at_ms;
  std::string rule;
  std::string detail;
};

class FakePins final : public interfaces::Pins {
 public:
  explicit FakePins(FakeClock& clock, const core::board::KillTiming& timing = core::board::kKillTiming);

  bool strict = true;
  std::vector<Transition> log;
  std::vector<Violation> violations;
  std::function<bool(const Op&)> fail_when;
  int operations = 0;

  // interfaces::Pins
  bool setMode(std::uint8_t gpio, interfaces::PinMode mode) override;
  bool write(std::uint8_t gpio, bool high) override;
  bool read(std::uint8_t gpio) override;
  bool holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) override;

  // The world's side of a pin: what it reads as when we are not driving it. nRESET defaults to
  // high (the nRF52's own pull-up), everything else to low.
  void setExternalLevel(std::uint8_t gpio, bool high) { pins_[gpio].external = high; }

  interfaces::PinMode modeOf(std::uint8_t gpio) const { return pins_[gpio].mode; }
  bool levelOf(std::uint8_t gpio) const { return pins_[gpio].level; }
  bool held(std::uint8_t gpio) const { return pins_[gpio].held; }
  bool driving(std::uint8_t gpio) const;   // push-pull, or open-drain pulling low
  bool pulling(std::uint8_t gpio) const;   // the weak pull-up
  bool wireLevel(std::uint8_t gpio) const;
  bool resetAsserted() const { return driving(core::board::kNReset.gpio) && !levelOf(core::board::kNReset.gpio); }
  bool killDriven() const;

  // Every transition of this pin that changed something, in order.
  std::vector<Transition> transitionsOf(std::uint8_t gpio) const;
  // Index in `log` of the first change matching, or -1.
  int indexOf(std::uint8_t gpio, OpKind kind, interfaces::PinMode mode, bool level, std::size_t from = 0) const;
  int countViolations(const char* rule) const;
  void clearLog() { log.clear(); }

  static constexpr std::size_t kGpioCount = 49;  // the ESP32-S3 has GPIO0..48

 private:
  struct Pin {
    interfaces::PinMode mode = interfaces::PinMode::kInput;
    bool level = false;
    bool external = false;
    bool held = false;
  };

  bool refused(const Op& op);
  void record(const Op& op, bool changed);
  void violate(const char* rule, const std::string& detail);
  void afterChange(std::uint8_t gpio, bool was_reset_asserted, bool was_kill_driven, bool clk_wire_was);
  bool isRadioPin(std::uint8_t gpio) const;

  FakeClock& clock_;
  core::board::KillTiming timing_;
  Pin pins_[kGpioCount];
  bool have_last_reset_end_ = false;
  std::uint64_t last_reset_end_ms_ = 0;
  std::uint64_t reset_start_ms_ = 0;
  std::uint64_t kill_start_ms_ = 0;
};

}  // namespace fakes
