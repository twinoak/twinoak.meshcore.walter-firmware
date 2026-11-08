// The link fixtures, read at run time. The path comes from TWINOAK_REPO_ROOT
// (host_test/CMakeLists.txt), so the tests read the specification's own copy of the fixtures and
// not a transcription of them; the hex both ways is fixtures/hex.h.
//
// **Three files, because the link has three minors** (spec/LINK-PROTOCOL.md, *Versions*).
// `link-v1.json` is 1.0's and keeps its bytes for ever; `link-v1.1.json` is every case of it plus
// the four of 1.1; `link-v1.2.json` is every case of 1.1 plus the fourteen of 1.2 (§5.9's
// LOCAL_REQUEST and the HELLOs that carry its bit). `linkFixture()` is 1.0's and is what the
// scripted radio answers from - the radio on the bench runs repeater v0.1.0, which speaks 1.0 -
// and `linkFixture11()`/`linkFixture12()` are the newer files, which the codec's own test walks
// as well (tasks p8-05 and p9-06).
#pragma once

#include <doctest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fixtures/hex.h"
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

// 1.2's file: every case of 1.1 and the fourteen of 1.2 (the HELLO that asks for minor 2, the
// answers with and without the `LOCAL_REQUEST` bit, the four requests §5.9 answers with their
// answers, and its two refusals).
inline const jsonfix::Value& linkFixture12() {
  static jsonfix::Value fixture = loadLinkFixtureFrom("link-v1.2.json");
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

}  // namespace fixtures
