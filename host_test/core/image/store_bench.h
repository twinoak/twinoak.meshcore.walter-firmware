// The image store's bench (task p2-02): a simulated flash the size of the layout, a wall clock a
// test can set, a trust table holding the specification's test key, one verdict and one store.
//
// Everything here goes through the whole of task p2-01's verdict, because that is the only way in:
// beginWrite() takes an ExpectedFile and commit() an AcceptedImage, and neither has a public
// constructor. So `put()` below is what the USB transfer and - in phase 3 - a download do, in the
// same order: judge the manifest and its signature, open the write, stream the bytes, judge the
// file as it now lies in flash, commit.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/flasher/board_profile.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/verdict.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_wall_clock.h"
#include "fixtures/manifest_files.h"
#include "sim/sim_flash.h"

namespace bench {

using core::board::Slot;
using core::image::AcceptedImage;
using core::image::ImageVerdict;
using core::image::Record;
using core::image::SlotNeeds;
using core::image::Store;
using core::image::StoreError;
using core::image::Use;
using core::image::Which;

// The fixture row for an image by name, with a sentence when there is none.
inline const fixtures::StoreImage& imageNamed(const char* name) {
  for (std::size_t i = 0; i < fixtures::kStoreImageCount; ++i) {
    if (std::strcmp(fixtures::kStoreImages[i].name, name) == 0) return fixtures::kStoreImages[i];
  }
  FAIL("no generated store image named " << name);
  return fixtures::kStoreImages[0];
}

// One image, its manifest and the manifest's signature, read off disk once.
struct Candidate {
  std::vector<std::uint8_t> manifest;
  std::string signature;
  std::vector<std::uint8_t> file;
  const char* role = "";
  const char* band = "";
  const char* version = "";

  explicit Candidate(const char* name) {
    const fixtures::StoreImage& row = imageNamed(name);
    manifest = fixtures::readGenerated(row.manifest);
    signature = fixtures::asText(fixtures::readGenerated(row.sig));
    file = fixtures::readGenerated(row.file);
    REQUIRE(file.size() == row.size);
    role = row.role;
    band = row.band;
    version = row.version;
  }

  SlotNeeds needs(Slot slot) const {
    SlotNeeds n;
    n.slot = slot;
    n.role = role;
    n.board = core::flasher::kRak4630Single.name;
    n.band = band;
    return n;
  }
};

struct StoreBench {
  // A region exactly the size of the layout: a test that reaches past it is reaching past the
  // partition too.
  sim::SimFlash flash{core::image::kStoreBytes};
  fakes::FakeWallClock wall;
  fakes::FakeClock clock;
  fixtures::TestTrust trust;
  core::image::ThisUnit unit;
  ImageVerdict verdict;
  Store store;

  StoreBench() : unit(makeUnit()), verdict(trust.table, unit), store(flash, wall) {
    REQUIRE(store.begin() == StoreError::kNone);
  }

  // What this build is, as far as a manifest's `requires` can ask: the fixture manifests require
  // walter 0.3.0 and link major 1 (spec/fixtures/manifest/manifest-repeater.json), and the host
  // build's own version is 0.0.0-dev, which meets nothing - so the tests say what a released
  // Walter would be rather than what the test binary is.
  static core::image::ThisUnit makeUnit() {
    core::image::ThisUnit u;
    u.version = "0.3.0+abc1234";
    u.link_majors[0] = 1;
    u.link_major_count = 1;
    return u;
  }

  // A Walter that restarted: the power back, and the store reading the index again. Whatever
  // write was open is gone from memory - only flash remembers, which is the point.
  void reboot() {
    flash.powerOn();
    REQUIRE(store.begin() == StoreError::kNone);
  }

  // The first half of the way in: checks 1, 2, 3 and the manifest's half of 6. The dual
  // adapter's second position has no board profile in this build (core/flasher/board_profile.h
  // says why), so where a test puts an image in slot 1 - to show that the store's areas are laid
  // out for two radios from the start - the single adapter's profile stands in. On a unit as
  // built, `image receive @1` is refused by the console before it gets this far.
  core::image::Outcome judgeManifest(const Candidate& image, Slot slot) {
    const core::flasher::BoardProfile* profile = core::flasher::profileForSlot(slot);
    if (profile == nullptr) profile = &core::flasher::kRak4630Single;
    return verdict.judgeManifest(image.manifest.data(), image.manifest.size(),
                                 image.signature.data(), image.signature.size(),
                                 image.needs(slot), Use::kSwd, profile);
  }

  // Opens the write with the verdict's promise. The caller then writes the bytes.
  StoreError open(Which which, Slot slot, const Candidate& image) {
    const core::image::Outcome o = judgeManifest(image, slot);
    REQUIRE_MESSAGE(o.ok(), core::image::describe(o.failed) << ": " << o.why);
    REQUIRE(verdict.expected() != nullptr);
    return store.beginWrite(which, slot, *verdict.expected());
  }

  // The bytes, in pieces of `chunk` from `from` (a resumed write starts where written() says).
  StoreError stream(const Candidate& image, std::uint32_t chunk = 240, std::uint32_t from = 0) {
    for (std::uint32_t at = from; at < image.file.size();) {
      const std::uint32_t left = static_cast<std::uint32_t>(image.file.size()) - at;
      const std::uint32_t n = left < chunk ? left : chunk;
      const StoreError e = store.write(image.file.data() + at, n);
      if (e != StoreError::kNone) return e;
      at += n;
    }
    return StoreError::kNone;
  }

  // Checks 4, 5 and the file's half of 6 over the bytes as they lie in flash, then the record.
  StoreError commit() {
    const StoreError ended = store.endWrite();
    if (ended != StoreError::kNone) return ended;
    core::image::StoredImage staged;
    const StoreError opened = store.openStaged(&staged);
    if (opened != StoreError::kNone) return opened;
    const core::image::Outcome o = verdict.judgeFile(staged);
    if (!o.ok()) {
      MESSAGE("the file was refused at " << core::image::describe(o.failed) << ": " << o.why);
      return StoreError::kHashMismatch;
    }
    REQUIRE(verdict.accepted() != nullptr);
    return store.commit(*verdict.accepted());
  }

  // Open, stream, commit - the whole way in, as the USB transfer and a download do it. The
  // streaming starts where written() says, so a put() of a file an interruption left half done
  // continues it instead of sending the first half again.
  StoreError put(Which which, Slot slot, const Candidate& image, std::uint32_t chunk = 240) {
    const StoreError opened = open(which, slot, image);
    if (opened != StoreError::kNone) return opened;
    const StoreError wrote = stream(image, chunk, store.written());
    if (wrote != StoreError::kNone) return wrote;
    return commit();
  }

  Record read(Which which, Slot slot = Slot{0}) const {
    Record record;
    REQUIRE(store.read(which, slot, &record) == StoreError::kNone);
    return record;
  }
};

}  // namespace bench
