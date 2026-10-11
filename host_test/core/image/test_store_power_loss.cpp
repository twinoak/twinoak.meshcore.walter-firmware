// The image store with the power going at every flash operation (task p2-02). Phase 2's rule:
// "a test in which nothing goes wrong proves very little here".
//
// Two loops, each running the whole operation once per flash write and erase it performs, with
// the power cut at that one - leaving it half done, as sim/sim_flash.h describes - then the
// Walter restarting and the store reading its flash again:
//
//   a store      afterwards the slot holds the old valid record or none, never a record that
//                validates over bytes that are not its own; and the store can be run again to
//                completion.
//   a promotion  at every point there is exactly one golden image, the old one or the new one.
//
// A third loop does the same over an image large enough to cross two of the store's resume
// points, so that the progress records are among the operations the power goes at.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/image/store.h"
#include "store_bench.h"

using bench::Candidate;
using bench::Slot;
using bench::StoreBench;
using bench::StoreError;
using bench::Which;
using core::image::Record;

namespace {

// How many writes and erases one store of `image` into the candidate slot takes, with the given
// slot already holding `was` - counted by doing it once with the power on.
std::uint32_t operationsOfAStore(const Candidate& was, const Candidate& image) {
  StoreBench b;
  REQUIRE(b.put(Which::kCandidate, Slot{0}, was) == StoreError::kNone);
  const std::uint32_t before = b.flash.operations();
  REQUIRE(b.put(Which::kCandidate, Slot{0}, image) == StoreError::kNone);
  return b.flash.operations() - before;
}

// The loop itself, over one pair of images.
void storeWithThePowerGoingEverywhere(const char* old_name, const char* new_name) {
  const Candidate was(old_name);
  const Candidate now(new_name);
  const std::uint32_t total = operationsOfAStore(was, now);
  REQUIRE(total > 4);

  int kept_the_old = 0;
  int left_empty = 0;
  for (std::uint32_t op = 1; op <= total; ++op) {
    StoreBench b;
    REQUIRE(b.put(Which::kCandidate, Slot{0}, was) == StoreError::kNone);
    // From here the operations are counted from 1 again, and the power goes at the opth.
    b.flash.powerOn();
    b.flash.lose_power_at_op = op;
    const StoreError interrupted = b.put(Which::kCandidate, Slot{0}, now);
    CAPTURE(op);
    CHECK(interrupted != StoreError::kNone);  // it cannot have finished: the power went
    CHECK(b.flash.powered() == false);

    // The Walter comes back and reads its flash.
    b.reboot();
    const Record record = b.read(Which::kCandidate);
    if (record.present) {
      // Only the old image can be there, and it must be whole: a record that validated over the
      // new image's half-written bytes is the failure this whole design is against.
      ++kept_the_old;
      CHECK(std::string(record.version) == was.version);
      CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
    } else {
      ++left_empty;
      CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kEmpty);
    }

    // And the operation runs again to completion - resuming where it got to, or starting over.
    REQUIRE(b.put(Which::kCandidate, Slot{0}, now) == StoreError::kNone);
    CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
    CHECK(std::string(b.read(Which::kCandidate).version) == now.version);
  }
  // The first of those operations is the one small write that invalidates the record that was
  // there (store.h: "invalidated by one small write before anything is overwritten"), so from
  // operation 1 onwards the slot is empty by design and not by accident; what matters, and what
  // every iteration shows, is that no cut anywhere leaves a record that validates over bytes
  // that are not its own.
  MESSAGE("a store of " << now.file.size() << " bytes over an image of " << was.file.size()
                        << " is " << total << " flash operations; the power went at each in turn: "
                        << kept_the_old << " left the old image whole, " << left_empty
                        << " left the slot empty, 0 left a record over the wrong bytes");
}

}  // namespace

TEST_CASE("power loss at every write and erase of a store: the old image or none, and it can be redone") {
  storeWithThePowerGoingEverywhere("store-a", "store-b");
}

