// The console's parser (task p0-03): every command, a blank line, an over-long line, a bad
// number, an unknown command, the refusals' wording.
#include <doctest.h>

#include <string>

#include "core/console/parser.h"

using core::console::kMaxLineLength;
using core::console::Parsed;
using core::console::parseLine;
using core::console::Verb;

namespace {

Verb verbOf(const char* line) {
  const Parsed p = parseLine(line);
  REQUIRE_MESSAGE(p.ok, line << ": " << p.error);
  return p.command.verb;
}

std::string errorOf(const char* line) {
  const Parsed p = parseLine(line);
  REQUIRE_MESSAGE(!p.ok, line << " was accepted");
  CHECK(p.command.verb == Verb::kEmpty);
  return p.error;
}

}  // namespace

TEST_CASE("every command parses, with and without a line ending, with surrounding spaces") {
  CHECK(verbOf("help") == Verb::kHelp);
  CHECK(verbOf("status") == Verb::kStatus);
  CHECK(verbOf("reboots") == Verb::kReboots);
  CHECK(verbOf("pins") == Verb::kPins);
  CHECK(verbOf("reboot") == Verb::kReboot);
  CHECK(verbOf("trust") == Verb::kTrust);
  CHECK(verbOf("  trust \n") == Verb::kTrust);
  CHECK_FALSE(core::console::parseLine("trust all").ok);
  CHECK(verbOf("rak reset") == Verb::kRakReset);
  CHECK(verbOf("rak kill 5") == Verb::kRakKill);
  // The radio's update (task p2-03).
  CHECK(verbOf("rak update") == Verb::kRakUpdate);
  CHECK(verbOf("rak update --first-install") == Verb::kRakUpdateFirstInstall);
  CHECK(verbOf("rak update status") == Verb::kRakUpdateStatus);
  CHECK(verbOf("rak @0 update status\r\n") == Verb::kRakUpdateStatus);
  CHECK(verbOf("rak role scanner") == Verb::kRakRoleScanner);
  CHECK(verbOf("rak role repeater") == Verb::kRakRoleRepeater);
  CHECK(parseLine("rak @1 update").command.slot == 1);
  CHECK(errorOf("rak update now") == "rak update: unknown word (rak update, rak update --first-install, rak update status): now");
  CHECK(errorOf("rak update status now") == "rak update takes one word: rak update [--first-install | status]");
  CHECK(errorOf("rak role") == "rak role needs a role: rak role scanner | repeater");
  CHECK(errorOf("rak role bridge") == "rak role: not one of scanner, repeater: bridge");
  CHECK(errorOf("rak role scanner now") == "rak role takes one word: rak role scanner | repeater");
  CHECK(verbOf("  status  \r\n") == Verb::kStatus);
  CHECK(verbOf("rak   kill\t7\n") == Verb::kRakKill);
  CHECK(verbOf("rak reset\r") == Verb::kRakReset);
}

TEST_CASE("rak kill carries its seconds") {
  CHECK(parseLine("rak kill 5").command.seconds == 5);
  CHECK(parseLine("rak kill 0").command.seconds == 0);  // the bounds are RadioPins', not the parser's
  CHECK(parseLine("rak kill 30").command.seconds == 30);
  CHECK(parseLine("rak kill 007").command.seconds == 7);
  CHECK(parseLine("rak kill 999999999").command.seconds == 999999999u);
}

TEST_CASE("a blank line is nothing to do and no error") {
  for (const char* line : {"", " ", "\t", "\r\n", "   \r\n", "\n"}) {
    const Parsed p = parseLine(line);
    CHECK_MESSAGE(p.ok, '"' << line << '"');
    CHECK(p.command.verb == Verb::kEmpty);
  }
  CHECK(parseLine(nullptr).ok);
}

