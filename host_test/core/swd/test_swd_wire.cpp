// The SWD wire against the simulated target (task p1-03): the request byte and the parities as
// ADIv5 B4 defines them, the bit timing as the fake pins recorded it, the half-period's bounds,
// what one transfer costs in clocks and microseconds, and what happens when the lines are not
// ours. The simulator is strict throughout: a wrong parity bit, a missing turnaround or a
// contention would fail these tests from underneath.
#include <doctest.h>

#include "core/board/pin_map.h"
#include "sim/swd_bench.h"

using core::board::kSwdClk;
using core::board::kSwdIo;
using core::board::Outcome;
namespace ack = core::swd::ack;
using core::swd::kDefaultHalfPeriodUs;
using core::swd::kIdleAfterTransfer;
using core::swd::kMaxHalfPeriodUs;
using core::swd::kMinHalfPeriodUs;
using core::swd::Transfer;
using core::swd::Wire;
using fakes::OpKind;
using interfaces::PinMode;

namespace {

int popcount(unsigned v) {
  int n = 0;
  for (; v != 0; v >>= 1) n += v & 1u;
  return n;
}

}  // namespace

TEST_CASE("the request byte: start, APnDP, RnW, A[2], A[3], even parity, stop, park - LSB first") {
  // Worked by hand from B4.2 ("Packet request") and B4.1.6: bit 0 is the start bit and goes
  // first on the wire (B4.1.5). A DPIDR read is 1,0,1,0,0,1,0,1 = 0xA5, the byte every SWD host
  // begins with.
  CHECK(Wire::request(false, true, 0x0) == 0xA5);   // DP read DPIDR
  CHECK(Wire::request(false, false, 0x0) == 0x81);  // DP write ABORT
  CHECK(Wire::request(false, true, 0x4) == 0x8D);   // DP read CTRL/STAT
  CHECK(Wire::request(false, false, 0x4) == 0xA9);  // DP write CTRL/STAT
  CHECK(Wire::request(false, false, 0x8) == 0xB1);  // DP write SELECT
  CHECK(Wire::request(false, true, 0x8) == 0x95);   // DP read RESEND
  CHECK(Wire::request(false, true, 0xC) == 0xBD);   // DP read RDBUFF
  CHECK(Wire::request(true, true, 0x0) == 0x87);    // AP read, A[3:2] = 0
  CHECK(Wire::request(true, false, 0xC) == 0xBB);   // AP write, A[3:2] = 3
  // All sixteen: the start bit set, the stop bit clear, the park bit set, and an even number of
  // ones over APnDP, RnW, A[2], A[3] and the parity bit together.
  for (int ap = 0; ap < 2; ++ap) {
    for (int read = 0; read < 2; ++read) {
      for (std::uint8_t addr = 0; addr < 16; addr += 4) {
        const std::uint8_t r = Wire::request(ap != 0, read != 0, addr);
        CHECK((r & 0x01) != 0);
        CHECK((r & 0x40) == 0);
        CHECK((r & 0x80) != 0);
        CHECK(popcount(r & 0x3E) % 2 == 0);
        CHECK(((r >> 1) & 1u) == static_cast<unsigned>(ap));
        CHECK(((r >> 2) & 1u) == static_cast<unsigned>(read));
        CHECK(((r >> 3) & 1u) == ((addr >> 2) & 1u));
        CHECK(((r >> 4) & 1u) == ((addr >> 3) & 1u));
      }
    }
  }
}

TEST_CASE("the data parity is even over the 32 bits (B4.1.6)") {
  CHECK(!Wire::parity32(0));
  CHECK(Wire::parity32(1));
  CHECK(Wire::parity32(0x80000000u));
  CHECK(!Wire::parity32(0xFFFFFFFFu));
  CHECK(Wire::parity32(0x7FFFFFFFu));
  CHECK(!Wire::parity32(0x2BA01477u));  // fourteen ones
  CHECK(Wire::parity32(0x2BA01476u));   // thirteen
}

TEST_CASE("the half-period is bounded on both sides and the default is 125 kHz") {
  sim::SwdBench b;
  CHECK(b.wire.halfPeriodUs() == kDefaultHalfPeriodUs);
  CHECK(b.wire.frequencyHz() == 125000);
  CHECK(b.wire.setHalfPeriodUs(0) == kMinHalfPeriodUs);
  CHECK(b.wire.frequencyHz() == 500000);
  CHECK(b.wire.setHalfPeriodUs(5) == 5);
  CHECK(b.wire.frequencyHz() == 100000);
  CHECK(b.wire.setHalfPeriodUs(1000000) == kMaxHalfPeriodUs);
  Wire slow(b.board.swd(), b.clock, 0);
  CHECK(slow.halfPeriodUs() == kMinHalfPeriodUs);
}

