#include "walter_agent/json_scan.h"

#include <cstring>

namespace agent {
namespace json {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::size_t skipSpace(const char* text, std::size_t len, std::size_t at) {
  while (at < len && isSpace(text[at])) ++at;
  return at;
}

// Past the string that begins at `at` (its opening quote): the index after the closing quote, or
// **len + 1** - "malformed" - when the text ends before it closes. (A well-formed value never
// reaches len + 1; the callers read `> len` as not found.)
std::size_t skipString(const char* text, std::size_t len, std::size_t at) {
  if (at >= len || text[at] != '"') return len + 1;
  ++at;
  while (at < len) {
    const char c = text[at];
    if (c == '\\') {
      at += 2;
      continue;
    }
    ++at;
    if (c == '"') return at;
  }
  return len + 1;
}

// Past the value that begins at `at`: a string, a container (to its matching close) or a scalar
// (to the next `,`, `}`, `]` or space). `len + 1` when a string or a container is not closed
// before the text ends.
std::size_t skipValue(const char* text, std::size_t len, std::size_t at) {
  at = skipSpace(text, len, at);
  if (at >= len) return len + 1;
  const char c = text[at];
  if (c == '"') return skipString(text, len, at);
  if (c == '{' || c == '[') {
    int depth = 0;
    while (at < len) {
      const char d = text[at];
      if (d == '"') {
        at = skipString(text, len, at);
        if (at > len) return len + 1;
        continue;
      }
      if (d == '{' || d == '[') ++depth;
      if (d == '}' || d == ']') {
        --depth;
        if (depth == 0) return at + 1;
      }
      ++at;
    }
    return len + 1;
  }
  while (at < len && text[at] != ',' && text[at] != '}' && text[at] != ']' && !isSpace(text[at])) ++at;
  return at;
}

}  // namespace

Value member(const char* obj, std::size_t len, const char* name) {
  Value none;
  if (obj == nullptr || name == nullptr) return none;
  const std::size_t want = std::strlen(name);
  std::size_t at = skipSpace(obj, len, 0);
  if (at >= len || obj[at] != '{') return none;
  ++at;
  for (;;) {
    at = skipSpace(obj, len, at);
    if (at >= len) return none;
    if (obj[at] == '}') return none;
    if (obj[at] == ',') {
      ++at;
      continue;
    }
    if (obj[at] != '"') return none;
    // The key.
    const std::size_t key_begin = at + 1;
    const std::size_t key_end = skipString(obj, len, at);
    if (key_end >= len) return none;
    const std::size_t key_len = key_end - 1 - key_begin;
    at = skipSpace(obj, len, key_end);
    if (at >= len || obj[at] != ':') return none;
    at = skipSpace(obj, len, at + 1);
    const std::size_t value_end = skipValue(obj, len, at);
    if (value_end > len) return none;
    if (key_len == want && std::memcmp(obj + key_begin, name, want) == 0) {
      Value v;
      v.at = obj + at;
      v.len = value_end - at;
      return v;
    }
    at = value_end;
  }
}

Value element(const Value& array, int index) {
  Value none;
  if (!array.found() || index < 0) return none;
  std::size_t at = skipSpace(array.at, array.len, 0);
  if (at >= array.len || array.at[at] != '[') return none;
  ++at;
  int i = 0;
  for (;;) {
    at = skipSpace(array.at, array.len, at);
    if (at >= array.len || array.at[at] == ']') return none;
    if (array.at[at] == ',') {
      ++at;
      continue;
    }
    const std::size_t end = skipValue(array.at, array.len, at);
    if (end > array.len) return none;
    if (i == index) {
      Value v;
      v.at = array.at + at;
      v.len = end - at;
      return v;
    }
    ++i;
    at = end;
  }
}

bool text(const Value& value, char* out, std::size_t cap) {
  if (!value.found() || value.len < 2 || value.at[0] != '"' || out == nullptr || cap == 0) return false;
  std::size_t n = 0;
  for (std::size_t i = 1; i + 1 < value.len; ++i) {
    char c = value.at[i];
    if (c == '\\' && i + 2 < value.len) {
      const char e = value.at[++i];
      switch (e) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u':
          // Four hex digits none of the four payloads carries: stepped over, one `?` kept.
          i += 4 < value.len - 1 - i ? 4 : value.len - 2 - i;
          c = '?';
          break;
        default: c = e; break;
      }
    }
    if (n + 1 >= cap) return false;
    out[n++] = c;
  }
  out[n] = 0;
  return true;
}

bool integer(const Value& value, long long* out) {
  if (!value.found() || value.len == 0 || out == nullptr) return false;
  std::size_t i = 0;
  bool negative = false;
  if (value.at[0] == '-') {
    negative = true;
    i = 1;
  }
  if (i >= value.len) return false;
  long long n = 0;
  for (; i < value.len; ++i) {
    const char c = value.at[i];
    if (c < '0' || c > '9') return false;  // a fraction or an exponent is not an integer here
    if (n > (0x7FFFFFFFFFFFFFFFLL - 9) / 10) return false;
    n = n * 10 + (c - '0');
  }
  *out = negative ? -n : n;
  return true;
}

bool boolean(const Value& value, bool* out) {
  if (!value.found() || out == nullptr) return false;
  if (value.len == 4 && std::memcmp(value.at, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (value.len == 5 && std::memcmp(value.at, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool isNull(const Value& value) {
  return value.found() && value.len == 4 && std::memcmp(value.at, "null", 4) == 0;
}

bool isObject(const Value& value) { return value.found() && value.len >= 2 && value.at[0] == '{'; }

}  // namespace json
}  // namespace agent
