// The release manifest (task p2-01): spec/FIRMWARE-MANIFEST.md §2 as fixed-size structures, a
// parser for schema 1 into them, and validate() - the rules a consumer applies after the
// signature has passed, as spec/tools/manifestref.py's validate() applies them, in its order and
// with its words (host_test/core/image/test_manifest.cpp holds both fixture manifests and the
// refusals against it).
//
// No heap: the bounds below are the structures' sizes, and a manifest that exceeds one is
// refused with a reason that names it rather than truncated. Unknown fields are ignored (§2);
// an unknown `schema` or `product` is refused, by validate().
//
// The parser is a tokenizer (third_party/jsmn, vendored, no allocation) and a walk over its
// tokens: the token array is a member of ManifestParser, 12.5 KB, so a parser lives in static
// storage and never on a task's stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE is 3584).
//
// The three bounds below are therefore static DRAM at boot, which is this board's scarce resource
// (docs/ARCHITECTURE.md §14): a token costs 20 bytes, a file 120 and an image 768, so a bound set
// for a release nobody writes is paid for by every image. Task p17-01 sized them for the releases
// there can be.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/image/crypto/sha256.h"
#include "core/image/descriptor.h"

namespace core {
namespace image {

// -- the bounds, each with its reasoning -------------------------------------------------------

// Images in one manifest. §2: an image is one role on one board in one band, and the roles §2
// names for the radio are three (repeater, bridge, scanner) in two bands (868, 433) - six. The
// Walter's own manifest has one image (manager.walter.lte-m), and a dual adapter's second radio
// is another position for the same six, not a seventh image. Today's biggest release is two
// images of four files. A larger release is a release this firmware cannot read, said so at
// check 2 with the bound it broke.
constexpr std::size_t kMaxImages = 6;

// Files per image. §2 lists seven `use` values and a file is "one way of delivering that image",
// so one per use is the most a well-formed image has - but no board has all seven. `swd`,
// `usb-dfu`, `usb-uf2` and `provision` (the board's base, §4a) are the radio's and `ota` and
// `factory` are the Walter's, so with `debug` beside either the most one image can have is five:
// swd, usb-dfu, usb-uf2, debug, provision on the radio; ota, factory, debug on the Walter. The
// repeater's release tooling writes four an image, the Walter's two.
constexpr std::size_t kMaxFilesPerImage = 5;

// JSON tokens the parser can hold: a manifest filled to the two bounds above, `upstream` and all,
// is 569 tokens - the count manifest.cpp makes and asserts. The 71 over it are room for fields a
// later minor version of the schema adds, which §2 says are ignored: ignored, but tokenized and
// still counted. The repeater's release today is 167 tokens and the Walter's own 64.
constexpr std::size_t kMaxJsonTokens = 640;

// A file name: `<product>-<board>-<band>-v<X.Y.Z>[-factory].<ext>` is under 60 characters for
// every name §2 and the fixtures show. 64 with the terminator. §2: "no path".
constexpr std::size_t kMaxFileName = 64;

// Strings that are compared with a descriptor's fields have the descriptor's sizes (§4: version
// 32, board 24, band 8, terminator included) - a value that does not fit there cannot name any
// image. `role` and `mcu` are single words; `id` is `<role>.<board>.<band>`; `tag` is vX.Y.Z;
// `built_at` is twenty characters of UTC; `format` is a file extension.
constexpr std::size_t kMaxRoleSize = 16;
constexpr std::size_t kMaxMcuSize = 16;
constexpr std::size_t kMaxIdSize = kMaxRoleSize + kBoardSize + kBandSize;  // 48
constexpr std::size_t kMaxTagSize = 16;
constexpr std::size_t kMaxBuiltAtSize = 24;
constexpr std::size_t kMaxFormatSize = 8;
constexpr std::size_t kCommitSize = 40;  // §2: 40 hex characters

// -- the words ---------------------------------------------------------------------------------

enum class Product : std::uint8_t { kUnknown, kRepeater, kWalter };
// §2's `use` values. kProvision is the board's base (§4a): not an image at all, which is why
// nothing that takes an image takes one (core/image/verdict.h's AcceptedBase).
enum class Use : std::uint8_t {
  kUnknown,
  kSwd,
  kUsbDfu,
  kUsbUf2,
  kOta,
  kFactory,
  kDebug,
  kProvision,
};

const char* name(Product product);
const char* name(Use use);
Product productFromName(const char* text, std::size_t size);
Use useFromName(const char* text, std::size_t size);

// A value that is a hex number in the manifest (`0x%08X`): absent, present but not a hex string,
// or parsed.
enum class HexField : std::uint8_t { kAbsent, kNotHex, kOk };

// -- the structures (§2's three tables) --------------------------------------------------------

struct FileEntry {
  Use use = Use::kUnknown;                 // kUnknown: missing, not a string, or not one of §2's
  bool has_name = false;                   // a string (its content is validate()'s business)
  char name[kMaxFileName] = {};
  char format[kMaxFormatSize] = {};
  bool has_size = false;                   // an integer that fits 32 bits
  std::uint32_t size = 0;
  bool has_sha256 = false;                 // 64 lowercase-or-uppercase hex characters
  std::uint8_t sha256[kSha256Size] = {};
  HexField load_address_state = HexField::kAbsent;
  std::uint32_t load_address = 0;
};

struct ImageEntry {
  char id[kMaxIdSize] = {};  // empty: missing or not a string (validate says "missing")
  char role[kMaxRoleSize] = {};
  char board[kBoardSize] = {};
  char band[kBandSize] = {};
  char mcu[kMaxMcuSize] = {};
  HexField app_region_state = HexField::kAbsent;  // kAbsent: no object, or no start/end strings
  std::uint32_t app_region_start = 0;
  std::uint32_t app_region_end = 0;
  // `requires`, optional (§2): what must already be there. Not validate()'s (manifestref.validate
  // does not look at it); chooseImage() reads it, and refuses an image whose requirement it
  // cannot evaluate.
  bool requires_present = false;
  bool requires_malformed = false;  // not an object, or a member of the wrong type
  bool has_requires_walter = false;
  char requires_walter[kVersionSize] = {};
  bool has_requires_link_major = false;
  std::uint32_t requires_link_major = 0;
  bool has_files = false;  // a list (possibly empty: validate says "image has no files")
  std::uint8_t file_count = 0;
  FileEntry files[kMaxFilesPerImage];
};

struct Manifest {
  bool has_schema = false;  // an integer
  std::uint32_t schema = 0;
  Product product = Product::kUnknown;
  char version[kVersionSize] = {};
  char tag[kMaxTagSize] = {};
  bool has_commit = false;  // a string; its length is what validate() checks
  std::size_t commit_length = 0;
  char commit[kCommitSize + 1] = {};  // the first 40 characters, when it is a string
  char built_at[kMaxBuiltAtSize] = {};
  bool has_link = false;  // an object with integer major and minor
  std::uint32_t link_major = 0;
  std::uint32_t link_minor = 0;
  bool has_images = false;  // a list (possibly empty: validate says "no images")
  std::uint8_t image_count = 0;
  ImageEntry images[kMaxImages];
};

// -- the parser --------------------------------------------------------------------------------

enum class ParseError : std::uint8_t {
  kNone,
  kNotJson,         // not one well-formed JSON text (the tokenizer refused it)
  kTooManyTokens,   // more than kMaxJsonTokens: a manifest this firmware cannot hold
  kNotAnObject,     // the text is JSON but not one object
  kTooManyImages,   // more than kMaxImages
  kTooManyFiles,    // more than kMaxFilesPerImage in one image
  kStringTooLong,   // a string the firmware keeps exceeds its field (`field` says which)
  kEscapedString,   // a string the firmware keeps contains a backslash (jsmn does not unescape)
};

const char* describe(ParseError error);

struct ParseResult {
  bool ok = false;
  ParseError error = ParseError::kNone;
  const char* field = "";  // for kStringTooLong and kEscapedString: the field's name
};

constexpr std::size_t kJsonTokenBytes = 20;  // sizeof(jsmntok_t) with parent links; asserted in manifest.cpp

class ManifestParser {
 public:
  // `json` need not be terminated; `size` bytes are read. On failure `out` is left as a
  // default-constructed Manifest (nothing half-parsed escapes).
  ParseResult parse(const char* json, std::size_t size, Manifest* out);

