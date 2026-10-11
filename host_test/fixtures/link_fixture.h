// spec/fixtures/link/link-v1.json, read at run time, and the small conversions every test over it
// needs. The path comes from TWINOAK_REPO_ROOT (host_test/CMakeLists.txt), so the tests read the
// specification's own copy of the fixtures and not a transcription of them.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fixtures/json.h"

namespace fixtures {

inline std::string linkFixturePath() {
  return std::string(TWINOAK_REPO_ROOT) + "/spec/fixtures/link/link-v1.json";
}

// Parsed once: the tests ask it which of its keys nobody read, so they all have to be looking at
// the same document. A file that cannot be read throws, which doctest reports as the failure of
// whichever test asked first.
inline jsonfix::Value loadLinkFixture() {
  const std::string path = linkFixturePath();
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    throw std::runtime_error("cannot read the link fixtures at " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return jsonfix::parse(text);
}

inline const jsonfix::Value& linkFixture() {
  static jsonfix::Value fixture = loadLinkFixture();
  return fixture;
}

inline std::vector<std::uint8_t> fromHex(const std::string& hex) {
  REQUIRE_MESSAGE(hex.size() % 2 == 0, "a hex string of an odd length: " << hex);
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

inline std::string toHex(const std::uint8_t* data, std::size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0F]);
  }
  return out;
}

inline std::string toHex(const std::vector<std::uint8_t>& bytes) {
  return toHex(bytes.data(), bytes.size());
}

}  // namespace fixtures
