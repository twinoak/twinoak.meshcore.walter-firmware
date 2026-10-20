// The base, judged (task p5-01): spec/FIRMWARE-MANIFEST.md §4a - what a `provision` file may
// hold - over the bytes of such a file, as a stream.
//
// A factory RAK4630 arrives with RAK's RUI3, not the MBR, SoftDevice S140 6.1.1 and bootloader a
// TwinOak application is linked against. The base is those three and the UICR words they need, as
// one Intel HEX file per board, written to a radio once by a person's command (spec/SYSTEM.md
// S-5a). It carries no descriptor - it is not ours and it is not an image - so §4a's seven rules
// are what stands in a descriptor's place, and this file is those seven rules.
//
// `spec/tools/manifestref.py` is the reference: `parse_ihex` and `validate_provision`, rule for
// rule, in their order and **with their words** - the refusals below carry the sentences
// `spec/fixtures/manifest/index.json` gives for each `base-*.hex` fixture, and
// host_test/core/image/test_base.cpp holds every fixture against them, word for word.
//
// -- a file is judged, not a register written ----------------------------------------------------
//
// The addresses §4a names are constants of this header, cited to it. They are **not**
// core/nrf52/regs.h's register names, which rail R3 (tools/check_rails.py) allows in that header
// alone: a base is a file being judged, and nothing here can write anything anywhere. Writing a
// base to a chip is task p5-03's, behind task p5-02's one door.
//
// -- as a stream ---------------------------------------------------------------------------------
//
// A real base is ~450 KB of text (192 KB of flash bytes at 44 characters a 16-byte record), so
// nothing here holds the file, or the bytes it describes, in RAM. What a pass keeps is:
//
//   the runs        BaseRuns: the addresses the file covers, as at most kMaxRuns merged
//                   intervals - 8 bytes each, 264 in all. A real base has five (the MBR, the
//                   SoftDevice, its information structure, the bootloader, the UICR words), and
//                   the rules that ask "is there data at this address" (6) and "how many flash
//                   bytes" (the summary) are answered from them rather than from a second pass.
//   the watched     the six words §4a reads - the SoftDevice's magic, end, id and version, and
//     words        UICR.NRFFW[0] and [1] - each as four bytes and a four-bit mask.
//   the UICR map    one bit per UICR register, 25 bytes: which the file gives a byte of.
//   a record        the one being parsed: 255 bytes of data, and a 256-byte read buffer.
//
// That is ~900 bytes a pass. The seventh rule - no flash byte between the application's start and
// the bootloader - needs `NRFFW[0]`, which the file may give last, and is checked in a second
// pass so that the address it names is the first **in the file's order**, as the reference's is.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/flasher/image_source.h"

