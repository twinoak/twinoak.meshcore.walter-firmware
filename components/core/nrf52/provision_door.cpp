// The one door (task p5-02; spec/SYSTEM.md S-5a) and the grant that opens it. This is one of the
// two files under components/ that may name the forbidden registers (rail R3: this one and
// regs.h); every register access below cites the document it came from:
//
//   PS    Nordic nRF52840 Product Specification, the HTML rendition at docs.nordicsemi.com
//         (bundle ps_nrf52840), read 2026-10-20: "Debug and trace > CTRL-AP", "Debug and trace >
//         Access port protection", "Debug Interface mode", "NVMC" (the electrical specification,
//         "Writing to flash", "Writing to user information configuration registers (UICR)",
//         "Erase all"), "UICR", "FICR".
//   NOTE  Nordic, "Working with the nRF52 Series' improved APPROTECT" (DevZone, 2022), which
//         points at IN-141 (nRF52840 revision 3, build codes Fx0) for the same rules.
#include "core/nrf52/provision_door.h"

#include "core/flasher/board_profile.h"
#include "core/nrf52/provision_grant.h"
#include "core/nrf52/regs.h"

namespace core {
namespace nrf52 {

// ---------------------------------------------------------------------------------- the grant

const char* describe(GrantKind kind) {
  switch (kind) {
    case GrantKind::kNone: return "no grant: not `<DEVICEID>`, `locked` or `locked:<key16>`";
    case GrantKind::kDeviceId: return "a DEVICEID";
    case GrantKind::kLocked: return "locked";
    case GrantKind::kLockedWithKey: return "locked:<key16>";
  }
  return "?";
}

namespace {

// A hex digit's value, or -1. S-5a: the DEVICEID is "read without regard to case".
int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

// Exactly 16 hex digits and then the end of the text: the two words, first the high one.
bool parseHex16(const char* text, std::uint32_t* high, std::uint32_t* low) {
  std::uint32_t words[2] = {0, 0};
  for (std::size_t i = 0; i < kKey16Digits; ++i) {
    const int v = hexValue(text[i]);
    if (v < 0) return false;  // a NUL lands here too: too short
    words[i / 8] = (words[i / 8] << 4) | static_cast<std::uint32_t>(v);
  }
  if (text[kKey16Digits] != 0) return false;  // too long
  *high = words[0];
  *low = words[1];
  return true;
}

bool sameText(const char* a, const char* b) {
  std::size_t i = 0;
  for (; a[i] != 0 && b[i] != 0; ++i) {
    if (a[i] != b[i]) return false;
  }
  return a[i] == 0 && b[i] == 0;
}

bool startsWith(const char* text, const char* prefix, std::size_t* length) {
  std::size_t i = 0;
  for (; prefix[i] != 0; ++i) {
    if (text[i] != prefix[i]) return false;
  }
  *length = i;
  return true;
}

}  // namespace

ProvisionGrant::ProvisionGrant(ProvisionGrant&& other) noexcept
    : kind_(other.kind_), spent_(other.spent_), device_id_{other.device_id_[0], other.device_id_[1]} {
  for (std::size_t i = 0; i <= kKey16Digits; ++i) key16_[i] = other.key16_[i];
  other.spent_ = true;
}

ProvisionGrant& ProvisionGrant::operator=(ProvisionGrant&& other) noexcept {
  if (this != &other) {
    kind_ = other.kind_;
    spent_ = other.spent_;
    device_id_[0] = other.device_id_[0];
    device_id_[1] = other.device_id_[1];
    for (std::size_t i = 0; i <= kKey16Digits; ++i) key16_[i] = other.key16_[i];
    other.spent_ = true;
  }
  return *this;
}

// S-5a's grammar, and nothing else. The DEVICEID is "%08X%08X of DEVICEID[1] then DEVICEID[0]",
// so the first eight digits are word 1.
ProvisionGrant grantFromTypedCommand(const char* text) {
  ProvisionGrant g;
  if (text == nullptr || text[0] == 0) return g;
  if (sameText(text, "locked")) {
    g.kind_ = GrantKind::kLocked;
    return g;
  }
  std::size_t prefix = 0;
  if (startsWith(text, "locked:", &prefix)) {
    std::uint32_t high = 0, low = 0;
    if (!parseHex16(text + prefix, &high, &low)) return g;
    for (std::size_t i = 0; i < kKey16Digits; ++i) g.key16_[i] = upper(text[prefix + i]);
    g.key16_[kKey16Digits] = 0;
    g.kind_ = GrantKind::kLockedWithKey;
    return g;
  }
  std::uint32_t high = 0, low = 0;
  if (!parseHex16(text, &high, &low)) return g;
  g.device_id_[1] = high;
  g.device_id_[0] = low;
  g.kind_ = GrantKind::kDeviceId;
  return g;
}

// ----------------------------------------------------------------------------------- the door

const char* describe(ApprotectKind kind) {
  switch (kind) {
    case ApprotectKind::kLegacy: return "legacy APPROTECT (build codes A-D): UICR.APPROTECT left erased";
    case ApprotectKind::kImproved: return "improved APPROTECT (build code F, revision 3): UICR.APPROTECT = 0x5A";
  }
  return "?";
}

const char* describe(DoorStatus status) {
  switch (status) {
    case DoorStatus::kOk: return "ok";
    case DoorStatus::kSwd: return "the debug port failed";
    case DoorStatus::kNrf52: return "the nRF52 layer refused";
    case DoorStatus::kNotOpen: return "no nRF52 session: Nrf52::open() first";
    case DoorStatus::kDoorClosed: return "the door is not open";
    case DoorStatus::kAlreadyOpened: return "the door was already opened: one grant, one opening";
    case DoorStatus::kGrantSpent: return "the grant was already used";
    case DoorStatus::kGrantMalformed: return "not a grant: type the DEVICEID, `locked` or `locked:<key16>`";
    case DoorStatus::kGrantMismatch: return "the grant does not fit the port: a DEVICEID names an open radio, `locked` a closed one";
    case DoorStatus::kWrongDevice: return "the DEVICEID typed is not this radio's";
    case DoorStatus::kNotNrf52840: return "not an nRF52840 with 4096-byte pages: refused";
    case DoorStatus::kProtected: return "the access port is closed: nothing can be written";
    case DoorStatus::kNotErased: return "refused: no successful chip erase in this opening";
    case DoorStatus::kEraseTimeout: return "ERASEALL did not finish in time";
    case DoorStatus::kNotAligned: return "not word-aligned";
    case DoorStatus::kBadArgument: return "no data";
    case DoorStatus::kOutsideUicr: return "refused: not inside UICR's registers (0x10001000-0x10001307)";
    case DoorStatus::kApprotectAddress: return "refused: UICR.APPROTECT is written by writeApprotect alone";
    case DoorStatus::kApplicationRegion: return "refused: inside the board's application region or its file system";
    case DoorStatus::kOutsideFlash: return "refused: beyond the chip's flash";
    case DoorStatus::kNoProfile: return "refused: no board profile for this slot";
    case DoorStatus::kNvmcTimeout: return "the flash controller did not become ready in time; CONFIG is read-only again";
  }
  return "?";
}

ProvisionDoor::ProvisionDoor(Nrf52& nrf, swd::Dp& dp, interfaces::Clock& clock, ProvisionGrant&& grant)
    : nrf_(nrf), dp_(dp), clock_(clock), grant_(static_cast<ProvisionGrant&&>(grant)) {}

DoorResult ProvisionDoor::refuse(DoorStatus status, std::uint32_t address) {
  counters_.refusals++;
  DoorResult r;
  r.status = status;
  r.address = address;
  return r;
}

DoorResult ProvisionDoor::fromDp(const swd::Result& d) {
  DoorResult r;
  r.value = d.value;
  if (!d.ok()) {
    r.status = DoorStatus::kSwd;
    r.swd = d.status;
  }
  return r;
}

DoorResult ProvisionDoor::fromNrf(const Result& n) {
  DoorResult r;
  r.address = n.address;
  r.value = n.value;
  if (n.ok()) return r;
  switch (n.status) {
    case Status::kSwd:
      r.status = DoorStatus::kSwd;
      r.swd = n.swd;
      break;
    case Status::kProtected: r.status = DoorStatus::kProtected; break;
    case Status::kNotNrf52840: r.status = DoorStatus::kNotNrf52840; break;
    case Status::kNvmcTimeout: r.status = DoorStatus::kNvmcTimeout; break;
    case Status::kNotOpen: r.status = DoorStatus::kNotOpen; break;
    default:
      r.status = DoorStatus::kNrf52;
      r.nrf = n.status;
      break;
  }
  return r;
}

// PS "CTRL-AP > APPROTECTSTATUS" (offset 0x00C): "Status register for access port protection",
// bit 0 Enabled (0) or Disabled (1). AP 1, readable on a closed part - "A custom access port that
// enables control of the device when other access ports in the DAP are disabled by the access
// port protection" - once the debugger has requested power ("Debug Interface mode"), which
// Nrf52::open() did.
DoorResult ProvisionDoor::readPort(bool* port_open) {
  const DoorResult r = fromDp(dp_.readAp(ctrl_ap::kApsel, ctrl_ap::kApprotectStatus));
  if (!r.ok()) return r;
  *port_open = (r.value & ctrl_ap::kApprotectStatusOpen) != 0;
  return r;
}

// The grant against the chip, then consumed. The pairing is S-5a's: an open port is named by
// its DEVICEID, a closed one by `locked` (with or without the key the sequence decides on). The
// DEVICEID is FICR's as identify() read it, compared word for word.
DoorResult ProvisionDoor::open() {
  if (opened_) return refuse(DoorStatus::kAlreadyOpened);
  if (grant_.spent()) return refuse(DoorStatus::kGrantSpent);
  grant_.spend();  // one grant, one attempt at opening: whatever comes of it
  if (!grant_.valid()) return refuse(DoorStatus::kGrantMalformed);
  if (!nrf_.isOpen()) return refuse(DoorStatus::kNotOpen);
  bool port_open = false;
  DoorResult r = readPort(&port_open);
  if (!r.ok()) return r;
  r.port_open = port_open;
  DoorStatus why = DoorStatus::kOk;
  if (port_open) {
    if (grant_.kind() != GrantKind::kDeviceId) {
      why = DoorStatus::kGrantMismatch;
    } else if (!nrf_.identified()) {
      why = DoorStatus::kNotNrf52840;
    } else {
      const Identity& id = nrf_.identity();
      if (id.device_id[0] != grant_.deviceId(0) || id.device_id[1] != grant_.deviceId(1)) {
        why = DoorStatus::kWrongDevice;
      }
    }
  } else if (grant_.kind() == GrantKind::kDeviceId) {
    why = DoorStatus::kGrantMismatch;
  }
  if (why != DoorStatus::kOk) {
    DoorResult refused = refuse(why);
    refused.port_open = port_open;  // the refusal says what the port read, for the console's line
    return refused;
  }
  opened_ = true;
  erased_ = false;
  return r;
}

DoorResult ProvisionDoor::approtectStatus(bool* port_open) {
  if (port_open == nullptr) return refuse(DoorStatus::kBadArgument);
  *port_open = false;
  if (!opened_) return refuse(DoorStatus::kDoorClosed);
  DoorResult r = readPort(port_open);
  r.port_open = *port_open;
  return r;
}

// Nrf52::open() again on the same slot: connect (line reset, DPIDR), power-up, the control
// port's IDR and APPROTECTSTATUS, and - when the port is open - the memory port and FICR. A
// closed port is not a failure here: the result says port_open = false and the sequence decides
// (S-5a: one soft reset, then stop).
DoorResult ProvisionDoor::reopen() {
  counters_.reconnects++;
  const Result n = nrf_.open(nrf_.slot());
  DoorResult r = fromNrf(n);
  if (n.status == Status::kProtected) {
    r.status = DoorStatus::kOk;
    r.port_open = false;
    return r;
  }
  if (!r.ok()) return r;
  r.port_open = nrf_.protection() == Protection::kOpen;
  return r;
}

// PS "CTRL-AP > ERASEALL" (offset 0x004): "Erase all flash and RAM", write Erase (1);
// "ERASEALLSTATUS" (offset 0x008): "Status register for the ERASEALL operation", Ready (0) or
// Busy (1). PS "Access port protection": on a part with the hardware-and-software protection
// (build codes Fxx and later) the protection is "Disabled via ERASEALL" and stays so "until" a
// pin, power or brownout reset; on a hardware-only part (Dxx and earlier) it is "Disabled by
// issuing an ERASEALL command via CTRL-AP" as well, the erased UICR taking effect at the next
// reset (NVMC: "the new UICR configuration will only take effect after a reset") - softReset()
// is that reset. Then the session is opened again, so that the port's state is read from the
// chip and not assumed, and identify() has run if it could.
DoorResult ProvisionDoor::eraseAll(std::uint32_t timeout_ms) {
  if (!opened_) return refuse(DoorStatus::kDoorClosed);
  counters_.erase_alls++;
  DoorResult r = fromDp(dp_.writeAp(ctrl_ap::kApsel, ctrl_ap::kEraseAll, 1));
  if (!r.ok()) return r;
  bool ready = false;
  for (std::uint32_t elapsed = 0; elapsed <= timeout_ms; elapsed += kChipErasePollMs) {
    if (elapsed > 0) clock_.delayMs(kChipErasePollMs);
    r = fromDp(dp_.readAp(ctrl_ap::kApsel, ctrl_ap::kEraseAllStatus));
    if (!r.ok()) return r;
    if ((r.value & 1u) == 0) {
      ready = true;
      break;
    }
  }
  if (!ready) {
    DoorResult t;
    t.status = DoorStatus::kEraseTimeout;
    return t;
  }
  erased_ = true;
  return reopen();
}

// PS "CTRL-AP > RESET" (offset 0x000): "Soft reset triggered through CTRL-AP", Reset (1) "Device
// is held in reset", NoReset (0). Held for kSoftResetHoldMs, released, given kSoftResetSettleMs.
// Why a reconnect afterwards: the reset takes the part through its reset sequence, and whether
// the debug power-up request the Product Specification asks for ("Debug Interface mode") and
// the DP's own state survive it is the part's business, not ours - a line reset and DPIDR are
// always allowed (ADIv5 B4.3.3) and cost a millisecond, so the door reconnects every time and is
// right for a part that needed it and for one that did not (both are tested). The session is
// opened again with it: on a legacy part this is where the erased UICR is read and the port
// opens; on an improved part the port is as the erase left it.
DoorResult ProvisionDoor::softReset() {
  if (!opened_) return refuse(DoorStatus::kDoorClosed);
  DoorResult r = fromDp(dp_.writeAp(ctrl_ap::kApsel, ctrl_ap::kReset, 1));
  if (!r.ok()) return r;
  clock_.delayMs(kSoftResetHoldMs);
  r = fromDp(dp_.writeAp(ctrl_ap::kApsel, ctrl_ap::kReset, 0));
  if (!r.ok()) return r;
  counters_.soft_resets++;
  clock_.delayMs(kSoftResetSettleMs);
  return reopen();
}

// Every write: the door open, the chip erased in this opening, the port open and the chip
// identified (the nRF52 layer's own gate would say the same, after the door's guards).
DoorResult ProvisionDoor::gateWrite() {
  if (!opened_) return refuse(DoorStatus::kDoorClosed);
  if (!erased_) return refuse(DoorStatus::kNotErased);
  if (!nrf_.isOpen()) return refuse(DoorStatus::kNotOpen);
  if (nrf_.protection() != Protection::kOpen) return refuse(DoorStatus::kProtected);
  if (!nrf_.identified()) return refuse(DoorStatus::kNotNrf52840);
  return DoorResult{};
}

// CONFIG back to "Read only access" on every path out of a UICR write; the earlier failure is
// the one reported (as Nrf52::restoreConfig does).
DoorResult ProvisionDoor::restoreConfig(DoorResult first) {
  const swd::MemResult m = nrf_.memAp().write32(nvmc::kConfig, nvmc::kConfigRen);
  if (!first.ok()) return first;
  if (!m.ok()) {
    DoorResult f;
    f.status = DoorStatus::kSwd;
    f.swd = m.status;
    f.address = nvmc::kConfig;
    return f;
  }
  return first;
}

// PS NVMC "Writing to user information configuration registers (UICR)": "written in the same
// way as flash" - CONFIG.WEN = Wen ("Write enabled"), full 32-bit words at word-aligned
// addresses, READY ("Ready = 1") awaited after each, CONFIG back to Ren. A word can be written
// nWRITE (2) times between erases; the base writes each once, after ERASEALL.
DoorResult ProvisionDoor::writeUicrWords(std::uint32_t addr, const std::uint32_t* words, std::size_t n) {
  swd::MemAp& mem = nrf_.memAp();
  swd::MemResult m = mem.write32(nvmc::kConfig, nvmc::kConfigWen);
  if (!m.ok()) {
    DoorResult f;
    f.status = DoorStatus::kSwd;
    f.swd = m.status;
    f.address = nvmc::kConfig;
    return restoreConfig(f);
  }
  DoorResult r;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t a = addr + 4u * static_cast<std::uint32_t>(i);
    m = mem.write32(a, words[i]);
    if (!m.ok()) {
      r.status = DoorStatus::kSwd;
      r.swd = m.status;
      r.address = a;
      return restoreConfig(r);
    }
    r = fromNrf(nrf_.waitFlashReady(kWriteTimeoutMs));
    if (!r.ok()) {
      r.address = a;
      return restoreConfig(r);
    }
    counters_.uicr_words++;
  }
  r.address = addr;
  r.value = static_cast<std::uint32_t>(n);
  return restoreConfig(r);
}

// Inside UICR's registers only (PS "UICR": 0x10001000, REGOUT0 the last at +0x304), never
// APPROTECT (+0x208: writeApprotect's alone), word-aligned, something to write - all before any
// access. The end is computed in 64 bits so that a count cannot wrap the address.
DoorResult ProvisionDoor::writeUicr(std::uint32_t addr, const std::uint32_t* words, std::size_t n) {
  DoorResult g = gateWrite();
  if (!g.ok()) return g;
  if (words == nullptr || n == 0) return refuse(DoorStatus::kBadArgument, addr);
  if ((addr % 4) != 0) return refuse(DoorStatus::kNotAligned, addr);
  const std::uint64_t end = static_cast<std::uint64_t>(addr) + static_cast<std::uint64_t>(n) * 4u;
  if (addr < uicr::kBase || end > uicr::kEnd) return refuse(DoorStatus::kOutsideUicr, addr);
  if (addr <= uicr::kApprotect && end > uicr::kApprotect) return refuse(DoorStatus::kApprotectAddress, addr);
  return writeUicrWords(addr, words, n);
}

// PS "UICR > APPROTECT", PALL: HwDisabled (0x5A) - "Hardware disable of access port protection
// for devices where access port protection is controlled by hardware and software" (NOTE: the
// value a revision 3 part must hold for the port to be open after a reset, with the firmware
// writing APPROTECT.DISABLE); on a hardware-only part the word is left erased (Disabled, 0xFF),
// since any other value there enables the protection at the next reset. The kind is the variant
// table's (task p5-03); the door never takes a value.
DoorResult ProvisionDoor::writeApprotect(ApprotectKind kind) {
  DoorResult g = gateWrite();
  if (!g.ok()) return g;
  if (kind == ApprotectKind::kLegacy) {
    g.address = uicr::kApprotect;
    return g;
  }
  const std::uint32_t hw_disabled = uicr::kApprotectHwDisabled;  // 0x0000005A, the upper bytes cleared
  return writeUicrWords(uicr::kApprotect, &hw_disabled, 1);
}

// The base's flash: everything the board table does not give the application or the file
// system - below the application region (the MBR, the SoftDevice) and from the bootloader's
// start up (the bootloader, the MBR parameter page, the settings page). The numbers are the
// profile's (core/flasher/board_profile.h: app 0x26000-0xED000, the file system from 0xED000 to
// the bootloader at 0xF4000), never a caller's; a run that touches the forbidden span is refused
// whole, before any access. The write itself is the nRF52 layer's guarded one, with the region
// the run lies in.
DoorResult ProvisionDoor::writeFlash(std::uint32_t addr, const std::uint32_t* words, std::size_t n) {
  DoorResult g = gateWrite();
  if (!g.ok()) return g;
  if (words == nullptr || n == 0) return refuse(DoorStatus::kBadArgument, addr);
  if ((addr % 4) != 0) return refuse(DoorStatus::kNotAligned, addr);
  const flasher::BoardProfile* profile = flasher::profileForSlot(nrf_.slot());
  if (profile == nullptr) return refuse(DoorStatus::kNoProfile, addr);
  const std::uint64_t end = static_cast<std::uint64_t>(addr) + static_cast<std::uint64_t>(n) * 4u;
  const std::uint32_t flash_end = nrf_.identity().flash_bytes;
  if (end > flash_end) return refuse(DoorStatus::kOutsideFlash, addr);
  const std::uint32_t forbidden_begin = profile->app.begin;        // the application region ...
  const std::uint32_t forbidden_end = profile->bootloader_start;   // ... and the file system after it
  if (addr < forbidden_end && end > forbidden_begin) return refuse(DoorStatus::kApplicationRegion, addr);
  FlashRegion region;
  if (addr < forbidden_begin) {
    region.begin = 0;
    region.end = forbidden_begin;
  } else {
    region.begin = forbidden_end;
    region.end = flash_end;
  }
  const DoorResult r = fromNrf(nrf_.writeWords(region, addr, words, n));
  if (r.ok()) counters_.flash_words += static_cast<std::uint32_t>(n);
  return r;
}

// The three words task p5-03's step 4 looks at after the erase: flash word 0 (the MBR's stack
// pointer once the base is in), UICR.NRFFW[0] ("Reserved for Nordic firmware design": the
// bootloader's address) and UICR.APPROTECT - all 0xFFFFFFFF on an erased part (PS UICR: "default
// to 0xFFFFFFFF"; flash: "all bits ... set to 1").
DoorResult ProvisionDoor::blankCheck(BlankCheck* out) {
  if (out == nullptr) return refuse(DoorStatus::kBadArgument);
  *out = BlankCheck{};
  if (!opened_) return refuse(DoorStatus::kDoorClosed);
  if (!nrf_.isOpen()) return refuse(DoorStatus::kNotOpen);
  if (nrf_.protection() != Protection::kOpen) return refuse(DoorStatus::kProtected);
  if (!nrf_.identified()) return refuse(DoorStatus::kNotNrf52840);
  DoorResult r = fromNrf(nrf_.read(flash::kBase, &out->flash0, 1));
  if (!r.ok()) return r;
  r = fromNrf(nrf_.read(uicr::kNrfFw0, &out->nrffw0, 1));
  if (!r.ok()) return r;
  r = fromNrf(nrf_.read(uicr::kApprotect, &out->approtect, 1));
  if (!r.ok()) return r;
  out->blank = out->flash0 == flash::kErased && out->nrffw0 == flash::kErased && out->approtect == flash::kErased;
  r.port_open = true;
  return r;
}

}  // namespace nrf52
}  // namespace core
