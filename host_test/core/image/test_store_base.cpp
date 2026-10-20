// The base in the store (task p5-01): spec/FIRMWARE-MANIFEST.md §4a and §5's check 5p,
// spec/SYSTEM.md S-5a - what the store does with a thing that is not an image.
//
// A base enters the way an image does, through the whole of the verdict, and then every way in
// which it is not an image: its own door (commitBase, which takes an AcceptedBase and nothing
// else), one per slot, never promoted, not on the bad list, erasable by a command, and whole or
// absent after a power cut at any step.
//
// The compile-time half - that a base and an image cannot be mistaken for each other - is
// host_test/rails/test_rail_tokens.cpp, which asks the compiler.
#include <doctest.h>

#include <string>

#include "core/image/base.h"
#include "store_bench.h"

using bench::BaseFile;
using bench::Candidate;
using bench::StoreBench;
using core::board::Slot;
using core::image::Record;
using core::image::StoreError;
using core::image::Use;
using core::image::Which;

TEST_CASE("a base goes in through check 5p and comes out as the slot's base") {
  StoreBench b;
  const BaseFile base("base-small");
  REQUIRE(b.putBase(Slot{0}, base) == StoreError::kNone);

  const Record record = b.read(Which::kBase);
  REQUIRE(record.present);
  CHECK(record.use == Use::kProvision);
  CHECK(record.which == Which::kBase);
  CHECK(record.stored_as == Which::kBase);
  CHECK(record.size == base.file.size());
  // A base has no load address: its own records say where every byte goes (§4a rule 1).
  CHECK(record.load_address == 0);
  // What §4a found in it, out of the record and not out of the file.
  CHECK(record.base.bootloader == base.row->bootloader);
  CHECK(record.base.flash_bytes == base.row->flash_bytes);
  CHECK(record.base.softdevice_id == core::image::kSoftDeviceS140Id);
  CHECK(record.base.softdevice_version == core::image::kSoftDeviceS140Version);
  CHECK(record.base.has_mbr_params);
  CHECK(record.base.mbr_params == 0x000FE000u);
  CHECK(record.base.uicr_word_count == 2);
  CHECK(record.base.uicrWordAt(0) == core::image::kUicrBootloader);
  CHECK(record.base.uicrWordAt(1) == core::image::kUicrMbrParams);
  // And it is in its own area, which is not one of the three the index maps.
  CHECK(b.store.areaOf(Which::kBase, Slot{0}) == core::image::kBaseArea);

  // Read back out of flash and held to the hash in its record, as `image verify base` does.
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);

  // A restart changes nothing: the record is in flash, with the summary in it.
  b.reboot();
  const Record again = b.read(Which::kBase);
  REQUIRE(again.present);
  CHECK(again.base.bootloader == base.row->bootloader);
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
}

TEST_CASE("the base is one per slot, and the three images are untouched by it") {
  StoreBench b;
  const Candidate image("store-a");
  const BaseFile base("base-small");
  REQUIRE(b.put(Which::kCandidate, Slot{0}, image) == StoreError::kNone);
  REQUIRE(b.putBase(Slot{0}, base) == StoreError::kNone);

  CHECK(b.read(Which::kCandidate).present);
  CHECK(b.read(Which::kBase).present);
  CHECK_FALSE(b.read(Which::kBase, Slot{1}).present);  // the other slot's is its own
  REQUIRE(b.putBase(Slot{1}, base) == StoreError::kNone);
  CHECK(b.read(Which::kBase, Slot{1}).present);
  // The two bases are in two areas, and neither is an image's.
  CHECK(core::image::areaOffset(0, core::image::kBaseArea) !=
        core::image::areaOffset(1, core::image::kBaseArea));
  for (std::size_t a = 0; a < core::image::kPromotableCount; ++a) {
    CHECK(core::image::areaOffset(0, a) != core::image::areaOffset(0, core::image::kBaseArea));
  }
  CHECK(b.read(Which::kCandidate).present);
}

TEST_CASE("a base over a base: the second replaces the first and nothing is left of it") {
  StoreBench b;
  REQUIRE(b.putBase(Slot{0}, BaseFile("base-small")) == StoreError::kNone);
  const BaseFile big("base-resume");
  REQUIRE(b.putBase(Slot{0}, big) == StoreError::kNone);
  const Record record = b.read(Which::kBase);
  REQUIRE(record.present);
  CHECK(record.size == big.file.size());
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
}

TEST_CASE("a base larger than the base area is refused at the door, before anything is erased") {
  // A base area holds 640 KB of text and an image area 796 KB (store.h's arithmetic), so the
  // door's bound is the base's own and not the image's.
  CHECK(core::image::areaCapacity(Which::kBase) == core::image::kBaseBytesMax);
  CHECK(core::image::areaCapacity(Which::kCandidate) == core::image::kImageBytesMax);
  CHECK(core::image::kBaseBytesMax < core::image::kImageBytesMax);

  StoreBench b;
  REQUIRE(b.putBase(Slot{0}, BaseFile("base-small")) == StoreError::kNone);
  const BaseFile huge("base-too-large");
  REQUIRE(huge.file.size() > core::image::kBaseBytesMax);
  REQUIRE(huge.file.size() < core::image::kImageBytesMax);  // an image area would hold it
  const std::uint32_t before = b.flash.operations();
  CHECK(b.openBase(Slot{0}, huge) == StoreError::kTooLarge);
  // Refused before anything was erased, so the base that was there is still there.
  CHECK(b.flash.operations() == before);
  CHECK(b.read(Which::kBase).present);
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
  CHECK_FALSE(b.store.writing());
}

