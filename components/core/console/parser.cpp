#include "core/console/parser.h"

#include <cstdio>
#include <cstring>

#include "core/image/store.h"

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
    // The supervisor (task p2-05): the ladder, run now.
    {"ladder", Verb::kRakLadder},
};

// `rak <subcommand> ...`, with the slot (if any) already taken off the front: see parseLine.
Parsed parseRak(char* rest) {
  if (*rest == 0) return refuse("rak needs a subcommand (try 'help')");
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
  // rak update [--first-install | status] (task p2-03).
  if (std::strcmp(sub, "update") == 0) {
    if (!hasArg) return accept(Verb::kRakUpdate);
    for (const char* p = arg; *p != 0; ++p) {
      if (isSpace(*p)) return refuse("rak update takes one word: rak update [--first-install | status]");
    }
    if (std::strcmp(arg, "--first-install") == 0) return accept(Verb::kRakUpdateFirstInstall);
    if (std::strcmp(arg, "status") == 0) return accept(Verb::kRakUpdateStatus);
    return refuse2("rak update: unknown word (rak update, rak update --first-install, rak update status): ", arg);
  }
  // rak watch [reset-caps] (task p2-05): the supervisor's view, and the bench's way of emptying
  // the day's counts and lifting a stop.
  if (std::strcmp(sub, "watch") == 0) {
    if (!hasArg) return accept(Verb::kRakWatch);
    for (const char* p = arg; *p != 0; ++p) {
      if (isSpace(*p)) return refuse("rak watch takes one word: rak watch [reset-caps]");
    }
    if (std::strcmp(arg, "reset-caps") == 0) return accept(Verb::kRakWatchResetCaps);
    return refuse2("rak watch: unknown word (rak watch, rak watch reset-caps): ", arg);
  }
  // rak role scanner | repeater (task p2-03).
  if (std::strcmp(sub, "role") == 0) {
    if (!hasArg) return refuse("rak role needs a role: rak role scanner | repeater");
    for (const char* p = arg; *p != 0; ++p) {
      if (isSpace(*p)) return refuse("rak role takes one word: rak role scanner | repeater");
    }
    if (std::strcmp(arg, "scanner") == 0) return accept(Verb::kRakRoleScanner);
    if (std::strcmp(arg, "repeater") == 0) return accept(Verb::kRakRoleRepeater);
    return refuse2("rak role: not one of scanner, repeater: ", arg);
  }
  // rak flash-test confirm (task p1-05, the word from task p3-03): the one command that writes
  // to a radio's flash outside the update machine, so it is never one typo away.
  if (std::strcmp(sub, "flash-test") == 0) {
    if (!hasArg) {
      return refuse3("rak flash-test writes two pages of the radio's flash; to do it, type: "
                     "rak flash-test ", kConfirmWord, "");
    }
    for (const char* p = arg; *p != 0; ++p) {
      if (isSpace(*p)) return refuse2("rak flash-test takes one word: ", kConfirmWord);
    }
    if (std::strcmp(arg, kConfirmWord) != 0) {
      return refuse3("rak flash-test: the word is '", kConfirmWord, "'");
    }
    return accept(Verb::kRakFlashTest);
  }
  if (std::strcmp(sub, "kill") == 0) {
    if (!hasArg) return refuse("rak kill needs a duration: rak kill <seconds>");
    char* number = arg;
    char* word = number;
    while (*word != 0 && !isSpace(*word)) ++word;
    if (*word != 0) {
      *word++ = 0;
      while (*word != 0 && isSpace(*word)) ++word;
    }
    for (const char* p = word; *p != 0; ++p) {
      if (isSpace(*p)) {
        return refuse2("rak kill takes a number and at most one word: ", kConfirmWord);
      }
    }
    Parsed p = accept(Verb::kRakKill);
    if (!parseSeconds(number, p.command.seconds)) {
      return refuse2("rak kill: not a whole number of seconds: ", number);
    }
    // Over a few seconds the radio is off the air long enough for a person to mean it (task
    // p3-03). The bounds themselves are RadioPins' and the console's.
    const bool confirmed = *word != 0 && std::strcmp(word, kConfirmWord) == 0;
    if (*word != 0 && !confirmed) {
      return refuse3("rak kill: the only word after the seconds is '", kConfirmWord, "'");
    }
    if (p.command.seconds > kKillSecondsWithoutWord && !confirmed) {
      Parsed refusal;
      refusal.ok = false;
      std::snprintf(refusal.error, sizeof refusal.error,
                    "rak kill: over %lu s takes the radio off the air; to do it, type: "
                    "rak kill %lu %s",
                    static_cast<unsigned long>(kKillSecondsWithoutWord),
                    static_cast<unsigned long>(p.command.seconds), kConfirmWord);
      return refusal;
    }
    return p;
  }
  return refuse2("unknown rak subcommand (try 'help'): ", sub);
}

