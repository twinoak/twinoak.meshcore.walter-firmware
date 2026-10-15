// The last four kilobytes of the log, so that a board on a windowsill can be asked what
// happened before you got there.
//
// The Pi agent has a journal and somebody who can ssh in; a board has neither (EXIT-NODES §5,
// "What it cannot do"), and its serial console only shows what was printed while a terminal was
// attached. So every line `logf` prints is also written here, into a fixed circular buffer, and
// `dump` on the console prints what it holds - oldest first, whole lines only.
//
// There is nothing to redact on the way in and that is the point: a line that reaches the log
// has already been through the rails of README.md - no secret is ever an argument, nothing the
// mesh says is logged as text, and a password is redacted where it is formatted, not here. The
// ring is the same bytes the port carried, which is what makes `dump` worth reading: if a
// secret could reach it, it would already have reached the terminal.
//
// Host-portable: no Arduino. test/test_buffers holds the wrap, the bound and the whole-line
// rule without a board.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

class LogRing {
 public:
  // Four kilobytes is what the task asks `dump` to print, and about fifty of this firmware's
  // log lines - a boot, a join, a clock, a config call and a job or two.
  static constexpr size_t kCapacity = 4096;

  // Append one line. A NUL is not written and the text is not read after this call; a line
  // longer than the ring is cut to its last kCapacity bytes.
  void add(const char* text, size_t len);
  void add(const char* text);

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool wrapped() const { return wrapped_; }
  uint32_t lines() const { return lines_; }
  uint32_t dropped() const { return dropped_; }  // lines the ring overwrote

  // Hand what is held to `sink`, oldest first, in at most two runs (the ring's two halves), so
  // that printing four kilobytes needs no four-kilobyte buffer to print them from. When the ring
  // has wrapped the first partial line is left out rather than printed half - the first byte
  // handed over is the one after the first newline - so `dump` never starts mid-word.
  void forEachChunk(void* ctx, void (*sink)(void* ctx, const char* text, size_t len)) const;

  // The same, into a buffer of the caller's: `cap` bytes including the NUL. Returns the length
  // written, without it.
  size_t copyTo(char* out, size_t cap) const;

  void clear();

 private:
  char buf_[kCapacity] = {0};
  size_t next_ = 0;    // where the next byte goes
  size_t size_ = 0;    // how much of buf_ is real
  bool wrapped_ = false;
  uint32_t lines_ = 0;
  uint32_t dropped_ = 0;
};

// The one ring this firmware writes to, filled by Log.cpp and printed by `dump`.
LogRing& logRing();

}  // namespace mm
