// The settings at the console (task p3-03): `get`, `set`, `erase confirm`, `dump` and `journal`,
// and - the part that is not about printing - that a key which has changed reaches the thing it
// is about: the uplink's APN and PIN, the server a self-update's trial must reach, the SWD clock,
// and what the console says when nobody asked.
#include <doctest.h>

#include <string>

#include "core/swd/wire.h"
#include "settings_console_rig.h"

using bench::hasText;
using bench::SettingsConsole;

TEST_CASE("get prints every setting in the table's order, and one key on its own") {
  SettingsConsole rig;
  const std::string all = rig.run("get");
  // Eleven lines, in the table's order.
  std::size_t at = 0;
  for (const core::settings::KeyDef& key : core::settings::kKeys) {
    const std::size_t found = all.find(key.name, at);
    CHECK_MESSAGE(found != std::string::npos, std::string(key.name));
    at = found;
  }
  CHECK(hasText(all, "lte.apn unset\n"));
  CHECK(hasText(all, "lte.pin unset\n"));
  CHECK(hasText(all, "log.level unset (info)\n"));
  CHECK(hasText(all, "swd.khz unset (125)\n"));
  CHECK(hasText(all, "wake.interval unset (auto)\n"));

  CHECK(rig.run("get swd.khz") == "swd.khz unset (125)\n");
  CHECK(rig.run("get lte.apn") == "lte.apn unset\n");
  // An unknown key, and a key with something after it.
  CHECK(rig.run("get nope") == "error: unknown key (try 'help'): nope\n");
  CHECK(rig.run("get swd.khz 1") == "error: get takes one key: get <key>\n");
}

TEST_CASE("set stores, answers ok and never echoes the value back") {
  SettingsConsole rig;
  CHECK(hasText(rig.run("set lte.apn lpwa.telia.iot"), "ok lte.apn\n"));
  CHECK(hasText(rig.run("get lte.apn"), "lte.apn lpwa.telia.iot"));
  // The value is the rest of the line, trimmed: spaces in it are part of it.
  CHECK(hasText(rig.run("set unit.name  Window at Grenaa  "), "ok unit.name\n"));
  CHECK(rig.run("get unit.name") == "unit.name Window at Grenaa\n");
  // A refusal is one line beginning `error:` that names the key and the rule.
  CHECK(rig.run("set swd.khz 9000") == "error: swd.khz: out of range, 25..1000\n");
  CHECK(rig.run("set log.level trace") == "error: log.level: error | warn | info | debug\n");
  CHECK(rig.run("set server.url http://meshmaster.dk") ==
        "error: server.url: the scheme must be https:// (S-12: TLS is verified)\n");
  CHECK(rig.run("set nope 1") == "error: unknown key (try 'help'): nope\n");
  CHECK(rig.run("set") == "error: set needs a key and a value: set <key> <value>\n");
  CHECK(hasText(rig.run("set lte.apn"), "error: set lte.apn needs a value"));
  // Nothing of the refusals was kept.
  CHECK(rig.run("get swd.khz") == "swd.khz unset (125)\n");
}

TEST_CASE("a secret prints set or unset, and the value echoes as asterisks as it is typed") {
  SettingsConsole rig;
  CHECK(rig.run("get lte.pin") == "lte.pin unset\n");
  // Typed, one character at a time, as a person does it.
  const std::string typed = rig.type("set lte.pin 1234\n");
  CHECK(hasText(typed, "set lte.pin ****"));
  CHECK(!hasText(typed, "1234"));
  CHECK(hasText(typed, "ok lte.pin"));
  CHECK(rig.run("get lte.pin") == "lte.pin set\n");
  // The key itself is not a secret - it echoes - and only what comes after the space is starred.
  const std::string token = rig.type("set server.token mma_abcdef\n");
  CHECK(hasText(token, "set server.token *********"));
  CHECK(!hasText(token, "mma_abcdef"));
  CHECK(rig.run("get server.token") == "server.token set\n");
  // A key that is not a secret echoes as it is typed.
  const std::string plain = rig.type("set lte.apn internet\n");
  CHECK(hasText(plain, "set lte.apn internet"));
}

