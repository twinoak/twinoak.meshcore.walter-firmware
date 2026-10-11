// The simulator's own protocol checks, each fired once on purpose (task p1-03): a wrong request
// parity, a missing turnaround, SWDIO driven during the target's phase - and the rest of what it
// holds a host to: a stop or park bit wrong, a request before any line reset, a request other
// than DPIDR in the reset state, the clock stopped too soon, a wrong switch sequence. Each test
// composes the bad sequence from the wire's primitives, with the simulator's strictness off so
// that the refusal is recorded instead of failing the test, and reads the sentence back.
#include <doctest.h>

#include "core/board/pin_map.h"
#include "sim/swd_bench.h"

using core::board::Outcome;
namespace ack = core::swd::ack;
using core::swd::Transfer;
using core::swd::Wire;
namespace dpr = core::swd::dp;

TEST_CASE("a wrong request parity: the target does not answer and names the error") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.strict = false;
  // The DPIDR read with its parity bit flipped (bit 5 of the byte).
  const std::uint8_t bad = Wire::request(false, true, dpr::kDpidr) ^ 0x20;
  REQUIRE(b.wire.writeBits(bad, 8));
  REQUIRE(b.wire.turnaround());
  std::uint32_t got = 0;
  REQUIRE(b.wire.readBits(3, &got));
  CHECK(got == ack::kNone);  // "does not reply to the packet request and does not drive the line" (B4.2.5)
  CHECK(b.target.countErrors("wrong parity") == 1);
  REQUIRE(b.target.errors().size() == 1);
  MESSAGE("deliberate error 1, the simulator said: " << b.target.errors()[0].what);
  REQUIRE(b.wire.backOff());
  // A line reset puts it right (B4.2.5: "The target must leave the protocol error state on a
  // line reset"), and the sequence is clean again.
  b.target.clearErrors();
  b.target.strict = true;
  CHECK(b.dp.connect().ok());
  b.release();
}

TEST_CASE("a stop bit of 1 and a park bit of 0 are protocol errors too") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.strict = false;
  REQUIRE(b.wire.writeBits(Wire::request(false, true, dpr::kDpidr) | 0x40, 8));
  REQUIRE(b.wire.turnaround());
  std::uint32_t got = 0;
  REQUIRE(b.wire.readBits(3, &got));
  CHECK(got == ack::kNone);
  CHECK(b.target.countErrors("stop bit is 1") == 1);
  REQUIRE(b.wire.backOff());
  b.target.clearErrors();
  REQUIRE(b.dp.connect().ok());
  REQUIRE(b.wire.writeBits(Wire::request(false, true, dpr::kDpidr) & 0x7F, 8));
  REQUIRE(b.wire.turnaround());
  REQUIRE(b.wire.readBits(3, &got));
  CHECK(got == ack::kNone);
  CHECK(b.target.countErrors("park bit is 0") == 1);
  REQUIRE(b.wire.backOff());
  b.release();
}

TEST_CASE("a missing turnaround: write data driven straight after the acknowledge") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.strict = false;
  REQUIRE(b.wire.writeBits(Wire::request(false, false, dpr::kSelect), 8));
  REQUIRE(b.wire.turnaround());
  std::uint32_t got = 0;
  REQUIRE(b.wire.readBits(3, &got));
  CHECK(got == ack::kOk);
  // No turnaround here: the host drives WDATA[0] on the edge that should be nobody's (B4.1.3).
  REQUIRE(b.wire.writeBits(0x01000000, 32));
  REQUIRE(b.wire.writeBits(Wire::parity32(0x01000000) ? 1u : 0u, 1));
  REQUIRE(b.target.countErrors("missing turnaround") == 1);
  MESSAGE("deliberate error 2, the simulator said: " << b.target.errors()[0].what);
  // The target then sampled the data one bit late: what it took in is not what was sent.
  CHECK(b.target.select() != 0x01000000);
  REQUIRE(b.wire.idle(8));
  b.target.clearErrors();
  b.target.strict = true;
  CHECK(b.dp.connect().ok());
  b.release();
}

