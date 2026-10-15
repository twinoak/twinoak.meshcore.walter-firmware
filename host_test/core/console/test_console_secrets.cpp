// The secrets (task p3-03): a SIM's PIN and a server's token are typed once and printed by
// nothing afterwards.
//
// The rail is not "the commands we thought of": it is **every command of the table**
// (core/console/commands.h). The list below has one line per row of that table and a check that
// it does, so a command added without a line here fails this test rather than quietly escaping
// it. Every one is run with both secrets set, and then everything the console printed - the
// replies, the refusals, the boot lines, the log ring `dump` prints, the journal - is searched
// for the two values and for the characters of them.
//
// S-12's other half is here too: nothing the mesh said is in the ring (`dump`), and the
// subscription's identity (IMEI, ICCID, IMSI) appears in `lte id` and nowhere else - which
// host_test/core/uplink/test_uplink_privacy.cpp holds for the uplink's own commands.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/console/commands.h"
#include "settings_console_rig.h"

using bench::hasText;
using bench::SettingsConsole;
using core::console::CommandDef;
using core::console::kCommands;
using core::console::kCommandCount;

namespace {

// The secrets this test sets. Long enough that a stray substring would be found, and nothing
// else in any line looks like either.
constexpr const char* kPin = "86421357";
constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";

// One line per command of the table: the command, and what to type after it so that it runs.
struct Line {
  const char* word;  // the table's `word`
  const char* line;  // what to type
};

const Line kLines[] = {
    {"help", "help"},
    {"status", "status"},
    {"get", "get"},
    {"set", "set unit.name Window at Grenaa"},
    {"erase", "erase"},  // the refusal: the forgetting has its own case below
    {"reboot", "reboot"},
    {"reboots", "reboots"},
    {"dump", "dump"},
    {"pins", "pins"},
    {"rak reset", "rak reset"},
    {"rak kill", "rak kill 1"},
    {"rak hello", "rak hello"},
    {"rak ping", "rak ping"},
    {"rak status", "rak status"},
    {"rak id", "rak id"},
    {"rak time", "rak time"},
    {"rak power", "rak power"},
    {"rak env", "rak env"},
    {"rak cli", "rak cli get radio"},
    {"rak events", "rak events"},
    {"rak text", "rak text"},
    {"rak swd connect", "rak swd connect"},
    {"rak swd id", "rak swd id"},
    {"rak swd mem", "rak swd mem 0x10000100 4"},
    {"rak flash-test", "rak flash-test confirm"},
    {"rak update", "rak update"},
    {"rak update --first-install", "rak update --first-install"},
    {"rak update status", "rak update status"},
    {"rak role", "rak role scanner"},
    {"rak watch", "rak watch"},
    {"rak watch reset-caps", "rak watch reset-caps"},
    {"rak ladder", "rak ladder"},
    {"image list", "image list"},
    {"image verify", "image verify candidate"},
    {"image erase", "image erase candidate"},
    {"image bad", "image bad"},
    {"image receive", "image receive candidate"},
    {"trust", "trust"},
    {"trust tls", "trust tls"},
    {"self status", "self status"},
    {"self receive", "self receive"},
    {"self switch", "self switch"},
    {"self confirm", "self confirm"},
    {"self reject", "self reject"},
    {"lte", "lte"},
    {"lte up", "lte up"},
    {"lte down", "lte down"},
    {"lte time", "lte time"},
    {"lte id", "lte id"},
    {"http get", "http get https://r.example/x"},
    {"fetch", "fetch https://r.example/v1"},
    {"wake", "wake"},
    {"power", "power"},
    {"sleep now", "sleep now"},
    {"sleep", "sleep 600"},
    {"stay", "stay"},
    {"journal", "journal"},
};

constexpr std::size_t kLineCount = sizeof kLines / sizeof kLines[0];

}  // namespace

