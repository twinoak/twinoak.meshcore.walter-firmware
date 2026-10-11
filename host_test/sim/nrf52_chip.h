// The simulated nRF52840 (task p1-04), behind task p1-03's simulated debug port: two access ports
// and the memory map they reach. It is only as true as the documents it was written from -
// Nordic's nRF52840 Product Specification (PS: FICR, UICR, NVMC, Memory, Debug and trace), Arm's
// Armv7-M Architecture Reference Manual (ARM: DHCSR, DEMCR, AIRCR), Arm's ADIv5 (the MEM-AP) and
// the Cortex-M4 Technical Reference Manual (the AHB-AP) - each cited where used; the register
// numbers are the firmware's own, from core/nrf52/regs.h, so that a wrong number there is wrong
// in both and a test against a transcription cannot pass by accident of agreement... except that
// the firmware reads them and this chip answers them, and the documents were read for both.
//
//   AP 0, the AHB-AP: CSW, TAR, DRW, IDR (0x24770011, DDI 0439B Table 8-5) with word accesses
//     and single auto-increment that wraps as the strict reading of ADIv5 C2.2.2 has it - inside
//     1 KB (tar_wrap_bytes; the Cortex-M4's own wraps at 4 KB, DDI 0439B Table 8-6, which the
//     knob can be set to). Forgetting to rewrite TAR at the boundary lands the access at the
//     start of the block, and a test notices. A DHCSR write that sets C_DEBUGEN while
//     CSW.MasterType is 0 is ignored and reported (DDI 0439B Table 8-6, note a).
//   AP 1, the CTRL-AP (PS "Debug and trace > CTRL-AP"): RESET, ERASEALL, ERASEALLSTATUS,
//     APPROTECTSTATUS, IDR (0x02880000).
//   Memory: 1 MB of flash in 4 KB pages that only clears bits on write (PS NVMC "Writing to
//     flash") and only with CONFIG right; 256 KB of RAM; FICR with the identification registers;
//     UICR, read-only to this firmware; the NVMC with a READY that takes simulated time (tWRITE
//     41 us, tERASEPAGE 85 ms typical, PS NVMC electrical specification) and stalls the bus while
//     busy; the core's DHCSR, DEMCR and AIRCR with a core that is running or halted, vector catch
//     at reset, and the reset's effect on each register.
//
// It records every flash erase and write (and every CONFIG write and READY read) in `log`, and
// fails the running test with a sentence when the host does what the documents say not to - or
// what S-5 says never to: a write to NVMC.ERASEALL, NVMC.ERASEUICR, the CTRL-AP's ERASEALL or
// any UICR address is the tripwire, and it fires at once (FAIL_CHECK while `strict`; recorded in
// `errors` either way, for the test that shows it firing on purpose).
//
// Knobs: protection on; protection re-engaged by a reset (a build code Fxx module whose firmware
// opens the port at run time); another part, page size or page count; READY late or never;
// power lost at the Nth flash operation, leaving a page half-erased or a word half-written;
// the auto-increment wrap; a region outside which any erase or write fails the test.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fakes/fake_clock.h"
#include "sim/swd_target.h"

namespace sim {

struct FlashEvent {
  enum class Kind : std::uint8_t {
    kConfig,         // NVMC.CONFIG written: value = WEN
    kErasePage,      // a page erased: addr
    kWrite,          // a word written: addr, value (what the host wrote), old_value, result = old & value
    kRejectedWrite,  // a flash write with CONFIG != Wen: nothing happened
    kRejectedErase,  // ERASEPAGE with CONFIG != Een: nothing happened
    kReadyRead,      // NVMC.READY read: value = what it read
    kPowerLost,      // the power went at this operation: addr of the half-done page or word
  };
  Kind kind;
  std::uint32_t addr;
  std::uint32_t value;
  std::uint32_t old_value;
  std::uint32_t result;
  std::uint32_t config;  // CONFIG.WEN at the time
  std::uint64_t at_us;
};

class Nrf52Chip {
 public:
  explicit Nrf52Chip(fakes::FakeClock& clock);

