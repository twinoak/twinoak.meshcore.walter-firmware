// The flasher (task p1-05; docs/ARCHITECTURE.md §5; spec/SYSTEM.md S-5): an image into the radio's
// application region and into nothing else, every byte checked, the bootloader left willing to
// start it, the radio restarted; interruptible anywhere and convergent when run again. Developed
// against the simulated chip with the power cut at every flash operation (host_test/core/flasher/).
// No ESP-IDF include, no heap, every loop bounded by the image's size or a constant.
//
// Every call takes the slot it is for (task p1-06), default 0 - the only radio of a unit today
// (spec/SYSTEM.md S-11). The slot is selected and held for the whole of the call, before a pin
// moves, so the wires cannot be steered to the other radio of a dual adapter in the middle of a
// flash; kSelect is the step that failed when they could not be.
//
// The steps of flash(), each a name in the result when it fails (the comments in flasher.cpp say
// what each does and why in that order):
//   0  kSelect                                              - the slot: selected and held
//   1  kPins, kConnect, kProtected, kIdentify, kSoftDevice   - the lines, the port, the chip
//   2  kImage, kScan                                        - the image checked and every page
//                                                             classified; nothing written yet
//   3  kHalt, kSettingsInvalidate                           - the core halted (not reset: task
//                                                             p1-04's Fxx finding); the bootloader
//                                                             told bank 0 holds nothing
//   4  kFirstPageErase                                      - the vector table page erased first
//   5  kPages, kFirstPage                                   - the pages; the first one last
//   6  kReadBack, kDigest, kSettingsValidate                - the whole image read back, hashed;
//                                                             the bootloader told it is valid
//   7  kReset                                               - debug interface mode ended, the lines
//                                                             released, and the image started by a
//                                                             pulse on nRESET (task p18-01: the one
//                                                             reset that clears a watchdog)
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/flasher/board_profile.h"
#include "core/flasher/bootloader_settings.h"
#include "core/flasher/image_source.h"
#include "core/image/crypto/sha256.h"
#include "core/nrf52/nrf52.h"
#include "core/swd/dp.h"
#include "core/swd/wire.h"
#include "interfaces/clock.h"

