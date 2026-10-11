// The debug port against the simulated target (task p1-03): connect and what the target saw of
// it, a line reset one clock short, every DP register read and written, the posted AP read made
// to look like a plain one, SELECT written only when it changes, WAIT a few times and WAIT for
// ever, FAULT with its sticky flags read and cleared, one bad parity, silence, no target at all,
// the power-up handshake slow and never, a target that starts in JTAG. The fake pins check the
// pin rules under all of it and the simulator checks the protocol.
#include <doctest.h>

#include "core/board/pin_map.h"
#include "sim/swd_bench.h"

using core::board::kSwdClk;
using core::board::kSwdIo;
using core::board::Outcome;
using core::swd::Result;
using core::swd::Status;
namespace dpr = core::swd::dp;
namespace cs = core::swd::ctrl_stat;
using interfaces::PinMode;

TEST_CASE("connect: DPIDR read, and the target saw a legal sequence") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  const Result r = b.dp.connect();
  CHECK(r.ok());
  CHECK(r.value == sim::kNrf52840Dpidr);
  CHECK(b.dp.dpidr() == 0x2BA01477);
  CHECK(b.dp.connected());
  const sim::SwdTarget::Counters& c = b.target.counters();
  CHECK(b.target.errors().empty());
  CHECK(c.switches_to_swd == 1);  // 0xE79E after 50 highs (B5.2.2), seen even by an SW-DP
  CHECK(c.line_resets == 1);      // the second run of 50 highs, with its two idle cycles
  CHECK(c.dpidr_reads == 1);
  CHECK(c.select_writes == 1);    // SELECT = 0, so that CTRL/STAT's bank is the one selected
  CHECK(c.requests == 2);
  CHECK(c.ok == 2);
  CHECK(!b.target.inResetState());
  CHECK(b.target.everReset());
  // And decodes as Arm's Cortex-M4 SW-DP: revision 2, part 0xBA, DPv1, designer 0x23B.
  const core::swd::DpidrFields f = core::swd::decodeDpidr(r.value);
  CHECK(f.revision == 2);
  CHECK(f.partno == 0xBA);
  CHECK(!f.min);
  CHECK(f.version == 1);
  CHECK(f.designer == core::swd::kDesignerArm);
  b.release();
}

TEST_CASE("a line reset one clock short is refused by the target, and connect says no target") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  b.target.strict = false;  // the refusal is what this test is about
  b.dp.setLineResetClocksForTest(49);
  const Result r = b.dp.connect();
  CHECK(r.status == Status::kNoTarget);
  CHECK(!b.dp.connected());
  CHECK(b.target.countErrors("a line reset of 49 clocks") >= 1);
  CHECK(b.target.counters().short_line_resets >= 1);
  CHECK(b.target.counters().line_resets == 0);
  CHECK(b.target.counters().dpidr_reads == 0);
  CHECK(!b.target.everReset());
  // Two attempts (B4.3.3: the reset sequence is retried once), then it gives up.
  CHECK(b.dp.counters().transfers == 2);
  // With the full 50 the same target connects.
  b.target.clearErrors();
  b.target.strict = true;
  b.dp.setLineResetClocksForTest(50);
  CHECK(b.dp.connect().ok());
  b.release();
}

