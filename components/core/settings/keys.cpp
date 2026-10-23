#include "core/settings/keys.h"

#include <cstdio>
#include <cstring>

#include "core/swd/wire.h"
#include "core/uplink/policy.h"

namespace core {
namespace settings {

// -- the table -----------------------------------------------------------------------------------
//
// A key is unset until a person sets it, and `erase confirm` is what makes it unset again: there
// is no `unset <key>`, because the one thing a half-provisioned unit must not do is look
// provisioned. `unset` below is what the unit does meanwhile, in the words `get` and `help` use.
constexpr KeyDef kKeys[kKeyCount] = {
    // The modem (task p3-01). The APN empty is "let the network choose", which is what a Danish
    // LTE-M SIM wants and what QuickSpot's own examples use; 63 is the longest APN 3GPP TS 23.003
    // §9.1 allows (the whole APN, labels and dots).
    {Key::kLteApn, "lte.apn", "lte_apn", Kind::kText, false, nullptr,
     "the network's default APN is used", 0, 0, 1, 63,
     "the APN to open the packet context with, at most 63 characters",
     "the next time the uplink is brought up ('lte up', or the next wake)"},
    // The SIM's PIN. A secret, and **never a default**: three refusals block a card and opening
    // it again needs the operator's PUK (core/uplink/policy.h, *The PIN*). Four to eight digits
    // is 3GPP TS 31.101 §5.4's range for a PIN.
    {Key::kLtePin, "lte.pin", "lte_pin", Kind::kDigits, true, nullptr,
     "no PIN is sent; a card that asks for one is reported sim-locked", 0, 0, 4, 8,
     "the SIM's PIN, 4 to 8 digits; never printed",
     "at once: a refusal remembered against the old PIN is forgotten, and the next uplink sends "
     "the new one"},
    {Key::kLtePsm, "lte.psm", "lte_psm", Kind::kOnOff, false, "off",
     "off: the modem's radio is put down between wakes", 0, 0, 0, 0,
     "on | off: ask the network for 3GPP power saving (PSM) instead",
     "the next time the uplink is brought up"},
    // The backend: meshmaster.dk, as the agent calls it (spec/SYSTEM.md §8, task p4-03). The
    // same host is the self-update trial's third condition - a verified TLS handshake with it
    // (core/update/uplink_check.h) - and with no agent built it is only that.
    {Key::kServerUrl, "server.url", "srv_url", Kind::kUrl, false, nullptr,
     "the agent makes no call, and a self-update's third condition does not apply", 0, 0, 9,
     static_cast<std::uint16_t>(kMaxUrlLen),
     "the backend: https://host[:port][/path], the scheme required, at most 128 characters",
     "at once: the host the agent's calls and a self-update's trial must reach"},
    {Key::kServerToken, "server.token", "srv_token", Kind::kText, true, nullptr,
     "the agent makes no call", 0, 0, 1, static_cast<std::uint16_t>(kMaxValueLen),
     "the backend's token, printable, at most 160 characters; never printed",
     "kept for the agent's calls ('agent probe', and every wake's heartbeat); nothing else "
     "reads it"},
    {Key::kFetchUrl, "fetch.url", "fetch_url", Kind::kUrl, false, nullptr,
     "`fetch` needs a base URL on the line", 0, 0, 9, static_cast<std::uint16_t>(kMaxUrlLen),
     "a release base for `fetch` with no argument: https://host[:port][/path]",
     "the next `fetch` typed with no base URL"},
    // The duty cycle (task p3-04). Five minutes is the shortest interval worth a wake; 10080 is
    // a week. `auto` is the battery deciding, which is the default and what a unit in the field
    // runs on.
    {Key::kWakeInterval, "wake.interval", "wake_interval", Kind::kMinutes, false, "auto",
     "auto: the battery chooses the interval (task p3-04)", 5, 10080, 0, 0,
     "auto, or the minutes between wakes, 5 to 10080",
     "the next time the interval is chosen, at the end of a wake (task p3-04)"},
    {Key::kWakeRadio, "wake.radio", "wake_radio", Kind::kOnOff, false, "on", nullptr, 0, 0, 0, 0,
     "on | off: may a start bit from the radio wake the Walter",
     "the next time the Walter goes to sleep (task p3-04)"},
    // The debug port (phase 1, task 03). 125 kHz and not the 100 kHz the task names: the
    // nRF52840 Product Specification gives f_SWDCLK a minimum of 0.125 MHz ("Debug and trace >
    // Electrical specification > Control access port"), which is what core/swd/wire.h's default
    // half-period of 4 us is, and 100 kHz is below the chip's own specification. The range keeps
    // the task's lower end, because a long cable on a bench is a reason to go slower than the
    // chip promises to answer at; the help line says so.
    {Key::kSwdKhz, "swd.khz", "swd_khz", Kind::kInt, false, "125", nullptr, 25, 1000, 0, 0,
     "the SWD clock in kHz, 25 to 1000; below 125 is slower than the nRF52840 is specified for",
     "at once: every SWD command and every flashing from here on"},
    {Key::kLogLevel, "log.level", "log_level", Kind::kLevel, false, "info", nullptr, 0, 0, 0, 0,
     "error | warn | info | debug: what is printed when nobody asked",
     "at once: a reply is always printed, the lines nobody asked for are not"},
    {Key::kUnitName, "unit.name", "unit_name", Kind::kText, false, nullptr,
     "the unit has no label", 0, 0, 1, 32,
     "a label for a person, at most 32 characters; it names this unit in `status`",
     "at once: what `status` and the boot line call this unit"},
};

namespace {
// strlen at compile time, for the one thing the key-value store bounds: the name a value is
// stored under (interfaces/key_value.h - NVS's NVS_KEY_NAME_MAX_SIZE).
constexpr std::size_t literalLength(const char* s) {
  std::size_t n = 0;
  while (s[n] != 0) ++n;
  return n;
}
constexpr bool storedNamesFit() {
  for (std::size_t i = 0; i < kKeyCount; ++i) {
    if (literalLength(kKeys[i].nvs) == 0) return false;
    if (literalLength(kKeys[i].nvs) > interfaces::kKeyNameMax) return false;
  }
  return true;
}
}  // namespace
static_assert(storedNamesFit(), "a key's stored name is 1 to interfaces::kKeyNameMax characters");

// Every row is where its own id says it is, so def() is an index and not a search.
static_assert(kKeys[static_cast<std::size_t>(Key::kLteApn)].id == Key::kLteApn, "the table is indexed by Key");
static_assert(kKeys[static_cast<std::size_t>(Key::kUnitName)].id == Key::kUnitName, "the table is indexed by Key");

// The one number this table and core/swd/wire.h both hold: the fallback above is the wire's own
// default, said in kHz. A change to either without the other is a failed build.
static_assert(swd::kDefaultHalfPeriodUs == 4, "swd.khz's fallback of 125 kHz is the wire's 4 us");

// And the one `lte.psm` and core/uplink/policy.h both hold: the fallback is the policy's own
// build constant, so a unit with no setting behaves exactly as task p3-01 left it.
static_assert(uplink::kPowerSaving == interfaces::PowerSavingMode::kOff,
              "lte.psm's fallback of off is the policy's own default");

const char* describe(Kind kind) {
  switch (kind) {
    case Kind::kText: return "text";
    case Kind::kDigits: return "digits";
    case Kind::kUrl: return "an https URL";
    case Kind::kInt: return "a whole number";
    case Kind::kMinutes: return "auto or minutes";
    case Kind::kOnOff: return "on | off";
    case Kind::kLevel: return "error | warn | info | debug";
  }
  return "?";
}

const char* name(Level level) {
  switch (level) {
    case Level::kError: return "error";
    case Level::kWarn: return "warn";
    case Level::kInfo: return "info";
    case Level::kDebug: return "debug";
  }
  return "?";
}

bool levelFromName(const char* text, Level* out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "error") == 0) { *out = Level::kError; return true; }
  if (std::strcmp(text, "warn") == 0) { *out = Level::kWarn; return true; }
  if (std::strcmp(text, "info") == 0) { *out = Level::kInfo; return true; }
  if (std::strcmp(text, "debug") == 0) { *out = Level::kDebug; return true; }
  return false;
}

