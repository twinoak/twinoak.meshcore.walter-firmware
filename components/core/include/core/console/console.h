// The console (task p0-03): bytes from the module's USB port in, lines parsed (parser.h), the
// commands run against RadioPins and Health, text out. Portable: the platform gives it the bytes
// and a TextOut; host_test/ gives it fake pins and a string, and runs a few thousand random
// command sequences through it to show that no sequence can break a pin rule.
//
// Behaviour as the agent firmware's console: a `>` prompt, typed characters echoed (so leave
// local echo off), backspace honoured, CR, LF and CR LF all end a line, a refusal is one line
// beginning `error:` that names what was wrong. docs/console.md describes it.
#pragma once

#include <cstddef>

#include "core/board/radio_pins.h"
#include "core/console/parser.h"
#include "core/health/health.h"
#include "interfaces/clock.h"
#include "interfaces/restart.h"
#include "interfaces/text_out.h"

namespace core {
namespace console {

class Console {
 public:
  Console(interfaces::TextOut& out, board::RadioPins& pins, health::Health& health,
          interfaces::Clock& clock, interfaces::Restart& restart);

  // How this boot happened, and the first prompt (the banner is printed by main before this).
  void begin();

  // One byte from the port.
  void feed(char c);

  // One whole line, as if typed and ended. What feed() calls; tests call it directly.
  void runLine(const char* line);

 private:
  void prompt();
  void print(const char* text);
  void printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
  void help();
  void status();
  void reboots();
  void pins();
  void rakReset();
  void rakKill(std::uint32_t seconds);
  void reboot();

  interfaces::TextOut& out_;
  board::RadioPins& pins_;
  health::Health& health_;
  interfaces::Clock& clock_;
  interfaces::Restart& restart_;
  char line_[kMaxLineLength + 8] = {};
  std::size_t length_ = 0;
  bool last_was_cr_ = false;
};

}  // namespace console
}  // namespace core
