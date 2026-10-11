// The image descriptor (task p0-02): spec/FIRMWARE-MANIFEST.md §4, held against every entry of
// spec/fixtures/manifest/index.json - which spec/tools/manifestref.py generated, so agreeing with
// it is agreeing with the reference implementation byte for byte.
//
// The builder is what puts the descriptor in this firmware's own image; the parser and the finder
// are what phase 2 reads a radio's image with, which is why the invalid fixtures - two
// descriptors, and one whose CRC is broken - matter as much as the valid ones.
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/image/descriptor.h"
#include "core/image/this_image.h"
#include "descriptor_fixtures.h"

namespace {

using core::image::Fields;
using core::image::Role;

// TWINOAK_SPEC_FIXTURES is spec/fixtures/manifest, from host_test/CMakeLists.txt.
std::vector<std::uint8_t> read_fixture(const char* name) {
  const std::string path = std::string(TWINOAK_SPEC_FIXTURES) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read the fixture " << path);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> from_hex(const char* hex) {
  const std::string text = hex;
  REQUIRE(text.size() % 2 == 0);
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string hex_of(const std::uint8_t* bytes, std::size_t length) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

// Copied by hand and not with strncpy: the arrays are already zero (Fields' member initialisers),
// and a truncating strncpy is a -Wstringop-truncation away from being an error here.
void copy_into(char* out, std::size_t size, const char* text) {
  REQUIRE(std::strlen(text) < size);
  for (std::size_t i = 0; text[i] != 0; ++i) {
    out[i] = text[i];
  }
}

Fields fields_of(const fixtures::Descriptor& f) {
  Fields fields;
  fields.role = static_cast<Role>(f.role);
  fields.slot = f.slot;
  fields.link_major = f.link_major;
  fields.link_minor = f.link_minor;
  fields.flags = f.flags;
  copy_into(fields.version, core::image::kVersionSize, f.version);
  copy_into(fields.board, core::image::kBoardSize, f.board);
  copy_into(fields.band, core::image::kBandSize, f.band);
  return fields;
}

}  // namespace

TEST_CASE("the fixture index has the cases section 4 describes") {
  // A guard on the generator, not on the specification: if index.json ever came through with no
  // descriptors, every loop below would pass by doing nothing.
  CHECK(fixtures::kDescriptorCount >= 4);
  std::size_t valid = 0;
  std::size_t invalid = 0;
  for (const auto& f : fixtures::kDescriptors) {
    (f.valid ? valid : invalid)++;
  }
  CHECK(valid >= 2);
  CHECK(invalid >= 2);
}

TEST_CASE("build() makes the bytes manifestref.py made, for every valid fixture") {
  for (const auto& f : fixtures::kDescriptors) {
    if (!f.valid) {
      continue;
    }
    CAPTURE(f.image);
    const core::image::Built built = core::image::build(fields_of(f));
    REQUIRE(built.ok);
    CHECK(hex_of(built.descriptor.bytes, core::image::kDescriptorSize) == std::string(f.hex));
  }
}

TEST_CASE("parse() reads back the fields index.json records, for every valid fixture") {
  for (const auto& f : fixtures::kDescriptors) {
    if (!f.valid) {
      continue;
    }
    CAPTURE(f.image);
    const std::vector<std::uint8_t> bytes = from_hex(f.hex);
    REQUIRE(bytes.size() == core::image::kDescriptorSize);

    Fields fields;
    REQUIRE(core::image::parse(bytes.data(), bytes.size(), &fields));
    CHECK(static_cast<std::uint8_t>(fields.role) == f.role);
    CHECK(std::string(core::image::role_name(fields.role)) == std::string(f.role_name));
    CHECK(fields.slot == f.slot);
    CHECK(fields.link_major == f.link_major);
    CHECK(fields.link_minor == f.link_minor);
    CHECK(fields.flags == f.flags);
    CHECK(std::string(fields.version) == std::string(f.version));
    CHECK(std::string(fields.board) == std::string(f.board));
    CHECK(std::string(fields.band) == std::string(f.band));
  }
}

TEST_CASE("find() finds exactly one descriptor in each valid fixture image, where index.json says") {
  for (const auto& f : fixtures::kDescriptors) {
    if (!f.valid) {
      continue;
    }
    CAPTURE(f.image);
    const std::vector<std::uint8_t> image = read_fixture(f.image);
    const core::image::Found found = core::image::find(image.data(), image.size());
    REQUIRE(found.count == 1);
    CHECK(found.offset == f.offset);
    CHECK(std::string(found.fields.version) == std::string(f.version));
    CHECK(std::string(found.fields.board) == std::string(f.board));
    CHECK(std::string(found.fields.band) == std::string(f.band));
    CHECK(static_cast<std::uint8_t>(found.fields.role) == f.role);
    // The bytes at that offset are the bytes index.json records.
    CHECK(hex_of(image.data() + found.offset, core::image::kDescriptorSize) == std::string(f.hex));
  }
}

TEST_CASE("find() refuses every image index.json marks invalid, and for its reason") {
  for (const auto& f : fixtures::kDescriptors) {
    if (f.valid) {
      continue;
    }
    CAPTURE(f.image);
    CAPTURE(f.why);
    const std::vector<std::uint8_t> image = read_fixture(f.image);
    const core::image::Found found = core::image::find(image.data(), image.size());
    // Not one: that is what §4 and §5 check 5 refuse on. The reason is the count itself - two
    // valid descriptors, or none because the only one fails its CRC.
    CHECK(found.count != 1);
    const std::string why = f.why;
    if (why.find("two") != std::string::npos) {
      CHECK(found.count == 2);
    } else {
      CHECK(found.count == 0);
    }
  }
}

TEST_CASE("a descriptor is found only on a 4-byte boundary") {
  const core::image::Built built =
      core::image::build(Role::kRepeater, 0, 1, 0, 0, "1.2.3+abcdefa", "rak4630-single", "868");
  REQUIRE(built.ok);

  for (std::size_t shift = 0; shift < 4; ++shift) {
    CAPTURE(shift);
    std::vector<std::uint8_t> image(256, 0x5A);
    std::memcpy(image.data() + 64 + shift, built.descriptor.bytes, core::image::kDescriptorSize);
    const core::image::Found found = core::image::find(image.data(), image.size());
    if (shift == 0) {
      REQUIRE(found.count == 1);
      CHECK(found.offset == 64);
    } else {
      CHECK(found.count == 0);  // the magic is there; the boundary is not (§4)
    }
  }
}

TEST_CASE("a descriptor that runs off the end of the image is not a descriptor") {
  const core::image::Built built =
      core::image::build(Role::kManager, core::image::kAnySlot, 1, 0, 0, "9.9.9", "walter", "lte-m");
  REQUIRE(built.ok);
  std::vector<std::uint8_t> image(built.descriptor.bytes,
                                  built.descriptor.bytes + core::image::kDescriptorSize);
  CHECK(core::image::find(image.data(), image.size()).count == 1);
  image.pop_back();
  CHECK(core::image::find(image.data(), image.size()).count == 0);
  CHECK(core::image::find(image.data(), 0).count == 0);
  CHECK(core::image::find(nullptr, 96).count == 0);
}

TEST_CASE("parse() refuses the three ways a descriptor can be wrong") {
  const core::image::Built built = core::image::build(
      Role::kManager, core::image::kAnySlot, 1, 0, 0, "1.0.0+0000000", "walter", "lte-m");
  REQUIRE(built.ok);
  Fields fields;
  REQUIRE(core::image::parse(built.descriptor.bytes, core::image::kDescriptorSize, &fields));

  SUBCASE("the magic") {
    core::image::Descriptor d = built.descriptor;
    d.bytes[7] = '2';
    CHECK_FALSE(core::image::parse(d.bytes, core::image::kDescriptorSize, &fields));
  }
  SUBCASE("the declared length") {
    core::image::Descriptor d = built.descriptor;
    d.bytes[8] = 97;
    CHECK_FALSE(core::image::parse(d.bytes, core::image::kDescriptorSize, &fields));
  }
  SUBCASE("the crc, for a flipped bit anywhere in the first 92 bytes") {
    for (std::size_t i = 0; i < 92; ++i) {
      core::image::Descriptor d = built.descriptor;
      d.bytes[i] ^= 0x01;
      CAPTURE(i);
      CHECK_FALSE(core::image::parse(d.bytes, core::image::kDescriptorSize, &fields));
    }
  }
  SUBCASE("fewer than 96 bytes") {
    CHECK_FALSE(core::image::parse(built.descriptor.bytes, 95, &fields));
    CHECK_FALSE(core::image::parse(nullptr, 96, &fields));
  }
  SUBCASE("out may be null: the checks still run") {
    CHECK(core::image::parse(built.descriptor.bytes, core::image::kDescriptorSize, nullptr));
    core::image::Descriptor d = built.descriptor;
    d.bytes[0] = 'X';
    CHECK_FALSE(core::image::parse(d.bytes, core::image::kDescriptorSize, nullptr));
  }
}

TEST_CASE("build() refuses a text that does not fit its field, terminator included") {
  const std::string v31(31, 'v');
  const std::string v32(32, 'v');
  CHECK(core::image::build(Role::kManager, 0xFF, 1, 0, 0, "", "walter", "lte-m").ok);
  CHECK(core::image::build(Role::kManager, 0xFF, 1, 0, 0, v31.c_str(), "walter", "lte-m").ok);
  CHECK_FALSE(core::image::build(Role::kManager, 0xFF, 1, 0, 0, v32.c_str(), "walter", "lte-m").ok);
  const std::string b23(23, 'b');
  const std::string b24(24, 'b');
  CHECK(core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0", b23.c_str(), "lte-m").ok);
  CHECK_FALSE(core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0", b24.c_str(), "lte-m").ok);
  CHECK(core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0", "walter", "1234567").ok);
  CHECK_FALSE(core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0", "walter", "12345678").ok);
  // Not ASCII: §4 says the three strings are ASCII.
  CHECK_FALSE(core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0\xc3\xa6", "walter", "lte-m").ok);
  // A refusal leaves nothing half-built behind.
  const core::image::Built bad =
      core::image::build(Role::kManager, 0xFF, 1, 0, 0, v32.c_str(), "walter", "lte-m");
  CHECK_FALSE(bad.ok);
  for (std::size_t i = 0; i < core::image::kDescriptorSize; ++i) {
    CHECK(bad.descriptor.bytes[i] == 0);
  }
}

TEST_CASE("the reserved bytes are zero and the length is 96") {
  const core::image::Built built =
      core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0", "walter", "lte-m");
  REQUIRE(built.ok);
  const std::uint8_t* b = built.descriptor.bytes;
  CHECK(b[8] == 96);
  CHECK(b[9] == 0);
  for (std::size_t i = 80; i < 92; ++i) {
    CHECK(b[i] == 0);
  }
  // Zero padding after each string, so two builds of the same fields are the same bytes and a
  // manifest can record one hash.
  const core::image::Built again =
      core::image::build(Role::kManager, 0xFF, 1, 0, 0, "1.0.0", "walter", "lte-m");
  CHECK(std::memcmp(b, again.descriptor.bytes, core::image::kDescriptorSize) == 0);
}

TEST_CASE("crc32 is the one zlib computes") {
  // The check value of CRC-32/ISO-HDLC, the algorithm §4 names: "123456789" -> 0xCBF43926.
  const std::uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(core::image::crc32(data, sizeof(data)) == 0xCBF43926u);
  CHECK(core::image::crc32(nullptr, 0) == 0u);
  const std::uint8_t zero[1] = {0};
  CHECK(core::image::crc32(zero, 1) == 0xD202EF8Du);
}

TEST_CASE("this firmware's own descriptor says what the manifest will say about it") {
  const Fields self = core::image::this_image();
  CHECK(self.role == Role::kManager);
  CHECK(std::string(core::image::role_name(self.role)) == "manager");
  CHECK(std::string(self.board) == "walter");
  CHECK(std::string(self.band) == "lte-m");
  CHECK(self.slot == core::image::kAnySlot);
  CHECK(self.link_major == core::image::kLinkMajor);
  CHECK(self.link_minor == core::image::kLinkMinor);
  CHECK(self.flags == 0);
  // The one version string: the console's banner, the descriptor's field and the manifest's.
  CHECK(std::string(self.version) == TWINOAK_VERSION);

  // And the bytes in the object are a descriptor a reader of the image would find: exactly one,
  // on a 4-byte boundary.
  CHECK(reinterpret_cast<std::uintptr_t>(core::image::kThisImage.bytes) % 4 == 0);
  const core::image::Found found =
      core::image::find(core::image::kThisImage.bytes, core::image::kDescriptorSize);
  REQUIRE(found.count == 1);
  CHECK(found.offset == 0);
}

TEST_CASE("role_name knows the five roles of section 4 and nothing else") {
  CHECK(std::string(core::image::role_name(Role::kUnknown)) == "unknown");
  CHECK(std::string(core::image::role_name(Role::kRepeater)) == "repeater");
  CHECK(std::string(core::image::role_name(Role::kBridge)) == "bridge");
  CHECK(std::string(core::image::role_name(Role::kScanner)) == "scanner");
  CHECK(std::string(core::image::role_name(Role::kManager)) == "manager");
  // A number section 4 does not give a name is not quietly given one.
  CHECK(core::image::role_name(static_cast<Role>(7)) == nullptr);
  CHECK(core::image::role_name(static_cast<Role>(255)) == nullptr);
}
