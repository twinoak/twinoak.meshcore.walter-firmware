#include "core/image/store.h"

#include <cstring>

namespace core {
namespace image {

const char* name(Which which) {
  switch (which) {
    case Which::kGolden: return "golden";
    case Which::kCandidate: return "candidate";
    case Which::kScanner: return "scanner";
    case Which::kBase: return "base";
  }
  return "?";
}

bool whichFromName(const char* text, Which* out) {
  if (text == nullptr || out == nullptr) return false;
  for (std::uint8_t i = 0; i < kWhichCount; ++i) {
    const Which which = static_cast<Which>(i);
    if (std::strcmp(text, name(which)) == 0) {
      *out = which;
      return true;
    }
  }
  return false;
}

const char* describe(StoreError error) {
  switch (error) {
    case StoreError::kNone: return "ok";
    case StoreError::kRegionTooSmall:
      return "the image partition is smaller than the layout, or its sector is not 4 KB";
    case StoreError::kNoSuchSlot: return "the store has no area for that slot";
    case StoreError::kFlashRead: return "the flash could not be read";
    case StoreError::kFlashWrite: return "the flash could not be written";
    case StoreError::kFlashErase: return "the flash could not be erased";
    case StoreError::kTooLarge: return "the file is larger than the area it would go in holds";
    case StoreError::kEmptyFile: return "there is nothing to store";
    case StoreError::kWrongUse:
      return "this store holds a radio's swd images and the board's provision file, nothing else";
    case StoreError::kOnBadList:
      return "this image failed a health check once and is never tried again";
    case StoreError::kGoldenNotEmpty:
      return "the golden image is already there: it is replaced by a promotion, not by a load";
    case StoreError::kNeverGolden: return "the golden image is not erased";
    case StoreError::kNotAnImage:
      return "the base is not one of the three images: it has a door of its own";
    case StoreError::kEmpty: return "there is no image in that slot";
    case StoreError::kBusy: return "another write is open on the store";
    case StoreError::kNotWriting: return "no write is open on the store";
    case StoreError::kSizeMismatch: return "not the number of bytes the manifest promised";
    case StoreError::kHashMismatch:
      return "what was read back from flash is not the hash the manifest names";
    case StoreError::kTokenMismatch: return "the verdict's token is not for the file being written";
  }
  return "?";
}

namespace {

// -- the three records, as bytes ----------------------------------------------------------------
//
// Explicit little-endian serialisation and not a packed structure: these bytes outlive the
// firmware that wrote them, so their layout is written down here and not left to a compiler's
// idea of padding (core/image/descriptor.h takes the same line about §4's 96 bytes).

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// The record in an area's header sector (offsets from the start of the sector).
constexpr std::uint32_t kRecOffFormat = 0x04;
constexpr std::uint32_t kRecOffStoredAs = 0x05;
constexpr std::uint32_t kRecOffSlot = 0x06;
constexpr std::uint32_t kRecOffUse = 0x07;
constexpr std::uint32_t kRecOffSize = 0x08;
constexpr std::uint32_t kRecOffLoad = 0x0C;
constexpr std::uint32_t kRecOffSha = 0x10;
constexpr std::uint32_t kRecOffDescriptor = 0x30;
// A base has no descriptor (spec/FIRMWARE-MANIFEST.md §4a), so what §4a found in it goes in those
// same 96 bytes; the record's `use` byte says which of the two they are, and nothing reads them as
// the other (store.h says why they share the room).
constexpr std::uint32_t kRecOffBaseBootloader = kRecOffDescriptor + 0x00;
constexpr std::uint32_t kRecOffBaseFlashBytes = kRecOffDescriptor + 0x04;
constexpr std::uint32_t kRecOffBaseSdId = kRecOffDescriptor + 0x08;
constexpr std::uint32_t kRecOffBaseSdVersion = kRecOffDescriptor + 0x0C;
constexpr std::uint32_t kRecOffBaseMbrParams = kRecOffDescriptor + 0x10;
constexpr std::uint32_t kRecOffBaseHasMbrParams = kRecOffDescriptor + 0x14;
constexpr std::uint32_t kRecOffBaseUicrCount = kRecOffDescriptor + 0x16;
constexpr std::uint32_t kRecOffBaseUicrMap = kRecOffDescriptor + 0x18;
constexpr std::uint32_t kRecBaseUsed = 0x18 + sizeof(BaseSummary{}.uicr);
static_assert(kRecBaseUsed <= kDescriptorSize,
              "a base's summary must fit the room the descriptor has in a record");
constexpr std::uint32_t kRecOffVersion = 0x90;
constexpr std::uint32_t kRecOffCommit = 0xB0;  // 40 characters, a terminator and three pad bytes
constexpr std::uint32_t kRecOffStoredAt = 0xDC;
constexpr std::uint32_t kRecOffKeyId = 0xE0;
constexpr std::uint32_t kRecOffCrc = 0xE8;
constexpr std::uint32_t kRecUsed = 0xEC;
static_assert(kRecUsed <= kRecordBytes, "the record must fit its place in the header sector");
static_assert((kRecUsed - 4) % 4 == 0, "the body is written as whole words");

// A progress record (offsets from the start of one).
constexpr std::uint32_t kProgOffFormat = 0x04;
constexpr std::uint32_t kProgOffWhich = 0x05;
constexpr std::uint32_t kProgOffSlot = 0x06;
constexpr std::uint32_t kProgOffSize = 0x08;
constexpr std::uint32_t kProgOffCommitted = 0x0C;
constexpr std::uint32_t kProgOffSha = 0x10;
constexpr std::uint32_t kProgOffCrc = 0x30;
constexpr std::uint32_t kProgUsed = 0x34;
static_assert(kProgUsed <= kProgressBytes, "a progress record must fit its slot");
static_assert((kProgUsed - 4) % 4 == 0, "the body is written as whole words");

// The index record (offsets from the start of its sector).
constexpr std::uint32_t kIdxOffFormat = 0x04;
constexpr std::uint32_t kIdxOffSlots = 0x05;
constexpr std::uint32_t kIdxOffRoles = 0x06;
constexpr std::uint32_t kIdxOffBadCount = 0x07;
constexpr std::uint32_t kIdxOffSeq = 0x08;
constexpr std::uint32_t kIdxOffAreas = 0x0C;  // kRadioSlots * kPromotableCount bytes
constexpr std::uint32_t kIdxOffStale = kIdxOffAreas + kRadioSlots * kPromotableCount;
constexpr std::uint32_t kIdxOffBad = 0x18;
constexpr std::uint32_t kIdxOffCrc = kIdxOffBad + kBadListMax * kSha256Size;
constexpr std::uint32_t kIdxUsed = kIdxOffCrc + 4;
static_assert(kIdxOffStale + kRadioSlots <= kIdxOffBad, "the stale markers must fit before the bad list");
static_assert(kIdxUsed <= kSectorSize, "the index record must fit its sector");
static_assert((kIdxOffCrc - 4) % 4 == 0, "the body is written as whole words");

// The CRC-32 of descriptor.h (IEEE 802.3, as zlib), over a record's body: everything after the
// magic word and before the CRC field itself. One implementation of it in this firmware.
std::uint32_t bodyCrc(const std::uint8_t* record, std::uint32_t crc_offset) {
  return crc32(record + 4, crc_offset - 4);
}

constexpr std::uint8_t kNoArea = 0xFF;

}  // namespace

// -- StoredImage --------------------------------------------------------------------------------

bool StoredImage::read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const {
  if (region_ == nullptr || out == nullptr) return false;
  if (offset > size_ || count > size_ - offset) return false;
  if (count == 0) return true;
  if (!region_->read(base_ + offset, out, count)) return false;
  // The hash follows a reader that goes forward without a gap; anything else is not a pass over
  // the image and cannot say anything about its integrity.
  if (offset == next_) {
    hash_.update(out, count);
    next_ += count;
  } else {
    sequential_ = false;
  }
  return true;
}

bool StoredImage::hashOk() const {
  if (!sequential_ || next_ != size_) return false;
  Sha256 copy = hash_;  // finish() resets, and a reader may ask more than once
  std::uint8_t digest[kSha256Size];
  copy.finish(digest);
  return sha256Equal(digest, sha256_);
}

void StoredImage::rewind() {
  hash_.reset();
  next_ = 0;
  sequential_ = true;
}

// -- the index ----------------------------------------------------------------------------------

void Store::identity(Index* index) {
  *index = Index{};
  for (std::size_t s = 0; s < kRadioSlots; ++s) {
    for (std::size_t w = 0; w < kPromotableCount; ++w) {
      index->area_of[s][w] = static_cast<std::uint8_t>(w);
    }
    index->stale[s] = kNoArea;
  }
}

bool Store::loadIndex(Index* out, std::uint8_t* from_copy) const {
  bool found = false;
  for (std::uint8_t copy = 0; copy < kIndexCopies; ++copy) {
    std::uint8_t bytes[kIdxUsed];
    if (!region_.read(copy * kSectorSize, bytes, kIdxUsed)) continue;
    if (get32(bytes) != kIndexMagic) continue;
    if (bytes[kIdxOffFormat] != kFormatVersion) continue;
    if (bytes[kIdxOffSlots] != kRadioSlots || bytes[kIdxOffRoles] != kPromotableCount) continue;
    if (get32(bytes + kIdxOffCrc) != bodyCrc(bytes, kIdxOffCrc)) continue;
    const std::uint32_t seq = get32(bytes + kIdxOffSeq);
    if (found && seq <= out->seq) continue;

    Index index;
    identity(&index);
    index.seq = seq;
    bool sane = true;
    for (std::size_t s = 0; s < kRadioSlots; ++s) {
      bool used[kPromotableCount] = {};
      for (std::size_t w = 0; w < kPromotableCount; ++w) {
        const std::uint8_t area = bytes[kIdxOffAreas + s * kPromotableCount + w];
        // A permutation of the three image areas and nothing else: the base's area is not in the
        // index, and an index that pointed an image at it would point it at a smaller area.
        if (area >= kPromotableCount || used[area]) sane = false;
        else used[area] = true;
        index.area_of[s][w] = area;
      }
      const std::uint8_t stale = bytes[kIdxOffStale + s];
      if (stale != kNoArea && stale >= kPromotableCount) sane = false;
      index.stale[s] = stale;
    }
    if (!sane) continue;
    index.bad_count = bytes[kIdxOffBadCount];
    if (index.bad_count > kBadListMax) continue;
    std::memcpy(index.bad, bytes + kIdxOffBad, index.bad_count * kSha256Size);

    *out = index;
    *from_copy = copy;
    found = true;
  }
  return found;
}

StoreError Store::writeIndex(const Index& next) {
  const std::uint8_t target = index_copy_ == 0 ? 1 : 0;
  const std::uint32_t at = target * kSectorSize;

  std::uint8_t bytes[kIdxUsed] = {};
  bytes[kIdxOffFormat] = kFormatVersion;
  bytes[kIdxOffSlots] = static_cast<std::uint8_t>(kRadioSlots);
  bytes[kIdxOffRoles] = static_cast<std::uint8_t>(kPromotableCount);
  bytes[kIdxOffBadCount] = next.bad_count;
  put32(bytes + kIdxOffSeq, index_.seq + 1);
  for (std::size_t s = 0; s < kRadioSlots; ++s) {
    for (std::size_t w = 0; w < kPromotableCount; ++w) {
      bytes[kIdxOffAreas + s * kPromotableCount + w] = next.area_of[s][w];
    }
    bytes[kIdxOffStale + s] = next.stale[s];
  }
  std::memcpy(bytes + kIdxOffBad, next.bad, next.bad_count * kSha256Size);
  put32(bytes + kIdxOffCrc, bodyCrc(bytes, kIdxOffCrc));
  put32(bytes, kIndexMagic);  // in the buffer; it reaches the flash last, below

  // The older copy is erased and rewritten; the newer one is untouched throughout, so a power
  // cut anywhere here leaves the index as it was.
  if (!region_.erase(at, kSectorSize)) return StoreError::kFlashErase;
  if (!region_.write(at + 4, bytes + 4, kIdxOffCrc + 4 - 4)) return StoreError::kFlashWrite;
  if (!region_.write(at, bytes, 4)) return StoreError::kFlashWrite;  // the one final small write

  index_ = next;
  index_.seq = get32(bytes + kIdxOffSeq);
  index_copy_ = target;
  return StoreError::kNone;
}

StoreError Store::repair() {
  bool any = false;
  for (std::size_t s = 0; s < kRadioSlots; ++s) {
    if (index_.stale[s] == kNoArea) continue;
    any = true;
    const StoreError e = invalidate(areaOffset(s, index_.stale[s]));
    if (e != StoreError::kNone) return e;
  }
  if (!any) return StoreError::kNone;
  Index next = index_;
  for (std::size_t s = 0; s < kRadioSlots; ++s) next.stale[s] = kNoArea;
  return writeIndex(next);
}

StoreError Store::begin() {
  if (region_.sectorSize() != kSectorSize || region_.size() < kStoreBytes) {
    return StoreError::kRegionTooSmall;
  }
  // A reset ends whatever write was open: nothing of it was in memory that flash does not say,
  // and beginWrite() picks it up again from the progress record. Calling begin() twice is how
  // the tests model a Walter that restarted.
  writing_ = false;
  buffered_ = 0;
  identity(&index_);
  index_copy_ = 1;  // nothing valid yet: the first record goes to copy 0
  std::uint8_t copy = 0;
  Index loaded;
  if (loadIndex(&loaded, &copy)) {
    index_ = loaded;
    index_copy_ = copy;
  }
  ready_ = true;
  // A promotion that a reset caught between the index and the invalidation of the area the index
  // no longer points the golden image at: finish it. Idempotent, so running it twice is harmless.
  return repair();
}

std::uint8_t Store::areaOf(Which which, board::Slot slot) const {
  if (slot.index >= kRadioSlots) return kNoArea;
  // The base's area is the same one for ever: nothing points at it, so nothing can be made to
  // point somewhere else (store.h, the index).
  if (which == Which::kBase) return kBaseArea;
  return index_.area_of[slot.index][static_cast<std::size_t>(which)];
}

std::uint32_t Store::headerOffset(Which which, board::Slot slot) const {
  return areaOffset(slot.index, areaOf(which, slot));
}

// -- reading ------------------------------------------------------------------------------------

bool Store::readRecord(std::uint32_t header, Record* out) const {
  std::uint8_t bytes[kRecUsed];
  if (!region_.read(header, bytes, kRecUsed)) return false;
  if (get32(bytes) != kRecordMagic) return false;
  if (bytes[kRecOffFormat] != kFormatVersion) return false;
  if (get32(bytes + kRecOffCrc) != bodyCrc(bytes, kRecOffCrc)) return false;

  out->present = true;
  out->stored_as = bytes[kRecOffStoredAs] < kWhichCount
                       ? static_cast<Which>(bytes[kRecOffStoredAs])
                       : Which::kGolden;
  out->slot = board::Slot{bytes[kRecOffSlot]};
  out->use = static_cast<Use>(bytes[kRecOffUse]);
  out->size = get32(bytes + kRecOffSize);
  out->load_address = get32(bytes + kRecOffLoad);
  std::memcpy(out->sha256, bytes + kRecOffSha, kSha256Size);
  if (out->use == Use::kProvision) {
    // A base: those 96 bytes are what §4a found in the file, not a descriptor (§4a: a base
    // carries none).
    out->base = BaseSummary{};
    out->base.bootloader = get32(bytes + kRecOffBaseBootloader);
    out->base.flash_bytes = get32(bytes + kRecOffBaseFlashBytes);
    out->base.softdevice_id = get32(bytes + kRecOffBaseSdId);
    out->base.softdevice_version = get32(bytes + kRecOffBaseSdVersion);
    out->base.mbr_params = get32(bytes + kRecOffBaseMbrParams);
    out->base.has_mbr_params = bytes[kRecOffBaseHasMbrParams] != 0;
    out->base.uicr_word_count = static_cast<std::uint16_t>(
        bytes[kRecOffBaseUicrCount] | (bytes[kRecOffBaseUicrCount + 1] << 8));
    std::memcpy(out->base.uicr, bytes + kRecOffBaseUicrMap, sizeof out->base.uicr);
  } else if (!parse(bytes + kRecOffDescriptor, kDescriptorSize, &out->descriptor)) {
    // An image's 96 bytes are kept verbatim, so they are read back with §4's own parser: magic,
    // length and CRC. A record whose copy of the descriptor does not parse is not a record.
    out->present = false;
    return false;
  }
  std::memcpy(out->version, bytes + kRecOffVersion, kVersionSize);
  out->version[kVersionSize - 1] = 0;
  std::memcpy(out->commit, bytes + kRecOffCommit, kCommitSize);
  out->commit[kCommitSize] = 0;
  out->stored_at = get32(bytes + kRecOffStoredAt);
  std::memcpy(out->key_id, bytes + kRecOffKeyId, kKeyIdSize);
  return true;
}

bool Store::readProgress(std::uint32_t header, Record* out) const {
  bool found = false;
  for (std::uint32_t i = 0; i < kProgressSlots; ++i) {
    std::uint8_t bytes[kProgUsed];
    if (!region_.read(header + kRecordBytes + i * kProgressBytes, bytes, kProgUsed)) continue;
    if (get32(bytes) != kProgressMagic) continue;
    if (bytes[kProgOffFormat] != kFormatVersion) continue;
    if (get32(bytes + kProgOffCrc) != bodyCrc(bytes, kProgOffCrc)) continue;
    const std::uint32_t committed = get32(bytes + kProgOffCommitted);
    if (found && committed <= out->transferred) continue;
    out->transfer = true;
    out->transferred = committed;
    out->transfer_size = get32(bytes + kProgOffSize);
    std::memcpy(out->transfer_sha256, bytes + kProgOffSha, kSha256Size);
    found = true;
  }
  return found;
}

StoreError Store::read(Which which, board::Slot slot, Record* out) const {
  if (out == nullptr) return StoreError::kNoSuchSlot;
  *out = Record{};
  if (!ready_) return StoreError::kRegionTooSmall;
  if (slot.index >= kRadioSlots) return StoreError::kNoSuchSlot;
  out->which = which;
  out->slot = slot;
  const std::uint8_t area = areaOf(which, slot);
  // A promotion interrupted between its index write and the invalidation it implies: the role
  // points at an area whose record is on its way out, and the slot is empty until begin()
  // finishes the job. Nothing is read out of it in the meantime.
  if (area == index_.stale[slot.index]) return StoreError::kNone;
  const std::uint32_t header = areaOffset(slot.index, area);
  // The record names the slot it was stored for; an area of another slot holding it means the
  // layout has moved under the firmware's feet, and nothing is read out of it.
  if (readRecord(header, out) && out->slot == slot) {
    out->which = which;
    return StoreError::kNone;
  }
  *out = Record{};
  out->which = which;
  out->slot = slot;
  readProgress(header, out);
  return StoreError::kNone;
}

StoreError Store::open(Which which, board::Slot slot, StoredImage* out) const {
  if (out == nullptr) return StoreError::kEmpty;
  *out = StoredImage{};
  Record record;
  const StoreError e = read(which, slot, &record);
  if (e != StoreError::kNone) return e;
  if (!record.present) return StoreError::kEmpty;
  out->region_ = &region_;
  out->base_ = areaOffset(slot.index, areaOf(which, slot)) + kSectorSize;
  out->size_ = record.size;
  out->load_address_ = record.load_address;
  std::memcpy(out->sha256_, record.sha256, kSha256Size);
  out->rewind();
  return StoreError::kNone;
}

StoreError Store::openStaged(StoredImage* out) const {
  if (out == nullptr) return StoreError::kEmpty;
  *out = StoredImage{};
  if (!writing_) return StoreError::kNotWriting;
  if (written_ != write_size_ || buffered_ != 0) return StoreError::kSizeMismatch;
  out->region_ = &region_;
  out->base_ = write_data_;
  out->size_ = write_size_;
  out->load_address_ = write_load_address_;
  std::memcpy(out->sha256_, write_sha256_, kSha256Size);
  out->rewind();
  return StoreError::kNone;
}

StoreError Store::verify(Which which, board::Slot slot) const {
  StoredImage image;
  const StoreError e = open(which, slot, &image);
  if (e != StoreError::kNone) return e;
  std::uint8_t piece[256];
  for (std::uint32_t at = 0; at < image.size();) {
    const std::uint32_t left = image.size() - at;
    const std::uint32_t n = left < sizeof piece ? left : static_cast<std::uint32_t>(sizeof piece);
    if (!image.read(at, piece, n)) return StoreError::kFlashRead;
    at += n;
  }
  return image.hashOk() ? StoreError::kNone : StoreError::kHashMismatch;
}

// -- writing ------------------------------------------------------------------------------------

StoreError Store::invalidate(std::uint32_t header) {
  std::uint8_t magic[4];
  if (!region_.read(header, magic, 4)) return StoreError::kFlashRead;
  if (get32(magic) == 0) return StoreError::kNone;  // already not a record
  // One small write, and no erase: on NOR flash a write only clears bits, so zeros over the
  // magic word destroy it wherever it was. A torn write leaves a word that is neither the magic
  // nor what it was, which is not a record either - which is the point.
  const std::uint8_t zeros[4] = {0, 0, 0, 0};
  if (!region_.write(header, zeros, 4)) return StoreError::kFlashWrite;
  return StoreError::kNone;
}

StoreError Store::beginWrite(Which which, board::Slot slot, const ExpectedFile& expected) {
  if (!ready_) return StoreError::kRegionTooSmall;
  if (slot.index >= kRadioSlots) return StoreError::kNoSuchSlot;
  if (writing_) return StoreError::kBusy;
  // An `swd` file into one of the three images, a `provision` file into the base, and nothing
  // else either way: the base is not an image (spec/FIRMWARE-MANIFEST.md §4a).
  const bool base = which == Which::kBase;
  const Use wanted = base ? Use::kProvision : Use::kSwd;
  if (expected.use() != Use::kSwd && expected.use() != Use::kProvision) {
    return StoreError::kWrongUse;
  }
  if (expected.use() != wanted) return StoreError::kNotAnImage;
  if (expected.slot() != slot) return StoreError::kNoSuchSlot;
  if (expected.size() == 0) return StoreError::kEmptyFile;
  if (expected.size() > areaCapacity(which)) return StoreError::kTooLarge;
  // The bad list is about images that failed a health check (task p2-03). A base is not
  // health-checked - S-5a's read-back of every word is its check - so it is not on that list and
  // not held to it.
  if (!base && isBad(expected.sha256())) return StoreError::kOnBadList;

  const std::uint32_t header = headerOffset(which, slot);
  Record existing;
  const StoreError e = read(which, slot, &existing);
  if (e != StoreError::kNone) return e;
  // S-5: a known-good image is always on hand. Golden is loaded directly only while it is empty -
  // a unit's first provisioning - and is afterwards reached only through a promotion.
  if (which == Which::kGolden && existing.present) return StoreError::kGoldenNotEmpty;

  write_which_ = which;
  write_slot_ = slot;
  write_use_ = expected.use();
  write_size_ = expected.size();
  write_load_address_ = expected.loadAddress();
  std::memcpy(write_sha256_, expected.sha256(), kSha256Size);
  write_header_ = header;
  write_data_ = header + kSectorSize;
  buffered_ = 0;

  // A transfer of this very file that an interruption left half done: continue it. The progress
  // record says how many bytes are in flash, always at a sector boundary, so the rest is written
  // into sectors that have not been touched (a download that broke at 60 % continues).
  if (!existing.present && existing.transfer && existing.transfer_size == expected.size() &&
      sha256Equal(existing.transfer_sha256, expected.sha256()) &&
      existing.transferred <= expected.size() && existing.transferred % kResumeStep == 0) {
    written_ = existing.transferred;
    progress_slot_ = static_cast<std::uint8_t>(written_ / kResumeStep);
    next_progress_ = written_ + kResumeStep;
    writing_ = true;
    return StoreError::kNone;
  }

  // Otherwise this is a fresh write: the record goes first (one small write, before anything of
  // the area is overwritten), then the header sector, which takes the old progress with it.
  const StoreError invalidated = invalidate(header);
  if (invalidated != StoreError::kNone) return invalidated;
  if (!region_.erase(header, kSectorSize)) return StoreError::kFlashErase;
  written_ = 0;
  progress_slot_ = 0;
  next_progress_ = kResumeStep;
  writing_ = true;
  return StoreError::kNone;
}

StoreError Store::writeProgress() {
  if (progress_slot_ >= kProgressSlots) return StoreError::kNone;  // nothing left to write it in
  std::uint8_t bytes[kProgUsed] = {};
  bytes[kProgOffFormat] = kFormatVersion;
  bytes[kProgOffWhich] = static_cast<std::uint8_t>(write_which_);
  bytes[kProgOffSlot] = write_slot_.index;
  put32(bytes + kProgOffSize, write_size_);
  put32(bytes + kProgOffCommitted, written_);
  std::memcpy(bytes + kProgOffSha, write_sha256_, kSha256Size);
  put32(bytes + kProgOffCrc, bodyCrc(bytes, kProgOffCrc));
  put32(bytes, kProgressMagic);

  const std::uint32_t at = write_header_ + kRecordBytes + progress_slot_ * kProgressBytes;
  if (!region_.write(at + 4, bytes + 4, kProgOffCrc + 4 - 4)) return StoreError::kFlashWrite;
  if (!region_.write(at, bytes, 4)) return StoreError::kFlashWrite;  // the one final small write
  ++progress_slot_;
  return StoreError::kNone;
}

StoreError Store::flushSector(bool last) {
  if (buffered_ == 0) return StoreError::kNone;
  // written_ is a multiple of the sector size: a fresh write starts at 0, a resumed one at a
  // multiple of kResumeStep, and only whole sectors are flushed before the last piece.
  const std::uint32_t at = write_data_ + written_;
  if (!region_.erase(at, kSectorSize)) return StoreError::kFlashErase;
  // Whole words: the tail of the last sector is padded with the 0xFF an erase left (a flash
  // write of a partial word would be a second write to that word later).
  const std::uint32_t n = (buffered_ + 3u) & ~3u;
  for (std::uint32_t i = buffered_; i < n; ++i) buffer_[i] = 0xFF;
  if (!region_.write(at, buffer_, n)) return StoreError::kFlashWrite;
  written_ += buffered_;
  buffered_ = 0;
  if (!last && written_ >= next_progress_) {
    const StoreError e = writeProgress();
    if (e != StoreError::kNone) return e;
    next_progress_ += kResumeStep;
  }
  return StoreError::kNone;
}

StoreError Store::write(const std::uint8_t* data, std::uint32_t size) {
  if (!writing_) return StoreError::kNotWriting;
  if (data == nullptr && size != 0) return StoreError::kFlashWrite;
  if (static_cast<std::uint64_t>(written_) + buffered_ + size > write_size_) {
    return StoreError::kSizeMismatch;
  }
  while (size > 0) {
    const std::uint32_t room = kSectorSize - buffered_;
    const std::uint32_t n = size < room ? size : room;
    std::memcpy(buffer_ + buffered_, data, n);
    buffered_ += n;
    data += n;
    size -= n;
    if (buffered_ == kSectorSize) {
      const StoreError e = flushSector(false);
      if (e != StoreError::kNone) return e;
    }
  }
  return StoreError::kNone;
}

StoreError Store::endWrite() {
  if (!writing_) return StoreError::kNotWriting;
  const StoreError flushed = flushSector(true);
  if (flushed != StoreError::kNone) return flushed;
  return written_ == write_size_ ? StoreError::kNone : StoreError::kSizeMismatch;
}

StoreError Store::readBackAndCheck() {
  // The bytes are in place: read them back out of flash and hash them. A record is never made
  // valid over bytes nobody has read.
  Sha256 hash;
  for (std::uint32_t at = 0; at < write_size_;) {
    const std::uint32_t left = write_size_ - at;
    const std::uint32_t n = left < kSectorSize ? left : kSectorSize;
    if (!region_.read(write_data_ + at, buffer_, n)) return StoreError::kFlashRead;
    hash.update(buffer_, n);
    at += n;
  }
  std::uint8_t digest[kSha256Size];
  hash.finish(digest);
  if (!sha256Equal(digest, write_sha256_)) return StoreError::kHashMismatch;
  return StoreError::kNone;
}

StoreError Store::writeRecord(const Fields* descriptor, const BaseSummary* base,
                              const char* version, const char* commit_hex,
                              const std::uint8_t* key_id) {
  std::uint32_t stored_at = 0;
  interfaces::WallTime when{};
  if (clock_.now(&when)) stored_at = when.epoch;

  std::uint8_t bytes[kRecUsed] = {};
  bytes[kRecOffFormat] = kFormatVersion;
  bytes[kRecOffStoredAs] = static_cast<std::uint8_t>(write_which_);
  bytes[kRecOffSlot] = write_slot_.index;
  bytes[kRecOffUse] = static_cast<std::uint8_t>(write_use_);
  put32(bytes + kRecOffSize, write_size_);
  put32(bytes + kRecOffLoad, write_load_address_);
  std::memcpy(bytes + kRecOffSha, write_sha256_, kSha256Size);
  if (descriptor != nullptr) {
    // The descriptor is kept as the 96 bytes of §4, rebuilt from the fields the verdict read out
    // of the image itself, so that `image list` and the update's health check read it with the
    // same parser the image was checked with.
    const Built built = build(*descriptor);
    if (!built.ok) return StoreError::kTokenMismatch;  // a descriptor that cannot be rebuilt
    std::memcpy(bytes + kRecOffDescriptor, built.descriptor.bytes, kDescriptorSize);
  } else if (base != nullptr) {
    put32(bytes + kRecOffBaseBootloader, base->bootloader);
    put32(bytes + kRecOffBaseFlashBytes, base->flash_bytes);
    put32(bytes + kRecOffBaseSdId, base->softdevice_id);
    put32(bytes + kRecOffBaseSdVersion, base->softdevice_version);
    put32(bytes + kRecOffBaseMbrParams, base->mbr_params);
    bytes[kRecOffBaseHasMbrParams] = base->has_mbr_params ? 1 : 0;
    bytes[kRecOffBaseUicrCount] = static_cast<std::uint8_t>(base->uicr_word_count);
    bytes[kRecOffBaseUicrCount + 1] = static_cast<std::uint8_t>(base->uicr_word_count >> 8);
    std::memcpy(bytes + kRecOffBaseUicrMap, base->uicr, sizeof base->uicr);
  }
  std::memcpy(bytes + kRecOffVersion, version, kVersionSize - 1);
  std::memcpy(bytes + kRecOffCommit, commit_hex, kCommitSize);
  put32(bytes + kRecOffStoredAt, stored_at);
  std::memcpy(bytes + kRecOffKeyId, key_id, kKeyIdSize);
  put32(bytes + kRecOffCrc, bodyCrc(bytes, kRecOffCrc));
  put32(bytes, kRecordMagic);

  if (!region_.write(write_header_ + 4, bytes + 4, kRecOffCrc + 4 - 4)) {
    return StoreError::kFlashWrite;
  }
  // The one final small write that makes the record - and what it describes - exist.
  if (!region_.write(write_header_, bytes, 4)) return StoreError::kFlashWrite;
  writing_ = false;
  return StoreError::kNone;
}

StoreError Store::commit(const AcceptedImage& token) {
  if (!writing_) return StoreError::kNotWriting;
  if (write_which_ == Which::kBase) return StoreError::kNotAnImage;
  if (buffered_ != 0 || written_ != write_size_) return StoreError::kSizeMismatch;
  // The token must be the verdict over the very file these bytes were written for.
  if (token.use() != write_use_ || token.slot() != write_slot_ ||
      token.file().size != write_size_ || !sha256Equal(token.sha256(), write_sha256_)) {
    return StoreError::kTokenMismatch;
  }
  const StoreError read_back = readBackAndCheck();
  if (read_back != StoreError::kNone) return read_back;
  const Fields descriptor = token.descriptor();
  return writeRecord(&descriptor, nullptr, token.version(), token.commit(), token.keyId());
}

StoreError Store::commitBase(const AcceptedBase& token) {
  if (!writing_) return StoreError::kNotWriting;
  if (write_which_ != Which::kBase) return StoreError::kNotAnImage;
  if (buffered_ != 0 || written_ != write_size_) return StoreError::kSizeMismatch;
  if (token.use() != write_use_ || token.slot() != write_slot_ ||
      token.file().size != write_size_ || !sha256Equal(token.sha256(), write_sha256_)) {
    return StoreError::kTokenMismatch;
  }
  const StoreError read_back = readBackAndCheck();
  if (read_back != StoreError::kNone) return read_back;
  const BaseSummary summary = token.summary();
  return writeRecord(nullptr, &summary, token.version(), token.commit(), token.keyId());
}

void Store::abandonWrite() {
  writing_ = false;
  buffered_ = 0;
}

// -- promotion, the bad list, erasing -----------------------------------------------------------

StoreError Store::promote(board::Slot slot) {
  if (!ready_) return StoreError::kRegionTooSmall;
  if (slot.index >= kRadioSlots) return StoreError::kNoSuchSlot;
  if (writing_) return StoreError::kBusy;
  Record candidate;
  const StoreError e = read(Which::kCandidate, slot, &candidate);
  if (e != StoreError::kNone) return e;
  if (!candidate.present) return StoreError::kEmpty;

  const std::uint8_t golden_area = areaOf(Which::kGolden, slot);
  const std::uint8_t candidate_area = areaOf(Which::kCandidate, slot);

  // One index write and the golden image is the new one. Before it, the old golden image is the
  // golden image; after it, the new one is - and at no point is there none.
  Index next = index_;
  next.area_of[slot.index][static_cast<std::size_t>(Which::kGolden)] = candidate_area;
  next.area_of[slot.index][static_cast<std::size_t>(Which::kCandidate)] = golden_area;
  next.stale[slot.index] = golden_area;
  const StoreError written = writeIndex(next);
  if (written != StoreError::kNone) return written;

  // The area that held it is now the candidate area, and what is in it is an image nobody asked
  // to try: its record goes. Until it does, `stale` keeps the candidate slot reading as empty,
  // and begin() finishes this if a reset lands in the middle.
  return repair();
}

bool Store::isBad(const std::uint8_t sha256[kSha256Size]) const {
  if (sha256 == nullptr) return false;
  for (std::uint8_t i = 0; i < index_.bad_count; ++i) {
    if (sha256Equal(index_.bad[i], sha256)) return true;
  }
  return false;
}

const std::uint8_t* Store::badAt(std::size_t i) const {
  return i < index_.bad_count ? index_.bad[i] : nullptr;
}

StoreError Store::markBad(const std::uint8_t sha256[kSha256Size]) {
  if (!ready_) return StoreError::kRegionTooSmall;
  if (sha256 == nullptr) return StoreError::kEmptyFile;
  if (isBad(sha256)) return StoreError::kNone;
  Index next = index_;
  if (next.bad_count == kBadListMax) {
    // A unit that has refused eight different images is not saved by remembering a ninth: the
    // oldest goes.
    for (std::size_t i = 1; i < kBadListMax; ++i) {
      std::memcpy(next.bad[i - 1], next.bad[i], kSha256Size);
    }
    next.bad_count = static_cast<std::uint8_t>(kBadListMax - 1);
  }
  std::memcpy(next.bad[next.bad_count], sha256, kSha256Size);
  ++next.bad_count;
  return writeIndex(next);
}

StoreError Store::erase(Which which, board::Slot slot) {
  if (!ready_) return StoreError::kRegionTooSmall;
  if (slot.index >= kRadioSlots) return StoreError::kNoSuchSlot;
  if (which == Which::kGolden) return StoreError::kNeverGolden;
  if (writing_ && write_which_ == which && write_slot_ == slot) return StoreError::kBusy;
  // The base goes the same way the candidate and the scanner image do: `image erase base`.
  const std::uint32_t header = headerOffset(which, slot);
  const StoreError e = invalidate(header);
  if (e != StoreError::kNone) return e;
  // The header sector goes too, so that a half-finished transfer is not resumed into the next
  // image. The bytes above it are left: nothing reads them without a record, and the next write
  // erases the sectors it needs anyway.
  if (!region_.erase(header, kSectorSize)) return StoreError::kFlashErase;
  return StoreError::kNone;
}

}  // namespace image
}  // namespace core