// `image <subcommand> ...` (task p2-02). The optional slot comes after the image's role -
// `image verify golden @1` - which is the shape the task gives the commands; `image list` and
// `image bad` are about the whole store and take nothing.
Parsed parseImage(char* rest, std::uint8_t* slot) {
  if (*rest == 0) {
    return refuse("image needs a subcommand: image list | verify <which> | erase <which> | bad | receive <which>");
  }
  char* sub = rest;
  char* arg = sub;
  while (*arg != 0 && !isSpace(*arg)) ++arg;
  if (*arg != 0) {
    *arg++ = 0;
    while (*arg != 0 && isSpace(*arg)) ++arg;
  }
  const bool hasArg = *arg != 0;

  if (std::strcmp(sub, "list") == 0) {
    if (hasArg) return refuse("image list takes no argument");
    return accept(Verb::kImageList);
  }
  if (std::strcmp(sub, "bad") == 0) {
    if (hasArg) return refuse("image bad takes no argument");
    return accept(Verb::kImageBad);
  }

  Verb verb = Verb::kImageVerify;
  if (std::strcmp(sub, "verify") == 0) {
    verb = Verb::kImageVerify;
  } else if (std::strcmp(sub, "erase") == 0) {
    verb = Verb::kImageErase;
  } else if (std::strcmp(sub, "receive") == 0) {
    verb = Verb::kImageReceive;
  } else {
    return refuse2("unknown image subcommand (list, verify, erase, bad, receive): ", sub);
  }

  if (!hasArg) {
    return refuse3("image ", sub, " needs an image: golden, candidate or scanner");
  }
  // The role, then an optional @slot and nothing else.
  char* word = arg;
  char* after = word;
  while (*after != 0 && !isSpace(*after)) ++after;
  if (*after != 0) {
    *after++ = 0;
    while (*after != 0 && isSpace(*after)) ++after;
  }
  core::image::Which which = core::image::Which::kGolden;
  if (!core::image::whichFromName(word, &which)) {
    return refuse2("image: not one of golden, candidate or scanner: ", word);
  }
  // The golden image is never erased by a command (S-5: a known-good image is always on hand);
  // said here so that the refusal names the command and not the store.
  if (verb == Verb::kImageErase && which == core::image::Which::kGolden) {
    return refuse("image erase: the golden image is not erased (image erase candidate | scanner)");
  }
  if (*after != 0) {
    char* slot_word = after;
    char* tail = slot_word;
    while (*tail != 0 && !isSpace(*tail)) ++tail;
    if (*tail != 0) {
      *tail++ = 0;
      while (*tail != 0 && isSpace(*tail)) ++tail;
    }
    if (*tail != 0) return refuse3("image ", sub, " takes an image and an optional @slot");
    std::uint32_t number = 0;
    if (slot_word[0] != '@' || !parseSeconds(slot_word + 1, number) || !board::isSlot(number)) {
      return refuse2("image: not a slot (@0 or @1): ", slot_word);
    }
    *slot = static_cast<std::uint8_t>(number);
  }
  Parsed p = accept(verb);
  p.command.which = static_cast<std::uint8_t>(which);
  return p;
}

// `self <subcommand>` (task p2-04): the Walter's own update. No slot - it is about this board.
Parsed parseSelf(char* rest) {
  if (*rest == 0) {
    return refuse("self needs a subcommand: self status | receive | switch [radio-down] | confirm | reject");
  }
  char* sub = rest;
  char* arg = sub;
  while (*arg != 0 && !isSpace(*arg)) ++arg;
  if (*arg != 0) {
    *arg++ = 0;
    while (*arg != 0 && isSpace(*arg)) ++arg;
  }
  const bool hasArg = *arg != 0;
  if (std::strcmp(sub, "switch") == 0) {
    if (!hasArg) return accept(Verb::kSelfSwitch);
    for (const char* p = arg; *p != 0; ++p) {
      if (isSpace(*p)) return refuse("self switch takes one word: self switch [radio-down]");
    }
    if (std::strcmp(arg, "radio-down") == 0) {
      Parsed p = accept(Verb::kSelfSwitch);
      p.command.radio_down = true;
      return p;
    }
    return refuse2("self switch: unknown word (self switch, self switch radio-down): ", arg);
  }
  Verb verb = Verb::kSelfStatus;
  if (std::strcmp(sub, "status") == 0) {
    verb = Verb::kSelfStatus;
  } else if (std::strcmp(sub, "receive") == 0) {
    verb = Verb::kSelfReceive;
  } else if (std::strcmp(sub, "confirm") == 0) {
    verb = Verb::kSelfConfirm;
  } else if (std::strcmp(sub, "reject") == 0) {
    verb = Verb::kSelfReject;
  } else {
    return refuse2("unknown self subcommand (status, receive, switch, confirm, reject): ", sub);
  }
  if (hasArg) return refuse3("self ", sub, " takes no argument");
  return accept(verb);
}

