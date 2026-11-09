// The image store (task p2-02): what it does when nothing goes wrong, and every refusal at its
// door. The power cuts are test_store_power_loss.cpp's, and they are the point of the design;
// this file is the behaviour those cuts have to preserve.
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "core/image/store.h"
#include "store_bench.h"

using bench::Candidate;
using bench::Slot;
using bench::StoreBench;
using bench::StoreError;
using bench::Which;
using core::image::AcceptedImage;
using core::image::ExpectedFile;
using core::image::Record;
using core::image::Store;

// ----------------------------------------------------------------- no token, no store
//
// The task asks for "no token, no store" as a compile-time property and for how it is enforced.
// It is enforced by two types with no public constructor and by the signatures of the only two
// doors into the store: there is no overload of either that takes anything else, so a caller
// without a verdict has nothing it can pass.
static_assert(!std::is_default_constructible<ExpectedFile>::value,
              "an ExpectedFile must come from ImageVerdict::judgeManifest and from nowhere else");
static_assert(!std::is_default_constructible<AcceptedImage>::value,
              "an AcceptedImage must come from ImageVerdict::judgeFile and from nowhere else");
static_assert(std::is_copy_constructible<ExpectedFile>::value &&
                  std::is_copy_constructible<AcceptedImage>::value,
              "both must be copyable: the store and the update keep one");
static_assert(std::is_same<decltype(&Store::beginWrite),
                           StoreError (Store::*)(Which, Slot, const ExpectedFile&)>::value,
              "the only way to open a write takes the verdict's promise");
static_assert(std::is_same<decltype(&Store::commit),
                           StoreError (Store::*)(const AcceptedImage&)>::value,
              "the only way to make a record takes the verdict's token");

namespace {

// The `rakfw` line of partitions.csv, read rather than transcribed: `name,type,subtype,at,size`.
struct Partition {
  bool found = false;
  std::uint32_t at = 0;
  std::uint32_t size = 0;
};

Partition rakfwFromTable() {
  Partition out;
  std::ifstream in(std::string(TWINOAK_REPO_ROOT) + "/partitions.csv");
  REQUIRE(in.good());
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("rakfw", 0) != 0) continue;
    std::vector<std::string> fields;
    std::string field;
    for (char c : line + ",") {
      if (c == ',') {
        while (!field.empty() && field.back() == ' ') field.pop_back();
        while (!field.empty() && field.front() == ' ') field.erase(field.begin());
        fields.push_back(field);
        field.clear();
      } else {
        field.push_back(c);
      }
    }
    REQUIRE(fields.size() >= 5);
    out.at = static_cast<std::uint32_t>(std::stoul(fields[3], nullptr, 0));
    out.size = static_cast<std::uint32_t>(std::stoul(fields[4], nullptr, 0));
    out.found = true;
    return out;
  }
  return out;
}

std::uint32_t dataOffsetOf(const Store& store, Which which, Slot slot) {
  return core::image::areaOffset(slot.index, store.areaOf(which, slot)) + core::image::kSectorSize;
}

}  // namespace

