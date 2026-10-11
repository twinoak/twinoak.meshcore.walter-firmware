#include "core/nrf52/nrf52.h"

namespace core {
namespace nrf52 {

const char* describe(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kSwd: return "the debug port failed";
    case Status::kNotOpen: return "not connected: open() first";
    case Status::kProtected: return "protected: access-port protection is on, memory access not attempted";
    case Status::kNoControlPort: return "no Nordic control access port at AP 1";
    case Status::kNoMemoryPort: return "no memory access port at AP 0";
    case Status::kNotNrf52840: return "not an nRF52840 with 4096-byte pages: refused";
    case Status::kBadRegion: return "the region is not page-aligned, is empty, or is not inside the chip's flash";
    case Status::kOutsideRegion: return "outside the region: refused before any flash controller access";
    case Status::kNotAligned: return "not aligned: a page address to the page, a word address to the word";
    case Status::kBadArgument: return "no data";
    case Status::kNvmcTimeout: return "the flash controller did not become ready in time; CONFIG is read-only again";
    case Status::kCoreTimeout: return "the core did not reach the state asked for in time";
    case Status::kSlotBusy: return "refused: no such slot, or another slot is in the middle of something";
  }
  return "?";
}

const char* describe(Protection protection) {
  switch (protection) {
    case Protection::kUnknown: return "unknown";
    case Protection::kOpen: return "open";
    case Protection::kProtected: return "PROTECTED";
  }
  return "?";
}

const char* describe(CoreState state) {
  switch (state) {
    case CoreState::kUnknown: return "unknown";
    case CoreState::kRunning: return "running";
    case CoreState::kHalted: return "halted";
    case CoreState::kSleeping: return "sleeping";
    case CoreState::kLockedUp: return "locked up";
  }
  return "?";
}

namespace {

Result refused(Status status, std::uint32_t address = 0) {
  Result r;
  r.status = status;
  r.address = address;
  return r;
}

}  // namespace

Nrf52::Nrf52(swd::Dp& dp, interfaces::Clock& clock, board::SlotLock* slots)
    : dp_(dp), clock_(clock), slots_(slots), mem_ap_(dp, 0) {}

Result Nrf52::fromMem(const swd::MemResult& m) {
  Result r;
  r.address = m.address;
  r.value = m.value;
  if (!m.ok()) {
    r.status = Status::kSwd;
    r.swd = m.status;
  }
  return r;
}

Result Nrf52::fromDp(const swd::Result& d) {
  Result r;
  r.value = d.value;
  if (!d.ok()) {
    r.status = Status::kSwd;
    r.swd = d.status;
  }
  return r;
}

// PS "Debug and trace > CTRL-AP > APPROTECTSTATUS": bit 0 reads 0 for "Access port protection
// enabled", 1 for "not enabled".
Result Nrf52::readProtection() {
  const Result r = fromDp(dp_.readAp(ctrl_ap::kApsel, ctrl_ap::kApprotectStatus));
  if (!r.ok()) return r;
  protection_ = (r.value & ctrl_ap::kApprotectStatusOpen) != 0 ? Protection::kOpen : Protection::kProtected;
  return r;
}

// PS "Debug and trace > Debug Interface mode": "Before an external debugger can access either
// CPU's access port (AHB-AP) or the control access port (CTRL-AP), the debugger must first
// request the device to power up via CxxxPWRUPREQ". Then the control access port first: its IDR
// (reset value 0x02880000, Nordic's), and whether the access port protection is on - "blocks the
// debugger from read and write access to all CPU registers and memory-mapped addresses" - before
// a single memory access is attempted. Only on an open chip is the memory access port opened and
// FICR read.
Result Nrf52::open(board::Slot slot) {
  open_ = false;
  protection_ = Protection::kUnknown;
  identity_ = Identity{};
  ctrl_ap_idr_ = 0;
  // The slot before the port (task p1-06): the debug lines are steered to that radio and held
  // until close(), so nothing can point them at the other one mid-session. A caller that holds
  // the slot already (the flasher) passes no lock, and the hold it took stands for this too.
  if (slots_ != nullptr && !slot_held_) {
    const board::SelectResult selected = slots_->take(slot);
    if (selected != board::SelectResult::kOk) return refused(Status::kSlotBusy);
    slot_held_ = true;
  }
  slot_ = slot;
  Result r = fromDp(dp_.connect());
  if (!r.ok()) return r;
  r = fromDp(dp_.powerUp());
  if (!r.ok()) return r;
  r = fromDp(dp_.readAp(ctrl_ap::kApsel, ctrl_ap::kIdr));
  if (!r.ok()) return r;
  ctrl_ap_idr_ = r.value;
  const swd::ApIdrFields f = swd::decodeApIdr(ctrl_ap_idr_);
  if (ctrl_ap_idr_ == 0 || f.designer != ctrl_ap::kDesignerNordic || !f.reserved_clear) {
    return refused(Status::kNoControlPort);
  }
  r = readProtection();
  if (!r.ok()) return r;
  open_ = true;
  if (protection_ == Protection::kProtected) return refused(Status::kProtected);
  const swd::MemResult m = mem_ap_.open();
  if (!m.ok()) {
    if (m.status == swd::Status::kNoMemAp) return refused(Status::kNoMemoryPort);
    return fromMem(m);
  }
  return identify();
}

// The power-up request withdrawn: "Once the debugger stops requesting power via CxxxPWRUPREQ,
// the device is back in normal mode" (PS "Debug Interface mode"). Nothing else: the core is left
// as the caller left it.
Result Nrf52::close() {
  open_ = false;
  const Result r = fromDp(dp_.powerDown());
  if (slots_ != nullptr && slot_held_) {
    slots_->release();
    slot_held_ = false;
  }
  return r;
}

// PS "FICR": CODEPAGESIZE "Code memory page size", CODESIZE "Code memory size in number of
// pages", DEVICEID[0..1], INFO.PART "Part code" (0x52840), INFO.VARIANT "Build code ... encoded
// in ASCII", INFO.FLASH "Flash variant" in KB. The chip this firmware is written for has 256
// pages of 4096 bytes (PS "Memory"); anything else is reported and refused.
Result Nrf52::identify() {
  identity_ = Identity{};
  if (!open_) return refused(Status::kNotOpen);
  if (protection_ != Protection::kOpen) return refused(Status::kProtected);
  if (!mem_ap_.opened()) return refused(Status::kNoMemoryPort);

  std::uint32_t code[2] = {};  // CODEPAGESIZE, CODESIZE
  Result r = fromMem(mem_ap_.readBlock(ficr::kCodePageSize, code, 2));
  if (!r.ok()) return r;
  std::uint32_t id[2] = {};
  r = fromMem(mem_ap_.readBlock(ficr::kDeviceId0, id, 2));
  if (!r.ok()) return r;
  std::uint32_t info[5] = {};  // PART, VARIANT, PACKAGE, RAM, FLASH
  r = fromMem(mem_ap_.readBlock(ficr::kInfoPart, info, 5));
  if (!r.ok()) return r;

  identity_.page_size = code[0];
  identity_.pages = code[1];
  identity_.device_id[0] = id[0];
  identity_.device_id[1] = id[1];
  identity_.part = info[0];
  identity_.variant_raw = info[1];
  identity_.info_flash_kb = info[4];
  for (unsigned i = 0; i < 4; ++i) {
    const std::uint32_t c = (info[1] >> (24 - 8 * i)) & 0xFF;  // first character in bits [31:24]
    identity_.variant[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  identity_.variant[4] = 0;
  const bool pages_sane = identity_.pages > 0 && identity_.pages <= kMaxPages;
  identity_.flash_bytes = pages_sane ? identity_.pages * identity_.page_size : 0;
  identity_.is_nrf52840 =
      identity_.part == ficr::kPartNrf52840 && identity_.page_size == flash::kPageSize && pages_sane;
  if (!identity_.is_nrf52840) return refused(Status::kNotNrf52840);
  r.value = identity_.part;
  return r;
}

// Every operation below identify(): the chip is open, not protected, and the one this firmware
// knows.
Result Nrf52::gate() {
  if (!open_) return refused(Status::kNotOpen);
  if (protection_ != Protection::kOpen) return refused(Status::kProtected);
  if (!identity_.is_nrf52840) return refused(Status::kNotNrf52840);
  return Result{};
}

// ---------------------------------------------------------------------------------- the core

// DHCSR read until (value & mask) == wanted, a millisecond apart, bounded. While a reset is in
// progress the access port can answer FAULT (the bus is in reset): a FAULT is polled through -
// the debug port has already cleared the sticky flag - and the bound still holds; any other
// failure returns at once.
Result Nrf52::pollDhcsr(std::uint32_t mask, std::uint32_t wanted, std::uint32_t timeout_ms) {
  Result r;
  for (std::uint32_t poll = 0; poll <= timeout_ms; ++poll) {
    if (poll > 0) clock_.delayMs(1);
    r = fromMem(mem_ap_.read32(scs::kDhcsr));
    if (!r.ok()) {
      if (r.swd == swd::Status::kFault) continue;
      return r;
    }
    if ((r.value & mask) == wanted) return r;
  }
  if (!r.ok()) return r;
  return refused(Status::kCoreTimeout, scs::kDhcsr);
}

// ARM C1.6.2: the debug key in the top half enables the write; C_DEBUGEN enables halting debug,
// C_HALT halts; S_HALT reads 1 "In Debug state".
Result Nrf52::halt() {
  Result r = gate();
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.write32(scs::kDhcsr, scs::kDbgKey | scs::kCDebugEn | scs::kCHalt));
  if (!r.ok()) return r;
  return pollDhcsr(scs::kSHalt, scs::kSHalt, kHaltTimeoutMs);
}

// ARM C1.4.1 "Entering Debug state on leaving reset state": "a debugger sets DHCSR.C_DEBUGEN to 1,
// to enable Halting debug, and sets DEMCR.VC_CORERESET to 1 to enable vector catch on the Reset
// exception. When the processor comes out of reset it sets DHCSR.C_HALT to 1, and enters Debug
// state." The reset is AIRCR.SYSRESETREQ (B3.2.6: "asserts a signal to the external system to
// request a Local reset"), with the key. The core is halted first so that nothing runs between
// the write and the reset; S_RESET_ST ("at least one reset since last DHCSR read", C1.6.2) and
// S_HALT together say the reset happened and was caught. VC_CORERESET is cleared again so a later
// reset runs free. Then the protection is read again: on a build code Fxx chip whose firmware
// opens the port at run time (regs.h, APPROTECT HwDisabled and APPROTECT.DISABLE) a reset closes
// it until that firmware runs, which a halted-at-reset core never does.
Result Nrf52::resetAndHalt() {
  Result r = gate();
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.write32(scs::kDhcsr, scs::kDbgKey | scs::kCDebugEn | scs::kCHalt));
  if (!r.ok()) return r;
  r = pollDhcsr(scs::kSHalt, scs::kSHalt, kHaltTimeoutMs);
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.read32(scs::kDemcr));
  if (!r.ok()) return r;
  const std::uint32_t demcr = r.value;
  r = fromMem(mem_ap_.write32(scs::kDemcr, demcr | scs::kVcCoreReset));
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.write32(scs::kAircr, scs::kVectKey | scs::kSysResetReq));
  if (!r.ok()) return r;
  r = pollDhcsr(scs::kSResetSt | scs::kSHalt, scs::kSResetSt | scs::kSHalt, kResetTimeoutMs);
  if (!r.ok()) {
    // The reset may have closed the port (above): say that rather than "timeout".
    const Result p = readProtection();
    if (p.ok() && protection_ == Protection::kProtected) return refused(Status::kProtected);
    return r;
  }
  // DEMCR as the reset left it (a Local reset clears only its DebugMonitor bits, C1.6.5), less
  // the vector catch.
  r = fromMem(mem_ap_.read32(scs::kDemcr));
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.write32(scs::kDemcr, r.value & ~scs::kVcCoreReset));
  if (!r.ok()) return r;
  r = readProtection();
  if (!r.ok()) return r;
  if (protection_ == Protection::kProtected) return refused(Status::kProtected);
  return r;
}

