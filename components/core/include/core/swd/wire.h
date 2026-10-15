// The SWD wire (task p1-03): Serial Wire Debug at the level of the two lines, over the SwdLines
// that RadioPins::swdAcquire() hands out and the clock's microsecond delay. Everything about the
// protocol here is Arm's: Debug Interface Architecture Specification ADIv5.0 to ADIv5.2,
// IHI 0031G (ID022122), chapter B4 (the Serial Wire Debug Port) and B5 (the SW/JTAG Debug Port);
// each number says the section it comes from. No ESP-IDF include, no heap, and no loop without a
// constant bound - every count here is a constant or an argument checked against one.
//
// The clock is ours (B4.3.1: the clock "can be ... provided by the host"), SWCLK idles low
// (RadioPins acquires it low) and one bit is one cycle: low, then high, then low. The target
// samples SWDIO on the rising edge of SWCLK and changes SWDIO on the rising edge (B4.3.1), so
//
//   - a bit of ours is set right after a falling edge and held through the rising edge that
//     follows, half a period later;
//   - a bit of the target's appears at a rising edge and is read at the end of the low half that
//     follows, just before the next rising edge - the latest moment it can be read, chosen for
//     the filtered lines (the comment at Wire::cycle in wire.cpp says why).
//
// SWDIO is an input whenever the target owns the line, and again after every transfer: the host
// drives it only for a request, a line reset, the switch sequence, write data and idle cycles.
#pragma once

#include <cstdint>

#include "core/board/radio_pins.h"
#include "interfaces/clock.h"

namespace core {
namespace swd {

// The half-period of SWCLK, in microseconds, and so the speed. The lines each carry 470 ohm in
// series and about 2.2 nF to ground, a time constant of about 1 us (hardware repository,
// BENCH-BREADBOARD.md section 4, which predicts a ceiling near 300 kHz and says to start at
// 100 kHz). The default is 4 us - 125 kHz - rather than the 5 us (100 kHz) the task names,
// because the nRF52840 Product Specification gives the SWDCLK frequency a minimum: "Debug and
// trace > Electrical specification > Control access port", f_SWDCLK min 0.125 MHz, max 8 MHz;
// 125 kHz is the slowest clock the chip is specified for, and through the filters a half-period
// of 4 us is four time constants, 98 % settled. The lower bound stops a setting from going faster
// than the filtered lines can carry without meaning to: at a half-period of one time constant the
// line reaches 63 % of the swing before the edge that samples it, and below that not even the
// threshold. A bench finding the real ceiling (docs/bench.md sections 4 and 13) has room up to
// 500 kHz. The setting is `swd.khz` (core/settings/keys.h, task p3-03), whose fallback is this
// default said in kHz - 125, and not the 100 the task named, because 0.125 MHz is the chip's own
// minimum.
constexpr std::uint32_t kDefaultHalfPeriodUs = 4;
constexpr std::uint32_t kMinHalfPeriodUs = 1;
constexpr std::uint32_t kMaxHalfPeriodUs = 1000;  // 500 Hz: slower than that is a mistake, not a setting

// The clock **in force**: `swd.khz` at the console (task p3-03), kDefaultHalfPeriodUs until a
// person sets one. One value, read by every Wire as it is built and by the flasher as it begins
// a plan, so that a setting reaches the console's own three wires, the update machine's flasher
// and the supervisor's without being threaded through three constructors that have no business
// knowing about settings. Set by core/console/console.cpp and by nothing else.
std::uint32_t halfPeriodInForceUs();
std::uint32_t setHalfPeriodInForceUs(std::uint32_t us);  // bounded as setHalfPeriodUs is

// A line reset is "holding the data signal HIGH for at least 50 clock cycles, followed by at
// least two idle cycles" (B4.3.3); an idle cycle is the line LOW (B4.1.4). Exactly the minimum
// on both counts: the specification says what the target counts, and a reset that did not take
// is retried (B4.3.3: "it must retry the reset sequence").
constexpr unsigned kLineResetClocks = 50;
constexpr unsigned kIdleAfterLineReset = 2;

// The JTAG-to-SWD select sequence, "0b0111 1001 1110 0111, most-significant-bit (MSB) first",
// which "can be represented as ... 0xE79E, transmitted least-significant-bit (LSB) first"
// (B5.2.2). Sent between two runs of at least 50 clocks HIGH (B5.2.2, steps 1 to 3). The
// nRF52840 has an SW-DP, not an SWJ-DP (Product Specification, "Debug and trace > DAP"), for
// which the sequence is harmless: "As long as the 50 cycles with SWDIOTMS HIGH sequence are sent
// first, the JTAG-to-SWD select sequence does not affect SW-DP" (B5.2.2).
constexpr std::uint16_t kJtagToSwd = 0xE79E;
constexpr unsigned kJtagToSwdBits = 16;

// After a transfer the host must start a new one at once, keep clocking idle cycles, or clock "at
// least 8 idle cycles" before it may stop the clock (B4.1.1). This host may stop at any time -
// the pins are released after a command - so every transfer ends with eight.
constexpr unsigned kIdleAfterTransfer = 8;

// When the acknowledge is not one of the three, the target may be in a data phase the host
// misread its way into: "it must not drive the line for at least the length of any potential
// data phase and then attempt a line reset" (B4.2.5, "Host response to protocol errors"; Table
// B4-6, "Back off because of possible data phase"). The longest potential data phase is a
// turnaround and 33 bits.
constexpr unsigned kBackOffClocks = 34;

// ACK[0:2], LSB first on the wire (B4.1.5: "the OK response of 0b001 appears on the wire as 1,
// followed by 0, followed by 0"). OK 0b001, WAIT 0b010, FAULT 0b100 (B4.2.1 to B4.2.4). All ones
// is what a released line with the target's pull-up reads as (B4.3.2) when nothing answers.
namespace ack {
constexpr std::uint8_t kOk = 0b001;
constexpr std::uint8_t kWait = 0b010;
constexpr std::uint8_t kFault = 0b100;
constexpr std::uint8_t kNone = 0b111;
}  // namespace ack

// What one transfer came to.
struct Transfer {
  bool lines = true;         // false: a pin operation was refused - the lines were taken away
  std::uint8_t ack = 0;      // the three bits as read
  std::uint32_t data = 0;    // read data, when ack is OK and it was a read
  bool parity_ok = true;     // of that read data (B4.1.6)
};

class Wire {
 public:
  // The default is the clock in force, evaluated at every construction: a `rak swd id` after a
  // `set swd.khz 250` runs at 250 kHz without the console saying so.
  Wire(board::SwdLines& lines, interfaces::Clock& clock,
       std::uint32_t half_period_us = halfPeriodInForceUs());

