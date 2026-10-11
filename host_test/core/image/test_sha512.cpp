// SHA-512 (task p2-01): FIPS 180-4's examples and NIST's long-message vector, and the streaming
// class held against TweetNaCl's own one-shot crypto_hash for every length from 0 to 300, fed in
// odd pieces - the padding is the one piece of hashing written here rather than vendored.
#include <doctest.h>

#include <tweetnacl/tweetnacl_verify.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/image/crypto/sha512.h"

using core::image::kSha512Size;
using core::image::Sha512;

namespace {

std::string hex(const std::uint8_t* d, std::size_t n = kSha512Size) {
  std::string s;
  char buf[3];
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof buf, "%02x", d[i]);
    s += buf;
  }
  return s;
}

std::string digestOf(const std::string& text) {
  std::uint8_t out[kSha512Size];
  core::image::sha512(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), out);
  return hex(out);
}

}  // namespace

TEST_CASE("SHA-512: FIPS 180-4 'abc', the empty message and the two-block message") {
  CHECK(digestOf("abc") ==
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  CHECK(digestOf("") ==
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
  CHECK(digestOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                 "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
}

TEST_CASE("SHA-512: a million 'a', fed in pieces of every awkward size, is NIST's vector") {
  Sha512 s;
  std::vector<std::uint8_t> chunk(1000, 'a');
  std::size_t fed = 0;
  std::size_t piece = 1;
  while (fed < 1000000) {
    const std::size_t n = (piece % 259) + 1;  // 1..259: straddling the 128-byte block in every way
    const std::size_t take = n < 1000000 - fed ? n : 1000000 - fed;
    s.update(chunk.data(), take);
    fed += take;
    piece++;
  }
  std::uint8_t out[kSha512Size];
  s.finish(out);
  CHECK(hex(out) ==
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
        "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
  // finish() reset it: the same object hashes "abc" afresh.
  s.update(reinterpret_cast<const std::uint8_t*>("abc"), 3);
  s.finish(out);
  CHECK(hex(out).substr(0, 16) == "ddaf35a193617aba");
}

TEST_CASE("SHA-512: the streaming class is TweetNaCl's crypto_hash for every length 0..300") {
  // Every length crosses the padding's two cases (a remainder under 112 bytes, or not) and the
  // block boundary at 128 and 256; the pieces make sure the buffering is exercised too.
  std::vector<std::uint8_t> data(300);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7 + 3);
  for (std::size_t n = 0; n <= 300; ++n) {
    std::uint8_t theirs[kSha512Size];
    twinoak_tweetnacl_hash(theirs, data.data(), n);
    Sha512 s;
    std::size_t at = 0;
    std::size_t piece = n % 5 + 1;
    while (at < n) {
      const std::size_t take = piece < n - at ? piece : n - at;
      s.update(data.data() + at, take);
      at += take;
      piece = piece * 3 % 61 + 1;
    }
    std::uint8_t ours[kSha512Size];
    s.finish(ours);
    CAPTURE(n);
    CHECK(hex(ours) == hex(theirs));
  }
}
