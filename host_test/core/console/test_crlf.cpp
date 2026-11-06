// The terminal's LF -> CR LF (task p8-02): core/console/crlf.h, the one place that conversion
// lives now. `platform::UsbConsole::write` and host_test/console_host/'s TextOut both call it, so
// what this file holds is what a board puts on the wire for every line it prints.
#include <doctest.h>

#include <cstddef>
#include <string>

#include "core/console/crlf.h"

using core::console::CrlfRun;
using core::console::toCrlf;

namespace {

// The whole of `text` through the conversion, in as many calls as a buffer of `capacity` needs -
// which is what both callers do.
std::string converted(const std::string& text, std::size_t capacity = 128) {
  std::string out;
  char buffer[256];
  REQUIRE(capacity <= sizeof buffer);
  std::size_t at = 0;
  while (at < text.size()) {
    const CrlfRun run = toCrlf(text.data() + at, text.size() - at, buffer, capacity);
    if (run.put == 0) break;
    out.append(buffer, run.put);
    at += run.taken;
  }
  return out;
}

}  // namespace

TEST_CASE("every '\\n' becomes CR LF and nothing else is touched") {
  CHECK(converted("") == "");
  CHECK(converted("status") == "status");
  CHECK(converted("one line\n") == "one line\r\n");
  CHECK(converted("a\nb\nc") == "a\r\nb\r\nc");
  CHECK(converted("\n\n\n") == "\r\n\r\n\r\n");
  // A CR that is already there is a byte like any other: the conversion is not idempotent and
  // does not pretend to be, which is why a frame must never come through it (an existing CR LF
  // arrives as CR CR LF, and tools/walterctl.py's UndoCrlf restores exactly that).
  CHECK(converted("a\r\nb") == "a\r\r\nb");
  // A zero in the text is kept: the conversion is about 0x0A and nothing else.
  CHECK(converted(std::string("a\0b\n", 4)) == std::string("a\0b\r\n", 5));
}

TEST_CASE("the conversion fills the caller's buffer and never splits a CR LF") {
  // Two bytes at a time: a '\n' needs both, so a call that has already put a byte stops in front
  // of it rather than putting the CR in one call and the LF in the next.
  char buffer[2];
  const std::string text = "a\nb";
  CrlfRun run = toCrlf(text.data(), text.size(), buffer, sizeof buffer);
  CHECK(run.taken == 1);
  CHECK(run.put == 1);
  CHECK(buffer[0] == 'a');
  run = toCrlf(text.data() + 1, text.size() - 1, buffer, sizeof buffer);
  CHECK(run.taken == 1);
  CHECK(run.put == 2);
  CHECK(buffer[0] == '\r');
  CHECK(buffer[1] == '\n');
  // And the whole of it is the same whatever the buffer's size.
  CHECK(converted("a\nb\n", 2) == "a\r\nb\r\n");
  CHECK(converted("a\nb\n", 3) == "a\r\nb\r\n");
  std::string every_byte_a_newline;
  for (int i = 0; i < 200; ++i) every_byte_a_newline += "\r\n";
  CHECK(converted(std::string(200, '\n'), 5) == every_byte_a_newline);
}

TEST_CASE("a buffer under two bytes converts nothing, and so does a null") {
  char one[1] = {0};
  const CrlfRun run = toCrlf("a\n", 2, one, sizeof one);
  CHECK(run.taken == 0);
  CHECK(run.put == 0);
  const CrlfRun none = toCrlf(nullptr, 4, one, sizeof one);
  CHECK(none.taken == 0);
  CHECK(none.put == 0);
  const CrlfRun no_out = toCrlf("a", 1, nullptr, 8);
  CHECK(no_out.put == 0);
}
