#include "core/flasher/flasher.h"

#include "core/link/codec/crc16.h"
#include "core/nrf52/regs.h"

namespace core {
namespace flasher {

namespace settings = bootloader_settings;

const char* describe(Step step) {
  switch (step) {
    case Step::kNone: return "nothing";
    case Step::kPins: return "acquiring the SWD lines";
    case Step::kConnect: return "connecting to the debug port";
    case Step::kProtected: return "the chip is protected";
    case Step::kIdentify: return "identifying the chip";
    case Step::kSoftDevice: return "checking the SoftDevice against the profile";
    case Step::kImage: return "checking the image against the region";
    case Step::kScan: return "reading the pages the image covers";
    case Step::kHalt: return "halting the core";
    case Step::kSettingsInvalidate: return "marking bank 0 invalid in the bootloader's settings";
    case Step::kFirstPageErase: return "erasing the first page";
    case Step::kPages: return "erasing, writing and verifying the pages";
    case Step::kFirstPage: return "writing and verifying the first page";
    case Step::kReadBack: return "reading the whole image back";
    case Step::kDigest: return "checking the SHA-256 of what was read back";
    case Step::kSettingsValidate: return "marking bank 0 valid in the bootloader's settings";
    case Step::kErase: return "erasing the test pattern again";
    case Step::kReset: return "resetting the radio";
    case Step::kDone: return "done";
  }
  return "?";
}

namespace {

constexpr std::uint32_t kPage = nrf52::flash::kPageSize;
constexpr std::uint32_t kErased = nrf52::flash::kErased;

std::uint32_t wordOf(const std::uint8_t* b) {
  return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
         (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

void bytesOf(std::uint32_t w, std::uint8_t* b) {
  b[0] = static_cast<std::uint8_t>(w);
  b[1] = static_cast<std::uint8_t>(w >> 8);
  b[2] = static_cast<std::uint8_t>(w >> 16);
  b[3] = static_cast<std::uint8_t>(w >> 24);
}

// The bench's test pattern (rak flash-test): two pages of words that are neither blank nor
// regular, computed from the address so that nothing has to be stored.
class PatternImage final : public ImageSource {
 public:
  explicit PatternImage(std::uint32_t size) : size_(size) {}
  std::uint32_t size() const override { return size_; }
  bool read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const override {
    if (offset > size_ || count > size_ - offset) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t index = (offset + i) / 4;
      std::uint32_t w = 0xC3A5F00Fu ^ (index * 2654435761u);
      if (w == kErased) w ^= 1u;
      out[i] = static_cast<std::uint8_t>(w >> (8 * ((offset + i) % 4)));
    }
    return true;
  }

 private:
  std::uint32_t size_;
};

}  // namespace

Flasher::Flasher(board::RadioPins& pins, interfaces::Clock& clock)
    : pins_(pins), clock_(clock), wire_(pins.swd(), clock), dp_(wire_), nrf_(dp_, clock) {}

Result Flasher::refused(Step step, const char* why, std::uint32_t address) {
  Result r;
  r.outcome = Outcome::kRefused;
  r.step = step;
  r.why = why;
  r.address = address;
  return r;
}

Result Flasher::failed(Step step, const nrf52::Result& nrf, std::uint32_t address) {
  Result r;
  r.outcome = Outcome::kFailed;
  r.step = step;
  r.nrf = nrf;
  r.address = address != 0 ? address : nrf.address;
  r.why = nrf.status == nrf52::Status::kSwd ? swd::describe(nrf.swd) : nrf52::describe(nrf.status);
  return r;
}

void Flasher::report(ProgressSink* progress, Step step, std::uint32_t done, std::uint32_t total,
                     std::uint32_t address) {
  if (progress != nullptr) progress->onProgress(Progress{step, done, total, address});
}

// The image's bytes for one page, padded with 0xFF beyond the image's end, as words.
bool Flasher::loadPage(const Plan& plan, std::uint32_t page_addr) {
  const std::uint32_t load = plan.image->load_address;
  const std::uint32_t offset = page_addr - load;
  const std::uint32_t remaining = plan.size - offset;
  const std::uint32_t take = remaining < kPage ? remaining : kPage;
  if (!plan.image->source->read(offset, bytes_, take)) return false;
  for (std::uint32_t i = take; i < kPage; ++i) bytes_[i] = 0xFF;
  for (std::uint32_t w = 0; w < kPageWords; ++w) want_[w] = wordOf(bytes_ + 4 * w);
  return true;
}

Result Flasher::readPage(std::uint32_t page_addr) {
  const nrf52::Result r = nrf_.read(page_addr, have_, kPageWords);
  if (!r.ok()) return failed(Step::kScan, r, page_addr);
  counters_.words_read += kPageWords;
  return Result{};
}

// ---------------------------------------------------------------------------- steps 1 and 2

Result Flasher::begin(const Image& image, const BoardProfile& profile, Plan* plan) {
  plan->profile = &profile;
  plan->image = &image;
  counters_ = Counters{};

  // 1. The lines, the port, the chip.
  const board::Outcome acquired = pins_.swdAcquire();
  if (acquired != board::Outcome::kOk) return refused(Step::kPins, board::describe(acquired));
  nrf52::Result r = nrf_.open();
  if (!r.ok()) {
    if (r.status == nrf52::Status::kProtected) {
      return failed(Step::kProtected, r);
    }
    if (r.status == nrf52::Status::kNotNrf52840) return failed(Step::kIdentify, r);
    return failed(Step::kConnect, r);
  }
  const nrf52::Identity& id = nrf_.identity();
  if (id.part != profile.expected_part || id.page_size != profile.page_size || id.flash_bytes < profile.app.end ||
      id.flash_bytes <= profile.settings_page) {
    return refused(Step::kIdentify, "not the part, page size or flash size the profile is for");
  }

  // The flash controller may still be busy - with an erase a previous, interrupted session
  // started, or one the application is doing - and a flash read stalls the bus while it is,
  // which the debug port gives up on long before an erase ends. READY is a register and does not
  // stall: wait for it before the first flash read, bounded by an erase's time.
  r = nrf_.waitFlashReady(nrf52::kErasePageTimeoutMs);
  if (!r.ok()) return failed(Step::kConnect, r);

  // The SoftDevice's own statement of where it ends (flasher.h, kSoftDeviceInfoAddress). An image
  // linked for the profile's start runs nowhere else; a chip without a SoftDevice, or with
  // another one, is refused before anything is written.
  std::uint32_t sd[4] = {};
  r = nrf_.read(kSoftDeviceInfoAddress, sd, 4);
  if (!r.ok()) return failed(Step::kSoftDevice, r);
  if (sd[1] != kSoftDeviceMagic) return refused(Step::kSoftDevice, "no SoftDevice information structure at 0x3000", kSoftDeviceInfoAddress);
  if (sd[2] != profile.softdevice_size) {
    return refused(Step::kSoftDevice, "the SoftDevice does not end where the profile's application region begins", sd[2]);
  }

  // 2. The image. Nothing has been written yet.
  if (image.source == nullptr || image.source->size() == 0) return refused(Step::kImage, "the image is empty");
  plan->size = image.source->size();
  plan->padded = (plan->size + 3u) & ~3u;
  if ((image.load_address % kPage) != 0) {
    return refused(Step::kImage, "the load address is not page-aligned", image.load_address);
  }
  if (!profile.holds(image.load_address, plan->padded)) {
    return refused(Step::kImage, "the image is not wholly inside the profile's application region", image.load_address);
  }
  plan->first_page = image.load_address;
  plan->pages = (plan->padded + kPage - 1) / kPage;
  if (plan->pages > kMaxPages) return refused(Step::kImage, "the image covers more pages than the chip has");
  plan->pages_region = nrf52::FlashRegion{plan->first_page, plan->first_page + plan->pages * kPage};
  plan->is_application = image.load_address == profile.app.begin;
  counters_.pages_total = plan->pages;
  return Result{};
}

// Every page read once and classified; nothing written. An image already on the chip is found
// out here, before the settings page or the core is touched.
Result Flasher::scan(Plan* plan, ProgressSink* progress) {
  plan->any_change = false;
  for (std::uint32_t p = 0; p < plan->pages; ++p) {
    const std::uint32_t addr = plan->first_page + p * kPage;
    if (!loadPage(*plan, addr)) return refused(Step::kImage, "the image's bytes could not be read", addr);
    Result r = readPage(addr);
    if (!r.ok()) return r;
    PageClass k = PageClass::kEqual;
    for (std::uint32_t w = 0; w < kPageWords; ++w) {
      if (have_[w] == want_[w]) continue;
      if (have_[w] == kErased) {
        if (k == PageClass::kEqual) k = PageClass::kInPlace;
      } else {
        k = PageClass::kErase;
        break;
      }
    }
    plan->klass[p] = k;
    if (k != PageClass::kEqual) plan->any_change = true;
    report(progress, Step::kScan, p + 1, plan->pages, addr);
  }
  return Result{};
}

// --------------------------------------------------------------------------------- the pages

// The words of `want` that differ from `have`, written in runs of consecutive words; words that
// are 0xFFFFFFFF in the image are never written (a write of all ones programs nothing and counts
// against nWRITE). With only_blank, a differing word that is not blank is a bug in the caller.
Result Flasher::writeRuns(const nrf52::FlashRegion& region, std::uint32_t page_addr, const std::uint32_t* want,
                          const std::uint32_t* have, bool only_blank) {
  std::uint32_t w = 0;
  while (w < kPageWords) {
    if (want[w] == have[w] || want[w] == kErased) {
      ++w;
      continue;
    }
    std::uint32_t end = w;
    while (end < kPageWords && want[end] != have[end] && want[end] != kErased) {
      if (only_blank && have[end] != kErased) {
        return refused(Step::kPages, "a word that needs an erase was to be written in place", page_addr + 4 * end);
      }
      ++end;
    }
    const nrf52::Result r = nrf_.writeWords(region, page_addr + 4 * w, want + w, end - w);
    if (!r.ok()) return failed(Step::kPages, r, page_addr + 4 * w);
    counters_.words_written += end - w;
    w = end;
  }
  return Result{};
}

// One page: skipped when it already holds the image's bytes, written in place when only blank
// words differ, erased and written otherwise; read back and compared; a mismatch retried once
// with an erase, then a failure naming the first wrong word.
Result Flasher::programPage(Plan* plan, std::uint32_t page_addr, Step step) {
  const std::uint32_t index = (page_addr - plan->first_page) / kPage;
  if (plan->klass[index] == PageClass::kEqual) {
    counters_.pages_skipped++;
    return Result{};
  }
  if (!loadPage(*plan, page_addr)) return refused(Step::kImage, "the image's bytes could not be read", page_addr);
  Result r = readPage(page_addr);
  if (!r.ok()) {
    r.step = step;
    return r;
  }
  bool in_place = true;
  for (std::uint32_t w = 0; w < kPageWords && in_place; ++w) {
    if (have_[w] != want_[w] && have_[w] != kErased) in_place = false;
  }
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (attempt == 1) counters_.retries++;
    if (in_place && attempt == 0) {
      r = writeRuns(plan->pages_region, page_addr, want_, have_, true);
      if (!r.ok()) return r;
      counters_.pages_written_in_place++;
    } else {
      const nrf52::Result e = nrf_.erasePage(plan->pages_region, page_addr);
      if (!e.ok()) return failed(step, e, page_addr);
      counters_.erases++;
      if (attempt == 0) counters_.pages_erased++;
      for (std::uint32_t w = 0; w < kPageWords; ++w) have_[w] = kErased;
      r = writeRuns(plan->pages_region, page_addr, want_, have_, false);
      if (!r.ok()) return r;
    }
    r = readPage(page_addr);
    if (!r.ok()) {
      r.step = step;
      return r;
    }
    std::uint32_t wrong = kPageWords;
    for (std::uint32_t w = 0; w < kPageWords; ++w) {
      if (have_[w] != want_[w]) {
        wrong = w;
        break;
      }
    }
    if (wrong == kPageWords) return Result{};
    if (attempt == 1) {
      nrf52::Result mismatch;
      mismatch.status = nrf52::Status::kOk;
      mismatch.address = page_addr + 4 * wrong;
      mismatch.value = have_[wrong];
      Result f = failed(step, mismatch, page_addr + 4 * wrong);
      f.why = "a page did not verify after an erase and a write, twice";
      return f;
    }
  }
  return Result{};
}

// Steps 4 and 5: the first page's erase, then every other page, then the first page.
Result Flasher::programPages(Plan* plan, ProgressSink* progress) {
  if (plan->klass[0] != PageClass::kEqual) {
    // 4. The vector table page erased before anything else, unless it is blank already (a
    // resumed flash): both bootloaders refuse an application whose first two words are blank,
    // so from here on the radio has no startable application until the last write.
    Result r = readPage(plan->first_page);
    if (!r.ok()) return r;
    bool blank = true;
    for (std::uint32_t w = 0; w < kPageWords && blank; ++w) blank = have_[w] == kErased;
    if (!blank) {
      const nrf52::Result e = nrf_.erasePage(plan->pages_region, plan->first_page);
      if (!e.ok()) return failed(Step::kFirstPageErase, e, plan->first_page);
      counters_.erases++;
      counters_.pages_erased++;
    }
    plan->klass[0] = PageClass::kInPlace;  // the erase is done; what remains is writing
  }
  // 5. The rest, in order.
  for (std::uint32_t p = 1; p < plan->pages; ++p) {
    const std::uint32_t addr = plan->first_page + p * kPage;
    Result r = programPage(plan, addr, Step::kPages);
    if (!r.ok()) return r;
    report(progress, Step::kPages, p, plan->pages, addr);
  }
  // Then the first page. Its classification is kInPlace or kEqual; programPage reads the chip
  // again and writes in place without an erase - the page was erased above or was blank.
  Result r = programPage(plan, plan->first_page, Step::kFirstPage);
  if (!r.ok()) return r;
  report(progress, Step::kFirstPage, plan->pages, plan->pages, plan->first_page);
  return Result{};
}

// Step 6: the whole image read back in order, compared with the source and hashed.
Result Flasher::readBack(Plan* plan, ProgressSink* progress, bool check_digest) {
  image::Sha256 sha;
  for (std::uint32_t p = 0; p < plan->pages; ++p) {
    const std::uint32_t addr = plan->first_page + p * kPage;
    if (!loadPage(*plan, addr)) return refused(Step::kImage, "the image's bytes could not be read", addr);
    Result r = readPage(addr);
    if (!r.ok()) {
      r.step = Step::kReadBack;
      return r;
    }
    for (std::uint32_t w = 0; w < kPageWords; ++w) {
      if (have_[w] != want_[w]) {
        nrf52::Result mismatch;
        mismatch.address = addr + 4 * w;
        mismatch.value = have_[w];
        Result f = failed(Step::kReadBack, mismatch, addr + 4 * w);
        f.why = "the flash does not hold the image's bytes";
        return f;
      }
      bytesOf(have_[w], bytes_ + 4 * w);
    }
    const std::uint32_t offset = addr - plan->first_page;
    const std::uint32_t remaining = plan->size - offset;
    sha.update(bytes_, remaining < kPage ? remaining : kPage);
    report(progress, Step::kReadBack, p + 1, plan->pages, addr);
  }
  std::uint8_t digest[image::kSha256Size];
  sha.finish(digest);
  if (check_digest && plan->image->sha256 != nullptr && !image::sha256Equal(digest, plan->image->sha256)) {
    return refused(Step::kDigest, "the SHA-256 of what was read back is not the one given", plan->first_page);
  }
  return Result{};
}

// --------------------------------------------------------------------- the settings page

// The one function that writes outside the application region: its region is the settings page
// and nothing else, so the nRF52 layer's guard refuses any other address (nrf52.h, FlashRegion).
// The record's words 1..6 go first and word 0 (bank_0 and the CRC) last, so that a power loss in
// the middle leaves bank_0 = 0xFFFF - an erased page to both bootloaders - and never a bank_0
// code with a torn body behind it.
Result Flasher::settingsWrite(const BoardProfile& profile, const settings::Record& record, bool erase_first) {
  const nrf52::FlashRegion page{profile.settings_page, profile.settings_page + kPage};
  std::uint32_t words[settings::kRecordWords];
  settings::encode(record, words);
  if (erase_first) {
    const nrf52::Result e = nrf_.erasePage(page, profile.settings_page);
    if (!e.ok()) return failed(Step::kSettingsValidate, e, profile.settings_page);
    counters_.erases++;
    counters_.settings_erases++;
  }
  nrf52::Result w = nrf_.writeWords(page, profile.settings_page + 4, words + 1, settings::kRecordWords - 1);
  if (!w.ok()) return failed(Step::kSettingsValidate, w, profile.settings_page + 4);
  w = nrf_.writeWords(page, profile.settings_page, words, 1);
  if (!w.ok()) return failed(Step::kSettingsValidate, w, profile.settings_page);
  counters_.words_written += settings::kRecordWords;
  counters_.settings_writes++;
  std::uint32_t back[settings::kRecordWords];
  const nrf52::Result r = nrf_.read(profile.settings_page, back, settings::kRecordWords);
  if (!r.ok()) return failed(Step::kSettingsValidate, r, profile.settings_page);
  counters_.words_read += settings::kRecordWords;
  for (std::size_t i = 0; i < settings::kRecordWords; ++i) {
    if (back[i] != words[i]) {
      nrf52::Result mismatch;
      mismatch.address = profile.settings_page + 4 * static_cast<std::uint32_t>(i);
      mismatch.value = back[i];
      Result f = failed(Step::kSettingsValidate, mismatch, mismatch.address);
      f.why = "the settings record did not verify";
      return f;
    }
  }
  return Result{};
}

Result Flasher::settingsRead(const BoardProfile& profile, settings::Record* out) {
  std::uint32_t words[settings::kRecordWords];
  const nrf52::Result r = nrf_.read(profile.settings_page, words, settings::kRecordWords);
  if (!r.ok()) return failed(Step::kSettingsValidate, r, profile.settings_page);
  counters_.words_read += settings::kRecordWords;
  *out = settings::decode(words);
  return Result{};
}

// A record both bootloaders accept for exactly this image (bootloader_settings.h, rule 2): bank 0
// valid with the CRC check off, or with a CRC-16 over bank_0_size bytes that is the image's.
bool Flasher::settingsAccept(const settings::Record& record, const Plan& plan) {
  if (record.bank_0 != settings::kBankValidApp) return false;
  if (record.bank_0_crc == 0) return true;
  if (record.bank_0_size != plan.size) return false;
  std::uint16_t crc = link::kCrc16Init;
  for (std::uint32_t offset = 0; offset < plan.size; offset += kPage) {
    const std::uint32_t remaining = plan.size - offset;
    const std::uint32_t take = remaining < kPage ? remaining : kPage;
    if (!plan.image->source->read(offset, bytes_, take)) return false;
    crc = link::crc16Update(crc, bytes_, take);
  }
  return crc == record.bank_0_crc;
}

// Before the first erase: "bank 0 holds nothing" (bootloader_settings.h, rule 4), the state the
// bootloader's own DFU puts the page in before it erases bank 0. From an erased page the record
// is written in place; from anything else the page is erased first. Why before step 4 and not
// after step 6: with the old record left in place, the write of the first page would pass through
// states the bootloaders accept - the stack pointer and reset vector written and the rest of the
// vector table blank satisfies rule 3, and any first word satisfies rule 2 with the CRC check
// off, which is what every RAK in the field has - so the only deterministic refusal is bank_0 =
// BANK_INVALID_APP from before the first erase to after the last verified write.
Result Flasher::settingsInvalidate(Plan* plan) {
  settings::Record current;
  Result r = settingsRead(*plan->profile, &current);
  if (!r.ok()) {
    r.step = Step::kSettingsInvalidate;
    return r;
  }
  const settings::Record wanted = settings::invalidRecord();
  if (current.bank_0 == settings::kBankInvalidApp && current.bank_0_crc == 0) return Result{};
  r = settingsWrite(*plan->profile, wanted, !current.blank());
  if (!r.ok()) r.step = Step::kSettingsInvalidate;
  return r;
}

// After the read-back: "bank 0 holds a valid application of this size, CRC checking not used" -
// the record every UF2 and DFU install leaves (bootloader_settings.h). A record the bootloader
// already accepts for exactly this image is left alone, so a repeated flash writes nothing here.
// Between the erase and the write of word 0 the page reads as erased: the newer stock bootloader
// then starts the complete image through rule 3, OTAFIX refuses it and the next flash() finishes
// the job with two flash operations.
Result Flasher::settingsValidate(Plan* plan) {
  settings::Record current;
  Result r = settingsRead(*plan->profile, &current);
  if (!r.ok()) return r;
  if (settingsAccept(current, *plan)) return Result{};
  return settingsWrite(*plan->profile, settings::validRecord(plan->size), !current.blank());
}

// ------------------------------------------------------------------------------- the exits

// Step 7, and the way out of every failure after the halt: a system reset so that the radio runs
// its bootloader (and, when the image is complete, the application) rather than sitting halted;
// the power-up withdrawn; the lines released; the reset recorded for the spacing rule.
Result Flasher::finish(Plan* plan, Result r, bool reset) {
  const bool pointless = r.outcome == Outcome::kFailed &&
                         (r.step == Step::kProtected || r.step == Step::kConnect ||
                          (r.nrf.status == nrf52::Status::kSwd &&
                           (r.nrf.swd == swd::Status::kNoTarget || r.nrf.swd == swd::Status::kLinesLost)));
  if (reset && plan->halted && !pointless) {
    const nrf52::Result reset_result = nrf_.systemReset();
    r.reset_issued = true;
    if (reset_result.status == nrf52::Status::kProtected) {
      // The AIRCR write went through - a closed port fails that write with a FAULT, not with
      // kProtected - and the module shut its port afterwards (task p1-04: build code Fxx).
      r.port_closed_after_reset = true;
    } else if (!reset_result.ok() && r.ok()) {
      r = failed(Step::kReset, reset_result);
      r.reset_issued = true;
    }
  }
  if (r.step != Step::kPins) nrf_.close();
  pins_.swdRelease();
  // Noted after the lines are released, so that the spacing runs from the moment the radio is
  // on its own again.
  if (r.reset_issued) pins_.noteRadioReset();
  counters_.elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - start_ms_);
  r.counters = counters_;
  return r;
}

// ----------------------------------------------------------------------------- the three

Result Flasher::flash(const Image& image, const BoardProfile& profile, ProgressSink* progress) {
  start_ms_ = clock_.nowMs();
  Plan plan;
  Result r = begin(image, profile, &plan);
  if (!r.ok()) return finish(&plan, r, false);
  r = scan(&plan, progress);
  if (!r.ok()) return finish(&plan, r, false);

  // 3. The core halted, and the flash controller given time to finish what the application had
  // in flight (an erase takes 85 ms typical; the bound is the erase's).
  nrf52::Result n = nrf_.halt();
  if (!n.ok()) return finish(&plan, failed(n.status == nrf52::Status::kProtected ? Step::kProtected : Step::kHalt, n), false);
  plan.halted = true;
  n = nrf_.waitFlashReady(nrf52::kErasePageTimeoutMs);
  if (!n.ok()) return finish(&plan, failed(Step::kHalt, n), true);

  if (plan.any_change) {
    if (plan.is_application) {
      r = settingsInvalidate(&plan);
      if (!r.ok()) return finish(&plan, r, true);
    }
    r = programPages(&plan, progress);
    if (!r.ok()) return finish(&plan, r, true);
  } else {
    counters_.pages_skipped = plan.pages;
  }

  // 6. Everything read back, hashed, and the bootloader told.
  r = readBack(&plan, progress, true);
  if (!r.ok()) return finish(&plan, r, true);
  if (plan.is_application) {
    r = settingsValidate(&plan);
    if (!r.ok()) return finish(&plan, r, true);
  }
  // 7.
  r = Result{};
  r.step = Step::kDone;
  return finish(&plan, r, true);
}

// Read-only: the pages compared with the source, the digest checked, the settings record read.
// Nothing is halted, nothing is written: the log of the simulated chip is empty after it.
Result Flasher::verify(const Image& image, const BoardProfile& profile) {
  start_ms_ = clock_.nowMs();
  Plan plan;
  Result r = begin(image, profile, &plan);
  if (!r.ok()) return finish(&plan, r, false);
  r = readBack(&plan, nullptr, true);
  if (!r.ok()) return finish(&plan, r, false);
  r.step = Step::kDone;
  if (plan.is_application) {
    settings::Record current;
    const Result s = settingsRead(profile, &current);
    if (!s.ok()) return finish(&plan, s, false);
    r.settings = current;
    r.settings_valid = settingsAccept(current, plan);
  }
  return finish(&plan, r, false);
}

// The bench's proof that erase, write and verify work on real silicon without risking the
// running firmware: a two-page pattern at the top of the application region, only if those pages
// are blank; written, verified, read back and hashed, erased again, and the radio reset. The
// settings page is not touched: the pattern is not the application.
Result Flasher::flashTest(const BoardProfile& profile, ProgressSink* progress) {
  start_ms_ = clock_.nowMs();
  const std::uint32_t size = kTestPages * kPage;
  const PatternImage pattern(size);
  std::uint8_t digest[image::kSha256Size];
  {
    image::Sha256 sha;
    for (std::uint32_t offset = 0; offset < size; offset += kPage) {
      pattern.read(offset, bytes_, kPage);
      sha.update(bytes_, kPage);
    }
    sha.finish(digest);
  }
  Image image;
  image.source = &pattern;
  image.load_address = profile.app.end - size;
  image.sha256 = digest;

  Plan plan;
  Result r = begin(image, profile, &plan);
  if (!r.ok()) return finish(&plan, r, false);
  // Only blank pages: anything else may be the application's tail.
  for (std::uint32_t p = 0; p < plan.pages; ++p) {
    const std::uint32_t addr = plan.first_page + p * kPage;
    r = readPage(addr);
    if (!r.ok()) return finish(&plan, r, false);
    for (std::uint32_t w = 0; w < kPageWords; ++w) {
      if (have_[w] != kErased) {
        return finish(&plan, refused(Step::kImage, "the last two pages of the application region are not blank: refused", addr + 4 * w), false);
      }
    }
    plan.klass[p] = PageClass::kInPlace;
    report(progress, Step::kScan, p + 1, plan.pages, addr);
  }
  plan.any_change = true;

  nrf52::Result n = nrf_.halt();
  if (!n.ok()) return finish(&plan, failed(n.status == nrf52::Status::kProtected ? Step::kProtected : Step::kHalt, n), false);
  plan.halted = true;
  n = nrf_.waitFlashReady(nrf52::kErasePageTimeoutMs);
  if (!n.ok()) return finish(&plan, failed(Step::kHalt, n), true);

  r = programPages(&plan, progress);
  if (!r.ok()) return finish(&plan, r, true);
  r = readBack(&plan, progress, true);
  if (!r.ok()) return finish(&plan, r, true);
  // Erased again, and blank again.
  for (std::uint32_t p = 0; p < plan.pages; ++p) {
    const std::uint32_t addr = plan.first_page + p * kPage;
    n = nrf_.erasePage(plan.pages_region, addr);
    if (!n.ok()) return finish(&plan, failed(Step::kErase, n, addr), true);
    counters_.erases++;
    r = readPage(addr);
    if (!r.ok()) return finish(&plan, r, true);
    for (std::uint32_t w = 0; w < kPageWords; ++w) {
      if (have_[w] != kErased) {
        nrf52::Result mismatch;
        mismatch.address = addr + 4 * w;
        mismatch.value = have_[w];
        Result f = failed(Step::kErase, mismatch, addr + 4 * w);
        f.why = "a page is not blank after its erase";
        return finish(&plan, f, true);
      }
    }
  }
  r = Result{};
  r.step = Step::kDone;
  return finish(&plan, r, true);
}

}  // namespace flasher
}  // namespace core
