// The modem library's trace behind its seam (task p10-06), on the host: it records what the
// console asked of it and what the console gave it, and prints nothing.
//
// The real one (components/modem/modem_trace.cpp) is `esp_log_set_vprintf` and
// `esp_log_level_set` and cannot be built here; what *can* be tested here is everything the
// console does around it - that the two secrets reach it from the settings store, that the
// console prints neither of them, that the state is nowhere kept, and that a refusal comes back
// as one `error:` line. The redaction itself is core's and is tested on its own
// (host_test/core/trace/test_redact.cpp) against a real request head.
#pragma once

#include <cstdint>
#include <string>

#include "core/console/modem_trace.h"

namespace fakes {

class FakeModemTrace final : public core::console::ModemTrace {
 public:
  // What a board can refuse with: the line buffer the hook needs.
  bool refuse = false;
  const char* refusal = "the trace needs 4096 bytes for one line and the heap would not give them";

  // What it was told, for the test to read.
  std::uint32_t turns = 0;
  std::string last_token;
  std::string last_pin;

  bool on() const override { return on_; }

  bool turn(bool on, const char* token, const char* pin, char* why, std::size_t cap) override {
    ++turns;
    last_token = token != nullptr ? token : "";
    last_pin = pin != nullptr ? pin : "";
    if (refuse) {
      if (why != nullptr && cap > 0) {
        std::size_t i = 0;
        for (; refusal[i] != 0 && i + 1 < cap; ++i) why[i] = refusal[i];
        why[i] = 0;
      }
      return false;
    }
    on_ = on;
    // What the real one counts: a line of the library's for every TX and every RX. Two a turn is
    // enough for the console's lines to have a number in them.
    if (on_) {
      lines_ += 2;
      redacted_ += 1;
    }
    return true;
  }

  std::uint32_t lines() const override { return lines_; }
  std::uint32_t redacted() const override { return redacted_; }

 private:
  bool on_ = false;
  std::uint32_t lines_ = 0;
  std::uint32_t redacted_ = 0;
};

}  // namespace fakes
