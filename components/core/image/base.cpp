#include "core/image/base.h"

#include <cstring>

namespace core {
namespace image {

namespace {

// -- the sentences ------------------------------------------------------------------------------
//
// The refusals are the reference's own words (spec/tools/manifestref.py, and
// spec/fixtures/manifest/index.json's `why` for each fixture), and some of them carry a number
// the file gave. core/image/ has no formatting - no <cstdio>, nothing that allocates - so the
// decimal and the `0x%08X` are written here, and the test holds the result against index.json
// character for character.

class Words {
 public:
  Words(char* out, std::size_t size) : out_(out), size_(size) {
    if (size_ > 0) out_[0] = 0;
  }

  void add(const char* text) {
    if (text == nullptr) return;
    while (*text != 0 && at_ + 1 < size_) out_[at_++] = *text++;
    if (size_ > 0) out_[at_] = 0;
  }

  void addDecimal(std::uint64_t value) {
    char digits[24];
    std::size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + static_cast<char>(value % 10));
      value /= 10;
    } while (value != 0 && n < sizeof digits);
    while (n > 0 && at_ + 1 < size_) out_[at_++] = digits[--n];
    if (size_ > 0) out_[at_] = 0;
  }

  // `0x%08X`: eight hex digits, more when the value needs them (as the reference's Python does).
  void addHex(std::uint64_t value) {
    char digits[16];
    std::size_t n = 0;
    do {
      const unsigned nibble = static_cast<unsigned>(value & 0xF);
      digits[n++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + (nibble - 10));
      value >>= 4;
    } while (value != 0 && n < sizeof digits);
    add("0x");
    for (std::size_t pad = n; pad < 8; ++pad) {
      if (at_ + 1 < size_) out_[at_++] = '0';
    }
    while (n > 0 && at_ + 1 < size_) out_[at_++] = digits[--n];
    if (size_ > 0) out_[at_] = 0;
  }

 private:
  char* out_;
  std::size_t size_;
  std::size_t at_ = 0;
};

bool isSpace(std::uint8_t c) {
  // Python's str.strip() and bytes.fromhex() skip these and nothing else of ASCII.
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C;
}

bool hexDigit(std::uint8_t c, std::uint8_t* out) {
  if (c >= '0' && c <= '9') {
    *out = static_cast<std::uint8_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *out = static_cast<std::uint8_t>(c - 'a' + 10);
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    *out = static_cast<std::uint8_t>(c - 'A' + 10);
    return true;
  }
  return false;
}

// A record is at most 255 data bytes plus length, two address bytes, type and checksum; a line
// with more decoded bytes than that cannot satisfy `len(raw) == raw[0] + 5` and is a record
// length error, which is what the reference says of it.
constexpr std::size_t kMaxRawBytes = 255 + 5;

bool inUicr(std::uint64_t address) {
  return address >= kUicrFirst && address < kUicrAfterLast;
}

}  // namespace

const char* describe(BaseReason reason) {
  switch (reason) {
    case BaseReason::kNone: return "a base";
    case BaseReason::kNotAscii:
    case BaseReason::kRecordAfterEnd:
    case BaseReason::kNotARecord:
    case BaseReason::kNotHexDigits:
    case BaseReason::kRecordLength:
    case BaseReason::kChecksum:
    case BaseReason::kTwoDifferentBytes:
    case BaseReason::kRecordType:
    case BaseReason::kNoEndRecord:
    case BaseReason::kNoData: return "rule 1, the records";
    case BaseReason::kNotFlashOrUicr: return "rule 2, flash or UICR";
    case BaseReason::kWritesApprotect: return "rule 3, the protection word";
    case BaseReason::kNoMbr: return "rule 4, the MBR";
    case BaseReason::kNoSoftDeviceInfo:
    case BaseReason::kSoftDeviceEnd:
    case BaseReason::kSoftDeviceVersion: return "rule 5, the SoftDevice";
    case BaseReason::kNoBootloaderAddress:
    case BaseReason::kBootloaderAddress: return "rule 6, the bootloader's address";
    case BaseReason::kInsideTheApplication: return "rule 7, the application";
    case BaseReason::kTooManyRuns:
    case BaseReason::kTooManyRepeats: return "a bound of this firmware, not a rule of 4a";
    case BaseReason::kUnreadable: return "the file could not be read";
  }
  return "?";
}

