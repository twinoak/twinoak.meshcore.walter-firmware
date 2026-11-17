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

  // **A restart that does not return** (task p15-06). On a board `esp_restart()` never comes
  // back, so everything after the line that asked for it does not happen; here `restart()`
  // returns and the caller carries on, which for the bound on a wake means the pass goes on to
  // step 10 and *sleeps* - the opposite of what the bound did. A test that wants a wake really
  // ended turns this on and catches `Restarted` out of the pass, which leaves the objects exactly
  // as a reset leaves them. **Opt-in**: every test written before this one relies on the return.
  struct Restarted {};
  bool does_not_return = false;

  void restart() override {
    asked++;
    if (does_not_return) throw Restarted{};
  }
};

}  // namespace fakes