TEST_CASE("erase confirm forgets every setting, and without the word it says what the word is") {
  SettingsConsole rig;
  CHECK(hasText(rig.run("set lte.apn internet"), "ok lte.apn\n"));
  CHECK(hasText(rig.run("set lte.pin 1234"), "ok lte.pin\n"));
  CHECK(hasText(rig.run("set log.level debug"), "ok log.level\n"));
  CHECK(rig.run("erase") ==
        "error: erase forgets every setting; to do it, type: erase confirm\n");
  CHECK(rig.run("erase now") == "error: erase: the word is 'confirm'\n");
  CHECK(hasText(rig.run("get lte.apn"), "lte.apn internet"));  // nothing happened

  const std::string text = rig.run("erase confirm");
  CHECK(hasText(text, "every setting forgotten; the images, the journal, the bad list and the "
                      "supervisor's counts are untouched"));
  CHECK(hasText(text, "the fallbacks are in force: log.level info, swd 125 kHz, no APN, no PIN, "
                      "no server"));
  CHECK(rig.run("get lte.apn") == "lte.apn unset\n");
  CHECK(rig.run("get lte.pin") == "lte.pin unset\n");
  CHECK(rig.run("get log.level") == "log.level unset (info)\n");
}

TEST_CASE("erase confirm leaves the images, the bad list, the journal and the supervisor alone") {
  SettingsConsole rig;
  // Something in the journal, something on the bad list, something in the supervisor's record.
  const std::uint8_t hash[core::image::kSha256Size] = {0xde, 0xad, 0xbe, 0xef};
  REQUIRE(rig.store.markBad(hash) == core::image::StoreError::kNone);
  rig.supervisor->begin();
  CHECK(hasText(rig.run("rak watch reset-caps"), "the day's counts are empty again"));
  const std::uint32_t entries_before = rig.journal.validEntries();
  CHECK(entries_before > 0);
  const bool record_before = rig.retained.has(interfaces::RetainedSlot::kSupervisor);
  CHECK(record_before);
  CHECK(hasText(rig.run("set lte.apn internet"), "ok lte.apn\n"));

  rig.run("erase confirm");

  // The images and the bad list are in `rakfw`, the journal in `journal`, the supervisor's
  // counts in RTC memory: `erase confirm` is eleven NVS keys and touches none of them.
  const std::string bad = rig.run("image bad");
  INFO("image bad said: " << bad << " count " << rig.store.badCount());
  CHECK(hasText(bad, "deadbeef"));
  CHECK(rig.journal.validEntries() == entries_before);
  CHECK(hasText(rig.run("journal"), "the day's counts emptied by hand"));
  CHECK(rig.retained.has(interfaces::RetainedSlot::kSupervisor));
  CHECK(hasText(rig.run("rak watch"), "today: resets 0/"));
}

TEST_CASE("swd.khz changes the clock every SWD command and every flashing runs at") {
  SettingsConsole rig;
  // At boot the setting is in force: unset, so the wire's own default.
  rig.begin();
  CHECK(core::swd::halfPeriodInForceUs() == core::swd::kDefaultHalfPeriodUs);
  const std::string text = rig.run("set swd.khz 250");
  CHECK(hasText(text, "ok swd.khz"));
  CHECK(hasText(text, "every SWD command and every flashing from here on runs at 250 kHz "
                      "(a half-period of 2 us)"));
  CHECK(core::swd::halfPeriodInForceUs() == 2);
  // A command that builds a wire picks it up without being told.
  CHECK(hasText(rig.run("rak swd connect"), "swd: 250 kHz (half-period 2 us)"));
  // And it is in force again after a reboot, because begin() applies the settings.
  rig.run("set swd.khz 25");
  CHECK(core::swd::halfPeriodInForceUs() == 20);
  core::swd::setHalfPeriodInForceUs(core::swd::kDefaultHalfPeriodUs);  // as a fresh boot has it
  rig.build();
  rig.begin();
  CHECK(core::swd::halfPeriodInForceUs() == 20);
  CHECK(hasText(rig.run("rak swd connect"), "swd: 25 kHz (half-period 20 us)"));
  // Leave the clock as every other test expects to find it.
  core::swd::setHalfPeriodInForceUs(core::swd::kDefaultHalfPeriodUs);
}

