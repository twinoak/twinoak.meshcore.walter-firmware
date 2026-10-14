// The settings store (task p3-03, core/settings/settings.h) over a map: a value stored and read
// back, a fallback while a key is unset, the typed getters, the PIN's epoch, `erase confirm`
// forgetting the keys of the table and nothing else, and - the one that matters at four in the
// morning - a store that is empty, corrupt, read-only or not there at all.
#include <doctest.h>

#include <string>

#include "core/settings/settings.h"
#include "core/swd/wire.h"
#include "fakes/sim_key_value.h"

using core::settings::def;
using core::settings::Key;
using core::settings::kKeyCount;
using core::settings::kKeys;
using core::settings::Level;
using core::settings::Settings;

namespace {

// `set` through the table's row for `name`, with the refusal if there is one.
bool put(Settings& s, const char* name, const char* value, std::string* why = nullptr) {
  const core::settings::KeyDef* key = core::settings::findKey(name);
  REQUIRE_MESSAGE(key != nullptr, std::string(name));
  char err[128] = {};
  const bool ok = s.put(*key, value, err, sizeof err);
  if (why != nullptr) *why = err;
  return ok;
}

}  // namespace

TEST_CASE("a value is stored, read back, and kept across a reboot") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  CHECK(settings.begun());
  CHECK(settings.storeReady());
  CHECK(settings.setCount() == 0);

  CHECK(put(settings, "lte.apn", "lpwa.telia.iot"));
  CHECK(std::string(settings.apn()) == "lpwa.telia.iot");
  CHECK(settings.isSet(Key::kLteApn));
  CHECK(settings.setCount() == 1);
  // It is in the store under the name the table gives, not under the name a person types.
  CHECK(kv.raw("lte_apn") == "lpwa.telia.iot");
  CHECK(!kv.has("lte.apn"));

  // A reboot: the same store, a new Settings over it.
  Settings again(kv);
  again.begin();
  CHECK(std::string(again.apn()) == "lpwa.telia.iot");
  CHECK(again.setCount() == 1);
}

TEST_CASE("an unset key falls back, and the fallbacks are what the firmware did before settings") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  // The ones with a fallback.
  CHECK(std::string(settings.value(Key::kLogLevel)) == "info");
  CHECK(settings.level() == Level::kInfo);
  CHECK(std::string(settings.value(Key::kWakeInterval)) == "auto");
  CHECK(settings.wakeIntervalMinutes() == 0);  // auto: the battery decides
  CHECK(settings.wakeOnRadio());
  CHECK_FALSE(settings.psm());  // off, as task p3-01's build constant was
  CHECK(settings.swdKhz() == 125);
  CHECK(settings.swdHalfPeriodUs() == core::swd::kDefaultHalfPeriodUs);
  // The ones without: empty, and empty is what every reader of them treats as "not configured".
  CHECK(std::string(settings.apn()).empty());
  CHECK(std::string(settings.pin()).empty());
  CHECK(std::string(settings.serverUrl()).empty());
  CHECK(std::string(settings.serverHost()).empty());
  CHECK(std::string(settings.serverToken()).empty());
  CHECK(std::string(settings.fetchUrl()).empty());
  CHECK(std::string(settings.unitName()).empty());
  CHECK(settings.serverPort() == 443);
}

TEST_CASE("every key: stored, read back, and the typed getter that reads it") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  CHECK(put(settings, "lte.apn", "internet"));
  CHECK(put(settings, "lte.pin", "4321"));
  CHECK(put(settings, "lte.psm", "on"));
  CHECK(put(settings, "server.url", "https://staging.meshmaster.dk:8443/agent/v1"));
  CHECK(put(settings, "server.token", "mma_token_value"));
  CHECK(put(settings, "fetch.url", "https://r.example/v1"));
  CHECK(put(settings, "wake.interval", "180"));
  CHECK(put(settings, "wake.radio", "off"));
  CHECK(put(settings, "swd.khz", "250"));
  CHECK(put(settings, "log.level", "debug"));
  CHECK(put(settings, "unit.name", "Window at Grenaa"));
  CHECK(settings.setCount() == kKeyCount);

  CHECK(std::string(settings.apn()) == "internet");
  CHECK(std::string(settings.pin()) == "4321");
  CHECK(settings.psm());
  CHECK(std::string(settings.serverUrl()) == "https://staging.meshmaster.dk:8443/agent/v1");
  // The URL is taken apart by the client's own parser, so a trial knows which host and port.
  CHECK(std::string(settings.serverHost()) == "staging.meshmaster.dk");
  CHECK(settings.serverPort() == 8443);
  CHECK(std::string(settings.serverToken()) == "mma_token_value");
  CHECK(std::string(settings.fetchUrl()) == "https://r.example/v1");
  CHECK(settings.wakeIntervalMinutes() == 180);
  CHECK_FALSE(settings.wakeOnRadio());
  CHECK(settings.swdKhz() == 250);
  CHECK(settings.swdHalfPeriodUs() == 2);
  CHECK(settings.level() == Level::kDebug);
  CHECK(std::string(settings.unitName()) == "Window at Grenaa");

  // The pointers are stable: what the uplink policy and the trial keep is a pointer into this
  // store, and a second `set` of another key must not move it.
  const char* apn_before = settings.apn();
  CHECK(put(settings, "unit.name", "Another name"));
  CHECK(settings.apn() == apn_before);
  CHECK(std::string(settings.apn()) == "internet");
}

