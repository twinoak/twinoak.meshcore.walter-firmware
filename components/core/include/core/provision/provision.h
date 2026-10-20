// `rak provision` (task p5-03): spec/SYSTEM.md S-5a, the sequence that gives a factory radio its
// base, step for step, on this firmware's own layers - the store (task p5-01: the base, checked
// again), the nRF52 session and the one door (task p5-02), the link client (HELLO, for a radio
// whose port is closed), RadioPins (the one pin reset, with its rules) and the supervisor (which
// is told the radio now awaits its first install). docs/provisioning.md is the session;
// docs/ARCHITECTURE.md §4 says where this sits.
//
// Seven steps, each a name in the result so that the console and the report can say where a run
// ended:
//
//   1  check     refuse before anything: no base in the store for this slot, or one that fails
//                check 5p again (the store is flash), or whose bytes are not its record's; an
//                update in hand or the supervisor climbing (either holds the radio); another
//                slot's operation holding the selection; the battery low or falling by the radio
//                update's rule - an *unknown* battery is accepted, exactly as `--first-install`
//                accepts it, because a factory RUI3 radio has no link and so no reading; the
//                kill line driven. The slot is taken and held for the whole command.
//   2  attach    DPIDR, the control port's IDR, APPROTECTSTATUS. Open: FICR - refused, nothing
//                erased, if the part is not an nRF52840, or the typed radio is not its DEVICEID
//                (case-free), or `locked` was typed. Closed: the link is asked HELLO - no answer:
//                only `locked` is accepted; an answer: IDENTITY, and only `locked:<key16>` with
//                the first 16 hex digits of that public key is accepted, so that a radio with an
//                identity is named before it loses it. Then the door is opened with the grant,
//                and the door checks the port half of this again against the chip.
//   3  variant   the table (variant.h): A-D legacy, F improved, anything else refused. On an
//                open radio before the erase, with nothing erased; on a closed one after it -
//                the identity is readable only then - and a variant outside the table then
//                stops the command with the radio erased (it has lost nothing it had not lost
//                already: extend the table, run it again).
//   4  erase     the door's eraseAll; APPROTECTSTATUS read back; closed -> the one CTRL-AP soft
//                reset, attach again; still closed -> stop. Identify again; the blank check
//                (flash word 0, NRFFW[0], UICR.APPROTECT read 0xFFFFFFFF); the core halted.
//   5  write     the base's words (core/image/base.h BaseWords) through the door in runs, flash
//                first then UICR, then writeApprotect with the table's kind. No pin, power-on or
//                brownout reset between the erase and this last write: step 4's one soft reset is
//                the only reset allowed there, which a test holds with the simulated chip's
//                resets counted by kind.
//   6  verify    every word of the base read back and compared; UICR.APPROTECT against the
//                table's value. A difference stops the command and names the first address: the
//                radio is then safe to provision again.
//   7  reset     the halt released, the session closed, one pin reset through RadioPins (open
//                drain, at least a second since the last - waited for, never skipped), the
//                bootloader given kBootloaderSettleMs, attach again, APPROTECTSTATUS and NRFFW[0]
//                read. The port after the reset is reported open or closed: closed on a revision
//                3 part means the bootloader does not copy UICR.APPROTECT into APPROTECT.DISABLE
//                (spec/SYSTEM.md §10.2) - the radio works, the flasher cannot reach it until that
//                is solved; it is reported, not a failure of the command.
//
// Before step 4 writes anything the supervisor is told the slot *awaits its first install*
// (core/supervisor/supervisor.h), so that a Walter which restarts in the middle - and one that
// finishes - leaves the silent, application-less radio alone until `rak update --first-install`
// commits. A failure anywhere leaves the radio erased or as it was, and the command starts from
// erased, so it is simply run again: no journal (S-5a).
//
// Who may call this: the console, from a typed line, with a grant it made from that line
// (core/nrf52/provision_grant.h). Rail R3b holds that nothing else includes the door; a later
// MeshMaster job (S-5a, *later*) would call run() with a grant of its own and get exactly these
// checks. No ESP-IDF include, no heap; one Provision is ~11 KB (the base's reader and its
// window, two run buffers) and belongs in static storage, which the console is.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/image/base.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/nrf52/nrf52.h"
#include "core/nrf52/provision_door.h"
#include "core/nrf52/provision_grant.h"
#include "core/provision/variant.h"
#include "core/supervisor/supervisor.h"
#include "core/swd/dp.h"
#include "core/swd/wire.h"
#include "core/update/battery.h"
#include "core/update/radio_update.h"
#include "interfaces/clock.h"

