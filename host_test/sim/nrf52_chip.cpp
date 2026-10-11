#include "sim/nrf52_chip.h"

#include <doctest.h>

#include <cstdio>

#include "core/nrf52/regs.h"
#include "core/swd/mem_ap.h"

namespace sim {

namespace regs = core::nrf52;
namespace map = core::swd::mem_ap;

namespace {
constexpr std::uint32_t kRamBase = 0x20000000;  // PS "Memory": RAM at 0x20000000, 256 KB
constexpr std::uint32_t kRamBytes = 256 * 1024;
constexpr std::uint32_t kFlashBytes = 1024 * 1024;  // PS "Memory": 1024 kB of flash
constexpr std::uint32_t kPageBytes = 4096;          // "256 pages of 4 kB each"
constexpr std::uint32_t kCpuId = 0xE000ED00;    // ARM B3.2.3; the Cortex-M4 r0p1's value, for flavour
constexpr std::uint32_t kCpuIdCortexM4 = 0x410FC241;
}  // namespace

Nrf52Chip::Nrf52Chip(fakes::FakeClock& clock)
    : clock_(clock), ahb_(*this), ctrl_(*this), csw_(map::kCswMasterTypeDebug | map::kCswHprot1 | map::kCswBit24) {
  // DDI 0439B Table 8-6: CSW resets with MasterType 1, Hprot1 1, bit 24 1, AddrInc 0, Size 0.
  flash_.assign(kFlashBytes / 4, regs::flash::kErased);
  writes_since_erase_.assign(kFlashBytes / 4, 0);
  ram_.assign(kRamBytes / 4, 0);
  for (std::uint32_t& w : uicr_) w = 0xFFFFFFFF;  // PS "UICR": "default to 0xFFFFFFFF"
  // A provisioned RAK4630: the Adafruit bootloader's address in NRFFW[0], nRESET on P0.18, the
  // port left open (HwDisabled), REG0 at 3.3 V (REGOUT0.VOUT = 5).
  setUicrWord(regs::uicr::kNrfFw0, 0x000F4000);
  setUicrWord(regs::uicr::kPselReset0, 18);
  setUicrWord(regs::uicr::kPselReset1, 18);
  setUicrWord(regs::uicr::kApprotect, 0xFFFFFF00u | regs::uicr::kApprotectHwDisabled);
  setUicrWord(regs::uicr::kRegOut0, 0xFFFFFFF8u | 5u);
}

AccessPort& Nrf52Chip::ahbAp() { return ahb_; }
AccessPort& Nrf52Chip::ctrlAp() { return ctrl_; }

void Nrf52Chip::attachTo(SwdTarget& target) {
  target.attachAp(0, ahb_);
  target.attachAp(regs::ctrl_ap::kApsel, ctrl_);
}

void Nrf52Chip::onlyTouch(std::uint32_t begin, std::uint32_t end) {
  only_touch_ = true;
  only_begin_ = begin;
  only_end_ = end;
}

std::string Nrf52Chip::hex(std::uint32_t v) const {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(v));
  return buf;
}

void Nrf52Chip::fail(const std::string& what) {
  errors_.push_back(what);
  if (strict) FAIL_CHECK("nRF52 chip: " << what);
}

int Nrf52Chip::countErrors(const char* substring) const {
  int n = 0;
  for (const std::string& e : errors_) {
    if (e.find(substring) != std::string::npos) n++;
  }
  return n;
}

void Nrf52Chip::record(FlashEvent::Kind kind, std::uint32_t addr, std::uint32_t value,
                       std::uint32_t old_value, std::uint32_t result) {
  log_.push_back(FlashEvent{kind, addr, value, old_value, result, config_, clock_.nowUs()});
}

unsigned Nrf52Chip::countLog(FlashEvent::Kind kind) const {
  unsigned n = 0;
  for (const FlashEvent& e : log_) n += e.kind == kind;
  return n;
}

bool Nrf52Chip::logInside(std::uint32_t begin, std::uint32_t end) const {
  for (const FlashEvent& e : log_) {
    if (e.kind == FlashEvent::Kind::kErasePage) {
      if (e.addr < begin || e.addr + kPageBytes > end) return false;
    } else if (e.kind == FlashEvent::Kind::kWrite || e.kind == FlashEvent::Kind::kRejectedWrite) {
      if (e.addr < begin || e.addr + 4 > end) return false;
    }
  }
  return true;
}

