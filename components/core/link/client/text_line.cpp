#include "core/link/client/text_line.h"

namespace core {
namespace link {
namespace client {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

// Where the value of a top-level `"name":` begins, or false. One pass over the line, tracking
// whether it is inside a string and how deep it is in nested objects and arrays, so that a key
// is only a key at the top level: `{"home":{"f":869.6},"cmd":"x"}` finds `cmd`, and
// `{"msg":"cmd:x"}` does not find `cmd` inside the message.
//
// A line that was cut (the client keeps kTextLineMax bytes of one) simply runs out: a field
// before the cut is found and one after it is not, which is the right answer either way.
bool fieldAt(Text line, Text name, std::size_t* at) {
  if (line.data == nullptr || line.size == 0 || line.data[0] != '{') {
    return false;
  }
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  bool have_key = false;  // a top-level string has just ended: it may be a key
  std::size_t key_start = 0;
  std::size_t key_end = 0;
  for (std::size_t i = 1; i < line.size; ++i) {
    const char c = line.data[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
        if (depth == 0) {
          key_end = i;
          have_key = true;
        }
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      if (depth == 0) {
        key_start = i + 1;
      }
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
      have_key = false;
      continue;
    }
    if (c == '}' || c == ']') {
      if (depth == 0) {
        return false;  // the end of the object: the field is not in it
      }
      --depth;
      continue;
    }
    if (depth != 0) {
      continue;
    }
    if (c == ':' && have_key) {
      std::size_t j = i + 1;
      while (j < line.size && isBlank(line.data[j])) {
        ++j;
      }
      if (j >= line.size) {
        return false;
      }
      if (equal(Text{line.data + key_start, key_end - key_start}, name)) {
        *at = j;
        return true;
      }
      have_key = false;
      continue;
    }
    if (c == ',') {
      have_key = false;
    }
  }
  return false;
}

bool wordAt(Text line, std::size_t at, const char* word) {
  std::size_t i = 0;
  for (; word[i] != 0; ++i) {
    if (at + i >= line.size || line.data[at + i] != word[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool lineText(Text line, Text name, Text* out) {
  std::size_t at = 0;
  if (out == nullptr || !fieldAt(line, name, &at) || line.data[at] != '"') {
    return false;
  }
  const std::size_t start = at + 1;
  std::size_t i = start;
  while (i < line.size && line.data[i] != '"') {
    if (line.data[i] == '\\') {
      ++i;
    }
    ++i;
  }
  if (i >= line.size) {
    return false;  // the line ended inside the value: it was cut, and there is no whole answer
  }
  *out = Text{line.data + start, i - start};
  return true;
}

bool lineFlag(Text line, Text name, bool* out) {
  std::size_t at = 0;
  if (out == nullptr || !fieldAt(line, name, &at)) {
    return false;
  }
  if (wordAt(line, at, "true")) {
    *out = true;
    return true;
  }
  if (wordAt(line, at, "false")) {
    *out = false;
    return true;
  }
  return false;
}

bool lineIs(Text line, Text type) {
  Text kind;
  return lineText(line, text("t"), &kind) && equal(kind, type);
}

bool isDoneFor(Text line, Text command) {
  if (command.data == nullptr || command.size == 0 || !lineIs(line, text("done"))) {
    return false;
  }
  Text said;
  return lineText(line, text("cmd"), &said) && equal(said, command);
}

Text firstWord(Text line) {
  if (line.data == nullptr) {
    return Text{};
  }
  std::size_t start = 0;
  while (start < line.size && isBlank(line.data[start])) {
    ++start;
  }
  std::size_t end = start;
  while (end < line.size && !isBlank(line.data[end])) {
    ++end;
  }
  return Text{line.data + start, end - start};
}

}  // namespace client
}  // namespace link
}  // namespace core
