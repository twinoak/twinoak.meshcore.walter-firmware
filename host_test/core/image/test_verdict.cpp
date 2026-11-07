// The verdict (task p2-01): the six checks of spec/FIRMWARE-MANIFEST.md section 5 in order, and
// a refusal shown at each - the signed variants gen_manifest_fixtures.py made for the purpose,
// the fixture images accepted against their manifests, the build's own (empty) trust table
// refusing everything at check 1, and the token that only a verdict can make.
#include <doctest.h>

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "core/flasher/board_profile.h"
#include "core/flasher/image_source.h"
#include "core/image/this_image.h"
#include "core/image/verdict.h"
#include "fixtures/manifest_files.h"

using core::flasher::MemoryImage;
using core::image::AcceptedImage;
using core::image::Check;
using core::image::ImageVerdict;
using core::image::Outcome;
using core::image::SlotNeeds;
using core::image::ThisUnit;
using core::image::Use;

namespace {

ThisUnit unit(const char* version = "0.3.0+abc1234", std::uint8_t major = 1) {
  ThisUnit u;
  u.version = version;
  u.link_majors[0] = major;
  u.link_major_count = 1;
  return u;
}

SlotNeeds repeaterSlot(const char* board = "rak4630-single", const char* band = "868", const char* role = "repeater") {
  SlotNeeds n;
  n.slot = core::board::Slot{0};
  n.role = role;
  n.board = board;
  n.band = band;
  return n;
}

SlotNeeds walterSelf() {
  SlotNeeds n;
  n.slot = core::board::Slot{0};
  n.role = "manager";
  n.board = "walter";
  n.band = "lte-m";
  return n;
}

// One verdict over files on disk. The verdict object is ~30 KB: static, as the firmware keeps it.
struct Bench {
  fixtures::TestTrust trust;
  ImageVerdict& verdict;
  std::vector<std::uint8_t> manifest;
  std::string line;
  std::vector<std::uint8_t> file;

  explicit Bench(const ThisUnit& u = unit()) : verdict(make(trust.table, u)) {}

  static ImageVerdict& make(const core::image::TrustTable& table, const ThisUnit& u) {
    static ImageVerdict* instance = nullptr;
    delete instance;
    instance = new ImageVerdict(table, u);
    return *instance;
  }

  void spec(const char* manifest_name, const char* sig_name, const char* file_name) {
    manifest = fixtures::readSpec(manifest_name);
    line = fixtures::asText(fixtures::readSpec(sig_name));
    file = file_name != nullptr ? fixtures::readSpec(file_name) : std::vector<std::uint8_t>();
  }

  void variant(const fixtures::Variant& v) {
    manifest = fixtures::readGenerated(v.manifest);
    line = fixtures::asText(fixtures::readGenerated(v.sig));
    file = v.file != nullptr ? fixtures::readVariantFile(v) : std::vector<std::uint8_t>();
  }

  Outcome judge(const SlotNeeds& needs, Use use, const core::flasher::BoardProfile* profile) {
    const MemoryImage source(file.data(), static_cast<std::uint32_t>(file.size()));
    return verdict.judge(manifest.data(), manifest.size(), line.data(), line.size(), needs, use, profile, source);
  }
  // The token of the last judge(): only a verdict can make one.
  const AcceptedImage& token() {
    REQUIRE(verdict.accepted() != nullptr);
    return *verdict.accepted();
  }
};

const fixtures::Variant& variantNamed(const char* name) {
  for (std::size_t i = 0; i < fixtures::kVariantCount; ++i) {
    if (std::strcmp(fixtures::kVariants[i].name, name) == 0) return fixtures::kVariants[i];
  }
  FAIL("no generated variant named " << name);
  return fixtures::kVariants[0];
}

}  // namespace

