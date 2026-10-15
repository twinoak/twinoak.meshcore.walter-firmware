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
//   selector-window       a programming window outside DUAL-RAK4630-FEASIBILITY.md section 3's
//                         100-500 us (checked only where one is allowed)
//
// A violation is recorded in `violations`; while `strict` (the default) it also fails the running
// test at once with doctest's FAIL_CHECK. A test that wants to see a rule fire sets strict false.
//
// The dual adapter's selector (task p1-06) is the one sanctioned "the clock toggles while reset
// is asserted", and a bench that has one on the wires says so with `selector_allowed`. Then an
// assertion of nRESET during which SWDCLK moved is a *programming window*, not a reset pulse: it
// is counted, its length is held against the document's 100-500 us in microseconds (the window is
// shorter than a millisecond), and it does not count as a reset the next pulse must be spaced
// from - the adapter's own RC and Schmitt buffer keep it away from the radios, so as far as these
// wires' rules are concerned no radio was reset. RadioPins is stricter and waits anyway.
//
// Failures can be injected: `fail_when` is asked before every setMode/write and, when it says so,
// the call returns false without changing anything - what a refused GPIO call does.
//
// **The rules are the suite's, not one test's** (task p3-06, rails R1 and R2). `strict` fails the
// running test at the transition, which is the first line; but a test could turn `strict` off and
// then do anything, and two tests do exactly that on purpose. So every instance also counts what
// it saw into a process-wide `pinTally()`, and at its destruction an instance that recorded a
// violation **it did not declare** fails the test whatever `strict` said. Declaring one is
// `allow("the-rule")`, which is what a test demonstrating a rule firing writes; anything else is
// a rail broken quietly. host_test/rails/test_rail_pins.cpp prints the tally and holds the
// undeclared count to zero over the whole run.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
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

// What the whole suite did to a set of radio-facing wires: how many fakes there were, how many
// transitions they checked, and every violation any of them recorded - split into the ones a test
// declared with allow() and the ones nobody did. One object for the process (task p3-06).
struct PinTally {
  unsigned instances = 0;
  unsigned operations = 0;    // setMode/write/hold calls the rules were checked on
  unsigned reset_pulses = 0;
  unsigned kill_cycles = 0;
  unsigned violations = 0;    // every one, declared or not
  unsigned declared = 0;      // ... that a test asked for with allow()
  unsigned undeclared = 0;    // ... that nobody did: a rail broken quietly
  std::map<std::string, unsigned> by_rule;
  std::map<std::string, unsigned> undeclared_by_rule;
};

PinTally& pinTally();

class FakePins final : public interfaces::Pins {
 public:
  explicit FakePins(FakeClock& clock, const core::board::KillTiming& timing = core::board::kKillTiming);
  ~FakePins();

  bool strict = true;
  bool record_log = true;  // false: the rules are still checked, the transitions not kept (a flash is millions)
  // A dual adapter's selector is on these wires: a clock edge while nRESET is asserted is the
  // selector being programmed and not a broken rule. False on a single-radio unit, which is
  // every unit today - and then swdclk-while-reset fires, which is the point.
  bool selector_allowed = false;
  std::vector<Transition> log;
  std::vector<Violation> violations;
  std::function<bool(const Op&)> fail_when;
  int operations = 0;
  // What the radio actually got, counted whether or not the log is kept (task p2-05's property
  // test runs a simulated month with record_log off and still has to count what was done to the
  // radio). A pulse and a cycle are counted when they *end*, which is when their rule is checked.
  int reset_pulses = 0;
  int kill_cycles = 0;
  // Called as each of those ends: how a bench makes the world react to what the Walter did -
  // a radio that comes back after a reset, or after the rail has been and gone.
  std::function<void()> on_reset_pulse;
  std::function<void()> on_kill_cycle;
  int selector_windows = 0;  // assertions of nRESET during which SWDCLK moved
  int selector_clocks = 0;   // SWDCLK level changes inside them
  std::uint64_t last_selector_window_us = 0;  // how long the last one lasted

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

  // "This test is about seeing that rule fire." Declared violations of it are counted and do not
  // fail anything; everything else does, at the destructor, whatever `strict` says (task p3-06).
  void allow(const char* rule) { allowed_.push_back(rule); }
  bool allows(const char* rule) const;
  // Violations this instance recorded that no allow() covers.
  int undeclaredViolations() const;
  // The one use for this: host_test/rails/test_rail_pins.cpp, which shows the destructor's own
  // check working and must not fail itself or dirty the tally while doing it.
  bool report_undeclared = true;

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
  std::vector<std::string> allowed_;
  Pin pins_[kGpioCount];
  bool have_last_reset_end_ = false;
  bool clk_moved_in_reset_ = false;  // this assertion of nRESET is a selector programming window
  std::uint64_t last_reset_end_ms_ = 0;
  std::uint64_t reset_start_ms_ = 0;
  std::uint64_t reset_start_us_ = 0;  // the same moment in microseconds: a window is sub-millisecond
  std::uint64_t kill_start_ms_ = 0;
};

}  // namespace fakes
