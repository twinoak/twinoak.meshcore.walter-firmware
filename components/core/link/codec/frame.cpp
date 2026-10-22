// The receiver and the writer, spec/LINK-PROTOCOL.md §2. The receiver's behaviour is checked
// against the `stream` and `invalid` fixtures of spec/fixtures/link/link-v1.json, fed whole and
// one byte at a time.
#include "core/link/codec/frame.h"

#include "core/link/codec/cobs.h"

namespace core {
namespace link {
namespace {

// Saturating: a counter that has stopped being exact must not start being wrong.
void bump(std::uint32_t* counter) {
  if (*counter != 0xFFFFFFFFu) {
    ++*counter;
  }
}

}  // namespace

void FrameReceiver::count(Drop reason) {
  bump(&dropped_);
  bump(&per_reason_[static_cast<std::size_t>(reason)]);
  last_drop_ = reason;
}

std::uint32_t FrameReceiver::droppedFor(Drop reason) const {
  return per_reason_[static_cast<std::size_t>(reason)];
}

void FrameReceiver::wipe() {
  auto* collected = static_cast<volatile std::uint8_t*>(collected_);
  for (std::size_t i = 0; i < sizeof collected_; ++i) collected[i] = 0;
  auto* decoded = static_cast<volatile std::uint8_t*>(decoded_);
  for (std::size_t i = 0; i < sizeof decoded_; ++i) decoded[i] = 0;
  have_ = 0;
  overflowed_ = false;
  packet_ = Packet{};
}

void FrameReceiver::discardPartial() {
  have_ = 0;
  overflowed_ = false;
}

FrameReceiver::Fed FrameReceiver::push(std::uint8_t byte) {
  if (byte != 0) {
    if (have_ < sizeof(collected_)) {
      collected_[have_++] = byte;
    } else {
      overflowed_ = true;  // counted when the terminator arrives, so a burst of noise is one drop
    }
    return Fed::kNothing;
  }

  // A terminator: whatever was collected is a whole frame, or nothing.
  const std::size_t have = have_;
  const bool overflowed = overflowed_;
  have_ = 0;
  overflowed_ = false;

  if (overflowed) {
    count(Drop::kOverflow);
    return Fed::kDropped;
  }
  if (have == 0) {
    bump(&empty_frames_);
    return Fed::kEmptyFrame;  // §2: zero bytes collected, ignore - not an error
  }

  const CobsDecoded decoded = cobsDecode(bytes(collected_, have), decoded_, sizeof(decoded_));
  if (!decoded.ok) {
    // kNoRoom means the packet would be longer than 244 bytes: a length, not a COBS, complaint.
    count(decoded.error == CobsError::kNoRoom ? Drop::kLength : Drop::kCobs);
    return Fed::kDropped;
  }

  PacketError error = PacketError::kNone;
  if (!parsePacket(bytes(decoded_, decoded.size), &packet_, &error)) {
    count(error == PacketError::kCrc ? Drop::kCrc : Drop::kLength);
    return Fed::kDropped;
  }
  bump(&delivered_);
  return Fed::kPacket;
}

std::size_t FrameWriter::write(std::uint8_t type, std::uint8_t seq, Bytes payload,
                               std::uint8_t* out, std::size_t out_size) {
  const std::uint64_t now = clock_.nowMs();
  // Nothing sent yet, or a clock that restarted under us (esp_timer does at every wake from deep
  // sleep - interfaces/clock.h): the line counts as idle, which costs one byte and ends whatever
  // the receiver may hold.
  const bool idle = !written_ || now < last_ms_ || (now - last_ms_) >= kIdleMs;
  if (out == nullptr || out_size < (idle ? 1u : 0u) + 1u) {
    return 0;
  }
  const std::size_t at = idle ? 1u : 0u;
  const std::size_t frame = buildFrame(type, seq, payload, out + at, out_size - at);
  if (frame == 0) {
    return 0;  // out is untouched past the prefix, which nobody has been told to send
  }
  if (idle) {
    out[0] = 0;
  }
  prefixed_ = idle;
  written_ = true;
  last_ms_ = now;
  return at + frame;
}

}  // namespace link
}  // namespace core
