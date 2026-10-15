// The settings a Walter takes over its USB console, as one table (task p3-03).
//
// One row per key: the name a person types, the name it is stored under, its type, its range or
// pattern, its fallback while it is unset, whether it is a secret, one line of what it is and one
// line of what happens when it changes. The table is the only place any of that is written down:
// the parser validates against it (`core/console/parser.cpp`), the store reads and writes by it
// (`settings.h`), `get` prints from it, `help` and `docs/console.md` list it, and a host test
// holds the document to it. After the agent firmware's `src/settings/Keys.h` at
// 51faafb427cba41b0de9983736b6621bc155f829, which is the console a person coming to this one has
// already used.
//
// Host-portable: no ESP-IDF, no NVS. The flash side is `interfaces/key_value.h`.
//
// **What is not here.** The thresholds that protect the hardware are build constants and stay
// constants - the battery floors for a power cycle and a flash, the supervisor's daily caps, the
// proving period, the trial window, a wake's budget. A typo at a console must not be able to
// remove a safety margin (`tasks/phase-3/03-settings-and-console.md`); `tasks/phase-3/reports/
// 03.md` lists what moved here and what deliberately did not.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/key_value.h"

namespace core {
namespace settings {

// What a value may be.
enum class Kind : std::uint8_t {
  kText,     // printable ASCII, min_len..max_len characters
  kDigits,   // decimal digits only, min_len..max_len of them
  kUrl,      // https://host[:port][/path], at most max_len characters
  kInt,      // a whole number in [min, max]
  kMinutes,  // the word `auto`, or a whole number of minutes in [min, max]
  kOnOff,    // `on` or `off`
  kLevel,    // `error`, `warn`, `info` or `debug`
};

const char* describe(Kind kind);

// How much this firmware says. The default is `info`; `error` and `warn` silence the lines the
// console prints when nobody asked - a resumed update's transitions, a trial's steps, the boot's
// own lines - and never a reply or a refusal, which are always printed. Nothing is at `debug`
// yet; the key exists because the agent firmware's console has it and because the first thing
// phase 3's task 05 will want is somewhere to put a wake's detail.
enum class Level : std::uint8_t { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

const char* name(Level level);
bool levelFromName(const char* text, Level* out);

// Every key, in the order `get` and `help` print them.
enum class Key : std::uint8_t {
  kLteApn = 0,
  kLtePin,
  kLtePsm,
  kServerUrl,
  kServerToken,
  kFetchUrl,
  kWakeInterval,
  kWakeRadio,
  kSwdKhz,
  kLogLevel,
  kUnitName,
  kCount,
};

constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::kCount);

// The longest value any key accepts: `server.token`'s 160 characters. A `set server.token
// <160 characters>` is 177 characters, inside the console's 200 (core/console/parser.h).
constexpr std::size_t kMaxValueLen = 160;
static_assert(kMaxValueLen <= interfaces::kKeyValueMax, "a value has to fit the key-value store");

// A URL's bound, the task's: `server.url` and `fetch.url`. Shorter than the client's own
// kMaxUrlLength (1536, which a GitHub redirect needs) because this is a URL a person types.
constexpr std::size_t kMaxUrlLen = 128;

struct KeyDef {
  Key id;
  const char* name;       // what a person types: `lte.apn`
  const char* nvs;        // what it is stored under, at most interfaces::kKeyNameMax characters
  Kind kind;
  bool secret;            // never printed: `get` says set or unset, `set` echoes asterisks
  const char* fallback;   // what is used while it is unset, as text; nullptr: there is none
  const char* unset;      // what the unit does while it is unset, in words
  std::int32_t min;       // kInt and kMinutes
  std::int32_t max;
  std::uint16_t min_len;  // kText, kDigits, kUrl
  std::uint16_t max_len;
  const char* help;       // one line: what the key is
  const char* effect;     // one line: what happens when it changes
};

extern const KeyDef kKeys[kKeyCount];

const KeyDef& def(Key key);

// The key called `name`, or nullptr. Exact, case-sensitive.
const KeyDef* findKey(const char* name);

// Is `value` allowed for `key`. On a refusal `err` (at most `cap` bytes) is one line saying why,
// naming the key and the rule it broke. Nothing is stored by this function.
bool validate(const KeyDef& key, const char* value, char* err, std::size_t cap);

// What `get <key>` prints: `<name> <value>`; for a secret `<name> set` or `<name> unset`; for an
// unset key with a fallback `<name> unset (<fallback>)`. `stored` is nullptr or "" when unset.
void describeValue(const KeyDef& key, const char* stored, char* out, std::size_t cap);

// The whole number a kInt key's text is (it has passed validate()).
std::int32_t number(const char* value);

// The minutes a kMinutes key's text is: 0 for `auto`.
std::uint32_t minutes(const char* value);

// The SWD clock in kHz as a half-period in microseconds, rounded to the nearest whole one and at
// least one: what core/swd/wire.h takes. 125 kHz is 4 us, the wire's own default.
std::uint32_t halfPeriodUsForKhz(std::int32_t khz);

}  // namespace settings
}  // namespace core
