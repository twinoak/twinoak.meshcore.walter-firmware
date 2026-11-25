// The debug port (task p1-03): the SW-DP's registers as calls over the wire, with every answer a
// result that says what happened. Arm, ADIv5.0 to ADIv5.2, IHI 0031G: the registers are chapter
// B2.2, the protocol's behaviour chapter B4. No ESP-IDF include, no heap, no loop without a
// constant bound: WAIT is retried kWaitRetries times back to back and then waited out for
// kWaitBoundMs (kWaitPauses tries of the line idle), a power-up is polled kPowerUpPolls times,
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

#include "core/swd/idle_pause.h"
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
constexpr unsigned kWaitRetries = 10;      // back to back (B4.2.3: "retries the same operation")
constexpr unsigned kPowerUpPolls = 100;    // CTRL/STAT reads a millisecond apart: 100 ms
constexpr unsigned kConnectAttempts = 2;   // the reset sequence, retried once (B4.3.3)

// -- **a WAIT is waited out** (task p19-01) -----------------------------------------------------
//
// A WAIT says the target "cannot currently process the request" (B4.2.3), and on an nRF52840 the
// thing that most often cannot is the flash: the flash "is not accessible" while the NVMC writes
// or erases (Product Specification, NVMC), and a MEM-AP whose bus access stalls answers WAIT
// until it clears (ADIv5 C2.2.3). The radio's own application writes its own flash - its clock's
// file at the first clock set after a boot - and a scan that reads the application's pages
// *while it runs* meets that write. kWaitRetries tries back to back are a few milliseconds; a
// page erase is tens, and from Walter 0.1.11 the first flash of every NoiseScope session on the
// unit at Bellevue failed in the scan with `swd wait-timeout`, nothing written (eight sessions
// of eight; `tasks/phase-19/README.md`).
//
// So the back-to-back tries stay - they answer the WAIT that clears in a transfer or two, and
// they are what a flash that goes through still costs - and after them the port **waits the
// operation out**: the same request again with the line idle between tries (idle_pause.h: the
// idle task of this core runs, so the task watchdog is fed), for as long as the longest flash
// operation an application can start may take.
//
// **The figure is the Product Specification's, not a guess.** nRF52840 Product Specification
// v1.11 (4413_417 v1.11, 2024-10-01), section 4.3.10.1 "Flash programming": tERASEALL "Time to
// erase all flash" **169 ms** and tERASEPAGE "Time to erase one page" **85 ms**, tWRITE "Time to
// write one 32-bit word" 41 us - all three in the table's **Max.** column, all three footnoted
// "Applies when HFXO is used. Timing varies according to HFINT accuracy when HFINT is used". The
// internal oscillator's tolerance is section 5.4.4.1's fTOL_HFINT, "Frequency tolerance", max
// **+/-8 %**, which makes 169 ms at worst 184 ms. ERASEALL is the longest of the three an
// application can start (NVMC.ERASEALL), so 184 ms is the figure.
//
// kWaitBoundMs is **500 ms**: that with a margin of 2.7, which is room for one flash operation
// that began just before the access and the next one the application has queued behind it.
constexpr std::uint32_t kWaitBoundMs = 500;

// What **all** the WAITs of one connection may cost together, so that a chip which answers WAIT
// for ever cannot turn a flash into an afternoon: every transfer that reaches the wait-out at
// all is one that would have ended the flash before this task, and the per-transfer bound alone
// says nothing about how many of them there may be. Spent and reset by connect(), which is once
// a flash (core/flasher/flasher.cpp, openChip()).
//
// Thirty seconds, and the arithmetic is the flash windows of core/update/radio_update.h: a flash
// that wrote nothing is given kFlashWroteNothingMs, three minutes, of which the scan of the
// 424 kB image of 23 November is a little over a minute and a half - 30 s more is still inside
// it; and a whole flash is given kFlashWindowMs, sixteen minutes, of which the unit's own
// journal says 14 min 51 s - 30 s more is 15 min 21 s, still inside it. So no window moves.
// Half a minute is also ~350 page erases of the radio's own, where what the scan is expected to
// meet is a handful.
constexpr std::uint32_t kWaitBudgetMs = 30000;

