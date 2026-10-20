#include "core/image/manifest.h"

#include <cstring>

// jsmn v1.1.0 (third_party/jsmn/README.md): a tokenizer into an array we own. Static (no
// symbols leave this file), strict (standard JSON only), with parent links (a token knows its
// parent, so a closing bracket costs no search and the walk below needs no recursion).
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include <jsmn/jsmn.h>

namespace core {
namespace image {

static_assert(sizeof(jsmntok_t) <= kJsonTokenBytes, "kJsonTokenBytes is smaller than a jsmn token");
static_assert(alignof(jsmntok_t) <= 4, "a jsmn token needs more alignment than the token store gives");

// Where kMaxJsonTokens comes from: a manifest filled to the bounds. Top level: the object, seven
// scalar fields (two tokens each), `link` (1 + 1 + 4), `upstream` (1 + 1 + 6), `images` (2) = 29.
// Each image: the object, five strings (10), `app_region` (1 + 1 + 4), `requires` (1 + 1 + 4),
// `files` (2) = 25, plus per file the object and six fields (13) * kMaxFilesPerImage = 91, so
// 116 an image, 928 for kMaxImages, 957 in all (task p5-01 made kMaxFilesPerImage seven: §2's
// seventh `use` is the base). Fields §2 does not know are ignored but still tokenized, so the
// bound has room over that.
static_assert(29 + kMaxImages * (25 + kMaxFilesPerImage * 13) <= kMaxJsonTokens,
              "kMaxJsonTokens cannot hold a manifest filled to kMaxImages and kMaxFilesPerImage");

namespace {

// -- words ------------------------------------------------------------------------------------

struct Word {
  const char* text;
  std::size_t size;
};

bool sameText(const char* a, std::size_t a_size, const char* b) {
  return std::strlen(b) == a_size && std::memcmp(a, b, a_size) == 0;
}

constexpr const char* kProductNames[] = {"?", "twinoak-repeater-firmware", "twinoak-walter-firmware"};
constexpr const char* kUseNames[] = {"?",   "swd",     "usb-dfu", "usb-uf2",
                                     "ota", "factory", "debug",   "provision"};

// -- the token walk ---------------------------------------------------------------------------

class Doc {
 public:
  Doc(const char* js, const jsmntok_t* tok, int count) : js_(js), tok_(tok), count_(count) {}

  int count() const { return count_; }
  const jsmntok_t& at(int i) const { return tok_[i]; }
  bool isObject(int i) const { return tok_[i].type == JSMN_OBJECT; }
  bool isArray(int i) const { return tok_[i].type == JSMN_ARRAY; }
  bool isString(int i) const { return tok_[i].type == JSMN_STRING; }
  const char* text(int i) const { return js_ + tok_[i].start; }
  std::size_t size(int i) const { return static_cast<std::size_t>(tok_[i].end - tok_[i].start); }

  // The index after token i's subtree. Tokens are in document order and a nested token starts
  // inside its parent's span, so the subtree ends at the first token that starts at or after
  // the parent's end. A key string's value starts after the key's closing quote, so for a key
  // this is key + 1, and the value's own subtree is skipped by the caller.
  int skip(int i) const {
    int j = i + 1;
    while (j < count_ && tok_[j].start < tok_[i].end) ++j;
    return j;
  }

  bool keyIs(int key, const char* name) const { return isString(key) && sameText(text(key), size(key), name); }

  // A JSON integer in [0, 2^32): digits only, no sign, no fraction or exponent, no leading zero
  // (JSON has none), at most ten digits and inside a u32.
  bool uint32Of(int i, std::uint32_t* out) const {
    if (tok_[i].type != JSMN_PRIMITIVE) return false;
    const char* p = text(i);
    const std::size_t n = size(i);
    if (n == 0 || n > 10) return false;
    if (n > 1 && p[0] == '0') return false;
    std::uint64_t value = 0;
    for (std::size_t k = 0; k < n; ++k) {
      if (p[k] < '0' || p[k] > '9') return false;
      value = value * 10 + static_cast<std::uint64_t>(p[k] - '0');
    }
    if (value > 0xFFFFFFFFull) return false;
    *out = static_cast<std::uint32_t>(value);
    return true;
  }

