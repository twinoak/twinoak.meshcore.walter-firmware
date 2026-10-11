// Where the console's text goes: bytes out, nothing back. The platform's is the module's USB
// serial port; the host tests' captures into a string. Lines end in '\n' here; the platform turns
// that into what the terminal wants.
#pragma once

#include <cstddef>

namespace interfaces {

class TextOut {
 public:
  virtual ~TextOut() = default;
  virtual void write(const char* text, std::size_t length) = 0;
};

}  // namespace interfaces