// -- the summary --------------------------------------------------------------------------------

std::uint32_t BaseSummary::uicrWordAt(std::uint16_t i) const {
  std::uint16_t seen = 0;
  for (std::uint16_t w = 0; w < kUicrWordCount; ++w) {
    if ((uicr[w / 8] & (1u << (w % 8))) == 0) continue;
    if (seen == i) return kUicrFirst + static_cast<std::uint32_t>(w) * 4;
    ++seen;
  }
  return 0;
}

// -- the runs -----------------------------------------------------------------------------------

bool BaseRuns::holds(std::uint64_t address) const {
  for (std::size_t i = 0; i < count; ++i) {
    if (address >= at[i].begin && address < at[i].end) return true;
  }
  return false;
}

bool BaseRuns::from(std::uint64_t address, std::uint64_t* out) const {
  bool found = false;
  std::uint64_t best = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint64_t candidate = address > at[i].begin ? address : at[i].begin;
    if (candidate >= at[i].end) continue;  // this run is wholly below `address`
    if (!found || candidate < best) {
      best = candidate;
      found = true;
    }
  }
  if (found && out != nullptr) *out = best;
  return found;
}

std::uint32_t BaseRuns::bytesBelow(std::uint64_t limit) const {
  std::uint64_t bytes = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (at[i].begin >= limit) continue;
    const std::uint64_t end = at[i].end < limit ? at[i].end : limit;
    bytes += end - at[i].begin;
  }
  return static_cast<std::uint32_t>(bytes);
}

// -- the records (rule 1) -----------------------------------------------------------------------

void HexRecords::begin(const flasher::ImageSource& file) {
  file_ = &file;
  at_ = 0;
  have_ = 0;
  next_ = 0;
  line_ = 1;
  base_ = 0;
  ended_ = false;
  any_data_ = false;
  non_ascii_ = false;
  reason_ = BaseReason::kNone;
  why_[0] = 0;
}

bool HexRecords::byte(std::uint8_t* out) {
  if (next_ == have_) {
    if (file_ == nullptr) return false;
    const std::uint32_t size = file_->size();
    if (at_ >= size) return false;
    const std::uint32_t left = size - at_;
    const std::uint32_t n = left < sizeof buffer_ ? left : static_cast<std::uint32_t>(sizeof buffer_);
    if (!file_->read(at_, buffer_, n)) {
      reason_ = BaseReason::kUnreadable;
      Words words(why_, sizeof why_);
      words.add("the file could not be read at byte ");
      words.addDecimal(at_);
      return false;
    }
    at_ += n;
    have_ = n;
    next_ = 0;
  }
  *out = buffer_[next_++];
  // The reference decodes the whole file as ASCII before it looks at a record, so one byte above
  // 0x7F anywhere refuses the file before any line's own trouble does.
  if (*out > 0x7F) non_ascii_ = true;
  return true;
}

void HexRecords::refuse(BaseReason reason, const char* before, std::uint64_t number,
                        const char* after) {
  if (reason_ != BaseReason::kNone) return;
  reason_ = reason;
  Words words(why_, sizeof why_);
  words.add(before);
  words.addDecimal(number);
  words.add(after);
}

void HexRecords::drain() {
  std::uint8_t ignored = 0;
  while (byte(&ignored)) {
  }
}

