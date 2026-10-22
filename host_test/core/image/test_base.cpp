// The base, judged (task p5-01): core/image/base.h against spec/FIRMWARE-MANIFEST.md §4a and the
// specification's own fixtures.
//
// The failure cases first, and they are the specification's: every `base-*.hex` of
// spec/fixtures/manifest/ is refused with the sentence `index.json` gives for it, **word for
// word**, and `base-rak4630.hex` is accepted with the summary `index.json` gives - the fixtures
// and their reasons coming from the generated table (host_test/fixtures/
// gen_manifest_fixtures.py), so that a case added to the specification becomes a case here.
//
// Then the three things that are this firmware's and not the reference's: the file is read as a
// **stream** and never held in RAM, the rules are applied against **the region the caller gives**,
// and the words the base will be written as (task p5-03) come out of it in address order.
#include <doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/flasher/board_profile.h"
#include "core/image/base.h"
#include "fixtures/manifest_files.h"

namespace img = core::image;

namespace {

// A file as an ImageSource that will serve a read of any range - the store's StoredImage does the
// same over the Walter's flash. `most` caps one read, so that a test can show the reader working
// in pieces no larger than it asks for.
class Bytes final : public core::flasher::ImageSource {
 public:
  explicit Bytes(std::vector<std::uint8_t> bytes, std::uint32_t most = 0)
      : bytes_(std::move(bytes)), most_(most) {}

