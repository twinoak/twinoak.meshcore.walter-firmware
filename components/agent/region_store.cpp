#include "walter_agent/region_store.h"

namespace agent {

namespace {

std::uint32_t saturate(std::uint32_t value) { return value == 0xFFFFFFFFu ? value : value + 1; }

}  // namespace

RegionStore::RegionStore(interfaces::FlashRegion& region, std::uint32_t offset, std::uint32_t length)
    : region_(region), offset_(offset) {
  sector_ = region.sectorSize() != 0 ? region.sectorSize() : 4096;
  const std::uint64_t end = static_cast<std::uint64_t>(offset) + length;
  // A window on a sector boundary, whole sectors long, inside the region, and worth having.
  if (offset % sector_ != 0 || end > region.size() || length < kRegionStoreLeastBytes) {
    capacity_ = 0;
    return;
  }
  capacity_ = length - (length % sector_);
}

bool RegionStore::write(const std::uint8_t* image, std::size_t len) {
  if (capacity_ == 0 || image == nullptr || len == 0 || len > capacity_) {
    counters_.refused = saturate(counters_.refused);
    return false;
  }
  // The sectors the image covers, and no more: an image that shrank leaves stale bytes after it,
  // which the library's own length field steps over.
  const std::uint32_t sectors = static_cast<std::uint32_t>((len + sector_ - 1) / sector_);
  if (!region_.erase(offset_, sectors * sector_)) {
    counters_.refused = saturate(counters_.refused);
    return false;
  }
  counters_.erases += sectors;
  if (!region_.write(offset_, image, static_cast<std::uint32_t>(len))) {
    counters_.refused = saturate(counters_.refused);
    return false;
  }
  counters_.writes = saturate(counters_.writes);
  counters_.bytes_written = static_cast<std::uint32_t>(len);
  return true;
}

std::size_t RegionStore::read(std::uint8_t* out, std::size_t cap) {
  if (capacity_ == 0 || out == nullptr || cap == 0) return 0;
  counters_.reads = saturate(counters_.reads);
  const std::uint32_t n = cap < capacity_ ? static_cast<std::uint32_t>(cap) : capacity_;
  if (!region_.read(offset_, out, n)) return 0;
  return n;
}

}  // namespace agent
