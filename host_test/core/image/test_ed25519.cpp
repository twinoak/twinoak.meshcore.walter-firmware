// Ed25519 verification (task p2-01): RFC 8032 section 7.1's first three test vectors, read from
// the RFC (TEST 1 also as the spec's index.json carries it), and the one that matters most here -
// what is refused: a flipped bit anywhere, the wrong key, and a signature whose S is not below
// the group order, which TweetNaCl's crypto_sign_open accepts and this firmware's wrapper does
// not (RFC 8032 section 5.1.7 step 1). The streaming path is held against the vendored
// crypto_sign_open on every vector.
#include <doctest.h>

#include <tweetnacl/tweetnacl_verify.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/image/crypto/ed25519.h"
#include "core/image/signature.h"
#include "fixtures/manifest_files.h"

using core::image::ed25519ScalarIsCanonical;
using core::image::Ed25519Verifier;
using core::image::ed25519Verify;
using fixtures::fromHex;

namespace {

struct Vector {
  const char* name;
  const char* public_key;
  const char* message;
  const char* signature;
};

// RFC 8032 section 7.1, read from https://www.rfc-editor.org/rfc/rfc8032.txt (TEST 1, 2, 3).
const Vector kRfc[] = {
    {"RFC 8032 TEST 1 (empty message)",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"RFC 8032 TEST 2 (one byte)",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"RFC 8032 TEST 3 (two bytes)",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
};

struct Case {
  std::vector<std::uint8_t> pk;
  std::vector<std::uint8_t> msg;
  std::vector<std::uint8_t> sig;
};

Case load(const Vector& v) {
  Case c{fromHex(v.public_key), fromHex(v.message), fromHex(v.signature)};
  REQUIRE(c.pk.size() == 32);
  REQUIRE(c.sig.size() == 64);
  return c;
}

bool ours(const Case& c) { return ed25519Verify(c.pk.data(), c.sig.data(), c.msg.data(), c.msg.size()); }

// The vendored crypto_sign_open, as TweetNaCl offers it: sm = sig || message into a buffer.
bool theirs(const Case& c) {
  std::vector<std::uint8_t> sm(c.sig);
  sm.insert(sm.end(), c.msg.begin(), c.msg.end());
  std::vector<std::uint8_t> m(sm.size() + 1);
  unsigned long long mlen = 0;
  return twinoak_tweetnacl_sign_open(m.data(), &mlen, sm.data(), sm.size(), c.pk.data()) == 0;
}

// The group order L, little-endian (RFC 8032 section 5.1).
const std::uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                             0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                             0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

// s + L, little-endian; fits in 32 bytes for every s < L (the sum is below 2^253).
std::vector<std::uint8_t> plusL(const std::uint8_t* s) {
  std::vector<std::uint8_t> out(32);
  unsigned carry = 0;
  for (std::size_t i = 0; i < 32; ++i) {
    const unsigned sum = static_cast<unsigned>(s[i]) + kL[i] + carry;
    out[i] = static_cast<std::uint8_t>(sum & 0xFF);
    carry = sum >> 8;
  }
  REQUIRE(carry == 0);
  return out;
}

}  // namespace

TEST_CASE("Ed25519: RFC 8032's first three vectors verify, and so does index.json's") {
  for (const Vector& v : kRfc) {
    CAPTURE(v.name);
    const Case c = load(v);
    CHECK(ours(c));
    CHECK(theirs(c));
  }
  // The spec's own copy of TEST 1, through the generated table.
  const Case c = load(Vector{"index.json rfc8032_test_1", fixtures::kRfc8032Test1.public_key,
                             fixtures::kRfc8032Test1.message, fixtures::kRfc8032Test1.signature});
  CHECK(ours(c));
  CHECK(std::string(fixtures::kRfc8032Test1.public_key) == kRfc[0].public_key);
}

TEST_CASE("Ed25519: a flipped bit anywhere in the signature, the message or the key is refused") {
  const Case base = load(kRfc[2]);  // two bytes of message: every part has something to flip
  REQUIRE(ours(base));
  SUBCASE("every byte of the signature") {
    for (std::size_t i = 0; i < 64; ++i) {
      Case c = base;
      c.sig[i] ^= static_cast<std::uint8_t>(1u << (i % 8));
      CAPTURE(i);
      CHECK_FALSE(ours(c));
    }
  }
  SUBCASE("every bit of the message") {
    for (std::size_t bit = 0; bit < 16; ++bit) {
      Case c = base;
      c.msg[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
      CAPTURE(bit);
      CHECK_FALSE(ours(c));
    }
  }
  SUBCASE("every byte of the public key") {
    for (std::size_t i = 0; i < 32; ++i) {
      Case c = base;
      c.pk[i] ^= static_cast<std::uint8_t>(1u << (i % 8));
      CAPTURE(i);
      CHECK_FALSE(ours(c));
    }
  }
  SUBCASE("another vector's key") {
    Case c = base;
    c.pk = load(kRfc[1]).pk;
    CHECK_FALSE(ours(c));
  }
  SUBCASE("a message one byte longer or shorter") {
    Case c = base;
    c.msg.push_back(0);
    CHECK_FALSE(ours(c));
    c.msg.pop_back();
    c.msg.pop_back();
    CHECK_FALSE(ours(c));
  }
}

TEST_CASE("Ed25519: a non-canonical S (S + L) is accepted by TweetNaCl's crypto_sign_open and refused here") {
  // RFC 8032 section 5.1.7 step 1: S must be in the range 0 <= S < L, and a signature with S
  // out of range is invalid. S + L names the same scalar modulo L, so the arithmetic alone
  // cannot tell them apart; that is what the range check is for. The vendored implementation
  // does not make it (its author's choice, known); the wrapper makes it first, and this test is
  // the record of both - if a future extract ever refused S + L itself, this is where to notice.
  for (const Vector& v : kRfc) {
    CAPTURE(v.name);
    const Case base = load(v);
    REQUIRE(ed25519ScalarIsCanonical(base.sig.data() + 32));
    Case c = base;
    const std::vector<std::uint8_t> s = plusL(base.sig.data() + 32);
    for (std::size_t i = 0; i < 32; ++i) c.sig[32 + i] = s[i];
    CHECK_FALSE(ed25519ScalarIsCanonical(c.sig.data() + 32));
    CHECK(theirs(c));       // the vendored code: accepted
    CHECK_FALSE(ours(c));   // this firmware: refused
  }
  // The boundary itself: L is not below L, L - 1 is, zero is.
  CHECK_FALSE(ed25519ScalarIsCanonical(kL));
  std::uint8_t below[32];
  for (std::size_t i = 0; i < 32; ++i) below[i] = kL[i];
  below[0] -= 1;
  CHECK(ed25519ScalarIsCanonical(below));
  const std::uint8_t zero[32] = {};
  CHECK(ed25519ScalarIsCanonical(zero));
  std::uint8_t top[32] = {};
  top[31] = 0x80;  // 2^255: above L however the low bytes read
  CHECK_FALSE(ed25519ScalarIsCanonical(top));
}

TEST_CASE("Ed25519: the streaming verifier gives the one-shot answer however the message is cut") {
  // The fixture manifest: a real message of some length, signed with the test key.
  const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater.json");
  const std::string line = fixtures::asText(fixtures::readSpec("manifest-repeater.json.sig"));
  // "ed25519 <id> <base64>\n": the base64 is the last 88 characters before the newline.
  const std::string b64 = line.substr(line.size() - 89, 88);
  // Decode through the signature module's own decoder (tested in test_signature.cpp).
  std::uint8_t sig[64];
  REQUIRE(core::image::base64Decode(b64.data(), b64.size(), sig, sizeof sig) == 64);
  const std::vector<std::uint8_t> pk = fromHex(fixtures::kTestPublicKeyHex);
  REQUIRE(ed25519Verify(pk.data(), sig, manifest.data(), manifest.size()));
  Case c{pk, manifest, std::vector<std::uint8_t>(sig, sig + 64)};
  REQUIRE(theirs(c));

  for (std::size_t piece : {1u, 2u, 7u, 63u, 64u, 65u, 127u, 128u, 129u, 500u, 4096u}) {
    Ed25519Verifier v;
    v.begin(pk.data(), sig);
    for (std::size_t at = 0; at < manifest.size(); at += piece) {
      const std::size_t take = piece < manifest.size() - at ? piece : manifest.size() - at;
      v.update(manifest.data() + at, take);
    }
    CAPTURE(piece);
    CHECK(v.finish());
    // finish() ends the stream: a second finish() without begin() is a refusal, not a repeat.
    CHECK_FALSE(v.finish());
  }
  // A verifier never begun refuses; one begun over a changed message refuses.
  Ed25519Verifier v;
  CHECK_FALSE(v.finish());
  v.begin(pk.data(), sig);
  std::vector<std::uint8_t> changed = manifest;
  changed[100] ^= 0x20;
  v.update(changed.data(), changed.size());
  CHECK_FALSE(v.finish());
}
