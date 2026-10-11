// SHA-512 (task p2-01), FIPS PUB 180-4 §6.4, as RFC 8032's Ed25519 uses it over R || A || M.
// Streaming, like Sha256, so that a manifest is hashed where it lies and never copied into the
// signed-message buffer TweetNaCl's crypto_sign_open wants. The compression function and the
// initial state are TweetNaCl's own (third_party/tweetnacl/tweetnacl_verify.c, verbatim); only
// the buffering and the padding are written here, and finish() is held byte for byte against the
// original one-shot crypto_hash for every length from 0 to 300 in host_test/core/image/
// test_sha512.cpp, beside the FIPS vectors. One source for the board and the host: this file.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace image {

constexpr std::size_t kSha512Size = 64;

class Sha512 {
 public:
  Sha512() { reset(); }
  void reset();
  void update(const std::uint8_t* data, std::size_t size);
  // The digest of everything fed so far; the object is then reset.
  void finish(std::uint8_t out[kSha512Size]);

 private:
  std::uint8_t state_[kSha512Size];  // the 64 digest bytes, big-endian, as TweetNaCl keeps them
  std::uint8_t buffer_[128];
  std::size_t buffered_;
  std::uint64_t total_;
};

// One call for a message that is all in memory.
void sha512(const std::uint8_t* data, std::size_t size, std::uint8_t out[kSha512Size]);

}  // namespace image
}  // namespace core