TEST_CASE("the fixture images are accepted against their manifests: the repeater's swd file, the walter's ota file") {
  Bench b;
  for (std::size_t i = 0; i < fixtures::kFileCount; ++i) {
    const fixtures::FileCase& f = fixtures::kFiles[i];
    CAPTURE(f.manifest);
    // The base of a release is a `provision` file and not an image: it has no descriptor, and
    // check 5p is what judges it (host_test/core/image/test_verdict_base.cpp).
    if (std::strcmp(f.use, "provision") == 0) continue;
    const bool walter = std::strcmp(f.manifest, "manifest-walter.json") == 0;
    b.spec(f.manifest, f.sig, f.bytes_of);
    const Outcome o = walter ? b.judge(walterSelf(), Use::kOta, nullptr)
                             : b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
    REQUIRE_MESSAGE(o.ok(), core::image::describe(o.failed) << ": " << o.why);
    const AcceptedImage accepted = b.token();  // a copy: the store keeps one like it
    CHECK(std::string(accepted.file().name) == f.name);
    CHECK(accepted.use() == (walter ? Use::kOta : Use::kSwd));
    CHECK(accepted.slot().index == 0);
    CHECK(accepted.file().size == b.file.size());
    std::uint8_t digest[32];
    core::image::sha256(b.file.data(), b.file.size(), digest);
    CHECK(std::memcmp(accepted.sha256(), digest, 32) == 0);
    CHECK(std::string(accepted.descriptor().board) == (walter ? "walter" : "rak4630-single"));
    CHECK(std::string(accepted.descriptor().version) == accepted.version());
    CHECK(accepted.descriptorOffset() == (walter ? 32u : 1024u));
    CHECK(accepted.linkMajor() == 1);
    CHECK(accepted.linkMinor() == 0);
    CHECK(fixtures::toHex(accepted.keyId(), 8) == std::string(fixtures::kTestKeyId));
    CHECK(std::string(accepted.image().id) == (walter ? "manager.walter.lte-m" : "repeater.rak4630-single.868"));
    MESSAGE("accepted: " << std::string(f.name) << " for " << std::string(accepted.image().id) << ", descriptor at " << accepted.descriptorOffset()
                         << ", signed by " << fixtures::toHex(accepted.keyId(), 8));
  }
}

TEST_CASE("a repeater image built against link 1.0 is accepted by this 1.1 Walter") {
  // §5's check 3 (`requires.link_major`, the manifest's `link.major`) and check 5 (the descriptor
  // against the entry) compare the **major** and never the minor: within a major a minor only
  // adds things (spec/LINK-PROTOCOL.md, *Versions*). So the day this firmware's own minor moved
  // to 1 (task p8-05) a 1.0 radio image had to stay acceptable - the unit on the bench runs
  // repeater v0.1.0 - and this is that, over all six checks and with the token read back.
  REQUIRE(core::image::kLinkMinor >= 1);
  Bench b;
  b.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  const Outcome o = b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  REQUIRE_MESSAGE(o.ok(), core::image::describe(o.failed) << ": " << o.why);
  const AcceptedImage accepted = b.token();
  CHECK(accepted.linkMajor() == core::image::kLinkMajor);
  CHECK(accepted.linkMinor() == 0);
  CHECK(accepted.linkMinor() != core::image::kLinkMinor);
}

TEST_CASE("with no key at all - a Walter before the release key existed - everything is refused at check 1") {
  // spec/FIRMWARE-MANIFEST.md section 3: "A Walter with no release key compiled in accepts no
  // update at all. That is the state before the key exists, and it is safe." The build's own
  // table holds the release key now (keys/release-keys.txt), so the table with no key is made
  // here; what the build's table does with these fixtures is the case below - it does not hold
  // the specification's test key either, so they are refused there too.
  static const core::image::TrustTable build(nullptr, 0);
  REQUIRE(build.empty());
  REQUIRE_FALSE(core::image::trustsTestKey());
  static ImageVerdict verdict(build, unit());
  std::size_t refused = 0;
  for (std::size_t i = 0; i < fixtures::kSignatureCount; ++i) {
    const fixtures::SignatureCase& c = fixtures::kSignatures[i];
    const std::vector<std::uint8_t> manifest = fixtures::readSpec(c.manifest);
    const std::string line = fixtures::asText(fixtures::readSpec(c.sig));
    const std::vector<std::uint8_t> file = fixtures::readSpec("image-repeater.bin");
    const MemoryImage source(file.data(), static_cast<std::uint32_t>(file.size()));
    const Outcome o = verdict.judge(manifest.data(), manifest.size(), line.data(), line.size(), repeaterSlot(),
                                    Use::kSwd, &core::flasher::kRak4630Single, source);
    CAPTURE(c.manifest);
    CHECK(o.failed == Check::kSignature);
    CHECK(std::string(o.why) == "the key id names no key this build trusts");
    CHECK(verdict.accepted() == nullptr);
    ++refused;
  }
  for (std::size_t i = 0; i < fixtures::kVariantCount; ++i) {
    const fixtures::Variant& v = fixtures::kVariants[i];
    const std::vector<std::uint8_t> manifest = fixtures::readGenerated(v.manifest);
    const std::string line = fixtures::asText(fixtures::readGenerated(v.sig));
    const MemoryImage source(nullptr, 0);
    const Outcome o = verdict.judge(manifest.data(), manifest.size(), line.data(), line.size(), repeaterSlot(),
                                    Use::kSwd, &core::flasher::kRak4630Single, source);
    CAPTURE(v.name);
    CHECK(o.failed == Check::kSignature);
    ++refused;
  }
  MESSAGE("empty trust table: " << refused << " manifests, all refused at check 1");
}

