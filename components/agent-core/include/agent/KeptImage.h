// The image a sleep keeps: its layout, its checksum, a writer and a reader (task p1.6-22).
//
// What the loop writes into an IRetained store at the end of a wake, and reads back after the
// next boot (agent/IRetained.h says when and why). One image: a header, then the kept results
// **oldest first**, then the ring's waiting adverts oldest first, then - since version 2 - the
// client id of the batch that is still waiting for an answer. Results before adverts because
// a lost result leaves a job leased until the server's lease lapses, and a lost advert is heard
// again. Each is written whole or not at all, in order, and the writer stops at the first that
// does not fit; what it stopped at and everything behind it is what the loop drops and counts.
//
// **Version 2 keeps the pending batch** (task p1.13-55). A batch that was posted and whose answer
// never came must go out again under the id it was built with, because the server de-duplicates
// on that id; until this version the id lived only in the loop's RAM, so a board that slept woke
// with the adverts and no id, built a new batch of them, and the server stored every one of them
// a second time. The id and how many of the adverts below it holds are in the image now, and the
// batch's adverts are by construction the oldest ones - the ring is drained oldest first and the
// image is written oldest first - so `batchCount` names the first `batchCount` advert records.
// A **version 1** image is still read, as an image with no batch pending; any other version is
// refused whole.
//
// The layout is bytes, little-endian, and not a struct copied: a unit lives with this format
// across thousands of sleeps and a firmware update, and a struct's padding is the compiler's.
//
//   header, 32 bytes
//     0   u32  magic           "KEPT" as the four bytes 4B 45 50 54
//     4   u32  crc             CRC-32 (ISO-HDLC, as zlib's crc32 and the ESP ROM compute it) over
//                              every byte from offset 8 to `length`
//     8   u16  version         kKeptImageVersion, or kKeptImageVersionWithoutBatch, which is read
//                              as an image with no batch pending; any other is refused whole
//    10   u16  resultCount     at most ResultBuffer::kMaxResults
//    12   u16  advertCount     at most ObservationRing::kCapacity
//    14   u16  batchCount      version 2: how many of the advert records below - the first of
//                              them, which are the oldest - are the batch still waiting for an
//                              answer, and 0 when none is. At most kObservationBatchSize, and
//                              never more than advertCount. Version 1's `reserved`, which that
//                              version always wrote 0 and a reader of it ignores
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
//   batch id, 40 bytes (kKeptBatchIdLen), **only when batchCount is not 0**, last of all
//     0   char id[40]          the client id the batch was built with, NUL-terminated and
//                              NUL-padded. Last so that every record before it is at the offset
//                              a version 1 image puts it at, and so that an image with no batch
//                              pending is the length it always was
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
#include <agent/ResultBuffer.h>

namespace mm {

constexpr uint32_t kKeptImageMagic = 0x5450454Bu;  // "KEPT", read as a little-endian u32
constexpr uint16_t kKeptImageVersion = 2;
// The version before the pending batch was kept (task p1.13-55). Read, never written: an image a
// board wrote before it was updated is taken at the first boot after, as an image with no batch
// pending. It is not a second format - version 2 only appends a block version 1 never had - and
// where that block is absent the two are the same bytes.
constexpr uint16_t kKeptImageVersionWithoutBatch = 1;
constexpr size_t kKeptImageHeaderLen = 32;
constexpr size_t kKeptResultHeaderLen = 12;
constexpr size_t kKeptAdvertLen = 108;
// The client id of a pending batch, NUL-terminated inside this block. Sized to AgentLoop's own
// `batchId_`, which holds "obs-" and thirty-two hex digits with room to spare.
constexpr size_t kKeptBatchIdLen = 40;

// **The largest image this library can write**, for whoever sizes the scratch and the store: a
// full result buffer (every one of its bytes, in as many results as it holds), a full ring, and a
// pending batch's id. A buffer of this many bytes can never be the reason something is dropped,
// and a smaller one drops the newest of what is kept and counts it. It is a constant and not a
// sum done by hand so that it moves when the library moves (task p1.13-55).
constexpr size_t kMaxKeptImageLen = kKeptImageHeaderLen + ResultBuffer::kMaxResults * kKeptResultHeaderLen +
                                    ResultBuffer::kCapacityBytes + ObservationRing::kCapacity * kKeptAdvertLen +
                                    kKeptBatchIdLen;

// CRC-32/ISO-HDLC: polynomial 0xEDB88320 reflected, initial 0xFFFFFFFF, final complement - the
// one zlib's `crc32()` and the ESP32 ROM's `esp_rom_crc32_le()` compute, so a consumer can check
// an image with what it has. Bitwise, with no table: it runs once a wake over a few kilobytes.
uint32_t crc32Of(const uint8_t* bytes, size_t len);

class KeptImageWriter {
 public:
  // Writes into `out`, never past `cap`. A `cap` smaller than the header fits nothing.
  KeptImageWriter(uint8_t* out, size_t cap);

