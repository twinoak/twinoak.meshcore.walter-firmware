// The table of settings (task p3-03, core/settings/keys.h): every key is there, every key's type
// and range refuse what they say they refuse, every refusal names the key, and what `get` prints
// for a set, an unset and a secret key.
//
// The table is the only place a key's rules are written down, so this file is where they are
// held: the parser validates through validate() and the store reads and writes by the same rows.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/settings/keys.h"
#include "core/swd/wire.h"

using core::settings::def;
using core::settings::findKey;
using core::settings::Key;
using core::settings::KeyDef;
using core::settings::kKeyCount;
using core::settings::kKeys;
using core::settings::kMaxValueLen;
using core::settings::Kind;
using core::settings::Level;

namespace {

// Is `value` allowed for the key called `name`, and what the refusal says.
bool allows(const char* name, const char* value) {
  const KeyDef* key = findKey(name);
  REQUIRE_MESSAGE(key != nullptr, std::string(name));
  char err[128] = {};
  const bool ok = core::settings::validate(*key, value, err, sizeof err);
  // A refusal always says something, and it always names the key it is about.
  if (!ok) {
    CHECK_MESSAGE(err[0] != 0, std::string(name));
    CHECK_MESSAGE(std::string(err).find(name) == 0, std::string(err));
  }
  return ok;
}

std::string refusalOf(const char* name, const char* value) {
  const KeyDef* key = findKey(name);
  REQUIRE_MESSAGE(key != nullptr, std::string(name));
  char err[128] = {};
  const bool ok = core::settings::validate(*key, value, err, sizeof err);
  REQUIRE_MESSAGE(!ok, std::string(name));
  return err;
}

std::string describe(const char* name, const char* stored) {
  const KeyDef* key = findKey(name);
  REQUIRE_MESSAGE(key != nullptr, std::string(name));
  char out[kMaxValueLen + 64] = {};
  core::settings::describeValue(*key, stored, out, sizeof out);
  return out;
}

}  // namespace

TEST_CASE("the table has the eleven keys of the task, in order, each with help and an effect") {
  const char* expected[] = {"lte.apn",       "lte.pin",    "lte.psm", "server.url",
                            "server.token",  "fetch.url",  "wake.interval", "wake.radio",
                            "swd.khz",       "log.level",  "unit.name"};
  REQUIRE(kKeyCount == sizeof expected / sizeof expected[0]);
  for (std::size_t i = 0; i < kKeyCount; ++i) {
    CHECK_MESSAGE(std::string(kKeys[i].name) == expected[i], std::string(expected[i]));
    // Every row: a name a person types, a name it is stored under, one line of help and one line
    // of what happens when it changes.
    CHECK_MESSAGE(kKeys[i].name[0] != 0, std::string(expected[i]));
    CHECK_MESSAGE(kKeys[i].nvs[0] != 0, std::string(expected[i]));
    CHECK_MESSAGE(kKeys[i].help != nullptr, std::string(expected[i]));
    CHECK_MESSAGE(kKeys[i].help[0] != 0, std::string(expected[i]));
    CHECK_MESSAGE(kKeys[i].effect != nullptr, std::string(expected[i]));
    CHECK_MESSAGE(kKeys[i].effect[0] != 0, std::string(expected[i]));
    // And it is where its own id says it is, which is what def() relies on.
    CHECK_MESSAGE(&def(kKeys[i].id) == &kKeys[i], std::string(expected[i]));
    // The stored name fits NVS's limit, and no two keys share one.
    CHECK_MESSAGE(std::strlen(kKeys[i].nvs) <= interfaces::kKeyNameMax, std::string(kKeys[i].nvs));
    for (std::size_t j = i + 1; j < kKeyCount; ++j) {
      CHECK_MESSAGE(std::strcmp(kKeys[i].nvs, kKeys[j].nvs) != 0, std::string(kKeys[i].nvs));
      CHECK_MESSAGE(std::strcmp(kKeys[i].name, kKeys[j].name) != 0, std::string(kKeys[i].name));
    }
  }
  CHECK(findKey("lte.apn") != nullptr);
  CHECK(findKey("LTE.APN") == nullptr);  // exact, case-sensitive
  CHECK(findKey("lte") == nullptr);
  CHECK(findKey("") == nullptr);
  CHECK(findKey(nullptr) == nullptr);
}

TEST_CASE("exactly two keys are secrets, and they are the SIM's PIN and the server's token") {
  std::size_t secrets = 0;
  for (const KeyDef& key : kKeys) {
    if (key.secret) ++secrets;
  }
  CHECK(secrets == 2);
  CHECK(def(Key::kLtePin).secret);
  CHECK(def(Key::kServerToken).secret);
  // And neither has a fallback: a default secret is a secret everybody knows (S-13).
  CHECK(def(Key::kLtePin).fallback == nullptr);
  CHECK(def(Key::kServerToken).fallback == nullptr);
}

