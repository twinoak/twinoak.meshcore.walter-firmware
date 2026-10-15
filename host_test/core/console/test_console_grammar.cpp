// One grammar, one table, one document (task p3-03). The console grew a command at a time from
// phase 0 onwards; this is what holds the three lists to each other, so that a command cannot be
// added to two of them:
//
//   * every row of core/console/commands.h parses into something - never "unknown";
//   * `help` prints every row's usage and every row's help line;
//   * every row's words appear in `docs/console.md`, and so does every settings key;
//   * **no command is parsed anywhere else**: `parseLine` is defined once and called from one
//     file, at one place, and no other file of the firmware matches a command word of its own.
//
// The last one is a test over the tree rather than a sentence in a header, after
// host_test/core/net/test_trust_rail.cpp.
#include <doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/console/commands.h"
#include "core/console/parser.h"
#include "core/settings/keys.h"
#include "settings_console_rig.h"

namespace fs = std::filesystem;

using bench::hasText;
using bench::SettingsConsole;
using core::console::CommandDef;
using core::console::Group;
using core::console::kCommandCount;
using core::console::kCommands;

namespace {

std::string readFile(const fs::path& p) {
  REQUIRE_MESSAGE(fs::is_regular_file(p), p.string());
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string consoleDoc() { return readFile(fs::path(TWINOAK_REPO_ROOT) / "docs" / "console.md"); }

// Does the document name this command: "`<word>`" or "`<word> <arguments>`" in a backquoted
// span, which is how every command of docs/console.md is written.
bool documented(const std::string& doc, const char* word) {
  const std::string quoted = std::string("`") + word;
  for (std::size_t at = doc.find(quoted); at != std::string::npos;
       at = doc.find(quoted, at + 1)) {
    const char next = doc[at + quoted.size()];
    if (next == '`' || next == ' ') return true;
  }
  return false;
}

std::vector<fs::path> firmwareSources() {
  std::vector<fs::path> files;
  for (const char* dir : {"components", "main"}) {
    const fs::path root = fs::path(TWINOAK_REPO_ROOT) / dir;
    REQUIRE(fs::is_directory(root));
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) continue;
      const std::string ext = entry.path().extension().string();
      if (ext == ".cpp" || ext == ".h") files.push_back(entry.path());
    }
  }
  return files;
}

bool isCommentLine(const std::string& line) {
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  return line.compare(i, 2, "//") == 0 || line.compare(i, 2, "/*") == 0 ||
         line.compare(i, 1, "*") == 0;
}

