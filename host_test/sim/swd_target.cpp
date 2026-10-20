#include "sim/swd_target.h"

#include <doctest.h>

#include <cstdio>

#include "core/board/pin_map.h"
#include "core/swd/dp.h"
#include "core/swd/wire.h"

namespace sim {

using core::board::kNReset;
using core::board::kSwdClk;
using core::board::kSwdIo;
namespace ack = core::swd::ack;
namespace cs = core::swd::ctrl_stat;
namespace ab = core::swd::abort;

namespace {

bool parity32(std::uint32_t v) { return core::swd::Wire::parity32(v); }

std::string hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(v));
  return buf;
}

}  // namespace

SwdTarget::SwdTarget(fakes::FakePins& pins) : pins_(pins) {
  // The target's own 100 k pull-up (B4.3.2) is what an undriven SWDIO reads as.
  pins_.setExternalLevel(kSwdIo.gpio, true);
}

// ---- the pins, forwarded and watched ------------------------------------------------------

bool SwdTarget::setMode(std::uint8_t gpio, interfaces::PinMode mode) {
  const bool clk_output_was = pins_.modeOf(kSwdClk.gpio) == interfaces::PinMode::kOutput;
  const bool reset_driven_was = pins_.modeOf(kNReset.gpio) == interfaces::PinMode::kOpenDrain;
  const bool ok = pins_.setMode(gpio, mode);
  if (!ok) return false;
  // nRESET released after a pulse (open-drain low, then input: RadioPins::ResetLine): the chip
  // resets (task p5-02).
  if (gpio == kNReset.gpio && reset_driven_was && mode == interfaces::PinMode::kInput && on_pin_reset) {
    on_pin_reset();
  }
  if (gpio == kSwdClk.gpio && clk_output_was && mode != interfaces::PinMode::kOutput) {
    // The clock stopped: B4.1.1 wants at least 8 idle cycles after the last transfer first.
    if (transfer_done_ && idle_since_transfer_ < core::swd::kIdleAfterTransfer) {
      fail("the clock stopped " + std::to_string(idle_since_transfer_) +
           " idle cycles after the last transfer; B4.1.1 asks for at least 8");
    }
    transfer_done_ = false;
  }
  if (gpio == kSwdClk.gpio || gpio == kSwdIo.gpio) observe();
  return true;
}

bool SwdTarget::write(std::uint8_t gpio, bool high) {
  const bool ok = pins_.write(gpio, high);
  if (!ok) return false;
  if (gpio == kSwdClk.gpio || gpio == kSwdIo.gpio) observe();
  return true;
}

bool SwdTarget::read(std::uint8_t gpio) { return pins_.read(gpio); }

bool SwdTarget::holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) {
  return pins_.holdThroughSleep(gpios, count, on);
}

void SwdTarget::attachAp(std::uint8_t apsel, AccessPort& ap) { aps_[apsel] = &ap; }

void SwdTarget::setStickyError() { sticky_ |= cs::kStickyErr; }

std::uint32_t SwdTarget::ctrlStat() const {
  return ctrl_stat_ack_ | ctrl_stat_req_ | sticky_ | (read_ok_ ? cs::kReadOk : 0) |
         (orun_detect_ ? cs::kOrunDetect : 0);
}

int SwdTarget::countErrors(const char* substring) const {
  int n = 0;
  for (const TargetError& e : errors_) {
    if (e.what.find(substring) != std::string::npos) n++;
  }
  return n;
}

void SwdTarget::fail(const std::string& what) {
  errors_.push_back(TargetError{counters_.clocks, what});
  if (strict) {
    FAIL_CHECK("SWD target: the host broke the protocol - " << what << " (at clock "
                                                           << counters_.clocks << ")");
  }
}

// What the line reads as when the host is not driving it.
void SwdTarget::driveIo(bool high) {
  target_driving_ = true;
  pins_.setExternalLevel(kSwdIo.gpio, high);
}

void SwdTarget::releaseIo() {
  target_driving_ = false;
  pins_.setExternalLevel(kSwdIo.gpio, present);  // the pull-up, or nothing
}

void SwdTarget::observe() {
  if (!present) {
    // No target, no pull-up: 2.2 nF at the far end keeps the level the host last drove.
    if (pins_.driving(kSwdIo.gpio)) held_level_ = pins_.levelOf(kSwdIo.gpio);
    pins_.setExternalLevel(kSwdIo.gpio, held_level_);
  }
  const bool clk = pins_.wireLevel(kSwdClk.gpio);
  if (clk && !clk_was_) onRisingEdge();
  clk_was_ = clk;
}

