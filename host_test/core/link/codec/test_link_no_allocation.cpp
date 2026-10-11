// The codec allocates nothing - the task's third acceptance criterion, as a test and not a
// promise. It reads every file of the codec, strips the comments and the string literals, and
// fails on a word that allocates or on an include that could bring one in.
//
// A grep would do the same thing, but a grep lives in a script somebody has to keep running;
// this runs with the rest of the host tests, names the file and the line, and fails the build
// through them. The two directories are the codec's sources and its headers - `core/` keeps its
// public headers under `include/core/`, so "under components/core/link/codec/" is both.
#include <doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* const kCodecDirectories[] = {
    "components/core/link/codec",
    "components/core/include/core/link/codec",
};

// What a line may not contain once the comments and the literals are gone. Each is matched as a
// whole word, so "renew" and "delete_me" would not be mistaken for one.
const char* const kForbiddenWords[] = {
    "new", "delete", "malloc", "calloc", "realloc", "free", "alloca", "strdup",
    "vector", "string", "map", "set", "list", "function", "unique_ptr", "shared_ptr",
};

// What the codec may include: the two freestanding headers it needs, its own, and the one
// interface the frame writer takes (a clock, for §2's idle 0x00).
const char* const kAllowedIncludes[] = {
    "cstddef",
    "cstdint",
    "interfaces/clock.h",
    "core/link/codec/cobs.h",
    "core/link/codec/crc16.h",
    "core/link/codec/frame.h",
    "core/link/codec/messages.h",
    "core/link/codec/packet.h",
    "core/link/codec/reader.h",
    "core/link/codec/view.h",
    "core/link/codec/wire.h",
    "core/link/codec/writer.h",
};

bool isWordByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The code of a line: no // comment, no /* */ comment (across lines, hence the state), no string
// and no character literal. Everything removed becomes a space, so words cannot grow together.
std::string codeOnly(const std::string& line, bool* in_block_comment) {
  std::string out;
  out.reserve(line.size());
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (*in_block_comment) {
      if (line.compare(i, 2, "*/") == 0) {
        *in_block_comment = false;
        ++i;
      }
      out.push_back(' ');
      continue;
    }
    if (line.compare(i, 2, "//") == 0) {
      break;
    }
    if (line.compare(i, 2, "/*") == 0) {
      *in_block_comment = true;
      out.push_back(' ');
      ++i;
      continue;
    }
    if (line[i] == '"' || line[i] == '\'') {
      const char quote = line[i];
      out.push_back(' ');
      for (++i; i < line.size(); ++i) {
        if (line[i] == '\\') {
          ++i;  // an escaped quote is not the end of the literal
          continue;
        }
        if (line[i] == quote) {
          break;
        }
      }
      continue;
    }
    out.push_back(line[i]);
  }
  return out;
}

std::vector<std::string> codecFiles() {
  std::vector<std::string> files;
  for (const char* directory : kCodecDirectories) {
    const std::filesystem::path path = std::filesystem::path(TWINOAK_REPO_ROOT) / directory;
    REQUIRE_MESSAGE(std::filesystem::is_directory(path), "no such directory: " << path.string());
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_regular_file()) {
        files.push_back(entry.path().string());
      }
    }
  }
  return files;
}

std::string includedBy(const std::string& code) {
  const std::size_t hash = code.find('#');
  if (hash == std::string::npos || code.find("include", hash) == std::string::npos) {
    return std::string();
  }
  const std::size_t open = code.find_first_of("<\"", hash);
  if (open == std::string::npos) {
    return std::string();
  }
  const char close = code[open] == '<' ? '>' : '"';
  const std::size_t end = code.find(close, open + 1);
  if (end == std::string::npos) {
    return std::string();
  }
  return code.substr(open + 1, end - open - 1);
}

}  // namespace

TEST_CASE("nothing under components/core/link/codec/ allocates") {
  const std::vector<std::string> files = codecFiles();
  // A guard on the test: a path that stopped matching anything would pass by doing nothing.
  CHECK(files.size() >= 10);

  std::size_t lines = 0;
  for (const std::string& file : files) {
    std::ifstream in(file);
    REQUIRE_MESSAGE(in.good(), "cannot read " << file);
    std::string line;
    bool in_block_comment = false;
    for (std::size_t number = 1; std::getline(in, line); ++number) {
      const std::string code = codeOnly(line, &in_block_comment);
      ++lines;

      for (const char* word : kForbiddenWords) {
        const std::size_t length = std::string(word).size();
        for (std::size_t at = code.find(word); at != std::string::npos;
             at = code.find(word, at + 1)) {
          const bool before = at > 0 && isWordByte(code[at - 1]);
          const bool after = at + length < code.size() && isWordByte(code[at + length]);
          // Part of a longer identifier - "kUnset" holds "set" - and not the word itself.
          const bool inside_a_word = before || after;
          // The word as a std::string: doctest prints a `const char*` variable as a pointer
          // (`filldata<T*>` in doctest.h), and a message that cannot name the word is no use.
          CHECK_MESSAGE(inside_a_word, file << ":" << number
                                           << " allocates, or looks as if it does: \""
                                           << std::string(word) << "\" in " << code);
        }
      }

      const std::string included = includedBy(code);
      if (included.empty()) {
        continue;
      }
      bool allowed = false;
      for (const char* candidate : kAllowedIncludes) {
        allowed = allowed || included == candidate;
      }
      CHECK_MESSAGE(allowed, file << ":" << number << " includes <" << included
                                 << ">, which is not one of the headers the codec may use");
    }
  }
  CHECK(lines >= 500);  // the codec is about a thousand lines; a file read as empty would pass
}