std::uint32_t Nrf52Chip::flashWord(std::uint32_t addr) const {
  return inFlash(addr) ? flash_[addr / 4] : regs::flash::kErased;
}

void Nrf52Chip::setFlashWord(std::uint32_t addr, std::uint32_t value) {
  if (!inFlash(addr)) return;
  flash_[addr / 4] = value;
  writes_since_erase_[addr / 4] = value == regs::flash::kErased ? 0 : 1;
}

void Nrf52Chip::fillFlash(std::uint32_t begin, std::uint32_t end, std::uint32_t value) {
  for (std::uint32_t a = begin; a < end && inFlash(a); a += 4) setFlashWord(a, value);
}

std::uint32_t Nrf52Chip::uicrWord(std::uint32_t addr) const {
  if (addr < regs::uicr::kBase || addr >= regs::uicr::kEnd) return 0xFFFFFFFF;
  return uicr_[(addr - regs::uicr::kBase) / 4];
}

void Nrf52Chip::setUicrWord(std::uint32_t addr, std::uint32_t value) {
  if (addr < regs::uicr::kBase || addr >= regs::uicr::kEnd) return;
  uicr_[(addr - regs::uicr::kBase) / 4] = value;
}

// ---- time and power ---------------------------------------------------------------------------

bool Nrf52Chip::nvmcBusy() const { return busy_for_ever_ || clock_.nowUs() < busy_until_us_; }

void Nrf52Chip::startBusy(std::uint64_t us) {
  if (ready_never) {
    busy_for_ever_ = true;
    return;
  }
  busy_until_us_ = clock_.nowUs() + us + ready_extra_us;
}

bool Nrf52Chip::flashOpCountsDown() {
  flash_ops_++;
  return lose_power_at_op != 0 && flash_ops_ == lose_power_at_op;
}

// The rail went: the chip stops answering (the bench silences the target), the flash keeps
// whatever the operation had reached.
void Nrf52Chip::losePower(std::uint32_t addr) {
  powered_ = false;
  busy_until_us_ = 0;
  busy_for_ever_ = false;
  record(FlashEvent::Kind::kPowerLost, addr, 0, 0, 0);
  if (on_power_loss) on_power_loss();
}

// ---- the AHB-AP ---------------------------------------------------------------------------------

// ADIv5 C2.2.2, Increment single: TAR + 4 after a successful DRW access, guaranteed only in the
// low ten bits; here it wraps inside tar_wrap_bytes, the strict reading (1 KB) by default.
void Nrf52Chip::autoIncrement() {
  if ((csw_ & map::kCswAddrIncMask) != map::kCswAddrIncSingle) return;
  const std::uint32_t block = tar_ & ~(tar_wrap_bytes - 1);
  const std::uint32_t next = (tar_ + 4) & (tar_wrap_bytes - 1);
  if (next == 0) counters_.tar_wraps++;
  tar_ = block | next;
}

bool Nrf52Chip::AhbAp::stalls(std::uint8_t addr, bool write) {
  (void)write;
  if (!chip_.powered_) return false;
  // A DRW access to the flash while the NVMC is busy stalls on the bus (PS NVMC: the flash is
  // not accessible during a write or erase; ADIv5 C2.2.3, stalling accesses): the DP sees WAIT.
  if (addr == map::kDrw && chip_.inFlash(chip_.tar_) && chip_.nvmcBusy()) {
    chip_.counters_.stalls++;
    return true;
  }
  return false;
}