TEST_CASE("lte.apn reaches the uplink, and a changed PIN forgets a refusal of the old one") {
  SettingsConsole rig;
  rig.begin();
  CHECK(hasText(rig.run("set lte.apn lpwa.telia.iot"),
                "the APN is used the next time the uplink is brought up"));
  CHECK(std::string(rig.uplink.settings().apn) == "lpwa.telia.iot");

  // The card wants a PIN and the one it is given is wrong: one of its three tries, and the
  // refusal is remembered so that it is never sent again (core/uplink/policy.h, *The PIN*).
  rig.modem.sim_state = interfaces::SimState::kPinRequired;
  const std::string first = rig.run("set lte.pin 9999");
  CHECK(hasText(first, "ok lte.pin"));
  // There was no refusal to forget yet, so the line says what will happen and not what was
  // undone.
  CHECK(hasText(first, "the next uplink sends it; a refused PIN is never sent a second time"));
  CHECK(!hasText(first, "forgotten"));
  rig.run("lte up");
  CHECK(rig.uplink.status().pin_refused);
  CHECK(rig.modem.unlocks == 1);
  CHECK(hasText(rig.run("lte"), "refused"));

  // A different PIN: the refusal was about the old one, so it is forgotten and the card gets
  // one more try - the right one this time.
  const std::string text = rig.run("set lte.pin 1234");
  CHECK(hasText(text, "ok lte.pin"));
  CHECK(hasText(text, "a PIN this unit had been refused is forgotten; the next uplink sends the "
                      "new one"));
  CHECK_FALSE(rig.uplink.status().pin_refused);
  rig.run("lte up");
  CHECK(rig.modem.unlocks == 2);
  CHECK_FALSE(rig.uplink.status().pin_refused);
}

TEST_CASE("lte.psm asks the network for power saving the next time the uplink comes up") {
  SettingsConsole rig;
  rig.begin();
  CHECK(rig.uplink.settings().power_saving == interfaces::PowerSavingMode::kOff);
  const std::string text = rig.run("set lte.psm on");
  CHECK(hasText(text, "power saving is asked for the next time the uplink is brought up (PSM on"));
  CHECK(rig.uplink.settings().power_saving == interfaces::PowerSavingMode::kPsm);
  CHECK(hasText(rig.run("set lte.psm off"), "PSM off"));
  CHECK(rig.uplink.settings().power_saving == interfaces::PowerSavingMode::kOff);
}

TEST_CASE("server.url is the host a self-update's trial must reach, and an unset one does not apply") {
  SettingsConsole rig;
  rig.begin();
  // Unset: no server, and the trial's third condition does not apply
  // (core/update/uplink_check.h).
  CHECK(std::string(rig.server.serverHost()).empty());
  CHECK(rig.server.check() == core::update::UplinkVerdict::kNotConfigured);
  CHECK(hasText(rig.run("status"), "backend: no server (set server.url)"));

  const std::string text = rig.run("set server.url https://staging.meshmaster.dk:8443/agent/v1");
  CHECK(hasText(text, "ok server.url"));
  CHECK(hasText(text, "a self-update's trial now has to reach staging.meshmaster.dk:8443"));
  CHECK(std::string(rig.server.serverHost()) == "staging.meshmaster.dk");
  CHECK(rig.server.serverPort() == 8443);
  CHECK(hasText(rig.run("status"), "backend: staging.meshmaster.dk:8443"));
  // And `erase confirm` takes the server away again: not configured, not unreachable.
  rig.run("erase confirm");
  CHECK(std::string(rig.server.serverHost()).empty());
  CHECK(rig.server.check() == core::update::UplinkVerdict::kNotConfigured);
}

