#include "core/update/journal.h"

#include "core/image/descriptor.h"  // crc32: the same CRC-32 as the descriptor's and the store's

namespace core {
namespace update {

namespace {

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t get16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

void put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}

}  // namespace

const char* describe(JournalError error) {
  switch (error) {
    case JournalError::kNone: return "ok";
    case JournalError::kNotBegun: return "the journal has not been opened";
    case JournalError::kRegion: return "the journal's region is too small, or its sector is not a multiple of an entry";
    case JournalError::kFlashRead: return "the journal's flash could not be read";
    case JournalError::kFlashWrite: return "the journal's flash could not be written";
    case JournalError::kFlashErase: return "the journal's flash could not be erased";
    case JournalError::kTooLarge: return "the entry's payload is larger than an entry holds";
    case JournalError::kFull: return "the journal is full: wrapping would erase the operation in progress";
    case JournalError::kBadKind: return "an entry needs a kind";
  }
  return "?";
}

// One slot read and judged. `used` is "any bit cleared": a torn entry is used and not valid, and
// the next append goes past it.
bool Journal::readSlot(std::uint32_t sector, std::uint32_t slot, Entry* out, bool* used) const {
  std::uint8_t raw[kEntryBytes];
  if (!region_.read(offsetOf(sector, slot), raw, kEntryBytes)) {
    *used = true;  // unreadable is not blank; treated as a torn slot
    return false;
  }
  bool any = false;
  for (std::uint32_t i = 0; i < kEntryBytes; ++i) {
    if (raw[i] != 0xFF) {
      any = true;
      break;
    }
  }
  *used = any;
  if (!any) return false;
  if (get32(raw + kMagicOffset) != kJournalMagic) return false;
  if (get32(raw + kCrcOffset) != image::crc32(raw, kCrcOffset)) return false;
  const std::uint32_t seq = get32(raw);
  const std::uint16_t kind = get16(raw + 4);
  const std::uint8_t size = raw[6];
  if (seq == 0 || seq == 0xFFFFFFFFu || kind == kind::kNone || size > kPayloadMax) return false;
  if (out != nullptr) {
    out->seq = seq;
    out->kind = kind;
    out->size = size;
    for (std::size_t i = 0; i < kPayloadMax; ++i) out->payload[i] = raw[8 + i];
  }
  return true;
}

JournalError Journal::begin() {
  begun_ = false;
  sector_bytes_ = region_.sectorSize();
  if (sector_bytes_ < kEntryBytes || sector_bytes_ % kEntryBytes != 0) return JournalError::kRegion;
  sectors_ = region_.size() / sector_bytes_;
  if (sectors_ < 2) return JournalError::kRegion;
  if (sectors_ > kMaxSectors) sectors_ = kMaxSectors;  // a larger partition: the first 16 sectors
  slots_per_sector_ = sector_bytes_ / kEntryBytes;

  have_last_ = false;
  last_ = Entry{};
  valid_ = 0;
  std::uint32_t used_in[kMaxSectors] = {};  // the last used slot + 1 of each sector
  for (std::uint32_t s = 0; s < sectors_; ++s) {
    sector_min_seq_[s] = 0;
    sector_max_seq_[s] = 0;
    sector_valid_[s] = 0;
    for (std::uint32_t i = 0; i < slots_per_sector_; ++i) {
      Entry e;
      bool used = false;
      const bool ok = readSlot(s, i, &e, &used);
      if (used) used_in[s] = i + 1;
      if (!ok) continue;
      ++valid_;
      ++sector_valid_[s];
      if (sector_min_seq_[s] == 0 || e.seq < sector_min_seq_[s]) sector_min_seq_[s] = e.seq;
      if (e.seq > sector_max_seq_[s]) sector_max_seq_[s] = e.seq;
      if (!have_last_ || e.seq > last_.seq) {
        have_last_ = true;
        last_ = e;
        last_sector_ = s;
        last_slot_ = i;
      }
    }
  }
  next_seq_ = have_last_ ? last_.seq + 1 : 1;

  // The head: the first unused slot of the sector being written. With a newest valid entry it is
  // that entry's sector while it has room, and the next sector when it is full - where a torn
  // first entry may already sit. With no valid entry at all, the first sector that has anything
  // in it (a torn first entry), or the very start.
  if (have_last_) {
    if (used_in[last_sector_] < slots_per_sector_) {
      head_sector_ = last_sector_;
      head_slot_ = used_in[last_sector_];
    } else {
      const std::uint32_t next = (last_sector_ + 1) % sectors_;
      if (sector_max_seq_[next] != 0) {
        // The previous lap's entries: erased before the first write there (append).
        head_sector_ = next;
        head_slot_ = 0;
        erase_pending_ = true;
      } else {
        head_sector_ = next;
        head_slot_ = used_in[next];  // past any torn slot; 0 when blank
        erase_pending_ = false;
      }
    }
  } else {
    head_sector_ = 0;
    head_slot_ = 0;
    erase_pending_ = false;
    for (std::uint32_t s = 0; s < sectors_; ++s) {
      if (used_in[s] != 0) {
        head_sector_ = s;
        head_slot_ = used_in[s];
        break;
      }
    }
  }
  if (!erase_pending_ && head_slot_ == 0 && used_in[head_sector_] != 0 && sector_max_seq_[head_sector_] == 0) {
    // A sector full of torn slots and nothing valid: not a case a working firmware produces, but
    // the head cannot be in a used slot, so it moves on and the sector is erased before use.
    erase_pending_ = true;
  }
  for (std::uint32_t s = 0; s < sectors_; ++s) sector_used_[s] = used_in[s];
  protected_ = false;
  begun_ = true;
  return JournalError::kNone;
}

bool Journal::last(Entry* out) const {
  if (!begun_ || !have_last_) return false;
  if (out != nullptr) *out = last_;
  return true;
}

bool Journal::sectorHolds(std::uint32_t sector, std::uint32_t seq) const {
  return sector_max_seq_[sector] != 0 && sector_max_seq_[sector] >= seq;
}

bool Journal::protect(std::uint32_t seq) {
  if (!begun_ || seq == 0 || seq >= next_seq_) return false;
  // Gone already? The oldest seq the ring still holds.
  std::uint32_t oldest = 0;
  for (std::uint32_t s = 0; s < sectors_; ++s) {
    if (sector_min_seq_[s] != 0 && (oldest == 0 || sector_min_seq_[s] < oldest)) oldest = sector_min_seq_[s];
  }
  if (oldest == 0 || seq < oldest) return false;
  protected_ = true;
  protected_seq_ = seq;
  return true;
}

JournalError Journal::append(std::uint16_t kind, const void* payload, std::size_t size,
                             std::uint32_t* out_seq) {
  if (!begun_) return JournalError::kNotBegun;
  if (kind == kind::kNone) return JournalError::kBadKind;
  if (size > kPayloadMax || (size > 0 && payload == nullptr)) return JournalError::kTooLarge;

  // Room: the next sector when this one is full.
  if (head_slot_ >= slots_per_sector_) {
    head_sector_ = (head_sector_ + 1) % sectors_;
    head_slot_ = 0;
    erase_pending_ = sector_used_[head_sector_] != 0;
  }
  if (erase_pending_) {
    // The sector holds the previous lap. Not if the operation in progress began in it.
    if (protected_ && sectorHolds(head_sector_, protected_seq_)) return JournalError::kFull;
    if (!region_.erase(head_sector_ * sector_bytes_, sector_bytes_)) return JournalError::kFlashErase;
    ++erases_;
    // What that sector held is gone from the counts.
    const std::uint32_t lost = sector_valid_[head_sector_];
    valid_ = valid_ >= lost ? valid_ - lost : 0;
    sector_valid_[head_sector_] = 0;
    sector_min_seq_[head_sector_] = 0;
    sector_max_seq_[head_sector_] = 0;
    sector_used_[head_sector_] = 0;
    erase_pending_ = false;
  }

  std::uint8_t raw[kEntryBytes];
  for (std::uint32_t i = 0; i < kEntryBytes; ++i) raw[i] = 0;
  const std::uint32_t seq = next_seq_;
  put32(raw, seq);
  put16(raw + 4, kind);
  raw[6] = static_cast<std::uint8_t>(size);
  raw[7] = 0;
  const std::uint8_t* in = static_cast<const std::uint8_t*>(payload);
  for (std::size_t i = 0; i < size; ++i) raw[8 + i] = in[i];
  put32(raw + kCrcOffset, image::crc32(raw, kCrcOffset));
  put32(raw + kMagicOffset, kJournalMagic);

  const std::uint32_t at = offsetOf(head_sector_, head_slot_);
  // The slot is used from the first byte written, whatever happens next.
  sector_used_[head_sector_] = head_slot_ + 1;
  const std::uint32_t slot = head_slot_;
  ++head_slot_;
  // The body and its CRC first; the magic word last, as its own write, so that a power cut on
  // either side leaves a slot that does not validate.
  if (!region_.write(at, raw, kMagicOffset)) return JournalError::kFlashWrite;
  if (!region_.write(at + kMagicOffset, raw + kMagicOffset, 4)) return JournalError::kFlashWrite;

  ++next_seq_;
  have_last_ = true;
  last_.seq = seq;
  last_.kind = kind;
  last_.size = static_cast<std::uint8_t>(size);
  for (std::size_t i = 0; i < kPayloadMax; ++i) last_.payload[i] = raw[8 + i];
  last_sector_ = head_sector_;
  last_slot_ = slot;
  ++valid_;
  ++sector_valid_[head_sector_];
  if (sector_min_seq_[head_sector_] == 0) sector_min_seq_[head_sector_] = seq;
  sector_max_seq_[head_sector_] = seq;
  if (out_seq != nullptr) *out_seq = seq;
  return JournalError::kNone;
}

bool Journal::first(Cursor* cursor, Entry* out) const {
  if (!begun_ || !have_last_ || cursor == nullptr) return false;
  cursor->valid = true;
  cursor->sector = last_sector_;
  cursor->slot = last_slot_;
  cursor->steps = 0;
  cursor->seq = last_.seq;
  if (out != nullptr) *out = last_;
  return true;
}

bool Journal::previous(Cursor* cursor, Entry* out) const {
  if (!begun_ || cursor == nullptr || !cursor->valid) return false;
  const std::uint32_t total = sectors_ * slots_per_sector_;
  for (;;) {
    if (cursor->steps + 1 >= total) {
      cursor->valid = false;
      return false;
    }
    ++cursor->steps;
    if (cursor->slot == 0) {
      cursor->sector = cursor->sector == 0 ? sectors_ - 1 : cursor->sector - 1;
      cursor->slot = slots_per_sector_ - 1;
    } else {
      --cursor->slot;
    }
    Entry e;
    bool used = false;
    const bool ok = readSlot(cursor->sector, cursor->slot, &e, &used);
    if (!used) {
      cursor->valid = false;  // past the oldest: an unused slot
      return false;
    }
    if (!ok) continue;  // a torn slot: stepped over
    if (e.seq >= cursor->seq) {
      cursor->valid = false;  // not older: the ring has been walked round
      return false;
    }
    cursor->seq = e.seq;
    if (out != nullptr) *out = e;
    return true;
  }
}

bool Journal::lastOf(std::uint16_t kind, Entry* out) const {
  Cursor c;
  Entry e;
  if (!first(&c, &e)) return false;
  for (;;) {
    if (e.kind == kind) {
      if (out != nullptr) *out = e;
      return true;
    }
    if (!previous(&c, &e)) return false;
  }
}

}  // namespace update
}  // namespace core
