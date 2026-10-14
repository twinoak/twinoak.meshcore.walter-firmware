#include "platform/flash_partition.h"

#include <cstdio>

namespace platform {

bool FlashPartition::begin(const char* name, esp_partition_subtype_t subtype) {
  partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, name);
  if (partition_ == nullptr) {
    std::printf("the partition '%s' (data, subtype 0x%02x) is not in the table\n",
                name != nullptr ? name : "?", static_cast<unsigned>(subtype));
    return false;
  }
  if (partition_->size % kSectorSize != 0) {
    std::printf("the partition '%s' is not a whole number of sectors\n", partition_->label);
    partition_ = nullptr;
    return false;
  }
  return true;
}

bool FlashPartition::inside(std::uint32_t offset, std::uint32_t count) const {
  if (partition_ == nullptr) return false;
  return offset <= partition_->size && count <= partition_->size - offset;
}

bool FlashPartition::read(std::uint32_t offset, void* out, std::uint32_t count) const {
  if (!inside(offset, count) || out == nullptr) return false;
  if (count == 0) return true;
  return esp_partition_read(partition_, offset, out, count) == ESP_OK;
}

bool FlashPartition::write(std::uint32_t offset, const void* data, std::uint32_t count) {
  if (!inside(offset, count) || data == nullptr) return false;
  if (count == 0) return true;
  return esp_partition_write(partition_, offset, data, count) == ESP_OK;
}

bool FlashPartition::erase(std::uint32_t offset, std::uint32_t count) {
  if (!inside(offset, count)) return false;
  if (offset % kSectorSize != 0 || count % kSectorSize != 0) return false;
  if (count == 0) return true;
  return esp_partition_erase_range(partition_, offset, count) == ESP_OK;
}

}  // namespace platform
