// The link fixtures, read at run time, and the small conversions every test over them needs. The
// path comes from TWINOAK_REPO_ROOT (host_test/CMakeLists.txt), so the tests read the
// specification's own copy of the fixtures and not a transcription of them.
//
// **Two files, because the link has two minors** (spec/LINK-PROTOCOL.md, *Versions*).
// `link-v1.json` is 1.0's and keeps its bytes for ever; `link-v1.1.json` is every case of it plus
// the four of 1.1. `linkFixture()` is 1.0's and is what the scripted radio answers from - the
// radio on the bench runs repeater v0.1.0, which speaks 1.0 - and `linkFixture11()` is the newer
// file, which the codec's own test walks as well (task p8-05).
#pragma once

#include <doctest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fixtures/json.h"

namespace fixtures {

inline std::string linkFixturePathFor(const std::string& file) {
  return std::string(TWINOAK_REPO_ROOT) + "/spec/fixtures/link/" + file;
}

inline std::string linkFixturePath() { return linkFixturePathFor("link-v1.json"); }

// Parsed once per file: the tests ask it which of its keys nobody read, so everybody over one file
// has to be looking at the same document. A file that cannot be read throws, which doctest reports
// as the failure of whichever test asked first.
inline jsonfix::Value loadLinkFixtureFrom(const std::string& file) {
  const std::string path = linkFixturePathFor(file);
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    throw std::runtime_error("cannot read the link fixtures at " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return jsonfix::parse(text);
}

inline jsonfix::Value loadLinkFixture() { return loadLinkFixtureFrom("link-v1.json"); }

// 1.0's file: what every test that is not about the band reads, and what the scripted radio
// answers from.
inline const jsonfix::Value& linkFixture() {
  static jsonfix::Value fixture = loadLinkFixture();
  return fixture;
}

// 1.1's file: every case of 1.0 and the four of 1.1 (the request that asks for minor 1, the answer
// and the EV_BOOT that end with a band, and a 1.0 radio's answer to a 1.1 request).
inline const jsonfix::Value& linkFixture11() {
  static jsonfix::Value fixture = loadLinkFixtureFrom("link-v1.1.json");
  return fixture;
}

// The fixtures' case of this name, in whichever file, or nullptr. The name is the specification's
// own words for it, which is how a test says which bytes it means.
inline const jsonfix::Value* linkCaseIn(const jsonfix::Value& fixture, const std::string& name) {
  const jsonfix::Value& all = fixture["cases"];
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all.at(i)["name"].string() == name) {
      return &all.at(i);
    }
  }
  return nullptr;
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