TEST_CASE("server.url without a port is 443, and one the client's parser refuses leaves no host") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  CHECK(put(settings, "server.url", "https://meshmaster.dk"));
  CHECK(std::string(settings.serverHost()) == "meshmaster.dk");
  CHECK(settings.serverPort() == 443);
  CHECK(put(settings, "server.url", "https://meshmaster.dk:9443/x"));
  CHECK(settings.serverPort() == 9443);
  // A port the client's parser will not have: the table takes the characters, the parser refuses
  // the number, and no server is configured - which is "the condition does not apply" and not a
  // host the trial would try to reach.
  CHECK(put(settings, "server.url", "https://meshmaster.dk:99999"));
  CHECK(std::string(settings.serverHost()).empty());
  CHECK(settings.serverPort() == 443);
}

TEST_CASE("a value the table refuses is not stored, and the refusal says why") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  std::string why;
  CHECK_FALSE(put(settings, "swd.khz", "9000", &why));
  CHECK(why == "swd.khz: out of range, 25..1000");
  CHECK_FALSE(settings.isSet(Key::kSwdKhz));
  CHECK(!kv.has("swd_khz"));
  CHECK(kv.writes == 0);
  CHECK(settings.swdKhz() == 125);  // still the fallback
}

TEST_CASE("the PIN's epoch moves when the PIN changes, and not when it is typed again") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  CHECK(settings.pinEpoch() == 0);
  CHECK(put(settings, "lte.pin", "1234"));
  const std::uint32_t first = settings.pinEpoch();
  CHECK(first != 0);
  // The same PIN again: nothing changed, so the epoch does not move - a refusal recorded against
  // this PIN is still about this PIN (core/uplink/policy.h, *The PIN*).
  CHECK(put(settings, "lte.pin", "1234"));
  CHECK(settings.pinEpoch() == first);
  // A different PIN: a different PIN.
  CHECK(put(settings, "lte.pin", "5678"));
  CHECK(settings.pinEpoch() == first + 1);
  // And it survives a reboot: a refusal in RTC memory outlives a reset, so which PIN it was
  // about has to outlive one too.
  Settings again(kv);
  again.begin();
  CHECK(again.pinEpoch() == first + 1);
}

TEST_CASE("erase confirm forgets the keys of the table, and only those") {
  fakes::SimKeyValue kv;
  // Something else in the same store that is not a setting of this table.
  kv.put("identity", "7f3a9c02");
  Settings settings(kv);
  settings.begin();
  CHECK(put(settings, "lte.apn", "internet"));
  CHECK(put(settings, "lte.pin", "1234"));
  CHECK(put(settings, "server.url", "https://meshmaster.dk"));
  CHECK(put(settings, "log.level", "debug"));
  const std::uint32_t epoch = settings.pinEpoch();

  char err[128] = {};
  CHECK(settings.eraseAll(err, sizeof err));
  CHECK(err[0] == 0);
  CHECK(settings.setCount() == 0);
  for (const core::settings::KeyDef& key : kKeys) {
    CHECK_MESSAGE(!kv.has(key.nvs), std::string(key.nvs));
  }
  // The fallbacks are in force again.
  CHECK(settings.level() == Level::kInfo);
  CHECK(std::string(settings.apn()).empty());
  CHECK(std::string(settings.serverHost()).empty());
  // What was not a setting of this table is still there.
  CHECK(kv.raw("identity") == "7f3a9c02");
  // The PIN is gone, so the next PIN is a new one: the epoch has moved, or a refusal recorded
  // against the PIN just forgotten would be applied to the one typed after it.
  CHECK(settings.pinEpoch() > epoch);
  CHECK(put(settings, "lte.pin", "1234"));
  CHECK(settings.pinEpoch() > epoch);
}

