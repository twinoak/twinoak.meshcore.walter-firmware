// The terminal's line endings, in one place (task p8-02).
//
// A terminal wants CR LF and this firmware's text carries '\n' (interfaces/text_out.h), so the
// console's output converts. Until 0.1.1 that conversion lived in `platform::UsbConsole::write`
// alone, and the host tests could not see it: `host_test/console_host/`'s TextOut wrote bytes
// verbatim, so the image transfer's answers - frames, not text - went through unconverted there
// and corrupted on a board (tasks/phase-6/FOLLOWUPS.md, 6 November). The conversion is this
// function now, and both the platform's console and the console host call it, so a test sees the
// bytes a board sends.
//
// A frame never comes through here: a binary path writes bytes with `TextOut::writeBytes`, which
// converts nothing (core/image/receive.cpp).
//
// It converts into a buffer the caller gives and says how much of the text it took, so that each
// caller keeps its own bound on a write: the platform's text write waits a few ticks and drops
// what a dead port will not take, which is right for a line on a mast and wrong for a frame.
#pragma once

#include <cstddef>

namespace core {
namespace console {

// What one call did: `taken` bytes of the text became `put` bytes in the buffer.
struct CrlfRun {
  std::size_t taken;
  std::size_t put;
};

// Converts the front of `text` into `out`, turning every '\n' into CR LF and copying every other
// byte as it is. Takes as much of the text as fits: `put` is never more than `capacity`, and a
// '\n' is never split across two calls. `capacity` under 2 converts nothing ({0, 0}), so a caller
// that loops on `taken` must give at least two bytes.
CrlfRun toCrlf(const char* text, std::size_t length, char* out, std::size_t capacity);

// A comfortable buffer for one call: `Console::printf` writes at most 200 bytes a line, and a
// line of 128 bytes of which every byte is '\n' still goes out in two calls.
constexpr std::size_t kCrlfBuffer = 128;

}  // namespace console
}  // namespace core