  // A string "0x%08X": a string token beginning with 0x. kNotHex when it does and what follows
  // is not 1 to 8 hex digits (either case: Python's int(x, 16) takes both).
  HexField hexOf(int i, std::uint32_t* out) const {
    if (!isString(i)) return HexField::kAbsent;
    const char* p = text(i);
    const std::size_t n = size(i);
    if (n < 2 || p[0] != '0' || p[1] != 'x') return HexField::kAbsent;
    if (n < 3 || n > 10) return HexField::kNotHex;
    std::uint32_t value = 0;
    for (std::size_t k = 2; k < n; ++k) {
      const char c = p[k];
      std::uint32_t d;
      if (c >= '0' && c <= '9') {
        d = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return HexField::kNotHex;
      }
      value = (value << 4) | d;
    }
    *out = value;
    return HexField::kOk;
  }

  // 64 hex characters into 32 bytes.
  bool sha256Of(int i, std::uint8_t out[kSha256Size]) const {
    if (!isString(i) || size(i) != 2 * kSha256Size) return false;
    const char* p = text(i);
    for (std::size_t k = 0; k < kSha256Size; ++k) {
      std::uint8_t value = 0;
      for (int half = 0; half < 2; ++half) {
        const char c = p[2 * k + half];
        std::uint8_t d;
        if (c >= '0' && c <= '9') {
          d = static_cast<std::uint8_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          d = static_cast<std::uint8_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
          d = static_cast<std::uint8_t>(c - 'A' + 10);
        } else {
          return false;
        }
        value = static_cast<std::uint8_t>((value << 4) | d);
      }
      out[k] = value;
    }
    return true;
  }