TEST_CASE("lte.apn: 1 to 63 printable characters") {
  CHECK(allows("lte.apn", "internet"));
  CHECK(allows("lte.apn", "lpwa.telia.iot"));
  CHECK(allows("lte.apn", std::string(63, 'a').c_str()));
  CHECK_FALSE(allows("lte.apn", std::string(64, 'a').c_str()));
  CHECK_FALSE(allows("lte.apn", ""));
  CHECK(refusalOf("lte.apn", "") == "lte.apn: 1..63 characters");
  // A value with a space is a value: the console takes the rest of the line.
  CHECK(allows("lte.apn", "an apn with spaces"));
}

TEST_CASE("lte.pin: 4 to 8 digits, and the refusal never quotes the value") {
  CHECK(allows("lte.pin", "1234"));
  CHECK(allows("lte.pin", "12345678"));
  CHECK(allows("lte.pin", "0000"));
  CHECK_FALSE(allows("lte.pin", "123"));
  CHECK_FALSE(allows("lte.pin", "123456789"));
  CHECK_FALSE(allows("lte.pin", "12a4"));
  CHECK_FALSE(allows("lte.pin", "1 34"));
  CHECK(refusalOf("lte.pin", "123") == "lte.pin: 4..8 digits");
  // The one thing this refusal must not do: print the PIN that was typed.
  CHECK(refusalOf("lte.pin", "12a4") == "lte.pin: digits only");
  CHECK(refusalOf("lte.pin", "12a4").find("12a4") == std::string::npos);
}

TEST_CASE("the on/off and level keys take their words and nothing else") {
  CHECK(allows("lte.psm", "on"));
  CHECK(allows("lte.psm", "off"));
  CHECK_FALSE(allows("lte.psm", "ON"));
  CHECK_FALSE(allows("lte.psm", "yes"));
  CHECK_FALSE(allows("lte.psm", "1"));
  CHECK(refusalOf("lte.psm", "yes") == "lte.psm: on | off");
  CHECK(allows("wake.radio", "on"));
  CHECK(allows("wake.radio", "off"));
  for (const char* level : {"error", "warn", "info", "debug"}) {
    CHECK_MESSAGE(allows("log.level", level), std::string(level));
    Level parsed = Level::kInfo;
    CHECK(core::settings::levelFromName(level, &parsed));
    CHECK(std::string(core::settings::name(parsed)) == level);
  }
  CHECK_FALSE(allows("log.level", "trace"));
  CHECK_FALSE(allows("log.level", "INFO"));
  CHECK(refusalOf("log.level", "trace") == "log.level: error | warn | info | debug");
}

TEST_CASE("a URL key wants https:// and a host, and at most 128 characters") {
  CHECK(allows("server.url", "https://meshmaster.dk"));
  CHECK(allows("server.url", "https://staging.meshmaster.dk:8443/agent/v1"));
  CHECK(allows("fetch.url", "https://github.com/twinoak/x/releases/download/v1.0.0"));
  CHECK_FALSE(allows("server.url", "http://meshmaster.dk"));
  CHECK(refusalOf("server.url", "http://meshmaster.dk") ==
        "server.url: the scheme must be https:// (S-12: TLS is verified)");
  CHECK_FALSE(allows("server.url", "meshmaster.dk"));
  CHECK_FALSE(allows("server.url", "ftp://meshmaster.dk"));
  CHECK_FALSE(allows("server.url", "https://"));
  CHECK_FALSE(allows("server.url", "https:///path"));
  CHECK(refusalOf("server.url", "https://") ==
        "server.url: https://host[:port][/path], at most 128 characters");
  CHECK_FALSE(allows("server.url", ("https://" + std::string(130, 'a')).c_str()));
  // A space, a quote and a backslash are not URL characters here, and the refusal says which.
  CHECK_FALSE(allows("server.url", "https://a b.example"));
  CHECK(refusalOf("server.url", "https://a b.example") ==
        "server.url: ' ' is not allowed in a URL here");
}

TEST_CASE("server.token: printable, at most 160 characters, and never quoted back") {
  CHECK(allows("server.token", "mma_abcdefghijklmnop"));
  CHECK(allows("server.token", std::string(160, 'x').c_str()));
  CHECK_FALSE(allows("server.token", std::string(161, 'x').c_str()));
  CHECK(refusalOf("server.token", std::string(161, 'x').c_str()) ==
        "server.token: longer than 160 characters");
  CHECK(refusalOf("server.token", std::string(161, 'x').c_str()).find("xxxx") == std::string::npos);
  // The table's bound is the parser's buffer, so a value that validates always fits.
  CHECK(def(Key::kServerToken).max_len == kMaxValueLen);
}