namespace core {
namespace provision {

// -- the numbers, PLACEHOLDERS for the bench (docs/bench.md §4a) ---------------------------------

// How long the bootloader is given after the pin reset before the debug port is spoken to again:
// the Adafruit bootloader starts, finds no application and waits in DFU mode. The bench tool
// used 1500 ms (walter-swd-bench nrf52.cpp provision(), step 5); the first board says.
constexpr std::uint32_t kBootloaderSettleMs = 1500;

// -- the words -----------------------------------------------------------------------------------

// How many words one run through the door carries: a kilobyte, which is also the memory
// access port's auto-increment block (docs/ARCHITECTURE.md §4), so a run never straddles one.
constexpr std::size_t kRunWords = 256;

// PS "UICR > APPROTECT", PALL = HwDisabled (0x5A): what step 6 expects to read back on an
// improved part, the whole word as the door writes it (the upper bytes cleared). On a legacy
// part the word is left erased and reads 0xFFFFFFFF.
constexpr std::uint32_t kApprotectHwDisabledWord = 0x0000005A;
constexpr std::uint32_t kErasedWord = 0xFFFFFFFF;

enum class Step : std::uint8_t {
  kNone = 0,
  kCheck = 1,
  kAttach = 2,
  kVariant = 3,
  kErase = 4,
  kWrite = 5,
  kVerify = 6,
  kReset = 7,
  kDone = 8,
};
constexpr std::size_t kStepCount = 9;
const char* describe(Step step);

enum class Status : std::uint8_t {
  kOk = 0,
  // step 1: nothing touched
  kNoStore,             // this build has no image store
  kNoProfile,           // no board profile for the slot
  kNoBase,              // no base in the store for this slot
  kBaseRefused,         // the stored base fails check 5p again: `why` says which rule
  kBaseUnreadable,      // its bytes are not its record's SHA-256 (flash rot)
  kUpdateRunning,       // the radio's update machine has a run in hand
  kSupervisorBusy,      // the supervisor is climbing its ladder (or classifying a silence)
  kSlotBusy,            // another slot's operation holds the selection
  kBatteryLow,          // below update::kBatteryMinMv
  kBatteryFalling,      // falling faster than update::kBatteryFallingMvPerHour
  kKillAsserted,        // the kill line is driven: the rail is off
  // step 2: nothing touched
  kPins,                // the SWD lines could not be acquired: `pins` says why
  kSwd,                 // the debug port failed: `nrf` says how
  kNotNrf52840,         // the open port is not an nRF52840 with 4096-byte pages
  kGrantMalformed,      // not S-5a's grammar
  kGrantMismatch,       // a DEVICEID for a closed port, `locked` for an open one
  kWrongDevice,         // the DEVICEID typed is not this radio's
  kApplicationAnswers,  // closed, and an application answers HELLO: only locked:<key16> will do
  kWrongKey,            // locked:<key16> with another key than IDENTITY's
  kNoApplication,       // locked:<key16> typed, and nothing answers HELLO: only `locked` will do
  kDoor,                // the door refused to open: `door` says why
  // step 3
  kVariantUnknown,      // not in the table, nothing erased
  kVariantUnknownErased,// not in the table, found after the erase: the radio is erased
  // step 4: the radio is erased from here on
  kEraseFailed,         // the door's erase or soft reset failed: `door` says how
  kPortStillClosed,     // closed after the erase and the one soft reset
  kNotBlank,            // the blank check failed
  kHaltFailed,
  // step 5
  kBaseWords,           // the stored file could not be walked (it changed, or could not be read)
  kWriteFailed,         // the door refused or failed a write: `door` says how
  // step 6
  kVerifyFailed,        // a read-back failed: `nrf` says how
  kVerifyMismatch,      // a word read back differs: `mismatch_*` name it
  // step 7
  kResetFailed,         // the pin reset was refused: `pins` says why
  kLookFailed,          // no debug port after the reset: the base was verified before it
};
const char* describe(Status status);

struct Result {
  Status status = Status::kOk;
  Step ended = Step::kNone;     // the step the run ended in: kDone when ok
  bool erased = false;          // ERASEALL was written in this run
  bool ok() const { return status == Status::kOk; }
  // check() only: the first refusal step 1 would make (the store, the battery, the kill line,
  // an update or a climb in hand), kOk when none - reported beside what the radio said.
  Status check_status = Status::kOk;