TEST_CASE("the same with an image that crosses two resume points, so a progress record is among them") {
  storeWithThePowerGoingEverywhere("store-exact", "store-resume");
}

TEST_CASE("power loss at every write and erase of a promotion: exactly one golden image throughout") {
  const Candidate first("store-a");
  const Candidate next("store-b");

  // How many operations a promotion takes.
  std::uint32_t total = 0;
  {
    StoreBench b;
    REQUIRE(b.put(Which::kGolden, Slot{0}, first) == StoreError::kNone);
    REQUIRE(b.put(Which::kCandidate, Slot{0}, next) == StoreError::kNone);
    const std::uint32_t before = b.flash.operations();
    REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);
    total = b.flash.operations() - before;
  }
  REQUIRE(total >= 4);

  int stayed_old = 0;
  int became_new = 0;
  for (std::uint32_t op = 1; op <= total; ++op) {
    CAPTURE(op);
    StoreBench b;
    REQUIRE(b.put(Which::kGolden, Slot{0}, first) == StoreError::kNone);
    REQUIRE(b.put(Which::kCandidate, Slot{0}, next) == StoreError::kNone);
    b.flash.powerOn();
    b.flash.lose_power_at_op = op;
    const StoreError interrupted = b.store.promote(Slot{0});
    CHECK(interrupted != StoreError::kNone);
    CHECK(b.flash.powered() == false);

    b.reboot();  // begin() finishes a promotion that was caught in the middle
    const Record golden = b.read(Which::kGolden);
    REQUIRE_MESSAGE(golden.present, "there is never a moment with no golden image");
    CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);
    const std::string version(golden.version);
    const Record candidate = b.read(Which::kCandidate);
    if (version == first.version) {
      // The promotion did not happen: the candidate is still there and it can be promoted now.
      ++stayed_old;
      REQUIRE(candidate.present);
      CHECK(std::string(candidate.version) == next.version);
      REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);
    } else {
      // It did: the new image is golden and the candidate slot is empty - the old golden image
      // is not something to try.
      ++became_new;
      CHECK(version == next.version);
      CHECK(candidate.present == false);
    }
    const Record after = b.read(Which::kGolden);
    REQUIRE(after.present);
    CHECK(std::string(after.version) == next.version);
    CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);
    CHECK(b.read(Which::kCandidate).present == false);
  }
  MESSAGE("a promotion is " << total << " flash operations; the power went at each in turn: "
                            << stayed_old << " left the old golden image, " << became_new
                            << " had already moved to the new one, 0 left none");
}

TEST_CASE("power loss while the bad list is written: the list before it, or the list after it") {
  const Candidate a("store-a");
  std::uint8_t hash[32] = {};
  hash[0] = 0x5A;

  std::uint32_t total = 0;
  {
    StoreBench b;
    const std::uint32_t before = b.flash.operations();
    REQUIRE(b.store.markBad(hash) == StoreError::kNone);
    total = b.flash.operations() - before;
  }
  for (std::uint32_t op = 1; op <= total; ++op) {
    CAPTURE(op);
    StoreBench b;
    std::uint8_t earlier[32] = {};
    earlier[0] = 0x11;
    REQUIRE(b.store.markBad(earlier) == StoreError::kNone);
    REQUIRE(b.put(Which::kGolden, Slot{0}, a) == StoreError::kNone);
    b.flash.powerOn();
    b.flash.lose_power_at_op = op;
    CHECK(b.store.markBad(hash) != StoreError::kNone);
    b.reboot();
    // The index is one of the two: with the new hash or without it - never a torn list, and
    // never without the hash that was already there.
    CHECK(b.store.isBad(earlier));
    CHECK(b.store.badCount() >= 1);
    // And the golden image is still where it was: the index moves pointers, not images.
    CHECK(b.read(Which::kGolden).present);
    CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);
    REQUIRE(b.store.markBad(hash) == StoreError::kNone);
    CHECK(b.store.isBad(hash));
    CHECK(b.store.isBad(earlier));
  }
}