 private:
  const char* js_;
  const jsmntok_t* tok_;
  int count_;
};

// A string the firmware keeps, copied into a field of `size` bytes. Not a string: left empty and
// false (the caller's flag stays off; validate() then says "missing" as the reference does).
// Too long, or holding a backslash: a ParseError, which stops the parse.
struct Copied {
  bool is_string = false;
  ParseError error = ParseError::kNone;
};

Copied copyString(const Doc& doc, int i, char* out, std::size_t size, const char* field, ParseResult* result) {
  Copied c;
  if (!doc.isString(i)) return c;
  c.is_string = true;
  const char* p = doc.text(i);
  const std::size_t n = doc.size(i);
  for (std::size_t k = 0; k < n; ++k) {
    if (p[k] == '\\') {
      c.error = ParseError::kEscapedString;
      result->error = c.error;
      result->field = field;
      return c;
    }
  }
  if (n + 1 > size) {
    c.error = ParseError::kStringTooLong;
    result->error = c.error;
    result->field = field;
    return c;
  }
  std::memcpy(out, p, n);
  out[n] = 0;
  return c;
}

// The same for a string that is only *compared* and never kept beyond this parse (`product`,
// `use`): too long is simply unknown.
bool parseFile(const Doc& doc, int obj, FileEntry* file, ParseResult* result) {
  const jsmntok_t& t = doc.at(obj);
  int k = obj + 1;
  for (int n = 0; n < t.size; ++n) {
    const int key = k;
    const int value = key + 1;
    if (!doc.isString(key) || doc.at(key).size != 1 || value >= doc.count()) {
      result->error = ParseError::kNotJson;
      return false;
    }
    k = doc.skip(value);
    if (doc.keyIs(key, "use")) {
      file->use = doc.isString(value) ? useFromName(doc.text(value), doc.size(value)) : Use::kUnknown;
    } else if (doc.keyIs(key, "name")) {
      // Not copyString: a backslash in a name is "bad file name" (§2: no path), validate()'s to
      // say, and a name too long for the field is refused here with the field's name.
      if (doc.isString(value)) {
        file->has_name = true;
        const std::size_t n = doc.size(value);
        if (n + 1 > kMaxFileName) {
          result->error = ParseError::kStringTooLong;
          result->field = "file.name";
          return false;
        }
        std::memcpy(file->name, doc.text(value), n);
        file->name[n] = 0;
      }
    } else if (doc.keyIs(key, "format")) {
      const Copied c = copyString(doc, value, file->format, kMaxFormatSize, "file.format", result);
      if (c.error != ParseError::kNone) return false;
    } else if (doc.keyIs(key, "size")) {
      file->has_size = doc.uint32Of(value, &file->size);
    } else if (doc.keyIs(key, "sha256")) {
      file->has_sha256 = doc.sha256Of(value, file->sha256);
    } else if (doc.keyIs(key, "load_address")) {
      file->load_address_state = doc.hexOf(value, &file->load_address);
    }
    // Anything else: ignored (§2).
  }
  return true;
}

bool parseRequires(const Doc& doc, int obj, ImageEntry* image, ParseResult* result) {
  image->requires_present = true;
  if (!doc.isObject(obj)) {
    image->requires_malformed = true;
    return true;
  }
  const jsmntok_t& t = doc.at(obj);
  int k = obj + 1;
  for (int n = 0; n < t.size; ++n) {
    const int key = k;
    const int value = key + 1;
    if (!doc.isString(key) || doc.at(key).size != 1 || value >= doc.count()) {
      result->error = ParseError::kNotJson;
      return false;
    }
    k = doc.skip(value);
    if (doc.keyIs(key, "walter")) {
      const Copied c = copyString(doc, value, image->requires_walter, kVersionSize, "requires.walter", result);
      if (c.error != ParseError::kNone) return false;
      if (c.is_string) {
        image->has_requires_walter = true;
      } else {
        image->requires_malformed = true;
      }
    } else if (doc.keyIs(key, "link_major")) {
      if (doc.uint32Of(value, &image->requires_link_major)) {
        image->has_requires_link_major = true;
      } else {
        image->requires_malformed = true;
      }
    }
  }
  return true;
}

bool parseRegion(const Doc& doc, int obj, ImageEntry* image, ParseResult* result) {
  if (!doc.isObject(obj)) return true;  // not an object: as if absent
  const jsmntok_t& t = doc.at(obj);
  HexField start = HexField::kAbsent;
  HexField end = HexField::kAbsent;
  int k = obj + 1;
  for (int n = 0; n < t.size; ++n) {
    const int key = k;
    const int value = key + 1;
    if (!doc.isString(key) || doc.at(key).size != 1 || value >= doc.count()) {
      result->error = ParseError::kNotJson;
      return false;
    }
    k = doc.skip(value);
    if (doc.keyIs(key, "start")) {
      start = doc.hexOf(value, &image->app_region_start);
    } else if (doc.keyIs(key, "end")) {
      end = doc.hexOf(value, &image->app_region_end);
    }
  }
  // manifestref: `"start" in region and "end" in region` is the presence check; the hex parse
  // comes after and, there, raises. Here: both present as strings -> kOk or kNotHex.
  if (start == HexField::kAbsent || end == HexField::kAbsent) {
    image->app_region_state = HexField::kAbsent;
  } else if (start == HexField::kOk && end == HexField::kOk) {
    image->app_region_state = HexField::kOk;
  } else {
    image->app_region_state = HexField::kNotHex;
  }
  return true;
}

bool parseImage(const Doc& doc, int obj, ImageEntry* image, ParseResult* result) {
  const jsmntok_t& t = doc.at(obj);
  int k = obj + 1;
  for (int n = 0; n < t.size; ++n) {
    const int key = k;
    const int value = key + 1;
    if (!doc.isString(key) || doc.at(key).size != 1 || value >= doc.count()) {
      result->error = ParseError::kNotJson;
      return false;
    }
    k = doc.skip(value);
    Copied c;
    if (doc.keyIs(key, "id")) {
      c = copyString(doc, value, image->id, kMaxIdSize, "image.id", result);
    } else if (doc.keyIs(key, "role")) {
      c = copyString(doc, value, image->role, kMaxRoleSize, "image.role", result);
    } else if (doc.keyIs(key, "board")) {
      c = copyString(doc, value, image->board, kBoardSize, "image.board", result);
    } else if (doc.keyIs(key, "band")) {
      c = copyString(doc, value, image->band, kBandSize, "image.band", result);
    } else if (doc.keyIs(key, "mcu")) {
      c = copyString(doc, value, image->mcu, kMaxMcuSize, "image.mcu", result);
    } else if (doc.keyIs(key, "app_region")) {
      if (!parseRegion(doc, value, image, result)) return false;
    } else if (doc.keyIs(key, "requires")) {
      if (!parseRequires(doc, value, image, result)) return false;
    } else if (doc.keyIs(key, "files")) {
      if (doc.isArray(value)) {
        image->has_files = true;
        const jsmntok_t& files = doc.at(value);
        int f = value + 1;
        for (int m = 0; m < files.size; ++m) {
          if (image->file_count >= kMaxFilesPerImage) {
            result->error = ParseError::kTooManyFiles;
            return false;
          }
          FileEntry& entry = image->files[image->file_count];
          entry = FileEntry{};
          if (doc.isObject(f)) {
            if (!parseFile(doc, f, &entry, result)) return false;
          }
          // Not an object: an entry with nothing in it, which validate() refuses ("unknown use").
          image->file_count++;
          f = doc.skip(f);
        }
      }
    }
    if (c.error != ParseError::kNone) return false;
  }
  return true;
}

bool parseLink(const Doc& doc, int obj, Manifest* out, ParseResult* result) {
  if (!doc.isObject(obj)) return true;
  const jsmntok_t& t = doc.at(obj);
  bool major = false;
  bool minor = false;
  int k = obj + 1;
  for (int n = 0; n < t.size; ++n) {
    const int key = k;
    const int value = key + 1;
    if (!doc.isString(key) || doc.at(key).size != 1 || value >= doc.count()) {
      result->error = ParseError::kNotJson;
      return false;
    }
    k = doc.skip(value);
    if (doc.keyIs(key, "major")) {
      major = doc.uint32Of(value, &out->link_major);
    } else if (doc.keyIs(key, "minor")) {
      minor = doc.uint32Of(value, &out->link_minor);
    }
  }
  out->has_link = major && minor;
  return true;
}

bool parseRoot(const Doc& doc, Manifest* out, ParseResult* result) {
  const jsmntok_t& root = doc.at(0);
  int k = 1;
  for (int n = 0; n < root.size; ++n) {
    const int key = k;
    const int value = key + 1;
    if (!doc.isString(key) || doc.at(key).size != 1 || value >= doc.count()) {
      result->error = ParseError::kNotJson;
      return false;
    }
    k = doc.skip(value);
    Copied c;
    if (doc.keyIs(key, "schema")) {
      out->has_schema = doc.uint32Of(value, &out->schema);
    } else if (doc.keyIs(key, "product")) {
      out->product = doc.isString(value) ? productFromName(doc.text(value), doc.size(value)) : Product::kUnknown;
    } else if (doc.keyIs(key, "version")) {
      c = copyString(doc, value, out->version, kVersionSize, "version", result);
    } else if (doc.keyIs(key, "tag")) {
      c = copyString(doc, value, out->tag, kMaxTagSize, "tag", result);
    } else if (doc.keyIs(key, "commit")) {
      if (doc.isString(value)) {
        out->has_commit = true;
        out->commit_length = doc.size(value);
        const std::size_t keep = out->commit_length < kCommitSize ? out->commit_length : kCommitSize;
        std::memcpy(out->commit, doc.text(value), keep);
        out->commit[keep] = 0;
      }
    } else if (doc.keyIs(key, "built_at")) {
      c = copyString(doc, value, out->built_at, kMaxBuiltAtSize, "built_at", result);
    } else if (doc.keyIs(key, "link")) {
      if (!parseLink(doc, value, out, result)) return false;
    } else if (doc.keyIs(key, "images")) {
      if (doc.isArray(value)) {
        out->has_images = true;
        const jsmntok_t& images = doc.at(value);
        int im = value + 1;
        for (int m = 0; m < images.size; ++m) {
          if (out->image_count >= kMaxImages) {
            result->error = ParseError::kTooManyImages;
            return false;
          }
          ImageEntry& entry = out->images[out->image_count];
          entry = ImageEntry{};
          if (doc.isObject(im)) {
            if (!parseImage(doc, im, &entry, result)) return false;
          }
          // Not an object: an empty entry, which validate() refuses ("image.id missing").
          out->image_count++;
          im = doc.skip(im);
        }
      }
    }
    // `upstream` and anything §2 does not name: ignored.
    if (c.error != ParseError::kNone) return false;
  }
  return true;
}

}  // namespace

const char* name(Product product) { return kProductNames[static_cast<std::uint8_t>(product)]; }

const char* name(Use use) { return kUseNames[static_cast<std::uint8_t>(use)]; }

Product productFromName(const char* text, std::size_t size) {
  for (std::uint8_t i = 1; i < sizeof kProductNames / sizeof kProductNames[0]; ++i) {
    if (sameText(text, size, kProductNames[i])) return static_cast<Product>(i);
  }
  return Product::kUnknown;
}

Use useFromName(const char* text, std::size_t size) {
  for (std::uint8_t i = 1; i < sizeof kUseNames / sizeof kUseNames[0]; ++i) {
    if (sameText(text, size, kUseNames[i])) return static_cast<Use>(i);
  }
  return Use::kUnknown;
}

const char* describe(ParseError error) {
  switch (error) {
    case ParseError::kNone: return "parsed";
    case ParseError::kNotJson: return "not well-formed JSON";
    case ParseError::kTooManyTokens: return "too large: more JSON tokens than this firmware holds";
    case ParseError::kNotAnObject: return "not one JSON object";
    case ParseError::kTooManyImages: return "more images than this firmware holds";
    case ParseError::kTooManyFiles: return "more files in one image than this firmware holds";
    case ParseError::kStringTooLong: return "a string is longer than its field";
    case ParseError::kEscapedString: return "a string holds a backslash escape";
  }
  return "?";
}

ParseResult ManifestParser::parse(const char* json, std::size_t size, Manifest* out) {
  ParseResult result;
  if (out == nullptr) return result;
  *out = Manifest{};
  if (json == nullptr) {
    result.error = ParseError::kNotJson;
    return result;
  }
  jsmntok_t* tokens = reinterpret_cast<jsmntok_t*>(tokens_);
  jsmn_parser parser;
  jsmn_init(&parser);
  const int count = jsmn_parse(&parser, json, size, tokens, kMaxJsonTokens);
  if (count == JSMN_ERROR_NOMEM) {
    result.error = ParseError::kTooManyTokens;
    return result;
  }
  if (count < 1) {
    result.error = ParseError::kNotJson;
    return result;
  }
  const Doc doc(json, tokens, count);
  if (!doc.isObject(0) || doc.skip(0) != count) {
    result.error = ParseError::kNotAnObject;
    return result;
  }
  if (!parseRoot(doc, out, &result)) {
    *out = Manifest{};
    return result;
  }
  result.ok = true;
  return result;
}

const char* describe(ValidateReason reason) {
  switch (reason) {
    case ValidateReason::kNone: return "valid";
    case ValidateReason::kSchema: return "schema is not 1";
    case ValidateReason::kProduct: return "unknown product";
    case ValidateReason::kVersionMissing: return "version missing";
    case ValidateReason::kTagMissing: return "tag missing";
    case ValidateReason::kCommitMissing: return "commit missing";
    case ValidateReason::kBuiltAtMissing: return "built_at missing";
    case ValidateReason::kCommitLength: return "commit is not 40 hex characters";
    case ValidateReason::kLinkMissing: return "link version missing";
    case ValidateReason::kNoImages: return "no images";
    case ValidateReason::kImageIdMissing: return "image.id missing";
    case ValidateReason::kImageRoleMissing: return "image.role missing";
    case ValidateReason::kImageBoardMissing: return "image.board missing";
    case ValidateReason::kImageBandMissing: return "image.band missing";
    case ValidateReason::kImageMcuMissing: return "image.mcu missing";
    case ValidateReason::kDuplicateImageId: return "duplicate image id";
    case ValidateReason::kNoFiles: return "image has no files";
    case ValidateReason::kUnknownUse: return "unknown use";
    case ValidateReason::kBadFileName: return "bad file name";
    case ValidateReason::kBadSize: return "bad size";
    case ValidateReason::kBadSha256: return "bad sha256";
    case ValidateReason::kLoadAddressMissing: return "load_address missing";
    case ValidateReason::kLoadAddressNotHex: return "load_address is not hex";
    case ValidateReason::kNeedsAppRegion: return "an swd image needs app_region";
    case ValidateReason::kAppRegionNotHex: return "app_region is not hex";
    case ValidateReason::kDoesNotFitRegion: return "the image does not fit its app_region";
    case ValidateReason::kHexIsForProvision: return "format hex is for a provision file only";
    case ValidateReason::kProvisionNotHex: return "a provision file is Intel HEX (format hex)";
    case ValidateReason::kProvisionLoadAddress:
      return "a provision file has no load_address: its records say where";
    case ValidateReason::kProvisionNotNrf52840: return "a provision file is for an nRF52840 board";
    case ValidateReason::kProvisionNeedsRegion:
      return "a provision file needs its image's app_region";
    case ValidateReason::kTwoProvisionFiles: return "more than one provision file in the image";
  }
  return "?";
}

// manifestref.validate, in its order. Each `need(...)` there is a line here.
ValidateResult validate(const Manifest& m) {
  ValidateResult r;
  auto fail = [&r](ValidateReason reason, std::uint8_t image = 0, std::uint8_t file = 0) {
    r.ok = false;
    r.reason = reason;
    r.image = image;
    r.file = file;
    return r;
  };
  if (!m.has_schema || m.schema != 1) return fail(ValidateReason::kSchema);
  if (m.product == Product::kUnknown) return fail(ValidateReason::kProduct);
  if (m.version[0] == 0) return fail(ValidateReason::kVersionMissing);
  if (m.tag[0] == 0) return fail(ValidateReason::kTagMissing);
  if (!m.has_commit || m.commit_length == 0) return fail(ValidateReason::kCommitMissing);
  if (m.built_at[0] == 0) return fail(ValidateReason::kBuiltAtMissing);
  if (m.commit_length != kCommitSize) return fail(ValidateReason::kCommitLength);
  if (!m.has_link) return fail(ValidateReason::kLinkMissing);
  if (!m.has_images || m.image_count == 0) return fail(ValidateReason::kNoImages);
  for (std::uint8_t i = 0; i < m.image_count; ++i) {
    const ImageEntry& image = m.images[i];
    if (image.id[0] == 0) return fail(ValidateReason::kImageIdMissing, i);
    if (image.role[0] == 0) return fail(ValidateReason::kImageRoleMissing, i);
    if (image.board[0] == 0) return fail(ValidateReason::kImageBoardMissing, i);
    if (image.band[0] == 0) return fail(ValidateReason::kImageBandMissing, i);
    if (image.mcu[0] == 0) return fail(ValidateReason::kImageMcuMissing, i);
    for (std::uint8_t j = 0; j < i; ++j) {
      if (std::strcmp(m.images[j].id, image.id) == 0) return fail(ValidateReason::kDuplicateImageId, i);
    }
    if (!image.has_files || image.file_count == 0) return fail(ValidateReason::kNoFiles, i);
    for (std::uint8_t f = 0; f < image.file_count; ++f) {
      const FileEntry& file = image.files[f];
      if (file.use == Use::kUnknown) return fail(ValidateReason::kUnknownUse, i, f);
      if (!file.has_name || std::strchr(file.name, '/') != nullptr || std::strchr(file.name, '\\') != nullptr) {
        return fail(ValidateReason::kBadFileName, i, f);
      }
      if (!file.has_size || file.size == 0) return fail(ValidateReason::kBadSize, i, f);
      if (!file.has_sha256) return fail(ValidateReason::kBadSha256, i, f);
      if (file.use == Use::kSwd || file.use == Use::kOta || file.use == Use::kFactory) {
        if (file.load_address_state == HexField::kAbsent) return fail(ValidateReason::kLoadAddressMissing, i, f);
        if (file.load_address_state == HexField::kNotHex) return fail(ValidateReason::kLoadAddressNotHex, i, f);
      }
      if (file.use == Use::kSwd) {
        if (image.app_region_state == HexField::kAbsent) return fail(ValidateReason::kNeedsAppRegion, i, f);
        if (image.app_region_state == HexField::kNotHex) return fail(ValidateReason::kAppRegionNotHex, i, f);
        // start <= load and load + size <= end, the sum taken in 64 bits so that it cannot wrap.
        const std::uint64_t end = static_cast<std::uint64_t>(file.load_address) + file.size;
        if (!(image.app_region_start <= file.load_address && end <= image.app_region_end)) {
          return fail(ValidateReason::kDoesNotFitRegion, i, f);
        }
      }
      // §2, the base (§4a): `hex` belongs to a `provision` file and to nothing else, and such a
      // file says where its bytes go in its own records - never in a load_address.
      if (std::strcmp(file.format, "hex") == 0 && file.use != Use::kProvision) {
        return fail(ValidateReason::kHexIsForProvision, i, f);
      }
      if (file.use == Use::kProvision) {
        if (std::strcmp(file.format, "hex") != 0) return fail(ValidateReason::kProvisionNotHex, i, f);
        if (file.load_address_state != HexField::kAbsent) {
          return fail(ValidateReason::kProvisionLoadAddress, i, f);
        }
        if (std::strcmp(image.mcu, "nrf52840") != 0) {
          return fail(ValidateReason::kProvisionNotNrf52840, i, f);
        }
        if (image.app_region_state == HexField::kAbsent) {
          return fail(ValidateReason::kProvisionNeedsRegion, i, f);
        }
        if (image.app_region_state == HexField::kNotHex) {
          return fail(ValidateReason::kAppRegionNotHex, i, f);
        }
      }
    }
    // At most one base per image (§2): the same file may be listed under several images of one
    // board - the hash makes it one file - but an image with two is not an image this firmware
    // can choose between.
    std::uint8_t provisions = 0;
    for (std::uint8_t f = 0; f < image.file_count; ++f) {
      if (image.files[f].use == Use::kProvision) ++provisions;
    }
    if (provisions > 1) return fail(ValidateReason::kTwoProvisionFiles, i);
  }
  r.ok = true;
  return r;
}

}  // namespace image
}  // namespace core
