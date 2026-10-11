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

}  // namespace image
}  // namespace core
