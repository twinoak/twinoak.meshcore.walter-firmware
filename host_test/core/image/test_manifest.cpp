// The manifest (task p2-01): both fixture manifests parsed into the bounded structures and
// validated, a reordered one parsed the same, and the refusals - each with its own reason, as
// spec/tools/manifestref.py's validate() gives it, or the parser's own where a bound is exceeded.
// The refusals are made by editing the fixture's text, so that each is one change away from a
// manifest that validates.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/flasher/board_profile.h"
#include "core/image/manifest.h"
#include "fixtures/manifest_files.h"

using core::image::HexField;
using core::image::Manifest;
using core::image::ManifestParser;
using core::image::ParseError;
using core::image::ParseResult;
using core::image::Product;
using core::image::Use;
using core::image::validate;
using core::image::ValidateReason;
using core::image::ValidateResult;

namespace {

std::string repeaterText() { return fixtures::asText(fixtures::readSpec("manifest-repeater.json")); }
std::string walterText() { return fixtures::asText(fixtures::readSpec("manifest-walter.json")); }

// The fixture with one substring replaced; the substring must be there exactly once.
std::string edited(const std::string& text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, "the fixture does not contain: " << from);
  REQUIRE_MESSAGE(text.find(from, at + 1) == std::string::npos, "the fixture contains twice: " << from);
  std::string out = text;
  out.replace(at, from.size(), to);
  return out;
}

struct Parsed {
  ParseResult result;
  Manifest manifest;
};

Parsed parse(const std::string& text) {
  // The parser's tokens are 20 KB: one static, as the firmware will keep it.
  static ManifestParser parser;
  Parsed p;
  p.result = parser.parse(text.data(), text.size(), &p.manifest);
  return p;
}

ValidateReason reasonOf(const std::string& text) {
  const Parsed p = parse(text);
  REQUIRE_MESSAGE(p.result.ok, core::image::describe(p.result.error) << " (" << p.result.field << ")");
  return validate(p.manifest).reason;
}

ParseError parseErrorOf(const std::string& text) {
  const Parsed p = parse(text);
  CHECK_FALSE(p.result.ok);
  return p.result.error;
}

}  // namespace

TEST_CASE("the repeater fixture parses into the structures and validates") {
  const Parsed p = parse(repeaterText());
  REQUIRE(p.result.ok);
  const Manifest& m = p.manifest;
  CHECK(m.has_schema);
  CHECK(m.schema == 1);
  CHECK(m.product == Product::kRepeater);
  CHECK(std::string(m.version) == "0.3.0+abc1234");
  CHECK(std::string(m.tag) == "v0.3.0");
  CHECK(m.has_commit);
  CHECK(m.commit_length == 40);
  CHECK(std::string(m.commit) == "abc1234000000000000000000000000000000000");
  CHECK(std::string(m.built_at) == "2026-10-12T08:00:00Z");
  CHECK(m.has_link);
  CHECK(m.link_major == 1);
  CHECK(m.link_minor == 0);
  REQUIRE(m.has_images);
  REQUIRE(m.image_count == 1);
  const core::image::ImageEntry& image = m.images[0];
  CHECK(std::string(image.id) == "repeater.rak4630-single.868");
  CHECK(std::string(image.role) == "repeater");
  CHECK(std::string(image.board) == "rak4630-single");
  CHECK(std::string(image.band) == "868");
  CHECK(std::string(image.mcu) == "nrf52840");
  CHECK(image.app_region_state == HexField::kOk);
  CHECK(image.app_region_start == 0x00026000);
  CHECK(image.app_region_end == 0x000D4000);
  CHECK(image.requires_present);
  CHECK_FALSE(image.requires_malformed);
  CHECK(image.has_requires_walter);
  CHECK(std::string(image.requires_walter) == "0.3.0");
  CHECK(image.has_requires_link_major);
  CHECK(image.requires_link_major == 1);
  REQUIRE(image.has_files);
  REQUIRE(image.file_count == 3);
  CHECK(image.files[0].use == Use::kSwd);
  CHECK(std::string(image.files[0].name) == "twinoak-repeater-rak4630-single-868-v0.3.0.bin");
  CHECK(std::string(image.files[0].format) == "bin");
  CHECK(image.files[0].has_size);
  CHECK(image.files[0].size == 4096);
  CHECK(image.files[0].has_sha256);
  CHECK(fixtures::toHex(image.files[0].sha256, 32) ==
        "662a969ee40557c75cdb9dec10d91477a848282914cdae9a740f08d1a4372b0a");
  CHECK(image.files[0].load_address_state == HexField::kOk);
  CHECK(image.files[0].load_address == 0x00026000);
  CHECK(image.files[1].use == Use::kUsbDfu);
  CHECK(image.files[1].size == 2048);
  CHECK(image.files[1].load_address_state == HexField::kAbsent);
  CHECK(image.files[2].use == Use::kUsbUf2);
  CHECK(image.files[2].size == 8192);
  CHECK(std::string(image.files[2].format) == "uf2");
  const ValidateResult v = validate(m);
  CHECK(v.ok);
  CHECK(v.reason == ValidateReason::kNone);
}

