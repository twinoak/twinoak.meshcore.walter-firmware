// The console's output into a string, and a restart that only counts.
#pragma once

#include <string>

#include "interfaces/restart.h"
#include "interfaces/text_out.h"

namespace fakes {

class CaptureOut final : public interfaces::TextOut {
 public:
  std::string text;
  void write(const char* bytes, std::size_t length) override { text.append(bytes, length); }
  void clear() { text.clear(); }
};

class FakeRestart final : public interfaces::Restart {
 public:
  int asked = 0;
  void restart() override { asked++; }
};

}  // namespace fakes
