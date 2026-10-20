#include "sim/nrf52_chip.h"

#include <doctest.h>

#include <cstdio>

#include "core/flasher/bootloader_settings.h"
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
  flashTally().chips++;
}

// The suite's own tally of what reached simulated flash (task p3-06, rail R4).
FlashTally& flashTally() {
  static FlashTally tally;
  return tally;
}

// Rail R4: an erase or a write outside the declared window already failed the test where it
// happened (`fail()` below). This is the other half - a test that flashed and never declared a
// window at all, which is a test that is not asserting the rail. The sentence names the two calls
// that declare one, so that the fix is in the message.
Nrf52Chip::~Nrf52Chip() {
  FlashTally& tally = flashTally();
  tally.erases += counters_.erases;
  tally.writes += counters_.writes;
  tally.erase_alls += counters_.erase_alls;
  if (provisioning_allowed_) {
    tally.provisioning_chips++;
    tally.uicr_writes += counters_.uicr_writes;
  }
  // A chip erase (task p5-02) is flash erased too: a door test declares the base's windows.
  const bool flashed = counters_.erases > 0 || counters_.writes > 0 || counters_.erase_alls > 0;
  if (!flashed) return;
  tally.flashed++;
  if (only_touch_) return;
  tally.undeclared++;
  FAIL_CHECK("no test may flash a chip without saying which region may be touched: this one did "
             << counters_.erases << " erase(s), " << counters_.erase_alls << " chip erase(s) and " << counters_.writes
             << " word write(s) with no onlyTouch() (docs/ARCHITECTURE.md §5; "
                "sim::Nrf52Chip::onlyTouch(begin, end) and alsoTouch() for the settings page)");
}

AccessPort& Nrf52Chip::ahbAp() { return ahb_; }
AccessPort& Nrf52Chip::ctrlAp() { return ctrl_; }

void Nrf52Chip::attachTo(SwdTarget& target) {
  target.attachAp(0, ahb_);
  target.attachAp(regs::ctrl_ap::kApsel, ctrl_);
  target_ = &target;
}

// ---- the two kinds of access-port protection (task p5-02) -------------------------------------

// INFO.VARIANT "encoded in ASCII", the build code's letter in bits [15:8] (regs.h; "AAF0" is
// F). PS "Access port protection": "Build codes Dxx and earlier" are hardware-only, "Fxx and
// later" hardware-and-software. A letter that is not text at all (an "Unspecified" 0xFFFFFFFF)
// is taken as the newer kind, the one that ships closed - the stricter answer.
bool Nrf52Chip::improvedApprotect() const {
  const std::uint32_t letter = (info_variant >> 8) & 0xFFu;
  return letter >= 'F' || letter < 'A' || letter > 'Z';
}

void Nrf52Chip::allowProvisioning() { provisioning_allowed_ = true; }

// Every flash word non-blank, as a factory RUI3 image leaves it (the bench tool's simulator used
// the same xorshift fill); RAK's bootloader address and nRESET on P0.18 in UICR; REGOUT0 at 3.3 V.
void Nrf52Chip::fillFactory() {
  std::uint32_t x = 0x2545F491u;
  for (std::size_t i = 0; i < flash_.size(); ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    flash_[i] = x | 1u;
    writes_since_erase_[i] = 1;
  }
  for (std::uint32_t& w : uicr_) w = 0xFFFFFFFF;
  for (std::uint8_t& n : uicr_writes_since_erase_) n = 0;
  setUicrWord(regs::uicr::kNrfFw0, 0x000F4000);
  setUicrWord(regs::uicr::kPselReset0, 18);
  setUicrWord(regs::uicr::kPselReset1, 18);
  setUicrWord(regs::uicr::kRegOut0, 0xFFFFFFF8u | 5u);
}

// Open at the factory: legacy, UICR.APPROTECT erased ("By default, access port protection is
// disabled"); improved, HwDisabled written and firmware that opens the port (the only way an Fxx
// part is open after a reset).
void Nrf52Chip::factoryOpen() {
  fillFactory();
  if (improvedApprotect()) {
    setUicrWord(regs::uicr::kApprotect, 0xFFFFFF00u | regs::uicr::kApprotectHwDisabled);
    bootloader_opens_port = true;
  }
  approtect = false;
}

// Locked: legacy, by its firmware - UICR.APPROTECT.PALL written Enabled (0x00), "Enabled by
// writing UICR.APPROTECT to Enabled and performing a reset"; improved, by default - the word
// erased, "By default, access port protection is enabled".
void Nrf52Chip::factoryLocked() {
  fillFactory();
  if (!improvedApprotect()) setUicrWord(regs::uicr::kApprotect, 0xFFFFFF00u | regs::uicr::kApprotectEnabled);
  approtect = true;
}