  std::uint32_t size() const override { return static_cast<std::uint32_t>(bytes_.size()); }
  bool read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const override {
    if (offset > bytes_.size() || count > bytes_.size() - offset) return false;
    if (most_ != 0) {
      CHECK(count <= most_);
      if (count > largest_) largest_ = count;
    }
    std::memcpy(out, bytes_.data() + offset, count);
    reads_ += count;
    return true;
  }
  std::uint64_t bytesRead() const { return reads_; }
  std::uint32_t largestRead() const { return largest_; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint32_t most_;
  mutable std::uint64_t reads_ = 0;
  mutable std::uint32_t largest_ = 0;
};

Bytes spec(const char* name, std::uint32_t most = 0) {
  return Bytes(fixtures::readSpec(name), most);
}

Bytes text(const std::string& lines, std::uint32_t most = 0) {
  return Bytes(std::vector<std::uint8_t>(lines.begin(), lines.end()), most);
}

// The region core/flasher/board_profile.h gives rak4630-single, which is what check 5p judges a
// base against, and the example region the specification's fixture manifest carries.
constexpr std::uint32_t kBoardStart = 0x00026000;
constexpr std::uint32_t kBoardEnd = 0x000ED000;

img::BaseReader& reader() {
  // ~2 KB: a static, as the firmware keeps it (inside the verdict).
  static img::BaseReader one;
  return one;
}

}  // namespace

TEST_CASE("every base fixture of the specification, accepted or refused in its own words") {
  REQUIRE(fixtures::kBaseCount >= 15);
  REQUIRE(fixtures::kProvisionRegionStart == kBoardStart);
  for (std::size_t i = 0; i < fixtures::kBaseCount; ++i) {
    const fixtures::BaseCase& c = fixtures::kBases[i];
    const std::string name = c.file;
    Bytes file = spec(c.file);
    const img::BaseResult result =
        reader().read(file, static_cast<std::uint32_t>(fixtures::kProvisionRegionStart),
                      static_cast<std::uint32_t>(fixtures::kProvisionRegionEnd));
    if (!c.valid) {
      const bool refused = !result.ok();
      CHECK_MESSAGE(refused, name << " was accepted and index.json says it is refused");
      // The reference's sentence, character for character: these words are the contract between
      // the three readers of a base (index.json's `provision`).
      const std::string why = result.why;
      CHECK_MESSAGE(why == std::string(c.why), name << ": " << why << " (index.json: " << c.why << ")");
      continue;
    }
    CHECK_MESSAGE(result.ok(), name << " was refused: " << result.why);
    if (!result.ok()) continue;
    CHECK(result.summary.bootloader == c.bootloader);
    CHECK(result.summary.flash_bytes == c.flash_bytes);
    CHECK(result.summary.softdevice_id == c.softdevice_id);
    CHECK(result.summary.softdevice_version == c.softdevice_version);
    CHECK(result.summary.has_mbr_params == c.has_mbr_params);
    if (c.has_mbr_params) CHECK(result.summary.mbr_params == c.mbr_params);
    CHECK(result.summary.uicr_word_count == c.uicr_word_count);
  }
}

TEST_CASE("and the same answer against the board's own region, which is what check 5p uses") {
  // index.json: "every base gives the same result against the rak4630-single board region,
  // 0x00026000-0x000ED000". The one rule the two regions could differ over is the sixth, and the
  // fixtures' bootloader is above both ends.
  for (std::size_t i = 0; i < fixtures::kBaseCount; ++i) {
    const fixtures::BaseCase& c = fixtures::kBases[i];
    Bytes file = spec(c.file);
    const img::BaseResult result = reader().read(file, kBoardStart, kBoardEnd);
    CHECK_MESSAGE(result.ok() == c.valid, c.file);
    if (!c.valid) CHECK_MESSAGE(std::string(result.why) == std::string(c.why), c.file);
  }
}

TEST_CASE("the region is the caller's: a base of one region is not a base of another") {
  // base-rak4630.hex says the SoftDevice ends at 0x26000, so it is a base for a board whose
  // application starts there and for no other - which is rule 5, and is what makes check 5p's
  // "against **the build's** region" mean something.
  Bytes file = spec("base-rak4630.hex");
  const img::BaseResult other = reader().read(file, 0x00027000, kBoardEnd);
  CHECK(other.reason == img::BaseReason::kSoftDeviceEnd);
  CHECK(std::string(other.why) ==
        "the SoftDevice ends at 0x00026000, the application starts at 0x00027000");
  // And a region whose end is above the file's bootloader leaves rule 6 unsatisfiable.
  const img::BaseResult high = reader().read(file, kBoardStart, 0x000F8000);
  CHECK(high.reason == img::BaseReason::kBootloaderAddress);
}

TEST_CASE("the file is read as a stream: no read larger than the reader's own buffer") {
  // A real base is ~450 KB of text. Nothing here holds it: the reader asks for at most 256 bytes
  // at a time, whatever the file's size, which is what lets it judge one out of the Walter's
  // flash without a buffer for it.
  Bytes file = spec("base-rak4630.hex", 256);
  const img::BaseResult result = reader().read(file, kBoardStart, kBoardEnd);
  REQUIRE(result.ok());
  CHECK(file.largestRead() <= 256);
  // Two passes over the file and no more: the rules, and the seventh over the addresses again.
  CHECK(file.bytesRead() <= 2 * file.size() + 512);
}

TEST_CASE("rule 1, the records: what the reference refuses and what it allows") {
  const std::string good = fixtures::asText(fixtures::readSpec("base-rak4630.hex"));
  auto judge = [](const std::string& lines) {
    Bytes file = text(lines);
    return reader().read(file, kBoardStart, kBoardEnd);
  };

  SUBCASE("a blank line is counted and skipped, and the file is still a base") {
    std::string with_blanks = "\n\n" + good;
    const img::BaseResult r = judge(with_blanks);
    CHECK(r.ok());
  }
  SUBCASE("CR LF is whitespace, not a record") {
    std::string crlf;
    for (char c : good) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    CHECK(judge(crlf).ok());
  }
  SUBCASE("a line that is not a record") {
    const img::BaseResult r = judge("# a comment\n" + good);
    CHECK(r.reason == img::BaseReason::kNotARecord);
    CHECK(std::string(r.why) == "line 1: not an Intel HEX record");
  }
  SUBCASE("an odd number of hex digits") {
    const img::BaseResult r = judge(":0400000\n" + good);
    CHECK(r.reason == img::BaseReason::kNotHexDigits);
    CHECK(std::string(r.why) == "line 1: not hex digits");
  }
  SUBCASE("a record whose length byte is not its length") {
    // :02 0000 00 00 FE - length 2, one data byte.
    const img::BaseResult r = judge(":020000000000\n" + good);
    CHECK(r.reason == img::BaseReason::kRecordLength);
    CHECK(std::string(r.why) == "line 1: record length");
  }
  SUBCASE("a record type nobody knows") {
    const img::BaseResult r = judge(":00000006FA\n" + good);
    CHECK(r.reason == img::BaseReason::kRecordType);
    CHECK(std::string(r.why) == "line 1: record type 6");
  }
  SUBCASE("a byte above 0x7F anywhere, which beats every line's own trouble") {
    // The file is not ASCII, and the reference decodes it before it parses a record - so this is
    // the refusal even though line 1 is also not a record.
    std::string bad = "# not a record\n" + good;
    bad.push_back(static_cast<char>(0xC3));
    bad.push_back('\n');
    const img::BaseResult r = judge(bad);
    CHECK(r.reason == img::BaseReason::kNotAscii);
    CHECK(std::string(r.why) == "not ASCII");
  }
  SUBCASE("an empty file: no end record") {
    const img::BaseResult r = judge("");
    CHECK(r.reason == img::BaseReason::kNoEndRecord);
    CHECK(std::string(r.why) == "no end record (a truncated file?)");
  }
  SUBCASE("an end record and nothing else: no data") {
    const img::BaseResult r = judge(":00000001FF\n");
    CHECK(r.reason == img::BaseReason::kNoData);
    CHECK(std::string(r.why) == "no data");
  }
  SUBCASE("the same byte given twice is allowed, as the reference allows it") {
    // The rule is "no address given two **different** bytes". base-rak4630.hex gives 0x00000000
    // the byte 0xFC (its first record): giving it again, with the same byte, changes nothing.
    std::string again = good;
    const std::size_t end = again.rfind(":00000001FF");
    REQUIRE(end != std::string::npos);
    again.insert(end, ":020000040000FA\n:01000000FC03\n");
    const img::BaseResult r = judge(again);
    CHECK_MESSAGE(r.ok(), r.why);
    // ... and a different byte at that same address is rule 1's refusal, with the line and the
    // address the reference names.
    std::string differs = good;
    differs.insert(end, ":020000040000FA\n:0100000000FF\n");
    const img::BaseResult bad = judge(differs);
    CHECK(bad.reason == img::BaseReason::kTwoDifferentBytes);
    CHECK(std::string(bad.why) == "line 38: 0x00000000 given two different bytes");
  }
}

// -- the words, in address order -----------------------------------------------------------------

namespace {

// A reference walk of a `provision` file, written here out of the file's own records and not out
// of the code under test: every address the file gives a byte of, as (word address -> four bytes
// with 0xFF where the file gives none). std::map, which core/ may not use and a test may.
std::map<std::uint32_t, std::uint32_t> referenceWords(const std::vector<std::uint8_t>& file) {
  std::map<std::uint32_t, std::uint8_t> given;
  std::uint32_t base = 0;
  std::string line;
  const std::string whole(file.begin(), file.end());
  std::size_t at = 0;
  while (at <= whole.size()) {
    const std::size_t nl = whole.find('\n', at);
    line = whole.substr(at, (nl == std::string::npos ? whole.size() : nl) - at);
    at = (nl == std::string::npos) ? whole.size() + 1 : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] != ':') continue;
    std::vector<std::uint8_t> raw;
    for (std::size_t i = 1; i + 1 < line.size(); i += 2) {
      raw.push_back(static_cast<std::uint8_t>(std::stoul(line.substr(i, 2), nullptr, 16)));
    }
    REQUIRE(raw.size() >= 5);
    const std::uint32_t address = static_cast<std::uint32_t>(raw[1] << 8) | raw[2];
    const std::uint8_t kind = raw[3];
    if (kind == 0) {
      for (std::uint8_t i = 0; i < raw[0]; ++i) given[base + address + i] = raw[4 + i];
    } else if (kind == 2) {
      base = (static_cast<std::uint32_t>(raw[4] << 8) | raw[5]) << 4;
    } else if (kind == 4) {
      base = (static_cast<std::uint32_t>(raw[4] << 8) | raw[5]) << 16;
    }
  }
  std::map<std::uint32_t, std::uint32_t> words;
  for (const auto& byte : given) {
    const std::uint32_t word = byte.first & ~3u;
    if (words.count(word) == 0) {
      std::uint32_t value = 0;
      for (std::uint32_t i = 0; i < 4; ++i) {
        const auto found = given.find(word + i);
        const std::uint8_t b = found == given.end() ? 0xFF : found->second;
        value |= static_cast<std::uint32_t>(b) << (8 * i);
      }
      words[word] = value;
    }
  }
  return words;
}

}  // namespace