  // Bounded to [kMinHalfPeriodUs, kMaxHalfPeriodUs]; returns what is in force.
  std::uint32_t setHalfPeriodUs(std::uint32_t us);
  std::uint32_t halfPeriodUs() const { return half_us_; }
  std::uint32_t frequencyHz() const { return 1000000u / (2u * half_us_); }

  // The primitives. Each returns false the moment a pin operation is refused (the lines are not
  // held, or were taken by a kill: board::SwdLines), and nothing after that is attempted.
  bool lineReset(unsigned clocks = kLineResetClocks);  // SWDIO high for that many clocks
  bool jtagToSwd();                                    // the 16 bits, LSB first
  bool idle(unsigned clocks);                          // SWDIO low for that many clocks
  bool writeBits(std::uint32_t bits, unsigned count);  // 1..32 bits, LSB first, SWDIO driven
  bool readBits(unsigned count, std::uint32_t* out);   // 1..32 bits, LSB first, SWDIO an input
  bool turnaround();                                   // SWDIO released, one clock (B4.1.3)
  bool release();                                      // SWDIO an input, no clock
  bool backOff();                                      // kBackOffClocks with SWDIO released

  // One transfer (B4.2): the request, a turnaround, the acknowledge, and - on OK - the data
  // phase with its parity and turnaround. On WAIT or FAULT there is no data phase (overrun
  // detection is left off, B4.2.6). On any other acknowledge the host backs off. Ends with the
  // idle cycles and SWDIO released. `addr` is the register's A[3:2] as the byte offset 0x0, 0x4,
  // 0x8 or 0xC (B4.2: "A[2:3] ... giving the A[3:2] address field").
  Transfer transfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata);

  // The request byte, LSB first on the wire: start (1), APnDP, RnW, A[2], A[3], parity, stop
  // (0), park (1) (B4.2, "Packet request"); even parity over the four bits before it (B4.1.6).
  static std::uint8_t request(bool ap, bool read, std::uint8_t addr);
  // Even parity of 32 data bits (B4.1.6): 1 when the number of ones is odd.
  static bool parity32(std::uint32_t value);

  // Clocks produced since construction: the tests' evidence that every loop is bounded.
  std::uint64_t clocks() const { return clocks_; }
  interfaces::Clock& clock() { return clock_; }

 private:
  // One cycle: the low half, then high, then low again. A sample, when asked for, is taken at the
  // end of the low half, just before the rising edge - see the comment in wire.cpp.
  bool cycle(bool* sample = nullptr);
  bool driveBit(bool high);

  board::SwdLines& lines_;
  interfaces::Clock& clock_;
  std::uint32_t half_us_;
  std::uint64_t clocks_ = 0;
};

}  // namespace swd
}  // namespace core