TEST_CASE("the layout is laid out for two radio slots and fits the rakfw partition of partitions.csv") {
  const Partition rakfw = rakfwFromTable();
  REQUIRE(rakfw.found);
  // partitions.csv is the authority; core/image/store.h's constant is held to it here so that a
  // partition table that shrinks fails this test rather than a unit in the field.
  CHECK(core::image::kPartitionBytes == rakfw.size);
  CHECK(core::image::kStoreBytes <= rakfw.size);

  // The arithmetic the report gives, derived rather than written down: an image area holds the
  // largest application region of any board profile this build knows, rounded up to a sector,
  // plus one header sector; six of those (two slots x three images); a base area of 640 KB and
  // one header sector a slot (task p5-01); two index sectors.
  CHECK(core::image::kLargestAppRegion ==
        core::flasher::kRak4630Single.app.end - core::flasher::kRak4630Single.app.begin);
  CHECK(core::image::kImageBytesMax == 0xC7000u);
  CHECK(core::image::kDataSectors == 199u);
  CHECK(core::image::kAreaBytes == 200u * 4096u);
  CHECK(core::image::kAreaCount == 6u);
  CHECK(core::image::kBaseBytesMax == 640u * 1024u);
  CHECK(core::image::kBaseSectors == 160u);
  CHECK(core::image::kBaseAreaBytes == 161u * 4096u);
  CHECK(core::image::kBaseAreaCount == 2u);
  CHECK(core::image::kStoreBytes ==
        2u * 4096u + 6u * 200u * 4096u + 2u * 161u * 4096u);
  CHECK(core::image::kStoreBytes == 6242304u);
  CHECK(rakfw.size - core::image::kStoreBytes == 49152u);

  // The areas are where areaOffset() says, they do not overlap, and the last one ends inside the
  // partition: the six image areas first - where they were before the base existed - then the two
  // base areas.
  std::uint32_t expect = core::image::kIndexBytes;
  for (std::size_t s = 0; s < core::image::kRadioSlots; ++s) {
    for (std::size_t a = 0; a < core::image::kPromotableCount; ++a) {
      CHECK(core::image::areaOffset(s, a) == expect);
      expect += core::image::kAreaBytes;
    }
  }
  for (std::size_t s = 0; s < core::image::kRadioSlots; ++s) {
    CHECK(core::image::areaOffset(s, core::image::kBaseArea) == expect);
    expect += core::image::kBaseAreaBytes;
  }
  CHECK(expect == core::image::kStoreBytes);
  MESSAGE("rakfw at " << rakfw.at << ", " << rakfw.size << " bytes; the store uses "
                      << core::image::kStoreBytes << " and leaves "
                      << (rakfw.size - core::image::kStoreBytes));
}

TEST_CASE("a region smaller than the layout, or with another sector size, is refused") {
  fakes::FakeWallClock wall;
  sim::SimFlash small(core::image::kStoreBytes - core::image::kSectorSize);
  Store store(small, wall);
  CHECK(store.begin() == StoreError::kRegionTooSmall);
  sim::SimFlash coarse(core::image::kStoreBytes, 65536);
  Store other(coarse, wall);
  CHECK(other.begin() == StoreError::kRegionTooSmall);
  // And nothing works until begin() has succeeded.
  Record record;
  CHECK(store.read(Which::kGolden, Slot{0}, &record) == StoreError::kRegionTooSmall);
}

TEST_CASE("an image is stored, read back and verified, and a second one goes over the first") {
  StoreBench b;
  const Candidate a("store-a");
  const Candidate second("store-b");

  CHECK(b.read(Which::kCandidate).present == false);
  REQUIRE(b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);

  const Record record = b.read(Which::kCandidate);
  REQUIRE(record.present);
  CHECK(record.size == a.file.size());
  CHECK(record.load_address == core::flasher::kRak4630Single.app.begin);
  CHECK(std::string(record.version) == a.version);
  CHECK(std::string(record.descriptor.version) == a.version);
  CHECK(std::string(record.descriptor.board) == "rak4630-single");
  CHECK(std::string(record.descriptor.band) == "868");
  CHECK(record.descriptor.role == core::image::Role::kRepeater);
  CHECK(record.stored_as == Which::kCandidate);
  CHECK(record.slot == Slot{0});
  CHECK(record.use == core::image::Use::kSwd);
  // Where it came from: the manifest's commit, and the key that signed the manifest.
  CHECK(std::string(record.commit).size() == core::image::kCommitSize);
  // The hash in the record is the file's own.
  std::uint8_t digest[32];
  core::image::sha256(a.file.data(), a.file.size(), digest);
  CHECK(std::memcmp(record.sha256, digest, 32) == 0);
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);

  // The bytes, as the flasher will read them: in order, and re-hashed as they come.
  core::image::StoredImage image;
  REQUIRE(b.store.open(Which::kCandidate, Slot{0}, &image) == StoreError::kNone);
  CHECK(image.size() == a.file.size());
  CHECK(image.loadAddress() == core::flasher::kRak4630Single.app.begin);
  std::vector<std::uint8_t> read_back(image.size());
  for (std::uint32_t at = 0; at < image.size(); at += 256) {
    const std::uint32_t n = std::min<std::uint32_t>(256, image.size() - at);
    REQUIRE(image.read(at, read_back.data() + at, n));
  }
  CHECK(read_back == a.file);
  CHECK(image.sequential());
  CHECK(image.hashOk());

  // The second image over the first: the same slot, a different hash and version.
  REQUIRE(b.put(Which::kCandidate, Slot{0}, second) == StoreError::kNone);
  const Record now = b.read(Which::kCandidate);
  REQUIRE(now.present);
  CHECK(std::string(now.version) == second.version);
  CHECK(now.size == second.file.size());
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
  CHECK(std::memcmp(now.sha256, record.sha256, 32) != 0);
}