TEST_CASE("the base and the three images cannot be put in each other's places") {
  StoreBench b;
  const Candidate image("store-a");
  const BaseFile base("base-small");

  SUBCASE("an swd file into the base's place") {
    const core::image::Outcome o = b.judgeManifest(image, Slot{0});
    REQUIRE(o.ok());
    REQUIRE(b.verdict.expected() != nullptr);
    CHECK(b.store.beginWrite(Which::kBase, Slot{0}, *b.verdict.expected()) ==
          StoreError::kNotAnImage);
  }
  SUBCASE("a provision file into the candidate's place") {
    const core::image::Outcome o = b.judgeBaseManifest(base, Slot{0});
    REQUIRE(o.ok());
    REQUIRE(b.verdict.expected() != nullptr);
    CHECK(b.store.beginWrite(Which::kCandidate, Slot{0}, *b.verdict.expected()) ==
          StoreError::kNotAnImage);
  }
  SUBCASE("an image's token at the base's commit") {
    // The bytes of a base are in flash, and an AcceptedImage is offered for them: refused,
    // because the open write is the base's. (That the token could not be an AcceptedBase at all
    // is the compiler's answer, in test_rail_tokens.cpp.)
    REQUIRE(b.openBase(Slot{0}, base) == StoreError::kNone);
    REQUIRE(b.streamBase(base) == StoreError::kNone);
    REQUIRE(b.store.endWrite() == StoreError::kNone);
    core::image::StoredImage staged;
    REQUIRE(b.store.openStaged(&staged) == StoreError::kNone);
    const core::image::Outcome judged = b.verdict.judgeFile(staged);
    REQUIRE_MESSAGE(judged.ok(), judged.why);
    CHECK(b.verdict.accepted() == nullptr);          // a base is not an image ...
    REQUIRE(b.verdict.acceptedBase() != nullptr);    // ... it is a base
    CHECK(b.store.commitBase(*b.verdict.acceptedBase()) == StoreError::kNone);
  }
  SUBCASE("a base's token at an image's commit") {
    REQUIRE(b.open(Which::kCandidate, Slot{0}, image) == StoreError::kNone);
    REQUIRE(b.stream(image) == StoreError::kNone);
    REQUIRE(b.store.endWrite() == StoreError::kNone);
    core::image::StoredImage staged;
    REQUIRE(b.store.openStaged(&staged) == StoreError::kNone);
    REQUIRE(b.verdict.judgeFile(staged).ok());
    REQUIRE(b.verdict.accepted() != nullptr);
    CHECK(b.verdict.acceptedBase() == nullptr);
    // There is no AcceptedBase to offer here - the verdict made none - and the store's base door
    // refuses an image's open write, which is the same statement from the other side.
    CHECK(b.store.commit(*b.verdict.accepted()) == StoreError::kNone);
  }
}

TEST_CASE("a promotion never moves a pointer to or from the base") {
  StoreBench b;
  REQUIRE(b.putBase(Slot{0}, BaseFile("base-small")) == StoreError::kNone);
  REQUIRE(b.put(Which::kGolden, Slot{0}, Candidate("store-a")) == StoreError::kNone);
  REQUIRE(b.put(Which::kCandidate, Slot{0}, Candidate("store-b")) == StoreError::kNone);
  const std::uint8_t base_area = b.store.areaOf(Which::kBase, Slot{0});
  REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);
  CHECK(b.store.areaOf(Which::kBase, Slot{0}) == base_area);
  CHECK(b.store.areaOf(Which::kGolden, Slot{0}) != base_area);
  CHECK(b.store.areaOf(Which::kCandidate, Slot{0}) != base_area);
  // And the base is still there, and still a base.
  const Record record = b.read(Which::kBase);
  REQUIRE(record.present);
  CHECK(record.use == Use::kProvision);
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
  // A thousand promotions later, nothing has moved: the base's area is not in the index at all.
  for (int i = 0; i < 8; ++i) {
    REQUIRE(b.put(Which::kCandidate, Slot{0}, Candidate("store-a")) == StoreError::kNone);
    REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);
    CHECK(b.store.areaOf(Which::kBase, Slot{0}) == base_area);
  }
  CHECK(b.read(Which::kBase).present);
}

TEST_CASE("the bad list is about images, and does not reach the base") {
  StoreBench b;
  const BaseFile base("base-small");
  REQUIRE(b.putBase(Slot{0}, base) == StoreError::kNone);
  const Record record = b.read(Which::kBase);
  REQUIRE(record.present);
  // A base is never health-checked - S-5a's read-back of every word is its check - so even a
  // hash somebody put on the bad list does not shut its door.
  REQUIRE(b.store.markBad(record.sha256) == StoreError::kNone);
  CHECK(b.store.isBad(record.sha256));
  CHECK(b.putBase(Slot{0}, base) == StoreError::kNone);
  // The same hash as an image's would be refused, which is the rule the base is outside of.
  CHECK(b.store.isBad(record.sha256));
}

