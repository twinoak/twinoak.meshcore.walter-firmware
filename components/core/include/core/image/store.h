// The image store (task p2-02): the radio's firmware, kept in the Walter's own flash.
// docs/ARCHITECTURE.md §6 and §11, spec/SYSTEM.md S-5, spec/FIRMWARE-MANIFEST.md §5.
//
// Per radio slot three images: the **golden** one (the last that proved itself), a **candidate**
// (verified, not yet proven) and a **scanner** image for the day a site needs NoiseScope. Each is
// either wholly there - with its hash, its descriptor and where it came from - or not there at
// all. A power cut while one is being written cannot leave something that looks like an image and
// is not.
//
// -- the layout ---------------------------------------------------------------------------------
//
//   rakfw (partitions.csv: data/0x40 at 0x630000, 6 MB), sector 4 KB
//
//     sector 0           the index, copy A        |  which physical area each of the three roles
//     sector 1           the index, copy B        |  is, the bad list; the newer valid copy wins
//     then six areas, slot 0's three and slot 1's three, each
//       + 0              the header sector: the record, and the progress of a transfer
//       + 4 KB           the image's bytes, kDataSectors of them
//
// The store is laid out for **two** radio slots from the start (task p1-06), so that a dual unit
// does not need the partition rewritten; the second slot's areas are there whether or not the
// adapter has a second radio. An area holds the largest radio image this build knows of - the
// board profile's application region, rounded up to a sector - plus one header sector. The
// arithmetic is held against the partition's own size by the static_assert below and by
// host_test/core/image/test_store.cpp, which reads the `rakfw` line of partitions.csv rather
// than trusting the copy of it here.
//
// -- what makes a record whole ------------------------------------------------------------------
//
// A record is **made valid by one final small write** - its magic word - after the image's bytes
// are in place and have been read back and hashed, and is **invalidated by one small write** -
// zeros over the same word - before anything of the area is overwritten. What a power loss on
// each side of each write leaves:
//
//   | the power goes                        | the slot afterwards                               |
//   |---------------------------------------|---------------------------------------------------|
//   | before the invalidating write         | the old image, whole                              |
//   | during it (a torn magic word)         | the old record does not validate: the slot is empty |
//   | after it, during the erases or writes | empty, with a transfer's progress or without it   |
//   | during the record's body              | empty (the magic is not there yet)                |
//   | during the final magic word           | empty (a torn magic is not the magic)             |
//   | after it                              | the new image, whole                              |
//
// There is no moment at which a record validates over bytes that are not its own: the magic is
// the last thing written and the first thing destroyed, and the body it belongs to was written
// after the bytes it describes were read back.
//
// -- what may enter -----------------------------------------------------------------------------
//
// Nothing enters without task p2-01's verdict. beginWrite() takes an `ExpectedFile` - what an
// accepted manifest promises about a file, which only ImageVerdict::judgeManifest() can make -
// and commit() takes an `AcceptedImage`, the token of all six checks of
// spec/FIRMWARE-MANIFEST.md §5. Neither has a public constructor, so there is no way to reach
// either door without a manifest signed by a key this build trusts; and since only commit()
// writes a record, bytes in an area are not an image until the six checks have passed over them.
//
// -- no heap, no ESP-IDF ------------------------------------------------------------------------
//
// One Store is ~4.3 KB (a sector buffer and the index) and belongs in static storage.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/flasher/board_profile.h"
#include "core/flasher/image_source.h"
#include "core/image/crypto/sha256.h"
#include "core/image/descriptor.h"
#include "core/image/manifest.h"
#include "core/image/verdict.h"
#include "interfaces/flash_region.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace image {

// -- the three images of a slot -----------------------------------------------------------------

// What an image in a slot is for. Not core::image::Role, which is the descriptor's byte (§4).
enum class Which : std::uint8_t { kGolden = 0, kCandidate = 1, kScanner = 2 };

constexpr std::size_t kWhichCount = 3;

