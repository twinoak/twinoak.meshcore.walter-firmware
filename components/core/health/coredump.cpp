#include "core/health/coredump.h"

namespace core {
namespace health {

namespace {

// The ELF32 numbers this reader uses. From the System V ABI's elf.h, which is also what
// $IDF_PATH/components/espcoredump/include_core_dump/elf.h is: a 52-byte file header whose
// program-header table is 32-byte entries, and a core file's type.
constexpr std::uint32_t kElfHeaderBytes = 52;
constexpr std::uint32_t kPhdrBytes = 32;
constexpr std::uint16_t kEtCore = 4;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtNote = 4;
constexpr std::uint16_t kPhnumMax = 32;  // this dump has six; a header claiming more is not read

// The note types of ESP-IDF v5.5.5's core_dump_elf.c, and the one the C library defines.
constexpr std::uint32_t kNtPrstatus = 1;          // one per task; `CORE` is its name
constexpr std::uint32_t kNoteCoreDumpInfo = 8266; // ELF_ESP_CORE_DUMP_INFO_TYPE
constexpr std::uint32_t kNoteExtraInfo = 677;     // ELF_ESP_CORE_DUMP_EXTRA_INFO_TYPE
constexpr std::uint32_t kNotePanicDetails = 679;  // ELF_ESP_CORE_DUMP_PANIC_DETAILS_TYPE
// `pr_pid` in xtensa_pr_status_t: si_signo, si_code, si_errno (12), pr_cursig, pr_pad0 (4),
// pr_sigpend, pr_sighold (8). core_dump_port.c puts the task's TCB address there.
constexpr std::uint32_t kPrstatusPidOffset = 24;
// A note's descriptor longer than this is not one of ours and is stepped over whole.
constexpr std::uint32_t kNoteDescMax = 4096;

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t get16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(p[0]) |
                                    (static_cast<std::uint32_t>(p[1]) << 8));
}

// Up to the next multiple of four, as an ELF note's name and descriptor are padded.
std::uint32_t align4(std::uint32_t n) { return (n + 3u) & ~3u; }

// CRC-32 as in IEEE 802.3 and zlib - the same polynomial core/image/descriptor.h's crc32() uses,
// here one byte at a time so that a 64 KB partition can be summed through a 128-byte window.
// ESP-IDF's checksum over a core dump is `esp_rom_crc32_le` fed every byte written
// (core_dump_checksum.h, CONFIG_ESP_COREDUMP_CHECKSUM_CRC32), which is this.
std::uint32_t crcStart() { return 0xFFFFFFFFu; }

std::uint32_t crcFeed(std::uint32_t crc, const std::uint8_t* data, std::uint32_t length) {
  for (std::uint32_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

bool printableName(const char* text) {
  if (text[0] == 0) return false;
  for (std::size_t i = 0; i < kTaskNameMax; ++i) {
    if (text[i] == 0) return true;
    if (text[i] < 0x20 || text[i] > 0x7E) return false;
  }
  return false;  // nothing terminated it inside configMAX_TASK_NAME_LEN
}

int nibbleOf(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// One read's working state, on that call's stack: see the header on why none of it is a member.
struct CoreDump::Reading {
  static constexpr std::size_t kLoadsMax = 16;
  struct Load {
    std::uint32_t vaddr = 0;
    std::uint32_t offset = 0;  // in the partition, not in the ELF
    std::uint32_t size = 0;
  };

  std::uint32_t end = 0;  // one past the last byte of the dump, once the header is read
  Load loads[kLoadsMax];
  std::size_t load_count = 0;
  std::uint32_t tcbs[kDumpTasksMax] = {};
  std::uint32_t crashed_tcb = 0;
  std::uint8_t buf[128] = {};
};

const char* describe(DumpState state) {
  switch (state) {
    case DumpState::kNoRegion: return "this build has no coredump partition";
    case DumpState::kErased: return "the partition is erased: nothing has panicked since it was last erased";
    case DumpState::kUnreadable: return "the partition could not be read";
    case DumpState::kNotADump: return "what is there is not a core dump this firmware can read";
    case DumpState::kTruncated: return "the dump says it is longer than the partition holds: it is truncated";
    case DumpState::kBadChecksum: return "the dump's checksum does not hold: nothing in it is trusted";
    case DumpState::kPresent: return "a core dump whose checksum holds";
  }
  return "?";
}

bool elfShaMatches(const std::uint8_t sha[32], const char* hex) {
  if (sha == nullptr || hex == nullptr || hex[0] == 0) return false;
  std::size_t i = 0;
  for (; hex[i] != 0; ++i) {
    if (i >= 2 * 32) return false;  // longer than a SHA-256 in hex: not a prefix of anything
    const int want = nibbleOf(hex[i]);
    if (want < 0) return false;
    const std::uint8_t byte = sha[i / 2];
    const int got = (i % 2) == 0 ? (byte >> 4) : (byte & 0x0F);
    if (got != want) return false;
  }
  return i > 0;
}

// -- the partition -------------------------------------------------------------------------------

bool CoreDump::at(const Reading& r, std::uint32_t offset, void* out, std::uint32_t count) const {
  if (count == 0 || offset > r.end || count > r.end - offset) return false;
  return region_.read(offset, out, count);
}

bool CoreDump::checksumOf(Reading& r, std::uint32_t length, std::uint32_t* out) const {
  std::uint32_t crc = crcStart();
  for (std::uint32_t done = 0; done < length;) {
    const std::uint32_t left = length - done;
    const std::uint32_t n = left < sizeof r.buf ? left : static_cast<std::uint32_t>(sizeof r.buf);
    if (!region_.read(done, r.buf, n)) return false;
    crc = crcFeed(crc, r.buf, n);
    done += n;
  }
  *out = crc ^ 0xFFFFFFFFu;
  return true;
}

bool CoreDump::read(Dump* out) {
  if (out == nullptr) return false;
  *out = Dump{};
  Reading r;

  const std::uint32_t region = region_.size();
  if (region < kDumpHeaderBytes + kDumpChecksumBytes) {
    out->state = DumpState::kNoRegion;
    return false;
  }
  std::uint8_t header[kDumpHeaderBytes];
  if (!region_.read(0, header, sizeof header)) {
    out->state = DumpState::kUnreadable;
    return false;
  }
  const std::uint32_t data_len = get32(header + 0);
  const std::uint32_t version = get32(header + 4);
  // An erased sector reads 0xFF: the length and the version are both all ones and there is no
  // dump. ESP-IDF's own reader treats the same bytes the same way (esp_core_dump_image_check).
  if (data_len == 0xFFFFFFFFu && version == 0xFFFFFFFFu) {
    out->state = DumpState::kErased;
    return false;
  }
  out->bytes = data_len;
  out->version = version;
  out->chip = static_cast<std::uint8_t>(version >> 16);
  out->format = static_cast<std::uint8_t>((version >> 8) & 0xFF);
  out->minor = static_cast<std::uint8_t>(version & 0xFF);
  out->revision = get32(header + 20);
  if (data_len <= kDumpHeaderBytes + kDumpChecksumBytes + kElfHeaderBytes) {
    out->state = DumpState::kNotADump;
    return false;
  }
  if (data_len > region) {
    out->state = DumpState::kTruncated;
    return false;
  }
  if (out->format != kDumpFormatElf || out->minor != kDumpFormatCrc32Minor) {
    // A binary dump, or an ELF one summed with SHA-256: both are real dumps and neither is what
    // this build writes, so nothing is read out of them rather than read wrongly.
    out->state = DumpState::kNotADump;
    return false;
  }

  // The checksum, over every byte before it, this header included. Nothing is read out of the
  // dump until it holds: a half-written dump (the power went while the panic handler was writing)
  // is exactly the thing that would otherwise be reported as a panic line.
  std::uint8_t stored[4];
  if (!region_.read(data_len - kDumpChecksumBytes, stored, sizeof stored)) {
    out->state = DumpState::kUnreadable;
    return false;
  }
  std::uint32_t want = 0;
  if (!checksumOf(r, data_len - kDumpChecksumBytes, &want)) {
    out->state = DumpState::kUnreadable;
    return false;
  }
  if (want != get32(stored)) {
    out->state = DumpState::kBadChecksum;
    return false;
  }

  out->state = DumpState::kPresent;
  r.end = data_len - kDumpChecksumBytes;
  readElf(r, kDumpHeaderBytes, r.end - kDumpHeaderBytes, out);
  return true;
}

bool CoreDump::erase() {
  const std::uint32_t sector = region_.sectorSize();
  if (sector == 0 || region_.size() < sector) return false;
  return region_.erase(0, region_.size() - (region_.size() % sector));
}

// -- the ELF -------------------------------------------------------------------------------------

void CoreDump::readElf(Reading& r, std::uint32_t elf, std::uint32_t size, Dump* out) const {
  if (size < kElfHeaderBytes) return;
  std::uint8_t head[kElfHeaderBytes];
  if (!at(r, elf, head, sizeof head)) return;
  const bool magic = head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
  const bool thirty_two = head[4] == 1;  // ELFCLASS32
  const bool little = head[5] == 1;      // ELFDATA2LSB
  if (!magic || !thirty_two || !little) return;
  if (get16(head + 16) != kEtCore) return;
  const std::uint32_t phoff = get32(head + 28);
  const std::uint16_t phentsize = get16(head + 42);
  const std::uint16_t phnum = get16(head + 44);
  if (phentsize != kPhdrBytes || phnum == 0 || phnum > kPhnumMax) return;

  // The loaded segments first: a task's name is in the TCB one of them holds, and the notes that
  // name the TCBs come before them in the file.
  std::uint8_t phdr[kPhdrBytes];
  for (std::uint16_t i = 0; i < phnum; ++i) {
    if (!at(r, elf + phoff + static_cast<std::uint32_t>(i) * kPhdrBytes, phdr, sizeof phdr)) return;
    if (get32(phdr + 0) != kPtLoad || r.load_count >= Reading::kLoadsMax) continue;
    Reading::Load& load = r.loads[r.load_count++];
    load.offset = elf + get32(phdr + 4);
    load.vaddr = get32(phdr + 8);
    load.size = get32(phdr + 16);
  }
  for (std::uint16_t i = 0; i < phnum; ++i) {
    if (!at(r, elf + phoff + static_cast<std::uint32_t>(i) * kPhdrBytes, phdr, sizeof phdr)) return;
    if (get32(phdr + 0) != kPtNote) continue;
    readNotes(r, elf + get32(phdr + 4), get32(phdr + 16), out);
  }
  // The names, now that both halves are known.
  const std::size_t named = out->tasks < kDumpTasksMax ? out->tasks : kDumpTasksMax;
  for (std::size_t i = 0; i < named; ++i) {
    if (nameOfTcb(r, r.tcbs[i], out->names[i])) out->named = static_cast<std::uint8_t>(i + 1);
    if (r.crashed_tcb != 0 && r.tcbs[i] == r.crashed_tcb) out->crashed = static_cast<std::int8_t>(i);
  }
  // A crashed TCB none of the kept tasks has says nothing: better no name than the wrong one.
  if (out->crashed >= 0 && out->names[out->crashed][0] == 0) out->crashed = -1;
}

void CoreDump::readNotes(Reading& r, std::uint32_t at_offset, std::uint32_t size, Dump* out) const {
  std::uint32_t p = at_offset;
  const std::uint32_t last = at_offset + size;
  // Bounded by the segment and by a step that always moves: a note of zero length would
  // otherwise be read for ever.
  while (p + 12 <= last) {
    std::uint8_t note[12];
    if (!at(r, p, note, sizeof note)) return;
    const std::uint32_t namesz = get32(note + 0);
    const std::uint32_t descsz = get32(note + 4);
    const std::uint32_t type = get32(note + 8);
    if (namesz > 64 || descsz > kNoteDescMax) return;  // not a note of ESP-IDF's making
    const std::uint32_t desc = p + 12 + align4(namesz);
    const std::uint32_t next = desc + align4(descsz);
    if (next <= p || next > last) return;
    if (type == kNotePanicDetails && descsz > 0 && out->panic[0] == 0) {
      // ESP-IDF writes the line with `strlen(panic_details)` and no terminator (core_dump_elf.c's
      // elf_add_note), so the descriptor's length *is* the line's length.
      const std::uint32_t n = descsz < kPanicMax ? descsz : static_cast<std::uint32_t>(kPanicMax);
      if (at(r, desc, out->panic, n)) {
        out->panic[n] = 0;
        std::size_t length = 0;
        // Whatever is in it, it goes out as one line: a control byte would break the console's.
        for (; length < kPanicMax && out->panic[length] != 0; ++length) {
          if (out->panic[length] < 0x20 || out->panic[length] > 0x7E) out->panic[length] = ' ';
        }
        out->panic_cut = length == kPanicMax && descsz > kPanicMax;
      }
    } else if (type == kNoteCoreDumpInfo && descsz > 4) {
      // The version word, then app_elf_sha256 as a string (ELF_APP_SHA256_SIZE bytes).
      const std::uint32_t have = descsz - 4;
      const std::uint32_t n = have < kElfShaMax ? have : static_cast<std::uint32_t>(kElfShaMax);
      if (at(r, desc + 4, out->elf_sha256, n)) {
        out->elf_sha256[n] = 0;
        for (std::size_t i = 0; i < kElfShaMax && out->elf_sha256[i] != 0; ++i) {
          // Hex and nothing else: the field is a string the image's build put there, and a byte
          // that is not a hex digit would make elfShaMatches() answer about rubbish.
          if (nibbleOf(out->elf_sha256[i]) < 0) {
            out->elf_sha256[i] = 0;
            break;
          }
        }
      }
    } else if (type == kNoteExtraInfo && descsz >= 4) {
      std::uint8_t tcb[4];
      if (at(r, desc, tcb, sizeof tcb)) r.crashed_tcb = get32(tcb);
    } else if (type == kNtPrstatus && descsz >= kPrstatusPidOffset + 4 && isCore(r, p, namesz)) {
      std::uint8_t pid[4];
      if (at(r, desc + kPrstatusPidOffset, pid, sizeof pid)) {
        if (out->tasks < kDumpTasksMax) r.tcbs[out->tasks] = get32(pid);
        if (out->tasks < 0xFF) ++out->tasks;
      }
    }
    p = next;
  }
}

bool CoreDump::isCore(const Reading& r, std::uint32_t p, std::uint32_t namesz) const {
  if (namesz != 5) return false;
  char name[5] = {};
  if (!at(r, p + 12, name, 5)) return false;
  return name[0] == 'C' && name[1] == 'O' && name[2] == 'R' && name[3] == 'E' && name[4] == 0;
}

bool CoreDump::nameOfTcb(Reading& r, std::uint32_t address, char* out) const {
  out[0] = 0;
  if (address == 0) return false;
  for (std::size_t i = 0; i < r.load_count; ++i) {
    const Reading::Load& load = r.loads[i];
    if (address < load.vaddr) continue;
    const std::uint32_t into = address - load.vaddr;
    if (into > load.size || load.size - into < kTcbNameOffset + kTaskNameMax + 1) continue;
    if (!at(r, load.offset + into + kTcbNameOffset, out, kTaskNameMax + 1)) continue;
    out[kTaskNameMax] = 0;
    if (printableName(out)) return true;
    out[0] = 0;  // the offset is a layout, not an API: what is not a name is not reported as one
    return false;
  }
  return false;
}

}  // namespace health
}  // namespace core