bool HexRecords::take(HexRecord* out) {
  if (reason_ != BaseReason::kNone || out == nullptr) return false;
  for (;;) {
    // One line, as Python's splitlines() cuts them: separated by '\n', the last one needing none.
    std::uint8_t raw[kMaxRawBytes];
    std::size_t raw_count = 0;   // decoded bytes, the first kMaxRawBytes of them kept
    std::size_t decoded = 0;     // how many there were in all
    bool any_char = false;       // the line has something other than whitespace
    bool is_record = false;      // ... and the first of it is ':'
    bool bad_digits = false;
    bool half_byte = false;
    std::uint8_t high = 0;
    bool have_high = false;
    bool end_of_file = false;
    for (;;) {
      std::uint8_t c = 0;
      if (!byte(&c)) {
        end_of_file = true;
        break;
      }
      if (c == '\n') break;
      if (isSpace(c)) continue;  // stripped at the ends; skipped inside, as fromhex skips it
      if (!any_char) {
        any_char = true;
        is_record = c == ':';
        if (is_record) continue;
      }
      if (!is_record) continue;  // the whole line is refused below; read it to its end
      std::uint8_t nibble = 0;
      if (!hexDigit(c, &nibble)) {
        bad_digits = true;
        continue;
      }
      if (!have_high) {
        high = nibble;
        have_high = true;
        continue;
      }
      have_high = false;
      if (raw_count < kMaxRawBytes) raw[raw_count++] = static_cast<std::uint8_t>((high << 4) | nibble);
      ++decoded;
    }
    half_byte = have_high;

    if (!any_char) {
      // A blank line: counted (the reference counts every line) and skipped.
      if (end_of_file) break;
      ++line_;
      continue;
    }
    if (ended_) {
      refuse(BaseReason::kRecordAfterEnd, "line ", line_, ": a record after the end record");
      return false;
    }
    if (!is_record) {
      refuse(BaseReason::kNotARecord, "line ", line_, ": not an Intel HEX record");
      return false;
    }
    if (bad_digits || half_byte) {
      // bytes.fromhex() first, then the length: a line that is both too long and not hex is "not
      // hex digits", which is why every digit of it was looked at above.
      refuse(BaseReason::kNotHexDigits, "line ", line_, ": not hex digits");
      return false;
    }
    if (decoded < 5 || decoded != static_cast<std::size_t>(raw[0]) + 5) {
      refuse(BaseReason::kRecordLength, "line ", line_, ": record length");
      return false;
    }
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < raw_count; ++i) sum += raw[i];
    if ((sum & 0xFF) != 0) {
      refuse(BaseReason::kChecksum, "line ", line_, ": checksum");
      return false;
    }

    const std::uint32_t address = static_cast<std::uint32_t>(raw[1] << 8) | raw[2];
    const std::uint8_t kind = raw[3];
    const std::uint8_t length = raw[0];
    const std::uint8_t* body = raw + 4;
    bool got_record = false;
    if (kind == 0) {
      out->line = line_;
      out->address = static_cast<std::uint64_t>(base_) + address;
      out->length = length;
      if (length > 0) {
        std::memcpy(out->data, body, length);
        any_data_ = true;
      }
      got_record = true;
    } else if (kind == 1) {
      ended_ = true;
    } else if (kind == 2 && length == 2) {
      base_ = (static_cast<std::uint64_t>(body[0] << 8) | body[1]) << 4;
    } else if (kind == 4 && length == 2) {
      base_ = (static_cast<std::uint64_t>(body[0] << 8) | body[1]) << 16;
    } else if (kind == 3 || kind == 5) {
      // A start address: read and ignored (§4a rule 1).
    } else {
      reason_ = BaseReason::kRecordType;
      Words words(why_, sizeof why_);
      words.add("line ");
      words.addDecimal(line_);
      words.add(": record type ");
      words.addDecimal(kind);
      return false;
    }
    if (end_of_file) {
      // The last line needs no newline; the next call finds nothing and says so.
      if (got_record) {
        ++line_;
        return true;
      }
      break;
    }
    ++line_;
    if (got_record) return true;
  }
  return false;
}

// -- the reader ---------------------------------------------------------------------------------

std::uint32_t BaseReader::Watched::word() const {
  return static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8) |
         (static_cast<std::uint32_t>(value[2]) << 16) | (static_cast<std::uint32_t>(value[3]) << 24);
}

bool BaseReader::addRun(std::uint64_t begin, std::uint64_t end) {
  if (begin >= end) return true;
  std::size_t i = 0;
  while (i < runs_.count && runs_.at[i].end < begin) ++i;
  if (i < runs_.count && runs_.at[i].begin <= end) {
    // It touches or overlaps this run, and perhaps the ones after it: one run comes of them all.
    std::uint64_t low = runs_.at[i].begin < begin ? runs_.at[i].begin : begin;
    std::uint64_t high = runs_.at[i].end > end ? runs_.at[i].end : end;
    std::size_t j = i;
    while (j < runs_.count && runs_.at[j].begin <= high) {
      if (runs_.at[j].end > high) high = runs_.at[j].end;
      ++j;
    }
    runs_.at[i].begin = low;
    runs_.at[i].end = high;
    const std::size_t gone = j - i - 1;
    for (std::size_t k = i + 1; k + gone < runs_.count; ++k) runs_.at[k] = runs_.at[k + gone];
    runs_.count -= gone;
    return true;
  }
  if (runs_.count == kMaxRuns) return false;
  for (std::size_t k = runs_.count; k > i; --k) runs_.at[k] = runs_.at[k - 1];
  runs_.at[i].begin = begin;
  runs_.at[i].end = end;
  ++runs_.count;
  return true;
}