const KeyDef& def(Key key) {
  const std::size_t i = static_cast<std::size_t>(key);
  return kKeys[i < kKeyCount ? i : 0];
}

const KeyDef* findKey(const char* name) {
  if (name == nullptr) return nullptr;
  for (const KeyDef& k : kKeys) {
    if (std::strcmp(k.name, name) == 0) return &k;
  }
  return nullptr;
}

namespace {

void say(char* err, std::size_t cap, const char* text) {
  if (err == nullptr || cap == 0) return;
  std::snprintf(err, cap, "%s", text);
}

bool printableAscii(const char* s) {
  for (; *s != 0; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

// A whole number, optionally signed, inside a 32-bit int. No strtol: errno and locale are not
// worth bringing into core for eleven keys.
bool parseInt(const char* s, std::int32_t* out) {
  if (s == nullptr || *s == 0) return false;
  bool negative = false;
  const char* p = s;
  if (*p == '-') { negative = true; ++p; }
  if (*p == 0) return false;
  std::int64_t value = 0;
  std::size_t digits = 0;
  for (; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return false;
    if (++digits > 10) return false;
    value = value * 10 + (*p - '0');
    if (value > 2147483648ll) return false;
  }
  if (negative) value = -value;
  if (value > 2147483647ll || value < -2147483648ll) return false;
  *out = static_cast<std::int32_t>(value);
  return true;
}

// What a host name and a path may hold here. The client's own parser is stricter and runs on the
// value before anything is sent (core/net/url.h); this is the keyboard-level check, so that a
// refusal names the character a person typed.
bool urlChar(char c) {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  return c == '.' || c == '-' || c == ':' || c == '/' || c == '_' || c == '~' || c == '%';
}

}  // namespace

bool validate(const KeyDef& key, const char* value, char* err, std::size_t cap) {
  if (value == nullptr) value = "";
  const std::size_t len = std::strlen(value);
  char msg[128];

  if (len > kMaxValueLen) {
    std::snprintf(msg, sizeof msg, "%s: longer than %u characters", key.name,
                  static_cast<unsigned>(kMaxValueLen));
    say(err, cap, msg);
    return false;
  }

  switch (key.kind) {
    case Kind::kText:
      if (len < key.min_len || len > key.max_len) {
        std::snprintf(msg, sizeof msg, "%s: %u..%u characters", key.name,
                      static_cast<unsigned>(key.min_len), static_cast<unsigned>(key.max_len));
        say(err, cap, msg);
        return false;
      }
      if (!printableAscii(value)) {
        std::snprintf(msg, sizeof msg, "%s: printable ASCII only", key.name);
        say(err, cap, msg);
        return false;
      }
      return true;

    case Kind::kDigits:
      if (len < key.min_len || len > key.max_len) {
        std::snprintf(msg, sizeof msg, "%s: %u..%u digits", key.name,
                      static_cast<unsigned>(key.min_len), static_cast<unsigned>(key.max_len));
        say(err, cap, msg);
        return false;
      }
      for (const char* p = value; *p != 0; ++p) {
        if (*p < '0' || *p > '9') {
          // The value itself is not quoted back: this key is a secret and the refusal is a line
          // the port prints.
          std::snprintf(msg, sizeof msg, "%s: digits only", key.name);
          say(err, cap, msg);
          return false;
        }
      }
      return true;

    case Kind::kUrl: {
      static const char kScheme[] = "https://";
      constexpr std::size_t kSchemeLen = sizeof kScheme - 1;
      if (std::strncmp(value, kScheme, kSchemeLen) != 0) {
        std::snprintf(msg, sizeof msg, "%s: the scheme must be https:// (S-12: TLS is verified)",
                      key.name);
        say(err, cap, msg);
        return false;
      }
      if (len < key.min_len || len > key.max_len || value[kSchemeLen] == 0 ||
          value[kSchemeLen] == '/') {
        std::snprintf(msg, sizeof msg, "%s: https://host[:port][/path], at most %u characters",
                      key.name, static_cast<unsigned>(key.max_len));
        say(err, cap, msg);
        return false;
      }
      for (const char* p = value; *p != 0; ++p) {
        if (!urlChar(*p)) {
          std::snprintf(msg, sizeof msg, "%s: '%c' is not allowed in a URL here", key.name, *p);
          say(err, cap, msg);
          return false;
        }
      }
      return true;
    }

    case Kind::kInt: {
      std::int32_t v = 0;
      if (!parseInt(value, &v)) {
        std::snprintf(msg, sizeof msg, "%s: not a whole number: %s", key.name, value);
        say(err, cap, msg);
        return false;
      }
      if (v < key.min || v > key.max) {
        std::snprintf(msg, sizeof msg, "%s: out of range, %ld..%ld", key.name,
                      static_cast<long>(key.min), static_cast<long>(key.max));
        say(err, cap, msg);
        return false;
      }
      return true;
    }

    case Kind::kMinutes: {
      if (std::strcmp(value, "auto") == 0) return true;
      std::int32_t v = 0;
      if (!parseInt(value, &v)) {
        std::snprintf(msg, sizeof msg, "%s: auto, or a whole number of minutes: %s", key.name,
                      value);
        say(err, cap, msg);
        return false;
      }
      if (v < key.min || v > key.max) {
        std::snprintf(msg, sizeof msg, "%s: auto, or %ld..%ld minutes", key.name,
                      static_cast<long>(key.min), static_cast<long>(key.max));
        say(err, cap, msg);
        return false;
      }
      return true;
    }

    case Kind::kOnOff:
      if (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0) return true;
      std::snprintf(msg, sizeof msg, "%s: on | off", key.name);
      say(err, cap, msg);
      return false;

    case Kind::kLevel: {
      Level level = Level::kInfo;
      if (levelFromName(value, &level)) return true;
      std::snprintf(msg, sizeof msg, "%s: error | warn | info | debug", key.name);
      say(err, cap, msg);
      return false;
    }
  }
  say(err, cap, "unknown value kind");
  return false;
}

void describeValue(const KeyDef& key, const char* stored, char* out, std::size_t cap) {
  const bool unset = stored == nullptr || stored[0] == 0;
  if (key.secret) {
    std::snprintf(out, cap, "%s %s", key.name, unset ? "unset" : "set");
    return;
  }
  if (unset) {
    if (key.fallback != nullptr) {
      std::snprintf(out, cap, "%s unset (%s)", key.name, key.fallback);
    } else {
      std::snprintf(out, cap, "%s unset", key.name);
    }
    return;
  }
  std::snprintf(out, cap, "%s %s", key.name, stored);
}

std::int32_t number(const char* value) {
  std::int32_t v = 0;
  if (parseInt(value, &v)) return v;
  return 0;
}

std::uint32_t minutes(const char* value) {
  if (value == nullptr || value[0] == 0) return 0;
  if (std::strcmp(value, "auto") == 0) return 0;
  std::int32_t v = 0;
  if (!parseInt(value, &v) || v < 0) return 0;
  return static_cast<std::uint32_t>(v);
}

std::uint32_t halfPeriodUsForKhz(std::int32_t khz) {
  if (khz <= 0) return swd::kDefaultHalfPeriodUs;
  // A half-period is 500/kHz microseconds; rounded to the nearest whole one, and never zero -
  // the wire's own bounds (core/swd/wire.h) hold it the rest of the way.
  const std::uint32_t k = static_cast<std::uint32_t>(khz);
  std::uint32_t us = (500u + k / 2u) / k;
  if (us < swd::kMinHalfPeriodUs) us = swd::kMinHalfPeriodUs;
  if (us > swd::kMaxHalfPeriodUs) us = swd::kMaxHalfPeriodUs;
  return us;
}

}  // namespace settings
}  // namespace core