const char* name(Which which);
// "golden", "candidate", "scanner" -> Which. False for anything else.
bool whichFromName(const char* text, Which* out);

// -- the layout, in numbers ---------------------------------------------------------------------

// The sector of the ESP32-S3's flash. ESP-IDF erases in units of SPI_FLASH_SEC_SIZE (4096); the
// real region says so itself (sectorSize()) and begin() refuses one that disagrees, so this is
// the number the layout is computed with and not an assumption about the part.
constexpr std::uint32_t kSectorSize = 4096;

// partitions.csv: `rakfw, data, 0x40, 0x630000, 0x600000`. Written down here only so that the
// arithmetic below can be checked at build time; the store never uses it at run time - what it
// gets is a region of whatever size, and begin() refuses one smaller than kStoreBytes.
constexpr std::uint32_t kPartitionBytes = 0x600000;

// Two radio slots from the start (board::kSlotCount; task p1-06, S-11).
constexpr std::size_t kRadioSlots = board::kSlotCount;

constexpr std::uint32_t roundUpToSector(std::uint32_t bytes) {
  return ((bytes + kSectorSize - 1) / kSectorSize) * kSectorSize;
}

// The largest radio image: the application region of the largest board profile this build knows
// (core/flasher/board_profile.h - for rak4630-single, 0x26000 to 0xED000), rounded up to a
// sector. An image larger than its board's region is refused by check 6 long before the store
// sees it, so an area this size can hold any image that can be accepted for any known board.
// The dual adapter's second position has no profile yet (board_profile.h says why), so its areas
// are laid out to the same size - which is why the arithmetic is worth checking rather than
// assuming.
constexpr std::uint32_t kLargestAppRegion =
    flasher::kRak4630Single.app.end - flasher::kRak4630Single.app.begin;
constexpr std::uint32_t kImageBytesMax = roundUpToSector(kLargestAppRegion);
constexpr std::uint32_t kDataSectors = kImageBytesMax / kSectorSize;
constexpr std::uint32_t kAreaSectors = kDataSectors + 1;  // + the header sector
constexpr std::uint32_t kAreaBytes = kAreaSectors * kSectorSize;

// The index: two copies, a sector each (the erase unit), the newer valid one wins.
constexpr std::uint32_t kIndexCopies = 2;
constexpr std::uint32_t kIndexBytes = kIndexCopies * kSectorSize;

constexpr std::uint32_t kAreaCount = static_cast<std::uint32_t>(kRadioSlots * kWhichCount);
constexpr std::uint32_t kStoreBytes = kIndexBytes + kAreaCount * kAreaBytes;

static_assert(kStoreBytes <= kPartitionBytes,
              "the image store's layout does not fit the rakfw partition (partitions.csv); "
              "either the partition grows or an area holds less");
static_assert(kImageBytesMax >= kLargestAppRegion, "an area must hold the largest image");

// Where an area begins in the region: the index first, then slot 0's three and slot 1's three.
// `area` is the *physical* area of that slot, 0 to 2; which role each holds is the index's.
constexpr std::uint32_t areaOffset(std::size_t slot, std::size_t area) {
  return kIndexBytes +
         static_cast<std::uint32_t>(slot * kWhichCount + area) * kAreaBytes;
}

// -- the record ---------------------------------------------------------------------------------

// The magic words, each the last thing written of its record and the first thing destroyed.
// Bytes, not a string literal: they are compared and copied as bytes.
constexpr std::uint32_t kRecordMagic = 0x31525754;    // "TWR1"
constexpr std::uint32_t kProgressMagic = 0x31505754;  // "TWP1"
constexpr std::uint32_t kIndexMagic = 0x31495754;     // "TWI1"

// The format of the three records. A record of another version is not read: a firmware whose
// layout changed does not read the old bytes as the new ones (interfaces/retained.h's rule).
constexpr std::uint8_t kFormatVersion = 1;