TEST_CASE("the walter fixture parses and validates") {
  const Parsed p = parse(walterText());
  REQUIRE(p.result.ok);
  const Manifest& m = p.manifest;
  CHECK(m.product == Product::kWalter);
  CHECK(std::string(m.version) == "0.3.0+def5678");
  REQUIRE(m.image_count == 1);
  const core::image::ImageEntry& image = m.images[0];
  CHECK(std::string(image.id) == "manager.walter.lte-m");
  CHECK(std::string(image.role) == "manager");
  CHECK(std::string(image.board) == "walter");
  CHECK(std::string(image.band) == "lte-m");
  CHECK(std::string(image.mcu) == "esp32s3");
  CHECK(image.app_region_state == HexField::kAbsent);  // no swd file, no region
  CHECK(image.requires_present);
  CHECK_FALSE(image.has_requires_walter);
  CHECK(image.has_requires_link_major);
  REQUIRE(image.file_count == 2);
  CHECK(image.files[0].use == Use::kOta);
  CHECK(image.files[0].load_address == 0x00020000);
  CHECK(image.files[0].size == 4096);
  CHECK(image.files[1].use == Use::kFactory);
  CHECK(image.files[1].load_address_state == HexField::kOk);
  CHECK(image.files[1].load_address == 0);
  CHECK(image.files[1].size == 8192);
  CHECK(validate(m).ok);
}

TEST_CASE("the canonical bytes are not required: reordered keys and other whitespace parse the same") {
  // gen_manifest_fixtures.py wrote the repeater manifest with sorted keys, a four-space indent,
  // spaced separators and CR LF; what is signed is the bytes, and what is parsed is the content.
  const Parsed a = parse(repeaterText());
  const Parsed b = parse(fixtures::asText(fixtures::readGenerated("repeater-reordered.json")));
  REQUIRE(a.result.ok);
  REQUIRE(b.result.ok);
  CHECK(validate(b.manifest).ok);
  CHECK(std::memcmp(&a.manifest, &b.manifest, sizeof(Manifest)) == 0);
}

TEST_CASE("unknown fields are ignored, at every level, and a repeated key takes its last value") {
  std::string text = repeaterText();
  text = edited(text, "\"schema\": 1,", "\"schema\": 1, \"colour\": \"blue\", \"extra\": {\"a\": [1, 2, {\"b\": null}]},");
  text = edited(text, "\"mcu\": \"nrf52840\",", "\"mcu\": \"nrf52840\", \"notes\": [\"x\", \"y\"],");
  text = edited(text, "\"format\": \"zip\",", "\"format\": \"zip\", \"mirror\": \"https://example/x\",");
  CHECK(reasonOf(text) == ValidateReason::kNone);
  // `upstream` is optional and unread.
  text = edited(repeaterText(), "\"upstream\": {\n    \"name\": \"MeshCore\",", "\"upstream\": {\n    \"name\": \"Mesh\\\"Core\",");
  CHECK(reasonOf(text) == ValidateReason::kNone);
  text = edited(repeaterText(), "\"tag\": \"v0.3.0\",", "\"tag\": \"v9.9.9\", \"tag\": \"v0.3.0\",");
  const Parsed p = parse(text);
  REQUIRE(p.result.ok);
  CHECK(std::string(p.manifest.tag) == "v0.3.0");
}