TEST_CASE("SWDIO driven during the target's phase: contention, named with the bit") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.strict = false;
  REQUIRE(b.wire.writeBits(Wire::request(false, true, dpr::kDpidr), 8));
  // The host keeps driving through the turnaround and into the acknowledge.
  REQUIRE(b.wire.idle(2));
  REQUIRE(b.target.countErrors("while the target drives ACK[0]") == 1);
  CHECK(b.target.countErrors("while the target drives ACK[1]") == 1);
  MESSAGE("deliberate error 3, the simulator said: " << b.target.errors()[0].what);
  // Let the transfer finish properly so the target is back in step: ACK[2], the 32 data bits,
  // the parity, the edge where the target lets go, the turnaround, then idle.
  std::uint32_t got = 0;
  REQUIRE(b.wire.readBits(1, &got));
  REQUIRE(b.wire.readBits(32, &got));
  REQUIRE(b.wire.readBits(1, &got));
  REQUIRE(b.wire.turnaround());
  REQUIRE(b.wire.turnaround());
  REQUIRE(b.wire.idle(8));
  CHECK(b.target.errors().size() == 2);
  b.target.clearErrors();
  b.target.strict = true;
  CHECK(b.dp.readDp(dpr::kDpidr).ok());
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("a request before any line reset is the host's error") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  b.target.strict = false;
  const Transfer t = b.wire.transfer(false, true, dpr::kDpidr, 0);
  CHECK(t.lines);
  CHECK(t.ack == ack::kNone);
  CHECK(b.target.countErrors("before any line reset") == 1);
  b.release();
  // Told not to insist, another one answers like a chip that was simply powered on.
  sim::SwdBench lenient;
  lenient.target.requires_line_reset = false;
  REQUIRE(lenient.acquire() == Outcome::kOk);
  const Transfer t2 = lenient.wire.transfer(false, true, dpr::kDpidr, 0);
  CHECK(t2.ack == ack::kOk);
  CHECK(t2.data == 0x2BA01477);
  CHECK(lenient.target.errors().empty());
  lenient.release();
}

TEST_CASE("in the reset state only a DPIDR read is valid") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  b.target.strict = false;
  REQUIRE(b.wire.lineReset());
  REQUIRE(b.wire.idle(2));
  CHECK(b.target.inResetState());
  const Transfer t = b.wire.transfer(false, true, dpr::kCtrlStat, 0);
  CHECK(t.ack == ack::kNone);
  CHECK(b.target.countErrors("other than a DPIDR read in the reset state") == 1);
  // The host backed off with the line released - 34 ones, which a target waiting for a header
  // takes for a bad one - so it is out of step until the next line reset, as a real one is.
  CHECK(!b.target.inResetState());
  REQUIRE(b.wire.lineReset());
  REQUIRE(b.wire.idle(2));
  CHECK(b.target.inResetState());
  const Transfer id = b.wire.transfer(false, true, dpr::kDpidr, 0);
  CHECK(id.ack == ack::kOk);
  CHECK(!b.target.inResetState());
  b.release();
}

TEST_CASE("the clock stopped fewer than 8 idle cycles after a transfer") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.strict = false;
  REQUIRE(b.wire.writeBits(Wire::request(false, true, dpr::kDpidr), 8));
  REQUIRE(b.wire.turnaround());
  std::uint32_t got = 0;
  REQUIRE(b.wire.readBits(3, &got));
  REQUIRE(b.wire.readBits(32, &got));
  REQUIRE(b.wire.readBits(1, &got));
  REQUIRE(b.wire.turnaround());
  REQUIRE(b.wire.idle(3));
  b.release();  // SWCLK goes back to an input: the clock has stopped
  CHECK(b.target.countErrors("the clock stopped 3 idle cycles") == 1);
}

TEST_CASE("a JTAG target ignores a wrong switch sequence, and a line reset without the switch") {
  sim::SwdBench b;
  b.target.startInJtag();
  REQUIRE(b.acquire() == Outcome::kOk);
  b.target.strict = false;
  REQUIRE(b.wire.lineReset());
  REQUIRE(b.wire.writeBits(core::swd::kJtagToSwd ^ 1u, 16));
  REQUIRE(b.wire.lineReset());
  REQUIRE(b.wire.idle(2));
  CHECK(!b.target.swdSelected());
  CHECK(b.target.counters().switches_to_swd == 0);
  CHECK(b.target.counters().line_resets == 0);
  const Transfer t = b.wire.transfer(false, true, dpr::kDpidr, 0);
  CHECK(t.ack == ack::kNone);
  // The right one, and it answers.
  REQUIRE(b.wire.lineReset());
  REQUIRE(b.wire.jtagToSwd());
  REQUIRE(b.wire.lineReset());
  REQUIRE(b.wire.idle(2));
  CHECK(b.target.swdSelected());
  const Transfer id = b.wire.transfer(false, true, dpr::kDpidr, 0);
  CHECK(id.ack == ack::kOk);
  CHECK(id.data == 0x2BA01477);
  b.release();
}

TEST_CASE("the stub access port stores what is written and the target counts the traffic") {
  sim::StubAp ap;
  ap.write(0x04, 7);
  CHECK(ap.read(0x04) == 7);
  CHECK(ap.read(0xFC) == 0);
  CHECK(ap.reads == 2);
  CHECK(ap.writes == 1);
}
