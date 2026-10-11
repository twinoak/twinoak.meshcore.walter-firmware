// The simulated SWD target (task p1-03): a debug port at the level of the two wires. It stands
// between RadioPins and the fake pins as an interfaces::Pins of its own, forwards every call to
// the fake (so the pin rules of docs/ARCHITECTURE.md section 2 are checked underneath as before)
// and watches SWCLK and SWDIO on the way through: it sees our rising edges, samples SWDIO on
// them as a target does, drives SWDIO on them when the line is its, and fails the running test
// with a sentence when the host breaks the protocol. Every rule it enforces is Arm's: ADIv5.0 to
// ADIv5.2, IHI 0031G, chapters B2, B4 and B5 - cited where used. Where the document leaves a
// target's behaviour IMPLEMENTATION DEFINED or UNPREDICTABLE, this one chooses silence and says
// so in an error, which is the strict reading a host should be written against.
//
// What it checks on the host (each fails the test while `strict`, and is recorded either way):
//   - a request whose parity, stop or park bit is wrong (B4.2.5), once the target is in step
//     with the host (after a line reset: before that, a run of ones looks like a bad header to
//     a target and to this one, and is not held against anyone);
//   - a request before any line reset (B4.3.3: "a line reset is required when first
//     connecting"), and a request other than a DPIDR read while in reset state (B4.3.3: "the
//     behavior of the target is UNPREDICTABLE");
//   - a line reset of fewer clocks than it asks for (B4.3.3: at least 50), when that run is
//     followed by the two idle cycles that would have made it one;
//   - SWDIO driven by the host on an edge where the target drives it (contention);
//   - SWDIO driven by the host on the turnaround edge after the target let go (a missing or
//     short turnaround, B4.1.3);
//   - the clock stopped (SWCLK released) fewer than 8 idle cycles after a completed transfer
//     (B4.1.1);
//   - CTRL/STAT.ORUNDETECT set, or a DPBANKSEL this simulator does not model.
//
// The debug port it implements: DPIDR, ABORT, CTRL/STAT (the power-up handshake, the sticky
// flags, READOK), DLCR (read as zero), SELECT, RESEND, RDBUFF, and the posted AP read. Access
// ports are plugged in behind a register-level interface; task 04 brings the MEM-AP and the
// CTRL-AP, and StubAp here stores what is written.
//
// It can be told to: answer WAIT the next N times; carry a sticky error (FAULT until ABORT);
// return bad read parity once; go silent; not be there at all; acknowledge a power-up only after
// N reads, or never; start in JTAG mode so that only the switch sequence reaches it; and how
// long a line reset has to be.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fakes/fake_pins.h"
#include "interfaces/pins.h"

namespace sim {

// An access port's registers, as the DP reaches them: `addr` is the byte offset 0x00..0xFC,
// SELECT.APBANKSEL in bits [7:4] and A[3:2] in bits [3:2] (ADIv5 B4.2.8). Task 04's simulated
// nRF52840 (nrf52_chip.h) implements the AHB-AP and the CTRL-AP against this.
//
// What an access completes with: kError is a transaction that "completes with an error
// response" - the DP sets CTRL/STAT.STICKYERR and answers FAULT to the next request (B2.2.2,
// B4.2.4); the data phase of the read that caused it still happens, with UNKNOWN data. kWait is
// not returned here: a port that cannot take the access now says so through stalls(), which is
// asked at the request, before the acknowledge, as the hardware decides (B4.2.3).
enum class ApAccess : std::uint8_t { kOk, kError };

class AccessPort {
 public:
  virtual ~AccessPort() = default;
  virtual ApAccess read(std::uint8_t addr, std::uint32_t* out) = 0;
  virtual ApAccess write(std::uint8_t addr, std::uint32_t value) = 0;
  // True: the DP answers WAIT to this request (the bus behind the port is busy).
  virtual bool stalls(std::uint8_t addr, bool write) {
    (void)addr;
    (void)write;
    return false;
  }
};

// Stores what is written, returns it on read. Enough for the DP's own tests.
class StubAp final : public AccessPort {
 public:
  std::uint32_t regs[64] = {};
  unsigned reads = 0;
  unsigned writes = 0;
  ApAccess read(std::uint8_t addr, std::uint32_t* out) override {
    reads++;
    *out = regs[(addr >> 2) & 63];
    return ApAccess::kOk;
  }
  ApAccess write(std::uint8_t addr, std::uint32_t value) override {
    writes++;
    regs[(addr >> 2) & 63] = value;
    return ApAccess::kOk;
  }
};

struct TargetError {
  std::uint64_t at_clock;  // rising edges seen so far
  std::string what;
};

constexpr std::uint32_t kNrf52840Dpidr = 0x2BA01477;  // core/swd/dp.h says where it comes from
constexpr std::uint32_t kUnknownData = 0xA5A5A5A5;    // what an UNKNOWN data phase returns here

class SwdTarget final : public interfaces::Pins {
 public:
  explicit SwdTarget(fakes::FakePins& pins);

