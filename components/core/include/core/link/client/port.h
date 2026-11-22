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
#include "core/link/codec/wire.h"
#include "interfaces/byte_stream.h"

namespace core {
namespace link {
namespace client {

// -- the ring the bytes arrive into, by arithmetic (task p18-03) --------------------------------
//
// The driver's receive ring is sized here, in core/, rather than at the driver, because what
// decides it is not the port: it is **how long the reader is away between two reads**, which is
// the session loop's business (core/update/scanner_session.h). platform/uart.cpp takes
// `kRxRingBytes` from here and nothing else names a number.
//
// Why the arithmetic and not a figure that looked small: on 21 November the unit at Bellevue lost
// nine of the sixteen lines of one `hist` step into a ring of 1 024 bytes. Seven lines - about
// 1.2 kB - is the ring and what the driver and the hardware hold behind it, and it was all the
// ring could hold while the reader was away on a call to the server.

// §1's table: UART1, 115 200 8N1, no flow control.
constexpr std::uint32_t kBaud = 115200;
// 8N1 is ten bits a byte - a start bit, eight data bits, a stop bit - so the line carries a tenth
// of the baud rate in bytes a second: 11 520.
constexpr std::uint32_t kBytesPerSecond = kBaud / 10;

// **The longest the reader is away while the port is listened to.** Two parts:
//
//   - the loop's own idle between two reads, which is `core::update::kListenIdleMs` - a session
//     listening to a step sleeps a tenth of a second and polls (a `static_assert` in
//     scanner_session.h holds the two equal, which is the one place either may change);
//   - what the loop does between two reads with the port unread. Since task p18-03 that is the
//     machine's own step and nothing else - no call to the server falls between a step's command
//     and its end - so fifty milliseconds is a tick of FreeRTOS and the slack of a task that is
//     not the only one on the chip.
constexpr std::uint32_t kReaderIdleMs = 100;
constexpr std::uint32_t kReaderWorkMs = 50;
constexpr std::uint32_t kReaderAwayMs = kReaderIdleMs + kReaderWorkMs;

// What the line carries while the reader is away: 1 728 bytes.
constexpr std::size_t kCarriedWhileAway =
    static_cast<std::size_t>(kBytesPerSecond) * kReaderAwayMs / 1000u;

// The ring, with better than twice that as the margin: 4 096 bytes is 355 ms of the line at this
// baud rate. It is the one buffer the link's frames (§2, at most 246 bytes) are read through too,
// so they get the room with it.
constexpr std::size_t kRxRingBytes = 4096;
static_assert(kRxRingBytes >= 2 * kCarriedWhileAway,
              "the receive ring must hold twice what the line carries while the reader is away");
static_assert(kRxRingBytes >= 8 * kMaxFrame,
              "and at least eight frames of this link, which is what task p1-02 asked of it");

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
  // Times the stream lost what had arrived: its ring was full, or the hardware's FIFO ran over
  // (interfaces::ByteStream::losses). Cumulative, and read through the port because the port is
  // the only caller of the stream.
  std::uint32_t losses() const { return stream_.losses(); }

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