ApAccess Nrf52Chip::AhbAp::read(std::uint8_t addr, std::uint32_t* out) {
  *out = 0;
  if (!chip_.powered_) return ApAccess::kError;
  switch (addr) {
    case map::kCsw:
      // DeviceEn (C2.6.5, "Transactions can be issued through the MEM-AP") reads 0 while the
      // access port protection has the AHB-AP disabled (PS "Access port protection").
      *out = (chip_.csw_ & ~map::kCswDeviceEn) | (chip_.approtect ? 0 : map::kCswDeviceEn);
      return ApAccess::kOk;
    case map::kTar:
      if (chip_.approtect) {
        chip_.counters_.protected_accesses++;
        return ApAccess::kError;
      }
      *out = chip_.tar_;
      return ApAccess::kOk;
    case map::kDrw: {
      if (chip_.approtect) {
        chip_.counters_.protected_accesses++;
        return ApAccess::kError;
      }
      if ((chip_.csw_ & map::kCswSizeMask) != map::kCswSizeWord) {
        chip_.fail("a DRW access with CSW.Size " + std::to_string(chip_.csw_ & map::kCswSizeMask) +
                   ": only word accesses are modelled (and the ones this host uses)");
        return ApAccess::kError;
      }
      const ApAccess a = chip_.readMem(chip_.tar_, out);
      if (a == ApAccess::kOk) chip_.autoIncrement();  // "if the DRW transaction completes with an error response ... the TAR is not incremented"
      return a;
    }
    case map::kIdr:
      *out = chip_.ahb_ap_idr;
      return ApAccess::kOk;
    case 0xF8:  // DBGDRAR, the ROM table address (DDI 0439B Table 8-5)
      *out = 0xE00FF003;
      return ApAccess::kOk;
    default:
      if (addr >= 0x10 && addr <= 0x1C) {
        chip_.fail("a banked data register (BD0-BD3) access: not modelled, and this host does not use them");
        return ApAccess::kError;
      }
      return ApAccess::kOk;  // DDI 0439B 8.2.2: "Any register that is not specified in this table reads as zero"
  }
}

ApAccess Nrf52Chip::AhbAp::write(std::uint8_t addr, std::uint32_t value) {
  if (!chip_.powered_) return ApAccess::kError;
  switch (addr) {
    case map::kCsw:
      // The RW fields; DeviceEn and TrInProg are read-only.
      chip_.csw_ = value & ~(map::kCswDeviceEn | map::kCswTrInProg);
      return ApAccess::kOk;
    case map::kTar:
      if (chip_.approtect) {
        chip_.counters_.protected_accesses++;
        return ApAccess::kError;
      }
      chip_.tar_ = value;
      return ApAccess::kOk;
    case map::kDrw: {
      if (chip_.approtect) {
        chip_.counters_.protected_accesses++;
        return ApAccess::kError;
      }
      if ((chip_.csw_ & map::kCswSizeMask) != map::kCswSizeWord) {
        chip_.fail("a DRW access with CSW.Size " + std::to_string(chip_.csw_ & map::kCswSizeMask) +
                   ": only word accesses are modelled (and the ones this host uses)");
        return ApAccess::kError;
      }
      const ApAccess a = chip_.writeMem(chip_.tar_, value);
      if (a == ApAccess::kOk) chip_.autoIncrement();
      return a;
    }
    case map::kIdr:
    case 0xF8:
      return ApAccess::kOk;  // read-only: the write is ignored
    default:
      if (addr >= 0x10 && addr <= 0x1C) {
        chip_.fail("a banked data register (BD0-BD3) access: not modelled, and this host does not use them");
        return ApAccess::kError;
      }
      return ApAccess::kOk;
  }
}

// ---- the CTRL-AP (PS "Debug and trace > CTRL-AP") -------------------------------------------------

ApAccess Nrf52Chip::CtrlAp::read(std::uint8_t addr, std::uint32_t* out) {
  *out = 0;
  if (!chip_.powered_) return ApAccess::kError;
  switch (addr) {
    case regs::ctrl_ap::kReset: *out = chip_.held_in_reset_ ? 1 : 0; return ApAccess::kOk;
    case regs::ctrl_ap::kEraseAll: return ApAccess::kOk;  // write-only: reads as zero
    case regs::ctrl_ap::kEraseAllStatus: *out = 0; return ApAccess::kOk;  // "Ready"
    case regs::ctrl_ap::kApprotectStatus:
      // "Enabled (0): Access port protection enabled. Disabled (1): Access port protection not enabled."
      *out = chip_.approtect ? 0 : regs::ctrl_ap::kApprotectStatusOpen;
      return ApAccess::kOk;
    case regs::ctrl_ap::kIdr: *out = chip_.ctrl_ap_idr; return ApAccess::kOk;
    default: return ApAccess::kOk;
  }
}