// ARM C1.5.4 "Exiting Debug state": "when the debugger writes 0 to DHCSR.C_HALT". Then halting
// debug is disabled altogether (C_DEBUGEN 0: "the processor ignores the values of all other
// bits in this register") and the reset vector catch is off, so the radio runs as if no debugger
// had been there.
Result Nrf52::run() {
  Result r = gate();
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.read32(scs::kDemcr));
  if (!r.ok()) return r;
  if ((r.value & scs::kVcCoreReset) != 0) {
    r = fromMem(mem_ap_.write32(scs::kDemcr, r.value & ~scs::kVcCoreReset));
    if (!r.ok()) return r;
  }
  r = fromMem(mem_ap_.write32(scs::kDhcsr, scs::kDbgKey | scs::kCDebugEn));
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.write32(scs::kDhcsr, scs::kDbgKey));
  if (!r.ok()) return r;
  return pollDhcsr(scs::kSHalt, 0, kHaltTimeoutMs);
}

// A reset that runs: vector catch off, halting debug off, then SYSRESETREQ. S_RESET_ST says it
// happened; the protection is read again (resetAndHalt says why).
Result Nrf52::systemReset() {
  Result r = gate();
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.read32(scs::kDemcr));
  if (!r.ok()) return r;
  if ((r.value & scs::kVcCoreReset) != 0) {
    r = fromMem(mem_ap_.write32(scs::kDemcr, r.value & ~scs::kVcCoreReset));
    if (!r.ok()) return r;
  }
  r = fromMem(mem_ap_.write32(scs::kDhcsr, scs::kDbgKey));
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.write32(scs::kAircr, scs::kVectKey | scs::kSysResetReq));
  if (!r.ok()) return r;
  r = pollDhcsr(scs::kSResetSt, scs::kSResetSt, kResetTimeoutMs);
  if (!r.ok()) {
    const Result p = readProtection();
    if (p.ok() && protection_ == Protection::kProtected) return refused(Status::kProtected);
    return r;
  }
  r = readProtection();
  if (!r.ok()) return r;
  if (protection_ == Protection::kProtected) return refused(Status::kProtected);
  return r;
}