TEST_CASE("an over-long line is refused by length, whatever it says") {
  const std::string ok(kMaxLineLength, 'x');
  CHECK(errorOf(ok.c_str()).find("unknown command") == 0);  // 200: parsed, then unknown
  const std::string long1(kMaxLineLength + 1, 'x');
  CHECK(errorOf(long1.c_str()) == "line too long (200 characters at most)");
  const std::string long3(kMaxLineLength + 3, 'x');
  CHECK(errorOf(long3.c_str()) == "line too long (200 characters at most)");
  const std::string padded = std::string(150, ' ') + "status" + std::string(60, ' ');  // 216 raw
  CHECK(errorOf(padded.c_str()) == "line too long (200 characters at most)");
  const std::string spaced = std::string(100, ' ') + "status" + std::string(90, ' ');  // 196 raw
  CHECK(verbOf(spaced.c_str()) == Verb::kStatus);
}

TEST_CASE("a bad number is refused with the text that was given") {
  CHECK(errorOf("rak kill x") == "rak kill: not a whole number of seconds: x");
  CHECK(errorOf("rak kill -1") == "rak kill: not a whole number of seconds: -1");
  CHECK(errorOf("rak kill 1.5") == "rak kill: not a whole number of seconds: 1.5");
  CHECK(errorOf("rak kill +5") == "rak kill: not a whole number of seconds: +5");
  CHECK(errorOf("rak kill 5s") == "rak kill: not a whole number of seconds: 5s");
  CHECK(errorOf("rak kill 1234567890") == "rak kill: not a whole number of seconds: 1234567890");
  CHECK(errorOf("rak kill") == "rak kill needs a duration: rak kill <seconds>");
  CHECK(errorOf("rak kill 5 6") == "rak kill takes one number: rak kill <seconds>");
}

TEST_CASE("unknown commands and subcommands are named in the refusal") {
  CHECK(errorOf("foo") == "unknown command (try 'help'): foo");
  CHECK(errorOf("Status") == "unknown command (try 'help'): Status");
  // The list of subcommands outgrew a 96-character refusal when the link arrived (task p1-02):
  // `help` lists them, and the refusal names what was wrong and points at it.
  CHECK(errorOf("rak") == "rak needs a subcommand (try 'help')");
  CHECK(errorOf("rak dance") == "unknown rak subcommand (try 'help'): dance");
  CHECK(errorOf("rak reset now") == "rak reset takes no argument");
  CHECK(errorOf("help me") == "help takes no argument");
  CHECK(errorOf("status please") == "status takes no argument");
  CHECK(errorOf("reboots 3") == "reboots takes no argument");
  CHECK(errorOf("pins 1") == "pins takes no argument");
  CHECK(errorOf("reboot now") == "reboot takes no argument");
}

TEST_CASE("control characters and non-ASCII are refused") {
  CHECK(errorOf("sta\x01tus") == "control characters are not accepted");
  CHECK(errorOf("status\x1b[A") == "control characters are not accepted");
  CHECK(errorOf("st\xc3\xa5tus") == "printable ASCII only");
}

TEST_CASE("a refusal fits one line") {
  const std::string junk(kMaxLineLength, 'z');
  const Parsed p = parseLine(junk.c_str());
  CHECK(!p.ok);
  CHECK(std::string(p.error).size() < sizeof p.error);
  CHECK(std::string(p.error).find('\n') == std::string::npos);
}

// ---------------------------------------------------------------------------- the link (p1-02)

TEST_CASE("the link's subcommands parse, and none of them takes an argument but two") {
  CHECK(verbOf("rak hello") == Verb::kRakHello);
  CHECK(verbOf("rak ping") == Verb::kRakPing);
  CHECK(verbOf("rak status") == Verb::kRakStatus);
  CHECK(verbOf("rak id") == Verb::kRakId);
  CHECK(verbOf("rak power") == Verb::kRakPower);
  CHECK(verbOf("rak env") == Verb::kRakEnv);
  CHECK(verbOf("rak events") == Verb::kRakEvents);
  CHECK(verbOf("rak text") == Verb::kRakText);
  CHECK(verbOf("  rak   hello  \r\n") == Verb::kRakHello);
  for (const char* line : {"rak hello now", "rak ping 1", "rak status x", "rak id 2",
                           "rak power on", "rak env 3", "rak events all", "rak text 4"}) {
    const Parsed p = parseLine(line);
    CHECK_MESSAGE(!p.ok, line);
    CHECK_MESSAGE(std::string(p.error).find("takes no argument") != std::string::npos, p.error);
  }
}

