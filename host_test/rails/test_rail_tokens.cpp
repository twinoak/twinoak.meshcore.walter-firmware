// Rail R5 (task p3-06): **nothing reaches the store or a radio without the six checks** of
// spec/FIRMWARE-MANIFEST.md §5.
//
// It is held by the *type*, not by a check somebody remembers to make: `ExpectedFile` and
// `AcceptedImage` have no public constructor and exactly one friend, `ImageVerdict`, and they are
// the only arguments the store's two write doors take - `beginWrite(which, slot, ExpectedFile)`
// and `commit(AcceptedImage)`. So a caller with no signed manifest has nothing to hand them, and
// there is no second way in.
//
// Task p5-01 added a third token and a third door, for the board's base: `AcceptedBase` and
// `commitBase(AcceptedBase)`, the token of checks 1, 2, 3, 4 and 5p. The same statement holds of
// it, and one more: **a base and an image cannot be mistaken for each other**. There is no
// conversion either way, neither door takes the other's token, and the compiler says so below.
//
// This file is that statement as code: the forgeries are refused at compile time (and the
// static_asserts below say so in their own words), the two doors are shown taking only the real
// thing, and the one way to make a token is shown making one. The grep half - that nothing else
// in the firmware befriends ImageVerdict, and that the doors' signatures have not changed - is
// tools/check_rails.py.
#include <doctest.h>

#include <type_traits>

#include "core/image/store.h"
#include "core/image/verdict.h"
#include "core/image/store_bench.h"

namespace img = core::image;

// -- the forgeries, refused by the compiler -----------------------------------------------------
//
// Each of these is a way somebody could try to make a token without a manifest. They are not
// commented-out code: they are the compiler's own answer, asked through <type_traits>, so that a
// change to verdict.h that made one possible fails this file at compile time rather than letting
// a sector be erased on a promise nobody signed.

static_assert(!std::is_default_constructible<img::ExpectedFile>::value,
              "ExpectedFile can be default-constructed: the store's first door could be opened "
              "with a token nobody signed a manifest for (spec/FIRMWARE-MANIFEST.md §5)");
static_assert(!std::is_default_constructible<img::AcceptedImage>::value,
              "AcceptedImage can be default-constructed: a record could be made valid without the "
              "six checks having run over the bytes");
// Nor from anything else: no converting constructor, no aggregate initialisation (both have
// private members and a private constructor, so neither is an aggregate).
static_assert(!std::is_constructible<img::ExpectedFile, core::board::Slot>::value,
              "ExpectedFile can be built from a slot");
static_assert(!std::is_constructible<img::AcceptedImage, const img::ExpectedFile&>::value,
              "AcceptedImage can be built from an ExpectedFile: a promise would become a verdict");
static_assert(!std::is_aggregate<img::ExpectedFile>::value, "ExpectedFile is an aggregate");
static_assert(!std::is_aggregate<img::AcceptedImage>::value, "AcceptedImage is an aggregate");

// The base's token (task p5-01), the same way: no constructor of anybody else's, and - the point
// of there being a second token at all - no road between it and an image's.
static_assert(!std::is_default_constructible<img::AcceptedBase>::value,
              "AcceptedBase can be default-constructed: a base could be written to a radio "
              "without check 5p having run over it (spec/FIRMWARE-MANIFEST.md §5, §4a)");
static_assert(!std::is_aggregate<img::AcceptedBase>::value, "AcceptedBase is an aggregate");
static_assert(!std::is_constructible<img::AcceptedBase, const img::AcceptedImage&>::value,
              "an AcceptedBase can be built from an AcceptedImage: an image could be stored as "
              "the board's base, and written to a chip's MBR and UICR by `rak provision`");
static_assert(!std::is_constructible<img::AcceptedImage, const img::AcceptedBase&>::value,
              "an AcceptedImage can be built from an AcceptedBase: a base could be flashed into "
              "a radio's application region by an update");
static_assert(!std::is_convertible<img::AcceptedBase, img::AcceptedImage>::value,
              "an AcceptedBase converts to an AcceptedImage");
static_assert(!std::is_convertible<img::AcceptedImage, img::AcceptedBase>::value,
              "an AcceptedImage converts to an AcceptedBase");
static_assert(!std::is_constructible<img::AcceptedBase, const img::ExpectedFile&>::value,
              "an AcceptedBase can be built from an ExpectedFile: a promise would become a verdict");
static_assert(std::is_copy_constructible<img::AcceptedBase>::value,
              "the store has to be able to keep an AcceptedBase");

// Neither of the store's two commits takes the other's token: the overload set is exactly these
// two, and `commit(AcceptedBase)` does not exist at all.
static_assert(!std::is_invocable<decltype(static_cast<img::StoreError (img::Store::*)(
                                     const img::AcceptedImage&)>(&img::Store::commit)),
                                 img::Store&, const img::AcceptedBase&>::value,
              "the store's commit() takes a base: a base could be stored as an image's record");