TEST_CASE("the base's words, in address order, are the file's own") {
  const std::vector<std::uint8_t> bytes = fixtures::readSpec("base-rak4630.hex");
  Bytes file(bytes);
  const img::BaseResult result = reader().read(file, kBoardStart, kBoardEnd);
  REQUIRE(result.ok());

  const std::map<std::uint32_t, std::uint32_t> want = referenceWords(bytes);
  static img::BaseWords words;
  words.begin(file, result.runs);
  std::uint64_t address = 0;
  std::uint32_t word = 0;
  std::uint64_t last = 0;
  std::size_t seen = 0;
  bool first = true;
  bool uicr_after_flash = true;
  bool saw_uicr = false;
  while (words.take(&address, &word)) {
    if (!first) {
      CHECK_MESSAGE(address > last, "the words come in address order");
    }
    if (address >= img::kUicrFirst) saw_uicr = true;
    else if (saw_uicr) uicr_after_flash = false;  // flash after UICR: not address order
    const auto found = want.find(static_cast<std::uint32_t>(address));
    REQUIRE_MESSAGE(found != want.end(), "a word the file gives no byte of was handed out");
    CHECK_MESSAGE(found->second == word, "the wrong value at a word of the base");
    last = address;
    first = false;
    ++seen;
  }
  CHECK_FALSE(words.failed());
  CHECK(seen == want.size());
  CHECK(saw_uicr);
  CHECK(uicr_after_flash);
}