TEST_CASE("refused by validate(): the rules of section 2 in manifestref.validate's order and words") {
  const std::string r = repeaterText();
  SUBCASE("an unknown schema") {
    CHECK(reasonOf(edited(r, "\"schema\": 1,", "\"schema\": 2,")) == ValidateReason::kSchema);
    CHECK(reasonOf(edited(r, "\"schema\": 1,", "\"schema\": \"1\",")) == ValidateReason::kSchema);
    CHECK(reasonOf(edited(r, "\"schema\": 1,", "\"schema\": 1.0,")) == ValidateReason::kSchema);
    CHECK(reasonOf(edited(r, "\"schema\": 1,", "")) == ValidateReason::kSchema);
    CHECK(std::string(core::image::describe(ValidateReason::kSchema)) == "schema is not 1");
  }
  SUBCASE("an unknown product") {
    CHECK(reasonOf(edited(r, "twinoak-repeater-firmware", "twinoak-toaster-firmware")) == ValidateReason::kProduct);
    CHECK(reasonOf(edited(r, "\"product\": \"twinoak-repeater-firmware\",", "\"product\": 7,")) == ValidateReason::kProduct);
    CHECK(reasonOf(edited(r, "\"product\": \"twinoak-repeater-firmware\",", "")) == ValidateReason::kProduct);
    CHECK(std::string(core::image::describe(ValidateReason::kProduct)) == "unknown product");
  }
  SUBCASE("the four strings, missing or empty or not strings") {
    CHECK(reasonOf(edited(r, "\"version\": \"0.3.0+abc1234\",", "\"version\": \"\",")) == ValidateReason::kVersionMissing);
    CHECK(reasonOf(edited(r, "\"version\": \"0.3.0+abc1234\",", "\"version\": 3,")) == ValidateReason::kVersionMissing);
    CHECK(reasonOf(edited(r, "\"version\": \"0.3.0+abc1234\",", "")) == ValidateReason::kVersionMissing);
    CHECK(reasonOf(edited(r, "\"tag\": \"v0.3.0\",", "")) == ValidateReason::kTagMissing);
    CHECK(reasonOf(edited(r, "\"commit\": \"abc1234000000000000000000000000000000000\",", "")) == ValidateReason::kCommitMissing);
    CHECK(reasonOf(edited(r, "\"commit\": \"abc1234000000000000000000000000000000000\",", "\"commit\": \"\",")) == ValidateReason::kCommitMissing);
    CHECK(reasonOf(edited(r, "\"built_at\": \"2026-10-12T08:00:00Z\",", "")) == ValidateReason::kBuiltAtMissing);
  }
  SUBCASE("a commit that is not 40 characters") {
    CHECK(reasonOf(edited(r, "\"commit\": \"abc1234000000000000000000000000000000000\",",
                          "\"commit\": \"abc12340000000000000000000000000000000\",")) == ValidateReason::kCommitLength);
    CHECK(reasonOf(edited(r, "\"commit\": \"abc1234000000000000000000000000000000000\",",
                          "\"commit\": \"abc12340000000000000000000000000000000000\",")) == ValidateReason::kCommitLength);
  }
  SUBCASE("the link version") {
    CHECK(reasonOf(edited(r, "\"major\": 1,\n    \"minor\": 0", "\"major\": 1")) == ValidateReason::kLinkMissing);
    CHECK(reasonOf(edited(r, "\"major\": 1,\n    \"minor\": 0", "\"major\": \"1\",\n    \"minor\": 0")) == ValidateReason::kLinkMissing);
    CHECK(reasonOf(edited(r, "\"link\": {\n    \"major\": 1,\n    \"minor\": 0\n  },", "")) == ValidateReason::kLinkMissing);
    CHECK(reasonOf(edited(r, "\"link\": {\n    \"major\": 1,\n    \"minor\": 0\n  },", "\"link\": [1, 0],")) == ValidateReason::kLinkMissing);
  }
  SUBCASE("no images") {
    const std::size_t at = r.find("\"images\": [");
    REQUIRE(at != std::string::npos);
    const std::string none = r.substr(0, at) + "\"images\": []\n}\n";
    CHECK(reasonOf(none) == ValidateReason::kNoImages);
    const std::string object = r.substr(0, at) + "\"images\": {}\n}\n";
    CHECK(reasonOf(object) == ValidateReason::kNoImages);
    const std::string missing = r.substr(0, at - 2) + "\n}\n";  // drop the comma too
    CHECK(reasonOf(missing) == ValidateReason::kNoImages);
    // An image that is not an object is an entry with nothing in it.
    const std::string number = r.substr(0, at) + "\"images\": [7]\n}\n";
    CHECK(reasonOf(number) == ValidateReason::kImageIdMissing);
  }
  SUBCASE("an image's five words") {
    CHECK(reasonOf(edited(r, "\"id\": \"repeater.rak4630-single.868\",", "")) == ValidateReason::kImageIdMissing);
    CHECK(reasonOf(edited(r, "\"role\": \"repeater\",", "\"role\": \"\",")) == ValidateReason::kImageRoleMissing);
    CHECK(reasonOf(edited(r, "\"board\": \"rak4630-single\",", "")) == ValidateReason::kImageBoardMissing);
    CHECK(reasonOf(edited(r, "\"band\": \"868\",", "\"band\": 868,")) == ValidateReason::kImageBandMissing);
    CHECK(reasonOf(edited(r, "\"mcu\": \"nrf52840\",", "")) == ValidateReason::kImageMcuMissing);
  }
  SUBCASE("a duplicate image id") {
    // The whole image object, twice.
    const std::size_t open = r.find("    {\n      \"id\"");
    const std::size_t close = r.find("\n    }\n  ]");
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    const std::string image = r.substr(open, close + 6 - open);
    const std::string twice = r.substr(0, open) + image + ",\n" + image + r.substr(close + 6);
    CHECK(reasonOf(twice) == ValidateReason::kDuplicateImageId);
    CHECK(std::string(core::image::describe(ValidateReason::kDuplicateImageId)) == "duplicate image id");
    // Two images with different ids are fine.
    const std::string other = edited(image, "repeater.rak4630-single.868", "repeater.rak4630-single.433");
    CHECK(reasonOf(r.substr(0, open) + image + ",\n" + other + r.substr(close + 6)) == ValidateReason::kNone);
  }
  SUBCASE("no files") {
    const std::size_t at = r.find("\"files\": [");
    REQUIRE(at != std::string::npos);
    const std::string none = r.substr(0, at) + "\"files\": []\n    }\n  ]\n}\n";
    CHECK(reasonOf(none) == ValidateReason::kNoFiles);
    const std::string missing = r.substr(0, at - 2) + "\n    }\n  ]\n}\n";
    CHECK(reasonOf(missing) == ValidateReason::kNoFiles);
    const std::string number = r.substr(0, at) + "\"files\": [1]\n    }\n  ]\n}\n";
    CHECK(reasonOf(number) == ValidateReason::kUnknownUse);
  }
  SUBCASE("a file's fields") {
    CHECK(reasonOf(edited(r, "\"use\": \"usb-uf2\",", "\"use\": \"floppy\",")) == ValidateReason::kUnknownUse);
    CHECK(reasonOf(edited(r, "\"use\": \"usb-uf2\",", "\"use\": 2,")) == ValidateReason::kUnknownUse);
    CHECK(reasonOf(edited(r, "\"use\": \"usb-uf2\",", "")) == ValidateReason::kUnknownUse);
    CHECK(reasonOf(edited(r, "twinoak-repeater-rak4630-single-868-v0.3.0.uf2", "dist/twinoak.uf2")) == ValidateReason::kBadFileName);
    CHECK(reasonOf(edited(r, "twinoak-repeater-rak4630-single-868-v0.3.0.uf2", "dist\\\\twinoak.uf2")) == ValidateReason::kBadFileName);
    CHECK(reasonOf(edited(r, "\"name\": \"twinoak-repeater-rak4630-single-868-v0.3.0.uf2\",", "")) == ValidateReason::kBadFileName);
    CHECK(reasonOf(edited(r, "\"size\": 8192,", "\"size\": 0,")) == ValidateReason::kBadSize);
    CHECK(reasonOf(edited(r, "\"size\": 8192,", "\"size\": -8192,")) == ValidateReason::kBadSize);
    CHECK(reasonOf(edited(r, "\"size\": 8192,", "\"size\": \"8192\",")) == ValidateReason::kBadSize);
    CHECK(reasonOf(edited(r, "\"size\": 8192,", "\"size\": 8192.0,")) == ValidateReason::kBadSize);
    CHECK(reasonOf(edited(r, "\"size\": 8192,", "\"size\": 4294967296,")) == ValidateReason::kBadSize);
    CHECK(reasonOf(edited(r, "\"size\": 8192,", "")) == ValidateReason::kBadSize);
    CHECK(reasonOf(edited(r, "7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155",
                          "7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b615")) == ValidateReason::kBadSha256);
    CHECK(reasonOf(edited(r, "7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155",
                          "7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b615g")) == ValidateReason::kBadSha256);
    CHECK(reasonOf(edited(r, "\"sha256\": \"7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155\"\n", "\"sha256\": 7\n")) == ValidateReason::kBadSha256);
  }
  SUBCASE("a missing load_address") {
    CHECK(reasonOf(edited(r, ",\n          \"load_address\": \"0x00026000\"", "")) == ValidateReason::kLoadAddressMissing);
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"26000\"")) == ValidateReason::kLoadAddressMissing);
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": 155648")) == ValidateReason::kLoadAddressMissing);
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"0x\"")) == ValidateReason::kLoadAddressNotHex);
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"0x2600g\"")) == ValidateReason::kLoadAddressNotHex);
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"0x100026000\"")) == ValidateReason::kLoadAddressNotHex);
    // Upper-case hex is hex (Python's int(x, 16) takes it).
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"0x26000\"")) == ValidateReason::kNone);
    // A usb-dfu file needs none; one given is kept but not required.
    CHECK(reasonOf(edited(r, "\"size\": 2048,", "\"size\": 2048, \"load_address\": \"0x0\",")) == ValidateReason::kNone);
  }
  SUBCASE("an swd file needs a region and must fit it") {
    CHECK(reasonOf(edited(r, "\"app_region\": {\n        \"start\": \"0x00026000\",\n        \"end\": \"0x000D4000\"\n      },", "")) == ValidateReason::kNeedsAppRegion);
    CHECK(reasonOf(edited(r, "\"end\": \"0x000D4000\"", "\"stop\": \"0x000D4000\"")) == ValidateReason::kNeedsAppRegion);
    CHECK(reasonOf(edited(r, "\"end\": \"0x000D4000\"", "\"end\": \"0xD4000x\"")) == ValidateReason::kAppRegionNotHex);
    // The file outside its region: below the start, or past the end.
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"0x00025000\"")) == ValidateReason::kDoesNotFitRegion);
    CHECK(reasonOf(edited(r, "\"end\": \"0x000D4000\"", "\"end\": \"0x00026FFF\"")) == ValidateReason::kDoesNotFitRegion);
    CHECK(reasonOf(edited(r, "\"end\": \"0x000D4000\"", "\"end\": \"0x00027000\"")) == ValidateReason::kNone);  // exactly fits
    CHECK(std::string(core::image::describe(ValidateReason::kDoesNotFitRegion)) == "the image does not fit its app_region");
    // A 32-bit sum that would wrap: load 0xFFFFF000 + 4096 is checked in 64 bits.
    CHECK(reasonOf(edited(r, "\"load_address\": \"0x00026000\"", "\"load_address\": \"0xFFFFF000\"")) == ValidateReason::kDoesNotFitRegion);
  }
  SUBCASE("a region outside the board profile validates (section 2 knows no boards) and the profile refuses it") {
    // That is check 6, not check 2: test_verdict.cpp shows the refusal there. Here: validate()
    // does not pretend to know the board.
    CHECK(reasonOf(edited(r, "\"start\": \"0x00026000\"", "\"start\": \"0x00020000\"")) == ValidateReason::kNone);
    CHECK_FALSE(core::flasher::kRak4630Single.accepts(core::nrf52::FlashRegion{0x00020000, 0x000D4000}));
    CHECK(core::flasher::kRak4630Single.accepts(core::nrf52::FlashRegion{0x00026000, 0x000D4000}));
  }
}

