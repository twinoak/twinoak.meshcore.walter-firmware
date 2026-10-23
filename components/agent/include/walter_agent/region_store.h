// What a sleep keeps, in flash (task p4-05): the copied library's `mm::IRetained`
// (agent/IRetained.h) over a window of one of this firmware's own flash regions. The loop
// writes one image a wake into it - the results the server did not take and the adverts a wake
// could not post, about 19.5 kB at the most (agent/KeptImage.h) - and reads it back after the
// next boot, so that *no job is left leased and silent* holds across a deep sleep and not only
// within one boot (docs/ARCHITECTURE.md §13, the fourth seam).
//
// Where the window is: the tail of the `rakfw` partition. The image store lays out 6 096 KB of
// the partition's 6 144 KB (core/image/store.h, kStoreBytes) and touches nothing past that, so
// the last 48 KB - twelve sectors - are free, and an image of five sectors with room to grow goes
// there; main/ opens this object at `kStoreBytes` with what the partition has left. No partition
// moves (docs/ARCHITECTURE.md §11 is unchanged), and a store written by a firmware that had no
// kept image is read by one that has. RTC memory was the other place and has 36 bytes free
// (tasks/phase-3/reports/05.md); NVS carries strings of 160 bytes.
//
// What it is, in three methods: `capacity()` is the window less nothing (the image carries its
// own length and CRC-32 and the loop reads no further than it says); `write()` erases the sectors
// the image covers and writes it, whole or not at all as far as this object can tell - a power
// cut between the erase and the last byte is a torn image the library refuses whole at the next
// read, counted and logged, which is the seam's own rule; `read()` is the window's bytes, as many
// as the caller's buffer takes.
//
// Wear: the loop writes only when the image would differ from the one the store holds, so a unit
// whose wakes post everything writes nothing, and one on a metered plan writes a few images a
// day (agent/IRetained.h, *Wear*). Nothing secret: the runner wipes every credential before a
// result is written, so the image holds nothing a result does not.
//
// Logic: no ESP-IDF include, no heap, no buffer of its own - the bytes are the loop's scratch.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IRetained.h>

#include "interfaces/flash_region.h"

namespace agent {

// The window's least size worth opening: one sector of the image's header and something after it.
constexpr std::uint32_t kRegionStoreLeastBytes = 2 * 4096;

struct RegionStoreCounters {
  std::uint32_t writes = 0;          // images written
  std::uint32_t erases = 0;          // sectors erased for them
  std::uint32_t reads = 0;
  std::uint32_t refused = 0;         // a write the region refused, or one too long for the window
  std::uint32_t bytes_written = 0;   // of the last image
};

class RegionStore final : public mm::IRetained {
 public:
  // `offset` and `length` are the window inside `region`; a window that does not fit the region,
  // or is shorter than kRegionStoreLeastBytes, is a store of capacity 0, which the loop reads as
  // "no store" and nothing is kept.
  RegionStore(interfaces::FlashRegion& region, std::uint32_t offset, std::uint32_t length);

  std::size_t capacity() const override { return capacity_; }
  bool write(const std::uint8_t* image, std::size_t len) override;
  std::size_t read(std::uint8_t* out, std::size_t cap) override;

  bool usable() const { return capacity_ != 0; }
  std::uint32_t offset() const { return offset_; }
  const RegionStoreCounters& counters() const { return counters_; }

 private:
  interfaces::FlashRegion& region_;
  std::uint32_t offset_ = 0;
  std::uint32_t capacity_ = 0;
  std::uint32_t sector_ = 4096;
  RegionStoreCounters counters_;
};

}  // namespace agent
