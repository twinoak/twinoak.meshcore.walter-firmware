// The link's port: the UART to the radio, taken and given back as one thing (task p1-02).
//
// Two objects have to move together. RadioPins owns the pads and the rules of
// docs/ARCHITECTURE.md §2 - TX is driven only between uartEnable() and uartRelease(), and a kill
// takes the lines from whoever holds them. interfaces::ByteStream owns the peripheral, which
// cannot be routed to a pad the board logic has not enabled. This class is the only place the two
// are sequenced, and that is what makes the task's fourth criterion a property of the code's shape
// rather than of everybody's care:
//
//   - open()  : RadioPins::uartEnable() first, then the stream. If the stream refuses, the pins go
//               back at once, so the UART is never enabled with nothing behind it.
//   - close() : the stream first (what was written leaves, the peripheral lets the pads go), then
//               RadioPins::uartRelease().
//   - send(), read(), idleFor() : nothing at all unless both halves are live. Every call asks
//               RadioPins whether the UART is still enabled, because a `rak kill` takes it away
//               without telling anyone (ARCHITECTURE §2 rule 3) - and then this object closes the
//               stream itself and counts the loss.
//
// So the only caller of uartEnable()/uartRelease() in the firmware is open()/close() here, and the
// only caller of the stream is this class. No logic above it can drive the pin, and no code below
// it knows the rules.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/radio_pins.h"
#include "interfaces/byte_stream.h"

namespace core {
namespace link {
namespace client {

class LinkPort {
 public:
  LinkPort(board::RadioPins& pins, interfaces::ByteStream& stream) : pins_(pins), stream_(stream) {}

  // The pins, then the stream. kOk when the UART is ours; RadioPins' own outcome when it refused
  // (the rail is off, a pin was refused); kPinRefused when the stream itself would not open, the
  // pins having been given back first.
  board::Outcome open();

  // The stream, then the pins. Idempotent.
  void close();

  // Whether both halves are live. Not const: it asks the board, and closes the stream when the
  // pins have gone (a kill, or anything else that called safe()).
  bool isOpen();

  // One frame out, whole, and off the port before returning: §2 says a frame goes out in one piece
  // with nothing interleaved, and the response timeout starts when the last bit has left. False
  // when the port is not ours or the stream took fewer bytes than asked - half a frame is nothing.
  bool send(const std::uint8_t* data, std::size_t size);

  // Whatever has arrived, up to `max`. 0 when nothing has, or when the port is not ours.
  std::size_t read(std::uint8_t* out, std::size_t max);

  // §2's idle line, as the port measures it. False when the port is not ours.
  bool idleFor(std::uint32_t ms);

  std::uint32_t opens() const { return opens_; }
  std::uint32_t lost() const { return lost_; }       // times the pins were taken from under it
  std::uint32_t sent() const { return sent_; }       // frames handed to the stream
  std::uint32_t failedSends() const { return failed_sends_; }

  // How long the stream is given to put a frame on the wire: 246 bytes at 115200 baud is 21 ms,
  // and a port that cannot do that in ten times as long is not working.
  static constexpr std::uint32_t kFlushMs = 200;

 private:
  board::RadioPins& pins_;
  interfaces::ByteStream& stream_;
  bool open_ = false;
  std::uint32_t opens_ = 0;
  std::uint32_t lost_ = 0;
  std::uint32_t sent_ = 0;
  std::uint32_t failed_sends_ = 0;
};

}  // namespace client
}  // namespace link
}  // namespace core