  // The batch still waiting for an answer: its client id and how many of the ring's oldest
  // adverts it holds (task p1.13-55). **Before the first `addResult` or `addAdvert`**, because
  // the id's block is held back at the tail from here on and the records are fitted into what is
  // left. A `count` of 0, a null or empty id, or a `cap` with no room for the block, is no batch.
  //
  // The batch is only written when the image ends up holding every advert of it - `finish` drops
  // it otherwise - because a batch missing an advert is not that batch any more, and a resend
  // under its id would have the server de-duplicate the whole of it away.
  void setPendingBatch(const char* id, size_t count);
  // The batch is not the ring's oldest adverts after all, so it is not kept and the block held
  // back for it goes back to the records. For an advert inside the batch that nothing can date,
  // which the writer is never handed (AgentLoop::keepAcrossSleep).
  void forgetPendingBatch();

  // One result, whole. False - and nothing written - when it does not fit, or when an advert has
  // already been added: results come first, and the order is the image's rule.
  bool addResult(long long jobId, const char* body, size_t len);
  // One advert, whole, with the epoch it was heard at (0 when no clock knew). False when it does
  // not fit.
  bool addAdvert(const HeardAdvert& advert, uint32_t heardAtEpoch);
  // The header, the pending batch's id when the image holds all of it, and the checksum: the
  // image is complete after this and nothing more may be added. Returns the image's length, 0
  // when the cap could not hold a header.
  size_t finish(uint32_t resultsDropped, uint32_t observationsDropped, uint32_t imagesRefused);

  uint32_t crc() const { return crc_; }
  size_t length() const { return len_; }
  size_t resultCount() const { return results_; }
  size_t advertCount() const { return adverts_; }
  // How many adverts the pending batch holds **as the image was written**: readable after
  // `finish`, and 0 when there was none or the image could not hold all of it.
  size_t batchCount() const { return batchCount_; }

 private:
  uint8_t* out_;
  size_t cap_;
  size_t len_;
  size_t results_ = 0;
  size_t adverts_ = 0;
  uint32_t crc_ = 0;
  bool finished_ = false;
  // The pending batch, until `finish` decides whether the image holds all of it. `tail_` is the
  // room held back for its block while the records are added.
  char pendingId_[kKeptBatchIdLen] = {0};
  size_t pendingCount_ = 0;
  size_t tail_ = 0;
  size_t batchCount_ = 0;
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
  // The batch that was still waiting for an answer when the image was written: how many of the
  // adverts - the first of them, the oldest - it holds, 0 when none was, and the client id it
  // was built with, "" when none was. A version 1 image has none (task p1.13-55).
  size_t batchCount() const { return batchCount_; }
  const char* batchId() const { return batchId_; }
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
  size_t batchCount_ = 0;
  char batchId_[kKeptBatchIdLen] = {0};
  uint32_t crc_ = 0;
  uint32_t resultsDropped_ = 0;
  uint32_t observationsDropped_ = 0;
  uint32_t imagesRefused_ = 0;
};

}  // namespace mm
