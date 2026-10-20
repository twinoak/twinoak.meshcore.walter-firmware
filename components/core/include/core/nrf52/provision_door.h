// The one door (task p5-02; spec/SYSTEM.md S-5a). Until this task the firmware could not name a
// register that erases a chip, and S-5 still says nothing automatic ever does. S-5a makes one
// exception - a person's typed `rak provision <radio> confirm` - and this is the door that
// exception goes through: the CTRL-AP's ERASEALL and soft reset, the UICR, and the base's flash
// outside the application's region, each behind a grant that only grantFromTypedCommand() can
// make (provision_grant.h) and that the door checks against the chip itself. Everything else of
// the house is as sealed as it was: rail R3 lets the forbidden registers' names into exactly two
// files, regs.h and provision_door.cpp; rail R3b lets this header be included by
// components/core/provision/ (the sequence, task p5-03) and nothing else; and every test of the
// tree that is not about the door runs against a simulated chip whose tripwire fires at the
// first of any of this.
//
// A door is made over an nRF52 session that Nrf52::open() has started - on a closed chip that
// session stops at kProtected with the debug port connected and powered, which is all the
// CTRL-AP needs (it is AP 1; nothing of AP 0 is touched until the port reads open). Then, on it
// only:
//
//   open()              the grant against the chip: APPROTECTSTATUS read; an open port needs a
//                       DEVICEID grant equal to FICR's, a closed port a `locked` or
//                       `locked:<key16>` grant; any other pairing, a malformed or a spent grant,
//                       refuses before a register is written. The grant is consumed either way.
//   approtectStatus()   CTRL-AP APPROTECTSTATUS, as a bool.
//   eraseAll(timeout)   CTRL-AP ERASEALL = 1, ERASEALLSTATUS polled until Ready or the timeout
//                       (tERASEALL is 169 ms typical; kChipEraseTimeoutMs below says why 5 s);
//                       then the session opened again - on an improved part the port is open now
//                       and identify() runs; on a legacy part it reads closed until a reset, and
//                       the result says so (port_open).
//   softReset()         CTRL-AP RESET 1, then 0, then the debug port reconnected (line reset,
//                       DPIDR, power-up) and the session opened again: S-5a's one allowed reset
//                       between the erase and the last UICR write.
//   writeUicr()         words inside UICR's registers (0x10001000-0x10001307) only, never
//                       UICR.APPROTECT; NVMC CONFIG = Wen, each word, READY after each, CONFIG =
//                       Ren whatever happened (the earlier failure wins).
//   writeApprotect(k)   UICR.APPROTECT = 0x0000005A for the improved kind, nothing for the legacy
//                       kind. The variant table (task p5-03) passes the kind; the door never
//                       takes a value.
//   writeFlash()        the base's flash runs, through the nRF52 layer's guarded write, refusing
//                       any address inside the board's application region or its file system -
//                       the board table's numbers (core/flasher/board_profile.h), not a caller's.
//   blankCheck()        the first flash word, UICR.NRFFW[0] and UICR.APPROTECT read back.
//
// The three writes refuse unless eraseAll() succeeded in this same opening. Nothing else: no
// NVMC ERASEALL, no ERASEUICR, no partial erase - the CTRL-AP's erase is the only erase S-5a
// needs, and the one that works on a locked part. No ESP-IDF include, no heap, every wait
// bounded by a constant or an argument.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/nrf52/nrf52.h"
#include "core/nrf52/provision_grant.h"
#include "core/swd/dp.h"
#include "interfaces/clock.h"

