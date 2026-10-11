// The console's line parser: one line in, one command or one refusal out (task p0-03). A pure
// function, after the agent firmware's src/console/Parser.h, so that every line shape and every
// refusal has a host test. What a command does is console.h.
//
// The grammar (one line per command, a `>` prompt; docs/console.md):
//
//   help                  the commands
//   status                version, uptime, why it booted, the pins and the radio's power
//   reboots               why this board last restarted, and the four before that
//   pins                  the seven pins: net, D-sub, GPIO, mode, level
//   rak reset             one reset pulse on nRESET (waits out the spacing)
//   rak kill <seconds>    the LoRa rail off for that long, then back
//   reboot                restart the Walter
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace console {

enum class Verb : std::uint8_t {
  kEmpty,  // a blank line: nothing to do, no error
  kHelp,
  kStatus,
  kReboots,
  kPins,
  kRakReset,
  kRakKill,
  kReboot,
};

struct Command {
  Verb verb = Verb::kEmpty;
  std::uint32_t seconds = 0;  // kRakKill
};

// The longest line the console accepts, without its line ending (the agent firmware's figure).
constexpr std::size_t kMaxLineLength = 200;

struct Parsed {
  bool ok = false;
  Command command;
  char error[96] = {};  // one line, when !ok, without the "error: " prefix
};

Parsed parseLine(const char* line);

}  // namespace console
}  // namespace core
