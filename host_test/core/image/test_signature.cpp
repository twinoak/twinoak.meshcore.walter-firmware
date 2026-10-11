// The signature line and check 1 (task p2-01): spec/FIRMWARE-MANIFEST.md section 3's one line
// parsed strictly, the base64 decoder held to the alphabet and the padding, and every entry of
// index.json's `signatures` through verifyManifestSignature with the test key - the valid ones
// accepted, each invalid one refused with its own reason.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/image/signature.h"
#include "fixtures/manifest_files.h"

using core::image::base64Decode;
using core::image::parseSignatureLine;
using core::image::SignatureLine;
using core::image::SignatureStatus;
using core::image::verifyManifestSignature;

namespace {

bool parses(const std::string& text, SignatureLine* out = nullptr) {
  SignatureLine line;
  return parseSignatureLine(text.data(), text.size(), out != nullptr ? out : &line);
}

std::string fixtureLine() { return fixtures::asText(fixtures::readSpec("manifest-repeater.json.sig")); }

}  // namespace

TEST_CASE("the fixture signature lines parse to the test key's id and 64 bytes") {
  for (std::size_t i = 0; i < fixtures::kSignatureCount; ++i) {
    const fixtures::SignatureCase& c = fixtures::kSignatures[i];
    CAPTURE(c.sig);
    const std::string text = fixtures::asText(fixtures::readSpec(c.sig));
    SignatureLine line;
    REQUIRE(parses(text, &line));
    const std::string id = fixtures::toHex(line.key_id, core::image::kKeyIdSize);
    if (std::strcmp(c.sig, "manifest-repeater-unknown-key.json.sig") == 0) {
      CHECK(id != std::string(fixtures::kTestKeyId));
    } else {
      CHECK(id == std::string(fixtures::kTestKeyId));
    }
  }
}

TEST_CASE("the signature line is parsed strictly: anything but section 3's one line is no signature") {
  const std::string good = fixtureLine();  // ends in one LF, as manifestref.py writes it
  REQUIRE(good.back() == '\n');
  const std::string bare = good.substr(0, good.size() - 1);
  CHECK(parses(good));
  CHECK(parses(bare));
  CHECK(parses(bare + "\r\n"));
  CHECK_FALSE(parses(bare + "\n\n"));
  CHECK_FALSE(parses(bare + " "));
  CHECK_FALSE(parses(bare + "\r"));
  CHECK_FALSE(parses(" " + bare));
  CHECK_FALSE(parses(bare + "\nsecond line"));
  CHECK_FALSE(parses(""));
  CHECK_FALSE(parses("ed25519"));
  CHECK_FALSE(parses(bare.substr(0, bare.size() - 1)));  // 87 characters of base64
  CHECK_FALSE(parses(bare + "A"));

  // The algorithm word, exactly.
  std::string s = bare;
  s[0] = 'E';
  CHECK_FALSE(parses(s));
  s = "ed448" + bare.substr(7);
  CHECK_FALSE(parses(s));

  // The separators: one space each, no tab, no double space.
  s = bare;
  s[7] = '\t';
  CHECK_FALSE(parses(s));
  s = bare;
  s.insert(8, " ");
  CHECK_FALSE(parses(s));
  s = bare;
  s[24] = '\t';
  CHECK_FALSE(parses(s));

  // The key id: 16 lowercase hex characters.
  s = bare;
  s[8] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[8])));
  if (s[8] >= 'A' && s[8] <= 'F') {
    CHECK_FALSE(parses(s));
  }
  s = bare;
  s[9] = 'g';
  CHECK_FALSE(parses(s));
  s = bare;
  s.erase(8, 1);
  CHECK_FALSE(parses(s));

  // The base64: the alphabet only, canonical padding.
  s = bare;
  s[30] = '-';
  CHECK_FALSE(parses(s));
  s = bare;
  s[30] = '=';
  CHECK_FALSE(parses(s));
  s = bare;
  s[s.size() - 1] = 'A';  // 64 bytes end in "==" - a third data character is one byte too many
  CHECK_FALSE(parses(s));
}

TEST_CASE("base64Decode: strict standard base64") {
  std::uint8_t out[8];
  auto decode = [&out](const std::string& text) { return base64Decode(text.data(), text.size(), out, sizeof out); };
  CHECK(decode("") == 0);
  CHECK(decode("TWFu") == 3);
  CHECK(std::string(reinterpret_cast<char*>(out), 3) == "Man");
  CHECK(decode("TWE=") == 2);
  CHECK(std::string(reinterpret_cast<char*>(out), 2) == "Ma");
  CHECK(decode("TQ==") == 1);
  CHECK(out[0] == 'M');
  CHECK(decode("TWFuTQ==") == 4);
  CHECK(decode("TWFuTQ") == 0);      // not a multiple of four
  CHECK(decode("TWF") == 0);
  CHECK(decode("TW=u") == 0);        // '=' before the end
  CHECK(decode("T===") == 0);        // three '='
  CHECK(decode("====") == 0);
  CHECK(decode("TWFu====") == 0);    // a group of nothing but padding
  CHECK(decode("TQ==TWFu") == 0);    // padding not in the last group
  CHECK(decode("TR==") == 0);        // non-canonical: the unused bits of 'R' are not zero
  CHECK(decode("TWB=") == 0);        // non-canonical: 'B' leaves a non-zero bit
  CHECK(decode("TWF-") == 0);        // url-safe alphabet is not standard
  CHECK(decode("TW\nFu") == 0);
  CHECK(decode("TWFuTWFuTWFu") == 0);  // 9 bytes into 8: too small an output is a refusal
  // The one the signature line needs: 88 characters into 64 bytes.
  const std::string line = fixtureLine();
  std::uint8_t sig[64];
  CHECK(base64Decode(line.data() + line.size() - 89, 88, sig, sizeof sig) == 64);
}