ApAccess Nrf52Chip::CtrlAp::write(std::uint8_t addr, std::uint32_t value) {
  if (!chip_.powered_) return ApAccess::kError;
  switch (addr) {
    case regs::ctrl_ap::kReset:
      // "Reset (1): Reset is active. Device is held in reset." Released: the system resets.
      if ((value & 1u) != 0) {
        chip_.held_in_reset_ = true;
        chip_.halted_ = false;
      } else if (chip_.held_in_reset_) {
        chip_.held_in_reset_ = false;
        chip_.localReset();
      }
      return ApAccess::kOk;
    case regs::ctrl_ap::kEraseAll:
      if ((value & 1u) != 0) {
        // THE TRIPWIRE. "Erase all flash and RAM" - and UICR with them: the bootloader, the
        // SoftDevice, the node's identity, REGOUT0. S-5: never, by anything automatic.
        chip_.counters_.tripwire++;
        chip_.fail("CTRL-AP ERASEALL written: a chip erase, which this firmware must never perform (S-5; "
                   "hardware repository BENCH-BREADBOARD.md caveat 6). Nothing was erased here, and the test fails.");
      }
      return ApAccess::kOk;
    default:
      return ApAccess::kOk;  // ERASEALLSTATUS, APPROTECTSTATUS, IDR are read-only
  }
}

// ---- the memory map (PS "Memory") ---------------------------------------------------------------

ApAccess Nrf52Chip::readMem(std::uint32_t addr, std::uint32_t* out) {
  *out = 0;
  if ((addr & 3u) != 0) {
    fail("an unaligned word access at " + hex(addr));
    return ApAccess::kError;
  }
  if (inFlash(addr)) {
    *out = flash_[addr / 4];
    return ApAccess::kOk;
  }
  if (addr >= kRamBase && addr < kRamBase + kRamBytes) {
    *out = ram_[(addr - kRamBase) / 4];
    return ApAccess::kOk;
  }
  if (addr >= regs::ficr::kBase && addr < regs::ficr::kBase + 0x1000) return readFicr(addr, out);
  if (addr >= regs::uicr::kBase && addr < regs::uicr::kEnd) {
    *out = uicr_[(addr - regs::uicr::kBase) / 4];
    return ApAccess::kOk;
  }
  if (addr >= regs::nvmc::kBase && addr < regs::nvmc::kBase + 0x1000) return readNvmc(addr, out);
  if (addr >= 0xE000E000 && addr < 0xE000F000) return readScs(addr, out);
  counters_.bus_errors++;
  errors_.push_back("a read of nothing at " + hex(addr));  // recorded, not a failure: the host sees FAULT
  return ApAccess::kError;
}

ApAccess Nrf52Chip::writeMem(std::uint32_t addr, std::uint32_t value) {
  if ((addr & 3u) != 0) {
    fail("an unaligned word access at " + hex(addr));
    return ApAccess::kError;
  }
  if (inFlash(addr)) {
    writeFlash(addr, value);
    return ApAccess::kOk;
  }
  if (addr >= kRamBase && addr < kRamBase + kRamBytes) {
    ram_[(addr - kRamBase) / 4] = value;
    return ApAccess::kOk;
  }
  if (addr >= regs::ficr::kBase && addr < regs::ficr::kBase + 0x1000) {
    fail("a write to FICR at " + hex(addr) + ": FICR is \"pre-programmed in factory\" and read-only (PS FICR)");
    return ApAccess::kError;
  }
  if (addr >= regs::uicr::kBase && addr < regs::uicr::kEnd) {
    // THE TRIPWIRE. UICR is read-only to this firmware (regs.h): REGOUT0, APPROTECT, the
    // bootloader's address live here. Nothing is changed, and the test fails.
    counters_.tripwire++;
    counters_.uicr_writes++;
    fail("a write to UICR at " + hex(addr) + " (value " + hex(value) +
         "): UICR is read-only to this firmware (S-5; core/nrf52/regs.h). Nothing was written, and the test fails.");
    return ApAccess::kOk;
  }
  if (addr >= regs::nvmc::kBase && addr < regs::nvmc::kBase + 0x1000) return writeNvmc(addr, value);
  if (addr >= 0xE000E000 && addr < 0xE000F000) return writeScs(addr, value);
  counters_.bus_errors++;
  errors_.push_back("a write to nothing at " + hex(addr));
  return ApAccess::kError;
}

