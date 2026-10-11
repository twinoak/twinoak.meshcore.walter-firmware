// The file's checks as a stream (task p2-01): the descriptor scanner held against find() over
// every fixture image at every chunk size, the hasher's two refusals, and the descriptor against
// the manifest entry, field by field.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/image/file_check.h"
#include "core/image/manifest.h"
#include "descriptor_fixtures.h"
#include "fixtures/manifest_files.h"

using core::image::DescriptorMismatch;
using core::image::DescriptorScanner;
using core::image::Fields;
using core::image::FileHasher;
using core::image::Found;

namespace {

Found scanned(const std::vector<std::uint8_t>& image, std::size_t piece) {
  DescriptorScanner s;
  for (std::size_t at = 0; at < image.size(); at += piece) {
    const std::size_t take = piece < image.size() - at ? piece : image.size() - at;
    s.update(image.data() + at, take);
  }
  return s.result();
}

bool sameFields(const Fields& a, const Fields& b) { return std::memcmp(&a, &b, sizeof(Fields)) == 0; }

}  // namespace

TEST_CASE("the scanner finds what find() finds, in every fixture image, however the bytes are cut") {
  for (const auto& f : fixtures::kDescriptors) {
    CAPTURE(f.image);
    const std::vector<std::uint8_t> image = fixtures::readSpec(f.image);
    const Found whole = core::image::find(image.data(), image.size());
    for (std::size_t piece : {1u, 2u, 3u, 4u, 5u, 7u, 16u, 95u, 96u, 97u, 100u, 191u, 192u, 193u, 256u, 1000u, 4096u, 8192u}) {
      CAPTURE(piece);
      const Found streamed = scanned(image, piece);
      CHECK(streamed.count == whole.count);
      if (whole.count > 0) {
        CHECK(streamed.offset == whole.offset);
        CHECK(sameFields(streamed.fields, whole.fields));
      }
    }
    if (f.valid) {
      CHECK(whole.count == 1);
      CHECK(whole.offset == f.offset);
    } else {
      CHECK(whole.count != 1);
    }
  }
}

TEST_CASE("the scanner: a descriptor at the very end, at the start, straddling a window, and unaligned") {
  const core::image::Built built =
      core::image::build(core::image::Role::kRepeater, 0xFF, 1, 0, 0, "1.0.0", "rak4630-single", "868");
  REQUIRE(built.ok);
  for (std::size_t at : {0u, 4u, 92u, 96u, 100u, 188u, 192u, 196u, 300u, 904u}) {
    std::vector<std::uint8_t> image(1000, 0x5A);
    std::memcpy(image.data() + at, built.descriptor.bytes, 96);
    for (std::size_t piece : {1u, 13u, 96u, 1000u}) {
      CAPTURE(at);
      CAPTURE(piece);
      const Found s = scanned(image, piece);
      REQUIRE(s.count == 1);
      CHECK(s.offset == at);
      CHECK(std::string(s.fields.version) == "1.0.0");
    }
  }
  // Unaligned: the magic is there, the boundary is not (section 4), at every cut.
  for (std::size_t shift = 1; shift < 4; ++shift) {
    std::vector<std::uint8_t> image(1000, 0x5A);
    std::memcpy(image.data() + 200 + shift, built.descriptor.bytes, 96);
    for (std::size_t piece : {1u, 7u, 96u, 1000u}) {
      CHECK(scanned(image, piece).count == 0);
    }
  }
  // Two, and the first one's fields are reported with count 2.
  std::vector<std::uint8_t> two(1000, 0x5A);
  std::memcpy(two.data() + 8, built.descriptor.bytes, 96);
  std::memcpy(two.data() + 500, built.descriptor.bytes, 96);
  const Found s = scanned(two, 33);
  CHECK(s.count == 2);
  CHECK(s.offset == 8);
  // An image shorter than a descriptor holds none; result() may be asked twice.
  DescriptorScanner short_scan;
  short_scan.update(built.descriptor.bytes, 95);
  CHECK(short_scan.result().count == 0);
  CHECK(short_scan.result().count == 0);
  short_scan.update(built.descriptor.bytes + 95, 1);
  CHECK(short_scan.result().count == 1);
  CHECK(short_scan.result().count == 1);
  short_scan.reset();
  CHECK(short_scan.result().count == 0);
}