TEST_CASE("every entry of index.json's signatures: the valid ones accepted, each invalid one refused at check 1") {
  const fixtures::TestTrust trust;
  REQUIRE(trust.table.count() == 1);
  std::size_t valid = 0;
  std::size_t invalid = 0;
  for (std::size_t i = 0; i < fixtures::kSignatureCount; ++i) {
    const fixtures::SignatureCase& c = fixtures::kSignatures[i];
    CAPTURE(c.manifest);
    CAPTURE(c.sig);
    const std::vector<std::uint8_t> manifest = fixtures::readSpec(c.manifest);
    const std::string line = fixtures::asText(fixtures::readSpec(c.sig));
    const core::image::TrustedKey* used = nullptr;
    const SignatureStatus status =
        verifyManifestSignature(trust.table, manifest.data(), manifest.size(), line.data(), line.size(), &used);
    if (c.valid) {
      CHECK(status == SignatureStatus::kOk);
      CHECK(used == &trust.keys[0]);
      ++valid;
    } else {
      CHECK(status != SignatureStatus::kOk);
      const std::string why = c.why;
      if (why.find("does not hold") != std::string::npos) {
        CHECK(status == SignatureStatus::kUnknownKey);  // an unknown id: no key was even tried
        CHECK(used == nullptr);
      } else {
        CHECK(status == SignatureStatus::kBadSignature);  // tampered, or another manifest's
        CHECK(used == &trust.keys[0]);
      }
      // std::string: doctest prints a const char* variable as a pointer (or "1"), not as text.
      MESSAGE("check 1 refuses " << std::string(c.manifest) << " with " << std::string(c.sig) << ": "
                                 << std::string(core::image::describe(status)) << " (index.json: " << why << ")");
      ++invalid;
    }
  }
  CHECK(valid >= 2);
  CHECK(invalid >= 3);
}

TEST_CASE("an unknown key id is refused without trying the keys the table does hold") {
  // The same manifest and signature, with the id in the line changed: the signature would verify
  // with the test key, but the line does not name it, and section 3 says the id picks the key.
  const fixtures::TestTrust trust;
  const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater.json");
  std::string line = fixtureLine();
  REQUIRE(verifyManifestSignature(trust.table, manifest.data(), manifest.size(), line.data(), line.size()) ==
          SignatureStatus::kOk);
  line[8] = line[8] == '0' ? '1' : '0';
  CHECK(verifyManifestSignature(trust.table, manifest.data(), manifest.size(), line.data(), line.size()) ==
        SignatureStatus::kUnknownKey);
  // And a line that is not a signature line at all.
  const std::string junk = "signed by somebody";
  CHECK(verifyManifestSignature(trust.table, manifest.data(), manifest.size(), junk.data(), junk.size()) ==
        SignatureStatus::kNoSignature);
}

TEST_CASE("with an empty trust table every manifest is refused at check 1, the valid fixtures included") {
  const core::image::TrustTable empty;
  REQUIRE(empty.empty());
  for (std::size_t i = 0; i < fixtures::kSignatureCount; ++i) {
    const fixtures::SignatureCase& c = fixtures::kSignatures[i];
    const std::vector<std::uint8_t> manifest = fixtures::readSpec(c.manifest);
    const std::string line = fixtures::asText(fixtures::readSpec(c.sig));
    CAPTURE(c.manifest);
    CHECK(verifyManifestSignature(empty, manifest.data(), manifest.size(), line.data(), line.size()) ==
          SignatureStatus::kUnknownKey);
  }
}

TEST_CASE("property: a valid manifest and signature with any single byte of either changed is refused") {
  // The walter fixture (the shorter of the two): every byte of the manifest changed in turn,
  // then every byte of the signature line. A change that leaves the text a signature line is a
  // bad signature or an unknown key; one that does not is no signature. Never kOk.
  const fixtures::TestTrust trust;
  const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-walter.json");
  const std::string line = fixtures::asText(fixtures::readSpec("manifest-walter.json.sig"));
  REQUIRE(verifyManifestSignature(trust.table, manifest.data(), manifest.size(), line.data(), line.size()) ==
          SignatureStatus::kOk);
  std::size_t refused = 0;
  for (std::size_t i = 0; i < manifest.size(); ++i) {
    std::vector<std::uint8_t> changed = manifest;
    changed[i] ^= static_cast<std::uint8_t>(1u << (i % 8));  // a different bit each byte
    const SignatureStatus status =
        verifyManifestSignature(trust.table, changed.data(), changed.size(), line.data(), line.size());
    if (status != SignatureStatus::kOk) ++refused;
    CHECK_MESSAGE(status == SignatureStatus::kBadSignature, "manifest byte " << i);
  }
  for (std::size_t i = 0; i < line.size(); ++i) {
    std::string changed = line;
    changed[i] = static_cast<char>(changed[i] ^ static_cast<char>(1u << (i % 7)));  // stays in 7 bits
    const SignatureStatus status =
        verifyManifestSignature(trust.table, manifest.data(), manifest.size(), changed.data(), changed.size());
    if (status != SignatureStatus::kOk) ++refused;
    CHECK_MESSAGE(status != SignatureStatus::kOk, "signature line byte " << i);
  }
  CHECK(refused == manifest.size() + line.size());
  MESSAGE("property: " << manifest.size() << " manifest bytes and " << line.size()
                       << " signature-line bytes changed one at a time, " << refused << " refusals");
}
