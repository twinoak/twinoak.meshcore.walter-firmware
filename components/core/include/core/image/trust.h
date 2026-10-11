// The trust table (task p2-01): the public keys a release manifest may be signed with, compiled
// into this build (spec/FIRMWARE-MANIFEST.md §3: "a Walter build trusts a small fixed list of
// keys (room for three)"). The table is generated at build time from keys/release-keys.txt by
// tools/gen_trust_table.py into trust_keys.h, which trust.cpp alone includes. Today that file
// names no key, so buildTrust() is empty and the verdict (verdict.h) refuses every manifest at
// check 1 - the safe state before the release key exists (spec/SYSTEM.md §10 item 1).
//
// The specification's test key (spec/fixtures/manifest/TEST-KEY.txt) is in the table only in a
// build made with -DTWINOAK_TRUST_TEST_KEY=1, which also stamps the version with +TESTKEY and
// makes main print a warning at every boot; tools/release/build_release.py refuses to release
// such a build, by its version and by the key's bytes in the image. The host tests use the test
// key through tables of their own, never through this build's.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/image/crypto/ed25519.h"

namespace core {
namespace image {

// §3: the key id is the first eight bytes of SHA-256 over the raw 32-byte public key, written as
// 16 lowercase hex characters.
constexpr std::size_t kKeyIdSize = 8;
constexpr std::size_t kKeyIdHexSize = 16;
constexpr std::size_t kMaxTrustedKeys = 3;

struct TrustedKey {
  std::uint8_t id[kKeyIdSize];
  std::uint8_t key[kEd25519PublicKeySize];
};

// A view over a fixed array of keys. The build's is buildTrust(); a test makes its own.
class TrustTable {
 public:
  constexpr TrustTable() = default;
  constexpr TrustTable(const TrustedKey* keys, std::size_t count)
      : keys_(keys), count_(count > kMaxTrustedKeys ? kMaxTrustedKeys : count) {}

  std::size_t count() const { return count_; }
  bool empty() const { return count_ == 0; }
  const TrustedKey& at(std::size_t i) const { return keys_[i]; }
  // The key with this id, or nullptr. An unknown id is a refusal in itself: the verifier never
  // tries the other keys (verdict.h, check 1).
  const TrustedKey* find(const std::uint8_t id[kKeyIdSize]) const;

 private:
  const TrustedKey* keys_ = nullptr;
  std::size_t count_ = 0;
};

// The keys this build trusts: keys/release-keys.txt, plus the test key in a +TESTKEY build.
const TrustTable& buildTrust();

// Whether this build trusts the specification's public test key. True only in a build made
// with -DTWINOAK_TRUST_TEST_KEY=1; such a build is never released.
bool trustsTestKey();

// §3's key id of a public key.
void keyIdOf(const std::uint8_t key[kEd25519PublicKeySize], std::uint8_t id[kKeyIdSize]);

// The id as 16 lowercase hex characters, terminated (out has room for 17).
void keyIdHex(const std::uint8_t id[kKeyIdSize], char out[kKeyIdHexSize + 1]);

}  // namespace image
}  // namespace core
