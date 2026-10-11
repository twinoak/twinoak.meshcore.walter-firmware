#include "core/console/parser.h"

#include <cstring>

namespace core {
namespace console {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Copies text into out, bounded, always terminated.
void put(char* out, std::size_t size, const char* text) {
  std::size_t i = 0;
  for (; text[i] != 0 && i + 1 < size; ++i) out[i] = text[i];
  out[i] = 0;
}

// out = a + b, bounded. Enough for the messages here; no printf, no heap.
void put2(char* out, std::size_t size, const char* a, const char* b) {
  put(out, size, a);
  const std::size_t n = std::strlen(out);
  if (n + 1 < size) put(out + n, size - n, b);
}

Parsed refuse(const char* message) {
  Parsed p;
  p.ok = false;
  put(p.error, sizeof p.error, message);
  return p;
}

Parsed refuse2(const char* message, const char* detail) {
  Parsed p;
  p.ok = false;
  put2(p.error, sizeof p.error, message, detail);
  return p;
}

Parsed accept(Verb verb) {
  Parsed p;
  p.ok = true;
  p.command.verb = verb;
  return p;
}

// A decimal number of seconds: digits only, no sign, no fraction, at most 9 digits so that it
// cannot overflow. The bounds themselves are RadioPins' (kKillTiming), not the parser's.
bool parseSeconds(const char* text, std::uint32_t& out) {
  if (*text == 0) return false;
  std::uint32_t value = 0;
  std::size_t digits = 0;
  for (const char* p = text; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return false;
    if (++digits > 9) return false;
    value = value * 10 + static_cast<std::uint32_t>(*p - '0');
  }
  out = value;
  return true;
}

}  // namespace

Parsed parseLine(const char* line) {
  if (line == nullptr) return accept(Verb::kEmpty);

  // Bound the line first, then copy it so the tokens can be cut in place.
  const std::size_t raw = std::strlen(line);
  if (raw > kMaxLineLength + 2) {  // + a CR LF a terminal may append
    return refuse("line too long (200 characters at most)");
  }
  char buf[kMaxLineLength + 3];
  std::memcpy(buf, line, raw + 1);

  for (const char* p = buf; *p != 0; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 && !isSpace(*p)) return refuse("control characters are not accepted");
    if (c > 0x7e) return refuse("printable ASCII only");
  }

  // Trim both ends.
  char* start = buf;
  while (*start != 0 && isSpace(*start)) ++start;
  char* end = start + std::strlen(start);
  while (end > start && isSpace(end[-1])) --end;
  *end = 0;
  if (*start == 0) return accept(Verb::kEmpty);
  if (static_cast<std::size_t>(end - start) > kMaxLineLength) {
    return refuse("line too long (200 characters at most)");
  }

  // The verb, then the rest.
  char* verb = start;
  char* rest = verb;
  while (*rest != 0 && !isSpace(*rest)) ++rest;
  if (*rest != 0) {
    *rest++ = 0;
    while (*rest != 0 && isSpace(*rest)) ++rest;
  }
  const bool hasRest = *rest != 0;

  if (std::strcmp(verb, "help") == 0) {
    if (hasRest) return refuse("help takes no argument");
    return accept(Verb::kHelp);
  }
  if (std::strcmp(verb, "status") == 0) {
    if (hasRest) return refuse("status takes no argument");
    return accept(Verb::kStatus);
  }
  if (std::strcmp(verb, "reboots") == 0) {
    if (hasRest) return refuse("reboots takes no argument");
    return accept(Verb::kReboots);
  }
  if (std::strcmp(verb, "pins") == 0) {
    if (hasRest) return refuse("pins takes no argument");
    return accept(Verb::kPins);
  }
  if (std::strcmp(verb, "reboot") == 0) {
    if (hasRest) return refuse("reboot takes no argument");
    return accept(Verb::kReboot);
  }
  if (std::strcmp(verb, "rak") == 0) {
    if (!hasRest) return refuse("rak needs a subcommand: rak reset, rak kill <seconds>");
    char* sub = rest;
    char* arg = sub;
    while (*arg != 0 && !isSpace(*arg)) ++arg;
    if (*arg != 0) {
      *arg++ = 0;
      while (*arg != 0 && isSpace(*arg)) ++arg;
    }
    const bool hasArg = *arg != 0;
    if (std::strcmp(sub, "reset") == 0) {
      if (hasArg) return refuse("rak reset takes no argument");
      return accept(Verb::kRakReset);
    }
    if (std::strcmp(sub, "kill") == 0) {
      if (!hasArg) return refuse("rak kill needs a duration: rak kill <seconds>");
      for (const char* p = arg; *p != 0; ++p) {
        if (isSpace(*p)) return refuse("rak kill takes one number: rak kill <seconds>");
      }
      Parsed p = accept(Verb::kRakKill);
      if (!parseSeconds(arg, p.command.seconds)) {
        return refuse2("rak kill: not a whole number of seconds: ", arg);
      }
      return p;
    }
    return refuse2("unknown rak subcommand (reset, kill <seconds>): ", sub);
  }
  return refuse2("unknown command (try 'help'): ", verb);
}

}  // namespace console
}  // namespace core