TEST_CASE("refused by the parser: not JSON, not one object, and the bounds, each with its own reason") {
  const std::string r = repeaterText();
  CHECK(parseErrorOf("") == ParseError::kNotJson);
  CHECK(parseErrorOf("{") == ParseError::kNotJson);
  CHECK(parseErrorOf("{\"a\": }") == ParseError::kNotJson);
  CHECK(parseErrorOf("{\"a\" 1}") == ParseError::kNotJson);
  CHECK(parseErrorOf(r + " x") == ParseError::kNotJson);
  CHECK(parseErrorOf("[]") == ParseError::kNotAnObject);
  // A bare number is refused by the tokenizer itself (strict jsmn wants a primitive followed by
  // something), a bare string by the object check: refused either way.
  CHECK(parseErrorOf("7") == ParseError::kNotJson);
  CHECK(parseErrorOf("\"text\"") == ParseError::kNotAnObject);
  CHECK(parseErrorOf(r + " {}") == ParseError::kNotAnObject);

  SUBCASE("more images than the parser's bound") {
    REQUIRE(fixtures::kManyImages == core::image::kMaxImages + 1);
    const std::string many = fixtures::asText(fixtures::readGenerated("repeater-many-images.json"));
    CHECK(parseErrorOf(many) == ParseError::kTooManyImages);
    CHECK(std::string(core::image::describe(ParseError::kTooManyImages)) == "more images than this firmware holds");
    // Exactly the bound is fine: the last image dropped.
    const std::size_t last = many.rfind("    {\n      \"id\"");
    const std::size_t comma = many.rfind(",", last);
    REQUIRE(last != std::string::npos);
    const std::string atBound = many.substr(0, comma) + "\n  ]\n}\n";
    const Parsed p = parse(atBound);
    REQUIRE_MESSAGE(p.result.ok, core::image::describe(p.result.error));
    CHECK(p.manifest.image_count == core::image::kMaxImages);
    CHECK(validate(p.manifest).ok);
  }
  SUBCASE("more files than the bound") {
    const std::string file = "        {\n          \"use\": \"debug\",\n          \"name\": \"x.elf\",\n          \"format\": \"elf\",\n          \"size\": 1,\n          \"sha256\": \"7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155\"\n        }";
    std::string files;
    for (std::size_t i = 0; i < core::image::kMaxFilesPerImage + 1 - 3; ++i) files += ",\n" + file;
    const std::string text = edited(r, "\"sha256\": \"7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155\"\n        }",
                                    "\"sha256\": \"7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155\"\n        }" + files);
    CHECK(parseErrorOf(text) == ParseError::kTooManyFiles);
    // One fewer fits.
    const std::string fits = edited(r, "\"sha256\": \"7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155\"\n        }",
                                    "\"sha256\": \"7eafa43413daf1447d9e3d044c44528e194d72505195bfa83bdec795481b6155\"\n        }" + files.substr(0, files.size() - file.size() - 2));
    const Parsed p = parse(fits);
    REQUIRE(p.result.ok);
    CHECK(p.manifest.images[0].file_count == core::image::kMaxFilesPerImage);
  }
  SUBCASE("more tokens than the parser holds") {
    std::string big = "\"padding\": [";
    for (std::size_t i = 0; i < core::image::kMaxJsonTokens; ++i) big += (i ? ", 1" : "1");
    big += "], ";
    CHECK(parseErrorOf(edited(r, "\"schema\": 1,", big + "\"schema\": 1,")) == ParseError::kTooManyTokens);
  }
  SUBCASE("a string longer than its field, naming the field") {
    const Parsed v = parse(edited(r, "0.3.0+abc1234", std::string(core::image::kVersionSize, 'v')));
    CHECK_FALSE(v.result.ok);
    CHECK(v.result.error == ParseError::kStringTooLong);
    CHECK(std::string(v.result.field) == "version");
    const Parsed n = parse(edited(r, "twinoak-repeater-rak4630-single-868-v0.3.0.uf2", std::string(core::image::kMaxFileName, 'n')));
    CHECK(n.result.error == ParseError::kStringTooLong);
    CHECK(std::string(n.result.field) == "file.name");
    const Parsed b = parse(edited(r, "\"board\": \"rak4630-single\"", "\"board\": \"" + std::string(core::image::kBoardSize, 'b') + "\""));
    CHECK(b.result.error == ParseError::kStringTooLong);
    CHECK(std::string(b.result.field) == "image.board");
    // One shorter fits.
    CHECK(parse(edited(r, "0.3.0+abc1234", std::string(core::image::kVersionSize - 1, 'v'))).result.ok);
  }
  SUBCASE("a kept string with an escape") {
    const Parsed p = parse(edited(r, "0.3.0+abc1234", "0.3.0+abc123\\u0034"));
    CHECK_FALSE(p.result.ok);
    CHECK(p.result.error == ParseError::kEscapedString);
    CHECK(std::string(p.result.field) == "version");
    // In a string that is ignored, an escape is nothing.
    CHECK(parse(edited(r, "repeater-v1.17.1", "repeater\\n-v1.17.1")).result.ok);
  }
  SUBCASE("a refused parse leaves nothing half-parsed behind") {
    const Parsed p = parse(edited(r, "0.3.0+abc1234", std::string(core::image::kVersionSize, 'v')));
    REQUIRE_FALSE(p.result.ok);
    const Manifest blank{};
    CHECK(std::memcmp(&p.manifest, &blank, sizeof(Manifest)) == 0);
  }
}

TEST_CASE("the words: products and uses by name") {
  CHECK(core::image::productFromName("twinoak-walter-firmware", 23) == Product::kWalter);
  CHECK(core::image::productFromName("twinoak-walter-firmware", 22) == Product::kUnknown);
  CHECK(core::image::productFromName("twinoak-walter-firmwarex", 24) == Product::kUnknown);
  CHECK(std::string(core::image::name(Product::kRepeater)) == "twinoak-repeater-firmware");
  const char* uses[] = {"swd", "usb-dfu", "usb-uf2", "ota", "factory", "debug"};
  for (const char* u : uses) {
    const Use use = core::image::useFromName(u, std::strlen(u));
    CHECK(use != Use::kUnknown);
    CHECK(std::string(core::image::name(use)) == u);
  }
  CHECK(core::image::useFromName("SWD", 3) == Use::kUnknown);
  CHECK(core::image::useFromName("", 0) == Use::kUnknown);
}
