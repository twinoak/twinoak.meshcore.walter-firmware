// The command table (task p3-03): every command this console has, in the order `help` prints
// them, with one line of help each.
//
// Before this task the help text was fifty `print()` calls in console.cpp and the list of
// commands existed three times - there, in the parser, and in `docs/console.md` - so a command
// could be added to two of them. Now there is one table: `help` walks it, a host test holds
// `docs/console.md` to it (every row's words appear in the document), and another holds the
// parser to it (every row's words parse into something). A new command that is not in the table
// fails a test; a row with no document fails another.
//
// What a command *does* is still console.cpp's, and how a line becomes one is still parser.cpp's
// - this is the index, not a dispatch table: the dispatch is one `switch` over Verb with no
// `default`, so a new verb that nobody runs is a failed build (-Wswitch, an error in our own
// components).
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace console {

// How `help` groups them. The order here is the order they are printed in.
enum class Group : std::uint8_t {
  kUnit = 0,    // the Walter itself: what it is, what it knows, what it is told
  kRadio,       // the radio on the other side of the seven wires
  kImages,      // what is in the store, what this firmware trusts, this firmware's own slots
  kUplink,      // the modem and what goes over it
  kDutyCycle,   // sleep, the battery's band and what woke us (task p3-04)
  kJournal,     // what was written down in flash
  kCount,
};

const char* describe(Group group);

struct CommandDef {
  // What `help` prints and what `docs/console.md` must name: the words and their arguments.
  const char* usage;
  // The words alone, no placeholder: `rak kill`. What the tests parse and look for.
  const char* word;
  Group group;
  const char* help;  // one line; the console wraps it
};

// 61, the length below: the table's own static_assert holds it.
constexpr std::size_t kCommandCount = 61;
extern const CommandDef kCommands[kCommandCount];

// The word a command that could cost a radio asks for before it does anything: `erase confirm`,
// `rak flash-test confirm`, `rak kill 20 confirm`. One word, the same one everywhere, so that a
// person who has typed it once knows it everywhere. "A command that could brick a radio is not
// made impossible on the bench, but it is never one typo away."
constexpr const char* kConfirmWord = "confirm";

// How long the radio's rail may be taken away without the word. A reset, a short power cycle and
// the supervisor's own rungs are all inside this; anything longer is a person deciding to take a
// repeater off the air for a while and is asked to say so.
constexpr std::uint32_t kKillSecondsWithoutWord = 5;

}  // namespace console
}  // namespace core
