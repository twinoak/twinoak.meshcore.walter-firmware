// The image a sleep keeps: its layout, its checksum, a writer and a reader (task p1.6-22).
//
// What the loop writes into an IRetained store at the end of a wake, and reads back after the
// next boot (agent/IRetained.h says when and why). One image: a header, then the kept results
// **oldest first**, then the ring's waiting adverts oldest first - results before adverts because
// a lost result leaves a job leased until the server's lease lapses, and a lost advert is heard
// again. Each is written whole or not at all, in order, and the writer stops at the first that
// does not fit; what it stopped at and everything behind it is what the loop drops and counts.
//
// The layout is bytes, little-endian, and not a struct copied: a unit lives with this format
// across thousands of sleeps and a firmware update, and a struct's padding is the compiler's.
//
//   header, 32 bytes
//     0   u32  magic           "KEPT" as the four bytes 4B 45 50 54
//     4   u32  crc             CRC-32 (ISO-HDLC, as zlib's crc32 and the ESP ROM compute it) over
//                              every byte from offset 8 to `length`
//     8   u16  version         kKeptImageVersion; another version is refused whole
//    10   u16  resultCount     at most ResultBuffer::kMaxResults
//    12   u16  advertCount     at most ObservationRing::kCapacity
//    14   u16  reserved        0
//    16   u32  length          the whole image, header included: a store may hand back more
//    20   u32  resultsDropped  the two counters the heartbeat carries, as they stood when the
//    24   u32  observationsDropped   image was written - so a loss survives the sleep it caused
//    28   u32  imagesRefused   how many images a boot has refused, so that count survives too
//   results, resultCount of
//     0   i64  jobId
//     8   u32  len             the body's length, without a NUL
//    12   u8   body[len]       the bytes `postJobResult` would be handed, verbatim
//   adverts, advertCount of 108 bytes each (kKeptAdvertLen)
//     0   u8   publicKey[32]
//    32   u32  heardAtEpoch    UNIX seconds the advert was heard; 0 when no clock knew
//    36   u8   pathBytes[24]
//    60   u8   pathByteLen, pathHashSize, hops;  i8 snrQ4
//    64   i16  rssi;  u8 advType;  u8 hasPosition
//    68   i32  latE6;  i32 lonE6
//    76   char name[32]        NUL-padded, no terminator stored
//
// A reader checks the whole image before anything of it is used - the header, the version, the
// length against what was read, the checksum, and then every record's bounds against the length
// and against the buffer and the ring it will go into - and says in one phrase why it refused.
// Nothing is half-read: a torn, short, foreign or corrupt image is refused whole.
//
// **The image may hold nothing a result does not.** A result body is what the runner would post,
// and the runner's rule keeps every credential out of it; an advert is what the ring holds. There
// is no field here for anything else, which is the point of writing the layout down.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/Observations.h>

namespace mm {

constexpr uint32_t kKeptImageMagic = 0x5450454Bu;  // "KEPT", read as a little-endian u32
constexpr uint16_t kKeptImageVersion = 1;
constexpr size_t kKeptImageHeaderLen = 32;
constexpr size_t kKeptResultHeaderLen = 12;
constexpr size_t kKeptAdvertLen = 108;

// CRC-32/ISO-HDLC: polynomial 0xEDB88320 reflected, initial 0xFFFFFFFF, final complement - the
// one zlib's `crc32()` and the ESP32 ROM's `esp_rom_crc32_le()` compute, so a consumer can check
// an image with what it has. Bitwise, with no table: it runs once a wake over a few kilobytes.
uint32_t crc32Of(const uint8_t* bytes, size_t len);

class KeptImageWriter {
 public:
  // Writes into `out`, never past `cap`. A `cap` smaller than the header fits nothing.
  KeptImageWriter(uint8_t* out, size_t cap);

  // One result, whole. False - and nothing written - when it does not fit, or when an advert has
  // already been added: results come first, and the order is the image's rule.
  bool addResult(long long jobId, const char* body, size_t len);
  // One advert, whole, with the epoch it was heard at (0 when no clock knew). False when it does
  // not fit.
  bool addAdvert(const HeardAdvert& advert, uint32_t heardAtEpoch);
  // The header and the checksum: the image is complete after this and nothing more may be added.
  // Returns the image's length, 0 when the cap could not hold a header.
  size_t finish(uint32_t resultsDropped, uint32_t observationsDropped, uint32_t imagesRefused);

  uint32_t crc() const { return crc_; }
  size_t length() const { return len_; }
  size_t resultCount() const { return results_; }
  size_t advertCount() const { return adverts_; }

 private:
  uint8_t* out_;
  size_t cap_;
  size_t len_;
  size_t results_ = 0;
  size_t adverts_ = 0;
  uint32_t crc_ = 0;
  bool finished_ = false;
};

class KeptImageReader {
 public:
  // The whole image checked before any of it is read: nullptr when it is good, otherwise one
  // static phrase saying why it is refused, for the log line. `len` is what the store handed
  // back, which may be more than the image is long.
  const char* check(const uint8_t* image, size_t len);

  // Readable after a `check` that returned nullptr.
  uint16_t version() const { return version_; }
  size_t resultCount() const { return resultCount_; }
  size_t advertCount() const { return advertCount_; }
  size_t length() const { return length_; }
  uint32_t crc() const { return crc_; }
  uint32_t resultsDropped() const { return resultsDropped_; }
  uint32_t observationsDropped() const { return observationsDropped_; }
  uint32_t imagesRefused() const { return imagesRefused_; }

  // The result at `i` (oldest first): its body and its length, nullptr past the count.
  const char* result(size_t i, long long& jobId, size_t& len) const;
  // The advert at `i` (oldest first) into `out`, with `heardAtEpoch` set and `heardAtMillis` 0 -
  // the uptime clock it was heard on is gone with the sleep, and the caller stamps it with the
  // clock it has. False past the count.
  bool advert(size_t i, HeardAdvert& out) const;

 private:
  const uint8_t* image_ = nullptr;
  size_t length_ = 0;
  size_t advertsAt_ = 0;  // where the first advert record starts
  uint16_t version_ = 0;
  size_t resultCount_ = 0;
  size_t advertCount_ = 0;
  uint32_t crc_ = 0;
  uint32_t resultsDropped_ = 0;
  uint32_t observationsDropped_ = 0;
  uint32_t imagesRefused_ = 0;
};

}  // namespace mm