  // The two ports, to attach to a SwdTarget at APSEL 0 and 1 (attachTo does both).
  AccessPort& ahbAp();
  AccessPort& ctrlAp();
  void attachTo(SwdTarget& target);

  // ---- what it can be told ----------------------------------------------------------------
  bool strict = true;               // a host error fails the running test at once
  bool approtect = false;           // access-port protection on: the AHB-AP refuses memory (PS "Access port protection")
  bool protects_after_reset = false;  // a reset turns approtect on (Fxx: the port is open only once the firmware has written APPROTECT.DISABLE)
  std::uint32_t info_part = 0x52840;       // FICR.INFO.PART
  std::uint32_t info_variant = 0x41414630;  // FICR.INFO.VARIANT "AAF0" (build code Fx0, hardware repository caveat 6)
  std::uint32_t info_package = 0x2004;     // FICR.INFO.PACKAGE QIxx
  std::uint32_t info_ram = 0x100;          // FICR.INFO.RAM, 256 KB
  std::uint32_t info_flash = 0x400;        // FICR.INFO.FLASH, 1 MB
  std::uint32_t code_page_size = 4096;     // FICR.CODEPAGESIZE, as reported (the array's pages are 4 KB regardless)
  std::uint32_t code_size = 256;           // FICR.CODESIZE, as reported (the array is 1 MB regardless)
  std::uint32_t device_id[2] = {0xDEADBEEF, 0x0BADF00D};
  std::uint32_t ahb_ap_idr = 0x24770011;   // DDI 0439B Table 8-5
  std::uint32_t ctrl_ap_idr = 0x02880000;  // PS "CTRL-AP > IDR", reset value
  std::uint32_t tar_wrap_bytes = 1024;     // the auto-increment block: 1024 (ADIv5's guarantee), 4096 (the Cortex-M4's)
  std::uint32_t write_time_us = 41;        // tWRITE, typical
  std::uint32_t erase_time_us = 85000;     // tERASEPAGE, typical
  std::uint32_t ready_extra_us = 0;        // READY this much later than that
  bool ready_never = false;                // READY stays busy for ever after the next operation starts
  std::uint32_t lose_power_at_op = 0;      // 1-based count of erases and word writes; 0 = never
  std::function<void()> on_power_loss;     // what the bench does when the power goes (silence the target)
  void onlyTouch(std::uint32_t begin, std::uint32_t end);  // any erase or write outside fails the test

  // The flash's contents, for a test to arrange and to check.
  std::uint32_t flashWord(std::uint32_t addr) const;
  void setFlashWord(std::uint32_t addr, std::uint32_t value);  // as if programmed earlier
  void fillFlash(std::uint32_t begin, std::uint32_t end, std::uint32_t value);
  // The flash array is always 1 MB in 4 KB pages; code_page_size and code_size are what FICR says
  // about it (a knob for a chip that lies, or is another chip).
  std::uint32_t flashBytes() const { return static_cast<std::uint32_t>(flash_.size() * 4); }
  std::uint32_t uicrWord(std::uint32_t addr) const;
  void setUicrWord(std::uint32_t addr, std::uint32_t value);

  // ---- what the tests read ----------------------------------------------------------------
  bool powered() const { return powered_; }
  bool coreHalted() const { return halted_; }
  bool heldInReset() const { return held_in_reset_; }
  bool debugEnabled() const { return debug_en_; }
  std::uint32_t demcr() const { return demcr_; }
  std::uint32_t config() const { return config_; }
  bool nvmcBusy() const;
  std::uint32_t tar() const { return tar_; }
  std::uint32_t csw() const { return csw_; }