TEST_CASE("check 1: every invalid signature case of index.json is refused there, and nothing after it is run") {
  Bench b;
  for (std::size_t i = 0; i < fixtures::kSignatureCount; ++i) {
    const fixtures::SignatureCase& c = fixtures::kSignatures[i];
    if (c.valid) continue;
    b.spec(c.manifest, c.sig, "image-repeater.bin");
    const Outcome o = b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
    CAPTURE(c.manifest);
    CAPTURE(c.sig);
    CHECK(o.failed == Check::kSignature);
    // Nothing was parsed: the verdict's manifest is blank.
    CHECK_FALSE(b.verdict.manifest().has_schema);
    MESSAGE("check 1 refused " << std::string(c.manifest) << " + " << std::string(c.sig) << ": " << std::string(o.why));
  }
}

TEST_CASE("a refusal at each check, from the signed variants and from what the caller brings") {
  Bench b;
  struct Expect {
    const char* variant;
    Check check;
    const char* why;
  };
  const Expect table[] = {
      {"repeater-unknown-product", Check::kManifest, "unknown product"},
      {"repeater-schema-2", Check::kManifest, "schema is not 1"},
      {"repeater-many-images", Check::kManifest, "more images than this firmware holds"},
      {"repeater-requires-link-2", Check::kImage, "the image requires a link major this build does not speak"},
      {"repeater-link-2", Check::kImage, "the image requires a link major this build does not speak"},
      {"repeater-requires-walter-9", Check::kImage, "the image requires a newer Walter than this build"},
      {"repeater-two-descriptors", Check::kDescriptor, "the file holds more than one valid descriptor"},
      {"repeater-broken-descriptor", Check::kDescriptor, "the file holds no valid descriptor"},
      {"repeater-other-board-descriptor", Check::kDescriptor, "the descriptor's board is not the manifest entry's"},
      {"repeater-other-version-descriptor", Check::kDescriptor, "the descriptor's version is not the manifest's"},
      {"repeater-region-outside-profile", Check::kRegion, "the manifest's app_region is not inside this build's region for the board"},
      {"repeater-region-above-profile", Check::kRegion, "the manifest's app_region is not inside this build's region for the board"},
  };
  for (const Expect& e : table) {
    b.variant(variantNamed(e.variant));
    const Outcome o = b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
    CAPTURE(e.variant);
    CHECK(o.failed == e.check);
    CHECK(std::string(o.why) == e.why);
    MESSAGE(std::string(core::image::describe(o.failed)) << " refused " << std::string(e.variant) << ": " << std::string(o.why));
  }

  // Check 3 from the slot's side: no image for this board, this band, this role.
  b.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  Outcome o = b.judge(repeaterSlot("rak4630-dual-a"), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kImage);
  CHECK(std::string(o.why) == "no image in the manifest has this slot's role, board and band");
  MESSAGE(std::string(core::image::describe(o.failed)) << " refused the repeater manifest for board rak4630-dual-a: " << std::string(o.why));
  o = b.judge(repeaterSlot("rak4630-single", "433"), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kImage);
  o = b.judge(repeaterSlot("rak4630-single", "868", "scanner"), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kImage);
  // And from this build's side: a development build meets no requires.walter.
  Bench dev(unit("0.0.0-dev"));
  dev.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  o = dev.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kImage);
  CHECK(std::string(o.why) == "the image requires a newer Walter than this build");
  MESSAGE(std::string(core::image::describe(o.failed)) << " refused the repeater manifest on a 0.0.0-dev build: " << std::string(o.why));

  // Check 4: a wrong size, a wrong hash.
  Bench c;
  c.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  c.file.pop_back();
  o = c.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kFile);
  CHECK(std::string(o.why) == "the file's size is not the manifest's");
  MESSAGE(std::string(core::image::describe(o.failed)) << " refused image-repeater.bin one byte short: " << std::string(o.why));
  c.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  c.file[4000] ^= 0x80;
  o = c.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kFile);
  CHECK(std::string(o.why) == "the file's SHA-256 is not the manifest's");
  MESSAGE(std::string(core::image::describe(o.failed)) << " refused image-repeater.bin with one bit changed: " << std::string(o.why));
  // The two-descriptor image, under the manifest that names the good one: it is the hash that
  // refuses it, at check 4, before its descriptors are counted.
  c.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-two-descriptors.bin");
  o = c.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK(o.failed == Check::kFile);

  // Check 6 from the caller's side: an swd file with no profile, or the wrong board's.
  c.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  o = c.judge(repeaterSlot(), Use::kSwd, nullptr);
  CHECK(o.failed == Check::kRegion);
  CHECK(std::string(o.why) == "no board profile for this slot in this build");
  core::flasher::BoardProfile other = core::flasher::kRak4630Single;
  other.name = "rak4630-dual-a";
  o = c.judge(repeaterSlot(), Use::kSwd, &other);
  CHECK(o.failed == Check::kRegion);
  CHECK(std::string(o.why) == "the board profile given is not the image's board");
  // And a profile whose region the file does not fit, though the manifest's region passes.
  core::flasher::BoardProfile narrow = core::flasher::kRak4630Single;
  narrow.app = core::nrf52::FlashRegion{0x00026000, 0x000D4000};
  o = c.judge(repeaterSlot(), Use::kSwd, &narrow);
  CHECK(o.ok());  // the fixture's 4096 bytes at 0x26000 fit any of these
  // An ota file is never a region question: the walter manifest with no profile at all.
  c.spec("manifest-walter.json", "manifest-walter.json.sig", "image-walter-ota.bin");
  o = c.judge(walterSelf(), Use::kOta, nullptr);
  CHECK(o.ok());
}

