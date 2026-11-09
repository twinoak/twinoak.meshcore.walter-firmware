// The update journal (task p2-03): a small append-only log in its own flash partition - the
// `journal` partition of partitions.csv (docs/ARCHITECTURE.md §11: "a journal that is not the
// settings store: its own wear, its own failure modes") - behind interfaces::FlashRegion, so that
// the host tests run it over sim/sim_flash.h with the power going at every write.
//
// What it is for (spec/SYSTEM.md §6): "Every step is journalled in the Walter's flash before it is
// taken, so a Walter that resets in the middle resumes or rolls back; it never guesses." The
// radio's update (core/update/radio_update.h) writes an entry before every state it enters; the
// Walter's own update (task p2-04) and the supervisor (task p2-05) write theirs into the same log,
// each under its own kind.
//
// -- the shape ----------------------------------------------------------------------------------
//
// Entries are a fixed 64 bytes, so that the reader never has to trust a length it found in flash
// to know where the next entry begins - a torn entry is skipped by stepping over its slot, not by
// reading a size out of it:
//
//     0   seq        u32   1, 2, 3 ... never reused (4 billion entries is never)
//     4   kind       u16   whose entry this is (kind::* below)
//     6   size       u8    bytes of payload, 0..kPayloadMax
//     7   flags      u8    0
//     8   payload    48 B  the owner's bytes, zero-padded
//    56   crc32      u32   descriptor.h's CRC-32 over bytes 0..55
//    60   magic      u32   "TWJ1" - written LAST, as its own write
//
// An entry is valid only when its magic word is there and its CRC matches. The body (bytes 0..55
// and the CRC) is one write, the magic a second one after it, so whatever a power cut does - half
// the body, no magic, half the magic - leaves a slot that does not validate, and the entry before
// it governs. A slot whose 64 bytes are all 0xFF is unused; anything else is used (valid or torn)
// and the next append goes past it, because NOR flash cannot be written twice.
//
// -- the ring -----------------------------------------------------------------------------------
//
// The partition is a ring of sectors (16 of 4 KB: 1024 entries). Entries are appended slot after
// slot; when a sector is full the next one is erased and appending continues there, so the oldest
// sector is overwritten first. "It wraps without ever losing the entries of the update in
// progress": a caller marks the first entry of an operation (protect()), and an append that would
// have to erase the sector holding it is refused (kFull) rather than erasing it. An update writes
// a few dozen entries at most and a sector holds 64, so a full journal is a bug, not a day.
//
// Reading after a reset: every sector is scanned once (a 64-byte buffer, 1024 reads; no copy of
// the log is kept in RAM), the newest valid entry is the one with the highest seq, the head is the
// first unused slot of its sector - or the next sector when that one is full - and the entries
// can be walked backwards from the newest (Cursor), which is what "what was the last thing I was
// doing" and `rak update status` need.
//
// No ESP-IDF include, no heap: one Journal is a few words of state and one 64-byte buffer.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/flash_region.h"

namespace core {
namespace update {

constexpr std::uint32_t kEntryBytes = 64;
constexpr std::size_t kPayloadMax = 48;
constexpr std::uint32_t kJournalMagic = 0x314A5754;  // "TWJ1", little-endian bytes T W J 1
constexpr std::uint32_t kMagicOffset = 60;           // where the magic word sits in an entry
constexpr std::uint32_t kCrcOffset = 56;

// The `journal` partition: partitions.csv gives it 64 KB. Written down for the arithmetic below
// and checked against the region at begin(); a region of another size is laid out by its own
// size, so the number is not a dependency.
constexpr std::uint32_t kJournalPartitionBytes = 0x10000;
constexpr std::uint32_t kEntriesPerSector = 4096 / kEntryBytes;  // 64
static_assert(4096 % kEntryBytes == 0, "an entry must divide a sector");

// Whose entry it is. The payload's layout is the owner's; the journal copies bytes.
namespace kind {
constexpr std::uint16_t kNone = 0;
constexpr std::uint16_t kRadioUpdate = 0x0001;  // core/update/radio_update.h (task p2-03)
constexpr std::uint16_t kSelfUpdate = 0x0002;   // the Walter's own (task p2-04)
constexpr std::uint16_t kSupervisor = 0x0003;   // the ladder (task p2-05)
constexpr std::uint16_t kDutyCycle = 0x0004;    // sleep and the band (task p3-04, core/cycle/sleep.h)
constexpr std::uint16_t kWakeCycle = 0x0005;    // one wake's record (task p3-05, core/cycle/wake_cycle.h)
// The Walter's own attention, when it is news rather than a letter: a restart the agent library
// asked for and did not get (task p4-04, core/cycle/agent_attention.h).
constexpr std::uint16_t kAgentAttention = 0x0006;
// A firmware job of the agent that outlives a boot (task p4-05, components/agent/firmware_jobs.h):
// a `twinoak.fw.apply` of the Walter's own image, whose result is posted by the boot after the
// trial from what this entry and the self-update's say; and the end of any apply, which is what
// `twinoak.fw.state` reports as the slot's last update.
constexpr std::uint16_t kAgentJob = 0x0007;
// What ended a trial of the Walter's own image (task p11-03, core/update/self_update.h): the
// reset reason of the boot that found the rollback, and the panic line and crashed task of the
// core dump when the partition held the trial image's own. Its own kind rather than more bytes in
// the *rolled back* entry, whose 48 are full, and written immediately after it - so an evidence
// entry is the last trial's when no self-update entry is newer than it.
constexpr std::uint16_t kTrialEvidence = 0x0008;
}  // namespace kind

struct Entry {
  std::uint32_t seq = 0;
  std::uint16_t kind = kind::kNone;
  std::uint8_t size = 0;
  std::uint8_t payload[kPayloadMax] = {};
};

enum class JournalError : std::uint8_t {
  kNone = 0,
  kNotBegun,       // begin() has not succeeded
  kRegion,         // the region is smaller than two sectors, or its sector is not a multiple of 64
  kFlashRead,
  kFlashWrite,
  kFlashErase,
  kTooLarge,       // a payload over kPayloadMax
  kFull,           // appending would erase the sector holding a protected entry
  kBadKind,        // kind::kNone is not an entry
};

const char* describe(JournalError error);

// Where a backwards walk is: the sector and slot of the entry last returned.
struct Cursor {
  bool valid = false;
  std::uint32_t sector = 0;
  std::uint32_t slot = 0;
  std::uint32_t steps = 0;  // slots stepped over so far, bounded by the ring's size
  std::uint32_t seq = 0;    // the seq last returned: the next must be older
};

class Journal {
 public:
  explicit Journal(interfaces::FlashRegion& region) : region_(region) {}

