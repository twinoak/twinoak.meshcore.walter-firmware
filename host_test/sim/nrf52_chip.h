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
// Since task p5-02 (S-5a, one door) the chip also knows what the door does, from the Product
// Specification's "Debug and trace > Access port protection" and "CTRL-AP" and from NVMC
// "Writing to user information configuration registers": the two kinds of protection by
// INFO.VARIANT's build code - hardware-only on Dxx and earlier ("legacy": open while
// UICR.APPROTECT reads 0xFF, read at every reset), hardware-and-software on Fxx and later
// ("improved": enabled by default, open after ERASEALL until a pin, power or brownout reset,
// and after such a reset only when UICR.APPROTECT is HwDisabled *and* the firmware in flash
// writes APPROTECT.DISABLE); the CTRL-AP's ERASEALL (flash, UICR and RAM to 0xFF,
// ERASEALLSTATUS busy for a set number of reads); its RESET (a soft reset); a pin reset and a
// power-on, each counted by kind; and UICR written like flash, through the NVMC with CONFIG =
// Wen, each word at most nWRITE times between erases. **The tripwire stays armed by default**:
// only a test that calls allowProvisioning() - a test about the door - gets the erase and the
// UICR writes performed instead of a failed test. NVMC.ERASEALL and NVMC.ERASEUICR trip always;
// the door does not use them.
//
// Knobs: protection on; protection re-engaged by a reset (a build code Fxx module whose firmware
// opens the port at run time); another part, page size or page count; READY late or never;
// power lost at the Nth flash operation, leaving a page half-erased or a word half-written;
// the auto-increment wrap; a region outside which any erase or write fails the test; the three
// factory states; whether the bootloader in flash opens the port; how many ERASEALLSTATUS reads
// stay busy, or for ever; whether a CTRL-AP soft reset leaves the debug port needing a reconnect.
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
    kEraseAll,       // the CTRL-AP's ERASEALL performed (allowProvisioning(), task p5-02): flash, UICR, RAM
    kUicrWrite,      // a UICR word written (allowProvisioning()): addr, value, old_value, result
  };
  Kind kind;
  std::uint32_t addr;
  std::uint32_t value;
  std::uint32_t old_value;
  std::uint32_t result;
  std::uint32_t config;  // CONFIG.WEN at the time
  std::uint64_t at_us;
};

// What the whole suite did to simulated flash (task p3-06, rail R4): how many chips there were,
// how many of them were flashed, and how many erases and writes landed inside the window the test
// declared. One object for the process; host_test/rails/test_rail_flash_region.cpp prints it.
struct FlashTally {
  unsigned chips = 0;
  unsigned flashed = 0;        // chips that saw at least one erase or write
  unsigned erases = 0;
  unsigned writes = 0;
  unsigned undeclared = 0;     // chips flashed without a declared window: rail R4 broken
  unsigned outside = 0;        // erases or writes outside the declared window
  unsigned erase_alls = 0;     // CTRL-AP chip erases performed (only ever behind allowProvisioning(), task p5-02)
  unsigned uicr_writes = 0;    // UICR words written (the same)
  unsigned provisioning_chips = 0;  // chips a test armed with allowProvisioning()
};

FlashTally& flashTally();

class Nrf52Chip {
 public:
  explicit Nrf52Chip(fakes::FakeClock& clock);
  // Rail R4 (task p3-06): a chip that was erased or written by a test which never said which
  // region may be touched fails that test here. `onlyTouch()` (with `alsoTouch()` for the
  // bootloader's settings page) is the declaration, and the flasher's and the update's benches
  // make it from the board profile - so "the flasher writes only inside the application region
  // and the settings page" is asserted at the end of **every** test that flashes, by the
  // simulator, and not by each test remembering to.
  ~Nrf52Chip();

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
  void alsoTouch(std::uint32_t begin, std::uint32_t end);  // ... except inside this second range (the settings page, task p1-05)
  // The one use for this: host_test/rails/test_rail_flash_region.cpp, which shows the window
  // guard biting and must not dirty the suite's tally while doing it (task p3-06, rail R4).
  bool report_outside = true;
  // A word with bits that will not take a write (task p1-05): after any erase or write of it the
  // bits of stuck_mask read 0. 0 = no such word.
  std::uint32_t stuck_addr = 0;
  std::uint32_t stuck_mask = 0;
  // The power back after losePower(): the chip as a power-on reset leaves it, the flash as it was
  // when the power went. The countdown starts over (lose_power_at_op is cleared). Since task
  // p5-02 a power-on also re-reads the protection as the kind says (below) and resets the debug
  // port (SwdTarget::resetDap).
  void powerOn();

