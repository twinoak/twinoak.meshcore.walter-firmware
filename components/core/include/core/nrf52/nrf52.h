// The nRF52840 behind the debug port (task p1-04): identification, access-port protection, the
// core's halt and reset, and the flash controller - with an address guard at the bottom and no
// path to a chip erase. Everything about the chip is Nordic's (the nRF52840 Product
// Specification, cited register by register in regs.h), everything about the core Arm's (the
// Armv7-M Architecture Reference Manual), and all of it was developed against the simulated chip
// in host_test/sim/nrf52_chip. No ESP-IDF include, no heap, every wait bounded by a constant.
//
// A session:
//
//   open(slot)    the slot first (task p1-06: the debug lines are steered to that radio and held
//                 until close(), when a SlotLock was given), then connect to the debug port and
//                 power it up; read the control access port's IDR
//                 and APPROTECTSTATUS -> protection(); if open, open the memory access port and
//                 identify() the chip from FICR. Nothing is halted, nothing is written to the chip.
//   identify()    part, variant (as text), page size, pages, flash size, device id. Anything but
//                 an nRF52840 with 4096-byte pages leaves identified() false, and every operation
//                 below is refused with kNotNrf52840.
//   protection()  kOpen or kProtected. On a protected chip memory access is never attempted and
//                 every operation below returns kProtected - a distinct, reportable result. The
//                 way in (CTRL-AP ERASEALL) destroys the bootloader, the SoftDevice, the node's
//                 identity and UICR; it is a bench procedure for a person (hardware repository,
//                 BENCH-BREADBOARD.md caveat 6), and this firmware has no code for it.
//   halt(), resetAndHalt(), run(), systemReset(), coreState()
//                 through DHCSR, DEMCR and AIRCR over the memory access port.
//   erasePage(region, addr), writeWords(region, addr, data, words), read(addr, out, words)
//                 the flash controller. erasePage and writeWords take a FlashRegion and refuse
//                 anything outside it - before any NVMC access - and there is no overload without
//                 one: the flasher (task p1-05) owns which region; this is the backstop. NVMC.CONFIG
//                 is returned to read-only after every operation and on every error path.
//   close()       the power-up request withdrawn: on the nRF52840 that is what ends debug
//                 interface mode (PS "Debug and trace > Debug Interface mode").
//
// What does not exist here: a function that writes NVMC.ERASEALL, NVMC.ERASEUICR, the control
// port's ERASEALL, or any address in UICR. The names are constants in regs.h for the simulator
// and the reader; host_test/core/nrf52/test_nrf52_forbidden.cpp fails if one of them is used
// anywhere else under components/.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/nrf52/regs.h"
#include "core/swd/dp.h"
#include "core/swd/mem_ap.h"
#include "interfaces/clock.h"

namespace core {
namespace nrf52 {

// The bounds on the flash controller's READY (regs.h: tWRITE 41 us and tERASEPAGE 85 ms are the
// typical values; the HTML rendition of the Product Specification gives no maximum, and footnotes
// that the timing "varies according to HFINT accuracy when HFINT is used"). READY is polled over
// SWD, one read a millisecond: an erase is given 300 ms, 3.5 times its typical value; a block of
// writes 50 ms after the last word, a thousand times one word's typical time (the words
// themselves are paced by the bus: the access port stalls while the controller is busy, which
// the debug port sees as WAIT and retries). The report says why these and not others.
constexpr std::uint32_t kErasePageTimeoutMs = 300;
constexpr std::uint32_t kWriteTimeoutMs = 50;
// The core: a halt takes effect within a few cycles; a reset through SYSRESETREQ takes the
// system's reset sequence. Both are polled a millisecond apart.
constexpr std::uint32_t kHaltTimeoutMs = 10;
constexpr std::uint32_t kResetTimeoutMs = 100;
// What identify() will believe about CODESIZE: more pages than 16 MB of flash is a misread.
constexpr std::uint32_t kMaxPages = 4096;

enum class Status : std::uint8_t {
  kOk,
  kSwd,            // the debug port or the access port failed: `swd` says how, `address` where
  kNotOpen,        // open() has not succeeded
  kProtected,      // access-port protection is on: nothing below was attempted
  kNoControlPort,  // AP 1 is not Nordic's control access port
  kNoMemoryPort,   // AP 0 is not a memory access port
  kNotNrf52840,    // identify() found another part, or pages that are not 4096 bytes
  kBadRegion,      // the region is not page-aligned, empty, or not inside the chip's flash
  kOutsideRegion,  // the address, or the end of the data, is outside the region
  kNotAligned,     // a page address that is not page-aligned, or a word address that is not word-aligned
  kBadArgument,    // no data, or zero words
  kNvmcTimeout,    // READY did not come within the bound; CONFIG was returned to read-only
  kCoreTimeout,    // the core did not halt, or did not report the reset, within the bound
  kSlotBusy,       // open(): no such slot, or another slot's operation holds the selection (p1-06)
};
const char* describe(Status status);

struct Result {
  Status status = Status::kOk;
  swd::Status swd = swd::Status::kOk;  // on kSwd
  std::uint32_t address = 0;           // the memory address concerned, when there is one
  std::uint32_t value = 0;
  bool ok() const { return status == Status::kOk; }
};

enum class Protection : std::uint8_t { kUnknown, kOpen, kProtected };
const char* describe(Protection protection);

enum class CoreState : std::uint8_t { kUnknown, kRunning, kHalted, kSleeping, kLockedUp };
const char* describe(CoreState state);

struct Identity {
  std::uint32_t part = 0;         // FICR.INFO.PART
  std::uint32_t variant_raw = 0;  // FICR.INFO.VARIANT
  char variant[5] = {};           // the same as text, "AAF0"
  std::uint32_t page_size = 0;    // FICR.CODEPAGESIZE
  std::uint32_t pages = 0;        // FICR.CODESIZE
  std::uint32_t flash_bytes = 0;  // pages * page_size
  std::uint32_t info_flash_kb = 0;  // FICR.INFO.FLASH
  std::uint32_t device_id[2] = {};  // FICR.DEVICEID[0], [1]
  bool is_nrf52840 = false;       // part 0x52840 and 4096-byte pages
};

// A range of flash this firmware may erase and write: [begin, end), page-aligned, inside the
// chip's flash. The flasher's table (task p1-05) makes them; nothing here makes one by default.
struct FlashRegion {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::uint32_t size() const { return end - begin; }
};

class Nrf52 {
 public:
  // `slots` is the slot seam (task p1-06): when one is given, open() takes a hold on the slot it
  // is asked for and keeps it until close(), so the debug lines cannot be steered to the other
  // radio in the middle of a session. nullptr for a caller that holds the slot itself - the
  // flasher does, around everything it then asks of this object.
  Nrf52(swd::Dp& dp, interfaces::Clock& clock, board::SlotLock* slots = nullptr);

