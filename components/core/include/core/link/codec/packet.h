// A packet and a frame, spec/LINK-PROTOCOL.md §2.
//
//   packet   type : u8 | seq : u8 | payload : 0..240 | crc : u16 (little-endian, over the rest)
//   frame    the packet in COBS, then one 0x00
//
// buildFrame() is the pure function: the same bytes the fixtures' `frame` holds for every case.
// The one 0x00 a sender puts in front of a frame after 100 ms of idle belongs to the sender, not
// to the frame, and lives in FrameWriter (codec/frame.h).
//
// Nothing of a frame is interpreted before its CRC has passed (§2), which is why parsePacket()
// gives back a payload and not fields: decoding fields is codec/messages.h, and it is only ever
// called on a payload that came out of here.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/view.h"

namespace core {
namespace link {

struct Packet {
  std::uint8_t type = 0;  // the wire type: the request's, or the request's with bit 7 set (§3)
  std::uint8_t seq = 0;
  Bytes payload;  // points into the bytes parsePacket() was given
};

// type, seq and payload -> the packet's bytes, CRC included. Returns the number of bytes written
// (4 to 244) or 0 when the payload is over 240 bytes or `out` is too small.
std::size_t buildPacket(std::uint8_t type, std::uint8_t seq, Bytes payload, std::uint8_t* out,
                        std::size_t out_size);

// The same, as a frame: COBS and the terminating 0x00. Returns the bytes written (6 to 246) or 0.
std::size_t buildFrame(std::uint8_t type, std::uint8_t seq, Bytes payload, std::uint8_t* out,
                       std::size_t out_size);

// Why a packet was refused. §11's `invalid` fixtures give a reason as well; the words are not the
// same and need not be - a receiver has to drop and count the frame, not agree on the word.
enum class PacketError : std::uint8_t {
  kNone = 0,
  kLength,  // under 4 bytes, so there is no room for a CRC, or over 244
  kCrc,     // the checksum does not match
};

// A packet's bytes -> its fields. False on the length or the CRC; `out` is untouched then.
bool parsePacket(Bytes packet, Packet* out, PacketError* error = nullptr);

}  // namespace link
}  // namespace core