  // ---- the two kinds of access-port protection (task p5-02; PS "Access port protection") ----
  // By INFO.VARIANT's build code, the third character: "Dxx and earlier" hardware-only (A-D),
  // "Fxx and later" hardware-and-software (F and after; IN-141 calls Fx0 revision 3). The
  // firmware's own variant table (task p5-03) is stricter and refuses what it does not know; this
  // is what the silicon does.
  bool improvedApprotect() const;
  // Improved only: the firmware in flash writes APPROTECT.DISABLE at start-up (the MDK does since
  // 8.40.2; whether the RAK's bootloader was built with it is the first board's to say -
  // spec/SYSTEM.md 10.2). Read at a pin reset and a power-on, together with UICR.APPROTECT and
  // whether flash holds anything at all.
  bool bootloader_opens_port = true;
  // The ERASEALLSTATUS reads that still say busy after an ERASEALL (the bench tool's simulator
  // used 3), or busy for ever - the door's timeout.
  unsigned erase_all_busy_reads = 3;
  bool erase_all_never_finishes = false;
  // A CTRL-AP soft reset that leaves the debug port needing a line reset and a power-up again
  // (resetDap): the part the door must reconnect to. False: the port survives the soft reset.
  bool soft_reset_drops_dap = false;
  // THE DOOR'S TESTS ONLY. Arms the chip for what S-5a allows a person's command to do: the
  // CTRL-AP's ERASEALL is performed and UICR takes writes through the NVMC. Every other test of
  // the tree leaves this alone, and the tripwire fails it at the first of either.
  void allowProvisioning();
  bool provisioningAllowed() const { return provisioning_allowed_; }
  // The factory states (PS "Access port protection"; the bench tool's simulator): a radio as RAK
  // ships it, every flash word holding RUI3 (a pseudo-random, non-blank fill), NRFFW[0] and
  // PSELRESET set. Open: legacy UICR.APPROTECT erased (0xFFFFFFFF); improved HwDisabled (0x5A),
  // the port open. Locked: legacy "locked by firmware" (UICR.APPROTECT.PALL = 0x00, the word
  // 0xFFFFFF00); improved "locked by default" (UICR.APPROTECT erased, which on Fxx is enabled).
  void factoryOpen();
  void factoryLocked();
  // nRESET pulsed by the host (the bench wires SwdTarget::on_pin_reset to it): a pin reset.
  // Legacy: UICR.APPROTECT read again. Improved: the port is open only when UICR.APPROTECT is
  // HwDisabled and the flash holds firmware (word 0 not erased) and bootloader_opens_port. The
  // core running, halting debug off, the debug port reset.
  void pinReset();
  // Resets counted by kind, so that a test can hold S-5a's rule: no pin, power-on or brownout
  // reset between the erase and the last UICR write; the one CTRL-AP soft reset is allowed.
  struct Resets {
    unsigned ctrl_ap_soft = 0;   // CTRL-AP RESET released
    unsigned sysresetreq = 0;    // AIRCR.SYSRESETREQ
    unsigned pin = 0;            // nRESET
    unsigned power_on = 0;       // powerOn()
  };
  const Resets& resets() const { return resets_; }
  // The SoftDevice information structure a provisioned chip carries at MBR_SIZE + 0x2000 (the
  // bootloader's nrf_sdm.h:99-138): the magic, the SoftDevice's size (where the application
  // starts) and its firmware id - S140 6.1.1's 0x26000 and 0x00B6 by default (MeshCore's
  // boards/rak4631.json:34-38).
  void installSoftDevice(std::uint32_t size = 0x26000, std::uint16_t fwid = 0x00B6);

  // The two bootloaders a RAK4630 may carry, each one's bootloader_app_is_valid() as read from its
  // source (core/flasher/bootloader_settings.h cites the lines): accepts() says whether that
  // bootloader would start what is in flash now. The application's start is what the SoftDevice
  // information structure says, as the bootloader computes it (dfu_types.h:77).
  enum class Bootloader : std::uint8_t { kStock, kOtafix };
  bool bootloaderAccepts(Bootloader which) const;
  std::uint32_t applicationStart() const;

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
    unsigned uicr_writes = 0;         // UICR words written - attempted (tripped) or, behind allowProvisioning(), done
    unsigned uicr_overwrites = 0;     // a UICR word written more than nWRITE (2) times since the last erase
    unsigned erase_alls = 0;          // CTRL-AP ERASEALL performed (allowProvisioning() only)
    unsigned erase_all_status_reads = 0;
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
  void writeUicr(std::uint32_t addr, std::uint32_t value);
  void eraseAll();
  void localReset();
  void hardReset();                // what a pin reset and a power-on share
  void readProtectionAtReset();    // UICR.APPROTECT (and the firmware) read, as the kind says
  void fillFactory();              // RUI3-like contents
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
  SwdTarget* target_ = nullptr;  // attachTo(): the port to reset at a pin reset

  std::vector<std::uint32_t> flash_;
  std::vector<std::uint8_t> writes_since_erase_;
  std::vector<std::uint32_t> ram_;
  std::uint32_t uicr_[0x308 / 4];
  std::uint8_t uicr_writes_since_erase_[0x308 / 4] = {};

  // the CTRL-AP's erase (task p5-02)
  bool provisioning_allowed_ = false;
  unsigned erase_all_busy_left_ = 0;
  bool erase_all_busy_for_ever_ = false;
  Resets resets_;

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

  bool allowed(std::uint32_t addr, std::uint32_t bytes) const;

  bool powered_ = true;
  unsigned flash_ops_ = 0;
  bool only_touch_ = false;
  std::uint32_t only_begin_ = 0;
  std::uint32_t only_end_ = 0;
  bool also_touch_ = false;
  std::uint32_t also_begin_ = 0;
  std::uint32_t also_end_ = 0;

  Counters counters_;
  std::vector<FlashEvent> log_;
  std::vector<std::string> errors_;
};

}  // namespace sim