// `lte [up | down | time | id]` (task p3-01): the uplink. No slot - a modem is not a radio -
// and a bare `lte` reads and does nothing, which is the shape `rak watch` has for the same
// reason: looking at something must never be able to change it.
Parsed parseLte(char* rest) {
  if (*rest == 0) return accept(Verb::kLte);
  char* sub = rest;
  char* arg = sub;
  while (*arg != 0 && !isSpace(*arg)) ++arg;
  if (*arg != 0) {
    *arg++ = 0;
    while (*arg != 0 && isSpace(*arg)) ++arg;
  }
  const bool hasArg = *arg != 0;
  Verb verb = Verb::kLte;
  if (std::strcmp(sub, "up") == 0) {
    verb = Verb::kLteUp;
  } else if (std::strcmp(sub, "down") == 0) {
    verb = Verb::kLteDown;
  } else if (std::strcmp(sub, "time") == 0) {
    verb = Verb::kLteTime;
  } else if (std::strcmp(sub, "id") == 0) {
    verb = Verb::kLteId;
  } else {
    return refuse2("unknown lte subcommand (lte, up, down, time, id): ", sub);
  }
  if (hasArg) return refuse3("lte ", sub, " takes no argument");
  return accept(verb);
}

// `http get <url>` (task p3-02): one GET. The URL is the rest of the line, one word; the client
// is what refuses a scheme other than https, so that the refusal is tested where it lives.
Parsed parseHttp(char* rest) {
  if (*rest == 0) return refuse("http needs a subcommand: http get <url>");
  char* sub = rest;
  char* arg = sub;
  while (*arg != 0 && !isSpace(*arg)) ++arg;
  if (*arg != 0) {
    *arg++ = 0;
    while (*arg != 0 && isSpace(*arg)) ++arg;
  }
  if (std::strcmp(sub, "get") != 0) return refuse2("unknown http subcommand (get): ", sub);
  if (*arg == 0) return refuse("http get needs a URL: http get <url>");
  for (const char* p = arg; *p != 0; ++p) {
    if (isSpace(*p)) return refuse("http get takes one URL and nothing after it");
  }
  Parsed p = accept(Verb::kHttpGet);
  put(p.command.url, sizeof p.command.url, arg);
  return p;
}

