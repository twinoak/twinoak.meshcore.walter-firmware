// interfaces::FlashRegion over one data partition of the Walter's flash (task p2-02): the
// `rakfw` partition of partitions.csv, where the radio's images live
// (docs/ARCHITECTURE.md §6 and §11). Thin, as everything in platform/ is: esp_partition_read,
// _write and _erase_range, the partition found once by its type and subtype, and the sector size
// ESP-IDF erases in.
#pragma once

#include <cstdint>

#include "esp_partition.h"

#include "interfaces/flash_region.h"

namespace platform {

// partitions.csv: `rakfw, data, 0x40, 0x630000, 0x600000`. The subtype is ours; the name is what
// the table says and what find() looks for.
constexpr const char* kImagePartitionName = "rakfw";
constexpr esp_partition_subtype_t kImagePartitionSubtype = static_cast<esp_partition_subtype_t>(0x40);

class FlashPartition final : public interfaces::FlashRegion {
 public:
  // Finds the partition. False when the table has no such partition, which is a build whose
  // partitions.csv and firmware disagree - said once, at boot, and the store then refuses
  // everything.
  bool begin(const char* name = kImagePartitionName);

  std::uint32_t size() const override { return partition_ != nullptr ? partition_->size : 0; }
  std::uint32_t sectorSize() const override { return kSectorSize; }
  bool read(std::uint32_t offset, void* out, std::uint32_t count) const override;
  bool write(std::uint32_t offset, const void* data, std::uint32_t count) override;
  bool erase(std::uint32_t offset, std::uint32_t count) override;

  // Where it is in the flash, for one console line.
  std::uint32_t address() const { return partition_ != nullptr ? partition_->address : 0; }

 private:
  // SPI_FLASH_SEC_SIZE: the unit esp_partition_erase_range works in. Written down rather than
  // included from spi_flash so that core/'s copy of the number (core/image/store.h's
  // kSectorSize) is checked against this one by the store itself, which refuses a region whose
  // sector is not what it laid the store out for.
  static constexpr std::uint32_t kSectorSize = 4096;

  bool inside(std::uint32_t offset, std::uint32_t count) const;

  const esp_partition_t* partition_ = nullptr;
};

}  // namespace platform
