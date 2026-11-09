// The `coredump` partition, read (task p11-03): what the panic handler left behind the last time
// this board died, in code that runs on the host over the real thing.
//
// Why this exists. On 9 November 2026 LTE Exit Node 1 fetched 0.1.3 over LTE, switched to it, and
// the bootloader came back to 0.1.2 by itself. All the unit could say was *the image never reached
// its main loop (the bootloader gave up on it)*. The reason had been on the board the whole time:
// the old image booted with `esp_reset_reason()` = panic, and the `coredump` partition held the
// trial's panic line, `assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)`.
// Reading it took a person at the bench with `esptool read_flash`. This reader is so that the unit
// reports it instead (tasks/phase-11/evidence/README.md).
//
// -- what is in the partition -------------------------------------------------------------------
//
// `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` with `..._DATA_FORMAT_ELF` and `..._CHECKSUM_CRC32`
// (sdkconfig.defaults), so the partition holds, from its first byte:
//
//   0   data_len      u32  the whole thing: this header, the ELF, its padding and the checksum
//   4   version       u32  (chip id << 16) | (format major << 8) | minor; ELF+CRC32 is 1.2
//   8   tasks_num     u32  0 in the ELF format (the tasks are ELF notes and segments)
//  12   tcb_sz        u32  0, likewise
//  16   mem_segs_num  u32  0, likewise
//  20   chip_rev      u32  the chip revision the dump was written on
//  24   ...                a whole ELF core file, `data_len - 24 - 4` bytes with its padding
//       crc32         u32  at `data_len - 4`: CRC-32 over bytes 0 .. data_len-5, header included
//
// (`core_dump_header_t` in `$IDF_PATH/components/espcoredump/include_core_dump/
// esp_core_dump_types.h` and `COREDUMP_VERSION_*` beside it; the checksum is `core_dump_crc_ctx_t`
// fed every byte written, ESP-IDF v5.5.5.) An erased partition reads 0xFF everywhere, which is how
// "there has never been a panic" is told from "there was one".
//
// What is read out of the ELF, and where it comes from (`$IDF_PATH/components/espcoredump/src/
// core_dump_elf.c`, v5.5.5):
//
//   - **the panic line**: the `ESP_PANIC_DETAILS` note (`ELF_ESP_CORE_DUMP_PANIC_DETAILS_TYPE`,
//     679) - the text the panic handler would have printed over the port, which a unit on a mast
//     has nobody to print to;
//   - **which image wrote it**: the `ESP_CORE_DUMP_INFO` note (8266): the dump's version word and
//     then `app_elf_sha256`, the string `esp_app_get_elf_sha256()` returns -
//     `CONFIG_APP_RETRIEVE_LEN_ELF_SHA` hex characters of the SHA-256 of the ELF the image was
//     linked from, 9 in an ESP-IDF default build. The same SHA-256 is in every image's own
//     application description (`esp_app_desc_t.app_elf_sha256`, 32 raw bytes), which is what
//     `interfaces::OwnSlot` carries - so a dump can be attributed to a slot, and `elfShaMatches`
//     below is that comparison;
//   - **the tasks**: one `CORE`/`NT_PRSTATUS` note each, whose `pr_pid` is the task's TCB address
//     (`core_dump_port.c`, `xtensa_pr_status_t` and
//     `s_reg_dump.pr_status.pr_pid = (uint32_t)task->tcb_addr`), and one PT_LOAD segment per TCB.
//     The name is `pcTaskName` at **offset 52** of the TCB: `pxTopOfStack` (4), `xStateListItem`
//     (20), `xEventListItem` (20), `uxPriority` (4), `pxStack` (4), then the name
//     (`$IDF_PATH/components/freertos/FreeRTOS-Kernel/tasks.c`'s `tskTaskControlBlock` with
//     `portUSING_MPU_WRAPPERS` 0 on Xtensa). The offset is a layout and not an API, so it is
//     **checked** rather than trusted: a name that is not printable ASCII terminated inside
//     `configMAX_TASK_NAME_LEN` is reported as unknown, not as whatever those bytes were;
//   - **which task crashed**: the `ESP_EXTRA_INFO` note (677) begins with
//     `xtensa_extra_info_t.crashed_task_tcb` (`core_dump_port.c`), matched against the TCB
//     addresses above.
//
// -- what this is not --------------------------------------------------------------------------
//
// It is not a replacement for `esp-coredump` on a laptop: there are no registers here, no stacks
// and no backtrace, because none of those can be read without the image's own ELF and its DWARF.
// What it reads is the four things a unit on a mast can report over LTE and a person can act on:
// is there a dump, does its checksum hold, whose image wrote it, and what it says killed it.
//
// No ESP-IDF include, no heap, **and no static storage**: one CoreDump is a reference to a region
// and nothing else. Everything `read()` needs while it works - its window on the flash, the
// segments it found, the TCB addresses - lives on its own stack, and the Dump it fills is the
// caller's. That is not tidiness: 370 bytes of `.bss` here were enough to move the 20 480-byte
// kept-image scratch out of one heap and into the one the main task's stack comes from, which is
// exactly how v0.1.3 came not to start (docs/ARCHITECTURE.md §14, rail R22). The build said so.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/flash_region.h"