bool BaseReader::repeatDiffers(const flasher::ImageSource& file, const HexRecord& record,
                               std::uint64_t* address) {
  std::memset(repeat_given_, 0, sizeof repeat_given_);
  earlier_.begin(file);
  HexRecord before;
  while (earlier_.take(&before)) {
    if (before.line >= record.line) break;
    for (std::uint8_t i = 0; i < before.length; ++i) {
      const std::uint64_t a = before.address + i;
      if (a < record.address || a >= record.address + record.length) continue;
      const std::size_t index = static_cast<std::size_t>(a - record.address);
      repeat_value_[index] = before.data[i];
      repeat_given_[index] = 1;
    }
  }
  for (std::uint8_t i = 0; i < record.length; ++i) {
    if (repeat_given_[i] != 0 && repeat_value_[i] != record.data[i]) {
      *address = record.address + i;
      return true;
    }
  }
  return false;
}

BaseResult BaseReader::read(const flasher::ImageSource& file, std::uint32_t app_start,
                            std::uint32_t app_end) {
  BaseResult result;
  runs_ = BaseRuns{};
  const std::uint32_t watched_addresses[6] = {kSoftDeviceMagicAt, kSoftDeviceEndAt,
                                              kSoftDeviceIdAt,    kSoftDeviceVersionAt,
                                              kUicrBootloader,    kUicrMbrParams};
  for (std::size_t i = 0; i < 6; ++i) {
    watched_[i] = Watched{};
    watched_[i].address = watched_addresses[i];
  }
  const Watched& sd_magic = watched_[0];
  const Watched& sd_end = watched_[1];
  const Watched& sd_id = watched_[2];
  const Watched& sd_version = watched_[3];
  const Watched& bootloader = watched_[4];
  const Watched& mbr_params = watched_[5];

  bool approtect = false;
  bool mbr = false;
  bool have_outside = false;
  std::uint64_t outside = 0;
  std::size_t repeats = 0;
  // A bound of this firmware, not a rule of §4a: kept and reported only once the whole file has
  // been parsed, so that a real rule-1 refusal further down the file is still the one given.
  BaseReason mine = BaseReason::kNone;
  std::uint64_t mine_number = 0;

  records_.begin(file);
  HexRecord record;
  while (records_.take(&record)) {
    if (record.length == 0) continue;
    const std::uint64_t begin = record.address;
    const std::uint64_t end = begin + record.length;

    // Rule 1's last clause: "no address given two different bytes". The runs say whether any of
    // these addresses has been given before; only then is the file walked again to see whether
    // the byte differs, because giving the same byte twice is allowed.
    bool overlaps = false;
    for (std::size_t i = 0; i < runs_.count && !overlaps; ++i) {
      overlaps = runs_.at[i].begin < end && begin < runs_.at[i].end;
    }
    if (overlaps) {
      if (++repeats > kMaxRepeats) {
        if (mine == BaseReason::kNone) mine = BaseReason::kTooManyRepeats;
      } else {
        std::uint64_t where = 0;
        if (repeatDiffers(file, record, &where)) {
          result.reason = BaseReason::kTwoDifferentBytes;
          Words words(result.why, sizeof result.why);
          words.add("line ");
          words.addDecimal(record.line);
          words.add(": ");
          words.addHex(where);
          words.add(" given two different bytes");
          return result;
        }
      }
    }
    if (!addRun(begin, end) && mine == BaseReason::kNone) {
      mine = BaseReason::kTooManyRuns;
      mine_number = kMaxRuns;
    }

    for (std::uint8_t i = 0; i < record.length; ++i) {
      const std::uint64_t a = begin + i;
      if (!have_outside && !(a < kFlashEnd || inUicr(a))) {
        have_outside = true;
        outside = a;
      }
      if (a >= kUicrApprotect && a < kUicrApprotect + 4) approtect = true;
      if (a == 0) mbr = true;
      if (inUicr(a)) {
        const std::uint16_t w = static_cast<std::uint16_t>((a - kUicrFirst) / 4);
        result.summary.uicr[w / 8] |= static_cast<std::uint8_t>(1u << (w % 8));
      }
      for (std::size_t w = 0; w < 6; ++w) {
        if (a < watched_[w].address || a >= watched_[w].address + 4) continue;
        const std::size_t byte_of = static_cast<std::size_t>(a - watched_[w].address);
        watched_[w].value[byte_of] = record.data[i];
        watched_[w].given |= static_cast<std::uint8_t>(1u << byte_of);
      }
    }
  }
  // The whole file is read whatever happened, because "not ASCII" is about all of it and comes
  // before any one line's refusal.
  records_.drain();

  Words why(result.why, sizeof result.why);
  if (records_.nonAscii()) {
    result.reason = BaseReason::kNotAscii;
    why.add("not ASCII");
    return result;
  }
  if (records_.reason() != BaseReason::kNone) {
    result.reason = records_.reason();
    why.add(records_.why());
    return result;
  }
  if (!records_.sawEnd()) {
    result.reason = BaseReason::kNoEndRecord;
    why.add("no end record (a truncated file?)");
    return result;
  }
  if (!records_.sawData()) {
    result.reason = BaseReason::kNoData;
    why.add("no data");
    return result;
  }
  if (mine == BaseReason::kTooManyRuns) {
    result.reason = mine;
    why.add("the file gives more than ");
    why.addDecimal(mine_number);
    why.add(" separate runs of addresses: this firmware does not read it as a base");
    return result;
  }
  if (mine == BaseReason::kTooManyRepeats) {
    result.reason = mine;
    why.add("the file gives an address a byte it has already given more than ");
    why.addDecimal(kMaxRepeats);
    why.add(" times: this firmware does not read it as a base");
    return result;
  }

  // 2. Every byte in flash or in UICR's registers.
  if (have_outside) {
    result.reason = BaseReason::kNotFlashOrUicr;
    why.addHex(outside);
    why.add(" is neither flash nor UICR");
    return result;
  }
  // 3. Nothing in UICR.APPROTECT.
  if (approtect) {
    result.reason = BaseReason::kWritesApprotect;
    why.add("the file writes UICR.APPROTECT: the Walter decides that word by the chip's revision");
    return result;
  }
  // 4. An MBR.
  if (!mbr) {
    result.reason = BaseReason::kNoMbr;
    why.add("nothing at 0x00000000: no MBR (a bootloader-only file?)");
    return result;
  }
  // 5. The SoftDevice's information structure, where it ends, and which SoftDevice it is.
  if (!sd_magic.whole() || sd_magic.word() != kSoftDeviceMagic) {
    result.reason = BaseReason::kNoSoftDeviceInfo;
    why.add("no SoftDevice information structure at 0x00003000");
    return result;
  }
  if (!sd_end.whole() || sd_end.word() != app_start) {
    result.reason = BaseReason::kSoftDeviceEnd;
    why.add("the SoftDevice ends at ");
    if (sd_end.whole()) {
      why.addHex(sd_end.word());
    } else {
      why.add("?");
    }
    why.add(", the application starts at ");
    why.addHex(app_start);
    return result;
  }
  if (!sd_id.whole() || !sd_version.whole() || sd_id.word() != kSoftDeviceS140Id ||
      sd_version.word() != kSoftDeviceS140Version) {
    result.reason = BaseReason::kSoftDeviceVersion;
    why.add("the SoftDevice is not S140 6.1.1 (id ");
    // "None" where the file gives no such word: the reference prints Python's None.
    if (sd_id.whole()) {
      why.addDecimal(sd_id.word());
    } else {
      why.add("None");
    }
    why.add(", version ");
    if (sd_version.whole()) {
      why.addDecimal(sd_version.word());
    } else {
      why.add("None");
    }
    why.add(")");
    return result;
  }
  // 6. The bootloader's address.
  if (!bootloader.whole()) {
    result.reason = BaseReason::kNoBootloaderAddress;
    why.add("no UICR.NRFFW[0]: the MBR would never start a bootloader");
    return result;
  }
  const std::uint32_t boot = bootloader.word();
  if (boot % kBasePageSize != 0 || boot < app_end || boot >= kFlashEnd || !runs_.holds(boot)) {
    result.reason = BaseReason::kBootloaderAddress;
    why.add("UICR.NRFFW[0] = ");
    why.addHex(boot);
    why.add(": not a page at or above the application region's end that the file fills");
    return result;
  }
  // 7. No flash byte between the application's start and the bootloader. A second pass, so that
  //    the address named is the first in the file's order, as the reference's is.
  records_.begin(file);
  while (records_.take(&record)) {
    for (std::uint8_t i = 0; i < record.length; ++i) {
      const std::uint64_t a = record.address + i;
      if (a >= kFlashEnd) continue;
      if (a < app_start || a >= boot) continue;
      result.reason = BaseReason::kInsideTheApplication;
      why.addHex(a);
      why.add(" lies between the SoftDevice and the bootloader: the application and the file "
              "system are not the base's");
      return result;
    }
  }
  if (records_.reason() != BaseReason::kNone) {
    // The same file, parsed the same way, cannot fail twice over - unless the flash it is read
    // from has changed under us, which is what this says.
    result.reason = BaseReason::kUnreadable;
    why.add(records_.why());
    return result;
  }

  result.runs = runs_;
  result.summary.bootloader = boot;
  result.summary.flash_bytes = runs_.bytesBelow(kFlashEnd);
  result.summary.softdevice_id = sd_id.word();
  result.summary.softdevice_version = sd_version.word();
  result.summary.has_mbr_params = mbr_params.whole();
  result.summary.mbr_params = mbr_params.whole() ? mbr_params.word() : 0;
  std::uint16_t words_of_uicr = 0;
  for (std::uint16_t w = 0; w < kUicrWordCount; ++w) {
    if ((result.summary.uicr[w / 8] & (1u << (w % 8))) != 0) ++words_of_uicr;
  }
  result.summary.uicr_word_count = words_of_uicr;
  return result;
}