namespace core {
namespace flasher {

enum class Step : std::uint8_t {
  kNone,
  kSelect,
  kPins,
  kConnect,
  kProtected,
  kIdentify,
  kSoftDevice,
  kImage,
  kScan,
  kHalt,
  kSettingsInvalidate,
  kFirstPageErase,
  kPages,
  kFirstPage,
  kReadBack,
  kDigest,
  kSettingsValidate,
  kErase,  // flashTest: the pattern erased again
  kReset,
  kDone,
};
const char* describe(Step step);
// One word, for a journal entry's line and for the note that goes to the server with a job
// (task p18-05): `scan`, `settings-invalidate`, `first-page`. `describe()` is the phrase.
const char* name(Step step);

enum class Outcome : std::uint8_t {
  kOk,
  kRefused,  // a check failed before anything was written: nothing on the chip changed
  kFailed,   // the chip, the debug port or the lines failed mid-way; `nrf` says how
};

struct Counters {
  std::uint32_t pages_total = 0;
  std::uint32_t pages_skipped = 0;           // already held the image's bytes
  std::uint32_t pages_written_in_place = 0;  // reached by clearing bits: no erase
  std::uint32_t pages_erased = 0;            // erased, then written
  std::uint32_t retries = 0;                 // a page that verified wrong and was done again
  std::uint32_t erases = 0;                  // page erases in all, the settings page's included
  std::uint32_t words_written = 0;
  std::uint32_t words_read = 0;
  std::uint32_t settings_erases = 0;
  std::uint32_t settings_writes = 0;  // records written
  std::uint32_t elapsed_ms = 0;
  // **What the debug port waited out during this flash** (task p19-01, deliverable 3). A WAIT is
  // the chip saying it cannot take the access now, and on this chip that is most often a flash
  // operation of the application's own in flight (`core/swd/dp.h`): the port asks again with the
  // line idle for as long as the Product Specification says one may take, and these are what it
  // cost. `waits` is how many transfers waited at all, `wait_longest_ms` the longest of them, and
  // `wait_longest_at`/`wait_longest_step` where that one was - the address off the access port,
  // the step off the flasher, which is the only layer that knows it. All nought on a flash that
  // never waited, which is every flash that has ever gone through on a bench.
  std::uint32_t waits = 0;
  std::uint32_t wait_longest_ms = 0;
  std::uint32_t wait_longest_at = 0;
  bool wait_longest_at_known = false;
  Step wait_longest_step = Step::kNone;
};

// What started the image this flash wrote (task p18-04). A unit whose module has the pin reset
// enabled gets the pulse of task p18-01; one whose module has not gets the exit every flash had
// before it, a soft reset through the debug port - because a pulse on a pin that is not nRESET
// cannot start anything, and the image would be left halted.
enum class Started : std::uint8_t {
  kNothing = 0,     // nothing was started: a refusal, a failure before the halt, or a verify
  kResetLine = 1,   // a pulse on nRESET (RadioPins::resetPulse())
  kDebugPort = 2,   // a soft reset over the debug port (Nrf52::systemReset())
};
const char* describe(Started started);

struct Result {
  Outcome outcome = Outcome::kOk;
  Step step = Step::kDone;    // the step that failed, or kDone
  nrf52::Result nrf;          // on kFailed: the nRF52 layer's result
  std::uint32_t address = 0;  // the flash address concerned, when there is one
  const char* why = "";       // one phrase for a person
  // The image was started (task p18-01, p18-04). False after a refusal, a failure before the
  // halt, and a verify - and when the start itself was refused, which is a kFailed at kReset with
  // the pin layer's or the nRF52 layer's word.
  bool reset_issued = false;
  Started started = Started::kNothing;  // and by which of the two
  // What UICR said about the reset pin at this flash, read while the port was open (task p18-04).
  // `known` false when the port never got that far, or the read failed.
  nrf52::ResetPin reset_pin;
  // startAgain() only: what the core was doing when the look was made.
  nrf52::CoreState core = nrf52::CoreState::kUnknown;
  // **What this flash did to the chip** (task p18-05, deliverable 1), worked out from what the
  // flasher did and not from the name of the step it stopped at.
  //
  // `wrote_nothing` is true while no word has been written and no page erased - of the image's
  // pages or of the bootloader's settings page - which is every way out before the first write:
  // a refusal, the slot, the lines, the port, the chip's identity, the SoftDevice, the image's
  // own checks, the scan, the halt, and the settings page's read. It is read off the counters,
  // so a path added later cannot forget to set it. A `verify()` and the look of
  // `startAgain()` write nothing and say so too.
  //
  // `core_halted` is true when the core was halted by this call, and so had to be started again
  // at the exit - which is what tells a flash that wrote nothing *and* left the radio running
  // from one that wrote nothing and had to restart it.
  bool wrote_nothing = true;
  bool core_halted = false;
  // verify() only: the settings page holds a record the bootloader accepts for this image.
  bool settings_valid = false;
  bootloader_settings::Record settings;
  std::uint16_t softdevice_fwid = 0;  // what the chip's SoftDevice information structure said
  Counters counters;
  bool ok() const { return outcome == Outcome::kOk; }
};

// **Whose fault a failed flash was** (task p18-05, deliverable 5): the image's own - its bytes,
// its load address, its digest - or the unit's, which is everything else: the slot, the lines,
// the debug port, the chip's identity, its SoftDevice, a page that would not take, a core that
// would not halt. `spec/SYSTEM.md` S-5's *the candidate's hash is remembered and never tried
// again* stands under the health check's *no* in §6's drawing, so only an image's own fault puts
// its hash on the store's bad list; a debug port that gave up is the unit's, and that image may
// be flashed again.
//
// Two steps are the image's: `kImage`, whose every way out is a statement about the file (empty,
// not page-aligned, not inside the region, more pages than the chip has, bytes the store would
// not read), and `kDigest`, which is the image's proof - the SHA-256 of what came back off the
// chip is not the one the record gave. `kReadBack`'s own mismatch is not: the bytes went in and
// the flash did not hold them, which is the chip's.
bool blamesTheImage(const Result& result);

struct Progress {
  Step step;  // kScan, kPages, kFirstPage, kReadBack
  std::uint32_t pages_done;
  std::uint32_t pages_total;
  std::uint32_t address;  // the page just finished
};

class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  virtual void onProgress(const Progress& progress) = 0;
};

// The SoftDevice information structure (the bootloader's copy of Nordic's nrf_sdm.h,
// s140_nrf52_6.1.1_API/include/nrf_sdm.h:99-138; nrf_mbr.h:68 MBR_SIZE 0x1000): at MBR_SIZE +
// 0x2000, the magic 0x51B1E5DB at +0x04 (dfu_types.h:64-71, is_sd_existed()), the SoftDevice's
// size at +0x08 (SD_SIZE_OFFSET), its firmware id at +0x0C (SD_FWID_OFFSET, 16 bits). The
// bootloader starts the application at that size (dfu_types.h:77); the profile check reads it.
constexpr std::uint32_t kSoftDeviceInfoAddress = 0x1000 + 0x2000;
constexpr std::uint32_t kSoftDeviceMagic = 0x51B1E5DB;

class Flasher {
 public:
  Flasher(board::RadioPins& pins, interfaces::Clock& clock, board::SlotLock& slots);

  Result flash(const Image& image, const BoardProfile& profile, ProgressSink* progress,
               board::Slot slot = {});
  Result verify(const Image& image, const BoardProfile& profile, board::Slot slot = {});
  Result flashTest(const BoardProfile& profile, ProgressSink* progress, board::Slot slot = {});

