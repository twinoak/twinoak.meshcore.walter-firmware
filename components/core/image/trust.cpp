#include "core/image/trust.h"

#include "core/image/crypto/sha256.h"
// Generated at build time from keys/release-keys.txt by tools/gen_trust_table.py (both
// CMake projects run it): generated::kTrustedKeys, kTrustedKeyCount, kTrustsTestKey.
#include "trust_keys.h"

namespace core {
namespace image {

static_assert(generated::kTrustedKeyCount <= kMaxTrustedKeys,
              "the trust table has room for three keys (spec/FIRMWARE-MANIFEST.md section 3)");
// The generated table and the build flag must agree: a table holding the test key can only come
// from a build that asked for it, and a build that asked for it must have got it.
#ifdef TWINOAK_TRUST_TEST_KEY
static_assert(generated::kTrustsTestKey,
              "TWINOAK_TRUST_TEST_KEY is set but the generated trust table has no test key");
#else
static_assert(!generated::kTrustsTestKey,
              "the generated trust table holds the test key but TWINOAK_TRUST_TEST_KEY is not set");
#endif

namespace {

// Constant-initialised: a view over the generated constexpr array.
constexpr TrustTable kBuildTrust(generated::kTrustedKeys, generated::kTrustedKeyCount);

}  // namespace

const TrustedKey* TrustTable::find(const std::uint8_t id[kKeyIdSize]) const {
  for (std::size_t i = 0; i < count_; ++i) {
    bool same = true;
    for (std::size_t j = 0; j < kKeyIdSize; ++j) same = same && keys_[i].id[j] == id[j];
    if (same) return &keys_[i];
  }
  return nullptr;
}

const TrustTable& buildTrust() { return kBuildTrust; }

bool trustsTestKey() { return generated::kTrustsTestKey; }

void keyIdOf(const std::uint8_t key[kEd25519PublicKeySize], std::uint8_t id[kKeyIdSize]) {
  std::uint8_t digest[kSha256Size];
  sha256(key, kEd25519PublicKeySize, digest);
  for (std::size_t i = 0; i < kKeyIdSize; ++i) id[i] = digest[i];
}

void keyIdHex(const std::uint8_t id[kKeyIdSize], char out[kKeyIdHexSize + 1]) {
  const char* digits = "0123456789abcdef";
  for (std::size_t i = 0; i < kKeyIdSize; ++i) {
    out[2 * i] = digits[id[i] >> 4];
    out[2 * i + 1] = digits[id[i] & 0x0F];
  }
  out[kKeyIdHexSize] = 0;
}

}  // namespace image
}  // namespace core
