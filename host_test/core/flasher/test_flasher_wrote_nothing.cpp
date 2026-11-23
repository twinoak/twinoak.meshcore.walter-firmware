// **Does a flash say whether it changed the chip?** (task p18-05, deliverable 1.)
//
// The answer has to be worked out from what the flasher *did* and not from the name of the step
// it stopped at, because the thing above it - the radio's update machine - decides whether to put
// the golden image back on it, and a wrong answer there is nine minutes off the mesh for nothing
// (23 November, meshmaster.dk job 41878). So `Result::wrote_nothing` is read off the counters:
// no page erased and no word written, of the image's pages or of the bootloader's settings page.
// `core_halted` is the other half - a flash that wrote nothing may still have stopped the core,
// and then the image has to be started again before anything else happens.
//
// This file holds that over every exit of all three entry points, and the one sentence the update
// machine reads off a failure besides: whose fault it was.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/flasher/flasher.h"
#include "flasher_bench.h"

using sim::digestOf;
using sim::fl::Image;
using sim::fl::MemoryImage;
using sim::fl::Outcome;
using sim::fl::Result;
using sim::fl::Step;
using FailAt = sim::Nrf52Chip::FailFlashAt;

namespace {

struct Bench : sim::FlasherBench {
  std::vector<std::uint8_t> image = sim::makeImage(2, false);
  std::vector<std::uint8_t> sha = digestOf(image);
  MemoryImage source{image.data(), static_cast<std::uint32_t>(image.size())};

  Image whole() {
    allow(0x26000, static_cast<std::uint32_t>(image.size()));
    return Image{&source, 0x26000, sha.data()};
  }
};

}  // namespace

TEST_CASE("a flash that worked changed the chip and halted the core, and says both") {
  Bench b;
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  REQUIRE(r.ok());
  CHECK_FALSE(r.wrote_nothing);
  CHECK(r.core_halted);
  CHECK(r.counters.erases > 0);
  CHECK(r.counters.words_written > 0);
}

TEST_CASE("a flash of an image the chip already holds wrote nothing, and says so") {
  Bench b;
  // The bytes and a settings record the bootloader accepts: the scan finds every page equal, the
  // read-back passes, and the settings record is left alone - so not one word is written, and the
  // result says the chip is as this flash found it even though it succeeded.
  b.install(b.image, 0x26000);
  b.setSettings(sim::fl::bootloader_settings::validRecord(static_cast<std::uint32_t>(b.image.size())));
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  REQUIRE(r.ok());
  CHECK(r.wrote_nothing);
  CHECK(r.core_halted);  // the core is halted before the settings page is read, whatever follows
  CHECK(sim::nothingWritten(b.chip));
}

TEST_CASE("a verify writes nothing and halts nothing") {
  Bench b;
  b.install(b.image, 0x26000);
  b.setSettings(sim::fl::bootloader_settings::validRecord(static_cast<std::uint32_t>(b.image.size())));
  const Result r = b.flasher.verify(b.whole(), b.profile);
  REQUIRE(r.ok());
  CHECK(r.wrote_nothing);
  CHECK_FALSE(r.core_halted);
  CHECK(sim::nothingWritten(b.chip));
}

TEST_CASE("every refusal before the first write says the chip is as it found it") {
  SUBCASE("an image that is empty") {
    Bench b;
    const MemoryImage nothing(b.image.data(), 0);
    const Image img{&nothing, 0x26000, b.sha.data()};
    const Result r = b.flasher.flash(img, b.profile, nullptr);
    CHECK(r.outcome == Outcome::kRefused);
    CHECK(r.step == Step::kImage);
    CHECK(r.wrote_nothing);
    CHECK_FALSE(r.core_halted);
  }
  SUBCASE("a load address that is not page-aligned") {
    Bench b;
    b.allow(0x26000, static_cast<std::uint32_t>(b.image.size()));
    const Image img{&b.source, 0x26001, b.sha.data()};
    const Result r = b.flasher.flash(img, b.profile, nullptr);
    CHECK(r.outcome == Outcome::kRefused);
    CHECK(r.wrote_nothing);
    CHECK_FALSE(r.core_halted);
  }
  SUBCASE("a chip that is protected") {
    Bench b;
    b.chip.approtect = true;
    const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
    CHECK_FALSE(r.ok());
    CHECK(r.step == Step::kProtected);
    CHECK(r.wrote_nothing);
    CHECK_FALSE(r.core_halted);
  }
  SUBCASE("a target that answers nothing") {
    Bench b;
    b.target.silent = true;
    const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
    CHECK_FALSE(r.ok());
    CHECK(r.step == Step::kConnect);
    CHECK(r.wrote_nothing);
    CHECK_FALSE(r.core_halted);
  }
}

