// Where a console report goes (task p1.5-15): the three calls the runner's and the loop's
// `jobs`, `sessions`, `server` and `errors` reports make of an Arduino Stream, without the
// Stream. The board hands in its serial port behind this (src/console/StreamOut.h); a host test
// hands in a string.
//
// The bytes are Print's: println() ends a line with "\r\n", and printf() formats exactly as
// Print::printf does, into a line buffer that is longer than any line these reports write.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace mm {

class TextOut {
 public:
  virtual ~TextOut() = default;
  virtual void write(const char* text, size_t len) = 0;

  void print(const char* text) { write(text, std::strlen(text)); }
  void println(const char* text = "") {
    print(text);
    write("\r\n", 2);
  }
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    // The longest line any report writes is the `errors` summary of `server`, at some 200
    // characters; a longer one is cut at the buffer, not written past it.
    char line[384];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (n <= 0) return;
    write(line, static_cast<size_t>(n) < sizeof line ? static_cast<size_t>(n) : sizeof line - 1);
  }
};

}  // namespace mm
