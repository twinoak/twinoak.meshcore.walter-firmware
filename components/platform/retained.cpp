#include "platform/retained.h"

#include <cstring>

#include "esp_attr.h"
#include "esp_rom_crc.h"

namespace platform {

namespace {

using interfaces::kRetainedSlotBytes;
using interfaces::RetainedSlot;

constexpr std::size_t kSlotCount = static_cast<std::size_t>(RetainedSlot::kCount);

// "TWRT" and a layout version: bumped if this structure changes, so that an older firmware's
// bytes are never read as this one's.
constexpr std::uint32_t kMagic = 0x54575201u;

// What a slot costs on top of its room: the magic word, the record's size and the checksum.
constexpr std::size_t kSlotOverhead = 3 * sizeof(std::uint32_t);

// **Each slot is as big as its record needs** (task p15-06, interfaces/retained.h's
// kRetainedSlotBytes). Until that task this was an array of one fixed-size structure and every
// slot cost kRetainedRecordMax bytes of RTC memory whatever its record held; RTC SLOW had 36
// bytes left and a ninth slot of 128 would not fit in them. So the slots are laid out one after
// another in one flat array, each `kSlotOverhead + kRetainedSlotBytes[i]` bytes long, in the
// order of the enumeration.
//
// **The first seven slots are byte for byte where and what they were**, which is the whole reason
// the layout is written this way rather than packed from scratch: their capacity is unchanged, so
// their offsets, their sizes and the span the checksum covers are unchanged, and the records of a
// unit in service are read back by the release that carries this. Slot 7's room shrank to its
// record's size and slot 8 is new, so those two begin again - once.
constexpr std::size_t slotOffset(std::size_t index) {
  std::size_t at = 0;
  for (std::size_t i = 0; i < index; ++i) at += kSlotOverhead + kRetainedSlotBytes[i];
  return at;
}
constexpr std::size_t kTotalBytes = slotOffset(kSlotCount);

// Where each of a slot's four fields sits inside its room. Written by hand rather than as a
// structure, because a structure of a fixed-size array cannot be sized per slot and because the
// layout is what the checksum and every stored record depend on.
//
//   +0   magic  u32
//   +4   size   u32  the record's own size, which is what a load is keyed on
//   +8   data   `kRetainedSlotBytes[slot]` bytes
//   +8+n crc    u32  over the magic, the size and the whole of the room
constexpr std::size_t kMagicAt = 0;
constexpr std::size_t kSizeAt = 4;
constexpr std::size_t kDataAt = 8;

// No initialiser, on purpose: RTC_NOINIT_ATTR means "leave these bytes alone at boot"
// (esp_attr.h, .rtc_noinit), and an initialised object there would be zeroed or refuse to link.
RTC_NOINIT_ATTR std::uint8_t g_slots[kTotalBytes];

std::uint32_t get32(const std::uint8_t* at) {
  std::uint32_t v = 0;
  std::memcpy(&v, at, sizeof v);
  return v;
}

void put32(std::uint8_t* at, std::uint32_t v) { std::memcpy(at, &v, sizeof v); }

// CRC-32 over magic, size and data (esp_rom_crc32_le: the ROM's table-driven CRC-32/ISO-HDLC,
// the same algorithm as core/image's crc32 - checked against its "123456789" value in the host
// tests there; here nothing cares which, only that store and load agree).
std::uint32_t checksum(const std::uint8_t* slot, std::size_t room) {
  return esp_rom_crc32_le(0, slot, static_cast<std::uint32_t>(kDataAt + room));
}

}  // namespace

bool RtcRetained::load(RetainedSlot slot, void* out, std::size_t size) {
  const std::size_t index = static_cast<std::size_t>(slot);
  if (index >= kSlotCount) return false;
  const std::size_t room = kRetainedSlotBytes[index];
  if (size > room) return false;
  std::uint8_t* s = g_slots + slotOffset(index);
  if (get32(s + kMagicAt) != kMagic || get32(s + kSizeAt) != size) return false;
  if (checksum(s, room) != get32(s + kDataAt + room)) return false;
  std::memcpy(out, s + kDataAt, size);
  return true;
}

bool RtcRetained::store(RetainedSlot slot, const void* data, std::size_t size) {
  const std::size_t index = static_cast<std::size_t>(slot);
  if (index >= kSlotCount) return false;
  const std::size_t room = kRetainedSlotBytes[index];
  if (size > room) return false;
  std::uint8_t* s = g_slots + slotOffset(index);
  put32(s + kMagicAt, kMagic);
  put32(s + kSizeAt, static_cast<std::uint32_t>(size));
  std::memset(s + kDataAt, 0, room);
  std::memcpy(s + kDataAt, data, size);
  put32(s + kDataAt + room, checksum(s, room));
  return true;
}

}  // namespace platform
