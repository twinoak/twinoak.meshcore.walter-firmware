// SHA-256 (task p1-05), FIPS PUB 180-4 §6.2: the digest the flasher holds a read-back image
// against (spec/FIRMWARE-MANIFEST.md §2: a file is identified by its hash), and the one phase 2's
// image checks need on the host and on the board alike. Streaming, so that bytes arriving in
// pieces - a page at a time from the radio's flash, a range at a time from a download - can be
// hashed without a buffer the size of the image. No ESP-IDF include, no heap, no table beyond the
// 64 round constants. Held to the FIPS test vectors ("abc", the empty message, the two-block
// message, a million 'a') in host_test/core/image/test_sha256.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace image {

constexpr std::size_t kSha256Size = 32;

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const std::uint8_t* data, std::size_t size);
  // The digest of everything fed so far; the object is then reset.
  void finish(std::uint8_t out[kSha256Size]);

 private:
  void block(const std::uint8_t* p);

  std::uint32_t h_[8];
  std::uint8_t buffer_[64];
  std::size_t buffered_;
  std::uint64_t total_;
};

// One call for a message that is all in memory.
void sha256(const std::uint8_t* data, std::size_t size, std::uint8_t out[kSha256Size]);

// Constant-time comparison of two digests: every byte is read whatever the first mismatch.
bool sha256Equal(const std::uint8_t a[kSha256Size], const std::uint8_t b[kSha256Size]);

}  // namespace image
}  // namespace core
