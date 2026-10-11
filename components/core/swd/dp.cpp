#include "core/swd/dp.h"

namespace core {
namespace swd {

const char* describe(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kLinesLost: return "the SWD lines are not ours (not acquired, or a kill took them)";
    case Status::kNoTarget: return "no target: nothing answered on the line";
    case Status::kProtocolError: return "protocol error: an acknowledge that is none of OK, WAIT or FAULT";
    case Status::kWaitTimeout: return "the target kept answering WAIT";
    case Status::kFault: return "FAULT: a sticky error flag was set (read, then cleared)";
    case Status::kParityError: return "parity error on the data read";
    case Status::kPowerUpTimeout: return "the power-up was not acknowledged in time";
    case Status::kBadAddress: return "not a register address";
  }
  return "?";
}

DpidrFields decodeDpidr(std::uint32_t dpidr) {
  DpidrFields f;
  f.revision = static_cast<std::uint8_t>((dpidr >> 28) & 0xF);
  f.partno = static_cast<std::uint8_t>((dpidr >> 20) & 0xFF);
  f.min = ((dpidr >> 16) & 1u) != 0;
  f.version = static_cast<std::uint8_t>((dpidr >> 12) & 0xF);
  f.designer = static_cast<std::uint16_t>((dpidr >> 1) & 0x7FF);
  return f;
}

namespace {

bool dpAddress(std::uint8_t addr) {
  return addr == dp::kDpidr || addr == dp::kCtrlStat || addr == dp::kSelect || addr == dp::kRdbuff;
}

Result failed(Status status, std::uint8_t ack = 0) {
  Result r;
  r.status = status;
  r.ack = ack;
  return r;
}

}  // namespace

// The transfer as the wire did it, mapped to a status and nothing more.
Result Dp::rawTransfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata) {
  counters_.transfers++;
  const Transfer t = wire_.transfer(ap, read, addr, wdata);
  Result r;
  r.ack = t.ack;
  if (!t.lines) return failed(Status::kLinesLost, t.ack);
  switch (t.ack) {
    case ack::kOk:
      if (read && !t.parity_ok) {
        counters_.parity_errors++;
        return failed(Status::kParityError, t.ack);
      }
      r.value = t.data;
      return r;
    case ack::kWait: return failed(Status::kWaitTimeout, t.ack);  // the caller counts the retries
    case ack::kFault: return failed(Status::kFault, t.ack);
    case ack::kNone:
      counters_.no_target++;
      return failed(Status::kNoTarget, t.ack);
    default:
      counters_.protocol_errors++;
      return failed(Status::kProtocolError, t.ack);
  }
}

// WAIT: the same request again, kWaitRetries times, with the transfer's own eight idle cycles
// between (B4.2.3: "when a debugger receives a WAIT response it retries the same operation").
// FAULT: CTRL/STAT read - a read that never WAITs or FAULTs (B4.2.3, B4.2.4) - then ABORT written
// to clear the sticky flags (B4.2.4: "the sticky error flags are cleared to 0b0 by writing bits in
// the ABORT register"), and the flags reported.
Result Dp::transfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata) {
  Result r;
  for (unsigned attempt = 0; attempt <= kWaitRetries; ++attempt) {
    r = rawTransfer(ap, read, addr, wdata);
    if (r.ack != ack::kWait) break;
    if (attempt < kWaitRetries) counters_.wait_retries++;
  }
  if (r.ack == ack::kWait) {
    counters_.wait_timeouts++;
    return failed(Status::kWaitTimeout, r.ack);
  }
  if (r.status == Status::kFault) {
    counters_.faults++;
    Result stat = rawTransfer(false, true, dp::kCtrlStat, 0);
    if (stat.status == Status::kLinesLost) return stat;
    const Result cleared = rawTransfer(false, false, dp::kAbort, abort::kClearAll);
    if (cleared.status == Status::kLinesLost) return cleared;
    r.ctrl_stat = stat.ok() ? stat.value & ctrl_stat::kStickyMask : 0;
  }
  return r;
}

// B5.2.2 steps 1 to 3 and B4.3.3: at least 50 clocks high, the 16-bit switch, at least 50 clocks
// high, at least two idle cycles, then a DPIDR read - "the only valid transactions in reset state"
// are that read and the switch sequence, and the read "takes the connection out of reset state".
// No response to the DPIDR read means the sequence is retried once (B4.3.3: "it must retry the
// reset sequence, because the target might have been in a state where ... it treated the initial
// line reset as a data phase"). Then SELECT is written to zero so that the bank CTRL/STAT lives
// in is the one selected and the cached value is known: a line reset does not reset SELECT
// (B4.3.3: "Other SW-DP registers are reset only by a powerup reset").
Result Dp::connect() {
  connected_ = false;
  select_known_ = false;
  counters_.connects++;
  Result r;
  for (unsigned attempt = 0; attempt < kConnectAttempts; ++attempt) {
    if (!wire_.lineReset(line_reset_clocks_) || !wire_.jtagToSwd() ||
        !wire_.lineReset(line_reset_clocks_) || !wire_.idle(kIdleAfterLineReset)) {
      return failed(Status::kLinesLost);
    }
    r = rawTransfer(false, true, dp::kDpidr, 0);
    if (r.status == Status::kLinesLost) return r;
    if (r.ok()) break;
    // kNoTarget, kParityError, kProtocolError, and a WAIT or FAULT the DP must not give to this
    // read: all of them are "not the expected response", and all get the one retry.
  }
  if (!r.ok()) {
    if (r.status == Status::kWaitTimeout || r.status == Status::kFault) {
      // A DPIDR read never WAITs or FAULTs (B4.2.3, B4.2.4): a DP that does is not following
      // the protocol.
      counters_.protocol_errors++;
      return failed(Status::kProtocolError, r.ack);
    }
    return r;
  }
  dpidr_ = r.value;
  const Result sel = writeDp(dp::kSelect, 0);
  if (!sel.ok()) return sel;
  connected_ = true;
  r.value = dpidr_;
  return r;
}