namespace core {
namespace health {

// The header, and the checksum after the ELF.
constexpr std::uint32_t kDumpHeaderBytes = 24;
constexpr std::uint32_t kDumpChecksumBytes = 4;
// The version word's two halves that matter: the ELF format with a CRC-32 is major 1, minor 2
// (COREDUMP_VERSION_ELF, and COREDUMP_VERSION_ELF_CRC32's minor). The chip id is the top 16 bits
// and is reported, not judged - a dump is read on the chip that wrote it.
constexpr std::uint8_t kDumpFormatElf = 1;
constexpr std::uint8_t kDumpFormatCrc32Minor = 2;

// The bounds. Every text this reader produces is cut to fit one of these and says it was cut.
constexpr std::size_t kPanicMax = 95;     // characters; ESP-IDF's own panic lines are under 80
constexpr std::size_t kTaskNameMax = 15;  // configMAX_TASK_NAME_LEN - 1 (CONFIG_FREERTOS_MAX_TASK_NAME_LEN 16)
constexpr std::size_t kElfShaMax = 64;    // CONFIG_APP_RETRIEVE_LEN_ELF_SHA's largest, 9 by default
constexpr std::size_t kDumpTasksMax = 8;  // names kept; `tasks` counts them all

// Where pcTaskName is in a FreeRTOS TCB, and how far a name may run (see the header comment).
constexpr std::uint32_t kTcbNameOffset = 52;

// What the partition held.
enum class DumpState : std::uint8_t {
  kNoRegion = 0,  // this build was handed no coredump partition
  kErased,        // 0xFF where the header would be: nothing has ever panicked, or it was erased
  kUnreadable,    // the flash read failed
  kNotADump,      // a header this firmware does not know: its version, or a length that cannot be
  kTruncated,     // the header's own length is more than the partition holds
  kBadChecksum,   // the CRC-32 over the dump's bytes is not the one it carries: nothing is trusted
  kPresent,       // a dump whose checksum holds
};

const char* describe(DumpState state);

// One dump, as much of it as a unit can say. Plain data, a few hundred bytes, the caller's.
struct Dump {
  DumpState state = DumpState::kNoRegion;
  std::uint32_t bytes = 0;    // the header's data_len: the whole dump, checksum included
  std::uint32_t version = 0;  // the raw version word
  std::uint8_t chip = 0;      // version >> 16: CONFIG_IDF_FIRMWARE_CHIP_ID (9 is the ESP32-S3)
  std::uint8_t format = 0;    // the format's major: 1 is ELF
  std::uint8_t minor = 0;     // 2 is a CRC-32 checksum, 3 a SHA-256 one
  std::uint32_t revision = 0;  // chip_rev
  // `app_elf_sha256` as the dump carries it: hex, lower case, 9 characters in a default build.
  char elf_sha256[kElfShaMax + 1] = {};
  // The ESP_PANIC_DETAILS note. Empty when the dump has none (a dump written by something other
  // than the panic handler, e.g. `esp_core_dump_write` from an application).
  char panic[kPanicMax + 1] = {};
  bool panic_cut = false;  // the line was longer than kPanicMax
  std::uint8_t tasks = 0;  // how many tasks the dump holds (PRSTATUS notes), all of them
  std::uint8_t named = 0;  // how many of names[] are filled: min(tasks, kDumpTasksMax)
  char names[kDumpTasksMax][kTaskNameMax + 1] = {};
  // Which of names[] was running when it died, from ESP_EXTRA_INFO's crashed_task_tcb; -1 when
  // the dump does not say, or says a TCB none of the tasks has.
  std::int8_t crashed = -1;