  // Scans the ring: the newest valid entry, the head. Once, before anything else.
  JournalError begin();
  bool begun() const { return begun_; }

  // The newest valid entry, if there is one.
  bool last(Entry* out) const;
  bool empty() const { return !have_last_; }
  std::uint32_t nextSeq() const { return next_seq_; }

  // The newest valid entry of one kind, walking backwards; false when there is none.
  bool lastOf(std::uint16_t kind, Entry* out) const;

  // Appends one entry: body, then magic. On success `out_seq` (if given) is its seq.
  JournalError append(std::uint16_t kind, const void* payload, std::size_t size,
                      std::uint32_t* out_seq = nullptr);

  // The entries from `seq` onwards are an operation in progress and must not be erased by a wrap:
  // an append that would need to is refused with kFull. unprotect() lifts it. A seq older than
  // the ring holds protects nothing (it is already gone) and is reported false.
  bool protect(std::uint32_t seq);
  void unprotect() { protected_ = false; }
  bool isProtected() const { return protected_; }

  // Walking backwards from the newest entry: first() places the cursor on it, previous() moves
  // to the valid entry before it. Both false when there is none (or none left). Torn slots are
  // stepped over; unused slots end the walk.
  bool first(Cursor* cursor, Entry* out) const;
  bool previous(Cursor* cursor, Entry* out) const;

  // For a console line and the tests.
  std::uint32_t sectors() const { return sectors_; }
  std::uint32_t headSector() const { return head_sector_; }
  std::uint32_t headSlot() const { return head_slot_; }
  std::uint32_t erases() const { return erases_; }      // sectors erased by this object
  std::uint32_t validEntries() const { return valid_; }  // counted at begin(), then kept up

 private:
  bool readSlot(std::uint32_t sector, std::uint32_t slot, Entry* out, bool* used) const;
  std::uint32_t offsetOf(std::uint32_t sector, std::uint32_t slot) const {
    return sector * sector_bytes_ + slot * kEntryBytes;
  }
  bool sectorHolds(std::uint32_t sector, std::uint32_t seq) const;

  interfaces::FlashRegion& region_;
  bool begun_ = false;
  std::uint32_t sector_bytes_ = 4096;
  std::uint32_t sectors_ = 0;
  std::uint32_t slots_per_sector_ = kEntriesPerSector;
  std::uint32_t head_sector_ = 0;
  std::uint32_t head_slot_ = 0;
  std::uint32_t next_seq_ = 1;
  bool have_last_ = false;
  Entry last_;
  std::uint32_t last_sector_ = 0;
  std::uint32_t last_slot_ = 0;
  bool protected_ = false;
  std::uint32_t protected_seq_ = 0;
  std::uint32_t erases_ = 0;
  std::uint32_t valid_ = 0;
  // Per sector: the lowest and highest seq it holds (0 = no valid entry), how many valid entries,
  // and the last used slot + 1 (0 = blank). For protect(), the counts and the head.
  static constexpr std::uint32_t kMaxSectors = kJournalPartitionBytes / 4096;
  std::uint32_t sector_min_seq_[kMaxSectors] = {};
  std::uint32_t sector_max_seq_[kMaxSectors] = {};
  std::uint32_t sector_valid_[kMaxSectors] = {};
  std::uint32_t sector_used_[kMaxSectors] = {};
  bool erase_pending_ = false;  // the head sector holds the previous lap: erase before the first write
};

}  // namespace update
}  // namespace core
