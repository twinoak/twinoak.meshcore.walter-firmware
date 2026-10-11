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
  CHECK(verbOf("rak reset") == Verb::kRakReset);
  CHECK(verbOf("rak kill 5") == Verb::kRakKill);
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
  CHECK(errorOf("rak") == "rak needs a subcommand: rak reset, rak kill <seconds>");
  CHECK(errorOf("rak dance") == "unknown rak subcommand (reset, kill <seconds>): dance");
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