  // How many of kMaxJsonTokens the last parse took; 0 when the tokenizer refused the text. What
  // makes the bound above checkable against a real release rather than argued about (task p17-01).
  std::uint16_t tokenCount() const { return token_count_; }

 private:
  alignas(4) unsigned char tokens_[kMaxJsonTokens * kJsonTokenBytes];
  std::uint16_t token_count_ = 0;
};

// -- validate(): manifestref.validate, rule for rule --------------------------------------------

enum class ValidateReason : std::uint8_t {
  kNone,
  kSchema,             // "schema is not 1"
  kProduct,            // "unknown product"
  kVersionMissing,     // "version missing"
  kTagMissing,
  kCommitMissing,
  kBuiltAtMissing,
  kCommitLength,       // "commit is not 40 hex characters"
  kLinkMissing,        // "link version missing"
  kNoImages,           // "no images"
  kImageIdMissing,     // "image.id missing" ... one per field
  kImageRoleMissing,
  kImageBoardMissing,
  kImageBandMissing,
  kImageMcuMissing,
  kDuplicateImageId,   // "duplicate image id"
  kNoFiles,            // "image has no files"
  kUnknownUse,         // "unknown use"
  kBadFileName,        // "bad file name"
  kBadSize,            // "bad size"
  kBadSha256,          // "bad sha256"
  kLoadAddressMissing, // "load_address missing"
  kLoadAddressNotHex,  // ours: present, "0x", but not 1 to 8 hex digits
  kNeedsAppRegion,     // "an swd image needs app_region"
  kAppRegionNotHex,    // ours: start or end is not 1 to 8 hex digits
  kDoesNotFitRegion,   // "the image does not fit its app_region"
  // §2's rules for a `provision` file (the base, §4a), each one `manifestref.validate`'s.
  kHexIsForProvision,      // "format hex is for a provision file only"
  kProvisionNotHex,        // "a provision file is Intel HEX (format hex)"
  kProvisionLoadAddress,   // "a provision file has no load_address: its records say where"
  kProvisionNotNrf52840,   // "a provision file is for an nRF52840 board"
  kProvisionNeedsRegion,   // "a provision file needs its image's app_region"
  kTwoProvisionFiles,      // "more than one provision file in image <id>"
};

const char* describe(ValidateReason reason);

struct ValidateResult {
  bool ok = false;
  ValidateReason reason = ValidateReason::kNone;
  std::uint8_t image = 0;  // which image, for the per-image reasons
  std::uint8_t file = 0;   // which file of it, for the per-file reasons
};

ValidateResult validate(const Manifest& manifest);

}  // namespace image
}  // namespace core
