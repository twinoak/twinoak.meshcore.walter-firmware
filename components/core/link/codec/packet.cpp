// Packets and frames, spec/LINK-PROTOCOL.md §2; spec/tools/linkref.py's make_packet, make_frame
// and parse_frame are the reference.
#include "core/link/codec/packet.h"

#include "core/link/codec/cobs.h"
#include "core/link/codec/crc16.h"
#include "core/link/codec/wire.h"

namespace core {
namespace link {

std::size_t buildPacket(std::uint8_t type, std::uint8_t seq, Bytes payload, std::uint8_t* out,
                        std::size_t out_size) {
  if (payload.size > kMaxPayload || (payload.data == nullptr && payload.size != 0)) {
    return 0;
  }
  const std::size_t size = payload.size + 4;
  if (out == nullptr || out_size < size) {
    return 0;
  }
  out[0] = type;
  out[1] = seq;
  for (std::size_t i = 0; i < payload.size; ++i) {
    out[2 + i] = payload.data[i];
  }
  const std::uint16_t crc = crc16(out, payload.size + 2);  // over type, seq and payload
  out[payload.size + 2] = static_cast<std::uint8_t>(crc & 0xFFu);  // little-endian (§2)
  out[payload.size + 3] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
  return size;
}

std::size_t buildFrame(std::uint8_t type, std::uint8_t seq, Bytes payload, std::uint8_t* out,
                       std::size_t out_size) {
  std::uint8_t packet[kMaxPacket];
  const std::size_t packet_size = buildPacket(type, seq, payload, packet, sizeof(packet));
  if (packet_size == 0) {
    return 0;
  }
  if (out == nullptr || out_size < cobsEncodedSize(packet_size) + 1) {
    return 0;
  }
  const std::size_t encoded = cobsEncode(bytes(packet, packet_size), out, out_size - 1);
  if (encoded == 0) {
    return 0;
  }
  out[encoded] = 0;  // the terminator, and the only 0x00 in a frame
  return encoded + 1;
}

bool parsePacket(Bytes packet, Packet* out, PacketError* error) {
  const auto fail = [error](PacketError why) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  };
  if (packet.data == nullptr || packet.size < kMinPacket || packet.size > kMaxPacket) {
    return fail(PacketError::kLength);
  }
  const std::uint16_t want =
      static_cast<std::uint16_t>(packet.data[packet.size - 2] |
                                 (static_cast<std::uint16_t>(packet.data[packet.size - 1]) << 8));
  if (crc16(packet.data, packet.size - 2) != want) {
    return fail(PacketError::kCrc);
  }
  if (error != nullptr) {
    *error = PacketError::kNone;
  }
  if (out != nullptr) {
    out->type = packet.data[0];
    out->seq = packet.data[1];
    out->payload = bytes(packet.data + 2, packet.size - 4);
  }
  return true;
}

}  // namespace link
}  // namespace core