// ---- the edge ----------------------------------------------------------------------------------

void SwdTarget::onRisingEdge() {
  counters_.clocks++;
  if (!present) return;
  const bool host_driving = pins_.driving(kSwdIo.gpio);
  const bool bit = pins_.wireLevel(kSwdIo.gpio);

  switch (phase_) {
    case Phase::kHost:
      hostEdge(bit);
      return;

    case Phase::kAck:
      if (host_driving) {
        fail("SWDIO driven by the host while the target drives ACK[" + std::to_string(bit_index_) +
             "] (the turnaround after the park bit was missed, or the host never let go)");
      }
      driveIo(((ack_ >> bit_index_) & 1u) != 0);
      bit_index_++;
      if (bit_index_ == 3) {
        bit_index_ = 0;
        phase_ = (ack_ == ack::kOk && req_read_) ? Phase::kReadData : Phase::kRelease;
      }
      return;

    case Phase::kReadData:
      if (host_driving) {
        fail("SWDIO driven by the host while the target drives RDATA[" +
             std::to_string(bit_index_) + "]");
      }
      if (bit_index_ < 32) {
        driveIo(((rdata_ >> bit_index_) & 1u) != 0);
      } else {
        driveIo(rdata_parity_ != flip_parity_);
      }
      bit_index_++;
      if (bit_index_ == 33) {
        bit_index_ = 0;
        phase_ = Phase::kRelease;
      }
      return;

    case Phase::kRelease:
      // "When the target ... stops driving it, signal changes are performed on the rising edge"
      // (B4.3.1): this edge is still the target's.
      if (host_driving) {
        fail("SWDIO driven by the host on the edge where the target lets go of the line");
      }
      releaseIo();
      phase_ = Phase::kTurn;
      return;

    case Phase::kTurn:
      // The turnaround: "neither the host nor the target drives the wire" (B4.1.3).
      if (host_driving) {
        fail(std::string("missing turnaround: SWDIO driven by the host on the turnaround edge after ") +
             (ack_ == ack::kOk ? (req_read_ ? "the read data" : "the acknowledge") : "the acknowledge"));
      }
      if (ack_ == ack::kOk && !req_read_) {
        phase_ = Phase::kWriteData;
        bit_index_ = 0;
        wdata_ = 0;
      } else {
        counters_.transfers++;
        transfer_done_ = true;
        idle_since_transfer_ = 0;
        phase_ = Phase::kHost;
      }
      return;

    case Phase::kWriteData:
      if (!host_driving) {
        fail("SWDIO not driven by the host during WDATA[" + std::to_string(bit_index_) + "]");
      }
      if (bit_index_ < 32) {
        if (bit) wdata_ |= 1u << bit_index_;
        bit_index_++;
        return;
      }
      // The parity bit (B4.1.6): a mismatch sets WDATAERR and discards the write.
      if (parity32(wdata_) != bit) {
        sticky_ |= cs::kWdataErr;
        counters_.wdata_parity_errors++;
      } else if (req_ap_) {
        const std::uint8_t apsel = static_cast<std::uint8_t>(select_ >> 24);
        const std::uint8_t addr = static_cast<std::uint8_t>((select_ & 0xF0) | req_a_);
        // An AP transaction that ends in an error sets STICKYERR (B2.2.2): the write itself was
        // acknowledged OK before its data, so the FAULT reaches the host on the next request.
        if (aps_[apsel] != nullptr && aps_[apsel]->write(addr, wdata_) == ApAccess::kError) {
          sticky_ |= cs::kStickyErr;
          counters_.ap_errors++;
        }
        counters_.ap_writes++;
      } else {
        writeDpRegister(req_a_, wdata_);
      }
      counters_.transfers++;
      transfer_done_ = true;
      idle_since_transfer_ = 0;
      phase_ = Phase::kHost;
      return;
  }
}

// An edge while the host owns the line: the reset and switch detectors see every bit; the
// header parser sees it when the target is in step with the host.
void SwdTarget::hostEdge(bool bit) {
  trackLineReset(bit);
  trackSwitch(bit);
  if (mode_ == Mode::kJtag) return;
  if (!bit && header_bits_ == 0) {
    counters_.idle_cycles++;
    if (transfer_done_) {
      idle_since_transfer_++;
      if (idle_since_transfer_ > counters_.max_idle_after_transfer) {
        counters_.max_idle_after_transfer = idle_since_transfer_;
      }
    }
    return;
  }
  if (sync_ || never_reset_) headerBit(bit);
}

