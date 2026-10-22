#include <agent/KeptImage.h>

#include <cstring>

#include <agent/ResultBuffer.h>

namespace mm {

namespace {

constexpr size_t kAtMagic = 0;
constexpr size_t kAtCrc = 4;
constexpr size_t kAtVersion = 8;
constexpr size_t kAtResultCount = 10;
constexpr size_t kAtAdvertCount = 12;
constexpr size_t kAtReserved = 14;
constexpr size_t kAtLength = 16;
constexpr size_t kAtResultsDropped = 20;
constexpr size_t kAtObservationsDropped = 24;
constexpr size_t kAtImagesRefused = 28;

// The checksum starts after itself: the magic and the CRC field are the two things it does not
// cover, so a torn header is a bad magic or a bad CRC and never a CRC over its own stale value.
constexpr size_t kCrcFrom = 8;

// Little-endian, byte by byte: the image's layout and not the compiler's.
void put16(uint8_t* at, uint16_t v) {
  at[0] = static_cast<uint8_t>(v);
  at[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* at, uint32_t v) {
  at[0] = static_cast<uint8_t>(v);
  at[1] = static_cast<uint8_t>(v >> 8);
  at[2] = static_cast<uint8_t>(v >> 16);
  at[3] = static_cast<uint8_t>(v >> 24);
}
void put64(uint8_t* at, uint64_t v) {
  put32(at, static_cast<uint32_t>(v));
  put32(at + 4, static_cast<uint32_t>(v >> 32));
}
uint16_t get16(const uint8_t* at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }
uint32_t get32(const uint8_t* at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
         (static_cast<uint32_t>(at[3]) << 24);
}
uint64_t get64(const uint8_t* at) { return static_cast<uint64_t>(get32(at)) | (static_cast<uint64_t>(get32(at + 4)) << 32); }

}  // namespace

uint32_t crc32Of(const uint8_t* bytes, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---- the writer ----------------------------------------------------------------------------------

KeptImageWriter::KeptImageWriter(uint8_t* out, size_t cap)
    : out_(out), cap_(out == nullptr ? 0 : cap), len_(cap_ >= kKeptImageHeaderLen ? kKeptImageHeaderLen : 0) {}

bool KeptImageWriter::addResult(long long jobId, const char* body, size_t len) {
  if (finished_ || len_ == 0 || adverts_ > 0 || body == nullptr || len == 0) return false;
  if (results_ >= ResultBuffer::kMaxResults || results_ >= 0xFFFFu) return false;
  if (kKeptResultHeaderLen + len > cap_ - len_) return false;
  uint8_t* at = out_ + len_;
  put64(at, static_cast<uint64_t>(jobId));
  put32(at + 8, static_cast<uint32_t>(len));
  std::memcpy(at + kKeptResultHeaderLen, body, len);
  len_ += kKeptResultHeaderLen + len;
  results_++;
  return true;
}

bool KeptImageWriter::addAdvert(const HeardAdvert& a, uint32_t heardAtEpoch) {
  if (finished_ || len_ == 0) return false;
  if (adverts_ >= ObservationRing::kCapacity || adverts_ >= 0xFFFFu) return false;
  if (kKeptAdvertLen > cap_ - len_) return false;
  uint8_t* at = out_ + len_;
  std::memset(at, 0, kKeptAdvertLen);
  std::memcpy(at, a.publicKey, kPublicKeyLen);
  put32(at + 32, heardAtEpoch);
  std::memcpy(at + 36, a.pathBytes, kMaxObservationPathBytes);
  at[60] = a.pathByteLen;
  at[61] = a.pathHashSize;
  at[62] = a.hops;
  at[63] = static_cast<uint8_t>(a.snrQ4);
  put16(at + 64, static_cast<uint16_t>(a.rssi));
  at[66] = a.advType;
  at[67] = a.hasPosition ? 1 : 0;
  put32(at + 68, static_cast<uint32_t>(a.latE6));
  put32(at + 72, static_cast<uint32_t>(a.lonE6));
  std::memcpy(at + 76, a.name, kMaxObservationNameLen);  // the NUL at [32] is not stored
  len_ += kKeptAdvertLen;
  adverts_++;
  return true;
}

size_t KeptImageWriter::finish(uint32_t resultsDropped, uint32_t observationsDropped, uint32_t imagesRefused) {
  if (len_ == 0) return 0;
  finished_ = true;
  put32(out_ + kAtMagic, kKeptImageMagic);
  put16(out_ + kAtVersion, kKeptImageVersion);
  put16(out_ + kAtResultCount, static_cast<uint16_t>(results_));
  put16(out_ + kAtAdvertCount, static_cast<uint16_t>(adverts_));
  put16(out_ + kAtReserved, 0);
  put32(out_ + kAtLength, static_cast<uint32_t>(len_));
  put32(out_ + kAtResultsDropped, resultsDropped);
  put32(out_ + kAtObservationsDropped, observationsDropped);
  put32(out_ + kAtImagesRefused, imagesRefused);
  crc_ = crc32Of(out_ + kCrcFrom, len_ - kCrcFrom);
  put32(out_ + kAtCrc, crc_);
  return len_;
}

// ---- the reader ----------------------------------------------------------------------------------

const char* KeptImageReader::check(const uint8_t* image, size_t len) {
  image_ = nullptr;
  length_ = 0;
  resultCount_ = 0;
  advertCount_ = 0;
  if (image == nullptr || len < kKeptImageHeaderLen) return "shorter than a header";
  if (get32(image + kAtMagic) != kKeptImageMagic) return "not a kept image";
  version_ = get16(image + kAtVersion);
  if (version_ != kKeptImageVersion) return "written by another version of the image";
  const uint32_t length = get32(image + kAtLength);
  if (length < kKeptImageHeaderLen || length > len) return "torn: its length runs past what the store holds";
  crc_ = get32(image + kAtCrc);
  if (crc32Of(image + kCrcFrom, length - kCrcFrom) != crc_) return "its checksum does not match";
  // The structure, record by record, against the length and against what it will be read into:
  // a count the buffer or the ring could not hold, or a record past the end, is a refusal and
  // never a partial read.
  const size_t results = get16(image + kAtResultCount);
  const size_t adverts = get16(image + kAtAdvertCount);
  if (results > ResultBuffer::kMaxResults || adverts > ObservationRing::kCapacity) return "its counts do not add up";
  size_t at = kKeptImageHeaderLen;
  size_t bodyBytes = 0;
  for (size_t i = 0; i < results; i++) {
    if (length - at < kKeptResultHeaderLen) return "its records do not add up";
    const uint32_t bodyLen = get32(image + at + 8);
    if (bodyLen == 0 || bodyLen > length - at - kKeptResultHeaderLen) return "its records do not add up";
    bodyBytes += bodyLen;
    if (bodyBytes > ResultBuffer::kCapacityBytes) return "its records do not add up";
    at += kKeptResultHeaderLen + bodyLen;
  }
  if ((length - at) != adverts * kKeptAdvertLen) return "its records do not add up";
  for (size_t i = 0; i < adverts; i++) {
    const uint8_t* a = image + at + i * kKeptAdvertLen;
    if (a[60] > kMaxObservationPathBytes || a[61] > 3) return "its records do not add up";
  }
  image_ = image;
  length_ = length;
  resultCount_ = results;
  advertCount_ = adverts;
  advertsAt_ = at;
  resultsDropped_ = get32(image + kAtResultsDropped);
  observationsDropped_ = get32(image + kAtObservationsDropped);
  imagesRefused_ = get32(image + kAtImagesRefused);
  return nullptr;
}

const char* KeptImageReader::result(size_t i, long long& jobId, size_t& len) const {
  jobId = 0;
  len = 0;
  if (image_ == nullptr || i >= resultCount_) return nullptr;
  // Eight at most, so walking to the i-th is cheaper than an index would be worth.
  size_t at = kKeptImageHeaderLen;
  for (size_t n = 0; n < i; n++) at += kKeptResultHeaderLen + get32(image_ + at + 8);
  jobId = static_cast<long long>(get64(image_ + at));
  len = get32(image_ + at + 8);
  return reinterpret_cast<const char*>(image_ + at + kKeptResultHeaderLen);
}

bool KeptImageReader::advert(size_t i, HeardAdvert& out) const {
  if (image_ == nullptr || i >= advertCount_) return false;
  const uint8_t* at = image_ + advertsAt_ + i * kKeptAdvertLen;
  out = HeardAdvert();
  std::memcpy(out.publicKey, at, kPublicKeyLen);
  out.heardAtEpoch = get32(at + 32);
  out.heardAtMillis = 0;
  std::memcpy(out.pathBytes, at + 36, kMaxObservationPathBytes);
  out.pathByteLen = at[60];
  out.pathHashSize = at[61];
  out.hops = at[62];
  out.snrQ4 = static_cast<int8_t>(at[63]);
  out.rssi = static_cast<int16_t>(get16(at + 64));
  out.advType = at[66];
  out.hasPosition = at[67] != 0;
  out.latE6 = static_cast<int32_t>(get32(at + 68));
  out.lonE6 = static_cast<int32_t>(get32(at + 72));
  std::memcpy(out.name, at + 76, kMaxObservationNameLen);
  out.name[kMaxObservationNameLen] = '\0';
  return true;
}

}  // namespace mm