  // interfaces::Pins: forwarded to the fake pins, then watched.
  bool setMode(std::uint8_t gpio, interfaces::PinMode mode) override;
  bool write(std::uint8_t gpio, bool high) override;
  bool read(std::uint8_t gpio) override;
  bool holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) override;

  fakes::FakePins& pins() { return pins_; }

  // ---- what it can be told ----------------------------------------------------------------
  bool strict = true;                 // a host protocol error fails the running test at once
  bool present = true;                // false: no target and no pull-up; the line keeps what was last driven
  bool silent = false;                // powered and there, answering nothing
  bool requires_line_reset = true;    // a request before any line reset is a host error (B4.3.3)
  unsigned min_line_reset_clocks = 50;  // B4.3.3
  unsigned wait_next = 0;             // WAIT to the next N requests that may WAIT (B4.2.3)
  unsigned bad_read_parity_next = 0;  // the next N read data phases carry the wrong parity bit
  bool powers_up = true;              // false: the power-up acknowledges never come
  unsigned powerup_ack_after_reads = 0;  // CTRL/STAT reads before the acknowledges appear
  bool ap_needs_powerup = true;       // an AP access without CDBGPWRUPREQ and ACK FAULTs (B2.3.2)
  std::uint32_t dpidr = kNrf52840Dpidr;

  void attachAp(std::uint8_t apsel, AccessPort& ap);
  void setStickyError();  // CTRL/STAT.STICKYERR: FAULT to everything but the three exempt requests
  void startInJtag() { mode_ = Mode::kJtag; }  // an SWJ-DP in JTAG mode until the switch (B5.2.2)

  // ---- what the tests read ----------------------------------------------------------------
  std::uint32_t ctrlStat() const;
  std::uint32_t select() const { return select_; }
  bool inResetState() const { return in_reset_; }
  bool swdSelected() const { return mode_ == Mode::kSwd; }
  bool everReset() const { return !never_reset_; }

  struct Counters {
    std::uint64_t clocks = 0;           // rising edges of SWCLK
    unsigned line_resets = 0;           // valid ones (B4.3.3)
    unsigned short_line_resets = 0;     // runs of at least 8 and under the minimum, then idle
    unsigned switches_to_swd = 0;       // 0xE79E after 50 highs (B5.2.2)
    unsigned requests = 0;              // valid headers
    unsigned transfers = 0;             // answered (OK, WAIT or FAULT)
    unsigned ok = 0;
    unsigned waits = 0;
    unsigned faults = 0;
    unsigned dpidr_reads = 0;
    unsigned ctrl_stat_reads = 0;
    unsigned ctrl_stat_writes = 0;
    unsigned select_writes = 0;
    unsigned abort_writes = 0;
    unsigned dap_aborts = 0;
    unsigned rdbuff_reads = 0;
    unsigned resend_reads = 0;
    unsigned ap_reads = 0;
    unsigned ap_writes = 0;
    unsigned ap_errors = 0;  // AP transactions that ended in an error response (STICKYERR set)
    unsigned ap_stalls = 0;  // WAITs the access port asked for
    unsigned wdata_parity_errors = 0;
    unsigned idle_cycles = 0;           // host idle cycles seen in all
    unsigned max_idle_after_transfer = 0;
  };
  const Counters& counters() const { return counters_; }
  const std::vector<TargetError>& errors() const { return errors_; }
  int countErrors(const char* substring) const;
  void clearErrors() { errors_.clear(); }

