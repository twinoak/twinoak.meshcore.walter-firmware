// Where the console's output goes: bytes out, nothing back. The platform's is the module's USB
// serial port; the host tests' captures into a string.
//
// Two ways out, because the port carries two different things (task p8-02):
//
//   write       text. Lines end in '\n' here and the implementation turns that into what the
//               terminal wants (core/console/crlf.h). It may drop what a port nobody is reading
//               will not take within a few ticks: a Walter on a mast prints into nothing and
//               must not stall for it.
//   writeBytes  bytes, verbatim and whole. A frame of the image transfer (core/image/receive.h)
//               is not text: one 0x0A turned into CR LF is a corrupted frame, which is what
//               stopped `walterctl.py push` on the first unit (tasks/phase-6/FOLLOWUPS.md,
//               6 November). The implementation converts nothing and keeps writing until every
//               byte is out or a bound of its own has passed; it returns false rather than
//               writing part of a frame in silence.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

class TextOut {
 public:
  virtual ~TextOut() = default;
  virtual void write(const char* text, std::size_t length) = 0;
  // True when all `count` bytes went out. Pure, not a default that forwards to write(): an
  // implementation that converts line endings must say what its raw path is, and a new one that
  // forgot would be the 6 November fault again.
  virtual bool writeBytes(const std::uint8_t* bytes, std::size_t count) = 0;
};

}  // namespace interfaces