TEST_CASE("an empty store at boot: every fallback, nothing rejected, no crash") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  CHECK(settings.rejected() == 0);
  CHECK(std::string(settings.firstRejected()).empty());
  CHECK(settings.setCount() == 0);
  CHECK(settings.level() == Level::kInfo);
}

TEST_CASE("a corrupt store at boot: the bad values are dropped, the good ones kept, one line to say so") {
  fakes::SimKeyValue kv;
  // A store written by something that is not this firmware: a value out of range, a URL with the
  // wrong scheme, a PIN with a letter in it, a level that does not exist - and two good values.
  kv.put("swd_khz", "9000");
  kv.put("srv_url", "http://meshmaster.dk");
  kv.put("lte_pin", "12a4");
  kv.put("log_level", "trace");
  kv.put("lte_apn", "internet");
  kv.put("unit_name", "Window at Grenaa");
  Settings settings(kv);
  settings.begin();
  CHECK(settings.rejected() == 4);
  // The first in the table's order, which is the order begin() reads them in.
  CHECK(std::string(settings.firstRejected()) == "lte.pin");
  // Each bad key reads as unset and falls back.
  CHECK_FALSE(settings.isSet(Key::kSwdKhz));
  CHECK(settings.swdKhz() == 125);
  CHECK_FALSE(settings.isSet(Key::kServerUrl));
  CHECK(std::string(settings.serverHost()).empty());
  CHECK_FALSE(settings.isSet(Key::kLtePin));
  CHECK(std::string(settings.pin()).empty());
  CHECK(settings.level() == Level::kInfo);
  // The good ones are kept.
  CHECK(std::string(settings.apn()) == "internet");
  CHECK(std::string(settings.unitName()) == "Window at Grenaa");
  CHECK(settings.setCount() == 2);
  // The bad bytes are still in the store - nothing is quietly rewritten - and a `set` of that
  // key replaces them.
  CHECK(kv.raw("swd_khz") == "9000");
  CHECK(put(settings, "swd.khz", "250"));
  CHECK(kv.raw("swd_khz") == "250");
}

TEST_CASE("a value too long for the buffer is dropped rather than cut") {
  fakes::SimKeyValue kv;
  kv.put("srv_token", std::string(400, 'x').c_str());
  Settings settings(kv);
  settings.begin();
  CHECK(settings.rejected() == 0);  // it did not fit the read at all: the key is simply unset
  CHECK_FALSE(settings.isSet(Key::kServerToken));
  CHECK(std::string(settings.serverToken()).empty());
}

TEST_CASE("no store at all: every key falls back and a set is refused rather than lost") {
  fakes::SimKeyValue kv;
  kv.notReady();
  Settings settings(kv);
  settings.begin();
  CHECK_FALSE(settings.storeReady());
  CHECK(settings.setCount() == 0);
  CHECK(settings.level() == Level::kInfo);
  CHECK(settings.swdKhz() == 125);
  std::string why;
  CHECK_FALSE(put(settings, "lte.apn", "internet", &why));
  CHECK(why.find("no settings store in this build") != std::string::npos);
  CHECK_FALSE(settings.isSet(Key::kLteApn));
}

TEST_CASE("a store that will not take a write: the value is not in force either") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  kv.readOnly();
  std::string why;
  CHECK_FALSE(put(settings, "lte.apn", "internet", &why));
  CHECK(why == "lte.apn could not be stored");
  CHECK_FALSE(settings.isSet(Key::kLteApn));
  CHECK(std::string(settings.apn()).empty());
}

TEST_CASE("a store that will not forget: erase says which key and forgets the rest") {
  fakes::SimKeyValue kv;
  Settings settings(kv);
  settings.begin();
  CHECK(put(settings, "lte.apn", "internet"));
  kv.refuseErase();
  char err[128] = {};
  CHECK_FALSE(settings.eraseAll(err, sizeof err));
  CHECK(std::string(err).find("the store would not forget lte.apn") == 0);
  // What is in force is the fallback all the same: a half-forgotten unit must not look
  // provisioned.
  CHECK(settings.setCount() == 0);
  CHECK(std::string(settings.apn()).empty());
}