TEST_CASE("every DP register, read and written") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());

  // DPIDR (read, 0x0)
  Result r = b.dp.readDp(dpr::kDpidr);
  CHECK(r.ok());
  CHECK(r.value == 0x2BA01477);

  // CTRL/STAT (0x4): the requests written come back, the acknowledges with them.
  r = b.dp.readDp(dpr::kCtrlStat);
  CHECK(r.ok());
  CHECK((r.value & cs::kPowerUpAcks) == 0);
  r = b.dp.writeDp(dpr::kCtrlStat, cs::kPowerUpRequests);
  CHECK(r.ok());
  r = b.dp.readDp(dpr::kCtrlStat);
  CHECK(r.ok());
  CHECK((r.value & cs::kPowerUpRequests) == cs::kPowerUpRequests);
  CHECK((r.value & cs::kPowerUpAcks) == cs::kPowerUpAcks);

  // SELECT (write, 0x8): the target holds it; RESEND (read, 0x8) repeats the last value sent.
  r = b.dp.writeDp(dpr::kSelect, 0x01000050);
  CHECK(r.ok());
  CHECK(b.target.select() == 0x01000050);
  r = b.dp.writeDp(dpr::kSelect, 0);
  CHECK(r.ok());

  // ABORT (write, 0x0): clears the sticky flags.
  b.target.setStickyError();
  CHECK((b.target.ctrlStat() & cs::kStickyErr) != 0);
  r = b.dp.clearErrors();
  CHECK(r.ok());
  CHECK((b.target.ctrlStat() & cs::kStickyErr) == 0);

  // RDBUFF (read, 0xC) and RESEND after an AP read: below, with the AP.
  // Not an address: refused before anything goes on the wire.
  const std::uint32_t transfers = b.dp.counters().transfers;
  CHECK(b.dp.readDp(0x1).status == Status::kBadAddress);
  CHECK(b.dp.writeDp(0x10, 0).status == Status::kBadAddress);
  CHECK(b.dp.readAp(0, 0x3).status == Status::kBadAddress);
  CHECK(b.dp.writeAp(0, 0x5, 0).status == Status::kBadAddress);
  CHECK(b.dp.counters().transfers == transfers);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("AP reads are posted and the API hides it; SELECT is written only when it changes") {
  sim::SwdBench b;
  sim::StubAp ap0, ap1;
  ap0.regs[0x04 >> 2] = 0x11111111;
  ap0.regs[0x08 >> 2] = 0x22222222;
  ap0.regs[0x4C >> 2] = 0x33333333;  // bank 4
  ap1.regs[0x00 >> 2] = 0x44444444;
  b.target.attachAp(0, ap0);
  b.target.attachAp(1, ap1);
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  REQUIRE(b.dp.powerUp().ok());
  const std::uint32_t selects = b.target.counters().select_writes;

  // Each read returns the register it asked for, not the one before it (B4.2.2: "the result
  // of the access is returned on the next transfer"): the API collects it through RDBUFF.
  Result r = b.dp.readAp(0, 0x04);
  CHECK(r.ok());
  CHECK(r.value == 0x11111111);
  r = b.dp.readAp(0, 0x08);
  CHECK(r.ok());
  CHECK(r.value == 0x22222222);
  CHECK(b.target.counters().select_writes == selects);  // same port, same bank: no SELECT write
  CHECK(b.target.counters().rdbuff_reads == 2);
  CHECK(b.target.counters().ap_reads == 2);

  r = b.dp.readAp(0, 0x4C);
  CHECK(r.ok());
  CHECK(r.value == 0x33333333);
  CHECK(b.target.counters().select_writes == selects + 1);  // the bank changed
  CHECK(b.target.select() == core::swd::select::value(0, 4, 0));

  r = b.dp.readAp(1, 0x00);
  CHECK(r.ok());
  CHECK(r.value == 0x44444444);
  CHECK(b.target.counters().select_writes == selects + 2);  // the port changed

  r = b.dp.readAp(1, 0x00);
  CHECK(r.ok());
  CHECK(b.target.counters().select_writes == selects + 2);  // nothing changed

  // Writes, and the same rule.
  r = b.dp.writeAp(0, 0x10, 0xCAFEF00D);
  CHECK(r.ok());
  CHECK(ap0.regs[0x10 >> 2] == 0xCAFEF00D);
  CHECK(b.target.counters().select_writes == selects + 3);  // port 1 bank 0 -> port 0 bank 1
  r = b.dp.writeAp(0, 0x1C, 0x00000001);
  CHECK(r.ok());
  CHECK(ap0.regs[0x1C >> 2] == 1);
  CHECK(b.target.counters().select_writes == selects + 3);
  CHECK(b.dp.counters().select_writes == selects + 3);

  // An AP that is not there reads as zero and ignores writes (B2.2.9, APSEL).
  r = b.dp.readAp(7, 0x00);
  CHECK(r.ok());
  CHECK(r.value == 0);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("an AP access before the power-up is a FAULT the target explains with STICKYERR") {
  sim::SwdBench b;
  sim::StubAp ap0;
  b.target.attachAp(0, ap0);
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  Result r = b.dp.readAp(0, 0x00);
  CHECK(r.status == Status::kFault);
  CHECK((r.ctrl_stat & cs::kStickyErr) != 0);
  CHECK((b.target.ctrlStat() & cs::kStickyMask) == 0);  // cleared through ABORT already
  REQUIRE(b.dp.powerUp().ok());
  r = b.dp.readAp(0, 0x00);
  CHECK(r.ok());
  b.release();
}

TEST_CASE("WAIT three times, then OK: three retries, the operation done once") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.wait_next = 3;
  const Result r = b.dp.writeDp(dpr::kSelect, 0x05000000);
  CHECK(r.ok());
  CHECK(b.target.select() == 0x05000000);
  CHECK(b.dp.counters().wait_retries == 3);
  CHECK(b.dp.counters().wait_timeouts == 0);
  CHECK(b.target.counters().waits == 3);
  CHECK(b.target.counters().select_writes == 2);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("WAIT for ever: bounded, then an error, and the SELECT cache is not trusted") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.wait_next = 1000;
  const std::uint32_t before = b.dp.counters().transfers;
  const Result r = b.dp.writeDp(dpr::kSelect, 0x06000000);
  CHECK(r.status == Status::kWaitTimeout);
  CHECK(r.ack == core::swd::ack::kWait);
  CHECK(b.dp.counters().transfers - before == core::swd::kWaitRetries + 1);
  CHECK(b.dp.counters().wait_retries == core::swd::kWaitRetries);
  CHECK(b.dp.counters().wait_timeouts == 1);
  CHECK(b.target.select() == 0);
  // Once the target is ready again, the next AP access rewrites SELECT: the cache knew the write
  // had not happened.
  b.target.wait_next = 0;
  sim::StubAp ap6;
  b.target.attachAp(6, ap6);
  REQUIRE(b.dp.powerUp().ok());
  const std::uint32_t selects = b.target.counters().select_writes;
  CHECK(b.dp.readAp(6, 0x00).ok());
  CHECK(b.target.counters().select_writes == selects + 1);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("FAULT: reported with the sticky flags, which are then cleared") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.setStickyError();
  const Result r = b.dp.writeDp(dpr::kSelect, 0x07000000);
  CHECK(r.status == Status::kFault);
  CHECK(r.ack == core::swd::ack::kFault);
  CHECK((r.ctrl_stat & cs::kStickyErr) != 0);
  CHECK(b.dp.counters().faults == 1);
  CHECK(b.target.counters().abort_writes == 1);
  CHECK((b.target.ctrlStat() & cs::kStickyMask) == 0);
  CHECK(b.target.select() == 0);  // the faulted write did not happen (B4.2.4)
  // The next access goes through.
  const Result again = b.dp.writeDp(dpr::kSelect, 0x08000000);
  CHECK(again.ok());
  CHECK(b.target.select() == 0x08000000);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("one bad parity: that read fails and is counted; the next is good") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  b.target.bad_read_parity_next = 1;
  Result r = b.dp.readDp(dpr::kCtrlStat);
  CHECK(r.status == Status::kParityError);
  CHECK(b.dp.counters().parity_errors == 1);
  r = b.dp.readDp(dpr::kCtrlStat);
  CHECK(r.ok());
  CHECK(b.dp.counters().parity_errors == 1);
  // A bad parity on the DPIDR read of connect(): retried once, and then good.
  b.target.bad_read_parity_next = 1;
  r = b.dp.connect();
  CHECK(r.ok());
  CHECK(b.dp.counters().parity_errors == 2);
  CHECK(b.target.counters().dpidr_reads == 3);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("silence: no target, two attempts, bounded, nothing else") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  b.target.silent = true;
  const Result r = b.dp.connect();
  CHECK(r.status == Status::kNoTarget);
  CHECK(r.ack == core::swd::ack::kNone);
  CHECK(!b.dp.connected());
  CHECK(b.dp.counters().no_target == 2);
  CHECK(b.dp.counters().transfers == 2);
  CHECK(b.target.counters().requests == 2);  // it heard both DPIDR reads and said nothing
  CHECK(b.target.counters().dpidr_reads == 0);
  // Each attempt: 118 clocks of resets, 12 of request and acknowledge, 34 of back-off.
  CHECK(b.wire.clocks() == 2 * (118 + 12 + 34));
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("not there at all: no pull-up, the line keeps what we drove - no target") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  b.target.present = false;
  const Result r = b.dp.connect();
  CHECK(r.status == Status::kNoTarget);
  CHECK(r.ack == core::swd::ack::kNone);  // the park bit's high is what the line still holds
  CHECK(b.dp.counters().no_target == 2);
  CHECK(b.target.errors().empty());
  b.release();
  CHECK(b.pins.modeOf(kSwdClk.gpio) == PinMode::kInput);
  CHECK(b.pins.modeOf(kSwdIo.gpio) == PinMode::kInput);
}