TEST_CASE("every way out before the first write, through the simulated chip's own knobs") {
  struct Case {
    const char* name;
    FailAt where;
    Step step;
    bool halted;
  };
  const Case cases[] = {
      {"the power-up", FailAt::kPowerUp, Step::kConnect, false},
      {"the control port", FailAt::kControlPort, Step::kConnect, false},
      {"the protection", FailAt::kProtection, Step::kProtected, false},
      {"the memory port", FailAt::kMemoryPort, Step::kConnect, false},
      {"the identity", FailAt::kIdentity, Step::kIdentify, false},
      {"the flash controller", FailAt::kFlashReady, Step::kConnect, false},
      {"the SoftDevice", FailAt::kSoftDevice, Step::kSoftDevice, false},
      {"a read that stalls while the application runs", FailAt::kScanRead, Step::kScan, false},
      // The one that wrote nothing and **did** stop the core.
      {"the flash controller after the halt", FailAt::kAfterTheHalt, Step::kHalt, true},
  };
  for (const Case& one : cases) {
    CAPTURE(one.name);
    Bench b;
    b.chip.failFlashAt(one.where, 0);  // every flash: this bench makes one
    const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
    CHECK_FALSE(r.ok());
    CHECK(r.step == one.step);
    CHECK(r.wrote_nothing);
    CHECK(r.core_halted == one.halted);
    CHECK(sim::nothingWritten(b.chip));
    // The chip is left startable either way: nothing was erased, so whatever it held it holds.
    CHECK(b.chip.counters().erases == 0);
    CHECK(b.chip.counters().writes == 0);
    MESSAGE(std::string(one.name) << ": " << std::string(sim::fl::name(r.step)) << ", "
                                  << std::string(core::nrf52::name(r.nrf.status)));
  }
}

TEST_CASE("a page that will not take: the chip was changed, and the fault is not the image's") {
  Bench b;
  b.chip.failFlashAt(FailAt::kPageWillNotTake, 0);
  const Result r = b.flasher.flash(b.whole(), b.profile, nullptr);
  CHECK_FALSE(r.ok());
  CHECK_FALSE(r.wrote_nothing);
  CHECK(r.core_halted);
  CHECK(b.chip.counters().writes > 0);
  CHECK_FALSE(sim::fl::blamesTheImage(r));
}

TEST_CASE("a digest that is not the one given: the chip was changed, and the fault is the image's") {
  Bench b;
  // The bytes go in and read back exactly, and the SHA-256 the caller gave is not theirs.
  std::vector<std::uint8_t> wrong = b.sha;
  wrong[0] ^= 0x40;
  b.allow(0x26000, static_cast<std::uint32_t>(b.image.size()));
  const Image img{&b.source, 0x26000, wrong.data()};
  const Result r = b.flasher.flash(img, b.profile, nullptr);
  CHECK_FALSE(r.ok());
  CHECK(r.step == Step::kDigest);
  CHECK_FALSE(r.wrote_nothing);
  CHECK(sim::fl::blamesTheImage(r));
}

TEST_CASE("the flasher's steps have a word each, and no two are the same") {
  std::vector<std::string> words;
  for (unsigned i = 0; i <= static_cast<unsigned>(Step::kDone); ++i) {
    const std::string word = sim::fl::name(static_cast<Step>(i));
    CHECK(word != "?");
    for (const std::string& seen : words) CHECK(seen != word);
    words.push_back(word);
  }
  // And so do the two layers under it, which a failed flash names beside the step.
  for (unsigned i = 0; i <= static_cast<unsigned>(core::nrf52::Status::kSlotBusy); ++i) {
    CHECK(std::string(core::nrf52::name(static_cast<core::nrf52::Status>(i))) != "?");
  }
  for (unsigned i = 0; i <= static_cast<unsigned>(core::swd::Status::kNoMemAp); ++i) {
    CHECK(std::string(core::swd::name(static_cast<core::swd::Status>(i))) != "?");
  }
}