// `fetch <base-url> [candidate|scanner]` (task p3-02): a release into the store. The default is
// the candidate slot; golden is never a target (it is reached by a promotion).
Parsed parseFetch(char* rest) {
  // No base URL at all: the `fetch.url` setting (task p3-03). The console is the one that knows
  // whether there is one, so an empty url here means "use the setting" and not a refusal.
  if (*rest == 0) {
    Parsed p = accept(Verb::kFetch);
    p.command.which = static_cast<std::uint8_t>(image::Which::kCandidate);
    return p;
  }
  // `fetch candidate` and `fetch scanner`: the setting's URL and a slot, with no base URL typed.
  if (std::strcmp(rest, "candidate") == 0) {
    Parsed p = accept(Verb::kFetch);
    p.command.which = static_cast<std::uint8_t>(image::Which::kCandidate);
    return p;
  }
  if (std::strcmp(rest, "scanner") == 0) {
    Parsed p = accept(Verb::kFetch);
    p.command.which = static_cast<std::uint8_t>(image::Which::kScanner);
    return p;
  }
  char* url = rest;
  char* which = url;
  while (*which != 0 && !isSpace(*which)) ++which;
  if (*which != 0) {
    *which++ = 0;
    while (*which != 0 && isSpace(*which)) ++which;
  }
  Parsed p = accept(Verb::kFetch);
  put(p.command.url, sizeof p.command.url, url);
  p.command.which = static_cast<std::uint8_t>(image::Which::kCandidate);
  if (*which == 0) return p;
  char* after = which;
  while (*after != 0 && !isSpace(*after)) ++after;
  if (*after != 0) return refuse("fetch takes a base URL and one word: fetch <base-url> [candidate|scanner]");
  if (std::strcmp(which, "candidate") == 0) return p;
  if (std::strcmp(which, "scanner") == 0) {
    p.command.which = static_cast<std::uint8_t>(image::Which::kScanner);
    return p;
  }
  if (std::strcmp(which, "golden") == 0) {
    return refuse("fetch: the golden image is reached by a promotion (rak update), not by a fetch");
  }
  return refuse2("fetch: not one of candidate, scanner: ", which);
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
  if (std::strcmp(verb, "trust") == 0) {
    if (!hasRest) return accept(Verb::kTrust);
    // `trust tls` (task p3-02): the roots on the wire, as `trust` is the release keys.
    if (std::strcmp(rest, "tls") == 0) return accept(Verb::kTrustTls);
    return refuse2("trust takes no argument, or 'tls': ", rest);
  }
  if (std::strcmp(verb, "http") == 0) return parseHttp(rest);
  if (std::strcmp(verb, "fetch") == 0) return parseFetch(rest);
  if (std::strcmp(verb, "image") == 0) {
    std::uint8_t slot = 0;
    Parsed p = parseImage(rest, &slot);
    if (p.ok) p.command.slot = slot;
    return p;
  }
  if (std::strcmp(verb, "self") == 0) return parseSelf(rest);
  if (std::strcmp(verb, "lte") == 0) return parseLte(rest);
  // The settings (task p3-03). The keys, their types and their ranges are
  // core/settings/keys.h's table; a refusal here is the table's own words.
  if (std::strcmp(verb, "get") == 0) {
    if (!hasRest) return accept(Verb::kGetAll);
    for (const char* p = rest; *p != 0; ++p) {
      if (isSpace(*p)) return refuse("get takes one key: get <key>");
    }
    const settings::KeyDef* key = settings::findKey(rest);
    if (key == nullptr) return refuse2("unknown key (try 'help'): ", rest);
    Parsed p = accept(Verb::kGet);
    p.command.key = key;
    return p;
  }
  if (std::strcmp(verb, "set") == 0) {
    if (!hasRest) return refuse("set needs a key and a value: set <key> <value>");
    char* name = rest;
    char* value = name;
    while (*value != 0 && !isSpace(*value)) ++value;
    if (*value != 0) {
      *value++ = 0;
      while (*value != 0 && isSpace(*value)) ++value;
    }
    const settings::KeyDef* key = settings::findKey(name);
    if (key == nullptr) return refuse2("unknown key (try 'help'): ", name);
    if (*value == 0) {
      return refuse3("set ", key->name, " needs a value (there is no way to unset one key: "
                     "'erase confirm' forgets them all)");
    }
    const std::size_t n = std::strlen(value);
    // Validated here, so that every refusal of every key has a parser test and the console
    // never sees a value the table would not have taken.
    Parsed p = accept(Verb::kSet);
    if (!settings::validate(*key, value, p.error, sizeof p.error)) {
      p.ok = false;
      p.command.verb = Verb::kEmpty;
      return p;
    }
    p.command.key = key;
    std::memcpy(p.command.text, value, n + 1);
    p.command.text_size = n;
    return p;
  }
  if (std::strcmp(verb, "erase") == 0) {
    if (!hasRest) {
      return refuse3("erase forgets every setting; to do it, type: erase ", kConfirmWord, "");
    }
    if (std::strcmp(rest, kConfirmWord) != 0) {
      return refuse3("erase: the word is '", kConfirmWord, "'");
    }
    return accept(Verb::kErase);
  }
  if (std::strcmp(verb, "dump") == 0) {
    if (hasRest) return refuse("dump takes no argument");
    return accept(Verb::kDump);
  }
  if (std::strcmp(verb, "journal") == 0) {
    if (hasRest) return refuse("journal takes no argument");
    return accept(Verb::kJournal);
  }
  if (std::strcmp(verb, "rak") == 0) {
    if (!hasRest) return refuse("rak needs a subcommand (try 'help')");
    // The slot (task p1-06): `rak @1 hello`. Optional, and the first word after `rak` rather
    // than the last of the line, so that every subcommand takes it the same way - `rak cli
    // <line>`, whose argument is the rest of the line verbatim, could not have it at the end.
    // Default @0: the only radio a unit has (spec/SYSTEM.md S-11). A number that is not a
    // position of any adapter is refused here; whether *this* adapter has that position is the
    // console's answer, from TargetSelect.
    std::uint8_t slot = 0;
    if (rest[0] == '@') {
      char* word = rest;
      char* after = word;
      while (*after != 0 && !isSpace(*after)) ++after;
      if (*after != 0) {
        *after++ = 0;
        while (*after != 0 && isSpace(*after)) ++after;
      }
      std::uint32_t number = 0;
      if (!parseSeconds(word + 1, number) || !board::isSlot(number)) {
        return refuse2("rak: not a slot (@0 or @1): ", word);
      }
      slot = static_cast<std::uint8_t>(number);
      rest = after;
    }
    Parsed p = parseRak(rest);
    if (p.ok) p.command.slot = slot;
    return p;
  }
  return refuse2("unknown command (try 'help'): ", verb);
}

}  // namespace console
}  // namespace core
