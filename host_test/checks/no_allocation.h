// "This part of core/ allocates nothing" as a test and not a promise (task p1-01 wrote it for
// the link; task p2-01 moved it here so that the image checks could use it too). It reads every
// source of a part, strips the comments and the string literals, and fails on a word that
// allocates or on an include that could bring one in.
//
// A grep would do the same thing, but a grep lives in a script somebody has to keep running;
// this runs with the rest of the host tests, names the file and the line, and fails the build
// through them.
#pragma once

#include <doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace checks {

// What a line may not contain once the comments and the literals are gone. Each is matched as a
// whole word, so "renew" and "delete_me" would not be mistaken for one.
inline const char* const kForbiddenWords[] = {
    "new", "delete", "malloc", "calloc", "realloc", "free", "alloca", "strdup",
    "vector", "string", "map", "set", "list", "function", "unique_ptr", "shared_ptr",
};

inline bool isWordByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The code of a line: no // comment, no /* */ comment (across lines, hence the state), no string
// and no character literal. Everything removed becomes a space, so words cannot grow together.
inline std::string codeOnly(const std::string& line, bool* in_block_comment) {
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

inline bool isSource(const std::filesystem::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".c";
}

// The source files (not the READMEs and licences beside them) directly under each directory.
inline std::vector<std::string> filesUnder(const char* const* directories, std::size_t count) {
  std::vector<std::string> files;
  for (std::size_t i = 0; i < count; ++i) {
    const std::filesystem::path path = std::filesystem::path(TWINOAK_REPO_ROOT) / directories[i];
    REQUIRE_MESSAGE(std::filesystem::is_directory(path), "no such directory: " << path.string());
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_regular_file() && isSource(entry.path())) {
        files.push_back(entry.path().string());
      }
    }
  }
  return files;
}

inline std::string includedBy(const std::string& code) {
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

// One part of core/: every file of it read, every line checked. `what` names the part in the
// failure message, `least_files` and `least_lines` are the guards on the test itself - a path that
// stopped matching anything, or a file read as empty, would otherwise pass by doing nothing.
inline void checkNoAllocation(const std::string& what, const std::vector<std::string>& files,
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
          bool inside_a_word = before || after;
          // A member call - `w.string(...)`, `p->string(...)` - is a method of an object and not
          // the `std::string` type: the library's bounded, heap-free `mm::JsonWriter::string`
          // (agent/JsonWriter.h) is how components/agent/ writes a job result (task p4-05), and a
          // method named for a forbidden word is not the thing the word forbids.
          const bool member = at >= 1 && (code[at - 1] == '.' || (at >= 2 && code[at - 1] == '>' && code[at - 2] == '-'));
          inside_a_word = inside_a_word || (member && at + length < code.size() && code[at + length] == '(');
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

}  // namespace checks