static_assert(!std::is_invocable<decltype(&img::Store::commitBase), img::Store&,
                                 const img::AcceptedImage&>::value,
              "the store's commitBase() takes an image: an image could be stored as the base");
// Copyable on purpose, both of them: the store keeps one (verdict.h says so in words).
static_assert(std::is_copy_constructible<img::ExpectedFile>::value,
              "the store has to be able to keep an ExpectedFile");
static_assert(std::is_copy_constructible<img::AcceptedImage>::value,
              "the store has to be able to keep an AcceptedImage");

// And the doors take nothing else. Written as the compiler sees them, so that a second overload -
// the easiest way this rail could be lost - fails here.
static_assert(std::is_same<decltype(&img::Store::beginWrite),
                           img::StoreError (img::Store::*)(img::Which, core::board::Slot,
                                                           const img::ExpectedFile&)>::value,
              "the store's beginWrite() no longer takes exactly (which, slot, ExpectedFile): a "
              "second overload is a second way into the store");
static_assert(std::is_same<decltype(&img::Store::commit),
                           img::StoreError (img::Store::*)(const img::AcceptedImage&)>::value,
              "the store's commit() no longer takes exactly (AcceptedImage): a second overload is "
              "a way to make a record valid without the six checks");
static_assert(std::is_same<decltype(&img::Store::commitBase),
                           img::StoreError (img::Store::*)(const img::AcceptedBase&)>::value,
              "the store's commitBase() no longer takes exactly (AcceptedBase): a second overload "
              "is a way to make a base's record valid without check 5p");

TEST_CASE("R5: the only constructor of either token is the verdict's, and it runs the six checks") {
  // The one way in: a manifest signed by a key this build trusts, judged, and the token it
  // yields. The bench is the store with the specification's test key in its trust table
  // (host_test/core/image/store_bench.h); the manifest and the file are the spec's own fixtures.
  bench::StoreBench b;
  const bench::Candidate image("store-a");

  // judgeManifest() is checks 1, 2, 3 and the manifest's half of 6, and what it yields is the
  // promise the store's first door takes. Before it there is no token in existence.
  CHECK(b.verdict.expected() == nullptr);
  CHECK(b.verdict.accepted() == nullptr);
  const img::Outcome judged = b.judgeManifest(image, core::board::Slot{0});
  REQUIRE_MESSAGE(judged.ok(), img::describe(judged.failed) << ": " << judged.why);
  const img::ExpectedFile* expected = b.verdict.expected();
  REQUIRE(expected != nullptr);
  CHECK(expected->size() == image.file.size());
  CHECK(expected->slot() == core::board::Slot{0});

  // And the second door needs the other token, which only judgeFile() over the bytes as they lie
  // in the Walter's own flash can make - never over what a sender said it sent.
  REQUIRE(b.put(img::Which::kGolden, core::board::Slot{0}, image) == img::StoreError::kNone);
  const img::AcceptedImage* accepted = b.verdict.accepted();
  REQUIRE(accepted != nullptr);
  CHECK(accepted->slot() == core::board::Slot{0});
  MESSAGE("the store's two doors took the verdict's two tokens, and there is no other way to "
          "open either: " << sizeof(img::ExpectedFile) << " bytes of promise, "
          << sizeof(img::AcceptedImage) << " bytes of verdict, each with one friend");
}

TEST_CASE("R5: the base's door is a third one of the same kind, and the two cannot be swapped") {
  // Task p5-01. The run-time half: the base goes in through checks 1 to 4 and 5p and comes out of
  // the verdict as an AcceptedBase, with no AcceptedImage in existence - and the other way round.
  // The compile-time half is the static_asserts above, which are the part that matters: a base
  // written where an image belongs would be flashed into a radio's application region, and an
  // image written where a base belongs would be given to `rak provision` to write over an MBR.
  bench::StoreBench b;
  const bench::BaseFile base("base-small");
  const bench::Candidate image("store-a");

  REQUIRE(b.putBase(core::board::Slot{0}, base) == img::StoreError::kNone);
  REQUIRE(b.verdict.acceptedBase() != nullptr);
  CHECK(b.verdict.accepted() == nullptr);

  REQUIRE(b.put(img::Which::kCandidate, core::board::Slot{0}, image) == img::StoreError::kNone);
  REQUIRE(b.verdict.accepted() != nullptr);
  CHECK(b.verdict.acceptedBase() == nullptr);
  const img::AcceptedImage kept = *b.verdict.accepted();  // the store keeps one like this

  // And the doors refuse each other's open write at run time as well as at compile time: an
  // image's token offered for the base's bytes.
  REQUIRE(b.openBase(core::board::Slot{0}, base) == img::StoreError::kNone);
  CHECK(b.store.commit(kept) == img::StoreError::kNotAnImage);
  b.store.abandonWrite();
  MESSAGE("the base's door took an AcceptedBase of " << sizeof(img::AcceptedBase)
          << " bytes, and neither door takes the other's token: the compiler holds that, above");
}