// ARM C1.6.2: S_LOCKUP "running but locked up", S_HALT "In Debug state", S_SLEEP "Sleeping".
Result Nrf52::coreState(CoreState* out) {
  if (out == nullptr) return refused(Status::kBadArgument);
  *out = CoreState::kUnknown;
  Result r = gate();
  if (!r.ok()) return r;
  r = fromMem(mem_ap_.read32(scs::kDhcsr));
  if (!r.ok()) return r;
  if (r.value & scs::kSLockup) {
    *out = CoreState::kLockedUp;
  } else if (r.value & scs::kSHalt) {
    *out = CoreState::kHalted;
  } else if (r.value & scs::kSSleep) {
    *out = CoreState::kSleeping;
  } else {
    *out = CoreState::kRunning;
  }
  return r;
}

// --------------------------------------------------------------------------------- the flash

// A region is [begin, end): page-aligned on both ends, not empty, and inside the flash the chip
// said it has - so no region can name FICR, UICR or a peripheral, whatever its maker meant.
Result Nrf52::checkRegion(const FlashRegion& region) {
  const std::uint32_t page = identity_.page_size;
  if (page == 0 || region.begin >= region.end || (region.begin % page) != 0 || (region.end % page) != 0 ||
      region.end > identity_.flash_bytes) {
    counters_.refusals++;
    return refused(Status::kBadRegion, region.begin);
  }
  return Result{};
}

