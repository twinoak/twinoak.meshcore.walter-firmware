// The spec's manifest fixtures and the generated variants, read from disk for the image tests
// (task p2-01), and the test key as a trust table of the tests' own - never the build's, which
// is keys/release-keys.txt's and empty today.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "core/image/trust.h"
#include "manifest_fixtures.h"

namespace fixtures {

inline std::vector<std::uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read the fixture " << path);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A file of spec/fixtures/manifest/.
inline std::vector<std::uint8_t> readSpec(const char* name) {
  return readFile(std::string(TWINOAK_SPEC_FIXTURES) + "/" + name);
}

// A file gen_manifest_fixtures.py wrote.
inline std::vector<std::uint8_t> readGenerated(const char* name) {
  return readFile(std::string(kGeneratedDir) + "/" + name);
}

inline std::vector<std::uint8_t> readVariantFile(const Variant& v) {
  return v.file_in_spec ? readSpec(v.file) : readGenerated(v.file);
}

inline std::string asText(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

inline std::vector<std::uint8_t> fromHex(const std::string& hex) {
  REQUIRE(hex.size() % 2 == 0);
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

inline std::string toHex(const std::uint8_t* bytes, std::size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

// The test key, with its id computed as section 3 says rather than copied.
inline core::image::TrustedKey testKey() {
  core::image::TrustedKey key{};
  const std::vector<std::uint8_t> raw = fromHex(kTestPublicKeyHex);
  REQUIRE(raw.size() == core::image::kEd25519PublicKeySize);
  for (std::size_t i = 0; i < raw.size(); ++i) key.key[i] = raw[i];
  core::image::keyIdOf(key.key, key.id);
  return key;
}

// The key ids keys/release-keys.txt names, in the file's order - the same lines
// tools/gen_trust_table.py compiles into the build's table (`ed25519 <key id> <public hex>`, # a
// comment). Read here rather than written down, so that a test says "the keys this repository
// names" and not "this key".
inline std::vector<std::string> releaseKeyIds() {
  std::vector<std::string> ids;
  std::ifstream in(std::string(TWINOAK_REPO_ROOT) + "/keys/release-keys.txt");
  REQUIRE_MESSAGE(in.good(), "cannot read keys/release-keys.txt");
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    std::size_t at = line.find_first_not_of(" \t\r\n");
    if (at == std::string::npos) continue;
    line = line.substr(at);
    if (line.compare(0, 8, "ed25519 ") != 0) continue;
    at = line.find_first_not_of(" \t", 8);
    if (at == std::string::npos) continue;
    const std::size_t end = line.find_first_of(" \t\r\n", at);
    ids.push_back(line.substr(at, end == std::string::npos ? std::string::npos : end - at));
  }
  return ids;
}

// A trust table holding the test key and nothing else: what a +TESTKEY build would have.
struct TestTrust {
  core::image::TrustedKey keys[1];
  core::image::TrustTable table;
  TestTrust() : keys{testKey()}, table(keys, 1) {}
};

}  // namespace fixtures