  bool present() const { return state == DumpState::kPresent; }
  const char* crashedName() const {
    return crashed >= 0 && static_cast<std::size_t>(crashed) < kDumpTasksMax ? names[crashed] : "";
  }
};

// True when `hex` - the dump's `app_elf_sha256`, the first characters of the ELF SHA-256 in hex -
// is a prefix of `sha`, the 32 raw bytes an image's application description carries. An odd number
// of characters compares the high nibble of the last byte, which is what a 9-character field needs.
// False when `hex` is empty, holds anything that is not a hex digit, or is longer than 64
// characters: "the dump says nothing" is never "the dump matches".
bool elfShaMatches(const std::uint8_t sha[32], const char* hex);

class CoreDump {
 public:
  explicit CoreDump(interfaces::FlashRegion& region) : region_(region) {}

  // Reads the partition and fills `out` (never left half-filled: `state` says how far it got).
  // True only when a dump is there and its checksum holds.
  bool read(Dump* out);

  // Erases the whole partition. `coredump erase confirm` is the one caller: **nothing erases a
  // dump by itself**, because the one of 9 November was read only because nothing had.
  bool erase();

  std::uint32_t regionBytes() const { return region_.size(); }

 private:
  // What one read() is doing while it does it: the window on the flash, where the dump ends, the
  // loaded segments a TCB could be in, the TCB addresses the PRSTATUS notes gave and the one
  // ESP_EXTRA_INFO called the crashed task's. Defined in the source and built on read()'s stack.
  struct Reading;

  // `count` bytes at `offset` of the partition, refused when they leave the dump.
  bool at(const Reading& r, std::uint32_t offset, void* out, std::uint32_t count) const;
  // CRC-32 over bytes 0 .. `length`-1 of the partition, read in windows of the reading's buffer.
  bool checksumOf(Reading& r, std::uint32_t length, std::uint32_t* out) const;
  // The ELF, from `elf` (the header's size) for `size` bytes: its notes and its segments.
  void readElf(Reading& r, std::uint32_t elf, std::uint32_t size, Dump* out) const;
  // One PT_NOTE segment's notes.
  void readNotes(Reading& r, std::uint32_t at_offset, std::uint32_t size, Dump* out) const;
  // The name of the task whose TCB is at `address`, into `out` of kTaskNameMax + 1 bytes.
  // False when no loaded segment holds it, or when what is there is not a name.
  bool nameOfTcb(Reading& r, std::uint32_t address, char* out) const;
  // Whether the note whose header is at `p` is named `CORE`: NT_PRSTATUS is type 1, which is a
  // task's registers only in the C library's `CORE` note and not in somebody else's type 1.
  bool isCore(const Reading& r, std::uint32_t p, std::uint32_t namesz) const;

  interfaces::FlashRegion& region_;
};

}  // namespace health
}  // namespace core