// How many code lines of `source` contain `needle`.
std::size_t codeLinesWith(const std::string& source, const char* needle) {
  std::size_t n = 0;
  std::istringstream in(source);
  std::string line;
  while (std::getline(in, line)) {
    if (isCommentLine(line)) continue;
    if (line.find(needle) != std::string::npos) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("every command of the table parses, and none of them is unknown to the parser") {
  for (const CommandDef& command : kCommands) {
    const core::console::Parsed p = core::console::parseLine(command.word);
    if (p.ok) continue;
    // A command whose words alone are not a whole line is refused for want of an argument -
    // never because the parser has never heard of it.
    const std::string why = p.error;
    const std::string word = command.word;
    CHECK_MESSAGE(why.find("unknown") == std::string::npos, word << ": " << why);
    CHECK_MESSAGE(!why.empty(), word);
  }
}

TEST_CASE("every row of the table has a usage, words, a group and one line of help") {
  CHECK(kCommandCount == sizeof kCommands / sizeof kCommands[0]);
  for (const CommandDef& command : kCommands) {
    const std::string usage = command.usage != nullptr ? command.usage : "";
    CHECK_MESSAGE(!usage.empty(), "a row with no usage");
    CHECK_MESSAGE(command.word != nullptr, usage);
    CHECK_MESSAGE(command.word[0] != 0, usage);
    CHECK_MESSAGE(command.help != nullptr, usage);
    CHECK_MESSAGE(command.help[0] != 0, usage);
    CHECK_MESSAGE(command.group != Group::kCount, usage);
    // The usage begins with the words: `rak kill <seconds> [confirm]` begins `rak kill`.
    CHECK_MESSAGE(usage.rfind(command.word, 0) == 0, usage);
    // And no two rows are the same command.
    for (const CommandDef& other : kCommands) {
      if (&other == &command) continue;
      CHECK_MESSAGE(std::strcmp(other.word, command.word) != 0, std::string(command.word));
    }
  }
  for (std::uint8_t g = 0; g < static_cast<std::uint8_t>(Group::kCount); ++g) {
    CHECK(std::string(core::console::describe(static_cast<Group>(g))) != "?");
  }
}

TEST_CASE("help prints every command of the table, grouped, and every settings key") {
  SettingsConsole rig;
  const std::string text = rig.run("help");
  for (const CommandDef& command : kCommands) {
    CHECK_MESSAGE(hasText(text, command.usage), std::string(command.usage));
    // The first few words of the help line as well, so that a row with a usage and no sentence
    // cannot pass.
    const std::string head = std::string(command.help).substr(0, 24);
    CHECK_MESSAGE(hasText(text, head.c_str()), std::string(command.usage));
  }
  for (std::uint8_t g = 0; g < static_cast<std::uint8_t>(Group::kCount); ++g) {
    CHECK(hasText(text, core::console::describe(static_cast<Group>(g))));
  }
  for (const core::settings::KeyDef& key : core::settings::kKeys) {
    CHECK_MESSAGE(hasText(text, key.name), std::string(key.name));
    CHECK_MESSAGE(hasText(text, std::string(key.help).substr(0, 20).c_str()),
                  std::string(key.name));
  }
  // And nothing of `help` is longer than a terminal window: the table's sentences are wrapped.
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    CHECK_MESSAGE(line.size() <= 100, line);  // a std::string: safe in a message
  }
}

TEST_CASE("docs/console.md names every command of the table and every settings key") {
  const std::string doc = consoleDoc();
  for (const CommandDef& command : kCommands) {
    CHECK_MESSAGE(documented(doc, command.word), std::string(command.word));
  }
  for (const core::settings::KeyDef& key : core::settings::kKeys) {
    CHECK_MESSAGE(documented(doc, key.name), std::string(key.name));
    // And what happens when it changes, which is the table's own `effect` column.
    CHECK_MESSAGE(doc.find(key.effect) != std::string::npos, std::string(key.name));
  }
  // The thing a person with the board in hand has to know, said out loud and not hidden.
  const bool says_not_encrypted = doc.find("not encrypted") != std::string::npos ||
                                  doc.find("NOT encrypted") != std::string::npos;
  CHECK(says_not_encrypted);
  // `documented` would pass on anything: shown refusing a command that is not there.
  CHECK_FALSE(documented(doc, "rak sing"));
}

TEST_CASE("no command is parsed anywhere else: parseLine has one definition and one caller") {
  std::size_t declarations = 0;
  std::size_t definitions = 0;
  std::size_t calls = 0;
  std::vector<std::string> callers;
  for (const fs::path& file : firmwareSources()) {
    const std::string source = readFile(file);
    const std::string name = file.filename().string();
    const std::size_t hits = codeLinesWith(source, "parseLine");
    if (hits == 0) continue;
    if (name == "parser.h") {
      declarations += hits;
    } else if (name == "parser.cpp") {
      definitions += hits;
    } else {
      calls += hits;
      callers.push_back(name);
    }
  }
  CHECK(declarations == 1);  // the one declaration, in parser.h
  CHECK(definitions == 1);   // the one definition, in parser.cpp
  CHECK(calls == 1);         // the one call, in console.cpp
  REQUIRE(callers.size() == 1);
  CHECK(callers[0] == "console.cpp");
}

TEST_CASE("no file but the parser matches a command word of its own") {
  // The parser compares a typed word with a command's; anything else in the firmware that did
  // the same would be a second grammar. The five words every command of the table begins with
  // are searched for as compared literals.
  const char* verbs[] = {"\"rak\"", "\"image\"", "\"self\"", "\"lte\"", "\"trust\"",
                         "\"help\"", "\"status\"", "\"get\"", "\"set\"", "\"erase\""};
  for (const fs::path& file : firmwareSources()) {
    const std::string name = file.filename().string();
    if (name == "parser.cpp" || name == "parser.h" || name == "commands.cpp" ||
        name == "commands.h") {
      continue;
    }
    const std::string source = readFile(file);
    for (const char* verb : verbs) {
      std::istringstream in(source);
      std::string line;
      int number = 0;
      while (std::getline(in, line)) {
        ++number;
        if (isCommentLine(line)) continue;
        if (line.find("strcmp") == std::string::npos) continue;
        CHECK_MESSAGE(line.find(verb) == std::string::npos,
                      name << ":" << number << " compares a command word: " << line);
      }
    }
  }
}