TEST_CASE("wake.interval: the word auto, or 5 to 10080 minutes") {
  CHECK(allows("wake.interval", "auto"));
  CHECK(allows("wake.interval", "5"));
  CHECK(allows("wake.interval", "60"));
  CHECK(allows("wake.interval", "10080"));
  CHECK_FALSE(allows("wake.interval", "4"));
  CHECK_FALSE(allows("wake.interval", "10081"));
  CHECK_FALSE(allows("wake.interval", "0"));
  CHECK_FALSE(allows("wake.interval", "-5"));
  CHECK_FALSE(allows("wake.interval", "AUTO"));
  CHECK_FALSE(allows("wake.interval", "ten"));
  CHECK(refusalOf("wake.interval", "4") == "wake.interval: auto, or 5..10080 minutes");
  CHECK(refusalOf("wake.interval", "ten") ==
        "wake.interval: auto, or a whole number of minutes: ten");
  // `auto` is zero minutes: the battery decides (task p3-04).
  CHECK(core::settings::minutes("auto") == 0);
  CHECK(core::settings::minutes("") == 0);
  CHECK(core::settings::minutes("90") == 90);
}

TEST_CASE("swd.khz: 25 to 1000, and the kHz turned into the wire's half-period") {
  CHECK(allows("swd.khz", "25"));
  CHECK(allows("swd.khz", "125"));
  CHECK(allows("swd.khz", "1000"));
  CHECK_FALSE(allows("swd.khz", "24"));
  CHECK_FALSE(allows("swd.khz", "1001"));
  CHECK_FALSE(allows("swd.khz", "125.5"));
  CHECK_FALSE(allows("swd.khz", "fast"));
  CHECK(refusalOf("swd.khz", "1001") == "swd.khz: out of range, 25..1000");
  CHECK(refusalOf("swd.khz", "fast") == "swd.khz: not a whole number: fast");
  // The fallback is the wire's own default said in kHz, and the arithmetic agrees both ways.
  CHECK(std::string(def(Key::kSwdKhz).fallback) == "125");
  CHECK(core::settings::halfPeriodUsForKhz(125) == core::swd::kDefaultHalfPeriodUs);
  CHECK(core::settings::halfPeriodUsForKhz(100) == 5);
  CHECK(core::settings::halfPeriodUsForKhz(25) == 20);
  // 1000 kHz is half a microsecond, and the wire's floor is one: the setting cannot ask the
  // bit-banged wire to go faster than it can.
  CHECK(core::settings::halfPeriodUsForKhz(1000) == core::swd::kMinHalfPeriodUs);
  CHECK(core::settings::halfPeriodUsForKhz(0) == core::swd::kDefaultHalfPeriodUs);
}

TEST_CASE("unit.name: 1 to 32 printable characters") {
  CHECK(allows("unit.name", "Window at Grenaa"));
  CHECK(allows("unit.name", std::string(32, 'n').c_str()));
  CHECK_FALSE(allows("unit.name", std::string(33, 'n').c_str()));
  CHECK_FALSE(allows("unit.name", ""));
}

TEST_CASE("what get prints: the value, the fallback in brackets, or set/unset for a secret") {
  CHECK(describe("lte.apn", "internet") == "lte.apn internet");
  CHECK(describe("lte.apn", "") == "lte.apn unset");          // no fallback: nothing in brackets
  CHECK(describe("lte.apn", nullptr) == "lte.apn unset");
  CHECK(describe("log.level", "") == "log.level unset (info)");
  CHECK(describe("wake.interval", "") == "wake.interval unset (auto)");
  CHECK(describe("swd.khz", "") == "swd.khz unset (125)");
  CHECK(describe("swd.khz", "250") == "swd.khz 250");
  // The two secrets: set or unset, and never the value, whatever the value is.
  CHECK(describe("lte.pin", "1234") == "lte.pin set");
  CHECK(describe("lte.pin", "") == "lte.pin unset");
  CHECK(describe("server.token", "mma_secret") == "server.token set");
  CHECK(describe("server.token", "") == "server.token unset");
}

TEST_CASE("a kind has a name, so that a document generated from the table reads") {
  for (const Kind kind : {Kind::kText, Kind::kDigits, Kind::kUrl, Kind::kInt, Kind::kMinutes,
                          Kind::kOnOff, Kind::kLevel}) {
    CHECK(std::string(core::settings::describe(kind)) != "?");
  }
}