// PS "FICR": the registers this chip has values for; the rest read as "Unspecified".
ApAccess Nrf52Chip::readFicr(std::uint32_t addr, std::uint32_t* out) {
  switch (addr) {
    case regs::ficr::kCodePageSize: *out = code_page_size; break;
    case regs::ficr::kCodeSize: *out = code_size; break;
    case regs::ficr::kDeviceId0: *out = device_id[0]; break;
    case regs::ficr::kDeviceId1: *out = device_id[1]; break;
    case regs::ficr::kInfoPart: *out = info_part; break;
    case regs::ficr::kInfoVariant: *out = info_variant; break;
    case regs::ficr::kInfoPackage: *out = info_package; break;
    case regs::ficr::kInfoRam: *out = info_ram; break;
    case regs::ficr::kInfoFlash: *out = info_flash; break;
    default: *out = regs::ficr::kUnspecified; break;
  }
  return ApAccess::kOk;
}

// PS "NVMC > Registers".
ApAccess Nrf52Chip::readNvmc(std::uint32_t addr, std::uint32_t* out) {
  switch (addr) {
    case regs::nvmc::kReady:
    case regs::nvmc::kReadyNext:
      *out = nvmcBusy() ? 0 : regs::nvmc::kReadyBit;
      if (addr == regs::nvmc::kReady) {
        counters_.ready_reads++;
        record(FlashEvent::Kind::kReadyRead, addr, *out, 0, 0);
      }
      return ApAccess::kOk;
    case regs::nvmc::kConfig: *out = config_; return ApAccess::kOk;
    case regs::nvmc::kErasePage:
    case regs::nvmc::kEraseAll:
    case regs::nvmc::kEraseUicr:
    case regs::nvmc::kErasePagePartial:
    case regs::nvmc::kErasePagePartialCfg:
      *out = 0;
      return ApAccess::kOk;
    default:
      *out = 0;
      return ApAccess::kOk;
  }
}

ApAccess Nrf52Chip::writeNvmc(std::uint32_t addr, std::uint32_t value) {
  switch (addr) {
    case regs::nvmc::kConfig:
      config_ = value & regs::nvmc::kConfigMask;
      counters_.config_writes++;
      record(FlashEvent::Kind::kConfig, addr, config_, 0, 0);
      return ApAccess::kOk;
    case regs::nvmc::kErasePage:
      // "When erase is enabled, the flash memory can be erased page by page using the ERASEPAGE":
      // the value is "the address to the page to be erased".
      if (config_ != regs::nvmc::kConfigEen) {
        counters_.rejected_erases++;
        record(FlashEvent::Kind::kRejectedErase, value, value, 0, 0);
        fail("ERASEPAGE written (" + hex(value) + ") with NVMC.CONFIG.WEN = " + std::to_string(config_) +
             ", not Een (2): nothing erased (PS NVMC \"Erasing a page in flash\")");
        return ApAccess::kOk;
      }
      if (!inFlash(value) || (value % kPageBytes) != 0) {
        fail("ERASEPAGE written with " + hex(value) + ": not the address of a flash page");
        return ApAccess::kOk;
      }
      if (nvmcBusy()) {
        fail("ERASEPAGE written while the NVMC is busy (READY = 0)");
        return ApAccess::kOk;
      }
      erasePage(value);
      return ApAccess::kOk;
    case regs::nvmc::kEraseAll:
      if ((value & 1u) != 0) {
        // THE TRIPWIRE: "Flash and UICR can be erased completely in one operation by using the
        // ERASEALL" (PS NVMC "Erase all"). S-5: never, by anything automatic.
        counters_.tripwire++;
        fail("NVMC.ERASEALL written: the chip erase this firmware must never perform (S-5). Nothing was erased here, and the test fails.");
      }
      return ApAccess::kOk;
    case regs::nvmc::kEraseUicr:
      if ((value & 1u) != 0) {
        // THE TRIPWIRE: "UICR can be erased using the ERASEUICR" (PS NVMC "Erase UICR") - REGOUT0
        // with it (hardware repository, BENCH-BREADBOARD.md caveat 5).
        counters_.tripwire++;
        fail("NVMC.ERASEUICR written: the UICR erase this firmware must never perform (S-5). Nothing was erased here, and the test fails.");
      }
      return ApAccess::kOk;
    case regs::nvmc::kErasePagePartial:
    case regs::nvmc::kErasePagePartialCfg:
      fail("a partial page erase register written: not modelled, and this host does not use it");
      return ApAccess::kOk;
    default:
      return ApAccess::kOk;  // READY, READYNEXT: read-only
  }
}