TEST_CASE("the hasher: the fixture file against its entry, a wrong size, a wrong hash") {
  static core::image::ManifestParser parser;
  core::image::Manifest m;
  const std::string text = fixtures::asText(fixtures::readSpec("manifest-repeater.json"));
  REQUIRE(parser.parse(text.data(), text.size(), &m).ok);
  const core::image::FileEntry& entry = m.images[0].files[0];
  const std::vector<std::uint8_t> image = fixtures::readSpec("image-repeater.bin");
  REQUIRE(image.size() == entry.size);

  FileHasher h;
  h.begin(entry.size, entry.sha256);
  for (std::size_t at = 0; at < image.size(); at += 300) {
    const std::size_t take = 300 < image.size() - at ? 300 : image.size() - at;
    h.update(image.data() + at, take);
  }
  CHECK(h.finish() == FileHasher::Result::kOk);
  CHECK(h.seen() == image.size());
  CHECK(fixtures::toHex(h.digest(), 32) == fixtures::toHex(entry.sha256, 32));

  // One byte short: the size, before the hash.
  h.begin(entry.size, entry.sha256);
  h.update(image.data(), image.size() - 1);
  CHECK(h.finish() == FileHasher::Result::kSizeMismatch);
  // One byte more.
  h.begin(entry.size, entry.sha256);
  h.update(image.data(), image.size());
  h.update(image.data(), 1);
  CHECK(h.finish() == FileHasher::Result::kSizeMismatch);
  // The right size, one bit different.
  std::vector<std::uint8_t> changed = image;
  changed[2000] ^= 0x01;
  h.begin(entry.size, entry.sha256);
  h.update(changed.data(), changed.size());
  CHECK(h.finish() == FileHasher::Result::kHashMismatch);
  CHECK(std::string(core::image::describe(FileHasher::Result::kHashMismatch)) == "the file's SHA-256 is not the manifest's");
}

TEST_CASE("the descriptor against the manifest entry: role, board, band, version, link major, slot") {
  static core::image::ManifestParser parser;
  core::image::Manifest m;
  const std::string text = fixtures::asText(fixtures::readSpec("manifest-repeater.json"));
  REQUIRE(parser.parse(text.data(), text.size(), &m).ok);
  const core::image::ImageEntry& image = m.images[0];
  const std::vector<std::uint8_t> bytes = fixtures::readSpec("image-repeater.bin");
  const Found found = core::image::find(bytes.data(), bytes.size());
  REQUIRE(found.count == 1);
  const core::board::Slot slot0{0};
  const core::board::Slot slot1{1};
  CHECK(core::image::descriptorAgrees(found.fields, m, image, slot0) == DescriptorMismatch::kNone);
  CHECK(core::image::descriptorAgrees(found.fields, m, image, slot1) == DescriptorMismatch::kNone);  // slot 0xFF: any

  Fields f = found.fields;
  f.role = core::image::Role::kScanner;
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kRole);
  f = found.fields;
  f.role = static_cast<core::image::Role>(7);  // a role this firmware has no name for
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kRole);
  f = found.fields;
  std::strcpy(f.board, "rak4630-dual-a");
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kBoard);
  f = found.fields;
  std::strcpy(f.band, "433");
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kBand);
  f = found.fields;
  std::strcpy(f.version, "0.3.1+abc1234");
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kVersion);
  f = found.fields;
  f.link_major = 2;
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kLinkMajor);
  f = found.fields;
  f.link_minor = 5;  // a minor is not held: section 5.1 says a minor adds, and the task holds the major
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kNone);
  f = found.fields;
  f.slot = 1;
  CHECK(core::image::descriptorAgrees(f, m, image, slot0) == DescriptorMismatch::kSlot);
  CHECK(core::image::descriptorAgrees(f, m, image, slot1) == DescriptorMismatch::kNone);
  CHECK(std::string(core::image::describe(DescriptorMismatch::kSlot)) == "the image is for another radio position than this slot");
}