 private:
  enum class Mode : std::uint8_t { kJtag, kSwd };
  // Where the target is in a transfer, in rising edges (see core/swd/wire.cpp, Wire::transfer).
  enum class Phase : std::uint8_t {
    kHost,       // the host's: idle cycles, a header, a line reset
    kAck,        // the target drives ACK[0..2]
    kReadData,   // the target drives RDATA[0..31] and the parity
    kRelease,    // the edge on which the target stops driving
    kTurn,       // the turnaround edge: nobody drives
    kWriteData,  // the host drives WDATA[0..31] and the parity; the target samples
  };

  void observe();
  void onRisingEdge();
  void hostEdge(bool bit);
  void trackLineReset(bool bit);
  void trackSwitch(bool bit);
  void headerBit(bool bit);
  void respond();
  bool exempt(bool ap, bool read, std::uint8_t a) const;
  std::uint32_t readDpRegister(std::uint8_t a);
  void writeDpRegister(std::uint8_t a, std::uint32_t value);
  void driveIo(bool high);
  void releaseIo();
  void fail(const std::string& what);
  void enterResetState();

  fakes::FakePins& pins_;
  bool clk_was_ = false;
  bool held_level_ = true;  // the absent line's memory of what the host last drove

  Mode mode_ = Mode::kSwd;
  Phase phase_ = Phase::kHost;
  bool sync_ = false;        // in step with the host: a line reset has been seen and no protocol error since
  bool never_reset_ = true;  // no line reset yet
  bool in_reset_ = false;    // reset state (B4.3.3)
  bool target_driving_ = false;

  // the line reset and switch detectors
  unsigned highs_ = 0;
  unsigned lows_after_highs_ = 0;
  unsigned run_before_lows_ = 0;
  std::uint32_t window_ = 0;
  unsigned bits_since_run_ = 0;
  bool run_was_long_ = false;

  // the header parser
  unsigned header_bits_ = 0;
  std::uint8_t header_ = 0;

  // the transfer in progress
  bool req_ap_ = false;
  bool req_read_ = false;
  std::uint8_t req_a_ = 0;
  std::uint8_t ack_ = 0;
  std::uint32_t rdata_ = 0;
  bool rdata_parity_ = false;
  bool flip_parity_ = false;
  unsigned bit_index_ = 0;
  std::uint32_t wdata_ = 0;
  bool transfer_done_ = false;
  unsigned idle_since_transfer_ = 0;

  // the DP
  std::uint32_t ctrl_stat_req_ = 0;  // CSYSPWRUPREQ, CDBGPWRUPREQ as written
  std::uint32_t ctrl_stat_ack_ = 0;
  std::uint32_t sticky_ = 0;
  bool read_ok_ = false;
  bool orun_detect_ = false;
  std::uint32_t dlcr_ = 0;
  std::uint32_t select_ = 0;
  std::uint32_t ap_result_ = kUnknownData;  // the posted AP read, for the next AP read or RDBUFF
  std::uint32_t last_sent_ = kUnknownData;  // for RESEND
  unsigned ack_countdown_ = 0;
  AccessPort* aps_[256] = {};

  Counters counters_;
  std::vector<TargetError> errors_;
};

}  // namespace sim