// B4.3.3: "50 clock cycles with the data signal held HIGH, followed by at least two idle
// cycles". A run that is long enough and the two lows after it: reset state. A run short of the
// minimum (but long enough to have been meant as one) and the two lows: the host's error.
void SwdTarget::trackLineReset(bool bit) {
  if (bit) {
    if (lows_after_highs_ > 0) {
      lows_after_highs_ = 0;
      run_before_lows_ = 0;
    }
    highs_++;
    return;
  }
  if (highs_ > 0) {
    run_before_lows_ = highs_;
    highs_ = 0;
    lows_after_highs_ = 1;
  } else if (lows_after_highs_ > 0) {
    lows_after_highs_++;
  }
  if (lows_after_highs_ == 2 && mode_ == Mode::kSwd) {
    if (run_before_lows_ >= min_line_reset_clocks) {
      counters_.line_resets++;
      enterResetState();
    } else if (run_before_lows_ + 8 >= min_line_reset_clocks) {
      // Within eight clocks of the minimum: meant as a line reset, and too short. (A shorter
      // run of highs is something else - a back-off's released line, a header's ones.)
      counters_.short_line_resets++;
      fail("a line reset of " + std::to_string(run_before_lows_) + " clocks, then idle: at least " +
           std::to_string(min_line_reset_clocks) + " are required (B4.3.3)");
    }
  }
}

// B5.2.2: at least 50 clocks HIGH, then 0xE79E LSB first. The window holds the last 16 bits in
// the order they came, the first in bit 0.
void SwdTarget::trackSwitch(bool bit) {
  window_ = (window_ >> 1) | (bit ? 0x8000u : 0u);
  if (!bit && highs_ == 0 && lows_after_highs_ == 1 && run_before_lows_ >= 50) {
    // The first low after a long run of highs: the sequence, if it is one, began with this bit.
    run_was_long_ = true;
    bits_since_run_ = 1;
    return;
  }
  const bool matched = (window_ & 0xFFFF) == core::swd::kJtagToSwd;
  if (matched) {
    // B5.2.2 step 3: "Send at least 50 SWCLKTCK cycles with SWDIOTMS HIGH" after the sequence.
    // The sequence's own last three ones are not part of those 50: the run is counted afresh,
    // so that a host sending 49 is seen to send 49 - whatever came before the sequence. (The
    // sixteen bits cannot occur in requests or idle cycles: no header has its stop bit set.)
    highs_ = 0;
  }
  if (!run_was_long_) return;
  bits_since_run_++;
  if (bits_since_run_ < 16) return;
  run_was_long_ = false;
  if (matched) {
    counters_.switches_to_swd++;
    if (mode_ == Mode::kJtag) {
      mode_ = Mode::kSwd;
      // "On selecting SWD operation, the SWD interface returns to its reset state" (B5.2.2).
      enterResetState();
    }
  }
}

// The chip was reset under the debugger (task p5-02): the debug power domain's requests are gone
// with it (PS "Debug Interface mode": the device is in that mode "as long as the debugger is
// requesting power"; a reset of the part ends it), the sticky flags and SELECT are at their
// reset values, and the port answers only a DPIDR read until the next line reset - what a host
// sees when it forgets to reconnect after a reset it caused.
void SwdTarget::resetDap() {
  ctrl_stat_req_ = 0;
  ctrl_stat_ack_ = 0;
  ack_countdown_ = 0;
  sticky_ = 0;
  read_ok_ = false;
  select_ = 0;
  ap_result_ = kUnknownData;
  last_sent_ = kUnknownData;
  enterResetState();
}

void SwdTarget::enterResetState() {
  never_reset_ = false;
  sync_ = true;
  in_reset_ = true;
  header_bits_ = 0;  // whatever header the trailing highs had begun is forgotten with them
  header_ = 0;
  // A line reset resets DLCR and nothing else (B4.3.3).
  dlcr_ = 0;
}

