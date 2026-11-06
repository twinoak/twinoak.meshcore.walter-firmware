// The console's output into a string, and a restart that only counts.
#pragma once

#include <cstdint>
#include <string>

#include "core/console/crlf.h"
#include "interfaces/restart.h"
#include "interfaces/text_out.h"

namespace fakes {

// Everything verbatim: what a test reads when it is the console's words it cares about.
class CaptureOut final : public interfaces::TextOut {
 public:
  std::string text;
  void write(const char* bytes, std::size_t length) override { text.append(bytes, length); }
  bool writeBytes(const std::uint8_t* bytes, std::size_t count) override {
    text.append(reinterpret_cast<const char*>(bytes), count);
    return true;
  }
  void clear() { text.clear(); }
};

// The board's console: text through the LF -> CR LF a terminal wants (core/console/crlf.h),
// bytes verbatim (task p8-02). A test that reads frames out of `text` has to use this one - a
// capture that wrote text verbatim is what hid the 6 November fault, where the transfer's answers
// went through the text path and arrived corrupted on a board.
//
// `cut_at` makes the port short: once that many bytes have been written, writeBytes takes no more
// and returns false, which is a frame cut in half.
class TerminalOut final : public interfaces::TextOut {
 public:
  std::string text;
  std::size_t cut_at = 0;  // 0: never short

  void write(const char* bytes, std::size_t length) override {
    char buffer[core::console::kCrlfBuffer];
    std::size_t at = 0;
    while (at < length) {
      const core::console::CrlfRun run =
          core::console::toCrlf(bytes + at, length - at, buffer, sizeof buffer);
      if (run.put == 0) return;
      text.append(buffer, run.put);
      at += run.taken;
    }
  }

  bool writeBytes(const std::uint8_t* bytes, std::size_t count) override {
    std::size_t room = count;
    if (cut_at != 0) room = text.size() >= cut_at ? 0 : cut_at - text.size();
    if (room > count) room = count;
    text.append(reinterpret_cast<const char*>(bytes), room);
    return room == count;
  }

  void clear() { text.clear(); }
};

class FakeRestart final : public interfaces::Restart {
 public:
  int asked = 0;
  void restart() override { asked++; }
};

}  // namespace fakes
