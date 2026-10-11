// The trust table (task p2-01): the build's own - generated from keys/release-keys.txt, which
// names no key today - and a table of the tests' own: lookup by id, the key id as section 3
// defines it, the bound of three.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/image/trust.h"
#include "fixtures/manifest_files.h"

using core::image::kKeyIdSize;
using core::image::kMaxTrustedKeys;
using core::image::TrustedKey;
using core::image::TrustTable;

TEST_CASE("this build trusts no key: keys/release-keys.txt names none, and the test key is not in it") {
  const TrustTable& build = core::image::buildTrust();
  CHECK(build.empty());
  CHECK(build.count() == 0);
  CHECK_FALSE(core::image::trustsTestKey());
  const TrustedKey test = fixtures::testKey();
  CHECK(build.find(test.id) == nullptr);
  std::uint8_t zero[kKeyIdSize] = {};
  CHECK(build.find(zero) == nullptr);
}

TEST_CASE("the key id is the first eight bytes of SHA-256 over the public key, as 16 lowercase hex characters") {
  const TrustedKey test = fixtures::testKey();
  char hex[core::image::kKeyIdHexSize + 1];
  core::image::keyIdHex(test.id, hex);
  CHECK(std::string(hex) == std::string(fixtures::kTestKeyId));
  CHECK(std::string(hex) == "c17969deb0026955");  // spec/fixtures/manifest/TEST-KEY.txt's "key id"
  CHECK(std::strlen(hex) == 16);
}

TEST_CASE("a table of three: found by id, an unknown id is nullptr, a fourth key is not held") {
  TrustedKey keys[4] = {};
  for (std::uint8_t k = 0; k < 4; ++k) {
    for (std::size_t i = 0; i < 32; ++i) keys[k].key[i] = static_cast<std::uint8_t>(k * 31 + i);
    core::image::keyIdOf(keys[k].key, keys[k].id);
  }
  const TrustTable table(keys, 4);
  CHECK(table.count() == kMaxTrustedKeys);  // the fourth is not seen
  CHECK_FALSE(table.empty());
  for (std::uint8_t k = 0; k < 3; ++k) {
    CHECK(table.find(keys[k].id) == &keys[k]);
    CHECK(&table.at(k) == &keys[k]);
  }
  CHECK(table.find(keys[3].id) == nullptr);
  std::uint8_t almost[kKeyIdSize];
  std::memcpy(almost, keys[1].id, kKeyIdSize);
  almost[7] ^= 1;
  CHECK(table.find(almost) == nullptr);
  // Two ids differing only in the last byte are two ids.
  CHECK(std::memcmp(keys[0].id, keys[1].id, kKeyIdSize) != 0);
  // The default table holds nothing.
  const TrustTable none;
  CHECK(none.empty());
  CHECK(none.find(keys[0].id) == nullptr);
  // The test key's table of the tests.
  const fixtures::TestTrust trust;
  CHECK(trust.table.count() == 1);
  CHECK(trust.table.find(fixtures::testKey().id) == &trust.keys[0]);
}