TEST_CASE("log.level: a reply is always printed, a line nobody asked for is not") {
  SettingsConsole rig;
  // At `info`, the default, the boot's own lines are printed.
  std::string boot = rig.begin();
  CHECK(hasText(boot, "boot 1: power-on reset"));
  CHECK(hasText(boot, "settings: 0 of 11 keys set; log.level info, swd 125 kHz"));
  CHECK(hasText(boot, "type 'help' for the commands"));

  CHECK(hasText(rig.run("set log.level error"),
                "what is printed when nobody asked is now error and below"));
  rig.build();
  boot = rig.begin();
  CHECK(!hasText(boot, "boot 1: power-on reset"));
  CHECK(!hasText(boot, "settings: 0 of 11 keys set"));
  // The invitation and the prompt are not log lines: a person at a port gets them whatever the
  // level says, and so does every reply.
  CHECK(hasText(boot, "type 'help' for the commands"));
  CHECK(hasText(rig.run("get log.level"), "log.level error"));
  CHECK(hasText(rig.run("foo"), "error: unknown command (try 'help'): foo"));

  CHECK(hasText(rig.run("set log.level debug"), "now debug and below"));
  rig.build();
  boot = rig.begin();
  CHECK(hasText(boot, "boot 1: power-on reset"));
}

TEST_CASE("dump prints the lines this port printed, oldest first, and nothing the radio said") {
  SettingsConsole rig;
  rig.begin();
  rig.run("get swd.khz");
  rig.run("set unit.name Grenaa");
  // A scanner's line: printed when it is asked for, and never kept (S-12). A scanner answers no
  // HELLO, and the lines it has already sent are what makes this a text session (§9), so the
  // session is found exactly as a bench finds it.
  rig.rak.scanner = true;
  rig.rak.sendLine("{\"noise\":-112,\"note\":\"what the mesh said\"}");
  rig.run("rak hello");
  REQUIRE(rig.link.counters().text_lines == 1);
  const std::string text = rig.run("rak text");
  CHECK(hasText(text, "what the mesh said"));

  const std::string dumped = rig.run("dump");
  CHECK(hasText(dumped, "dump: "));
  CHECK(hasText(dumped, "swd.khz unset (125)"));
  CHECK(hasText(dumped, "ok unit.name"));
  CHECK(hasText(dumped, "boot 1: power-on reset"));
  // The reply's own line is there; the radio's text is not.
  CHECK(hasText(dumped, "line kept of 1 seen"));
  CHECK(!hasText(dumped, "what the mesh said"));
  CHECK(hasText(dumped, "dump: end ("));
  // A secret never reaches a printed line, so there is none in the ring either.
  rig.run("set lte.pin 4321");
  CHECK(!hasText(rig.run("dump"), "4321"));
}

TEST_CASE("dump keeps only whole lines when the ring has wrapped") {
  SettingsConsole rig;
  // More than the ring holds: `get` is about 250 bytes a time.
  for (int i = 0; i < 40; ++i) rig.run("get");
  const std::string dumped = rig.run("dump");
  CHECK(hasText(dumped, "the ring has wrapped, so the log begins at the first whole line"));
  // The first line after the header is a whole line of `get`'s output, not the tail of one.
  const std::size_t first = dumped.find('\n');
  REQUIRE(first != std::string::npos);
  const std::string body = dumped.substr(first + 1);
  bool whole = false;
  for (const core::settings::KeyDef& key : core::settings::kKeys) {
    if (body.rfind(key.name, 0) == 0) whole = true;
  }
  CHECK(whole);
  CHECK(hasText(dumped, "byte(s) dropped since this boot"));
}

TEST_CASE("journal prints the last entries, whatever wrote them") {
  SettingsConsole rig;
  rig.begin();
  CHECK(hasText(rig.run("journal"), "journal: empty: nothing has been journalled on this unit"));
  rig.supervisor->begin();
  rig.run("rak watch reset-caps");
  const std::string text = rig.run("journal");
  CHECK(hasText(text, "entries in 16 sectors, next seq 2"));
  CHECK(hasText(text, "the last entries, newest first:"));
  CHECK(hasText(text, "seq 1: supervisor, the day's counts emptied by hand"));
  CHECK(hasText(text, "today 0 resets, 0 power cycles, 0 flashes"));
  CHECK(hasText(text, "a person's command"));
  CHECK(rig.run("journal now") == "error: journal takes no argument\n");
}

