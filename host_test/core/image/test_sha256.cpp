// SHA-256 (task p1-05) against FIPS 180-4's examples and NIST's long-message vector: "abc", the
// empty message, the two-block message, a million 'a'; streamed in odd pieces against one call.
#include <doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/image/crypto/sha256.h"

using core::image::kSha256Size;
using core::image::Sha256;

namespace {

std::string hex(const std::uint8_t* d) {
  std::string s;
  char buf[3];
  for (std::size_t i = 0; i < kSha256Size; ++i) {
    std::snprintf(buf, sizeof buf, "%02x", d[i]);
    s += buf;
  }
  return s;
}

std::string digestOf(const std::string& text) {
  std::uint8_t out[kSha256Size];
  core::image::sha256(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), out);
  return hex(out);
}

}  // namespace

TEST_CASE("SHA-256: FIPS 180-4 'abc', the empty message and the two-block message") {
  CHECK(digestOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(digestOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(digestOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256: a million 'a', fed in pieces of every awkward size, is NIST's vector") {
  Sha256 s;
  std::vector<std::uint8_t> chunk(1000, 'a');
  std::size_t fed = 0;
  std::size_t piece = 1;
  while (fed < 1000000) {
    const std::size_t n = (piece % 131) + 1;  // 1..131: straddling the 64-byte block in every way
    const std::size_t take = n < 1000000 - fed ? n : 1000000 - fed;
    s.update(chunk.data(), take);
    fed += take;
    piece++;
  }
  std::uint8_t out[kSha256Size];
  s.finish(out);
  CHECK(hex(out) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // finish() reset it: the same object hashes "abc" afresh.
  s.update(reinterpret_cast<const std::uint8_t*>("abc"), 3);
  s.finish(out);
  CHECK(hex(out) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256: the comparison reads every byte and says equal only when all are") {
  std::uint8_t a[kSha256Size], b[kSha256Size];
  core::image::sha256(reinterpret_cast<const std::uint8_t*>("abc"), 3, a);
  core::image::sha256(reinterpret_cast<const std::uint8_t*>("abc"), 3, b);
  CHECK(core::image::sha256Equal(a, b));
  b[31] ^= 1;
  CHECK(!core::image::sha256Equal(a, b));
  b[31] ^= 1;
  b[0] ^= 0x80;
  CHECK(!core::image::sha256Equal(a, b));
}
