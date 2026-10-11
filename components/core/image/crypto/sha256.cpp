#include "core/image/crypto/sha256.h"

namespace core {
namespace image {

namespace {

// FIPS 180-4 §4.2.2: the first thirty-two bits of the fractional parts of the cube roots of the
// first sixty-four primes.
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
inline std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (~x & z); }
inline std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline std::uint32_t bsig0(std::uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline std::uint32_t bsig1(std::uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline std::uint32_t ssig0(std::uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline std::uint32_t ssig1(std::uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

}  // namespace

// §5.3.3: the initial hash value, the first thirty-two bits of the fractional parts of the square
// roots of the first eight primes.
void Sha256::reset() {
  h_[0] = 0x6a09e667;
  h_[1] = 0xbb67ae85;
  h_[2] = 0x3c6ef372;
  h_[3] = 0xa54ff53a;
  h_[4] = 0x510e527f;
  h_[5] = 0x9b05688c;
  h_[6] = 0x1f83d9ab;
  h_[7] = 0x5be0cd19;
  buffered_ = 0;
  total_ = 0;
}

// §6.2.2: one 512-bit block.
void Sha256::block(const std::uint8_t* p) {
  std::uint32_t w[64];
  for (unsigned t = 0; t < 16; ++t) {
    w[t] = (static_cast<std::uint32_t>(p[4 * t]) << 24) | (static_cast<std::uint32_t>(p[4 * t + 1]) << 16) |
           (static_cast<std::uint32_t>(p[4 * t + 2]) << 8) | static_cast<std::uint32_t>(p[4 * t + 3]);
  }
  for (unsigned t = 16; t < 64; ++t) w[t] = ssig1(w[t - 2]) + w[t - 7] + ssig0(w[t - 15]) + w[t - 16];
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (unsigned t = 0; t < 64; ++t) {
    const std::uint32_t t1 = h + bsig1(e) + ch(e, f, g) + kK[t] + w[t];
    const std::uint32_t t2 = bsig0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) {
  total_ += size;
  while (size > 0) {
    if (buffered_ == 0 && size >= 64) {
      block(data);
      data += 64;
      size -= 64;
      continue;
    }
    const std::size_t take = (64 - buffered_) < size ? (64 - buffered_) : size;
    for (std::size_t i = 0; i < take; ++i) buffer_[buffered_ + i] = data[i];
    buffered_ += take;
    data += take;
    size -= take;
    if (buffered_ == 64) {
      block(buffer_);
      buffered_ = 0;
    }
  }
}

// §5.1.1: a 1 bit, zeros to 448 bits modulo 512, the length in bits as 64 bits big-endian.
void Sha256::finish(std::uint8_t out[kSha256Size]) {
  const std::uint64_t bits = total_ * 8u;
  std::uint8_t pad[72] = {0x80};
  const std::size_t pad_len = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  update(pad, pad_len);  // total_ grows, but `bits` was taken first
  std::uint8_t len[8];
  for (unsigned i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  for (unsigned i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
    out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
    out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
    out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
  }
  reset();
}

void sha256(const std::uint8_t* data, std::size_t size, std::uint8_t out[kSha256Size]) {
  Sha256 s;
  s.update(data, size);
  s.finish(out);
}

bool sha256Equal(const std::uint8_t a[kSha256Size], const std::uint8_t b[kSha256Size]) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < kSha256Size; ++i) diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0;
}

}  // namespace image
}  // namespace core