// Where things are in the header sector. The record is first so that its magic word is at the
// start of the sector, which an erase clears before anything else; the progress records are
// appended one after another above it and are gone with the same erase.
constexpr std::uint32_t kRecordBytes = 512;
constexpr std::uint32_t kProgressBytes = 64;
constexpr std::uint32_t kProgressSlots = (kSectorSize - kRecordBytes) / kProgressBytes;  // 56

// How often a transfer's progress is written down: a power cut loses at most this much of the
// bytes already sent, and resuming starts at a sector boundary (kResumeStep is a whole number of
// sectors, so a resumed write never has to re-enter a sector it had begun). 56 progress records
// cover 3.5 MB at this step, four times the largest image.
constexpr std::uint32_t kResumeStep = 16 * kSectorSize;  // 64 KB
static_assert(kResumeStep % kSectorSize == 0, "a resume point must be a sector boundary");
static_assert(kProgressSlots * kResumeStep >= kImageBytesMax,
              "there must be a progress record for every resume point of the largest image");

// The bad list: the hashes of candidates that failed their health check (task p2-03). Eight is a
// bound, not a policy - a unit that has refused eight different images is not going to be saved
// by remembering a ninth, and the oldest is dropped. It lives in the index record, so it is
// written with the same two-copy atomicity.
constexpr std::size_t kBadListMax = 8;

// What a slot holds. `present` is the only thing a caller may act on: a record whose magic,
// format or CRC is not right is not present at all.
struct Record {
  bool present = false;
  Which which = Which::kGolden;       // what it is now - which role was asked for
  Which stored_as = Which::kGolden;   // what it was stored as; a promotion moves a pointer, not
                                      // the bytes, so a golden image usually says "candidate"
  board::Slot slot;
  Use use = Use::kUnknown;
  std::uint32_t size = 0;
  std::uint32_t load_address = 0;
  std::uint8_t sha256[kSha256Size] = {};
  Fields descriptor;                       // the 96 bytes inside the image (§4), as stored
  char version[kVersionSize] = {};         // the manifest's `version`
  char commit[kCommitSize + 1] = {};       // the manifest's `commit`
  std::uint32_t stored_at = 0;             // epoch seconds, 0 when the Walter had no time to give
  std::uint8_t key_id[kKeyIdSize] = {};    // which release key signed the manifest it came from

  // When !present: a transfer that was interrupted, and how far it got. The bytes are in flash
  // but nothing says they are whole, so they are not an image.
  bool transfer = false;
  std::uint32_t transferred = 0;
  std::uint32_t transfer_size = 0;
  std::uint8_t transfer_sha256[kSha256Size] = {};
};

// -- what can go wrong --------------------------------------------------------------------------

enum class StoreError : std::uint8_t {
  kNone = 0,
  kRegionTooSmall,   // the region handed over is smaller than the layout, or its sector is not 4 KB
  kNoSuchSlot,       // a slot the layout has no area for
  kFlashRead,
  kFlashWrite,
  kFlashErase,
  kTooLarge,         // the file is larger than an image area holds
  kEmptyFile,        // nothing to store
  kWrongUse,         // this store holds a radio's images: §2's `swd` files and nothing else
  kOnBadList,        // this hash failed a health check once and is never tried again
  kGoldenNotEmpty,   // golden is reached through a promotion once there has been one
  kNeverGolden,      // the golden image is not erased by a command
  kEmpty,            // there is no image in that slot
  kBusy,             // another write is open on this store
  kNotWriting,       // commit() or write() with no write open
  kSizeMismatch,     // more bytes than promised, or commit() before the last of them
  kHashMismatch,     // what was read back is not the hash the token names
  kTokenMismatch,    // the token is not for the file the write was opened for
};

const char* describe(StoreError error);

// -- the store ----------------------------------------------------------------------------------

// One image in the store, as the flasher wants it (core/flasher/image_source.h): any range of
// bytes, read from the Walter's flash. It **re-hashes the bytes as they are read in order**, so
// that flash rot is noticed before it reaches a radio: a reader that goes through the image from
// start to end - which is what the flasher's write pass and verify() do - can ask afterwards
// whether what it read was what the record says, and a single flipped bit says no.
class StoredImage final : public flasher::ImageSource {
 public:
  std::uint32_t size() const override { return size_; }
  bool read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const override;

