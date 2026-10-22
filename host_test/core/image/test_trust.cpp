// The trust table (task p2-01): the build's own - generated from keys/release-keys.txt, whatever
// that file names - and a table of the tests' own: lookup by id, the key id as section 3 defines
// it, the bound of three.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/image/trust.h"
#include "fixtures/manifest_files.h"

using core::image::kKeyIdSize;
using core::image::kMaxTrustedKeys;
using core::image::TrustedKey;
using core::image::TrustTable;

TEST_CASE("this build trusts the keys keys/release-keys.txt names, and never the test key") {
  // The release key was made on 2026-10-20 (spec/SYSTEM.md section 10 item 1) and its public half
  // is in keys/release-keys.txt, so the build's table is no longer empty. What the table must say
  // is what that file says - read here, never written down - and that the specification's public
  // test key is not among it: a build that trusted it would accept anybody's firmware (section 3).
  const TrustTable& build = core::image::buildTrust();
  const std::vector<std::string> named = fixtures::releaseKeyIds();
  CHECK(build.count() == named.size());
  CHECK(build.empty() == named.empty());
  for (std::size_t i = 0; i < named.size() && i < build.count(); ++i) {
    char hex[core::image::kKeyIdHexSize + 1];
    core::image::keyIdHex(build.at(i).id, hex);
    CHECK_MESSAGE(std::string(hex) == named[i], "key " << i << " of the build's table");
    CHECK(build.find(build.at(i).id) == &build.at(i));
  }
  CHECK_FALSE(core::image::trustsTestKey());
  const TrustedKey test = fixtures::testKey();
  CHECK(build.find(test.id) == nullptr);
  std::uint8_t zero[kKeyIdSize] = {};
  CHECK(build.find(zero) == nullptr);
  MESSAGE("keys/release-keys.txt names " << named.size() << " key(s); the build's table holds "
          << build.count());
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
