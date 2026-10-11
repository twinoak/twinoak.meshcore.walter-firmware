// The console (task p0-03): bytes from the module's USB port in, lines parsed (parser.h), the
// commands run against RadioPins, Health and - from task p1-02 - the radio over the box link, text
// out. Portable: the platform gives it the bytes and a TextOut; host_test/ gives it fake pins, a
// scripted radio and a string, and runs a few thousand random command sequences through it to show
// that no sequence can break a pin rule.
//
// Behaviour as the agent firmware's console: a `>` prompt, typed characters echoed (so leave
// local echo off), backspace honoured, CR, LF and CR LF all end a line, a refusal is one line
// beginning `error:` that names what was wrong. docs/console.md describes it.
#pragma once

#include <cstddef>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/parser.h"
#include "core/flasher/flasher.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "interfaces/clock.h"
#include "interfaces/restart.h"
#include "interfaces/text_out.h"

namespace core {
namespace console {

class Console {
 public:
  Console(interfaces::TextOut& out, board::RadioPins& pins, health::Health& health,
          interfaces::Clock& clock, interfaces::Restart& restart, link::client::Client& radio,
          board::SlotLock& slots);

  // How this boot happened, and the first prompt (the banner is printed by main before this).
  void begin();

  // One byte from the port.
  void feed(char c);

  // One whole line, as if typed and ended. What feed() calls; tests call it directly.
  void runLine(const char* line);

 private:
  // The slot a `rak ...` line is about (task p1-06), resolved once in runLine: an optional `@N`
  // after the word `rak`, `@0` without one. slotOk() is where a slot this adapter does not have
  // becomes one line of refusal; the selection itself is taken by whoever faces the radio - the
  // link client around each exchange, a SlotHold around a pin or an SWD command, the flasher
  // inside its own call.
  bool slotOk(board::Slot slot);

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

  // The link (task p1-02). Each prints the decoded answer in words, or why there is none.
  void linkError(const char* what, link::client::Fault fault, std::uint8_t status);
  void rakHello();
  void rakPing();
  void rakStatus();
  void rakId();
  void rakTime();
  void rakTimeSet(std::uint32_t epoch);
  void rakPower();
  void rakEnv();
  void rakCli(const char* line, std::size_t size);
  void rakEvents();
  void rakText();
  void printEvent(const link::client::QueuedEvent& event, std::size_t number);
  void printBytes(const char* label, link::Bytes bytes);
  void printText(link::Text text);

  // The debug port (task p1-03).
  void rakSwdConnect();

  // The nRF52 behind the debug port (task p1-04): a session opened and closed inside the command,
  // the pins released on every exit path.
  void rakSwdId();
  void rakSwdMem(std::uint32_t address, std::uint32_t words);

  // The flasher (task p1-05): the bench's proof of erase, write and verify on real silicon.
  void rakFlashTest();
  void printFlashResult(const char* what, const flasher::Result& r);

  // The trust table (task p2-01): which release keys this build accepts, or that it accepts none.
  void trust();

  interfaces::TextOut& out_;
  board::RadioPins& pins_;
  health::Health& health_;
  interfaces::Clock& clock_;
  interfaces::Restart& restart_;
  link::client::Client& radio_;
  board::SlotLock& slots_;
  board::Slot slot_{};  // the slot the line being run is about
  // A member, not a local: it carries three page buffers (12 KB), more than the main task's stack
  // (CONFIG_ESP_MAIN_TASK_STACK_SIZE 3584) would hold; the console itself has static storage.
  flasher::Flasher flasher_;
  char line_[kMaxLineLength + 8] = {};
  std::size_t length_ = 0;
  bool last_was_cr_ = false;
};

}  // namespace console
}  // namespace core
