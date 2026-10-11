#include "core/image/crypto/ed25519.h"

#include <tweetnacl/tweetnacl_verify.h>

namespace core {
namespace image {

namespace {

// The group order L = 2^252 + 27742317777372353535851937790883648493, little-endian (RFC 8032
// §5.1, and the same bytes as TweetNaCl's `L`).
constexpr std::uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                 0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                 0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

}  // namespace

// S < L, compared from the most significant byte down. Not constant-time, and need not be: S is
// in the signature, which is public.
bool ed25519ScalarIsCanonical(const std::uint8_t s[32]) {
  for (int i = 31; i >= 0; --i) {
    if (s[i] < kL[i]) return true;
    if (s[i] > kL[i]) return false;
  }
  return false;  // S == L
}

void Ed25519Verifier::begin(const std::uint8_t public_key[kEd25519PublicKeySize],
                            const std::uint8_t signature[kEd25519SignatureSize]) {
  for (std::size_t i = 0; i < kEd25519PublicKeySize; ++i) public_key_[i] = public_key[i];
  for (std::size_t i = 0; i < kEd25519SignatureSize; ++i) signature_[i] = signature[i];
  // RFC 8032 §5.1.7 step 2: SHA-512(R || A || M). R is the first half of the signature.
  hash_.reset();
  hash_.update(signature_, 32);
  hash_.update(public_key_, kEd25519PublicKeySize);
  begun_ = true;
}

void Ed25519Verifier::update(const std::uint8_t* data, std::size_t size) {
  if (begun_) hash_.update(data, size);
}

bool Ed25519Verifier::finish() {
  if (!begun_) return false;
  begun_ = false;
  std::uint8_t hram[kSha512Size];
  hash_.finish(hram);
  // Step 1 before anything else: a non-canonical S is a refusal, not a computation.
  if (!ed25519ScalarIsCanonical(signature_ + 32)) return false;
  return twinoak_tweetnacl_ed25519_check(public_key_, signature_, hram) == 0;
}

bool ed25519Verify(const std::uint8_t public_key[kEd25519PublicKeySize],
                   const std::uint8_t signature[kEd25519SignatureSize], const std::uint8_t* message,
                   std::size_t size) {
  Ed25519Verifier v;
  v.begin(public_key, signature);
  v.update(message, size);
  return v.finish();
}

namespace {

// RFC 8032 section 7.1, TEST 1: the empty message (the same vector spec/fixtures/manifest/
// index.json carries as rfc8032_test_1).
constexpr std::uint8_t kTest1PublicKey[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                                              0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                                              0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
constexpr std::uint8_t kTest1Signature[64] = {
    0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
    0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
    0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
    0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};

}  // namespace

bool ed25519SelfTest() {
  if (!ed25519Verify(kTest1PublicKey, kTest1Signature, nullptr, 0)) return false;
  std::uint8_t changed[64];
  for (std::size_t i = 0; i < 64; ++i) changed[i] = kTest1Signature[i];
  changed[7] ^= 0x01;
  return !ed25519Verify(kTest1PublicKey, changed, nullptr, 0);
}

}  // namespace image
}  // namespace core
