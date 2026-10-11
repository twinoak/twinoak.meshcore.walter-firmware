// COBS, spec/LINK-PROTOCOL.md §2. The reference is spec/tools/linkref.py (cobs_encode,
// cobs_decode) and the host test holds both directions against every vector of
// spec/fixtures/link/link-v1.json.
#include "core/link/codec/cobs.h"

#include "core/link/codec/wire.h"

namespace core {
namespace link {
namespace {

// A block's code byte is the number of data bytes in it plus one, so 0xFF would mean 254 data
// bytes with no zero after them. §2: a packet is at most 244 bytes, so that cannot arise here.
constexpr std::size_t kBlockFull = 255;

}  // namespace

std::size_t cobsEncode(Bytes raw, std::uint8_t* out, std::size_t out_size) {
  if (raw.data == nullptr && raw.size != 0) {
    return 0;
  }
  if (raw.size > kMaxPacket || out == nullptr || out_size < 1) {
    return 0;  // longer than a packet: not something this link sends, and the one input that
               // could need the 0xFF code
  }
  std::size_t code_at = 0;  // where this block's code byte goes; filled in when the block ends
  std::size_t written = 1;
  out[0] = 1;
  for (std::size_t i = 0; i < raw.size; ++i) {
    if (raw.data[i] == 0) {
      out[code_at] = static_cast<std::uint8_t>(written - code_at);
      if (written >= out_size) {
        return 0;
      }
      code_at = written;
      out[written++] = 1;  // a placeholder: the next block's code byte
      continue;
    }
    if (written >= out_size) {
      return 0;
    }
    out[written++] = raw.data[i];
    if (written - code_at == kBlockFull) {
      return 0;  // unreachable for a packet of this link; §2 lets an encoder not do 0xFF
    }
  }
  out[code_at] = static_cast<std::uint8_t>(written - code_at);
  return written;
}

CobsDecoded cobsDecode(Bytes encoded, std::uint8_t* out, std::size_t out_size) {
  CobsDecoded result;
  if (encoded.data == nullptr || encoded.size == 0) {
    result.error = CobsError::kEmpty;
    return result;
  }
  std::size_t i = 0;
  std::size_t written = 0;
  while (i < encoded.size) {
    const std::uint8_t code = encoded.data[i];
    if (code == 0) {
      result.error = CobsError::kZeroInside;  // the terminator cannot appear inside a frame
      return result;
    }
    if (code == 0xFF) {
      result.error = CobsError::kCodeFf;  // §2: a decoder that meets it refuses the frame
      return result;
    }
    ++i;
    const std::size_t end = i + code - 1;  // one past this block's data
    if (end > encoded.size) {
      result.error = CobsError::kRunsPastEnd;
      return result;
    }
    for (; i < end; ++i) {
      if (encoded.data[i] == 0) {
        result.error = CobsError::kZeroInside;
        return result;
      }
      if (written >= out_size || out == nullptr) {
        result.error = CobsError::kNoRoom;
        return result;
      }
      out[written++] = encoded.data[i];
    }
    if (i < encoded.size) {
      // Not the last block, so this one stood for a zero.
      if (written >= out_size || out == nullptr) {
        result.error = CobsError::kNoRoom;
        return result;
      }
      out[written++] = 0;
    }
  }
  result.ok = true;
  result.size = written;
  return result;
}

}  // namespace link
}  // namespace core