  struct Counters {
    unsigned resets = 0;              // SYSRESETREQ and CTRL-AP resets taken
    unsigned erases = 0;
    unsigned writes = 0;              // flash words written (CONFIG right)
    unsigned rejected_writes = 0;     // flash words written with CONFIG wrong
    unsigned rejected_erases = 0;
    unsigned overwrites = 0;          // a word written more than nWRITE (2) times since its erase
    unsigned config_writes = 0;
    unsigned ready_reads = 0;
    unsigned stalls = 0;              // AP requests answered WAIT because the NVMC was busy
    unsigned protected_accesses = 0;  // memory accesses refused by the protection
    unsigned bus_errors = 0;          // accesses to nothing
    unsigned dhcsr_writes = 0;
    unsigned aircr_resets = 0;
    unsigned tripwire = 0;            // ERASEALL, ERASEUICR, CTRL-AP ERASEALL or a UICR write attempted
    unsigned uicr_writes = 0;
    unsigned tar_wraps = 0;           // auto-increment wrapped inside its block
  };
  const Counters& counters() const { return counters_; }
  const std::vector<FlashEvent>& log() const { return log_; }
  void clearLog() { log_.clear(); }
  unsigned countLog(FlashEvent::Kind kind) const;
  const std::vector<std::string>& errors() const { return errors_; }
  int countErrors(const char* substring) const;
  void clearErrors() { errors_.clear(); }
  // Every page a flash erase or write in the log touched lies inside [begin, end).
  bool logInside(std::uint32_t begin, std::uint32_t end) const;

 private:
  class AhbAp final : public AccessPort {
   public:
    explicit AhbAp(Nrf52Chip& chip) : chip_(chip) {}
    ApAccess read(std::uint8_t addr, std::uint32_t* out) override;
    ApAccess write(std::uint8_t addr, std::uint32_t value) override;
    bool stalls(std::uint8_t addr, bool write) override;

   private:
    Nrf52Chip& chip_;
  };
  class CtrlAp final : public AccessPort {
   public:
    explicit CtrlAp(Nrf52Chip& chip) : chip_(chip) {}
    ApAccess read(std::uint8_t addr, std::uint32_t* out) override;
    ApAccess write(std::uint8_t addr, std::uint32_t value) override;

   private:
    Nrf52Chip& chip_;
  };

  ApAccess readMem(std::uint32_t addr, std::uint32_t* out);
  ApAccess writeMem(std::uint32_t addr, std::uint32_t value);
  ApAccess readFicr(std::uint32_t addr, std::uint32_t* out);
  ApAccess readNvmc(std::uint32_t addr, std::uint32_t* out);
  ApAccess writeNvmc(std::uint32_t addr, std::uint32_t value);
  ApAccess readScs(std::uint32_t addr, std::uint32_t* out);
  ApAccess writeScs(std::uint32_t addr, std::uint32_t value);
  void writeFlash(std::uint32_t addr, std::uint32_t value);
  void erasePage(std::uint32_t addr);
  void localReset();
  void startBusy(std::uint64_t us);
  bool flashOpCountsDown();  // the power-loss countdown: true when this operation is the one
  void losePower(std::uint32_t addr);
  void autoIncrement();
  bool inFlash(std::uint32_t addr) const { return addr < flashBytes(); }
  void fail(const std::string& what);
  void record(FlashEvent::Kind kind, std::uint32_t addr, std::uint32_t value, std::uint32_t old_value,
              std::uint32_t result);
  std::string hex(std::uint32_t v) const;

  fakes::FakeClock& clock_;
  AhbAp ahb_;
  CtrlAp ctrl_;

  std::vector<std::uint32_t> flash_;
  std::vector<std::uint8_t> writes_since_erase_;
  std::vector<std::uint32_t> ram_;
  std::uint32_t uicr_[0x308 / 4];

  // the AHB-AP
  std::uint32_t csw_;
  std::uint32_t tar_ = 0;

  // the NVMC
  std::uint32_t config_ = 0;
  std::uint64_t busy_until_us_ = 0;
  bool busy_for_ever_ = false;

  // the core
  bool halted_ = false;
  bool debug_en_ = false;
  bool held_in_reset_ = false;
  bool reset_sticky_ = false;
  std::uint32_t demcr_ = 0;

  bool powered_ = true;
  unsigned flash_ops_ = 0;
  bool only_touch_ = false;
  std::uint32_t only_begin_ = 0;
  std::uint32_t only_end_ = 0;

  Counters counters_;
  std::vector<FlashEvent> log_;
  std::vector<std::string> errors_;
};

}  // namespace sim
