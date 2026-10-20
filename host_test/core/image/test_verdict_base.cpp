// Check 5p (task p5-01): spec/FIRMWARE-MANIFEST.md §5 - "For a `provision` file, 5 and 6 are one
// check instead: the image's app_region lies inside the region this Walter build knows for the
// board, and the file passes §4a against **the build's** region".
//
// What it has to show: the specification's own base manifest passes 1 to 4 and 5p; the manifest
// cannot widen the region, and the file is judged against the board's and not the manifest's; a
// `provision` file a manifest describes wrongly is refused at check 2 with §2's words; and the
// token that comes of it is an AcceptedBase, never an AcceptedImage.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/flasher/board_profile.h"
#include "core/image/base.h"
#include "core/image/verdict.h"
#include "fixtures/manifest_files.h"
#include "store_bench.h"

using bench::BaseFile;
using bench::Slot;
using bench::StoreBench;
using core::image::AcceptedBase;
using core::image::Check;
using core::image::ImageVerdict;
using core::flasher::MemoryImage;
using core::image::Outcome;
using core::image::SlotNeeds;
using core::image::Use;
using core::image::ValidateReason;

namespace {

// The slot a base is judged for: the repeater's image, on the board this build knows.
SlotNeeds repeaterSlot() {
  SlotNeeds needs;
  needs.slot = Slot{0};
  needs.role = "repeater";
  needs.board = core::flasher::kRak4630Single.name;
  needs.band = "868";
  return needs;
}

core::image::ThisUnit unit() { return StoreBench::makeUnit(); }

// One judgement of a `provision` file: a manifest, its signature, the file's bytes.
struct Judged {
  Outcome outcome;
  const AcceptedBase* token = nullptr;
};

Judged judgeProvision(ImageVerdict& verdict, const std::vector<std::uint8_t>& manifest,
                      const std::string& signature, const std::vector<std::uint8_t>& file) {
  const MemoryImage source(file.data(), static_cast<std::uint32_t>(file.size()));
  Judged out;
  out.outcome = verdict.judge(manifest.data(), manifest.size(), signature.data(),
                              signature.size(), repeaterSlot(), Use::kProvision,
                              &core::flasher::kRak4630Single, source);
  out.token = verdict.acceptedBase();
  return out;
}

struct Bench {
  fixtures::TestTrust trust;
  core::image::ThisUnit u = unit();
  ImageVerdict verdict{trust.table, u};
};

}  // namespace

TEST_CASE("the specification's base manifest passes 1 to 4 and 5p, and the token is a base") {
  Bench b;
  const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater-base.json");
  const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater-base.json.sig"));
  const std::vector<std::uint8_t> file = fixtures::readSpec("base-rak4630.hex");
  const Judged judged = judgeProvision(b.verdict, manifest, sig, file);
  REQUIRE_MESSAGE(judged.outcome.ok(),
                  core::image::describe(judged.outcome.failed) << ": " << judged.outcome.why);
  REQUIRE(judged.token != nullptr);
  // No AcceptedImage came of it: a base is not an image, and the store's doors say so.
  CHECK(b.verdict.accepted() == nullptr);

  const AcceptedBase base = *judged.token;  // a copy, as the store keeps one
  CHECK(base.use() == Use::kProvision);
  CHECK(base.slot().index == 0);
  CHECK(std::string(base.file().name) == "twinoak-base-rak4630-s140-6.1.1.hex");
  CHECK(std::string(base.file().format) == "hex");
  CHECK(base.file().size == file.size());
  CHECK(std::string(base.image().id) == "repeater.rak4630-single.868");
  CHECK(fixtures::toHex(base.keyId(), 8) == std::string(fixtures::kTestKeyId));
  // Judged against **this build's** region for the board, not the manifest's example one.
  CHECK(base.regionStart() == core::flasher::kRak4630Single.app.begin);
  CHECK(base.regionEnd() == core::flasher::kRak4630Single.app.end);
  // And what §4a found in it: index.json's summary for base-rak4630.hex.
  CHECK(base.summary().bootloader == 0x000F4000u);
  CHECK(base.summary().flash_bytes == 504u);
  CHECK(base.summary().softdevice_id == 140u);
  CHECK(base.summary().softdevice_version == 6001001u);
  CHECK(base.summary().uicr_word_count == 2u);
  CHECK(base.runs().count == 5u);
  // doctest's stream does not take std::hex, so the address is spelled out here.
  MESSAGE("accepted as a base: bootloader " << fixtures::toHex(
              reinterpret_cast<const std::uint8_t*>(&base.summary().bootloader), 4)
          << " (little-endian), " << base.summary().flash_bytes
          << " bytes of flash in " << base.runs().count
          << " runs, judged against the board's region 0x26000 to 0xED000");
}