// "The NVMC is only able to write 0 to bits in flash memory that are erased (set to 1). It
// cannot rewrite a bit back to 1" - the result is old & value. "The same 32-bit word in flash
// memory can only be written nWRITE number of times before a page erase must be performed":
// nWRITE is 2, and a third write is counted. Write needs CONFIG.WEN = Wen.
void Nrf52Chip::writeFlash(std::uint32_t addr, std::uint32_t value) {
  const std::uint32_t old = flash_[addr / 4];
  if (config_ != regs::nvmc::kConfigWen) {
    counters_.rejected_writes++;
    record(FlashEvent::Kind::kRejectedWrite, addr, value, old, old);
    fail("a flash write at " + hex(addr) + " with NVMC.CONFIG.WEN = " + std::to_string(config_) +
         ", not Wen (1): nothing written (PS NVMC \"Writing to flash\")");
    return;
  }
  if (only_touch_ && (addr < only_begin_ || addr + 4 > only_end_)) {
    fail("a flash write at " + hex(addr) + " outside the region the test allowed [" + hex(only_begin_) + ", " +
         hex(only_end_) + ")");
  }
  if (flashOpCountsDown()) {
    // Half-written: the power went with the low half of the word still to program.
    const std::uint32_t result = old & (value | 0x0000FFFFu);
    flash_[addr / 4] = result;
    record(FlashEvent::Kind::kWrite, addr, value, old, result);
    losePower(addr);
    return;
  }
  const std::uint32_t result = old & value;
  flash_[addr / 4] = result;
  if (writes_since_erase_[addr / 4] < 255) writes_since_erase_[addr / 4]++;
  if (writes_since_erase_[addr / 4] > regs::nvmc::kWritesPerEraseMax) counters_.overwrites++;
  counters_.writes++;
  record(FlashEvent::Kind::kWrite, addr, value, old, result);
  startBusy(write_time_us);
}

// "After erasing a flash page, all bits in the page are set to 1."
void Nrf52Chip::erasePage(std::uint32_t addr) {
  if (only_touch_ && (addr < only_begin_ || addr + kPageBytes > only_end_)) {
    fail("a page erase at " + hex(addr) + " outside the region the test allowed [" + hex(only_begin_) + ", " +
         hex(only_end_) + ")");
  }
  const std::uint32_t words = kPageBytes / 4;
  if (flashOpCountsDown()) {
    // Half-erased: the first half of the page is blank, the next word half so, the rest as it was.
    for (std::uint32_t i = 0; i < words / 2; ++i) {
      flash_[addr / 4 + i] = regs::flash::kErased;
      writes_since_erase_[addr / 4 + i] = 0;
    }
    flash_[addr / 4 + words / 2] |= 0x0000FFFFu;
    record(FlashEvent::Kind::kErasePage, addr, 0, 0, 0);
    losePower(addr);
    return;
  }
  for (std::uint32_t i = 0; i < words; ++i) {
    flash_[addr / 4 + i] = regs::flash::kErased;
    writes_since_erase_[addr / 4 + i] = 0;
  }
  counters_.erases++;
  record(FlashEvent::Kind::kErasePage, addr, 0, 0, 0);
  startBusy(erase_time_us);
}

// ---- the core (ARM C1.6.2 DHCSR, C1.6.5 DEMCR, B3.2.6 AIRCR) -----------------------------------

ApAccess Nrf52Chip::readScs(std::uint32_t addr, std::uint32_t* out) {
  switch (addr) {
    case regs::scs::kDhcsr: {
      std::uint32_t v = 0;
      if (reset_sticky_) v |= regs::scs::kSResetSt;  // "clears to 0 on a read of DHCSR"
      reset_sticky_ = false;
      if (!halted_ && !held_in_reset_) v |= regs::scs::kSRetireSt;  // "software execution is progressing"
      if (halted_) v |= regs::scs::kSHalt | regs::scs::kCHalt;
      if (debug_en_) v |= regs::scs::kCDebugEn;
      *out = v;
      return ApAccess::kOk;
    }
    case regs::scs::kDemcr: *out = demcr_; return ApAccess::kOk;
    case regs::scs::kAircr: *out = regs::scs::kVectKeyStat; return ApAccess::kOk;  // "On reads, returns 0xFA05"; little-endian, PRIGROUP 0
    case kCpuId: *out = kCpuIdCortexM4; return ApAccess::kOk;
    default:
      counters_.bus_errors++;
      errors_.push_back("a read of an unmodelled SCS register at " + hex(addr));
      return ApAccess::kError;
  }
}