Result Nrf52::waitFlashReady(std::uint32_t timeout_ms) {
  const Result g = gate();
  if (!g.ok()) return g;
  return waitReady(timeout_ms);
}

// PS "NVMC > READY": "Ready = 1 (NVMC is ready). Busy = 0 (NVMC is busy [on-going write or
// erase])". One read a millisecond, bounded by the caller's timeout.
Result Nrf52::waitReady(std::uint32_t timeout_ms) {
  Result r;
  for (std::uint32_t poll = 0; poll <= timeout_ms; ++poll) {
    if (poll > 0) clock_.delayMs(1);
    counters_.ready_polls++;
    r = fromMem(mem_ap_.read32(nvmc::kReady));
    if (!r.ok()) return r;
    if ((r.value & nvmc::kReadyBit) != 0) return r;
  }
  return refused(Status::kNvmcTimeout, nvmc::kReady);
}

Result Nrf52::setConfig(std::uint32_t wen) {
  counters_.config_writes++;
  return fromMem(mem_ap_.write32(nvmc::kConfig, wen & nvmc::kConfigMask));
}

// CONFIG back to "Read only access" - on every path out of an erase or a write. The earlier
// failure is the one reported; a failure to restore is reported when nothing else went wrong.
Result Nrf52::restoreConfig(Result first) {
  const Result restored = setConfig(nvmc::kConfigRen);
  if (!first.ok() || !restored.ok()) return first.ok() ? restored : first;
  return first;
}

