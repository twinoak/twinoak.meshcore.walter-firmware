#include "core/image/crypto/sha512.h"

#include <tweetnacl/tweetnacl_verify.h>

namespace core {
namespace image {

void Sha512::reset() {
  twinoak_tweetnacl_sha512_iv(state_);
  buffered_ = 0;
  total_ = 0;
}

void Sha512::update(const std::uint8_t* data, std::size_t size) {
  total_ += size;
  while (size > 0) {
    if (buffered_ == 0 && size >= 128) {
      // Whole blocks straight from the caller's buffer; what is left is fewer than 128 bytes.
      const std::size_t whole = size - (size % 128);
      twinoak_tweetnacl_hashblocks(state_, data, whole);
      data += whole;
      size -= whole;
      continue;
    }
    const std::size_t take = (128 - buffered_) < size ? (128 - buffered_) : size;
    for (std::size_t i = 0; i < take; ++i) buffer_[buffered_ + i] = data[i];
    buffered_ += take;
    data += take;
    size -= take;
    if (buffered_ == 128) {
      twinoak_tweetnacl_hashblocks(state_, buffer_, 128);
      buffered_ = 0;
    }
  }
}

// FIPS 180-4 §5.1.2: a 1 bit, zeros to 896 bits modulo 1024, then the length in bits as 128 bits
// big-endian - the same arithmetic as TweetNaCl's crypto_hash tail, which this is held against.
void Sha512::finish(std::uint8_t out[kSha512Size]) {
  std::uint8_t x[256] = {};
  const std::size_t n = buffered_;
  for (std::size_t i = 0; i < n; ++i) x[i] = buffer_[i];
  x[n] = 0x80;
  const std::size_t blocks = n < 112 ? 128 : 256;
  const std::uint64_t bits_low = total_ << 3;
  const std::uint64_t bits_high = total_ >> 61;
  for (unsigned i = 0; i < 8; ++i) {
    x[blocks - 16 + i] = static_cast<std::uint8_t>(bits_high >> (56 - 8 * i));
    x[blocks - 8 + i] = static_cast<std::uint8_t>(bits_low >> (56 - 8 * i));
  }
  twinoak_tweetnacl_hashblocks(state_, x, blocks);
  for (std::size_t i = 0; i < kSha512Size; ++i) out[i] = state_[i];
  reset();
}

void sha512(const std::uint8_t* data, std::size_t size, std::uint8_t out[kSha512Size]) {
  Sha512 s;
  s.update(data, size);
  s.finish(out);
}

}  // namespace image
}  // namespace core
