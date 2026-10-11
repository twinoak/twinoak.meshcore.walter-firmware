// The codec allocates nothing - task p1-01's third acceptance criterion, as a test and not a
// promise - and neither does the client above it (task p1-02: core/ has no heap, and a client
// whose buffers are members is one a reader can size). It reads every file of both, strips the
// comments and the string literals, and fails on a word that allocates or on an include that
// could bring one in.
//
// A grep would do the same thing, but a grep lives in a script somebody has to keep running;
// this runs with the rest of the host tests, names the file and the line, and fails the build
// through them. Each part has two directories - its sources and its headers - because `core/`
// keeps its public headers under `include/core/`.
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

const char* const kClientDirectories[] = {
    "components/core/link/client",
    "components/core/include/core/link/client",
};

// What a line may not contain once the comments and the literals are gone. Each is matched as a
// whole word, so "renew" and "delete_me" would not be mistaken for one.
const char* const kForbiddenWords[] = {
    "new", "delete", "malloc", "calloc", "realloc", "free", "alloca", "strdup",
    "vector", "string", "map", "set", "list", "function", "unique_ptr", "shared_ptr",
};

// What the codec may include: the two freestanding headers it needs, its own, and the one
// interface the frame writer takes (a clock, for §2's idle 0x00).
const char* const kCodecIncludes[] = {
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

// What the client may include: the codec's public headers, the four interfaces it is written
// against, the board object that owns its two pins, its own headers, and the version string it
// puts in a HELLO. Nothing of ESP-IDF, nothing of the standard library but the two freestanding
// headers (tools/cmake/no_idf_include.cmake checks the first half of that for all of core/).
const char* const kClientIncludes[] = {
    "cstddef",
    "cstdint",
    "core/board/radio_pins.h",
    "core/link/client/client.h",
    "core/link/client/port.h",
    "core/link/client/words.h",
    "core/link/codec/frame.h",
    "core/link/codec/messages.h",
    "core/link/codec/packet.h",
    "core/link/codec/view.h",
    "core/link/codec/wire.h",
    "core/version.h",
    "interfaces/byte_stream.h",
    "interfaces/clock.h",
    "interfaces/wall_clock.h",
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

std::vector<std::string> filesUnder(const char* const* directories, std::size_t count) {
  std::vector<std::string> files;
  for (std::size_t i = 0; i < count; ++i) {
    const std::filesystem::path path = std::filesystem::path(TWINOAK_REPO_ROOT) / directories[i];
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

// One part of core/link/: every file of it read, every line checked. `what` names the part in the
// failure message, `least_files` and `least_lines` are the guards on the test itself - a path that
// stopped matching anything, or a file read as empty, would otherwise pass by doing nothing.
void checkNoAllocation(const std::string& what, const std::vector<std::string>& files,
                       const char* const* allowed_includes, std::size_t allowed_count,
                       std::size_t least_files, std::size_t least_lines) {
  CHECK_MESSAGE(files.size() >= least_files, what);

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
      for (std::size_t i = 0; i < allowed_count; ++i) {
        allowed = allowed || included == allowed_includes[i];
      }
      CHECK_MESSAGE(allowed, file << ":" << number << " includes <" << included
                                 << ">, which is not one of the headers " << what << " may use");
    }
  }
  CHECK_MESSAGE(lines >= least_lines, what);
}

}  // namespace

TEST_CASE("nothing under components/core/link/codec/ allocates") {
  // The codec is about a thousand lines in thirteen files (task p1-01).
  checkNoAllocation("the codec",
                    filesUnder(kCodecDirectories,
                               sizeof kCodecDirectories / sizeof kCodecDirectories[0]),
                    kCodecIncludes, sizeof kCodecIncludes / sizeof kCodecIncludes[0], 10, 500);
}

TEST_CASE("nothing under components/core/link/client/ allocates") {
  // The client is six files: the port, the client, the words and their headers (task p1-02).
  checkNoAllocation("the client",
                    filesUnder(kClientDirectories,
                               sizeof kClientDirectories / sizeof kClientDirectories[0]),
                    kClientIncludes, sizeof kClientIncludes / sizeof kClientIncludes[0], 6, 500);
}