// How many times a WAIT is asked again while it is waited out: the bound in time divided by the
// pause between two tries, so the loop is bounded by a constant as well as by the clock (the
// clock is what decides, because `delayMs` may overshoot and a pause is 10 to 11 ms on the
// board). 50.
constexpr unsigned kWaitPauses = kWaitBoundMs / kIdlePauseMs;
static_assert(kWaitPauses > 0, "a WAIT must get at least one try with the line idle");

enum class Status : std::uint8_t {
  kOk,
  kLinesLost,       // a pin operation was refused: the lines are not held, or a kill took them
  kNoTarget,        // the acknowledge was all ones: nothing answered (an unpowered radio, or none)
  kProtocolError,   // an acknowledge that is none of the three: the line is confused
  kWaitTimeout,     // WAIT kWaitRetries times, and then for kWaitBoundMs (or the budget was spent)
  kFault,           // FAULT; ctrl_stat holds the sticky flags, which ABORT then cleared
  kParityError,     // the read data's parity is wrong; the value is not trusted
  kPowerUpTimeout,  // the acknowledges did not come in kPowerUpPolls reads
  kBadAddress,      // not a DP register address, or an AP address that is not word-aligned
  kNoMemAp,         // the access port is not a memory access port, or is not there (task p1-04)
};
const char* describe(Status status);
// One word, for a journal entry's line and a job's note (task p18-05): `no-target`.
const char* name(Status status);

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

  // **What this connection waited out** (task p19-01, deliverable 3). The counters above are the
  // port's whole life; what a flash has to say about itself is one debug session's, so this
  // record is cleared by connect() - which a flash makes exactly once. `longest_ms` is measured
  // on the clock, not counted in pauses, so it is what the port really waited.
  struct Waits {
    std::uint32_t transfers = 0;      // transfers that still answered WAIT after the back-to-back tries
    std::uint32_t longest_ms = 0;     // the longest of them was waited out this long
    std::uint32_t total_ms = 0;       // all of them together: what kWaitBudgetMs bounds
    std::uint32_t longest_at = 0;     // the memory address the longest one was at
    bool longest_at_known = false;    // false when no access port had said one yet
    bool gave_up = false;             // one outlasted kWaitBoundMs, or the budget was spent
  };
  const Waits& waits() const { return waits_; }

  // The memory address the access port is about to use, so that the record above can say **where**
  // the longest WAIT was. Set by core/swd/mem_ap.cpp before each DRW access and by nothing else;
  // the debug port itself only knows registers.
  void noteAddress(std::uint32_t address) {
    address_ = address;
    address_known_ = true;
  }

  // Test-only: how many clocks the next connect()'s line resets are, so that a test can send one
  // clock too few and watch the simulator refuse it. Never called by the firmware.
  void setLineResetClocksForTest(unsigned clocks) { line_reset_clocks_ = clocks; }

 private:
  // One transfer with the WAIT retries, the FAULT handling and the parity check. The three
  // requests that never WAIT or FAULT (B4.2.3, B4.2.4) go through here all the same.
  Result transfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata);
  Result rawTransfer(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata);
  // The same request again with the line idle between tries, until the target answers or
  // kWaitBoundMs has passed (task p19-01). Called only when kWaitRetries back-to-back tries have
  // all answered WAIT.
  Result waitItOut(bool ap, bool read, std::uint8_t addr, std::uint32_t wdata);
  Result ensureSelect(std::uint8_t ap, std::uint8_t ap_bank, std::uint8_t dp_bank);
  Result pollPowerUp(std::uint32_t wanted_acks);

  Wire& wire_;
  bool connected_ = false;
  std::uint32_t dpidr_ = 0;
  bool select_known_ = false;
  std::uint32_t select_ = 0;
  unsigned line_reset_clocks_ = kLineResetClocks;
  std::uint32_t address_ = 0;    // what noteAddress() last said
  bool address_known_ = false;
  Counters counters_;
  Waits waits_;
};

}  // namespace swd
}  // namespace core
