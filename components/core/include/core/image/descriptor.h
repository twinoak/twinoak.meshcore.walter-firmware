// The 96 bytes a firmware image carries about itself: spec/FIRMWARE-MANIFEST.md §4, and
// spec/tools/manifestref.py (make_descriptor, parse_descriptor, find_descriptor) as the reference
// these must agree with byte for byte - host_test/core/image/test_descriptor.cpp holds them
// against every entry of spec/fixtures/manifest/index.json.
//
// Three things live here, and all three are logic: no ESP-IDF include, no heap, no formatting.
//
//   build()  the bytes - used at compile time for this firmware's own descriptor (this_image.h)
//            and at run time by the tests and by the release tooling's fixtures
//   parse()  96 bytes in, fields out, magic, length and CRC-32 checked
//   find()   one image in, the offset and fields of the descriptor in it out, and how many valid
//            ones there were: exactly one is the only acceptable answer (§4, and §5 check 5).
//            This is what phase 2 reads a radio's image with.
//
// Everything is constexpr-capable and takes no exceptions (core/ is compiled -fno-exceptions):
// a text that does not fit its field is an unsuccessful Built, which at compile time is a
// static_assert and at run time a false.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace image {

// FIRMWARE-MANIFEST.md §4: `magic` is ASCII "TWOKFWD1"; the descriptor is 96 bytes and sits on a
// 4-byte boundary. Not a string literal: it is compared and copied as bytes, and a literal in
// .rodata would be a second occurrence of the magic in the image.
constexpr std::uint8_t kMagic[8] = {'T', 'W', 'O', 'K', 'F', 'W', 'D', '1'};
constexpr std::size_t kDescriptorSize = 96;
constexpr std::size_t kVersionSize = 32;
constexpr std::size_t kBoardSize = 24;
constexpr std::size_t kBandSize = 8;

// The `role` byte. The same numbers as manifestref.ROLES; `manager` (16) is the Walter's.
enum class Role : std::uint8_t {
  kUnknown = 0,
  kRepeater = 1,
  kBridge = 2,
  kScanner = 3,
  kManager = 16,
};

// `slot` 0xFF: this image is for any radio position (§4).
constexpr std::uint8_t kAnySlot = 0xFF;

// What the descriptor says, with the three strings already terminated. A plain aggregate so that
// a caller can write one as an initialiser list at compile time.
struct Fields {
  Role role = Role::kUnknown;
  std::uint8_t slot = kAnySlot;
  std::uint8_t link_major = 0;
  std::uint8_t link_minor = 0;
  std::uint16_t flags = 0;
  char version[kVersionSize] = {};
  char board[kBoardSize] = {};
  char band[kBandSize] = {};
};

// The bytes, as they are in the image. A byte array and not a packed structure: §4 is a byte
// layout, and a structure with the same layout would depend on the compiler agreeing about
// padding and about the order of its bit fields.
struct Descriptor {
  std::uint8_t bytes[kDescriptorSize] = {};
};

struct Built {
  bool ok = false;             // false: one of the three texts does not fit its field, terminator included
  Descriptor descriptor = {};  // all zero when !ok
};

// CRC-32 as in IEEE 802.3 and zlib (reflected, polynomial 0xEDB88320, initial and final
// 0xFFFFFFFF): what §4 says bytes 92..95 are, over bytes 0..91. Bitwise, so there is no table in
// the image; 92 bytes is 736 iterations, once, at build time.
constexpr std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

namespace detail {

// A field of `size` bytes holding `text` in ASCII, zero-padded, with at least one terminating zero
// (§4) - so the text itself is at most size-1 bytes. False if it is longer, or not ASCII.
constexpr bool put_text(std::uint8_t* out, std::size_t size, const char* text) {
  std::size_t i = 0;
  for (; text != nullptr && text[i] != 0; ++i) {
    if (i + 1 >= size) {
      return false;  // no room left for the terminator
    }
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0x80u) {
      return false;  // §4 says ASCII
    }
    out[i] = static_cast<std::uint8_t>(c);
  }
  for (; i < size; ++i) {
    out[i] = 0;
  }
  return true;
}

// The other direction: the field's bytes up to the first zero, always terminated.
constexpr void take_text(char* out, const std::uint8_t* in, std::size_t size) {
  std::size_t i = 0;
  for (; i + 1 < size && in[i] != 0; ++i) {
    out[i] = static_cast<char>(in[i]);
  }
  for (; i < size; ++i) {
    out[i] = 0;
  }
}

}  // namespace detail