TEST_CASE("an image shorter than a sector, and one of exactly two sectors, are stored the same way") {
  StoreBench b;
  for (const char* name : {"store-part", "store-exact"}) {
    CAPTURE(name);
    const Candidate image(name);
    REQUIRE(b.put(Which::kCandidate, Slot{0}, image) == StoreError::kNone);
    const Record record = b.read(Which::kCandidate);
    REQUIRE(record.present);
    CHECK(record.size == image.file.size());
    CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
    // Nothing beyond the image was written: the sector after the last one is still erased.
    const std::uint32_t data = dataOffsetOf(b.store, Which::kCandidate, Slot{0});
    const std::uint32_t past = data + core::image::roundUpToSector(record.size);
    CHECK(b.flash.at(past) == 0xFF);
  }
}

TEST_CASE("a piece of any size is taken: the store buffers whole sectors itself") {
  const Candidate a("store-a");
  for (std::uint32_t chunk : {1u, 3u, 240u, 1024u, 4096u, 5000u, 20480u}) {
    CAPTURE(chunk);
    StoreBench b;
    REQUIRE(b.put(Which::kCandidate, Slot{0}, a, chunk) == StoreError::kNone);
    CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
  }
}

TEST_CASE("golden is loaded directly only while it is empty; afterwards only a promotion reaches it") {
  StoreBench b;
  const Candidate a("store-a");
  const Candidate second("store-b");
  // A unit's first provisioning: there is no golden image, so one may be put there.
  REQUIRE(b.put(Which::kGolden, Slot{0}, a) == StoreError::kNone);
  CHECK(b.read(Which::kGolden).present);
  // And never again.
  CHECK(b.open(Which::kGolden, Slot{0}, second) == StoreError::kGoldenNotEmpty);
  // The refusal happens before anything is touched: the golden image is still whole.
  CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);
  CHECK(std::string(b.read(Which::kGolden).version) == a.version);
}

TEST_CASE("the candidate becomes golden by moving a pointer, and the old golden image is kept") {
  StoreBench b;
  const Candidate first("store-a");
  const Candidate next("store-b");
  REQUIRE(b.put(Which::kGolden, Slot{0}, first) == StoreError::kNone);
  REQUIRE(b.put(Which::kCandidate, Slot{0}, next) == StoreError::kNone);
  const std::uint8_t golden_area = b.store.areaOf(Which::kGolden, Slot{0});
  const std::uint8_t candidate_area = b.store.areaOf(Which::kCandidate, Slot{0});
  CHECK(golden_area != candidate_area);

  // Not one byte of the images moves: what moves is the index.
  const std::uint32_t writes_before = b.flash.operations();
  REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);
  const std::uint32_t operations = b.flash.operations() - writes_before;
  MESSAGE("a promotion is " << operations << " flash operations, whatever the image's size");
  CHECK(operations <= 10);

  CHECK(b.store.areaOf(Which::kGolden, Slot{0}) == candidate_area);
  CHECK(b.store.areaOf(Which::kCandidate, Slot{0}) == golden_area);
  const Record golden = b.read(Which::kGolden);
  REQUIRE(golden.present);
  CHECK(std::string(golden.version) == next.version);
  CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);
  // The candidate slot is empty again: what was golden is not a candidate to try.
  CHECK(b.read(Which::kCandidate).present == false);
  // It survives a restart.
  b.reboot();
  CHECK(std::string(b.read(Which::kGolden).version) == next.version);
  CHECK(b.read(Which::kCandidate).present == false);

  // And a promotion with nothing to promote is refused, leaving the golden image alone.
  CHECK(b.store.promote(Slot{0}) == StoreError::kEmpty);
  CHECK(std::string(b.read(Which::kGolden).version) == next.version);
}

