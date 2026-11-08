#include "core/trace/redact.h"

#include <cstdio>
#include <cstring>

namespace core {
namespace trace {

namespace {

// Where a header's value or an AT command's argument ends, looking forward from `at`. The
// library escapes what it sends and receives before it prints it (`escapeBuffer` in
// `WalterModem.cpp`: CR becomes a backslash and an `r`, LF a backslash and an `n`), so in a
// payload line the end of a header is the **two characters** `\r`; in a line of the
// `_transmitCmd` macro, which prints its elements unescaped, it is a real CR or LF or the end of
// the line. All four are ends here, because a rail that only knew one of them would be a rail
// that depended on which line it was looking at.
std::size_t endOfValue(const char* line, std::size_t length, std::size_t at) {
  for (std::size_t i = at; i < length; ++i) {
    if (line[i] == '\r' || line[i] == '\n') return i;
    const bool escape = line[i] == '\\' && i + 1 < length;
    if (escape && (line[i + 1] == 'r' || line[i + 1] == 'n')) return i;
  }
  return length;
}

// Asterisks over `[from, to)`. Returns whether anything was there to take out.
bool blank(char* line, std::size_t from, std::size_t to) {
  if (to <= from) return false;
  for (std::size_t i = from; i < to; ++i) line[i] = kAsterisk;
  return true;
}

// `needle` at `at` in a line of `length` bytes.
bool matches(const char* line, std::size_t length, std::size_t at, const char* needle,
             std::size_t needle_length) {
  if (at + needle_length > length) return false;
  return std::memcmp(line + at, needle, needle_length) == 0;
}

// Everything after `prefix` up to the end of its value, taken out, wherever `prefix` occurs. The
// spaces that follow the prefix are left as they are: a reader of the trace should see that the
// header or the command was there, with its shape, and only the value gone.
std::size_t blankAfter(char* line, std::size_t length, const char* prefix) {
  const std::size_t prefix_length = std::strlen(prefix);
  std::size_t taken = 0;
  for (std::size_t at = 0; at + prefix_length <= length; ++at) {
    if (!matches(line, length, at, prefix, prefix_length)) continue;
    std::size_t from = at + prefix_length;
    while (from < length && line[from] == ' ') ++from;
    const std::size_t to = endOfValue(line, length, from);
    if (blank(line, from, to)) ++taken;
    // On from the end of the value: `to` is at least `from`, which is past `at`, so this never
    // goes backwards and the loop's own ++at lands on `to`.
    at = to - 1;
  }
  return taken;
}

}  // namespace

bool Redactor::keep(std::size_t which, const char* value) {
  if (which >= kSecrets) return false;
  secret_[which][0] = 0;
  if (value == nullptr || value[0] == 0) return true;
  const std::size_t length = std::strlen(value);
  if (length > kMaxSecret) return false;
  std::memcpy(secret_[which], value, length + 1);
  return true;
}

void Redactor::forget() {
  for (std::size_t i = 0; i < kSecrets; ++i) secret_[i][0] = 0;
}

std::size_t Redactor::holding() const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kSecrets; ++i) {
    if (std::strlen(secret_[i]) >= kLeastSecret) ++n;
  }
  return n;
}

std::size_t Redactor::apply(char* line, std::size_t length) const {
  if (line == nullptr || length == 0) return 0;
  // The two positional rails first: they are the exact ones, and they hold for a value this
  // redactor was never given.
  std::size_t taken = blankAfter(line, length, kAuthorization);
  taken += blankAfter(line, length, kPinCommand);
  // Then every held value, wherever it is. Each occurrence becomes asterisks of its own length,
  // so the search can go on from the end of it without finding the same stretch again.
  for (std::size_t i = 0; i < kSecrets; ++i) {
    const std::size_t secret_length = std::strlen(secret_[i]);
    if (secret_length < kLeastSecret || secret_length > length) continue;
    for (std::size_t at = 0; at + secret_length <= length;) {
      if (!matches(line, length, at, secret_[i], secret_length)) {
        ++at;
        continue;
      }
      blank(line, at, at + secret_length);
      ++taken;
      at += secret_length;
    }
  }
  return taken;
}

Formatted formatAndRedact(const Redactor& redactor, char* buffer, std::size_t capacity,
                          const char* format, std::va_list args) {
  Formatted out;
  if (buffer == nullptr || capacity == 0) return out;
  buffer[0] = 0;
  if (format == nullptr) return out;
  const int formatted = std::vsnprintf(buffer, capacity, format, args);
  if (formatted <= 0) return out;
  // vsnprintf answers how long the line *would* have been: a longer one is cut at the buffer.
  out.length = static_cast<std::size_t>(formatted) < capacity
                   ? static_cast<std::size_t>(formatted)
                   : capacity - 1;
  out.taken = redactor.apply(buffer, out.length);
  return out;
}

}  // namespace trace
}  // namespace core