// The bytes of §4's table, in its order. Offsets: 0 magic, 8 length, 10 role, 11 slot, 12 and 13
// the link version, 14 flags, 16 version, 48 board, 72 band, 80 twelve reserved zeroes, 92 crc32.
// The three texts come in as pointers, not as the arrays of Fields, so that a caller can pass a
// macro, a literal or a string it was given at run time.
constexpr Built build(Role role, std::uint8_t slot, std::uint8_t link_major, std::uint8_t link_minor,
                      std::uint16_t flags, const char* version, const char* board, const char* band) {
  Built built;
  std::uint8_t* b = built.descriptor.bytes;
  for (std::size_t i = 0; i < sizeof(kMagic); ++i) {
    b[i] = kMagic[i];
  }
  b[8] = static_cast<std::uint8_t>(kDescriptorSize & 0xFFu);  // length, little-endian
  b[9] = static_cast<std::uint8_t>((kDescriptorSize >> 8) & 0xFFu);
  b[10] = static_cast<std::uint8_t>(role);
  b[11] = slot;
  b[12] = link_major;
  b[13] = link_minor;
  b[14] = static_cast<std::uint8_t>(flags & 0xFFu);
  b[15] = static_cast<std::uint8_t>((flags >> 8) & 0xFFu);
  if (!detail::put_text(b + 16, kVersionSize, version) ||
      !detail::put_text(b + 48, kBoardSize, board) ||
      !detail::put_text(b + 72, kBandSize, band)) {
    return Built{};  // ok stays false and the bytes stay zero: nothing half-built escapes
  }
  for (std::size_t i = 80; i < 92; ++i) {
    b[i] = 0;  // reserved, zero
  }
  const std::uint32_t crc = crc32(b, 92);
  b[92] = static_cast<std::uint8_t>(crc & 0xFFu);
  b[93] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
  b[94] = static_cast<std::uint8_t>((crc >> 16) & 0xFFu);
  b[95] = static_cast<std::uint8_t>((crc >> 24) & 0xFFu);
  built.ok = true;
  return built;
}

// The same, from a Fields - what parse() gives back, so that a round trip is one line.
constexpr Built build(const Fields& fields) {
  return build(fields.role, fields.slot, fields.link_major, fields.link_minor, fields.flags,
               fields.version, fields.board, fields.band);
}

// 96 bytes in, fields out. False when the magic, the length or the CRC is not what §4 says - the
// three things that keep a run of bytes which happens to spell the magic from being a descriptor.
constexpr bool parse(const std::uint8_t* bytes, std::size_t length, Fields* out) {
  if (bytes == nullptr || length < kDescriptorSize) {
    return false;
  }
  for (std::size_t i = 0; i < sizeof(kMagic); ++i) {
    if (bytes[i] != kMagic[i]) {
      return false;
    }
  }
  const std::uint16_t declared =
      static_cast<std::uint16_t>(bytes[8] | (static_cast<std::uint16_t>(bytes[9]) << 8));
  if (declared != kDescriptorSize) {
    return false;
  }
  const std::uint32_t want = static_cast<std::uint32_t>(bytes[92]) |
                             (static_cast<std::uint32_t>(bytes[93]) << 8) |
                             (static_cast<std::uint32_t>(bytes[94]) << 16) |
                             (static_cast<std::uint32_t>(bytes[95]) << 24);
  if (want != crc32(bytes, 92)) {
    return false;
  }
  if (out != nullptr) {
    out->role = static_cast<Role>(bytes[10]);
    out->slot = bytes[11];
    out->link_major = bytes[12];
    out->link_minor = bytes[13];
    out->flags = static_cast<std::uint16_t>(bytes[14] | (static_cast<std::uint16_t>(bytes[15]) << 8));
    detail::take_text(out->version, bytes + 16, kVersionSize);
    detail::take_text(out->board, bytes + 48, kBoardSize);
    detail::take_text(out->band, bytes + 72, kBandSize);
  }
  return true;
}

struct Found {
  std::size_t count = 0;   // valid descriptors in the image. Only 1 may be acted on (§4, §5.5)
  std::size_t offset = 0;  // of the first one, when count > 0
  Fields fields = {};      // of the first one, when count > 0
};

// Searches the image for descriptors on a 4-byte boundary and counts the valid ones. The caller
// refuses the image unless count == 1: none means the file is not one of ours, two means it cannot
// be trusted to say what it is (§4). Every one is counted, not just the first two, so that a log
// line can say how many there were.
constexpr Found find(const std::uint8_t* image, std::size_t length) {
  Found found;
  if (image == nullptr || length < kDescriptorSize) {
    return found;
  }
  for (std::size_t i = 0; i + kDescriptorSize <= length; i += 4) {
    Fields fields;
    if (parse(image + i, kDescriptorSize, &fields)) {
      if (found.count == 0) {
        found.offset = i;
        found.fields = fields;
      }
      ++found.count;
    }
  }
  return found;
}

// The names manifestref.ROLES uses, for a console line and for holding a descriptor against a
// manifest entry's `role`. A number this firmware does not know is nullptr - never a name it
// might be mistaken for, and never a formatted buffer (core/ has no heap and no sprintf).
constexpr const char* role_name(Role role) {
  switch (role) {
    case Role::kUnknown: return "unknown";
    case Role::kRepeater: return "repeater";
    case Role::kBridge: return "bridge";
    case Role::kScanner: return "scanner";
    case Role::kManager: return "manager";
  }
  return nullptr;
}

}  // namespace image
}  // namespace core