TEST_CASE("the bad list refuses an image at the door, survives a restart, and drops the oldest") {
  StoreBench b;
  const Candidate a("store-a");
  REQUIRE(b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  const Record record = b.read(Which::kCandidate);
  REQUIRE(record.present);

  CHECK(b.store.isBad(record.sha256) == false);
  REQUIRE(b.store.markBad(record.sha256) == StoreError::kNone);
  CHECK(b.store.isBad(record.sha256));
  CHECK(b.store.badCount() == 1);
  // Marking it again is not a second entry.
  REQUIRE(b.store.markBad(record.sha256) == StoreError::kNone);
  CHECK(b.store.badCount() == 1);
  // An image on the list never enters again - and the refusal is before any erase.
  const std::uint32_t before = b.flash.operations();
  CHECK(b.open(Which::kCandidate, Slot{0}, a) == StoreError::kOnBadList);
  CHECK(b.flash.operations() == before);
  CHECK(b.read(Which::kCandidate).present);  // what was there is untouched

  b.reboot();
  CHECK(b.store.isBad(record.sha256));

  // Bounded: the ninth hash drops the first.
  std::uint8_t hash[32] = {};
  for (std::uint8_t i = 0; i < core::image::kBadListMax; ++i) {
    hash[0] = static_cast<std::uint8_t>(0xA0 + i);
    REQUIRE(b.store.markBad(hash) == StoreError::kNone);
  }
  CHECK(b.store.badCount() == core::image::kBadListMax);
  // Nine hashes went in and eight are kept: the first one - the stored image's - was pushed off
  // the end, and the eight after it are all there.
  CHECK(b.store.isBad(record.sha256) == false);
  for (std::uint8_t i = 0; i < core::image::kBadListMax; ++i) {
    hash[0] = static_cast<std::uint8_t>(0xA0 + i);
    CHECK(b.store.isBad(hash));
  }
  hash[0] = static_cast<std::uint8_t>(0xA0 + core::image::kBadListMax - 1);
  CHECK(b.store.isBad(hash));
  b.reboot();
  CHECK(b.store.isBad(hash));
  CHECK(b.store.badCount() == core::image::kBadListMax);
}

TEST_CASE("a flipped bit in a stored image is caught by verify and by the read stream") {
  StoreBench b;
  const Candidate a("store-a");
  REQUIRE(b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  REQUIRE(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);

  // Flash rot in the middle of the image: no write put it there, and the record still validates
  // over its own header - which is why the bytes are re-hashed and not trusted.
  const std::uint32_t data = dataOffsetOf(b.store, Which::kCandidate, Slot{0});
  b.flash.rot(data + 5000, 0x08);
  CHECK(b.read(Which::kCandidate).present);  // the record is whole; the image is not
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kHashMismatch);

  // And the stream the flasher reads says so too, after it has read the whole image in order.
  core::image::StoredImage image;
  REQUIRE(b.store.open(Which::kCandidate, Slot{0}, &image) == StoreError::kNone);
  std::vector<std::uint8_t> buffer(512);
  for (std::uint32_t at = 0; at < image.size(); at += 512) {
    const std::uint32_t n = std::min<std::uint32_t>(512, image.size() - at);
    REQUIRE(image.read(at, buffer.data(), n));
  }
  CHECK(image.sequential());
  CHECK(image.hashOk() == false);

  // A reader that jumps about cannot say anything either way, and says so rather than lying.
  image.rewind();
  REQUIRE(image.read(1024, buffer.data(), 512));
  CHECK(image.sequential() == false);
  CHECK(image.hashOk() == false);
  // A read outside the image is refused.
  CHECK(image.read(image.size(), buffer.data(), 1) == false);
  CHECK(image.read(0, buffer.data(), image.size() + 1) == false);
}

TEST_CASE("a write resumes where an interruption left it; a different file starts over") {
  StoreBench b;
  const Candidate big("store-resume");
  REQUIRE(big.file.size() > 2 * core::image::kResumeStep);

  // 60 % of the way, then the Walter resets.
  REQUIRE(b.open(Which::kCandidate, Slot{0}, big) == StoreError::kNone);
  const std::uint32_t part = static_cast<std::uint32_t>(big.file.size() * 3 / 5);
  for (std::uint32_t at = 0; at < part; at += 240) {
    const std::uint32_t n = std::min<std::uint32_t>(240, part - at);
    REQUIRE(b.store.write(big.file.data() + at, n) == StoreError::kNone);
  }
  b.reboot();

  // The slot is empty, and says how far a transfer of that very file got.
  const Record half = b.read(Which::kCandidate);
  CHECK(half.present == false);
  CHECK(half.transfer);
  CHECK(half.transfer_size == big.file.size());
  CHECK(half.transferred == (part / core::image::kResumeStep) * core::image::kResumeStep);
  CHECK(half.transferred > 0);

  // Asking again continues from there: the bytes already in flash are not sent twice.
  REQUIRE(b.open(Which::kCandidate, Slot{0}, big) == StoreError::kNone);
  CHECK(b.store.written() == half.transferred);
  REQUIRE(b.stream(big, 240, b.store.written()) == StoreError::kNone);
  REQUIRE(b.commit() == StoreError::kNone);
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);

  // A resume whose file is another file starts over at zero.
  StoreBench other;
  REQUIRE(other.open(Which::kCandidate, Slot{0}, big) == StoreError::kNone);
  for (std::uint32_t at = 0; at < part; at += 4096) {
    const std::uint32_t n = std::min<std::uint32_t>(4096, part - at);
    REQUIRE(other.store.write(big.file.data() + at, n) == StoreError::kNone);
  }
  other.reboot();
  CHECK(other.read(Which::kCandidate).transferred > 0);
  const Candidate different("store-a");
  REQUIRE(other.open(Which::kCandidate, Slot{0}, different) == StoreError::kNone);
  CHECK(other.store.written() == 0);
  REQUIRE(other.stream(different) == StoreError::kNone);
  REQUIRE(other.commit() == StoreError::kNone);
  CHECK(std::string(other.read(Which::kCandidate).version) == different.version);
}