  std::uint32_t loadAddress() const { return load_address_; }
  const std::uint8_t* sha256() const { return sha256_; }

  // Whether every read so far continued where the last one ended (a seek backwards or a gap sets
  // this false for good, and hashOk() then cannot be true).
  bool sequential() const { return sequential_; }
  // True only when the whole image has been read in order and its SHA-256 is the record's.
  bool hashOk() const;

  // Forget what has been read, to go through the image again.
  void rewind();

 private:
  friend class Store;
  const interfaces::FlashRegion* region_ = nullptr;
  std::uint32_t base_ = 0;  // where the image's bytes begin in the region
  std::uint32_t size_ = 0;
  std::uint32_t load_address_ = 0;
  std::uint8_t sha256_[kSha256Size] = {};
  mutable Sha256 hash_;
  mutable std::uint32_t next_ = 0;
  mutable bool sequential_ = true;
};

class Store {
 public:
  // The region is the `rakfw` partition (platform/flash_partition.h) or a simulated one. The
  // wall clock is what dates a record; it answers "I have no time to give" until phase 3 attaches
  // to a network, and a record stored then says 0.
  Store(interfaces::FlashRegion& region, interfaces::WallClock& clock)
      : region_(region), clock_(clock) {}

  // Reads the index and finishes a promotion that a reset interrupted. Must be called once
  // before anything else; everything else answers kRegionTooSmall until it has succeeded.
  StoreError begin();

  // -- reading ---------------------------------------------------------------------------------

  // What a slot holds, or `present == false`. kNoSuchSlot for a slot outside the layout.
  StoreError read(Which which, board::Slot slot, Record* out) const;

  // The image in a slot as an ImageSource. kEmpty when there is none.
  StoreError open(Which which, board::Slot slot, StoredImage* out) const;

  // Reads the whole image and compares its SHA-256 with the record's: what `image verify` does,
  // and what the update does before it flashes. kHashMismatch when a bit has rotted.
  StoreError verify(Which which, board::Slot slot) const;

  // -- writing: only with task p2-01's verdict --------------------------------------------------

  // Opens a write of `expected` into a slot. The existing record is invalidated first (one small
  // write), so the slot is empty from here on until commit() succeeds. An interrupted write of
  // the same file resumes where it left off: written() says where, and the caller sends the rest.
  //
  // Refusals: kOnBadList (the hash failed a health check once), kGoldenNotEmpty (golden may be
  // loaded directly only while it is empty - a unit's first provisioning; afterwards golden is
  // reached only through promote()), kTooLarge, kNoSuchSlot, kBusy.
  StoreError beginWrite(Which which, board::Slot slot, const ExpectedFile& expected);

  bool writing() const { return writing_; }
  // How many bytes of the file are in flash: 0, or where a resumed write continues.
  std::uint32_t written() const { return written_; }
  // What the open write is for: the promised size and hash, copied out of the ExpectedFile
  // (which has no public constructor, so the store keeps its fields and not the object).
  std::uint32_t expectedSize() const { return write_size_; }
  const std::uint8_t* expectedSha256() const { return write_sha256_; }

  // The next piece of the file. Pieces may be any size; the store buffers them into whole
  // sectors, erases each sector as it reaches it, and writes a progress record every 64 KB.
  StoreError write(const std::uint8_t* data, std::uint32_t size);

  // The last byte has been given: the tail of the last sector goes to flash, and the bytes can
  // then be read back and judged (openStaged). kSizeMismatch when fewer bytes arrived than the
  // manifest promised. Nothing of the record is written here - that is commit's, with the token.
  StoreError endWrite();

  // The bytes of the open write as an ImageSource, so that the verdict's checks 4, 5 and 6 can
  // run over what is actually in the Walter's flash - not over what a laptop or a server said it
  // was sending - before any record exists. kNotWriting when no write is open, kSizeMismatch
  // until endWrite() has put the last of them there.
  StoreError openStaged(StoredImage* out) const;