TEST_CASE("status says what the settings are for, and names the unit when it has a name") {
  SettingsConsole rig;
  rig.begin();
  std::string text = rig.run("status");
  CHECK(hasText(text, "unit: no name (set unit.name); role manager, board walter"));
  CHECK(hasText(text, "next wake: nothing sleeps yet (task p3-04); wake.interval auto, "
                      "radio may wake it: yes"));
  CHECK(hasText(text, "images: golden=empty candidate=empty scanner=empty"));
  CHECK(hasText(text, "uplink: "));
  // No secret and no subscription identity, whatever is set.
  rig.run("set lte.pin 4321");
  rig.run("set server.token mma_shhh");
  text = rig.run("status");
  CHECK(!hasText(text, "4321"));
  CHECK(!hasText(text, "mma_shhh"));

  rig.run("set unit.name Window at Grenaa");
  rig.run("set wake.interval 180");
  rig.run("set wake.radio off");
  text = rig.run("status");
  CHECK(hasText(text, "unit: Window at Grenaa; role manager"));
  CHECK(hasText(text, "wake.interval 180 min, radio may wake it: no"));
}

TEST_CASE("status is one screen: a dozen lines, none of them over a hundred characters") {
  SettingsConsole rig;
  rig.begin();
  rig.run("set unit.name Window at Grenaa");
  rig.run("set server.url https://staging.meshmaster.dk:8443/agent/v1");
  const std::string text = rig.run("status");
  std::size_t lines = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? end : end - at);
    // docs/bench.md §13: it is read in a terminal window, and a line that wraps hides itself.
    CHECK_MESSAGE(line.size() <= 100, line);
    ++lines;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  CHECK(lines >= 10);
  CHECK(lines <= 16);
}

TEST_CASE("a build with no settings store says so in one line and keeps working") {
  SettingsConsole rig;
  rig.withoutSettings();
  const char* lines[] = {"get", "get swd.khz", "set lte.apn internet", "erase confirm"};
  for (const char* line : lines) {
    CHECK_MESSAGE(hasText(rig.run(line), "error: this build has no settings store"), std::string(line));
  }
  // Everything else still answers, and the boot lines are at `info`, which is what every build
  // before this task did.
  CHECK(hasText(rig.begin(), "boot 1: power-on reset"));
  CHECK(hasText(rig.run("status"), "unit: no name (set unit.name)"));
  CHECK(hasText(rig.run("help"), "the unit:"));
  CHECK(hasText(rig.run("dump"), "dump: "));
}

TEST_CASE("a store that would not open: one line at boot, the fallbacks, and a set that is refused") {
  SettingsConsole rig;
  rig.kv.notReady();
  rig.settings.begin();
  const std::string boot = rig.begin();
  CHECK(hasText(boot, "settings: the store would not open: every key falls back and a 'set' "
                      "cannot be kept"));
  CHECK(hasText(rig.run("get swd.khz"), "swd.khz unset (125)"));
  CHECK(hasText(rig.run("set swd.khz 250"), "error: there is no settings store in this build"));
  CHECK(core::swd::halfPeriodInForceUs() == core::swd::kDefaultHalfPeriodUs);
}

TEST_CASE("a corrupt store: one line at boot naming the first value it would not have") {
  SettingsConsole rig;
  rig.kv.put("swd_khz", "9000");
  rig.kv.put("log_level", "trace");
  rig.settings.begin();
  const std::string boot = rig.begin();
  CHECK(hasText(boot, "settings: 2 stored value(s) this firmware will not have were ignored "
                      "(the first is swd.khz); those keys fall back"));
  CHECK(hasText(rig.run("get swd.khz"), "swd.khz unset (125)"));
  CHECK(hasText(rig.run("get log.level"), "log.level unset (info)"));
}
