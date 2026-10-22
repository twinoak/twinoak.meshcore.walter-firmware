// The two fixture sets, read from disk (task p1.5-17).
//
// `tests/fixtures/companion/*.hex` are the repeater's reply bytes one layer out - a push code, a
// reserved byte and a prefix or a tag, then the reply - and `tests/fixtures/contracts/*.json` are
// the envelopes MeshMaster.Contracts writes. Both are copies of twinoak/meshmaster.dk's, never
// edited here (their READMEs say so), and this header is how the fakes and the suites of
// test/test_runner and test/test_loop read them: FakeMesh answers with the bytes of a fixture
// wherever one exists, and a result is held against the shape of its contract fixture.
//
// The three strippers are test/test_reply_codec's, moved here so there is one reading of the
// wrapper and not two: the table they implement is tests/fixtures/companion/README.md.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef FIXTURES_DIR
#error "FIXTURES_DIR must be defined; platformio.ini's [env:native] does it"
#endif
#ifndef COMPANION_DIR
#error "COMPANION_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace fake {

namespace fs = std::filesystem;

inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string readWholeFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot read " << path;
  std::ostringstream all;
  all << in.rdbuf();
  return all.str();
}

// A contract fixture, without its trailing newline: the bytes ContractsJson wrote.
inline std::string contract(const std::string& name) {
  std::string text = readWholeFile(fs::path(FIXTURES_DIR) / (name + ".json"));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

// A companion fixture's frame: the comment lines are the citation and are not bytes.
inline std::vector<uint8_t> frame(const std::string& name) {
  std::ifstream in(fs::path(COMPANION_DIR) / (name + ".hex"));
  EXPECT_TRUE(in.good()) << "no such companion fixture: " << name;
  std::string line;
  std::string hex;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    for (char c : line) {
      if (hexDigit(c) >= 0) hex.push_back(c);
    }
  }
  EXPECT_EQ(hex.size() % 2, 0u) << name << " has an odd number of hex digits";
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>((hexDigit(hex[i]) << 4) | hexDigit(hex[i + 1])));
  }
  return bytes;
}

// The wrappers of the three shapes (tests/fixtures/companion/README.md).
inline std::vector<uint8_t> afterPrefix(const std::string& name) {  // code, reserved, prefix[6]
  const auto f = frame(name);
  EXPECT_GE(f.size(), 8u) << name;
  return {f.begin() + 8, f.end()};
}

inline std::vector<uint8_t> afterTag(const std::string& name) {  // code, reserved, tag[4]
  const auto f = frame(name);
  EXPECT_GE(f.size(), 6u) << name;
  return {f.begin() + 6, f.end()};
}

inline std::vector<uint8_t> body(const std::string& name) {  // code
  const auto f = frame(name);
  EXPECT_GE(f.size(), 1u) << name;
  return {f.begin() + 1, f.end()};
}

inline std::string toHex(const uint8_t* bytes, std::size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < len; i++) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// The shape of an envelope: its member names in the order ContractsJson wrote them.
//
// A result this firmware writes is held against its contract fixture **byte for byte** by
// test/test_job_results, for the one sample the fixture was written from. What a *run* of the
// runner can be held to is the shape: the same members, in the same order, with the optional
// ones this run had nothing to say about left out - which is what the records' nulls mean. So
// `keysOf` reads the top-level member names of a JSON object and `followsShapeOf` says that one
// object's members are the fixture's, in the fixture's order, and that none of them is a member
// the fixture has not got.

inline std::vector<std::string> keysOf(const std::string& json) {
  std::vector<std::string> keys;
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  std::string current;
  bool collecting = false;
  for (std::size_t i = 0; i < json.size(); i++) {
    const char c = json[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
        if (collecting) {
          // A member name at depth 1 is followed by a colon; a string value is not.
          std::size_t j = i + 1;
          while (j < json.size() && (json[j] == ' ' || json[j] == '\n')) j++;
          if (j < json.size() && json[j] == ':') keys.push_back(current);
        }
        collecting = false;
        current.clear();
      } else if (collecting) {
        current.push_back(c);
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      collecting = depth == 1;
      current.clear();
    } else if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
    }
  }
  return keys;
}

// `json` has the members of `fixtureJson`, in its order, and no others. Missing members are
// allowed (an absent optional field is what a null is on the wire); extra ones are not.
inline ::testing::AssertionResult followsShapeOf(const std::string& json, const std::string& fixtureJson) {
  const std::vector<std::string> want = keysOf(fixtureJson);
  const std::vector<std::string> got = keysOf(json);
  std::size_t w = 0;
  for (const std::string& key : got) {
    std::size_t at = w;
    while (at < want.size() && want[at] != key) at++;
    if (at == want.size()) {
      const bool known = std::find(want.begin(), want.end(), key) != want.end();
      return ::testing::AssertionFailure()
             << "member '" << key << "' is " << (known ? "out of the fixture's order" : "not a member of the fixture")
             << "\n  written: " << json << "\n  fixture: " << fixtureJson;
    }
    w = at + 1;
  }
  return ::testing::AssertionSuccess();
}

}  // namespace fake