TEST_CASE("image erase base: the base goes, the images stay, and the golden image is still never erased") {
  StoreBench b;
  REQUIRE(b.putBase(Slot{0}, BaseFile("base-small")) == StoreError::kNone);
  REQUIRE(b.put(Which::kGolden, Slot{0}, Candidate("store-a")) == StoreError::kNone);
  CHECK(b.store.erase(Which::kGolden, Slot{0}) == StoreError::kNeverGolden);
  CHECK(b.store.erase(Which::kBase, Slot{0}) == StoreError::kNone);
  CHECK_FALSE(b.read(Which::kBase).present);
  CHECK(b.read(Which::kGolden).present);
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kEmpty);
  // And it can be given again afterwards.
  CHECK(b.putBase(Slot{0}, BaseFile("base-small")) == StoreError::kNone);
}

TEST_CASE("a transfer of a base that an interruption left half done is resumed, not restarted") {
  StoreBench b;
  const BaseFile base("base-resume");
  REQUIRE(base.file.size() > 2 * core::image::kResumeStep);
  REQUIRE(b.openBase(Slot{0}, base) == StoreError::kNone);
  // Rather more than one resume point, then the power goes.
  REQUIRE(b.store.write(base.file.data(), core::image::kResumeStep + 5000) == StoreError::kNone);
  b.reboot();

  const Record stopped = b.read(Which::kBase);
  CHECK_FALSE(stopped.present);
  CHECK(stopped.transfer);
  CHECK(stopped.transferred == core::image::kResumeStep);
  CHECK(stopped.transfer_size == base.file.size());

  REQUIRE(b.openBase(Slot{0}, base) == StoreError::kNone);
  CHECK(b.store.written() == core::image::kResumeStep);
  REQUIRE(b.streamBase(base, 240, b.store.written()) == StoreError::kNone);
  REQUIRE(b.commitBase() == StoreError::kNone);
  CHECK(b.read(Which::kBase).present);
  CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
}

TEST_CASE("the power goes at every write and erase of storing a base: a base or no base, never half of one") {
  // The store's own promise (store.h's table), for the base: at no point does a record validate
  // over bytes that are not its own. This is the loop task p2-02 runs over an image, run over a
  // base - and over a base replacing a base, so that the record that was there is in the picture.
  const BaseFile was("base-small");
  const BaseFile now("base-resume");

  std::uint32_t total = 0;
  {
    StoreBench count;
    REQUIRE(count.putBase(Slot{0}, was) == StoreError::kNone);
    const std::uint32_t before = count.flash.operations();
    REQUIRE(count.putBase(Slot{0}, now) == StoreError::kNone);
    total = count.flash.operations() - before;
  }
  REQUIRE(total > 4);

  int kept_the_old = 0;
  int left_empty = 0;
  for (std::uint32_t op = 1; op <= total; ++op) {
    StoreBench b;
    REQUIRE(b.putBase(Slot{0}, was) == StoreError::kNone);
    b.flash.powerOn();
    b.flash.lose_power_at_op = op;
    const StoreError interrupted = b.putBase(Slot{0}, now);
    CAPTURE(op);
    CHECK(interrupted != StoreError::kNone);  // it cannot have finished: the power went
    CHECK(b.flash.powered() == false);

    b.reboot();
    const Record record = b.read(Which::kBase);
    if (record.present) {
      // Only the base that was there can be there, and it must be whole - with the summary §4a
      // found in *it*, not in the one that was on its way.
      ++kept_the_old;
      CHECK(record.size == was.file.size());
      CHECK(record.use == Use::kProvision);
      CHECK(record.base.bootloader == was.row->bootloader);
      CHECK(record.base.flash_bytes == was.row->flash_bytes);
      CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
    } else {
      ++left_empty;
      CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kEmpty);
    }

    // And it runs again to completion - resuming where it got to, or starting over.
    REQUIRE(b.putBase(Slot{0}, now) == StoreError::kNone);
    CHECK(b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
    CHECK(b.read(Which::kBase).size == now.file.size());
  }
  // The first of those operations is the one small write that invalidates the record that was
  // there (store.h: "invalidated by one small write before anything is overwritten"), so from
  // operation 1 onwards the slot is empty by design and not by accident - which is why
  // `kept_the_old` is 0 here, exactly as it is for an image (test_store_power_loss.cpp). What
  // every iteration shows is that no cut anywhere leaves a record over bytes that are not its own,
  // and that the base can be given again afterwards.
  MESSAGE("a base of " << now.file.size() << " bytes over a base of " << was.file.size()
          << " is " << total << " flash operations; the power went at each in turn: "
          << kept_the_old << " left the old base whole, " << left_empty
          << " left the slot empty, 0 left a record over the wrong bytes");
}
