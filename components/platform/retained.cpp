#include "platform/retained.h"

#include <cstring>

#include "esp_attr.h"
#include "esp_rom_crc.h"

namespace platform {

namespace {

using interfaces::kRetainedRecordMax;
using interfaces::RetainedSlot;

constexpr std::size_t kSlotCount = static_cast<std::size_t>(RetainedSlot::kCount);

// "TWRT" and a layout version: bumped if this structure changes, so that an older firmware's
// bytes are never read as this one's.
constexpr std::uint32_t kMagic = 0x54575201u;

struct Slot {
  std::uint32_t magic;
  std::uint32_t size;
  std::uint8_t data[kRetainedRecordMax];
  std::uint32_t crc;
};

// No initialiser, on purpose: RTC_NOINIT_ATTR means "leave these bytes alone at boot"
// (esp_attr.h, .rtc_noinit), and an initialised object there would be zeroed or refuse to link.
RTC_NOINIT_ATTR Slot g_slots[kSlotCount];

// CRC-32 over magic, size and data (esp_rom_crc32_le: the ROM's table-driven CRC-32/ISO-HDLC,
// the same algorithm as core/image's crc32 - checked against its "123456789" value in the host
// tests there; here nothing cares which, only that store and load agree).
std::uint32_t checksum(const Slot& slot) {
  return esp_rom_crc32_le(0, reinterpret_cast<const std::uint8_t*>(&slot),
                          static_cast<std::uint32_t>(offsetof(Slot, crc)));
}

}  // namespace

bool RtcRetained::load(RetainedSlot slot, void* out, std::size_t size) {
  const std::size_t index = static_cast<std::size_t>(slot);
  if (index >= kSlotCount || size > kRetainedRecordMax) return false;
  const Slot& s = g_slots[index];
  if (s.magic != kMagic || s.size != size || checksum(s) != s.crc) return false;
  std::memcpy(out, s.data, size);
  return true;
}

bool RtcRetained::store(RetainedSlot slot, const void* data, std::size_t size) {
  const std::size_t index = static_cast<std::size_t>(slot);
  if (index >= kSlotCount || size > kRetainedRecordMax) return false;
  Slot& s = g_slots[index];
  s.magic = kMagic;
  s.size = static_cast<std::uint32_t>(size);
  std::memset(s.data, 0, sizeof s.data);
  std::memcpy(s.data, data, size);
  s.crc = checksum(s);
  return true;
}

}  // namespace platform