ApAccess Nrf52Chip::writeScs(std::uint32_t addr, std::uint32_t value) {
  switch (addr) {
    case regs::scs::kDhcsr: {
      counters_.dhcsr_writes++;
      // "A debugger must write 0xA05F to this field to enable write accesses to bits[15:0],
      // otherwise the processor ignores the write access."
      if ((value & 0xFFFF0000u) != regs::scs::kDbgKey) {
        fail("DHCSR written without the debug key 0xA05F in bits [31:16] (ARM C1.6.2): ignored");
        return ApAccess::kOk;
      }
      const bool want_debug = (value & regs::scs::kCDebugEn) != 0;
      if (want_debug && (csw_ & map::kCswMasterTypeDebug) == 0) {
        // DDI 0439B Table 8-6, note a: MasterType clear "prevents the debugger from setting the
        // C_DEBUGEN bit ... and so prevents the debugger from being able to halt the processor".
        fail("DHCSR.C_DEBUGEN set with AHB-AP CSW.MasterType = 0: ignored (DDI 0439B Table 8-6 note a)");
        return ApAccess::kOk;
      }
      debug_en_ = want_debug;
      if (!debug_en_) {
        // "When C_DEBUGEN is set to 0, the processor ignores the values of all other bits":
        // halting debug off means the core is not held.
        halted_ = false;
        return ApAccess::kOk;
      }
      if (held_in_reset_) return ApAccess::kOk;
      // C1.5 "a debug event halts the processor in Debug state"; C1.5.4 "exits Debug state when
      // the debugger writes 0 to DHCSR.C_HALT".
      halted_ = (value & regs::scs::kCHalt) != 0;
      return ApAccess::kOk;
    }
    case regs::scs::kDemcr:
      demcr_ = value;
      return ApAccess::kOk;
    case regs::scs::kAircr:
      // "Register writes must write 0x05FA to this field, otherwise the write is ignored."
      if ((value & 0xFFFF0000u) != regs::scs::kVectKey) {
        fail("AIRCR written without the key 0x05FA in bits [31:16] (ARM B3.2.6): ignored");
        return ApAccess::kOk;
      }
      if ((value & regs::scs::kVectReset) != 0) {
        fail("AIRCR.VECTRESET written: UNPREDICTABLE unless halted, and never this host's (ARM B3.2.6)");
      }
      if ((value & regs::scs::kSysResetReq) != 0) {
        // "Writing 1 to this bit asserts a signal to the external system to request a Local reset."
        counters_.aircr_resets++;
        localReset();
      }
      return ApAccess::kOk;
    default:
      counters_.bus_errors++;
      errors_.push_back("a write to an unmodelled SCS register at " + hex(addr));
      return ApAccess::kError;
  }
}

// A Local reset (ARM B1.5.5; C1.4.1): the core comes out of reset halted when C_DEBUGEN and
// DEMCR.VC_CORERESET are set ("When the processor comes out of reset it sets DHCSR.C_HALT to 1,
// and enters Debug state"), running otherwise; S_RESET_ST is set; DEMCR keeps all but its
// DebugMonitor bits [19:16] (C1.6.5: "A Local reset resets only the bits related to the
// DebugMonitor"); C_DEBUGEN survives (B1.5.1: "A power-on reset resets the processor, System
// Control Space and debug logic. A Local reset resets the processor and System Control Space,
// except for some fault and debug related resources"). The
// NVMC comes up read-only, nothing busy. And on a module whose firmware opens the port at run
// time, the port is closed again until that firmware runs (PS "Access port protection"; Nordic's
// APPROTECT note).
void Nrf52Chip::localReset() {
  counters_.resets++;
  reset_sticky_ = true;
  halted_ = debug_en_ && (demcr_ & regs::scs::kVcCoreReset) != 0;
  demcr_ &= ~0x000F0000u;
  config_ = regs::nvmc::kConfigRen;
  busy_until_us_ = 0;
  busy_for_ever_ = false;
  if (protects_after_reset) approtect = true;
}

}  // namespace sim
