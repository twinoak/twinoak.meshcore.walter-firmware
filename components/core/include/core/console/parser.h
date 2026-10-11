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
//
// and the link to the radio (task p1-02, spec/LINK-PROTOCOL.md):
//
//   rak hello             HELLO: the version, role, slot and capabilities the radio answers with
//   rak ping              PING, with the round trip
//   rak status            STATUS: uptime, why it booted, its clock, its flags and counters
//   rak id                IDENTITY: the node's public key and advert name
//   rak time              TIME_GET
//   rak time set <epoch>  TIME_SET from a person at this console (§5.4's source 3)
//   rak power             POWER_GET: the three channels
//   rak env               ENV_GET: temperature, humidity, pressure
//   rak cli <line>        CLI: one line run by the radio's own console interpreter
//   rak events            the events the client has queued
//   rak text              the lines a scanner has sent (§9)
//
// and the debug port (task p1-03) and the chip behind it (task p1-04):
//
//   rak swd connect       acquire the SWD lines, connect, print DPIDR and the power-up, release
//   rak swd id            connect, identify the nRF52840 (FICR), its protection, its core's state, release
//   rak swd mem <addr> [words]
//                         read 1 to 32 words at a word-aligned address (hex 0x... or decimal), read-only
//
// and the flasher (task p1-05):
//
//   rak flash-test        a two-page pattern at the top of the application region, only if those
//                         pages are blank: written, verified, erased again; the radio restarted
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/wire.h"

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
  // The link (task p1-02).
  kRakHello,
  kRakPing,
  kRakStatus,
  kRakId,
  kRakTime,
  kRakTimeSet,
  kRakPower,
  kRakEnv,
  kRakCli,
  kRakEvents,
  kRakText,
  // The debug port (task p1-03) and the nRF52 behind it (task p1-04).
  kRakSwdConnect,
  kRakSwdId,
  kRakSwdMem,
  // The flasher (task p1-05).
  kRakFlashTest,
};

// rak swd mem reads at most this many words: eight lines of four.
constexpr std::uint32_t kMaxMemWords = 32;

// §5.7: a CLI line is 1 to 160 bytes of UTF-8. The parser refuses a longer one here rather than
// letting the encoder refuse it later, so that the console can say which limit was passed.
constexpr std::size_t kMaxCliLine = link::kMaxCliLine;

struct Command {
  Verb verb = Verb::kEmpty;
  std::uint32_t seconds = 0;              // kRakKill
  std::uint32_t epoch = 0;                // kRakTimeSet
  char text[kMaxCliLine + 1] = {};        // kRakCli: the rest of the line, verbatim
  std::size_t text_size = 0;
  std::uint32_t address = 0;              // kRakSwdMem: word-aligned
  std::uint32_t words = 1;                // kRakSwdMem: 1..kMaxMemWords
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