TEST_CASE("the accepted variants: reordered bytes, no requires, the right one of three images") {
  Bench b;
  b.variant(variantNamed("repeater-reordered"));
  Outcome o = b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK_MESSAGE(o.ok(), o.why);
  CHECK(std::string(b.token().image().id) == "repeater.rak4630-single.868");
  Bench dev(unit("0.0.0-dev"));
  dev.variant(variantNamed("repeater-no-requires"));
  o = dev.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK_MESSAGE(o.ok(), o.why);
  CHECK(dev.verdict.accepted() != nullptr);
  Bench three;
  three.variant(variantNamed("repeater-three-images"));
  o = three.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single);
  CHECK_MESSAGE(o.ok(), o.why);
  CHECK(std::string(three.token().image().id) == "repeater.rak4630-single.868");
  CHECK(std::string(three.token().image().band) == "868");
  CHECK(three.verdict.manifest().image_count == 3);
}

TEST_CASE("the token: an AcceptedImage comes from a verdict and from nowhere else") {
  // A type, not a convention: nothing outside ImageVerdict can construct one. These are
  // compile-time facts; the store (task p2-02) relies on them.
  static_assert(!std::is_default_constructible<AcceptedImage>::value,
                "an AcceptedImage must not be constructible outside the verdict");
  static_assert(std::is_copy_constructible<AcceptedImage>::value, "the store may keep a copy");
  static_assert(std::is_copy_assignable<AcceptedImage>::value, "and assign one");
  // Before any judge() there is no token; after an accepting one there is; after a refusal there
  // is none again - and a copy taken in between is the caller's to keep.
  Bench b;
  CHECK(b.verdict.accepted() == nullptr);
  b.spec("manifest-repeater.json", "manifest-repeater.json.sig", "image-repeater.bin");
  REQUIRE(b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single).ok());
  const AcceptedImage kept = b.token();
  b.file[0] ^= 1;
  REQUIRE(b.judge(repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single).failed == Check::kFile);
  CHECK(b.verdict.accepted() == nullptr);
  CHECK(std::string(kept.image().id) == "repeater.rak4630-single.868");
}