  // **The look after a flash whose pulse nothing answered** (task p18-04, deliverable 3): the
  // debug port opened once more and the core's state read, and - only where the core is still
  // halted where the flash left it, which is a pulse that did not reach the chip - the image
  // started through the debug port. Nothing is written to flash and nothing is sent to a core
  // that is running. `core` says what the look found, `started` whether anything was sent, and
  // `reset_pin` what UICR says; a port that cannot be opened is the refusal or failure it always
  // was. The caller makes one such look a flash (core/update/radio_update.cpp).
  Result startAgain(const BoardProfile& profile, board::Slot slot = {});

  // What a pulse on the reset line was seen to do, written where the line's record lives
  // (core/board/radio_pins.h, RadioPins::noteResetLine). The flasher is the object that both
  // pulses the line and holds the pins, so the update machine - which is the one thing that can
  // tell whether a boot answered a pulse - says it through here. The supervisor holds the pins
  // itself and says it directly.
  void noteResetLine(bool restarted);

  static constexpr std::uint32_t kTestPages = 2;
  static constexpr std::uint32_t kMaxPages = nrf52::flash::kPagesNrf52840;
  static constexpr std::uint32_t kPageWords = nrf52::flash::kPageSize / 4;

  swd::Dp& dp() { return dp_; }
  nrf52::Nrf52& nrf() { return nrf_; }

 private:
  enum class PageClass : std::uint8_t { kEqual, kInPlace, kErase };

  struct Plan {
    board::Slot slot{};
    bool slot_held = false;  // the hold begin() took and finish() gives back (task p1-06)
    const BoardProfile* profile = nullptr;
    const Image* image = nullptr;
    std::uint32_t size = 0;    // the image's bytes
    std::uint32_t padded = 0;  // to a whole word
    std::uint32_t first_page = 0;
    std::uint32_t pages = 0;
    nrf52::FlashRegion pages_region;  // exactly the image's pages
    bool is_application = false;  // load address = the region's start: the settings page is ours
    bool any_change = false;
    bool halted = false;
    nrf52::ResetPin reset_pin;             // what UICR said, read by openChip() (task p18-04)
    Started started = Started::kNothing;   // what finish(), or startAgain(), started the image by
    nrf52::CoreState core = nrf52::CoreState::kUnknown;  // startAgain()'s look
    PageClass klass[kMaxPages] = {};
  };

  Result openChip(Plan* plan);  // step 1: the slot, the lines, the port, the chip, UICR's two words
  Result begin(const Image& image, const BoardProfile& profile, board::Slot slot, Plan* plan);  // steps 1 and 2 (kImage)
  Result scan(Plan* plan, ProgressSink* progress);
  Result programPages(Plan* plan, ProgressSink* progress);
  Result programPage(Plan* plan, std::uint32_t page_addr, Step step);
  Result writeRuns(const nrf52::FlashRegion& region, std::uint32_t page_addr, const std::uint32_t* want,
                   const std::uint32_t* have, bool only_blank);
  Result readBack(Plan* plan, ProgressSink* progress, bool check_digest);
  Result settingsRead(const BoardProfile& profile, bootloader_settings::Record* out);
  Result settingsWrite(const BoardProfile& profile, const bootloader_settings::Record& record, bool erase_first);
  Result settingsInvalidate(Plan* plan);
  Result settingsValidate(Plan* plan);
  bool settingsAccept(const bootloader_settings::Record& record, const Plan& plan);
  Result finish(Plan* plan, Result r, bool reset);  // the common exit: reset, close, release
  bool loadPage(const Plan& plan, std::uint32_t page_addr);  // the image's bytes for a page -> want_
  Result readPage(std::uint32_t page_addr);                  // the chip's -> have_
  Result failed(Step step, const nrf52::Result& nrf, std::uint32_t address = 0);
  static Result refused(Step step, const char* why, std::uint32_t address = 0);
  void report(ProgressSink* progress, Step step, std::uint32_t done, std::uint32_t total, std::uint32_t address);
  // The debug port's record of what this connection waited out, copied into the counters, with
  // the longest WAIT credited to the step in hand (task p19-01). Called as each step passes, so a
  // figure that grew is credited to the step that was running when it did.
  void noteWaits(Step step);

  board::RadioPins& pins_;
  interfaces::Clock& clock_;
  board::SlotLock& slots_;
  swd::Wire wire_;
  swd::Dp dp_;
  nrf52::Nrf52 nrf_;
  Counters counters_;
  std::uint64_t start_ms_ = 0;
  std::uint8_t bytes_[nrf52::flash::kPageSize];
  std::uint32_t want_[kPageWords];
  std::uint32_t have_[kPageWords];
};

}  // namespace flasher
}  // namespace core
