// The flasher (task p1-05; docs/ARCHITECTURE.md §5; spec/SYSTEM.md S-5): an image into the radio's
// application region and into nothing else, every byte checked, the bootloader left willing to
// start it, the radio restarted; interruptible anywhere and convergent when run again. Developed
// against the simulated chip with the power cut at every flash operation (host_test/core/flasher/).
// No ESP-IDF include, no heap, every loop bounded by the image's size or a constant.
//
// The steps of flash(), each a name in the result when it fails (the comments in flasher.cpp say
// what each does and why in that order):
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
//   7  kReset                                               - a system reset; pins released
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
};

struct Result {
  Outcome outcome = Outcome::kOk;
  Step step = Step::kDone;    // the step that failed, or kDone
  nrf52::Result nrf;          // on kFailed: the nRF52 layer's result
  std::uint32_t address = 0;  // the flash address concerned, when there is one
  const char* why = "";       // one phrase for a person
  bool reset_issued = false;  // the system reset was requested through the debug port
  bool port_closed_after_reset = false;  // ... and the module then closed its access port (Fxx)
  // verify() only: the settings page holds a record the bootloader accepts for this image.
  bool settings_valid = false;
  bootloader_settings::Record settings;
  std::uint16_t softdevice_fwid = 0;  // what the chip's SoftDevice information structure said
  Counters counters;
  bool ok() const { return outcome == Outcome::kOk; }
};

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
  Flasher(board::RadioPins& pins, interfaces::Clock& clock);

  Result flash(const Image& image, const BoardProfile& profile, ProgressSink* progress);
  Result verify(const Image& image, const BoardProfile& profile);
  Result flashTest(const BoardProfile& profile, ProgressSink* progress);

  static constexpr std::uint32_t kTestPages = 2;
  static constexpr std::uint32_t kMaxPages = nrf52::flash::kPagesNrf52840;
  static constexpr std::uint32_t kPageWords = nrf52::flash::kPageSize / 4;

  swd::Dp& dp() { return dp_; }
  nrf52::Nrf52& nrf() { return nrf_; }

 private:
  enum class PageClass : std::uint8_t { kEqual, kInPlace, kErase };

  struct Plan {
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
    PageClass klass[kMaxPages] = {};
  };

  Result begin(const Image& image, const BoardProfile& profile, Plan* plan);  // steps 1 and 2 (kImage)
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

  board::RadioPins& pins_;
  interfaces::Clock& clock_;
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
