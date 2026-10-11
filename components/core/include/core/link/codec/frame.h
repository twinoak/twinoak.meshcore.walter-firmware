// The two ends of the wire, spec/LINK-PROTOCOL.md §2 - and nothing of a UART: the receiver takes
// bytes in whatever chunking they arrive in and gives back packets, the writer builds the bytes of
// one frame into a buffer. Task 02 puts them on GPIO7/8.
//
// The receiver's rules, straight from §2:
//
//   - collect bytes until 0x00; zero bytes collected means an empty frame, which is ignored and is
//     not an error (a sender prefixes one after an idle line, on purpose);
//   - decode COBS, check the length (4..244) and the CRC. A frame that fails any of these is
//     dropped and counted, and nothing is sent in reply - the sender's timeout is the recovery;
//   - a receive buffer needs 246 bytes; a frame that overflows it is dropped and counted when its
//     terminator arrives - so one long burst of noise is one drop, not one per byte;
//   - never interpret bytes of a frame before its CRC has passed.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/packet.h"
#include "core/link/codec/view.h"
#include "core/link/codec/wire.h"
#include "interfaces/clock.h"

namespace core {
namespace link {

class FrameReceiver {
 public:
  // What a byte did.
  enum class Fed : std::uint8_t {
    kNothing,     // collected, or discarded because the frame is already too long
    kPacket,      // a frame ended and its CRC passed: packet() is it
    kEmptyFrame,  // a terminator with nothing before it (§2: ignore, not an error)
    kDropped,     // a frame ended and was refused: dropped() has counted it
  };

  // Why the frame that was dropped was dropped. This firmware's own words - §11: a receiver has
  // to drop and count the frame, not agree on the word.
  enum class Drop : std::uint8_t {
    kNone = 0,
    kOverflow,  // more bytes than the buffer holds arrived before a terminator
    kCobs,      // a zero inside the frame, a block that runs past the end, or the 0xFF code
    kLength,    // the packet is under 4 bytes or over 244
    kCrc,       // the checksum does not match
  };

  // One byte in. After kPacket, packet() holds it until the next call.
  Fed push(std::uint8_t byte);

  // Any chunking in: every complete packet is handed to on_packet(const Packet&) as it appears.
  // Returns how many there were. A template and not a std::function: core/ has no heap.
  template <typename OnPacket>
  std::size_t feed(Bytes data, OnPacket on_packet) {
    std::size_t delivered = 0;
    for (std::size_t i = 0; i < data.size; ++i) {
      if (push(data.data[i]) == Fed::kPacket) {
        on_packet(packet());
        ++delivered;
      }
    }
    return delivered;
  }

  // Valid after push() returned kPacket, until the next push(). Its payload points into this
  // object's own buffer.
  const Packet& packet() const { return packet_; }

  // Counted since this receiver was made, saturating rather than wrapping: a count that has
  // stopped being exact must not start being wrong.
  std::uint32_t delivered() const { return delivered_; }
  std::uint32_t dropped() const { return dropped_; }
  std::uint32_t emptyFrames() const { return empty_frames_; }
  Drop lastDrop() const { return last_drop_; }
  std::uint32_t droppedFor(Drop reason) const;

  // Forget a half-collected frame, keeping the counters: what a client does when it has decided
  // the radio restarted, or that the UART carries text and not frames (§9).
  void discardPartial();

 private:
  void count(Drop reason);

  std::uint8_t collected_[kReceiveBuffer] = {};
  std::size_t have_ = 0;
  bool overflowed_ = false;

  std::uint8_t decoded_[kMaxPacket] = {};
  Packet packet_;

  std::uint32_t delivered_ = 0;
  std::uint32_t dropped_ = 0;
  std::uint32_t empty_frames_ = 0;
  std::uint32_t per_reason_[5] = {};  // one per Drop, kNone included and always 0
  Drop last_drop_ = Drop::kNone;
};

// The sending end: the frame's bytes, with the one 0x00 of §2 in front of it when the line has
// been idle for 100 ms or more.
//
// The idle time is measured from the last frame this writer built, because this is where the last
// frame was built: §2 says a frame goes out in one piece and nothing is interleaved inside it, so
// whoever owns the UART (task 02) sends what write() returns before it asks for another.
class FrameWriter {
 public:
  explicit FrameWriter(interfaces::Clock& clock) : clock_(clock) {}

  // One frame into `out`, which needs kMaxWrite bytes for the longest of them. Returns the bytes
  // written, or 0 when the payload is over 240 bytes or `out` is too small.
  std::size_t write(std::uint8_t type, std::uint8_t seq, Bytes payload, std::uint8_t* out,
                    std::size_t out_size);

  // Whether the last write() put the idle 0x00 in front. For a test and for a console line.
  bool prefixed() const { return prefixed_; }

  // The line is not ours any more (the radio was reset, the UART was released): the next frame
  // gets the 0x00 whatever the clock says.
  void forgetIdle() { written_ = false; }

 private:
  interfaces::Clock& clock_;
  std::uint64_t last_ms_ = 0;
  bool written_ = false;   // nothing has gone out yet, so the line counts as idle
  bool prefixed_ = false;
};

}  // namespace link
}  // namespace core