// The eight bits of a request, start first (B4.2, "Packet request"). The start bit is the 1
// that ended the idle; the bits after it fill the byte.
void SwdTarget::headerBit(bool bit) {
  if (header_bits_ == 0 && !bit) return;
  if (bit) header_ |= static_cast<std::uint8_t>(1u << header_bits_);
  header_bits_++;
  if (header_bits_ < 8) return;
  const std::uint8_t h = header_;
  header_ = 0;
  header_bits_ = 0;

  req_ap_ = ((h >> 1) & 1u) != 0;
  req_read_ = ((h >> 2) & 1u) != 0;
  req_a_ = static_cast<std::uint8_t>((((h >> 3) & 1u) << 2) | (((h >> 4) & 1u) << 3));
  const bool parity = ((h >> 5) & 1u) != 0;
  const bool stop = ((h >> 6) & 1u) != 0;
  const bool park = ((h >> 7) & 1u) != 0;
  const bool parity_wanted = (((h >> 1) & 1u) + ((h >> 2) & 1u) + ((h >> 3) & 1u) + ((h >> 4) & 1u)) % 2 != 0;
  if (parity != parity_wanted || stop || !park) {
    // B4.2.5: a protocol error; "the SW-DP does not reply to the packet request and does not
    // drive the line", and it leaves this state only on a line reset. A header of all ones is
    // the start of a line reset and is nobody's mistake; anything else, once in step, is.
    if (sync_ && h != 0xFF) {
      std::string why = parity != parity_wanted ? "wrong parity" : (stop ? "stop bit is 1" : "park bit is 0");
      fail("a request with the " + why + " (B4.2.5): header bits " + hex(h));
    }
    sync_ = false;
    in_reset_ = false;
    return;
  }
  counters_.requests++;
  if (never_reset_ && requires_line_reset) {
    fail("a request before any line reset (B4.3.3)");
    return;
  }
  if (silent) return;
  const bool dpidr_read = !req_ap_ && req_read_ && req_a_ == 0;
  if (in_reset_ && !dpidr_read) {
    fail("a request other than a DPIDR read in the reset state (B4.3.3): " +
         std::string(req_ap_ ? "AP" : "DP") + (req_read_ ? " read" : " write") + " at " +
         hex(req_a_));
    return;
  }
  respond();
}

// The three requests the DP must always answer OK (B4.2.3, B4.2.4): DPIDR read, CTRL/STAT read,
// ABORT write.
bool SwdTarget::exempt(bool ap, bool read, std::uint8_t a) const {
  if (ap) return false;
  if (read && a == 0) return true;
  if (read && a == 4 && (select_ & 0xF) == 0) return true;
  if (!read && a == 0) return true;
  return false;
}

// B4.2.8's tables: FAULT if a sticky flag is set, WAIT if the target is not ready, else OK with
// the register's value (a read) or the write accepted.
void SwdTarget::respond() {
  flip_parity_ = false;
  if (!exempt(req_ap_, req_read_, req_a_)) {
    if (req_ap_ && ap_needs_powerup &&
        ((ctrl_stat_req_ & cs::kCdbgPwrUpReq) == 0 || (ctrl_stat_ack_ & cs::kCdbgPwrUpAck) == 0)) {
      // B2.3.2: "If CDBGPWRUPREQ or CDBGPWRUPACK is LOW, any AP transfer might generate a
      // fault response." This target does, and says why.
      sticky_ |= cs::kStickyErr;
    }
    if (sticky_ != 0) {
      ack_ = ack::kFault;
      counters_.faults++;
      if (req_ap_ || req_a_ == 0xC) read_ok_ = false;
      phase_ = Phase::kAck;
      bit_index_ = 0;
      return;
    }
    if (wait_next > 0) {
      wait_next--;
      ack_ = ack::kWait;
      counters_.waits++;
      if (req_ap_ || req_a_ == 0xC) read_ok_ = false;
      phase_ = Phase::kAck;
      bit_index_ = 0;
      return;
    }
    if (req_ap_) {
      // The port's own reason to WAIT: the bus behind it is busy (B4.2.3, "the target is not
      // ready to complete the transaction"; the MEM-AP's stalling accesses, C2.2.3).
      const std::uint8_t apsel = static_cast<std::uint8_t>(select_ >> 24);
      const std::uint8_t addr = static_cast<std::uint8_t>((select_ & 0xF0) | req_a_);
      if (aps_[apsel] != nullptr && aps_[apsel]->stalls(addr, !req_read_)) {
        ack_ = ack::kWait;
        counters_.waits++;
        counters_.ap_stalls++;
        read_ok_ = false;
        phase_ = Phase::kAck;
        bit_index_ = 0;
        return;
      }
    }
  }
  ack_ = ack::kOk;
  counters_.ok++;
  if (req_read_) {
    if (req_ap_) {
      // Posted (B4.2.2): the data phase carries the previous AP read; this one is started.
      const std::uint8_t apsel = static_cast<std::uint8_t>(select_ >> 24);
      const std::uint8_t addr = static_cast<std::uint8_t>((select_ & 0xF0) | req_a_);
      rdata_ = ap_result_;
      ap_result_ = 0;
      if (aps_[apsel] != nullptr && aps_[apsel]->read(addr, &ap_result_) == ApAccess::kError) {
        // The error response sets STICKYERR; what RDBUFF then returns is UNKNOWN, and the
        // request for it gets FAULT (B4.2.4).
        sticky_ |= cs::kStickyErr;
        counters_.ap_errors++;
        ap_result_ = kUnknownData;
      }
      read_ok_ = true;
      last_sent_ = rdata_;
      counters_.ap_reads++;
    } else {
      rdata_ = readDpRegister(req_a_);
    }
    rdata_parity_ = parity32(rdata_);
    if (bad_read_parity_next > 0) {
      bad_read_parity_next--;
      flip_parity_ = true;
    }
  }
  phase_ = Phase::kAck;
  bit_index_ = 0;
}