TEST_CASE("one transfer: 46 clocks of protocol and 8 idle, 432 us at the default speed") {
  // A read: 8 request + 1 turnaround + 3 acknowledge + 33 data + 1 turnaround = 46 (B4.2.2); a
  // write: 8 + 1 + 3 + 1 + 33 = 46 (B4.2.1); then the 8 idle cycles B4.1.1 wants before the
  // clock may stop. At a half-period of 4 us a clock is 8 us: 54 x 8 = 432 us.
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.strict = true;

  const std::uint64_t clocks = b.wire.clocks();
  const std::uint64_t us = b.clock.delayed_us;
  const Transfer read = b.wire.transfer(false, true, core::swd::dp::kCtrlStat, 0);
  CHECK(read.lines);
  CHECK(read.ack == ack::kOk);
  CHECK(read.parity_ok);
  CHECK(b.wire.clocks() - clocks == 46 + kIdleAfterTransfer);
  CHECK(b.clock.delayed_us - us == (46 + kIdleAfterTransfer) * 2 * kDefaultHalfPeriodUs);
  CHECK(b.target.counters().clocks == b.wire.clocks());

  const std::uint64_t clocks2 = b.wire.clocks();
  const Transfer write = b.wire.transfer(false, false, core::swd::dp::kSelect, 0x01000000);
  CHECK(write.lines);
  CHECK(write.ack == ack::kOk);
  CHECK(b.wire.clocks() - clocks2 == 46 + kIdleAfterTransfer);
  CHECK(b.target.select() == 0x01000000);

  // WAIT and FAULT: 8 + 1 + 3 + 1 = 13 and the idle (B4.2.3, B4.2.4: no data phase).
  b.target.wait_next = 1;
  const std::uint64_t clocks3 = b.wire.clocks();
  const Transfer waited = b.wire.transfer(false, false, core::swd::dp::kSelect, 0);
  CHECK(waited.ack == ack::kWait);
  CHECK(b.wire.clocks() - clocks3 == 13 + kIdleAfterTransfer);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("connect is 226 clocks: 50 high, 16 of switch, 50 high, 2 idle, DPIDR, SELECT") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  CHECK(b.wire.clocks() == 50 + 16 + 50 + 2 + 54 + 54);
  CHECK(b.clock.delayed_us == b.wire.clocks() * 2 * kDefaultHalfPeriodUs);
  b.release();
}

TEST_CASE("SWDIO changes only while SWCLK is low, and is an input whenever the target drives") {
  // The fake pins' log: every change of SWDIO's level or mode by the host happened with SWCLK
  // low - the target samples on the rising edge (B4.3.1) and the host changes after the falling
  // one. And the simulator, strict, would have failed the test on any contention.
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  b.pins.clearLog();
  REQUIRE(b.dp.connect().ok());
  REQUIRE(b.dp.powerUp().ok());
  bool clk = false;
  int io_changes = 0;
  int clk_rises = 0;
  for (const fakes::Transition& t : b.pins.log) {
    if (!t.changed) continue;
    if (t.op.gpio == kSwdClk.gpio && t.op.kind == OpKind::kLevel) {
      clk = t.op.level;
      clk_rises += clk;
      continue;
    }
    if (t.op.gpio == kSwdIo.gpio) {
      CHECK_MESSAGE(!clk, "SWDIO changed while SWCLK was high, log entry " << (&t - &b.pins.log[0]));
      io_changes++;
    }
  }
  CHECK(io_changes > 40);  // the switch sequence, two requests, the data phases: 56 as written
  CHECK(clk_rises == static_cast<int>(b.wire.clocks()));
  CHECK(b.pins.modeOf(kSwdIo.gpio) == PinMode::kInput);  // released after the last transfer
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("without the lines nothing is driven and every primitive says so") {
  sim::SwdBench b;
  // Not acquired: SwdLines refuses everything (task p0-03), and the wire reports it.
  CHECK(!b.wire.lineReset());
  CHECK(!b.wire.jtagToSwd());
  CHECK(!b.wire.idle(2));
  CHECK(!b.wire.writeBits(0xA5, 8));
  std::uint32_t bits = 0;
  CHECK(!b.wire.readBits(3, &bits));
  CHECK(!b.wire.turnaround());
  CHECK(!b.wire.backOff());
  const Transfer t = b.wire.transfer(false, true, 0, 0);
  CHECK(!t.lines);
  CHECK(b.wire.clocks() == 0);
  CHECK(b.pins.transitionsOf(kSwdClk.gpio).empty());
  CHECK(b.pins.transitionsOf(kSwdIo.gpio).empty());

  // Acquired and then taken away (a kill does this; here the release): the same answer, and the
  // debug port reports kLinesLost rather than anything about the target.
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.release();
  const core::swd::Result r = b.dp.readDp(core::swd::dp::kCtrlStat);
  CHECK(r.status == core::swd::Status::kLinesLost);
  CHECK(b.pins.modeOf(kSwdClk.gpio) == PinMode::kInput);
  CHECK(b.pins.modeOf(kSwdIo.gpio) == PinMode::kInput);
}

TEST_CASE("the lines taken by a kill in the middle of a transfer: kLinesLost, pins safe, no rule broken") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  // The fake clock's hook runs inside a delay - here, inside one of the wire's half-periods:
  // delayUs does not call it, so use a delayMs of the power-up poll: the first poll finds no
  // acknowledge, the millisecond's delay fires the hook, and the kill takes the lines.
  b.target.powerup_ack_after_reads = 3;
  bool killed = false;
  b.clock.on_delay = [&](std::uint32_t) {
    if (killed) return;
    killed = true;
    // A kill refuses while SWD is held? No: the kill takes the lines (ARCHITECTURE section 2
    // rule 3) and the holder's operations return false from then on.
    CHECK(b.board.killCycle(core::board::kKillTiming.min_ms) == Outcome::kOk);
  };
  const core::swd::Result r = b.dp.powerUp();
  CHECK(killed);
  CHECK(r.status == core::swd::Status::kLinesLost);
  CHECK(!b.board.swdHeld());
  CHECK(b.pins.modeOf(kSwdClk.gpio) == PinMode::kInput);
  CHECK(b.pins.modeOf(kSwdIo.gpio) == PinMode::kInput);
  CHECK(b.pins.violations.empty());
}