TEST_CASE("5p judges the file against the board's region, which a manifest cannot widen") {
  SUBCASE("a manifest whose app_region reaches past the board's: refused at 5p before the file") {
    Bench b;
    const std::vector<std::uint8_t> manifest = fixtures::readGenerated("repeater-base-widened.json");
    const std::string sig = fixtures::asText(fixtures::readGenerated("repeater-base-widened.json.sig"));
    const std::vector<std::uint8_t> file = fixtures::readSpec("base-rak4630.hex");
    const Judged judged = judgeProvision(b.verdict, manifest, sig, file);
    CHECK(judged.outcome.failed == Check::kBase);
    CHECK(std::string(judged.outcome.why) ==
          "the manifest's app_region is not inside this build's region for the board");
    CHECK(judged.token == nullptr);
  }
  SUBCASE("a file that is a base against the manifest's region and not against the board's") {
    // base-in-the-manifest-region.hex puts the bootloader at 0xD4000 - the end of the fixture
    // manifest's example region, and inside the board's application region. §4a rule 6 is met
    // against the manifest's numbers and broken against the board's, and 5p uses the board's.
    Bench b;
    const BaseFile base("base-in-the-manifest-region");
    REQUIRE_FALSE(base.row->valid_for_the_board);
    // The reference accepts it against the manifest's region ...
    static core::image::BaseReader reader;
    const MemoryImage source(base.file.data(), static_cast<std::uint32_t>(base.file.size()));
    const core::image::BaseResult against_manifest =
        reader.read(source, static_cast<std::uint32_t>(base.row->region_start),
                    static_cast<std::uint32_t>(base.row->region_end));
    CHECK_MESSAGE(against_manifest.ok(), against_manifest.why);
    // ... and the verdict refuses it, because the verdict uses the board's.
    const Judged judged = judgeProvision(b.verdict, base.manifest, base.signature, base.file);
    CHECK(judged.outcome.failed == Check::kBase);
    CHECK(std::string(judged.outcome.why) ==
          "UICR.NRFFW[0] = 0x000D4000: not a page at or above the application region's end that "
          "the file fills");
    CHECK(judged.token == nullptr);
  }
}

TEST_CASE("every base fixture of the specification, through the whole of the verdict") {
  // The same cases as test_base.cpp, reached the way a unit reaches them: a signed manifest whose
  // `provision` entry names these bytes. The refusal is check 5p's, with §4a's own sentence.
  Bench b;
  const std::vector<std::uint8_t> good = fixtures::readSpec("base-rak4630.hex");
  for (std::size_t i = 0; i < fixtures::kBaseCount; ++i) {
    const fixtures::BaseCase& c = fixtures::kBases[i];
    if (c.valid) continue;
    CAPTURE(c.file);
    // A manifest naming this file's bytes, signed with the test key: made here from the
    // specification's, so that only the entry's size and hash differ.
    const std::vector<std::uint8_t> file = fixtures::readSpec(c.file);
    const std::vector<std::uint8_t> manifest =
        fixtures::readGenerated((std::string("base-refused-") + c.file + ".json").c_str());
    const std::string sig = fixtures::asText(
        fixtures::readGenerated((std::string("base-refused-") + c.file + ".json.sig").c_str()));
    const Judged judged = judgeProvision(b.verdict, manifest, sig, file);
    CHECK(judged.outcome.failed == Check::kBase);
    CHECK(std::string(judged.outcome.why) == std::string(c.why));
    CHECK(judged.token == nullptr);
    CHECK(b.verdict.accepted() == nullptr);
    // The verdict keeps what §4a said, for the console's line.
    CHECK(b.verdict.base().reason != core::image::BaseReason::kNone);
  }
  CHECK(good.size() > 0);
}

TEST_CASE("a manifest that describes a provision file wrongly is refused at check 2, in section 2's words") {
  struct Case {
    const char* variant;
    ValidateReason reason;
  };
  const Case cases[] = {
      {"repeater-base-load-address", ValidateReason::kProvisionLoadAddress},
      {"repeater-base-format-bin", ValidateReason::kProvisionNotHex},
      {"repeater-base-hex-swd", ValidateReason::kHexIsForProvision},
      {"repeater-base-not-nrf52840", ValidateReason::kProvisionNotNrf52840},
      {"repeater-base-twice", ValidateReason::kTwoProvisionFiles},
  };
  for (const Case& c : cases) {
    Bench b;
    CAPTURE(c.variant);
    const std::vector<std::uint8_t> manifest =
        fixtures::readGenerated((std::string(c.variant) + ".json").c_str());
    const std::string sig =
        fixtures::asText(fixtures::readGenerated((std::string(c.variant) + ".json.sig").c_str()));
    const std::vector<std::uint8_t> file = fixtures::readSpec("base-rak4630.hex");
    const Judged judged = judgeProvision(b.verdict, manifest, sig, file);
    CHECK(judged.outcome.failed == Check::kManifest);
    CHECK(std::string(judged.outcome.why) == std::string(core::image::describe(c.reason)));
    CHECK(judged.token == nullptr);
  }
}

TEST_CASE("the three images of a release are still accepted, and a base is not one of them") {
  // The manifest with a `provision` file in it is the repeater's manifest with one more file: the
  // `swd` file of the same image is accepted as before, which is what "an existing manifest
  // without a provision file is accepted exactly as before" means from the other side.
  Bench b;
  const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater-base.json");
  const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater-base.json.sig"));
  const std::vector<std::uint8_t> image = fixtures::readSpec("image-repeater.bin");
  const MemoryImage source(image.data(), static_cast<std::uint32_t>(image.size()));
  const Outcome o = b.verdict.judge(manifest.data(), manifest.size(), sig.data(), sig.size(),
                                    repeaterSlot(), Use::kSwd, &core::flasher::kRak4630Single,
                                    source);
  REQUIRE_MESSAGE(o.ok(), core::image::describe(o.failed) << ": " << o.why);
  REQUIRE(b.verdict.accepted() != nullptr);
  CHECK(b.verdict.accepted()->use() == Use::kSwd);
  CHECK(b.verdict.acceptedBase() == nullptr);  // the swd file is an image, not a base
}

TEST_CASE("a provision file asked for where there is none, and an image asked for where there is none") {
  Bench b;
  SUBCASE("the specification's plain repeater manifest has no provision file") {
    const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater.json");
    const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater.json.sig"));
    const std::vector<std::uint8_t> file = fixtures::readSpec("base-rak4630.hex");
    const Judged judged = judgeProvision(b.verdict, manifest, sig, file);
    CHECK(judged.outcome.failed == Check::kImage);
    CHECK(std::string(judged.outcome.why) ==
          std::string(core::image::describe(core::image::ChooseReason::kNoFileOfUse)));
  }
}