std::uint32_t SwdTarget::readDpRegister(std::uint8_t a) {
  switch (a) {
    case 0x0:
      counters_.dpidr_reads++;
      in_reset_ = false;  // B4.3.3: "This transaction takes the connection out of reset state."
      return dpidr;
    case 0x4: {
      const std::uint32_t bank = select_ & 0xF;
      if (bank == 0) {
        counters_.ctrl_stat_reads++;
        // The power controller answers after however many reads it was told to (B2.3.2).
        if (powers_up) {
          if (ack_countdown_ > 0) {
            ack_countdown_--;
          } else {
            ctrl_stat_ack_ = 0;
            if (ctrl_stat_req_ & cs::kCdbgPwrUpReq) ctrl_stat_ack_ |= cs::kCdbgPwrUpAck;
            if (ctrl_stat_req_ & cs::kCsysPwrUpReq) ctrl_stat_ack_ |= cs::kCsysPwrUpAck;
          }
        }
        return ctrlStat();
      }
      if (bank == 1) return dlcr_;
      fail("a read of DP address 0x4 with DPBANKSEL " + std::to_string(bank) + ", which this simulator does not model");
      return 0;
    }
    case 0x8:
      counters_.resend_reads++;
      return last_sent_;
    case 0xC: {
      counters_.rdbuff_reads++;
      const std::uint32_t value = ap_result_;
      // "After reading the DP Read Buffer, its contents are no longer valid" (B2.2.7).
      ap_result_ = kUnknownData;
      read_ok_ = true;
      last_sent_ = value;
      return value;
    }
  }
  return 0;
}

void SwdTarget::writeDpRegister(std::uint8_t a, std::uint32_t value) {
  switch (a) {
    case 0x0:
      counters_.abort_writes++;
      if (value & ab::kOrunErrClr) sticky_ &= ~cs::kStickyOrun;
      if (value & ab::kWdErrClr) sticky_ &= ~cs::kWdataErr;
      if (value & ab::kStkErrClr) sticky_ &= ~cs::kStickyErr;
      if (value & ab::kStkCmpClr) sticky_ &= ~cs::kStickyCmp;
      if (value & ab::kDapAbort) {
        counters_.dap_aborts++;
        wait_next = 0;  // the stalled transaction is dropped (B2.2.1)
      }
      return;
    case 0x4: {
      const std::uint32_t bank = select_ & 0xF;
      if (bank == 0) {
        counters_.ctrl_stat_writes++;
        const std::uint32_t req = value & (cs::kCdbgPwrUpReq | cs::kCsysPwrUpReq);
        if ((req & cs::kCsysPwrUpReq) && !(req & cs::kCdbgPwrUpReq)) {
          fail("CSYSPWRUPREQ set without CDBGPWRUPREQ (B2.3.2: UNPREDICTABLE)");
        }
        if (req != ctrl_stat_req_) ack_countdown_ = powerup_ack_after_reads;
        ctrl_stat_req_ = req;
        if (req == 0) ctrl_stat_ack_ = 0;  // B2.3.2: the controller deasserts the acknowledge
        orun_detect_ = (value & cs::kOrunDetect) != 0;
        if (orun_detect_) fail("CTRL/STAT.ORUNDETECT set: overrun detection is not modelled here, and the host leaves it off");
        return;
      }
      if (bank == 1) {
        dlcr_ = value;
        return;
      }
      fail("a write of DP address 0x4 with DPBANKSEL " + std::to_string(bank) + ", which this simulator does not model");
      return;
    }
    case 0x8:
      counters_.select_writes++;
      select_ = value;
      return;
    case 0xC:
      // DPv1: reserved, the write is ignored (Table B4-3).
      return;
  }
}

}  // namespace sim