TEST_CASE("a word the file gives part of is written with the rest erased") {
  // Two bytes at 0x00000000 and nothing of the other two: the word goes out as 0xFFFF0201, so
  // that the bytes the file says nothing about are left as an erase left them (the NVMC can only
  // clear bits - base.h says why). The file is built here, record by record, so that what is in
  // it is what this test says is in it.
  auto record = [](std::uint32_t address, std::uint8_t kind,
                   const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> raw;
    raw.push_back(static_cast<std::uint8_t>(body.size()));
    raw.push_back(static_cast<std::uint8_t>(address >> 8));
    raw.push_back(static_cast<std::uint8_t>(address));
    raw.push_back(kind);
    for (std::uint8_t b : body) raw.push_back(b);
    std::uint32_t sum = 0;
    for (std::uint8_t b : raw) sum += b;
    raw.push_back(static_cast<std::uint8_t>((-static_cast<std::int32_t>(sum)) & 0xFF));
    static const char* digits = "0123456789ABCDEF";
    std::string out = ":";
    for (std::uint8_t b : raw) {
      out.push_back(digits[b >> 4]);
      out.push_back(digits[b & 0x0F]);
    }
    out.push_back('\n');
    return out;
  };
  auto word32 = [](std::uint32_t v) {
    return std::vector<std::uint8_t>{static_cast<std::uint8_t>(v),
                                     static_cast<std::uint8_t>(v >> 8),
                                     static_cast<std::uint8_t>(v >> 16),
                                     static_cast<std::uint8_t>(v >> 24)};
  };
  std::vector<std::uint8_t> info;
  for (std::uint32_t v : {0u, img::kSoftDeviceMagic, kBoardStart, 0xB6u,
                          img::kSoftDeviceS140Id, img::kSoftDeviceS140Version}) {
    for (std::uint8_t b : word32(v)) info.push_back(b);
  }
  std::vector<std::uint8_t> uicr;
  for (std::uint8_t b : word32(0x000F4000)) uicr.push_back(b);

  std::string file;
  file += record(0, 4, {0x00, 0x00});
  file += record(0x0000, 0, {0x01, 0x02});  // two bytes of the word at 0x00000000
  file += record(0x3000, 0, info);
  file += record(0x4000, 0, {0xAA});  // a byte below the application region: rule 7 allows it
  file += record(0x0000, 0, {});      // an empty data record: nothing, and no refusal
  file += record(0, 4, {0x00, 0x0F});  // ... and now 0xF0000 and up
  file += record(0x4000, 0, {0x5A});   // 0x000F4000: the bootloader's first byte
  file += record(0, 4, {0x10, 0x00});
  file += record(0x1014, 0, uicr);
  file += record(0, 1, {});
  Bytes source(std::vector<std::uint8_t>(file.begin(), file.end()));
  const img::BaseResult result = reader().read(source, kBoardStart, kBoardEnd);
  REQUIRE_MESSAGE(result.ok(), result.why);

  static img::BaseWords words;
  words.begin(source, result.runs);
  std::uint64_t address = 0;
  std::uint32_t word = 0;
  REQUIRE(words.take(&address, &word));
  CHECK(address == 0);
  CHECK(word == 0xFFFF0201u);  // the two bytes the file gives, the two it does not left erased
}