TEST_CASE("rak time and rak time set <epoch>") {
  CHECK(verbOf("rak time") == Verb::kRakTime);
  CHECK(verbOf("rak time set 1791720000") == Verb::kRakTimeSet);
  CHECK(parseLine("rak time set 1791720000").command.epoch == 1791720000u);
  // §5.4's `epoch` is a u32, so ten digits are allowed and the range is the type's.
  CHECK(parseLine("rak time set 4294967295").command.epoch == 4294967295u);
  CHECK(errorOf("rak time set 4294967296") == "rak time set: not a unix epoch in seconds: 4294967296");
  CHECK(errorOf("rak time set 99999999999") == "rak time set: not a unix epoch in seconds: 99999999999");
  CHECK(errorOf("rak time set -1") == "rak time set: not a unix epoch in seconds: -1");
  CHECK(errorOf("rak time set now") == "rak time set: not a unix epoch in seconds: now");
  CHECK(errorOf("rak time set") == "rak time set needs a unix epoch: rak time set <epoch>");
  CHECK(errorOf("rak time set 1 2") == "rak time set takes one number: rak time set <epoch>");
  CHECK(errorOf("rak time get") == "rak time: unknown word (rak time, rak time set <epoch>): get");
}

TEST_CASE("rak swd connect parses, and rak swd with anything else is refused by name") {
  CHECK(verbOf("rak swd connect") == Verb::kRakSwdConnect);
  CHECK(verbOf("  rak   swd   connect  ") == Verb::kRakSwdConnect);
  Parsed p = parseLine("rak swd");
  CHECK(!p.ok);
  CHECK(std::string(p.error) == "rak swd needs an operation: rak swd connect | id | mem <addr> [words]");
  p = parseLine("rak swd disconnect");
  CHECK(!p.ok);
  CHECK(std::string(p.error) == "rak swd: unknown operation (connect, id, mem): disconnect");
  p = parseLine("rak swd connect now");
  CHECK(!p.ok);
  CHECK(std::string(p.error) == "rak swd connect takes no argument");
}

TEST_CASE("rak swd id and rak swd mem <addr> [words] parse, and their refusals name the fault (task p1-04)") {
  CHECK(verbOf("rak swd id") == Verb::kRakSwdId);
  CHECK(errorOf("rak swd id now") == "rak swd id takes no argument");

  Parsed p = parseLine("rak swd mem 0x10000100 2");
  REQUIRE(p.ok);
  CHECK(p.command.verb == Verb::kRakSwdMem);
  CHECK(p.command.address == 0x10000100u);
  CHECK(p.command.words == 2);
  p = parseLine("rak swd mem 0x00026000");
  REQUIRE(p.ok);
  CHECK(p.command.address == 0x26000u);
  CHECK(p.command.words == 1);  // the default
  p = parseLine("rak swd mem 268435712 32");  // decimal 0x10000100, the most words allowed
  REQUIRE(p.ok);
  CHECK(p.command.address == 0x10000100u);
  CHECK(p.command.words == 32);
  p = parseLine("rak swd mem 0xE000EDF0 1");  // upper-case hex digits
  REQUIRE(p.ok);
  CHECK(p.command.address == 0xE000EDF0u);

  CHECK(errorOf("rak swd mem") == "rak swd mem needs an address: rak swd mem <addr> [words]");
  CHECK(errorOf("rak swd mem 0x10000102") == "rak swd mem: the address must be word-aligned (a multiple of 4): 0x10000102");
  CHECK(errorOf("rak swd mem 0x") == "rak swd mem: not an address (hex 0x... or decimal, 32 bits): 0x");
  CHECK(errorOf("rak swd mem 0x100000000") == "rak swd mem: not an address (hex 0x... or decimal, 32 bits): 0x100000000");
  CHECK(errorOf("rak swd mem 0xfg") == "rak swd mem: not an address (hex 0x... or decimal, 32 bits): 0xfg");
  CHECK(errorOf("rak swd mem 4294967296") == "rak swd mem: not an address (hex 0x... or decimal, 32 bits): 4294967296");
  CHECK(errorOf("rak swd mem ficr") == "rak swd mem: not an address (hex 0x... or decimal, 32 bits): ficr");
  CHECK(errorOf("rak swd mem 0x10000100 0") == "rak swd mem: words is 1 to 32: 0");
  CHECK(errorOf("rak swd mem 0x10000100 33") == "rak swd mem: words is 1 to 32: 33");
  CHECK(errorOf("rak swd mem 0x10000100 two") == "rak swd mem: words is 1 to 32: two");
  CHECK(errorOf("rak swd mem 0x10000100 2 3") == "rak swd mem takes an address and a word count: rak swd mem <addr> [words]");
}