TEST_CASE("a confused line: an acknowledge that is none of the three is a protocol error") {
  sim::SwdBench b;
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  // The line stuck low: a target that answers nothing, and no pull-up but a short to ground.
  b.target.silent = true;
  b.pins.setExternalLevel(kSwdIo.gpio, false);
  const Result r = b.dp.readDp(dpr::kCtrlStat);
  CHECK(r.status == Status::kProtocolError);
  CHECK(r.ack == 0);
  CHECK(b.dp.counters().protocol_errors == 1);
  b.release();
}

TEST_CASE("the power-up handshake: at once, after a few reads, and never") {
  SUBCASE("at once") {
    sim::SwdBench b;
    REQUIRE(b.acquire() == Outcome::kOk);
    REQUIRE(b.dp.connect().ok());
    const Result r = b.dp.powerUp();
    CHECK(r.ok());
    CHECK((r.ctrl_stat & cs::kPowerUpAcks) == cs::kPowerUpAcks);
    CHECK((r.value & cs::kPowerUpRequests) == cs::kPowerUpRequests);
    CHECK(b.target.counters().ctrl_stat_reads == 1);
    const Result down = b.dp.powerDown();
    CHECK(down.ok());
    CHECK((b.target.ctrlStat() & (cs::kPowerUpRequests | cs::kPowerUpAcks)) == 0);
    b.release();
  }
  SUBCASE("after five reads, a millisecond apart") {
    sim::SwdBench b;
    REQUIRE(b.acquire() == Outcome::kOk);
    REQUIRE(b.dp.connect().ok());
    b.target.powerup_ack_after_reads = 5;
    const std::uint64_t ms = b.clock.now_ms;
    const Result r = b.dp.powerUp();
    CHECK(r.ok());
    CHECK(b.target.counters().ctrl_stat_reads == 6);
    // Five milliseconds of waiting, plus the six reads' own 432 us each on the fake clock.
    CHECK(b.clock.now_ms - ms >= 5);
    CHECK(b.clock.now_ms - ms <= 9);
    b.release();
  }
  SUBCASE("never: bounded, then kPowerUpTimeout with the last CTRL/STAT") {
    sim::SwdBench b;
    REQUIRE(b.acquire() == Outcome::kOk);
    REQUIRE(b.dp.connect().ok());
    b.target.powers_up = false;
    const std::uint64_t ms = b.clock.now_ms;
    const Result r = b.dp.powerUp();
    CHECK(r.status == Status::kPowerUpTimeout);
    CHECK((r.ctrl_stat & cs::kPowerUpAcks) == 0);
    CHECK((r.ctrl_stat & cs::kPowerUpRequests) == cs::kPowerUpRequests);
    CHECK(b.target.counters().ctrl_stat_reads == core::swd::kPowerUpPolls);
    // 99 waits of a millisecond, plus 100 reads of 432 us: about 142 ms, and bounded.
    CHECK(b.clock.now_ms - ms >= core::swd::kPowerUpPolls - 1);
    CHECK(b.clock.now_ms - ms <= core::swd::kPowerUpPolls + 50);
    CHECK(b.clock.delayed_ms == core::swd::kPowerUpPolls - 1);
    CHECK(b.target.errors().empty());
    b.release();
  }
}