  // After endWrite(): reads the bytes back from flash, hashes them, holds them to the token, and
  // then writes the record's body and makes it valid with one final small write. The only way a
  // record comes to exist.
  StoreError commit(const AcceptedImage& token);

  // Give up on an open write. The slot stays empty; the bytes and the progress record stay in
  // flash, so the same file can be resumed later. Nothing is written.
  void abandonWrite();

  // -- promotion, the bad list and erasing ------------------------------------------------------

  // The candidate becomes the golden image, without copying a byte: the index's role-to-area
  // pointers are swapped and written atomically (two copies, sequence numbers, the newer valid
  // one wins). The image that was golden is kept until the new one is in place, and there is
  // never a moment with no golden image once there has been one. The area that held the old
  // golden image becomes the candidate area and its record is invalidated afterwards; a reset in
  // that window is finished by begin().
  StoreError promote(board::Slot slot);

  // Remember a hash as one that failed its health check (task p2-03): it is refused at
  // beginWrite() from now on. Already there: nothing happens. Full: the oldest is dropped.
  StoreError markBad(const std::uint8_t sha256[kSha256Size]);
  bool isBad(const std::uint8_t sha256[kSha256Size]) const;
  std::size_t badCount() const { return index_.bad_count; }
  const std::uint8_t* badAt(std::size_t i) const;

  // Invalidates a slot's record and clears its header sector. kNeverGolden for the golden image:
  // S-5 says a known-good image is always on hand, and a console command is not how it goes.
  StoreError erase(Which which, board::Slot slot);

  // Which physical area a role is in (0 to 2), for a console line and for the tests that show a
  // promotion moving a pointer and not megabytes.
  std::uint8_t areaOf(Which which, board::Slot slot) const;
  std::uint32_t indexSequence() const { return index_.seq; }

 private:
  // The index, in memory.
  struct Index {
    std::uint32_t seq = 0;
    std::uint8_t area_of[kRadioSlots][kWhichCount] = {};
    std::uint8_t stale[kRadioSlots] = {};  // an area whose record a promotion must still destroy
    std::uint8_t bad_count = 0;
    std::uint8_t bad[kBadListMax][kSha256Size] = {};
  };

  static void identity(Index* index);
  bool loadIndex(Index* out, std::uint8_t* from_copy) const;
  StoreError writeIndex(const Index& next);
  StoreError repair();

  std::uint32_t headerOffset(Which which, board::Slot slot) const;
  bool readRecord(std::uint32_t header, Record* out) const;
  bool readProgress(std::uint32_t header, Record* out) const;
  StoreError invalidate(std::uint32_t header);
  StoreError flushSector(bool last);
  StoreError writeProgress();

  interfaces::FlashRegion& region_;
  interfaces::WallClock& clock_;
  bool ready_ = false;
  Index index_;
  // Where the newest valid index record is; the next one goes in the other copy, so that the
  // erase that precedes it cannot destroy the only one there is. 1 before anything was written,
  // so the first record goes to copy 0.
  std::uint8_t index_copy_ = 1;

  // The open write: the promise's fields, where the area is, and how far the bytes have got.
  bool writing_ = false;
  Which write_which_ = Which::kCandidate;
  board::Slot write_slot_;
  Use write_use_ = Use::kUnknown;
  std::uint32_t write_size_ = 0;
  std::uint32_t write_load_address_ = 0;
  std::uint8_t write_sha256_[kSha256Size] = {};
  std::uint32_t write_header_ = 0;
  std::uint32_t write_data_ = 0;
  std::uint32_t written_ = 0;       // bytes of the file in flash
  std::uint32_t buffered_ = 0;      // bytes in buffer_ not yet in flash
  std::uint32_t next_progress_ = 0; // the written_ at which the next progress record goes down
  std::uint8_t progress_slot_ = 0;  // which of the header sector's progress slots is next
  std::uint8_t buffer_[kSectorSize] = {};
};

}  // namespace image
}  // namespace core
