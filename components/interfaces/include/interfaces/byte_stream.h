// A byte stream, as core/ sees the UART to the radio (task p1-02): read what is there, write,
// flush, and whether the line has been idle. Nothing of a baud rate, a driver or a pin is in a
// signature here - the platform's implementation (platform/uart.h) is a UART1 at 115200 8N1 on
// GPIO7/8, and host_test/sim/scripted_radio.h is a radio on the other end of the same interface.
//
// Two of the six operations are not "a stream" and are here for one reason each:
//
//   - **open() and close()** exist because this stream's pins belong to something else. The UART
//     may own them only between RadioPins::uartEnable() and uartRelease()
//     (docs/ARCHITECTURE.md §2 rule 1), so somebody has to tell the peripheral when to take the
//     pads and when to let go; core/link/client/port.h is the one place that sequences the two,
//     and it is the only caller of either.
//   - **idleFor()** is spec/LINK-PROTOCOL.md §2's "the line has been idle for 100 ms or more",
//     measured where it is true - at the port - rather than guessed one layer up.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

class ByteStream {
 public:
  virtual ~ByteStream() = default;

  // Takes the pins and makes the peripheral ready. False when it could not be; the caller is then
  // not open and nothing of it drives a pad. Called only after the pins have been enabled.
  virtual bool open() = 0;

  // Lets the pins go: whatever was written has left first (bounded), and the pads come back to
  // whoever owns them. Harmless when not open.
  virtual void close() = 0;

  // The bytes that have arrived, up to `max`, and how many that was. Never blocks: 0 means
  // nothing is there, not an error. The caller polls (the link client, in slices of a tick).
  virtual std::size_t read(std::uint8_t* out, std::size_t max) = 0;

  // Hands over bytes to send and returns how many it took. Fewer than asked is a full buffer or a
  // closed stream, and the caller treats it as a failed send: §2 says a frame goes out in one
  // piece, so half of one is nothing.
  virtual std::size_t write(const std::uint8_t* data, std::size_t size) = 0;

  // Blocks until what was written has left the port, at most `timeout_ms`. False on timeout.
  // Bounded because nothing in this firmware waits on a UART without a bound; the longest frame
  // of this link is 246 bytes, which is 21 ms at 115200 baud.
  virtual bool flush(std::uint32_t timeout_ms) = 0;

  // Whether nothing has arrived and nothing has been written for at least this long - §2's idle
  // line. A stream just opened answers true: nothing has happened on it yet.
  virtual bool idleFor(std::uint32_t ms) = 0;
};

}  // namespace interfaces