// -- the base's words ---------------------------------------------------------------------------

void BaseWords::begin(const flasher::ImageSource& file, const BaseRuns& runs) {
  file_ = &file;
  runs_ = runs;
  window_ = 0;
  have_window_ = false;
  cursor_ = 0;
  failed_ = false;
}

bool BaseWords::fill(std::uint64_t window) {
  std::memset(given_, 0, sizeof given_);
  records_.begin(*file_);
  HexRecord record;
  while (records_.take(&record)) {
    for (std::uint8_t i = 0; i < record.length; ++i) {
      const std::uint64_t a = record.address + i;
      if (a < window || a >= window + kBasePageSize) continue;
      const std::size_t at = static_cast<std::size_t>(a - window);
      page_[at] = record.data[i];
      given_[at / 8] |= static_cast<std::uint8_t>(1u << (at % 8));
    }
  }
  return records_.reason() == BaseReason::kNone;
}

bool BaseWords::take(std::uint64_t* address, std::uint32_t* word) {
  if (failed_ || file_ == nullptr || address == nullptr || word == nullptr) return false;
  for (;;) {
    if (!have_window_) {
      std::uint64_t next = 0;
      if (!runs_.from(cursor_, &next)) return false;  // the base's last word has been given
      window_ = next & ~static_cast<std::uint64_t>(kBasePageSize - 1);
      if (!fill(window_)) {
        failed_ = true;
        return false;
      }
      have_window_ = true;
      cursor_ = next & ~static_cast<std::uint64_t>(3);
      if (cursor_ < window_) cursor_ = window_;
    }
    while (cursor_ < window_ + kBasePageSize) {
      const std::size_t at = static_cast<std::size_t>(cursor_ - window_);
      std::uint32_t value = 0;
      bool any = false;
      for (std::size_t i = 0; i < 4; ++i) {
        const bool given = (given_[(at + i) / 8] & (1u << ((at + i) % 8))) != 0;
        // A word the file gives part of is written with the rest erased: 0xFF, what an erase
        // leaves, because the NVMC can only clear bits (base.h says why).
        const std::uint8_t byte_of = given ? page_[at + i] : 0xFF;
        value |= static_cast<std::uint32_t>(byte_of) << (8 * i);
        any = any || given;
      }
      const std::uint64_t where = cursor_;
      cursor_ += 4;
      if (any) {
        *address = where;
        *word = value;
        return true;
      }
    }
    have_window_ = false;  // on to the next window the runs cover
  }
}

}  // namespace image
}  // namespace core