  // kSlotBusy when the slot is not there, or when another slot's operation holds the selection;
  // nothing reached the wire. Slot 0 by default: the only radio of a unit today (S-11).
  Result open(board::Slot slot = {});
  Result close();
  Result identify();

  board::Slot slot() const { return slot_; }
  bool isOpen() const { return open_; }
  Protection protection() const { return protection_; }
  bool identified() const { return identity_.is_nrf52840; }
  const Identity& identity() const { return identity_; }
  std::uint32_t ctrlApIdr() const { return ctrl_ap_idr_; }
  swd::MemAp& memAp() { return mem_ap_; }
  swd::Dp& dp() { return dp_; }

  // The core (ARM C1.6.2, C1.6.5, B3.2.6).
  Result halt();
  Result resetAndHalt();
  Result run();
  Result systemReset();
  Result coreState(CoreState* out);

  // The flash controller (PS "NVMC").
  Result erasePage(const FlashRegion& region, std::uint32_t page_addr);
  Result writeWords(const FlashRegion& region, std::uint32_t addr, const std::uint32_t* data,
                    std::size_t words);
  // Any address the access port can reach, read-only; refused like the rest when the chip is
  // protected or not an nRF52840.
  Result read(std::uint32_t addr, std::uint32_t* out, std::size_t words);
  // READY awaited, bounded: for a caller that has just halted a core whose own firmware may have
  // an erase or a write in flight (the flasher, task p1-05). Writes nothing.
  Result waitFlashReady(std::uint32_t timeout_ms);

  struct Counters {
    std::uint32_t erases = 0;
    std::uint32_t words_written = 0;
    std::uint32_t ready_polls = 0;
    std::uint32_t config_writes = 0;
    std::uint32_t refusals = 0;  // by the guards, before anything reached the chip
  };
  const Counters& counters() const { return counters_; }

 private:
  Result fromMem(const swd::MemResult& m);
  Result fromDp(const swd::Result& r);
  Result gate();  // open, not protected, identified
  Result checkRegion(const FlashRegion& region);
  Result readProtection();
  Result waitReady(std::uint32_t timeout_ms);
  Result setConfig(std::uint32_t wen);
  Result restoreConfig(Result first);  // CONFIG = Ren; the earlier failure wins
  Result pollDhcsr(std::uint32_t mask, std::uint32_t wanted, std::uint32_t timeout_ms);

  swd::Dp& dp_;
  interfaces::Clock& clock_;
  board::SlotLock* slots_;
  swd::MemAp mem_ap_;
  board::Slot slot_{};
  bool slot_held_ = false;
  bool open_ = false;
  Protection protection_ = Protection::kUnknown;
  std::uint32_t ctrl_ap_idr_ = 0;
  Identity identity_;
  Counters counters_;
};

}  // namespace nrf52
}  // namespace core