TEST_CASE("rak cli takes the rest of the line verbatim, up to §5.7's 160 bytes") {
  const Parsed one = parseLine("rak cli get radio");
  REQUIRE(one.ok);
  CHECK(one.command.verb == Verb::kRakCli);
  CHECK(std::string(one.command.text) == "get radio");
  CHECK(one.command.text_size == 9);
  // Internal spacing is the radio's business, not the parser's.
  CHECK(std::string(parseLine("rak cli  set  name   TwinOak Risskov ").command.text) ==
        "set  name   TwinOak Risskov");
  CHECK(errorOf("rak cli") == "rak cli needs a line: rak cli <line>");
  const std::string longest = "rak cli " + std::string(core::console::kMaxCliLine, 'x');
  CHECK(parseLine(longest.c_str()).command.text_size == core::console::kMaxCliLine);
  const std::string over = "rak cli " + std::string(core::console::kMaxCliLine + 1, 'x');
  CHECK(errorOf(over.c_str()) == "rak cli: the line is 1 to 160 bytes");
}

// ------------------------------------------------------------------ the slot (task p1-06)

TEST_CASE("every rak command takes an optional slot after the word rak, @0 without one") {
  CHECK(parseLine("rak hello").command.slot == 0);
  CHECK(parseLine("rak @0 hello").command.slot == 0);
  CHECK(parseLine("rak @1 hello").command.slot == 1);
  CHECK(verbOf("rak @1 hello") == Verb::kRakHello);
  CHECK(verbOf("rak @1 reset") == Verb::kRakReset);
  CHECK(verbOf("rak @1 swd id") == Verb::kRakSwdId);
  CHECK(verbOf("rak @1 flash-test") == Verb::kRakFlashTest);
  CHECK(parseLine("rak @1 kill 5").command.seconds == 5);
  CHECK(parseLine("rak @1 kill 5").command.slot == 1);
  CHECK(parseLine("rak @1 swd mem 0x10000100 4").command.address == 0x10000100);
  CHECK(parseLine("rak @1 swd mem 0x10000100 4").command.words == 4);
  CHECK(parseLine("rak @1 time set 1760000000").command.epoch == 1760000000);
  // The slot is in front, not behind, so `rak cli` can still take the rest of the line verbatim.
  CHECK(std::string(parseLine("rak @1 cli get radio").command.text) == "get radio");
  CHECK(std::string(parseLine("rak cli get radio @1").command.text) == "get radio @1");
  CHECK(parseLine("rak cli get radio @1").command.slot == 0);
  // Spacing is a line's own business here too.
  CHECK(parseLine("rak   @1   ping").command.slot == 1);
}

TEST_CASE("a slot that is not a position of any adapter is refused, and so is a slot alone") {
  CHECK(errorOf("rak @2 hello") == "rak: not a slot (@0 or @1): @2");
  CHECK(errorOf("rak @9 hello") == "rak: not a slot (@0 or @1): @9");
  CHECK(errorOf("rak @ hello") == "rak: not a slot (@0 or @1): @");
  CHECK(errorOf("rak @a hello") == "rak: not a slot (@0 or @1): @a");
  CHECK(errorOf("rak @-1 hello") == "rak: not a slot (@0 or @1): @-1");
  CHECK(errorOf("rak @1") == "rak needs a subcommand (try 'help')");
  CHECK(errorOf("rak") == "rak needs a subcommand (try 'help')");
  // An @ anywhere but in front of the subcommand is the subcommand's own text.
  CHECK(errorOf("rak hello @1") == "rak hello takes no argument");
}