namespace core {
namespace nrf52 {

// PS NVMC electrical specification: tERASEALL 169 ms typical (with the HFXO; "varies according
// to HFINT accuracy when HFINT is used"), no maximum given. The bench tool stopped at 5 s; so
// does this, thirty times the typical value, polled every 10 ms.
constexpr std::uint32_t kChipEraseTimeoutMs = 5000;
constexpr std::uint32_t kChipErasePollMs = 10;
// CTRL-AP RESET held, and the part given time to come out of reset, before the port is spoken
// to again. The Product Specification gives no figure for the soft reset; the bench tool used
// 10 ms for each and the first board confirms it (docs/bench.md).
constexpr std::uint32_t kSoftResetHoldMs = 10;
constexpr std::uint32_t kSoftResetSettleMs = 10;
// UICR's registers: the first and the last (REGOUT0 at +0x304), FIRMWARE-MANIFEST.md §4a's bounds.
constexpr std::uint32_t kUicrFirst = 0x10001000;
constexpr std::uint32_t kUicrLast = 0x10001307;

// Which kind of access-port protection the part has, by the variant table (task p5-03):
// legacy (build codes A-D, hardware-only) or improved (F, revision 3, IN-141).
enum class ApprotectKind : std::uint8_t { kLegacy, kImproved };
const char* describe(ApprotectKind kind);

enum class DoorStatus : std::uint8_t {
  kOk,
  kSwd,               // the debug port failed: `swd` says how
  kNrf52,             // the nRF52 layer refused or failed: `nrf` says how
  kNotOpen,           // Nrf52::open() has not been called on this session
  kDoorClosed,        // open() has not succeeded
  kAlreadyOpened,     // open() a second time: one grant, one opening
  kGrantSpent,        // the grant was already used (moved from, or opened with)
  kGrantMalformed,    // not S-5a's grammar
  kGrantMismatch,     // a DEVICEID grant on a closed port, or a `locked` grant on an open one
  kWrongDevice,       // the DEVICEID typed is not this chip's
  kNotNrf52840,       // the open port is not an nRF52840 with 4096-byte pages
  kProtected,         // the port is closed: a memory write cannot be attempted
  kNotErased,         // a write without a successful eraseAll() in this opening
  kEraseTimeout,      // ERASEALLSTATUS still busy at the timeout
  kNotAligned,        // a word address that is not word-aligned
  kBadArgument,       // no data, or zero words
  kOutsideUicr,       // writeUicr: not inside 0x10001000-0x10001307
  kApprotectAddress,  // writeUicr: UICR.APPROTECT is writeApprotect()'s alone
  kApplicationRegion, // writeFlash: inside the board's application region or its file system
  kOutsideFlash,      // writeFlash: beyond the chip's flash
  kNoProfile,         // writeFlash: no board profile for this slot
  kNvmcTimeout,       // READY did not come in time; CONFIG is read-only again
};
const char* describe(DoorStatus status);

struct DoorResult {
  DoorStatus status = DoorStatus::kOk;
  swd::Status swd = swd::Status::kOk;  // on kSwd
  Status nrf = Status::kOk;            // on kNrf52
  std::uint32_t address = 0;           // the address concerned, when there is one
  std::uint32_t value = 0;
  bool port_open = false;              // APPROTECTSTATUS as last read by this call
  bool ok() const { return status == DoorStatus::kOk; }
};

struct BlankCheck {
  std::uint32_t flash0 = 0;     // the first flash word (the MBR's initial stack pointer once written)
  std::uint32_t nrffw0 = 0;     // UICR.NRFFW[0], the bootloader's address
  std::uint32_t approtect = 0;  // UICR.APPROTECT
  bool blank = false;           // all three read 0xFFFFFFFF
};

class ProvisionDoor {
 public:
  ProvisionDoor(Nrf52& nrf, swd::Dp& dp, interfaces::Clock& clock, ProvisionGrant&& grant);

  DoorResult open();
  DoorResult approtectStatus(bool* port_open);
  DoorResult eraseAll(std::uint32_t timeout_ms = kChipEraseTimeoutMs);
  DoorResult softReset();
  DoorResult writeUicr(std::uint32_t addr, const std::uint32_t* words, std::size_t n);
  DoorResult writeApprotect(ApprotectKind kind);
  DoorResult writeFlash(std::uint32_t addr, const std::uint32_t* words, std::size_t n);
  DoorResult blankCheck(BlankCheck* out);

  bool opened() const { return opened_; }
  bool erased() const { return erased_; }
  GrantKind grantKind() const { return grant_.kind(); }

  struct Counters {
    std::uint32_t refusals = 0;         // by the guards, before anything reached the chip
    std::uint32_t erase_alls = 0;       // ERASEALL written
    std::uint32_t soft_resets = 0;      // CTRL-AP RESET released
    std::uint32_t uicr_words = 0;       // UICR words written (APPROTECT included)
    std::uint32_t flash_words = 0;      // flash words written
    std::uint32_t reconnects = 0;       // Nrf52::open() run again
  };
  const Counters& counters() const { return counters_; }

 private:
  DoorResult refuse(DoorStatus status, std::uint32_t address = 0);
  DoorResult fromDp(const swd::Result& r);
  DoorResult fromNrf(const Result& r);
  DoorResult readPort(bool* port_open);
  DoorResult reopen();  // Nrf52::open() again, kProtected folded into port_open
  DoorResult gateWrite();  // opened, erased, the port open, the chip identified
  DoorResult writeUicrWords(std::uint32_t addr, const std::uint32_t* words, std::size_t n);
  DoorResult restoreConfig(DoorResult first);

  Nrf52& nrf_;
  swd::Dp& dp_;
  interfaces::Clock& clock_;
  ProvisionGrant grant_;
  bool opened_ = false;
  bool erased_ = false;
  Counters counters_;
};

}  // namespace nrf52
}  // namespace core