  // The underlying answers, for the console's line.
  nrf52::DoorResult door;
  nrf52::Result nrf;
  board::Outcome pins = board::Outcome::kOk;
  // What was found (step 1).
  bool base_present = false;
  std::uint8_t base_sha256[image::kSha256Size] = {};
  image::BaseSummary base;
  char why[image::kMaxBaseWhy] = {};  // a 5p refusal's sentence
  std::uint32_t base_check_ms = 0;
  bool battery_known = false;
  std::uint16_t battery_mv = 0;
  // What was found (step 2).
  std::uint32_t dpidr = 0;
  std::uint32_t ctrl_ap_idr = 0;
  bool attached = false;
  bool port_open_at_attach = false;
  bool identified = false;
  nrf52::Identity identity;
  Variant variant;
  nrf52::ApprotectKind kind = nrf52::ApprotectKind::kLegacy;
  bool link_asked = false;
  bool application_answered = false;
  char key16[nrf52::kKey16Digits + 1] = {};  // the first 16 hex digits of IDENTITY's key, upper-cased
  nrf52::GrantKind grant = nrf52::GrantKind::kNone;
  // Steps 4 to 7.
  bool port_open_after_erase = false;
  bool soft_reset_done = false;
  bool blank = false;
  std::uint32_t flash_words = 0;
  std::uint32_t flash_runs = 0;
  std::uint32_t uicr_words = 0;
  std::uint32_t approtect_written = 0;    // the word written at UICR.APPROTECT (kErasedWord: none)
  std::uint32_t verified_words = 0;
  std::uint32_t approtect_read = 0;
  std::uint32_t mismatch_address = 0;
  std::uint32_t mismatch_expected = 0;
  std::uint32_t mismatch_actual = 0;
  std::uint32_t reset_wait_ms = 0;        // what the spacing made the pin reset wait
  bool port_open_after_reset = false;
  std::uint32_t nrffw0_after = 0;
  // How long each step took, and the whole.
  std::uint32_t step_ms[kStepCount] = {};
  std::uint32_t total_ms = 0;
  // What to type next (check(): the command; run(): after a refusal, the same).
  char next[64] = {};
};

// What the console prints a line for as each step ends, so that a person watching a command that
// takes minutes sees it move. `step` has just ended; `so_far` is the result as it stands.
class StepListener {
 public:
  virtual ~StepListener() = default;
  virtual void onStep(Step step, const Result& so_far) = 0;
};

class Provision {
 public:
  Provision(board::RadioPins& pins, board::SlotLock& slots, interfaces::Clock& clock,
            link::client::Client& radio, image::Store* store, update::BatterySource* battery,
            update::RadioUpdate* update, supervisor::Supervisor* supervisor);

  // `rak provision check`: steps 1 and 2 with nothing written and no grant - what the radio is,
  // what the table would do, the base in the store, and `next` the exact line to type.
  Result check(board::Slot slot);

  // `rak provision <radio> confirm`: the whole sequence with the grant the console made from
  // the typed text. One grant, one run.
  Result run(board::Slot slot, nrf52::ProvisionGrant&& grant, StepListener* listener = nullptr);

  // The variant table in force: kVariantTable unless a test extends it.
  void useVariantTable(const VariantEntry* table, std::size_t size) {
    table_ = table;
    table_size_ = size;
  }

 private:
  struct Session;  // the wire, the debug port, the nRF52 session and the door, on run()'s stack

  bool checkStore(board::Slot slot, Result* r);
  bool checkBattery(Result* r);
  bool precheck(board::Slot slot, Result* r);
  bool attach(Session& s, board::Slot slot, Result* r, bool with_grant);
  bool askLink(Result* r);
  bool nameMatches(Result* r);
  bool identifyAndLookUp(Session& s, Result* r);
  bool erase(Session& s, Result* r);
  bool writeBase(Session& s, Result* r);
  bool verifyBase(Session& s, Result* r);
  bool resetAndLook(Session& s, board::Slot slot, Result* r);
  bool flushRun(Session& s, Result* r, std::uint64_t at, std::size_t n);
  bool compareRun(Session& s, Result* r, std::uint64_t at, std::size_t n);
  void suggest(Result* r);
  void step(Step step, Result* r, StepListener* listener);
  bool fail(Result* r, Status status, Step step, StepListener* listener);

  board::RadioPins& pins_;
  board::SlotLock& slots_;
  interfaces::Clock& clock_;
  link::client::Client& radio_;
  image::Store* store_;
  update::BatterySource* battery_;
  update::RadioUpdate* update_;
  supervisor::Supervisor* supervisor_;
  const VariantEntry* table_ = kVariantTable;
  std::size_t table_size_ = kVariantTableSize;

  std::uint64_t step_began_ms_ = 0;
  // The base, as the store holds it: the file, §4a's reader over it (the runs it gives are what
  // BaseWords walks) and the window the words come out of. ~9 KB, which is why this object is
  // static storage.
  image::StoredImage file_;
  image::BaseReader reader_;
  image::BaseResult judged_;
  image::BaseWords words_;
  std::uint32_t run_[kRunWords];
  std::uint32_t back_[kRunWords];
};

}  // namespace provision
}  // namespace core
