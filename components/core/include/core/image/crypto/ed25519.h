// Ed25519 verification (task p2-01): RFC 8032 §5.1.7, no pre-hash, no context - what
// spec/FIRMWARE-MANIFEST.md §3 signs a manifest with. Verification only: this firmware never
// signs anything, and the vendored code has no signing in it (third_party/tweetnacl/README.md).
//
// Two things are this file's and not TweetNaCl's:
//
//   - the hash of R || A || M is streamed (Sha512), so that a manifest is verified where it lies;
//   - S is checked to be below the group order L before the arithmetic runs (RFC 8032 §5.1.7
//     step 1), which TweetNaCl's crypto_sign_open does not do. Without it a signature stays valid
//     with S + L in place of S - a second encoding of the same signature, and a property this
//     firmware does not want to find out about later (spec: "what is signed is the bytes").
//
// Held to RFC 8032 §7.1's vectors, the spec's fixtures and the original crypto_sign_open in
// host_test/core/image/test_ed25519.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/image/crypto/sha512.h"

namespace core {
namespace image {

constexpr std::size_t kEd25519PublicKeySize = 32;
constexpr std::size_t kEd25519SignatureSize = 64;

// S (the second half of a signature, little-endian) is in the range 0 <= S < L.
bool ed25519ScalarIsCanonical(const std::uint8_t s[32]);

// The message in pieces: begin(), update() as the bytes come, finish() for the verdict. A
// verifier may be reused; begin() resets it.
class Ed25519Verifier {
 public:
  void begin(const std::uint8_t public_key[kEd25519PublicKeySize],
             const std::uint8_t signature[kEd25519SignatureSize]);
  void update(const std::uint8_t* data, std::size_t size);
  // True only when the signature verifies over everything fed since begin().
  bool finish();

 private:
  Sha512 hash_;
  std::uint8_t public_key_[kEd25519PublicKeySize] = {};
  std::uint8_t signature_[kEd25519SignatureSize] = {};
  bool begun_ = false;
};

// One call for a message that is all in memory.
bool ed25519Verify(const std::uint8_t public_key[kEd25519PublicKeySize],
                   const std::uint8_t signature[kEd25519SignatureSize], const std::uint8_t* message,
                   std::size_t size);

// RFC 8032 section 7.1 TEST 1 verified, and the same signature with one bit changed refused:
// what `trust` on the console runs, so that a bench can see the arithmetic is right on the
// board's compiler, where no host test runs.
bool ed25519SelfTest();

}  // namespace image
}  // namespace core
