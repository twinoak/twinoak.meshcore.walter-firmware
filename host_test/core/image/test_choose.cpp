// Choosing the image (task p2-01): check 3 of section 5 - the right image among several, no
// image for this board, a `requires.walter` newer than this build, a `link_major` this build does
// not speak - and the version comparison underneath it.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/image/choose.h"
#include "core/image/this_image.h"
#include "core/version.h"
#include "fixtures/manifest_files.h"

using core::image::Choice;
using core::image::chooseImage;
using core::image::ChooseReason;
using core::image::compareVersions;
using core::image::Manifest;
using core::image::ManifestParser;
using core::image::SlotNeeds;
using core::image::ThisUnit;
using core::image::Use;

namespace {

Manifest parsed(const std::string& text) {
  static ManifestParser parser;
  Manifest m;
  const core::image::ParseResult r = parser.parse(text.data(), text.size(), &m);
  REQUIRE_MESSAGE(r.ok, core::image::describe(r.error));
  REQUIRE(core::image::validate(m).ok);
  return m;
}

ThisUnit unit(const char* version = "0.3.0+abc1234", std::uint8_t major = 1) {
  ThisUnit u;
  u.version = version;
  u.link_majors[0] = major;
  u.link_major_count = 1;
  return u;
}

SlotNeeds needs(const char* role = "repeater", const char* board = "rak4630-single", const char* band = "868") {
  SlotNeeds n;
  n.slot = core::board::Slot{0};
  n.role = role;
  n.board = board;
  n.band = band;
  return n;
}

std::string edited(const std::string& text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  REQUIRE(at != std::string::npos);
  std::string out = text;
  out.replace(at, from.size(), to);
  return out;
}

}  // namespace

TEST_CASE("the right image among several, with its swd file") {
  const Manifest m = parsed(fixtures::asText(fixtures::readGenerated("repeater-three-images.json")));
  REQUIRE(m.image_count == 3);
  Choice c = chooseImage(m, needs(), Use::kSwd, unit());
  REQUIRE(c.ok());
  CHECK(c.image_index == 2);
  CHECK(std::string(c.image->id) == "repeater.rak4630-single.868");
  CHECK(c.file->use == Use::kSwd);
  CHECK(c.file_index == 0);
  CHECK(c.image == &m.images[2]);
  c = chooseImage(m, needs("repeater", "rak4630-single", "433"), Use::kSwd, unit());
  REQUIRE(c.ok());
  CHECK(c.image_index == 0);
  CHECK(std::string(c.image->band) == "433");
  c = chooseImage(m, needs("scanner"), Use::kSwd, unit());
  REQUIRE(c.ok());
  CHECK(c.image_index == 1);
  CHECK(std::string(c.image->role) == "scanner");
  // The Walter's own: the ota file of the manager image.
  const Manifest w = parsed(fixtures::asText(fixtures::readSpec("manifest-walter.json")));
  c = chooseImage(w, needs("manager", "walter", "lte-m"), Use::kOta, unit());
  REQUIRE(c.ok());
  CHECK(c.file->use == Use::kOta);
  CHECK(c.file->load_address == 0x20000);
}

TEST_CASE("no image for this board, band or role") {
  const Manifest m = parsed(fixtures::asText(fixtures::readSpec("manifest-repeater.json")));
  CHECK(chooseImage(m, needs("repeater", "rak4630-dual-a", "868"), Use::kSwd, unit()).reason == ChooseReason::kNoImage);
  CHECK(chooseImage(m, needs("repeater", "rak4630-single", "433"), Use::kSwd, unit()).reason == ChooseReason::kNoImage);
  CHECK(chooseImage(m, needs("bridge"), Use::kSwd, unit()).reason == ChooseReason::kNoImage);
  CHECK(chooseImage(m, needs("manager", "walter", "lte-m"), Use::kOta, unit()).reason == ChooseReason::kNoImage);
  // The image is there but has no file of the use wanted.
  CHECK(chooseImage(m, needs(), Use::kOta, unit()).reason == ChooseReason::kNoFileOfUse);
  CHECK(chooseImage(m, needs(), Use::kDebug, unit()).reason == ChooseReason::kNoFileOfUse);
  const Choice none = chooseImage(m, needs("bridge"), Use::kSwd, unit());
  CHECK(none.image == nullptr);
  CHECK(none.file == nullptr);
}

TEST_CASE("two images with the same role, board and band are ambiguous, not a choice") {
  const std::string r = fixtures::asText(fixtures::readSpec("manifest-repeater.json"));
  const std::size_t open = r.find("    {\n      \"id\"");
  const std::size_t close = r.find("\n    }\n  ]");
  const std::string image = r.substr(open, close + 6 - open);
  const std::string other = edited(image, "repeater.rak4630-single.868", "repeater.rak4630-single.868-again");
  const Manifest m = parsed(r.substr(0, open) + image + ",\n" + other + r.substr(close + 6));
  REQUIRE(m.image_count == 2);
  CHECK(chooseImage(m, needs(), Use::kSwd, unit()).reason == ChooseReason::kAmbiguous);
}

