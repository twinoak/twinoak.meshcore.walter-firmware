// Consistent Overhead Byte Stuffing, as spec/LINK-PROTOCOL.md §2 uses it: the packet encoded so
// that no 0x00 is left in it, which makes 0x00 the frame terminator and nothing else.
//
// Because a packet is at most 244 bytes, no run inside one reaches 254 bytes: the overhead is
// exactly one byte and the 0xFF code of general COBS never occurs. §2 says an encoder need not
// implement it and a decoder that meets it refuses the frame - so encode() refuses an input
// longer than a packet (the only way a 254-byte run could arise) and decode() refuses the 0xFF
// code, a 0x00 inside the data, and a block that runs past the end.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/view.h"

namespace core {
namespace link {

// The largest encoding of `size` raw bytes: one code byte per block, and one block per 254 bytes
// of data plus the final one. For the packets this link carries, size + 1.
constexpr std::size_t cobsEncodedSize(std::size_t size) { return size + 1 + size / 254; }

// raw -> encoded, into `out`. Returns the number of bytes written, or 0 when `raw` is longer than
// a packet or `out` is too small. 0 is never a valid encoding (even an empty input encodes to the
// single byte 0x01), so it is an unambiguous failure.
std::size_t cobsEncode(Bytes raw, std::uint8_t* out, std::size_t out_size);

// Why a decoding was refused. The reason is this firmware's own word for it; §11 says a receiver
// has to drop and count a bad frame, not agree on the word.
enum class CobsError : std::uint8_t {
  kNone = 0,
  kEmpty,        // nothing to decode: an empty frame, which is not an error to a receiver (§2)
  kZeroInside,   // a 0x00 in the encoded bytes: the terminator cannot appear inside a frame
  kRunsPastEnd,  // a block whose length reaches beyond the bytes there are
  kCodeFf,       // the 0xFF code of general COBS, which a packet of this link never produces
  kNoRoom,       // `out` is smaller than the decoding, so the bytes are not a packet of ours
};

struct CobsDecoded {
  bool ok = false;
  std::size_t size = 0;  // bytes written to `out` when ok
  CobsError error = CobsError::kNone;
};

// encoded (the frame without its terminating 0x00) -> raw, into `out`.
CobsDecoded cobsDecode(Bytes encoded, std::uint8_t* out, std::size_t out_size);

}  // namespace link
}  // namespace core
