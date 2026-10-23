// The kept image's store over a flash window (task p4-05, walter_agent/region_store.h): the
// window's capacity, an image written whole and read back, the sectors erased for it and no
// more, a window that does not fit or is too small, a write the region refuses, and the
// library's own image through it - written by the loop at the end of a wake and read back by
// `restoreKept()` after a boot - which is the one thing the seam exists for.
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_flash.h"
#include "walter_agent/region_store.h"

using agent::RegionStore;

TEST_CASE("a window of twelve sectors: the capacity, a write, a read, the erases") {
  sim::SimFlash flash(16 * 4096);
  RegionStore store(flash, 4 * 4096, 12 * 4096);
  REQUIRE(store.usable());
  CHECK(store.capacity() == 12 * 4096);

  std::vector<std::uint8_t> image(5000);
  for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<std::uint8_t>(i * 7 + 3);
  REQUIRE(store.write(image.data(), image.size()));
  CHECK(store.counters().writes == 1);
  CHECK(store.counters().erases == 2);  // 5000 bytes cover two sectors, and two were erased
  CHECK(flash.erasesOfSector(4) == 1);
  CHECK(flash.erasesOfSector(5) == 1);
  CHECK(flash.erasesOfSector(6) == 0);
  CHECK(flash.erasesOfSector(3) == 0);  // nothing before the window

  std::vector<std::uint8_t> back(12 * 4096, 0xAA);
  const std::size_t n = store.read(back.data(), back.size());
  CHECK(n == 12 * 4096);  // the whole window: the image says its own length
  CHECK(std::memcmp(back.data(), image.data(), image.size()) == 0);
  // A shorter buffer takes what it can.
  std::vector<std::uint8_t> head(100);
  CHECK(store.read(head.data(), head.size()) == 100);
  CHECK(std::memcmp(head.data(), image.data(), 100) == 0);

  // A second, smaller image: the sector it covers erased again, the stale tail left behind.
  std::vector<std::uint8_t> small(10, 0x5A);
  REQUIRE(store.write(small.data(), small.size()));
  CHECK(store.counters().erases == 3);
  CHECK(flash.erasesOfSector(4) == 2);
  CHECK(flash.erasesOfSector(5) == 1);
}

TEST_CASE("a window that does not fit, is not on a sector boundary or is too small is a store of no capacity") {
  sim::SimFlash flash(16 * 4096);
  CHECK_FALSE(RegionStore(flash, 10 * 4096, 7 * 4096).usable());  // past the end
  CHECK_FALSE(RegionStore(flash, 100, 8 * 4096).usable());        // off a boundary
  CHECK_FALSE(RegionStore(flash, 0, 4096).usable());              // one sector: too small
  RegionStore none(flash, 10 * 4096, 7 * 4096);
  std::uint8_t byte = 1;
  CHECK_FALSE(none.write(&byte, 1));
  CHECK(none.read(&byte, 1) == 0);
  CHECK(none.counters().refused == 1);
  // A window of whole sectors plus a tail: the tail is not counted.
  RegionStore odd(flash, 0, 3 * 4096 + 100);
  CHECK(odd.capacity() == 3 * 4096);
}

TEST_CASE("an image longer than the window, an empty one, and a region that refuses, are refused and counted") {
  sim::SimFlash flash(16 * 4096);
  RegionStore store(flash, 0, 2 * 4096);
  std::vector<std::uint8_t> big(2 * 4096 + 1, 1);
  CHECK_FALSE(store.write(big.data(), big.size()));
  CHECK_FALSE(store.write(big.data(), 0));
  CHECK_FALSE(store.write(nullptr, 10));
  CHECK(store.counters().refused == 3);
  CHECK(store.counters().writes == 0);
  // The power goes: the region refuses, and so does the store.
  flash.powerOff();
  std::uint8_t one = 1;
  CHECK_FALSE(store.write(&one, 1));
  CHECK(store.counters().refused == 4);
  flash.powerOn();
}