// What a pin reset and a power-on read (PS "Access port protection"). Legacy: UICR.APPROTECT,
// Disabled (0xFF) or anything else. Improved: the protection "remains disabled until" a pin,
// power or brownout reset; after one it is enabled unless UICR.APPROTECT is HwDisabled and the
// firmware writes APPROTECT.DISABLE at start-up - which needs firmware in flash at all (the
// MBR's vector table at word 0) that was built to do it (bootloader_opens_port).
void Nrf52Chip::readProtectionAtReset() {
  const std::uint32_t pall = uicrWord(regs::uicr::kApprotect) & 0xFFu;
  if (!improvedApprotect()) {
    approtect = pall != regs::uicr::kApprotectDisabled;
    return;
  }
  const bool firmware = flash_[0] != regs::flash::kErased;
  approtect = !(pall == regs::uicr::kApprotectHwDisabled && firmware && bootloader_opens_port);
}

// The part of a pin reset and a power-on that is the same (ARM B1.5.1: "resets the processor,
// System Control Space and debug logic"; PS "Reset behavior"): the core running, halting debug
// off, DEMCR clear, the NVMC read-only and idle, the access port's registers at their reset
// values, the protection read again, and the debug port's power-up gone with the rest.
void Nrf52Chip::hardReset() {
  halted_ = false;
  debug_en_ = false;
  held_in_reset_ = false;
  reset_sticky_ = true;
  demcr_ = 0;
  config_ = regs::nvmc::kConfigRen;
  busy_until_us_ = 0;
  busy_for_ever_ = false;
  erase_all_busy_left_ = 0;
  erase_all_busy_for_ever_ = false;
  tar_ = 0;
  csw_ = map::kCswMasterTypeDebug | map::kCswHprot1 | map::kCswBit24;
  counters_.resets++;
  readProtectionAtReset();
  if (target_ != nullptr) target_->resetDap();
}

void Nrf52Chip::pinReset() {
  resets_.pin++;
  hardReset();
}

void Nrf52Chip::onlyTouch(std::uint32_t begin, std::uint32_t end) {
  only_touch_ = true;
  only_begin_ = begin;
  only_end_ = end;
}

void Nrf52Chip::alsoTouch(std::uint32_t begin, std::uint32_t end) {
  also_touch_ = true;
  also_begin_ = begin;
  also_end_ = end;
}

bool Nrf52Chip::allowed(std::uint32_t addr, std::uint32_t bytes) const {
  if (!only_touch_) return true;
  if (addr >= only_begin_ && addr + bytes <= only_end_) return true;
  return also_touch_ && addr >= also_begin_ && addr + bytes <= also_end_;
}

// A power-on reset (ARM B1.5.1: "resets the processor, System Control Space and debug logic";
// PS "Reset behavior"): the core running, halting debug off, DEMCR clear, the NVMC read-only and
// idle, the access port's registers at their reset values. The flash keeps what the lost power
// left in it.
void Nrf52Chip::powerOn() {
  powered_ = true;
  flash_ops_ = 0;
  lose_power_at_op = 0;
  resets_.power_on++;
  hardReset();
}

// nrf_sdm.h: the structure's size in its first byte (0x18 for S140 6.x), SD_MAGIC_NUMBER at
// +0x04 (dfu_types.h:65), the size at +0x08, the FWID at +0x0C (16 bits), the SD id at +0x10
// (0x90 is "S140") and the version at +0x14 (6.1.1 = 6001001, SD_VERSION's encoding).
void Nrf52Chip::installSoftDevice(std::uint32_t size, std::uint16_t fwid) {
  const std::uint32_t base = 0x1000 + 0x2000;
  setFlashWord(base + 0x00, 0x00000018);
  setFlashWord(base + 0x04, 0x51B1E5DB);
  setFlashWord(base + 0x08, size);
  setFlashWord(base + 0x0C, 0xFFFF0000u | fwid);
  setFlashWord(base + 0x10, 0x00000090);
  setFlashWord(base + 0x14, 6001001);
  // And a SoftDevice that is not blank at its start, like a real one (the MBR's vector table).
  setFlashWord(0x0000, 0x20000400);
  setFlashWord(0x0004, 0x00000A81);
}

// dfu_types.h:77: CODE_REGION_1_START = is_sd_existed() ? SD_SIZE_GET(MBR_SIZE) : MBR_SIZE.
std::uint32_t Nrf52Chip::applicationStart() const {
  const std::uint32_t base = 0x1000 + 0x2000;
  if (flashWord(base + 0x04) == 0x51B1E5DB) return flashWord(base + 0x08);
  return 0x1000;
}

