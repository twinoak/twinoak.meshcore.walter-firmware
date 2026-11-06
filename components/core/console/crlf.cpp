#include "core/console/crlf.h"

namespace core {
namespace console {

CrlfRun toCrlf(const char* text, std::size_t length, char* out, std::size_t capacity) {
  CrlfRun run{0, 0};
  if (text == nullptr || out == nullptr || capacity < 2) return run;
  while (run.taken < length) {
    const char c = text[run.taken];
    // A '\n' needs two bytes and is never split: the caller's next call starts at it again.
    const std::size_t needs = c == '\n' ? 2u : 1u;
    if (run.put + needs > capacity) break;
    if (c == '\n') {
      out[run.put++] = '\r';
      out[run.put++] = '\n';
    } else {
      out[run.put++] = c;
    }
    ++run.taken;
  }
  return run;
}

}  // namespace console
}  // namespace core