TEST_CASE("an SWJ-DP that starts in JTAG: only the switch sequence reaches it, and it does") {
  sim::SwdBench b;
  b.target.startInJtag();
  REQUIRE(b.acquire() == Outcome::kOk);
  CHECK(!b.target.swdSelected());
  const Result r = b.dp.connect();
  CHECK(r.ok());
  CHECK(r.value == 0x2BA01477);
  CHECK(b.target.swdSelected());
  CHECK(b.target.counters().switches_to_swd == 1);
  CHECK(b.target.errors().empty());
  b.release();
}

TEST_CASE("every loop in the debug port is bounded: the worst case in clocks") {
  // WAIT for ever on an AP read after a FAULT on its SELECT... the largest single operation is
  // readAp with every transfer WAITing: SELECT (11 transfers), and that is it - the first
  // failure ends the operation. Counted, so that a change here is noticed.
  sim::SwdBench b;
  sim::StubAp ap;
  b.target.attachAp(3, ap);
  REQUIRE(b.acquire() == Outcome::kOk);
  REQUIRE(b.dp.connect().ok());
  REQUIRE(b.dp.powerUp().ok());
  b.target.wait_next = 1000000;
  const std::uint64_t clocks = b.wire.clocks();
  const Result r = b.dp.readAp(3, 0x00);
  CHECK(r.status == Status::kWaitTimeout);
  CHECK(b.wire.clocks() - clocks == (core::swd::kWaitRetries + 1) * (13 + core::swd::kIdleAfterTransfer));
  b.release();
}
