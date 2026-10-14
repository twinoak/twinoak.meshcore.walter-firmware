// What `dump` prints (task p3-03): the last few kilobytes the port printed, oldest first, whole
// lines only. A board's answer to a journal, for a unit found quiet on a mast with a laptop and
// no history.
//
// It is **the same bytes**, not a second rendering of them: every line the console prints goes
// through Console::print and Console::printf, and those are the only two ways anything of the
// console reaches interfaces::TextOut, so the ring is filled by construction rather than by
// somebody remembering to log. A binary transfer's frames (`image receive`) do not go through
// them and are not in it.
//
// **What is never kept** (spec/SYSTEM.md S-12: "Nothing the mesh says is logged as text"): the
// lines the radio sent as text - a scanner's NDJSON, a CLI reply, an advert's name. The console
// prints them inside a `LogRing::Quiet`, and while one is alive the ring takes nothing. A secret
// never reaches a printed line at all (core/settings/keys.h), so there is nothing to keep out
// of the ring; the host test greps everything this ring ever held for both of them.
#pragma once

#include <cstddef>

#include "interfaces/text_out.h"

namespace core {
namespace console {

class LogRing {
 public:
  // A few kilobytes, the agent firmware's figure for the same command (its `dump` is 4 kB).
  // Static storage inside the console, never the stack.
  static constexpr std::size_t kBytes = 4096;

  // Keeps `length` bytes. The oldest are overwritten; nothing is kept while a Quiet is alive.
  void append(const char* text, std::size_t length);

  // Everything kept, oldest first, beginning at a line boundary: when the ring has wrapped, the
  // part of the oldest line that survived is dropped rather than printed as a line it never was.
  void writeTo(interfaces::TextOut& out) const;

  std::size_t kept() const { return wrapped_ ? kBytes : next_; }
  bool wrapped() const { return wrapped_; }
  // Bytes the ring has had to throw away, so `dump` can say the log is older than it looks.
  std::size_t dropped() const { return dropped_; }
  void clear();

  // Nothing is kept while one of these is alive: used around every line whose text came from the
  // radio. Nested quiets are counted, so a quiet inside a quiet is still quiet.
  class Quiet {
   public:
    explicit Quiet(LogRing& ring) : ring_(ring) { ring_.quiet_++; }
    ~Quiet() { ring_.quiet_--; }
    Quiet(const Quiet&) = delete;
    Quiet& operator=(const Quiet&) = delete;

   private:
    LogRing& ring_;
  };

 private:
  friend class Quiet;

  char bytes_[kBytes] = {};
  std::size_t next_ = 0;  // where the next byte goes
  bool wrapped_ = false;
  std::size_t dropped_ = 0;
  int quiet_ = 0;
};

}  // namespace console
}  // namespace core
