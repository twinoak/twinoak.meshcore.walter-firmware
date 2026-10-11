// A region of the Walter's own flash, as core/ sees it (task p2-02): read, write, erase by
// sector, and how big a sector is. The image store (core/image/store.h) is the only thing in this
// firmware that holds one; behind it is the `rakfw` partition of partitions.csv
// (platform/flash_partition.h) or, in the tests, a simulated flash that only clears bits, counts
// its erases and can lose power in the middle of any operation (host_test/sim/sim_flash.h).
//
// The shape of NOR flash, which everything above this interface is written for:
//
//   - **a write only clears bits.** Writing 0x0F over 0xFF gives 0x0F; writing 0xF0 over 0x0F
//     gives 0x00, not 0xF0. The implementation does not check or complain: it is what the silicon
//     does, and the store uses it on purpose - a record is invalidated by writing zeros over its
//     magic, which no erase is needed for.
//   - **only an erase sets bits**, and only a whole sector at a time.
//   - **an operation interrupted by a power cut is half done**, and which half is not knowable.
//     Every caller above here is written so that a torn write leaves either the old state or
//     nothing that validates - never something in between that looks whole.
//
// Offsets are from the start of the region, so nothing above this interface knows where in the
// flash it is. A range that leaves the region is refused (false), not clamped.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

class FlashRegion {
 public:
  virtual ~FlashRegion() = default;

  // The region's size in bytes. Always a whole number of sectors.
  virtual std::uint32_t size() const = 0;

  // The erase unit, in bytes (4096 on the ESP32-S3's flash).
  virtual std::uint32_t sectorSize() const = 0;

  // `count` bytes from `offset` into `out`. False when the range is not inside the region or the
  // read failed; `out` is then not to be trusted.
  virtual bool read(std::uint32_t offset, void* out, std::uint32_t count) const = 0;

  // `count` bytes at `offset`, which must have been erased since anything was last written there
  // *unless* the caller means to clear bits (see above). False when the range is not inside the
  // region or the write failed.
  virtual bool write(std::uint32_t offset, const void* data, std::uint32_t count) = 0;

  // Erases `count` bytes at `offset`; both must be multiples of sectorSize(). False when they are
  // not, when the range leaves the region, or when the erase failed.
  virtual bool erase(std::uint32_t offset, std::uint32_t count) = 0;
};

}  // namespace interfaces