namespace core {
namespace image {

// -- the addresses of §4a ------------------------------------------------------------------------
//
// Every one of these is §4a's own number; the section says where each came from (Nordic's
// nrf_sdm.h for the SoftDevice's structure, the nRF52840 product specification for UICR's
// registers) and this header cites the section rather than repeating the chain.

// §4a rule 2: "every byte is in flash (0x00000000-0x000FFFFF) or in UICR's registers
// (0x10001000-0x10001307: REGOUT0, at 0x10001304, is the last)".
constexpr std::uint32_t kFlashEnd = 0x00100000;
constexpr std::uint32_t kUicrFirst = 0x10001000;
constexpr std::uint32_t kUicrAfterLast = 0x10001308;  // one past REGOUT0's last byte

// §4a rule 3: the word the Walter writes from the chip's INFO.VARIANT and never from a file.
constexpr std::uint32_t kUicrApprotect = 0x10001208;

// §4a rule 6: the bootloader's address, which the MBR reads. NRFFW[1] (rule 6's note) is the
// MBR's parameter page: allowed, and written as the file says.
constexpr std::uint32_t kUicrBootloader = 0x10001014;  // NRFFW[0]
constexpr std::uint32_t kUicrMbrParams = 0x10001018;   // NRFFW[1]

// §4a rule 5: the SoftDevice's information structure, "MBR size 0x1000 + Nordic's
// SD_INFO_STRUCT_OFFSET 0x2000, nrf_sdm.h", and the four words of it the rule reads.
constexpr std::uint32_t kSoftDeviceInfo = 0x00003000;
constexpr std::uint32_t kSoftDeviceMagicAt = kSoftDeviceInfo + 0x04;
constexpr std::uint32_t kSoftDeviceEndAt = kSoftDeviceInfo + 0x08;
constexpr std::uint32_t kSoftDeviceIdAt = kSoftDeviceInfo + 0x10;
constexpr std::uint32_t kSoftDeviceVersionAt = kSoftDeviceInfo + 0x14;
constexpr std::uint32_t kSoftDeviceMagic = 0x51B1E5DB;
constexpr std::uint32_t kSoftDeviceS140Id = 140;
constexpr std::uint32_t kSoftDeviceS140Version = 6001001;  // S140 6.1.1

// §4a rule 6: "a 4 KB page". The nRF52840's page, as core/flasher/board_profile.h reads it from
// the product specification; said again here because a base is judged without a board profile.
constexpr std::uint32_t kBasePageSize = 0x1000;

// -- what came of reading one --------------------------------------------------------------------

// Which rule refused the file. The sentence each carries is index.json's `why` for the fixture
// that breaks it; `describe()` says which rule of §4a it is.
enum class BaseReason : std::uint8_t {
  kNone,
  // Rule 1, the records (manifestref.parse_ihex).
  kNotAscii,
  kRecordAfterEnd,
  kNotARecord,
  kNotHexDigits,
  kRecordLength,
  kChecksum,
  kTwoDifferentBytes,
  kRecordType,
  kNoEndRecord,
  kNoData,
  // Rules 2 to 7 (manifestref.validate_provision).
  kNotFlashOrUicr,
  kWritesApprotect,
  kNoMbr,
  kNoSoftDeviceInfo,
  kSoftDeviceEnd,
  kSoftDeviceVersion,
  kNoBootloaderAddress,
  kBootloaderAddress,
  kInsideTheApplication,
  // Ours, not the reference's: the bounds within which this firmware reads a file, and a file it
  // could not read at all. A file that reaches one of these is not a base; each says so in its
  // own words, which are not §4a's because the rule it breaks is not §4a's.
  kTooManyRuns,
  kTooManyRepeats,
  kUnreadable,
};

// Which rule of §4a, for a console line: "rule 1, the records" ... "rule 7, the application".
const char* describe(BaseReason reason);

// UICR's registers, one bit each (§4a rule 2's bounds): REGOUT0 at 0x10001304 is the 194th.
constexpr std::uint16_t kUicrWordCount = (kUicrAfterLast - kUicrFirst) / 4;  // 194

// What the file is, when it is a base: the fields `manifestref.validate_provision` returns.
struct BaseSummary {
  std::uint32_t bootloader = 0;  // UICR.NRFFW[0]
  std::uint32_t flash_bytes = 0;
  std::uint32_t softdevice_id = 0;       // 140
  std::uint32_t softdevice_version = 0;  // 6001001
  bool has_mbr_params = false;
  std::uint32_t mbr_params = 0;  // UICR.NRFFW[1], when the file gives it
  // Which UICR registers the file gives a byte of, lowest address first. A bitmap and not an
  // array of addresses so that a summary is 50 bytes and can be copied into a token and a record.
  std::uint8_t uicr[(kUicrWordCount + 7) / 8] = {};
  std::uint16_t uicr_word_count = 0;

