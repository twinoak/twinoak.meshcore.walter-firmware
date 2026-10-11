// The debug port (task p1-03): the SW-DP's registers as calls over the wire, with every answer a
// result that says what happened. Arm, ADIv5.0 to ADIv5.2, IHI 0031G: the registers are chapter
// B2.2, the protocol's behaviour chapter B4. No ESP-IDF include, no heap, no loop without a
// constant bound: WAIT is retried kWaitRetries times, a power-up is polled kPowerUpPolls times,
// a connection is tried kConnectAttempts times, and then it is an error.
//
// What the layer above (task 04: the memory access port, the nRF52) gets from it:
//   connect()      line reset, the JTAG-to-SWD switch, line reset, idle, DPIDR - then SELECT
//                  written to a known value;
//   readDp/writeDp a DP register by its address (0x0, 0x4, 0x8, 0xC);
//   readAp/writeAp an AP register by access port and address, SELECT rewritten only when the
//                  port or the bank changes, and an AP read's posted value collected through
//                  RDBUFF so the caller sees the register it asked for;
//   powerUp()      CTRL/STAT's two power-up requests and a bounded wait for both acknowledges;
//   powerDown()    the requests withdrawn and the acknowledges waited out;
//   clearErrors()  ABORT's four clear bits.
// A FAULT is reported with CTRL/STAT as it was read before ABORT cleared it; a read whose parity
// is wrong fails that read; no acknowledge at all is "no target", which the supervisor needs to
// tell from everything else.
#pragma once

#include <cstdint>

#include "core/swd/wire.h"

namespace core {
namespace swd {

// The DP register map (B2.2, "Accessing <register>", DPv1): the address is the byte offset whose
// bits [3:2] go on the wire as A[2:3].
namespace dp {
constexpr std::uint8_t kDpidr = 0x0;     // read-only (B2.2.5)
constexpr std::uint8_t kAbort = 0x0;     // write-only (B2.2.1)
constexpr std::uint8_t kCtrlStat = 0x4;  // SELECT.DPBANKSEL 0 (B2.2.2); bank 1 is DLCR (B2.2.3)
constexpr std::uint8_t kSelect = 0x8;    // write-only in DPv1 (B2.2.9)
constexpr std::uint8_t kResend = 0x8;    // read-only (B2.2.8)
constexpr std::uint8_t kRdbuff = 0xC;    // read-only (B2.2.7)
}  // namespace dp

// CTRL/STAT (B2.2.2, "Field descriptions").
namespace ctrl_stat {
constexpr std::uint32_t kCsysPwrUpAck = 1u << 31;
constexpr std::uint32_t kCsysPwrUpReq = 1u << 30;
constexpr std::uint32_t kCdbgPwrUpAck = 1u << 29;
constexpr std::uint32_t kCdbgPwrUpReq = 1u << 28;
constexpr std::uint32_t kCdbgRstAck = 1u << 27;
constexpr std::uint32_t kCdbgRstReq = 1u << 26;
constexpr std::uint32_t kWdataErr = 1u << 7;
constexpr std::uint32_t kReadOk = 1u << 6;
constexpr std::uint32_t kStickyErr = 1u << 5;
constexpr std::uint32_t kStickyCmp = 1u << 4;
constexpr std::uint32_t kStickyOrun = 1u << 1;
constexpr std::uint32_t kOrunDetect = 1u << 0;
// The flags that make the DP answer FAULT until they are cleared (B4.2.4: "the SW-DP issues a
// FAULT response if any sticky flag is set"; B2.2.2 names WDATAERR, STICKYERR, STICKYCMP,
// STICKYORUN as the sticky ones).
constexpr std::uint32_t kStickyMask = kWdataErr | kStickyErr | kStickyCmp | kStickyOrun;
constexpr std::uint32_t kPowerUpRequests = kCdbgPwrUpReq | kCsysPwrUpReq;
constexpr std::uint32_t kPowerUpAcks = kCdbgPwrUpAck | kCsysPwrUpAck;
}  // namespace ctrl_stat

// ABORT (B2.2.1, "Field descriptions", DPv1 or higher).
namespace abort {
constexpr std::uint32_t kOrunErrClr = 1u << 4;
constexpr std::uint32_t kWdErrClr = 1u << 3;
constexpr std::uint32_t kStkErrClr = 1u << 2;
constexpr std::uint32_t kStkCmpClr = 1u << 1;
constexpr std::uint32_t kDapAbort = 1u << 0;  // "only in extreme cases" (B2.2.1); nothing here writes it
constexpr std::uint32_t kClearAll = kOrunErrClr | kWdErrClr | kStkErrClr | kStkCmpClr;
}  // namespace abort

// SELECT (B2.2.9, "Field descriptions"): APSEL[31:24], APBANKSEL[7:4], DPBANKSEL[3:0].
namespace select {
constexpr std::uint32_t value(std::uint8_t ap, std::uint8_t ap_bank, std::uint8_t dp_bank) {
  return (static_cast<std::uint32_t>(ap) << 24) | (static_cast<std::uint32_t>(ap_bank & 0xF) << 4) |
         (dp_bank & 0xF);
}
}  // namespace select

// DPIDR (B2.2.5): REVISION[31:28], PARTNO[27:20], MIN[16], VERSION[15:12], DESIGNER[11:1]
// (JEP106: continuation code [11:8], identification code [7:1]; Arm is 0x4, 0x3B), bit 0 RAO.
struct DpidrFields {
  std::uint8_t revision;
  std::uint8_t partno;
  bool min;
  std::uint8_t version;
  std::uint16_t designer;
};
DpidrFields decodeDpidr(std::uint32_t dpidr);
constexpr std::uint16_t kDesignerArm = (0x4u << 7) | 0x3Bu;  // 0x23B

// What the nRF52840 answers. The Product Specification says only that the DAP "implements a
// standard ARM CoreSight serial wire debug port (SW-DP)" ("Debug and trace > DAP"); the value
// is the one every debugger expects from an nRF52, and it decodes by B2.2.5 as revision 2, part
// 0xBA, DPv1, designer Arm - a Cortex-M4 SW-DP. The bench confirms it (docs/bench.md section 4);
// task 04's identification reads the chip's own FICR and does not rest on this number.
constexpr std::uint32_t kNrf52840Dpidr = 0x2BA01477;

// The bounds.
constexpr unsigned kWaitRetries = 10;      // then kWaitTimeout (B4.2.3: "retries the same operation")
constexpr unsigned kPowerUpPolls = 100;    // CTRL/STAT reads a millisecond apart: 100 ms
constexpr unsigned kConnectAttempts = 2;   // the reset sequence, retried once (B4.3.3)

enum class Status : std::uint8_t {
  kOk,
  kLinesLost,       // a pin operation was refused: the lines are not held, or a kill took them
  kNoTarget,        // the acknowledge was all ones: nothing answered (an unpowered radio, or none)
  kProtocolError,   // an acknowledge that is none of the three: the line is confused
  kWaitTimeout,     // WAIT kWaitRetries times
  kFault,           // FAULT; ctrl_stat holds the sticky flags, which ABORT then cleared
  kParityError,     // the read data's parity is wrong; the value is not trusted
  kPowerUpTimeout,  // the acknowledges did not come in kPowerUpPolls reads
  kBadAddress,      // not a DP register address, or an AP address that is not word-aligned
};
const char* describe(Status status);

struct Result {
  Status status = Status::kOk;
  std::uint32_t value = 0;      // a read's data; connect()'s DPIDR; powerUp()'s CTRL/STAT
  std::uint32_t ctrl_stat = 0;  // on kFault: as read before it was cleared; on power-up: the last read
  std::uint8_t ack = 0;         // the last acknowledge seen
  bool ok() const { return status == Status::kOk; }
};

class Dp {
 public:
  explicit Dp(Wire& wire) : wire_(wire) {}