Result Dp::readDp(std::uint8_t addr) {
  if (!dpAddress(addr)) return failed(Status::kBadAddress);
  return transfer(false, true, addr, 0);
}

Result Dp::writeDp(std::uint8_t addr, std::uint32_t value) {
  if (!dpAddress(addr)) return failed(Status::kBadAddress);
  const Result r = transfer(false, false, addr, value);
  if (addr == dp::kSelect) {
    counters_.select_writes++;
    // What the DP holds now, whatever the answer: after a WAIT that timed out the write did not
    // happen (B4.2.3), after a FAULT it did not either (B4.2.4), so the cache is only trusted on
    // OK.
    select_known_ = r.ok();
    select_ = value;
  }
  return r;
}

Result Dp::ensureSelect(std::uint8_t ap, std::uint8_t ap_bank, std::uint8_t dp_bank) {
  const std::uint32_t wanted = select::value(ap, ap_bank, dp_bank);
  if (select_known_ && select_ == wanted) {
    Result ok;
    return ok;
  }
  return writeDp(dp::kSelect, wanted);
}

// An AP read is posted: "the result of the access is returned on the next transfer. This result
// can be another AP register read, or a DP register read of RDBUFF" (B4.2.2). One register at a
// time here: the AP read, its data phase discarded, then RDBUFF, which "presents data that was
// captured during the previous AP read" (B2.2.7). The address is the register's byte offset in
// the AP, 0x00 to 0xFC: bits [7:4] select the bank through SELECT.APBANKSEL, bits [3:2] go on
// the wire (B4.2.8, "Target SW-DP responses to AP transaction requests").
Result Dp::readAp(std::uint8_t ap, std::uint8_t addr) {
  if ((addr & 0x3) != 0) return failed(Status::kBadAddress);
  Result r = ensureSelect(ap, static_cast<std::uint8_t>(addr >> 4), 0);
  if (!r.ok()) return r;
  r = transfer(true, true, static_cast<std::uint8_t>(addr & 0xC), 0);
  if (!r.ok()) return r;
  return transfer(false, true, dp::kRdbuff, 0);
}

Result Dp::writeAp(std::uint8_t ap, std::uint8_t addr, std::uint32_t value) {
  if ((addr & 0x3) != 0) return failed(Status::kBadAddress);
  const Result r = ensureSelect(ap, static_cast<std::uint8_t>(addr >> 4), 0);
  if (!r.ok()) return r;
  return transfer(true, false, static_cast<std::uint8_t>(addr & 0xC), value);
}

Result Dp::clearErrors() { return writeDp(dp::kAbort, abort::kClearAll); }

// B2.3.2: both requests at once (CSYSPWRUPREQ without CDBGPWRUPREQ is UNPREDICTABLE), then
// CTRL/STAT read until both acknowledges are up - "Arm strongly recommends that tools only
// initiate an AP transfer when CDBGPWRUPREQ and CDBGPWRUPACK are asserted". Bounded by
// kPowerUpPolls, a millisecond apart after the first.
Result Dp::powerUp() {
  Result r = ensureSelect(0, 0, 0);
  if (!r.ok()) return r;
  r = writeDp(dp::kCtrlStat, ctrl_stat::kPowerUpRequests);
  if (!r.ok()) return r;
  return pollPowerUp(ctrl_stat::kPowerUpAcks);
}

// The requests withdrawn, and the acknowledges waited out: "tools must wait until CxxxPWRUPACK
// is LOW before making a new request for powerup" (B2.3.2). On the nRF52840 this is also what
// ends debug interface mode (Product Specification, "Debug and trace > Debug Interface mode":
// the device stays in it "as long as the debugger is requesting power via CxxxPWRUPREQ").
Result Dp::powerDown() {
  Result r = ensureSelect(0, 0, 0);
  if (!r.ok()) return r;
  r = writeDp(dp::kCtrlStat, 0);
  if (!r.ok()) return r;
  return pollPowerUp(0);
}

Result Dp::pollPowerUp(std::uint32_t wanted_acks) {
  Result r;
  for (unsigned poll = 0; poll < kPowerUpPolls; ++poll) {
    if (poll > 0) wire_.clock().delayMs(1);
    r = readDp(dp::kCtrlStat);
    if (!r.ok()) return r;
    r.ctrl_stat = r.value;
    if ((r.value & ctrl_stat::kPowerUpAcks) == wanted_acks) return r;
  }
  Result timeout = failed(Status::kPowerUpTimeout, r.ack);
  timeout.ctrl_stat = r.value;
  timeout.value = r.value;
  return timeout;
}

}  // namespace swd
}  // namespace core