TEST_CASE("requires.walter: a newer Walter than this build is not met; the same or older is") {
  const Manifest m = parsed(fixtures::asText(fixtures::readSpec("manifest-repeater.json")));  // requires 0.3.0
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("0.3.0+abc1234")).ok());
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("0.3.0")).ok());
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("0.3.1+0000000")).ok());
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("1.0.0")).ok());
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("0.2.9")).reason == ChooseReason::kRequiresWalterNewer);
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("0.3.0-dev")).reason == ChooseReason::kRequiresWalterNewer);
  // A development build is 0.0.0-dev and meets nothing: the repeater fixture is refused on it.
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("0.0.0-dev")).reason == ChooseReason::kRequiresWalterNewer);
  // A version this firmware cannot read meets nothing either.
  CHECK(chooseImage(m, needs(), Use::kSwd, unit("latest")).reason == ChooseReason::kRequiresWalterNewer);
  const Manifest nine = parsed(fixtures::asText(fixtures::readGenerated("repeater-requires-walter-9.json")));
  CHECK(chooseImage(nine, needs(), Use::kSwd, unit("1.0.0")).reason == ChooseReason::kRequiresWalterNewer);
  // No requires at all: any build.
  const Manifest none = parsed(fixtures::asText(fixtures::readGenerated("repeater-no-requires.json")));
  CHECK(chooseImage(none, needs(), Use::kSwd, unit("0.0.0-dev")).ok());
  // A requires that cannot be read is not met.
  const std::string r = fixtures::asText(fixtures::readSpec("manifest-repeater.json"));
  const Manifest bad = parsed(edited(r, "\"walter\": \"0.3.0\"", "\"walter\": 3"));
  CHECK(chooseImage(bad, needs(), Use::kSwd, unit("9.9.9")).reason == ChooseReason::kRequiresMalformed);
  const Manifest list = parsed(edited(r, "\"requires\": {\n        \"walter\": \"0.3.0\",\n        \"link_major\": 1\n      }", "\"requires\": [1]"));
  CHECK(chooseImage(list, needs(), Use::kSwd, unit("9.9.9")).reason == ChooseReason::kRequiresMalformed);
}

TEST_CASE("a link major this build does not speak: the image's requires, and the manifest's own link") {
  const Manifest req2 = parsed(fixtures::asText(fixtures::readGenerated("repeater-requires-link-2.json")));
  CHECK(chooseImage(req2, needs(), Use::kSwd, unit("0.3.0", 1)).reason == ChooseReason::kRequiresLinkMajor);
  // A Walter that speaks both majors (S-7) meets it - unless the manifest's link itself is one
  // it does not speak.
  ThisUnit both = unit("0.3.0", 1);
  both.link_majors[1] = 2;
  both.link_major_count = 2;
  CHECK(chooseImage(req2, needs(), Use::kSwd, both).ok());
  const Manifest link2 = parsed(fixtures::asText(fixtures::readGenerated("repeater-link-2.json")));
  CHECK(chooseImage(link2, needs(), Use::kSwd, unit("0.3.0", 1)).reason == ChooseReason::kRequiresLinkMajor);
  CHECK(chooseImage(link2, needs(), Use::kSwd, both).ok());
  const std::string r = fixtures::asText(fixtures::readSpec("manifest-repeater.json"));
  const Manifest speaks2 = parsed(edited(r, "\"major\": 1,\n    \"minor\": 0", "\"major\": 2,\n    \"minor\": 0"));
  CHECK(chooseImage(speaks2, needs(), Use::kSwd, unit("0.3.0", 1)).reason == ChooseReason::kManifestLinkMajor);
  CHECK(chooseImage(speaks2, needs(), Use::kSwd, both).ok());
}

TEST_CASE("compareVersions: X.Y.Z numerically, pre-release below, build metadata ignored") {
  int order = 99;
  CHECK(compareVersions("1.2.3", "1.2.3", &order));
  CHECK(order == 0);
  CHECK(compareVersions("1.2.3", "1.2.10", &order));
  CHECK(order == -1);
  CHECK(compareVersions("1.10.0", "1.9.9", &order));
  CHECK(order == 1);
  CHECK(compareVersions("2.0.0", "1.99.99", &order));
  CHECK(order == 1);
  CHECK(compareVersions("0.3.0+abc1234", "0.3.0", &order));
  CHECK(order == 0);
  CHECK(compareVersions("0.3.0+abc1234+TESTKEY", "0.3.0+def", &order));
  CHECK(order == 0);
  CHECK(compareVersions("0.3.0-dev", "0.3.0", &order));
  CHECK(order == -1);
  CHECK(compareVersions("0.3.0", "0.3.0-rc1", &order));
  CHECK(order == 1);
  CHECK(compareVersions("0.0.0-dev", "0.0.0-dryrun+abc", &order));
  CHECK(order == 0);
  CHECK_FALSE(compareVersions("abc", "1.0.0", &order));
  CHECK_FALSE(compareVersions("1.2", "1.2.0", &order));
  CHECK_FALSE(compareVersions("1.2.3.4", "1.2.3", &order));
  CHECK_FALSE(compareVersions("v1.2.3", "1.2.3", &order));
  CHECK_FALSE(compareVersions("", "1.2.3", &order));
  CHECK_FALSE(compareVersions(nullptr, "1.2.3", &order));
  CHECK_FALSE(compareVersions("1.2.3 ", "1.2.3", &order));
  CHECK_FALSE(compareVersions("1234567890.0.0", "1.2.3", &order));  // ten digits: not read
}

TEST_CASE("thisUnit() is this build: its version and the link major it speaks") {
  const ThisUnit u = core::image::thisUnit();
  CHECK(std::string(u.version) == core::version());
  CHECK(u.link_major_count == 1);
  CHECK(u.speaks(core::image::kLinkMajor));
  CHECK_FALSE(u.speaks(core::image::kLinkMajor + 1));
  CHECK_FALSE(u.speaks(0));
}