TEST_CASE("the list below has a line for every command of the table") {
  CHECK(kLineCount == kCommandCount);
  for (const CommandDef& command : kCommands) {
    bool found = false;
    for (const Line& line : kLines) {
      if (std::strcmp(line.word, command.word) == 0) found = true;
    }
    CHECK_MESSAGE(found, std::string(command.word));
  }
  // And every line is a line the parser takes, or a refusal it names - never an unknown command,
  // which would mean this test ran nothing.
  for (const Line& line : kLines) {
    const core::console::Parsed p = core::console::parseLine(line.line);
    if (!p.ok) {
      CHECK_MESSAGE(std::string(p.error).find("unknown") == std::string::npos, std::string(line.line));
    }
  }
}

TEST_CASE("with both secrets set, no command prints either of them") {
  SettingsConsole rig;
  rig.begin();
  // Typed as a person types them, so the echo is in `everything` too.
  rig.type("set lte.pin 86421357\n");
  rig.type("set server.token mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV\n");
  CHECK(rig.settings.isSet(core::settings::Key::kLtePin));
  CHECK(rig.settings.isSet(core::settings::Key::kServerToken));
  // A scanner line as well, so that `rak text` and `dump` both have something to be wrong about.
  rig.rak.scanner = true;
  rig.rak.sendLine("{\"noise\":-112,\"note\":\"what the mesh said\"}");
  rig.clock.advance(50);

  for (const Line& line : kLines) {
    const std::string text = rig.run(line.line);
    CHECK_MESSAGE(text.find(kPin) == std::string::npos, std::string(line.line));
    CHECK_MESSAGE(text.find(kToken) == std::string::npos, std::string(line.line));
    // Not the token's distinctive body either, in case something printed a prefix.
    CHECK_MESSAGE(text.find("zQ7x4LpR") == std::string::npos, std::string(line.line));
  }
  // `dump` last of all: the ring now holds every line above, and it must hold neither secret.
  const std::string dumped = rig.run("dump");
  CHECK(dumped.find(kPin) == std::string::npos);
  CHECK(dumped.find(kToken) == std::string::npos);
  CHECK(!hasText(dumped, "what the mesh said"));

  // And the whole session, echo included.
  CHECK(rig.everything.find(kPin) == std::string::npos);
  CHECK(rig.everything.find(kToken) == std::string::npos);
  CHECK(rig.everything.find("zQ7x4LpR") == std::string::npos);
  // The session really did print a great deal - this is not a test that passed by printing
  // nothing.
  CHECK(rig.everything.size() > 4000);
  CHECK(hasText(rig.everything, "lte.pin set"));
  CHECK(hasText(rig.everything, "server.token set"));
}

TEST_CASE("the journal and the retained records never carry a secret either") {
  SettingsConsole rig;
  rig.begin();
  rig.supervisor->begin();
  rig.run("set lte.pin 86421357");
  rig.run("set server.token mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV");
  rig.run("lte up");
  rig.run("rak watch reset-caps");
  // The journal's bytes, read straight out of the simulated flash.
  std::string flash;
  for (std::uint32_t at = 0; at < rig.journal_flash.size(); at += 64) {
    std::uint8_t buf[64] = {};
    REQUIRE(rig.journal_flash.read(at, buf, sizeof buf));
    flash.append(reinterpret_cast<const char*>(buf), sizeof buf);
  }
  CHECK(flash.find(kPin) == std::string::npos);
  CHECK(flash.find(kToken) == std::string::npos);
  // And what `journal` prints of them.
  const std::string text = rig.run("journal");
  CHECK(text.find(kPin) == std::string::npos);
  CHECK(text.find(kToken) == std::string::npos);
}

TEST_CASE("erase confirm says nothing of what it forgot") {
  SettingsConsole rig;
  rig.run("set lte.pin 86421357");
  rig.run("set server.token mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV");
  const std::string text = rig.run("erase confirm");
  CHECK(text.find(kPin) == std::string::npos);
  CHECK(text.find(kToken) == std::string::npos);
  CHECK(rig.run("get lte.pin") == "lte.pin unset\n");
  CHECK(rig.run("get server.token") == "server.token unset\n");
}