TEST_CASE("the store refuses more bytes than were promised, and a commit before the last of them") {
  StoreBench b;
  const Candidate a("store-a");
  REQUIRE(b.open(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  REQUIRE(b.stream(a) == StoreError::kNone);
  const std::uint8_t more[4] = {1, 2, 3, 4};
  CHECK(b.store.write(more, 4) == StoreError::kSizeMismatch);
  REQUIRE(b.commit() == StoreError::kNone);
  // And a write with nothing open.
  CHECK(b.store.write(more, 4) == StoreError::kNotWriting);

  // openStaged() is only for a write whose last byte has reached flash, which for an image
  // that does not end on a sector boundary is endWrite()'s doing.
  const Candidate part("store-part");
  StoreBench c;
  REQUIRE(c.open(Which::kCandidate, Slot{0}, part) == StoreError::kNone);
  REQUIRE(c.stream(part, 240, 0) == StoreError::kNone);
  core::image::StoredImage staged;
  CHECK(c.store.openStaged(&staged) == StoreError::kSizeMismatch);  // the tail is not in flash yet
  REQUIRE(c.store.endWrite() == StoreError::kNone);
  CHECK(c.store.openStaged(&staged) == StoreError::kNone);
  c.store.abandonWrite();
  CHECK(c.store.openStaged(&staged) == StoreError::kNotWriting);
  CHECK(c.read(Which::kCandidate).present == false);
}

TEST_CASE("a token for another file does not make a record") {
  StoreBench b;
  const Candidate a("store-a");
  const Candidate second("store-b");
  REQUIRE(a.file.size() == second.file.size());  // the same length, other bytes

  // Open a write for A, put A's bytes in, then hand over B's token.
  REQUIRE(b.open(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  REQUIRE(b.stream(a) == StoreError::kNone);
  // B's verdict, over B's bytes - a perfectly good token, for another file.
  core::image::ImageVerdict other(b.trust.table, b.unit);
  const core::image::Outcome first = other.judgeManifest(
      second.manifest.data(), second.manifest.size(), second.signature.data(),
      second.signature.size(), second.needs(Slot{0}), core::image::Use::kSwd,
      &core::flasher::kRak4630Single);
  REQUIRE(first.ok());
  const core::flasher::MemoryImage source(second.file.data(),
                                          static_cast<std::uint32_t>(second.file.size()));
  REQUIRE(other.judgeFile(source).ok());
  REQUIRE(other.accepted() != nullptr);
  CHECK(b.store.commit(*other.accepted()) == StoreError::kTokenMismatch);
  CHECK(b.read(Which::kCandidate).present == false);
}

TEST_CASE("erase forgets a candidate and a scanner image, and never the golden one") {
  StoreBench b;
  const Candidate a("store-a");
  const Candidate scanner("store-scanner");
  REQUIRE(b.put(Which::kGolden, Slot{0}, a) == StoreError::kNone);
  REQUIRE(b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  REQUIRE(b.put(Which::kScanner, Slot{0}, scanner) == StoreError::kNone);

  CHECK(b.store.erase(Which::kGolden, Slot{0}) == StoreError::kNeverGolden);
  CHECK(b.read(Which::kGolden).present);

  REQUIRE(b.store.erase(Which::kCandidate, Slot{0}) == StoreError::kNone);
  CHECK(b.read(Which::kCandidate).present == false);
  CHECK(b.read(Which::kCandidate).transfer == false);  // the progress went with the header sector
  REQUIRE(b.store.erase(Which::kScanner, Slot{0}) == StoreError::kNone);
  CHECK(b.read(Which::kScanner).present == false);
  CHECK(b.read(Which::kGolden).present);
  CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kEmpty);

  b.reboot();
  CHECK(b.read(Which::kCandidate).present == false);
  CHECK(b.read(Which::kGolden).present);
}

TEST_CASE("the scanner slot holds a scanner image, and the three images of a slot are independent") {
  StoreBench b;
  const Candidate a("store-a");
  const Candidate scanner("store-scanner");
  REQUIRE(b.put(Which::kScanner, Slot{0}, scanner) == StoreError::kNone);
  REQUIRE(b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  CHECK(b.read(Which::kScanner).descriptor.role == core::image::Role::kScanner);
  CHECK(b.read(Which::kCandidate).descriptor.role == core::image::Role::kRepeater);
  CHECK(b.store.verify(Which::kScanner, Slot{0}) == StoreError::kNone);
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
  // A promotion moves the candidate and leaves the scanner's area where it was.
  const std::uint8_t scanner_area = b.store.areaOf(Which::kScanner, Slot{0});
  REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);
  CHECK(b.store.areaOf(Which::kScanner, Slot{0}) == scanner_area);
  CHECK(b.store.verify(Which::kScanner, Slot{0}) == StoreError::kNone);
}

TEST_CASE("the second radio slot has its areas, and a slot the layout has none for is refused") {
  StoreBench b;
  const Candidate a("store-a");
  // Slot 1's areas exist - the store is laid out for a dual unit from the start - and the areas
  // of the two slots do not overlap: an image in slot 1 leaves slot 0 alone. (A unit has one
  // radio today, so nothing but a test puts an image there.)
  REQUIRE(b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  const Candidate second("store-b");
  REQUIRE(b.put(Which::kCandidate, Slot{1}, second) == StoreError::kNone);
  CHECK(std::string(b.read(Which::kCandidate, Slot{0}).version) == a.version);
  CHECK(std::string(b.read(Which::kCandidate, Slot{1}).version) == second.version);
  CHECK(b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
  CHECK(b.store.verify(Which::kCandidate, Slot{1}) == StoreError::kNone);

  Record record;
  CHECK(b.store.read(Which::kGolden, Slot{2}, &record) == StoreError::kNoSuchSlot);
  CHECK(b.store.promote(Slot{2}) == StoreError::kNoSuchSlot);
  CHECK(b.store.erase(Which::kCandidate, Slot{2}) == StoreError::kNoSuchSlot);
}

TEST_CASE("one write at a time") {
  StoreBench b;
  const Candidate a("store-a");
  REQUIRE(b.open(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  CHECK(b.open(Which::kScanner, Slot{0}, a) == StoreError::kBusy);
  CHECK(b.store.promote(Slot{0}) == StoreError::kBusy);
  b.store.abandonWrite();
  CHECK(b.open(Which::kScanner, Slot{0}, a) == StoreError::kNone);
}

TEST_CASE("wear: a thousand images through the candidate slot, each promoted") {
  StoreBench b;
  const Candidate a("store-a");
  // One verdict, kept: an AcceptedImage is copyable on purpose, and this loop is about the flash
  // and not about Ed25519 (which test_ed25519.cpp and test_verdict.cpp are about). The store's
  // own read-back and hash happen on every one of the thousand commits all the same.
  REQUIRE(b.open(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  REQUIRE(b.stream(a) == StoreError::kNone);
  REQUIRE(b.store.endWrite() == StoreError::kNone);
  core::image::StoredImage staged;
  REQUIRE(b.store.openStaged(&staged) == StoreError::kNone);
  REQUIRE(b.verdict.judgeFile(staged).ok());
  const core::image::ExpectedFile promise = *b.verdict.expected();
  const core::image::AcceptedImage token = *b.verdict.accepted();
  REQUIRE(b.store.commit(token) == StoreError::kNone);
  REQUIRE(b.store.promote(Slot{0}) == StoreError::kNone);

  b.flash.forgetErases();
  constexpr int kImages = 1000;
  int failures = 0;
  for (int i = 1; i < kImages; ++i) {
    if (b.store.beginWrite(Which::kCandidate, Slot{0}, promise) != StoreError::kNone) ++failures;
    if (b.stream(a, 4096) != StoreError::kNone) ++failures;
    if (b.store.endWrite() != StoreError::kNone) ++failures;
    if (b.store.commit(token) != StoreError::kNone) ++failures;
    if (b.store.promote(Slot{0}) != StoreError::kNone) ++failures;
  }
  CHECK(failures == 0);
  CHECK(b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);

  // What the erases went on. An image of five sectors erases its five and its header sector,
  // once each, per stored image - there is nothing to level, because every store rewrites the
  // same sectors of its area from the beginning - and a promotion alternates the two areas, so
  // each area sees half of them.
  const std::uint32_t image_sectors = core::image::roundUpToSector(
      static_cast<std::uint32_t>(a.file.size())) / core::image::kSectorSize;
  std::uint32_t per_area[core::image::kWhichCount] = {};
  for (std::size_t area = 0; area < core::image::kWhichCount; ++area) {
    const std::uint32_t base = core::image::areaOffset(0, area) / core::image::kSectorSize;
    per_area[area] = b.flash.erasesOfSector(base);  // the header sector
    for (std::uint32_t s = 1; s <= image_sectors; ++s) {
      CHECK(b.flash.erasesOfSector(base + s) == per_area[area]);
    }
    // Nothing above the image is ever erased.
    CHECK(b.flash.erasesOfSector(base + image_sectors + 1) == 0);
  }
  const std::uint32_t index_a = b.flash.erasesOfSector(0);
  const std::uint32_t index_b = b.flash.erasesOfSector(1);
  MESSAGE("999 images of " << a.file.size() << " bytes, each promoted: area 0 " << per_area[0]
                           << " erases per sector, area 1 " << per_area[1] << ", area 2 (the"
                           << " scanner's) " << per_area[2] << "; the two index sectors "
                           << index_a << " and " << index_b);
  CHECK(per_area[2] == 0);
  CHECK(per_area[0] + per_area[1] == kImages - 1);
  // Evenly enough: the two areas a candidate and a golden image take turns in differ by at most
  // one erase over a thousand images.
  const std::uint32_t low = per_area[0] < per_area[1] ? per_area[0] : per_area[1];
  const std::uint32_t high = per_area[0] < per_area[1] ? per_area[1] : per_area[0];
  CHECK(high - low <= 1);
  // The index is written twice per promotion and the two copies take turns.
  CHECK(index_a + index_b == 2 * (kImages - 1));
  CHECK((index_a > index_b ? index_a - index_b : index_b - index_a) <= 1);
}

TEST_CASE("only the store reads and writes the image partition") {
  // interfaces::FlashRegion is named in the interface, in the store, in the platform's partition
  // and where main/ constructs one - and nowhere else in the firmware. The store is the only
  // thing that erases or writes the radio's images, which is why a record can be relied on.
  // The update journal (task p2-03) holds the second one: the `journal` partition, which is not
  // the image partition and never names an image's bytes. The agent's RegionStore (task p4-05)
  // holds a third: a window of its own for the loop's retained state and the one kept image a wake
  // may leave behind - again not the image partition, and given its region by main/ as the store
  // is given the image partition's. The core dump reader (task p11-03) holds a fourth: the
  // `coredump` partition, which it reads, and which it erases only when a person has typed
  // `coredump erase confirm` - no image's bytes are anywhere near it.
  const std::vector<std::string> allowed = {
      "components/interfaces/include/interfaces/flash_region.h",
      "components/core/include/core/image/store.h",
      "components/core/image/store.cpp",
      "components/core/include/core/update/journal.h",
      "components/core/update/journal.cpp",
      "components/platform/include/platform/flash_partition.h",
      "components/platform/flash_partition.cpp",
      "components/agent/include/walter_agent/region_store.h",
      "components/agent/region_store.cpp",
      "components/core/include/core/health/coredump.h",
      "components/core/health/coredump.cpp",
  };
  std::vector<std::string> offenders;
  const std::filesystem::path root(TWINOAK_REPO_ROOT);
  for (const char* dir : {"components", "main"}) {
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(root / dir)) {
      if (!entry.is_regular_file()) continue;
      const std::string ext = entry.path().extension().string();
      if (ext != ".cpp" && ext != ".h" && ext != ".c") continue;
      std::string relative = std::filesystem::relative(entry.path(), root).generic_string();
      std::ifstream in(entry.path());
      std::string line;
      bool names_it = false;
      while (std::getline(in, line)) {
        // `interfaces::FlashRegion`, spelt out: core::nrf52::FlashRegion is another type with
        // the same short name (the nRF52's own flash), and this check is not about it.
        if (line.find("interfaces::FlashRegion") != std::string::npos &&
            line.find("//") > line.find("interfaces::FlashRegion")) {
          names_it = true;
        }
      }
      if (!names_it) continue;
      bool ok = false;
      for (const std::string& a : allowed) {
        if (relative == a) ok = true;
      }
      if (!ok) offenders.push_back(relative);
    }
  }
  std::string names;
  for (const std::string& o : offenders) names += o + " ";
  CHECK_MESSAGE(offenders.empty(), "interfaces::FlashRegion is named outside the store and the "
                                   "platform's partition: " << names.c_str());
}