  // The i-th UICR register the file gives, lowest address first; 0 past the last.
  std::uint32_t uicrWordAt(std::uint16_t i) const;
};

// How many runs of addresses this firmware reads a base within. A base's are the MBR, the
// SoftDevice, its information structure, the bootloader and the UICR words: five. Thirty-two is
// room for a file written with gaps, and a bound a base cannot need.
constexpr std::size_t kMaxRuns = 32;

// How many times a file may give an address a byte it has already given before this firmware
// stops looking. Each one costs a walk of the file (the byte may be the same one, which the
// reference allows); a base gives every address once.
constexpr std::size_t kMaxRepeats = 8;

// The addresses a base covers, merged: [begin, end) each, in address order, never touching. Flash
// comes before UICR because 0x000FFFFF < 0x10001000, which is what makes "flash first, then UICR"
// (BaseWords below) the same thing as address order.
struct BaseRuns {
  // 64 bits because an Intel HEX record may name an address above 0x100000000 (a 04 record of
  // 0xFFFF and an offset), which rule 2 then refuses by name: the arithmetic is the reference's
  // and does not wrap.
  struct Run {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
  };
  Run at[kMaxRuns];
  std::size_t count = 0;

  bool holds(std::uint64_t address) const;
  // The lowest covered address at or above `address`, when there is one.
  bool from(std::uint64_t address, std::uint64_t* out) const;
  // How many covered bytes lie below `limit`: the summary's flash byte count, with kFlashEnd.
  std::uint32_t bytesBelow(std::uint64_t limit) const;
};

// How long a refusal's sentence may be. The longest of §4a's is 119 characters ("0x00026000 lies
// between the SoftDevice and the bootloader: ..."); these are the reference's words, not a format
// a caller chooses.
constexpr std::size_t kMaxBaseWhy = 160;

struct BaseResult {
  BaseReason reason = BaseReason::kNone;
  // The reference's sentence, with the line, address or value it names in it. Empty when
  // accepted. A buffer and not static text because the numbers are the file's: core/image/ holds
  // no formatting, so base.cpp writes the decimal and the `0x%08X` itself.
  char why[kMaxBaseWhy] = {};
  BaseSummary summary;
  BaseRuns runs;
  bool ok() const { return reason == BaseReason::kNone; }
};

// -- the records ---------------------------------------------------------------------------------

// One Intel HEX data record, as rule 1 reads it.
struct HexRecord {
  std::uint32_t line = 0;
  std::uint64_t address = 0;  // the absolute address of data[0] (the 02/04 base applied)
  std::uint8_t length = 0;
  std::uint8_t data[255] = {};
};

// A cursor over a `provision` file: bytes in chunks, lines, hex digits, one record at a time -
// rule 1 and nothing else. Used by the reader, by the repeat check and by BaseWords, so that
// there is one Intel HEX parser in this firmware.
class HexRecords {
 public:
  void begin(const flasher::ImageSource& file);
  // The next **data** record (type 00). False at the end of the file or on a refusal; the
  // records that are not data - 01, 02, 03, 04, 05 - are applied or ignored on the way.
  bool take(HexRecord* out);
  // Reads whatever is left of the file, for the one rule that is about the whole of it: a byte
  // above 0x7F anywhere means the file is not ASCII, and that refusal comes before any line's
  // (the reference decodes the whole file before it parses a record).
  void drain();
  BaseReason reason() const { return reason_; }
  const char* why() const { return why_; }
  bool sawEnd() const { return ended_; }
  bool sawData() const { return any_data_; }
  // A byte above 0x7F was read: the file is not ASCII, which refuses it before any line does.
  bool nonAscii() const { return non_ascii_; }

 private:
  bool byte(std::uint8_t* out);  // the next byte of the file, or false at its end
  void refuse(BaseReason reason, const char* before, std::uint64_t number, const char* after);