// PS "NVMC > Erasing a page in flash": "When erase is enabled, the flash memory can be erased
// page by page using the ERASEPAGE" - CONFIG.WEN = Een, the page's address written to ERASEPAGE,
// READY awaited, CONFIG back to Ren. "After erasing a flash page, all bits in the page are set
// to 1." The guards come first, and a refusal touches nothing.
Result Nrf52::erasePage(const FlashRegion& region, std::uint32_t page_addr) {
  Result r = gate();
  if (!r.ok()) return r;
  r = checkRegion(region);
  if (!r.ok()) return r;
  const std::uint32_t page = identity_.page_size;
  if ((page_addr % page) != 0) {
    counters_.refusals++;
    return refused(Status::kNotAligned, page_addr);
  }
  if (page_addr < region.begin || page_addr + page > region.end) {
    counters_.refusals++;
    return refused(Status::kOutsideRegion, page_addr);
  }
  r = setConfig(nvmc::kConfigEen);
  if (!r.ok()) return restoreConfig(r);
  r = fromMem(mem_ap_.write32(nvmc::kErasePage, page_addr));
  if (!r.ok()) return restoreConfig(r);
  r = waitReady(kErasePageTimeoutMs);
  if (r.ok()) {
    counters_.erases++;
    r.address = page_addr;
  }
  return restoreConfig(r);
}

// PS "NVMC > Writing to flash": "When write is enabled, full 32-bit words can be written to
// word-aligned addresses in flash memory" - CONFIG.WEN = Wen, the words, READY awaited, CONFIG
// back to Ren. "The NVMC is only able to write 0 to bits in flash memory that are erased (set to
// 1). It cannot rewrite a bit back to 1": what lands is the AND of old and new, which is the
// flasher's to erase first and verify after. The guards come first, with the end of the data
// checked in 64 bits so that a count cannot wrap the address.
Result Nrf52::writeWords(const FlashRegion& region, std::uint32_t addr, const std::uint32_t* data,
                         std::size_t words) {
  Result r = gate();
  if (!r.ok()) return r;
  r = checkRegion(region);
  if (!r.ok()) return r;
  if (data == nullptr || words == 0) {
    counters_.refusals++;
    return refused(Status::kBadArgument, addr);
  }
  if ((addr % 4) != 0) {
    counters_.refusals++;
    return refused(Status::kNotAligned, addr);
  }
  const std::uint64_t end = static_cast<std::uint64_t>(addr) + static_cast<std::uint64_t>(words) * 4u;
  if (addr < region.begin || end > region.end) {
    counters_.refusals++;
    return refused(Status::kOutsideRegion, addr);
  }
  r = setConfig(nvmc::kConfigWen);
  if (!r.ok()) return restoreConfig(r);
  r = fromMem(mem_ap_.writeBlock(addr, data, words));
  if (!r.ok()) return restoreConfig(r);
  r = waitReady(kWriteTimeoutMs);
  if (r.ok()) {
    counters_.words_written += static_cast<std::uint32_t>(words);
    r.address = addr;
  }
  return restoreConfig(r);
}

Result Nrf52::read(std::uint32_t addr, std::uint32_t* out, std::size_t words) {
  Result r = gate();
  if (!r.ok()) return r;
  if (out == nullptr || words == 0) return refused(Status::kBadArgument, addr);
  if ((addr % 4) != 0) return refused(Status::kNotAligned, addr);
  return fromMem(mem_ap_.readBlock(addr, out, words));
}

}  // namespace nrf52
}  // namespace core
