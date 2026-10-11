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

// out = a + b + c, bounded: a refusal that names the subcommand it is about.
Parsed refuse3(const char* a, const char* b, const char* c) {
  Parsed p;
  p.ok = false;
  put(p.error, sizeof p.error, a);
  std::size_t n = std::strlen(p.error);
  if (n + 1 < sizeof p.error) put(p.error + n, sizeof p.error - n, b);
  n = std::strlen(p.error);
  if (n + 1 < sizeof p.error) put(p.error + n, sizeof p.error - n, c);
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

// A Unix epoch: digits only, at most ten of them (4 294 967 295 is ten digits) and inside a u32.
// §5.4's `epoch` is a u32, and 2026 is already 1.7 billion, so the nine digits rak kill allows
// itself are not enough here.
bool parseEpoch(const char* text, std::uint32_t& out) {
  if (*text == 0) return false;
  std::uint64_t value = 0;
  std::size_t digits = 0;
  for (const char* p = text; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return false;
    if (++digits > 10) return false;
    value = value * 10 + static_cast<std::uint64_t>(*p - '0');
    if (value > 0xFFFFFFFFull) return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

// A 32-bit address: hexadecimal with a 0x prefix (at most eight digits), or decimal (at most ten
// digits, inside a u32). The alignment is checked by the caller, so that the message can say so.
bool parseAddress(const char* text, std::uint32_t& out) {
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    const char* p = text + 2;
    if (*p == 0) return false;
    std::uint32_t value = 0;
    std::size_t digits = 0;
    for (; *p != 0; ++p) {
      std::uint32_t d;
      if (*p >= '0' && *p <= '9') {
        d = static_cast<std::uint32_t>(*p - '0');
      } else if (*p >= 'a' && *p <= 'f') {
        d = static_cast<std::uint32_t>(*p - 'a' + 10);
      } else if (*p >= 'A' && *p <= 'F') {
        d = static_cast<std::uint32_t>(*p - 'A' + 10);
      } else {
        return false;
      }
      if (++digits > 8) return false;
      value = (value << 4) | d;
    }
    out = value;
    return true;
  }
  return parseEpoch(text, out);
}

// The subcommands of `rak` that take nothing after them.
struct Simple {
  const char* word;
  Verb verb;
};
constexpr Simple kSimple[] = {
    {"reset", Verb::kRakReset},   {"hello", Verb::kRakHello},  {"ping", Verb::kRakPing},
    {"status", Verb::kRakStatus}, {"id", Verb::kRakId},        {"power", Verb::kRakPower},
    {"env", Verb::kRakEnv},       {"events", Verb::kRakEvents}, {"text", Verb::kRakText},
    {"flash-test", Verb::kRakFlashTest},
};

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
    if (!hasRest) return refuse("rak needs a subcommand (try 'help')");
    char* sub = rest;
    char* arg = sub;
    while (*arg != 0 && !isSpace(*arg)) ++arg;
    if (*arg != 0) {
      *arg++ = 0;
      while (*arg != 0 && isSpace(*arg)) ++arg;
    }
    const bool hasArg = *arg != 0;
    for (const Simple& simple : kSimple) {
      if (std::strcmp(sub, simple.word) != 0) continue;
      if (hasArg) return refuse3("rak ", simple.word, " takes no argument");
      return accept(simple.verb);
    }
    // rak time, and rak time set <epoch> (§5.4: source 3, a person at a console).
    if (std::strcmp(sub, "time") == 0) {
      if (!hasArg) return accept(Verb::kRakTime);
      char* word = arg;
      char* value = word;
      while (*value != 0 && !isSpace(*value)) ++value;
      if (*value != 0) {
        *value++ = 0;
        while (*value != 0 && isSpace(*value)) ++value;
      }
      if (std::strcmp(word, "set") != 0) {
        return refuse2("rak time: unknown word (rak time, rak time set <epoch>): ", word);
      }
      if (*value == 0) return refuse("rak time set needs a unix epoch: rak time set <epoch>");
      for (const char* p = value; *p != 0; ++p) {
        if (isSpace(*p)) return refuse("rak time set takes one number: rak time set <epoch>");
      }
      Parsed p = accept(Verb::kRakTimeSet);
      if (!parseEpoch(value, p.command.epoch)) {
        return refuse2("rak time set: not a unix epoch in seconds: ", value);
      }
      return p;
    }
    // rak cli <line>: the rest of the line verbatim, spaces and all (§5.7).
    if (std::strcmp(sub, "cli") == 0) {
      if (!hasArg) return refuse("rak cli needs a line: rak cli <line>");
      const std::size_t n = std::strlen(arg);
      if (n > kMaxCliLine) {
        return refuse("rak cli: the line is 1 to 160 bytes");
      }
      Parsed p = accept(Verb::kRakCli);
      std::memcpy(p.command.text, arg, n + 1);
      p.command.text_size = n;
      return p;
    }
    // rak swd <operation> (tasks p1-03, p1-04): connect, id, mem <addr> [words].
    if (std::strcmp(sub, "swd") == 0) {
      if (!hasArg) return refuse("rak swd needs an operation: rak swd connect | id | mem <addr> [words]");
      char* op = arg;
      char* operand = op;
      while (*operand != 0 && !isSpace(*operand)) ++operand;
      if (*operand != 0) {
        *operand++ = 0;
        while (*operand != 0 && isSpace(*operand)) ++operand;
      }
      const bool hasOperand = *operand != 0;
      if (std::strcmp(op, "connect") == 0) {
        if (hasOperand) return refuse("rak swd connect takes no argument");
        return accept(Verb::kRakSwdConnect);
      }
      if (std::strcmp(op, "id") == 0) {
        if (hasOperand) return refuse("rak swd id takes no argument");
        return accept(Verb::kRakSwdId);
      }
      if (std::strcmp(op, "mem") == 0) {
        if (!hasOperand) return refuse("rak swd mem needs an address: rak swd mem <addr> [words]");
        char* count = operand;
        while (*count != 0 && !isSpace(*count)) ++count;
        if (*count != 0) {
          *count++ = 0;
          while (*count != 0 && isSpace(*count)) ++count;
        }
        for (const char* p = count; *p != 0; ++p) {
          if (isSpace(*p)) return refuse("rak swd mem takes an address and a word count: rak swd mem <addr> [words]");
        }
        Parsed p = accept(Verb::kRakSwdMem);
        if (!parseAddress(operand, p.command.address)) {
          return refuse2("rak swd mem: not an address (hex 0x... or decimal, 32 bits): ", operand);
        }
        if ((p.command.address & 0x3u) != 0) {
          return refuse2("rak swd mem: the address must be word-aligned (a multiple of 4): ", operand);
        }
        if (*count != 0) {
          std::uint32_t words = 0;
          if (!parseSeconds(count, words) || words < 1 || words > kMaxMemWords) {
            return refuse2("rak swd mem: words is 1 to 32: ", count);
          }
          p.command.words = words;
        }
        return p;
      }
      return refuse2("rak swd: unknown operation (connect, id, mem): ", op);
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
    return refuse2("unknown rak subcommand (try 'help'): ", sub);
  }
  return refuse2("unknown command (try 'help'): ", verb);
}

}  // namespace console
}  // namespace core