  const flasher::ImageSource* file_ = nullptr;
  std::uint32_t at_ = 0;    // the next file offset to read into buffer_
  std::uint32_t have_ = 0;  // bytes in buffer_
  std::uint32_t next_ = 0;  // the next of them
  std::uint8_t buffer_[256] = {};
  std::uint32_t line_ = 1;
  std::uint64_t base_ = 0;
  bool ended_ = false;
  bool any_data_ = false;
  bool non_ascii_ = false;
  BaseReason reason_ = BaseReason::kNone;
  char why_[kMaxBaseWhy] = {};
};

// -- reading a base ------------------------------------------------------------------------------

// The seven rules of §4a over a file, against the application region they are judged beside.
// One BaseReader is ~1.5 KB: it may sit in a static or on a task's stack, and keeps nothing of a
// file between calls.
class BaseReader {
 public:
  // `app_start` and `app_end` are the region the rules are applied against - check 5p
  // (spec/FIRMWARE-MANIFEST.md §5) gives this firmware's own board region, which a manifest
  // cannot widen.
  BaseResult read(const flasher::ImageSource& file, std::uint32_t app_start,
                  std::uint32_t app_end);

 private:
  // One of the six words §4a reads out of the file, as the bytes the file gives of it.
  struct Watched {
    std::uint32_t address = 0;
    std::uint8_t value[4] = {};
    std::uint8_t given = 0;  // a bit per byte
    bool whole() const { return given == 0x0F; }
    std::uint32_t word() const;
  };

  bool addRun(std::uint64_t begin, std::uint64_t end);
  // Rule 1's last clause, for a record covering addresses the file has already given: were any of
  // them given a *different* byte? The file is walked again, up to this record's line.
  bool repeatDiffers(const flasher::ImageSource& file, const HexRecord& record,
                     std::uint64_t* address);

  BaseRuns runs_;
  Watched watched_[6];
  HexRecords records_;
  HexRecords earlier_;  // the walk the repeat check makes, when there is one
  // What the file gave at a repeated record's addresses before it: members and not locals, so
  // that a rare path does not put half a kilobyte on a task's stack.
  std::uint8_t repeat_value_[255] = {};
  std::uint8_t repeat_given_[255] = {};
};

// -- the base's words, in address order ----------------------------------------------------------
//
// What task p5-03 writes: the file's data as (address, word) in address order - flash first, then
// UICR, which is what address order is - so that the command does not parse the file a second
// time to decide what to write.
//
// **A word the file gives none of is never written. A word it gives part of is written with the
// rest erased (0xFF).** Erased flash reads 0xFF and the nRF52's NVMC can only clear bits, so a
// word written from a partial record with zeros in the gaps would leave those bytes unwritable
// afterwards; 0xFF leaves them as the erase left them. The same goes for a UICR register.
//
// One pass over the file fills one 4 KB window - the nRF52840's page - and the words of that
// window are then handed out; the next covered window follows. A 192 KB base is 49 windows and so
// 49 passes over the file, which at 450 KB a pass is ~22 MB read from the Walter's own flash:
// nothing beside the minutes the same base takes to write over a 125 kHz debug port.
//
// One BaseWords is ~6 KB and belongs in static storage, not on a task's stack.
class BaseWords {
 public:
  // The file and the runs an accepting BaseReader::read() of **that same file** gave.
  void begin(const flasher::ImageSource& file, const BaseRuns& runs);
  // The next word and where it goes. False at the end of the base, or when the file could not be
  // read or stopped being the one that was judged (failed()).
  bool take(std::uint64_t* address, std::uint32_t* word);
  bool failed() const { return failed_; }

 private:
  bool fill(std::uint64_t window);

  const flasher::ImageSource* file_ = nullptr;
  BaseRuns runs_;
  HexRecords records_;
  std::uint64_t window_ = 0;  // the 4 KB window page_ holds
  bool have_window_ = false;
  std::uint64_t cursor_ = 0;  // the next address to look at
  bool failed_ = false;
  std::uint8_t page_[kBasePageSize] = {};
  std::uint8_t given_[kBasePageSize / 8] = {};
};

}  // namespace image
}  // namespace core