// bootloader_app_is_valid(), from the two sources named in core/flasher/bootloader_settings.h:
// stock c67f0bc bootloader.c:175-224, OTAFIX 3a75482 bootloader.c:148-182.
bool Nrf52Chip::bootloaderAccepts(Bootloader which) const {
  namespace bs = core::flasher::bootloader_settings;
  const std::uint32_t app_addr = applicationStart();
  const std::uint32_t sp_addr = flashWord(app_addr);
  const std::uint32_t reset_addr = flashWord(app_addr + 4);
  // Both: "Application is invalid if first 2 words are all 0xFFFFFFF" (stock :183-186, OTAFIX :158-163).
  if (sp_addr == 0xFFFFFFFFu && reset_addr == 0xFFFFFFFFu) return false;
  std::uint32_t words[bs::kRecordWords];
  for (std::size_t i = 0; i < bs::kRecordWords; ++i) words[i] = flashWord(0xFF000 + 4 * static_cast<std::uint32_t>(i));
  const bs::Record s = bs::decode(words);
  // Both: "The application in CODE region 1 is flagged as valid during update" (stock :192-204,
  // OTAFIX :166-178), CRC-16 (crc16.c:46-60, CCITT-FALSE) over bank_0_size bytes unless the stored
  // CRC is 0.
  if (s.bank_0 == bs::kBankValidApp) {
    std::uint16_t image_crc = 0;
    if (s.bank_0_crc != 0) {
      image_crc = 0xFFFF;
      for (std::uint32_t i = 0; i < s.bank_0_size; ++i) {
        const std::uint32_t a = app_addr + i;
        const std::uint8_t byte = static_cast<std::uint8_t>(flashWord(a & ~3u) >> (8 * (a & 3u)));
        image_crc = static_cast<std::uint16_t>(image_crc ^ (static_cast<std::uint16_t>(byte) << 8));
        for (int bit = 0; bit < 8; ++bit) {
          image_crc = (image_crc & 0x8000u) != 0 ? static_cast<std::uint16_t>((image_crc << 1) ^ 0x1021u)
                                                 : static_cast<std::uint16_t>(image_crc << 1);
        }
      }
    }
    return image_crc == s.bank_0_crc;
  }
  // Stock only (commit 67105f8, :207-222): "Fallback for debugger-flashed apps: settings page is
  // erased (bank_0 = 0xFFFF) ... Accept it if its Cortex-M vector table is plausible: initial SP
  // within RAM, reset vector within app flash region with the Thumb bit set."
  if (which == Bootloader::kStock && s.bank_0 == bs::kBankBlank) {
    const std::uint32_t ram_start = 0x20000000u;
    const std::uint32_t ram_end = ram_start + (info_ram << 10u);
    const bool sp_valid = sp_addr >= ram_start && sp_addr <= ram_end && (sp_addr & 3u) == 0;
    const bool reset_in_app = (reset_addr & 1u) != 0 && reset_addr >= app_addr && reset_addr < 0xF4000u;
    return sp_valid && reset_in_app;
  }
  return false;  // stock :224 / OTAFIX :181
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
    case regs::ctrl_ap::kEraseAllStatus:
      // "Status register for the ERASEALL operation": Ready (0) or Busy (1). Busy for the set
      // number of reads after an erase (task p5-02), or for ever when told to.
      chip_.counters_.erase_all_status_reads++;
      if (chip_.erase_all_busy_for_ever_) {
        *out = 1;
      } else if (chip_.erase_all_busy_left_ > 0) {
        chip_.erase_all_busy_left_--;
        *out = 1;
      } else {
        *out = 0;
      }
      return ApAccess::kOk;
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
        // "Soft reset triggered through CTRL-AP" (task p5-02): a soft reset, counted as such.
        // Legacy: the protection is UICR.APPROTECT as read at any reset (localReset). Improved:
        // the port stays as it is - the protection "remains disabled until" a pin, power or
        // brownout reset, and a soft reset is none of those. The debug port itself survives
        // unless the knob says this part drops it.
        chip_.held_in_reset_ = false;
        chip_.resets_.ctrl_ap_soft++;
        chip_.localReset();
        if (chip_.soft_reset_drops_dap && chip_.target_ != nullptr) chip_.target_->resetDap();
      }
      return ApAccess::kOk;
    case regs::ctrl_ap::kEraseAll:
      if ((value & 1u) != 0) {
        if (chip_.provisioning_allowed_) {
          // S-5a's one door (task p5-02): a test about it armed the chip, and the erase happens.
          chip_.eraseAll();
          return ApAccess::kOk;
        }
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
    if (provisioning_allowed_) {
      // S-5a's one door (task p5-02): the base's UICR words and APPROTECT, through the NVMC.
      writeUicr(addr, value);
      return ApAccess::kOk;
    }
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
  if (!allowed(addr, 4)) {
    if (report_outside) flashTally().outside++;
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
  std::uint32_t result = old & value;
  if (stuck_mask != 0 && addr == stuck_addr) result &= ~stuck_mask;
  flash_[addr / 4] = result;
  if (writes_since_erase_[addr / 4] < 255) writes_since_erase_[addr / 4]++;
  if (writes_since_erase_[addr / 4] > regs::nvmc::kWritesPerEraseMax) counters_.overwrites++;
  counters_.writes++;
  record(FlashEvent::Kind::kWrite, addr, value, old, result);
  startBusy(write_time_us);
}

// PS NVMC "Writing to user information configuration registers (UICR)": UICR "is written in the
// same way as flash" - CONFIG.WEN = Wen, a word-aligned 32-bit word, bits only cleared, nWRITE
// (2) times between erases; "After UICR has been written, the new UICR configuration will only
// take effect after a reset" (which is why the protection is read at a reset, above). Only
// behind allowProvisioning() (task p5-02); the tripwire has the word otherwise.
void Nrf52Chip::writeUicr(std::uint32_t addr, std::uint32_t value) {
  const std::size_t i = (addr - regs::uicr::kBase) / 4;
  const std::uint32_t old = uicr_[i];
  if (config_ != regs::nvmc::kConfigWen) {
    counters_.rejected_writes++;
    record(FlashEvent::Kind::kRejectedWrite, addr, value, old, old);
    fail("a UICR write at " + hex(addr) + " with NVMC.CONFIG.WEN = " + std::to_string(config_) +
         ", not Wen (1): nothing written (PS NVMC \"Writing to user information configuration registers\")");
    return;
  }
  const std::uint32_t result = old & value;
  uicr_[i] = result;
  if (uicr_writes_since_erase_[i] < 255) uicr_writes_since_erase_[i]++;
  if (uicr_writes_since_erase_[i] > regs::nvmc::kWritesPerEraseMax) counters_.uicr_overwrites++;
  counters_.uicr_writes++;
  record(FlashEvent::Kind::kUicrWrite, addr, value, old, result);
  startBusy(write_time_us);
}

// CTRL-AP ERASEALL (PS "CTRL-AP > ERASEALL": "Erase all flash and RAM"; "Access port
// protection": "will erase the flash, UICR, and RAM"; NVMC "Erase all": "This operation will not
// erase the factory information configuration registers (FICR)"). Every word to 0xFF, the write
// counts cleared; ERASEALLSTATUS busy for the set number of reads. Then the kinds part: improved,
// "Disabled via ERASEALL" and so "until" a pin, power or brownout reset - the port opens now;
// legacy, the erased UICR is what the next reset reads (the conservative model, the bench
// tool's too, so that the soft-reset path is the one exercised). Only behind allowProvisioning().
void Nrf52Chip::eraseAll() {
  for (std::size_t i = 0; i < flash_.size(); ++i) {
    flash_[i] = regs::flash::kErased;
    writes_since_erase_[i] = 0;
  }
  for (std::uint32_t& w : uicr_) w = 0xFFFFFFFF;
  for (std::uint8_t& n : uicr_writes_since_erase_) n = 0;
  for (std::uint32_t& w : ram_) w = 0xFFFFFFFF;
  counters_.erase_alls++;
  record(FlashEvent::Kind::kEraseAll, 0, 0, 0, 0);
  erase_all_busy_left_ = erase_all_busy_reads;
  erase_all_busy_for_ever_ = erase_all_never_finishes;
  if (improvedApprotect()) approtect = false;
}

// "After erasing a flash page, all bits in the page are set to 1."
void Nrf52Chip::erasePage(std::uint32_t addr) {
  if (!allowed(addr, kPageBytes)) {
    if (report_outside) flashTally().outside++;
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
  // A word with bits that will not take a write: they stay 0 after the erase too.
  if (stuck_mask != 0 && stuck_addr >= addr && stuck_addr < addr + kPageBytes) flash_[stuck_addr / 4] &= ~stuck_mask;
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
        resets_.sysresetreq++;
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
// NVMC comes up read-only, nothing busy. The protection (task p5-02, PS "Access port
// protection"): a legacy part reads UICR.APPROTECT at every reset, this one included; an
// improved part keeps the port as it is - "disabled until" a pin, power or brownout reset, and
// a soft reset is none of those. And on a module whose firmware opens the port at run time, the
// knob closes it again until that firmware runs (Nordic's APPROTECT note: a soft reset
// re-enables the protection when the firmware's own start-up is what disables it), which is
// what resetAndHalt() reports.
void Nrf52Chip::localReset() {
  counters_.resets++;
  reset_sticky_ = true;
  halted_ = debug_en_ && (demcr_ & regs::scs::kVcCoreReset) != 0;
  demcr_ &= ~0x000F0000u;
  config_ = regs::nvmc::kConfigRen;
  busy_until_us_ = 0;
  busy_for_ever_ = false;
  if (!improvedApprotect()) readProtectionAtReset();
  if (protects_after_reset) approtect = true;
}

}  // namespace sim