  Result connect();  // value = DPIDR
  Result readDp(std::uint8_t addr);
  Result writeDp(std::uint8_t addr, std::uint32_t value);
  Result readAp(std::uint8_t ap, std::uint8_t addr);
  Result writeAp(std::uint8_t ap, std::uint8_t addr, std::uint32_t value);
  Result powerUp();
  Result powerDown();
  Result clearErrors();

  bool connected() const { return connected_; }
  std::uint32_t dpidr() const { return dpidr_; }
  Wire& wire() { return wire_; }

  struct Counters {
    std::uint32_t transfers = 0;
    std::uint32_t wait_retries = 0;
    std::uint32_t wait_timeouts = 0;
    std::uint32_t faults = 0;
    std::uint32_t parity_errors = 0;
    std::uint32_t protocol_errors = 0;
    std::uint32_t no_target = 0;
    std::uint32_t select_writes = 0;
    std::uint32_t connects = 0;
  };
  const Counters& counters() const { return counters_; }

  // Test-only: how many clocks the next connect()'s line resets are, so that a test can send one
  // clock too few and watch the simulator refuse it. Never called by the firmware.
  void setLineResetClocksForTest(unsigned clocks) { line_reset_clocks_ = clocks; }

 private:
  // One transfer with the WAIT retries, the FAULT handling and the parity check. The three
  // requests that never WAIT or FAULT (B4.2.3, B4.2.4) go through here all the same.
  Result transfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata);
  Result rawTransfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata);
  Result ensureSelect(std::uint8_t ap, std::uint8_t ap_bank, std::uint8_t dp_bank);
  Result pollPowerUp(std::uint32_t wanted_acks);

  Wire& wire_;
  bool connected_ = false;
  std::uint32_t dpidr_ = 0;
  bool select_known_ = false;
  std::uint32_t select_ = 0;
  unsigned line_reset_clocks_ = kLineResetClocks;
  Counters counters_;
};

}  // namespace swd
}  // namespace core
