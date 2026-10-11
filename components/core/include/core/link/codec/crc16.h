// CRC-16/CCITT-FALSE, the checksum of a packet: spec/LINK-PROTOCOL.md §2 - polynomial 0x1021,
// initial value 0xFFFF, no reflection of the input or the output, no final XOR. Its check value
// over the ASCII string "123456789" is 0x29B1, which §2 gives and the static_assert below holds
// it to at compile time.
//
// Bitwise and constexpr, like the descriptor's CRC-32 (core/image/descriptor.h): a 512-byte table
// would sit in the image for 244 bytes of work per frame, and 1952 iterations at 160 MHz is a few
// microseconds against a frame that takes 21 ms to clock out at 115200 baud.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/view.h"

namespace core {
namespace link {

constexpr std::uint16_t kCrc16Init = 0xFFFF;
constexpr std::uint16_t kCrc16Poly = 0x1021;

constexpr std::uint16_t crc16Byte(std::uint16_t crc, std::uint8_t byte) {
  crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8));
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc & 0x8000u) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ kCrc16Poly)
                               : static_cast<std::uint16_t>(crc << 1);
  }
  return crc;
}

// The running CRC, so that a caller can checksum a header and a payload without joining them
// first - which is what building a packet does (codec/packet.cpp).
constexpr std::uint16_t crc16Update(std::uint16_t crc, const std::uint8_t* data, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    crc = crc16Byte(crc, data[i]);
  }
  return crc;
}

constexpr std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
  return crc16Update(kCrc16Init, data, size);
}

constexpr std::uint16_t crc16(Bytes data) { return crc16(data.data, data.size); }

namespace detail {

// The same over text, for the one check value §2 gives: a string literal cannot be cast to
// const std::uint8_t* in a constant expression (reinterpret_cast is not allowed in one), so the
// characters are widened one at a time.
constexpr std::uint16_t crc16OfText(const char* text) {
  std::uint16_t crc = kCrc16Init;
  for (std::size_t i = 0; text[i] != 0; ++i) {
    crc = crc16Byte(crc, static_cast<std::uint8_t>(text[i]));
  }
  return crc;
}

}  // namespace detail

static_assert(detail::crc16OfText("123456789") == 0x29B1,
              "CRC-16/CCITT-FALSE over \"123456789\" is 0x29B1 (LINK-PROTOCOL.md §2)");

}  // namespace link
}  // namespace core
