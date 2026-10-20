// The console's `rak provision check` and `rak provision <radio> [confirm]` (task p5-03): what
// they print, the refusal without the word, a malformed radio, and the session of
// docs/provisioning.md's *A factory radio* replayed line for line against this console - the way
// the provisioning session already is (test_provisioning_doc.cpp), with the blocks marked
// ```console-provision, because this session needs a simulated chip under the console.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/provision/provision_bench.h"

using bench::Factory;
using bench::ProvUnit;

namespace fs = std::filesystem;

namespace {

struct Exchange {
  int at = 0;
  std::string typed;
  std::vector<std::string> expected;
};

std::vector<Exchange> sessionOf(const std::string& doc, const char* fence) {
  std::vector<Exchange> out;
  std::istringstream in(doc);
  std::string line;
  int number = 0;
  bool inside = false;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("```", 0) == 0) {
      inside = line == fence;
      continue;
    }
    if (!inside) continue;
    if (line.rfind("> ", 0) == 0) {
      Exchange e;
      e.at = number;
      e.typed = line.substr(2);
      out.push_back(e);
      continue;
    }
    if (line == ">") continue;
    if (out.empty()) continue;
    out.back().expected.push_back(line);
  }
  return out;
}

std::vector<std::string> linesOf(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

bool matches(const std::string& expected, const std::string& got) {
  static const std::string kAny = "...";
  if (expected.size() > kAny.size() && expected.compare(expected.size() - kAny.size(), kAny.size(), kAny) == 0) {
    const std::string prefix = expected.substr(0, expected.size() - kAny.size());
    return got.rfind(prefix, 0) == 0;
  }
  return expected == got;
}

}  // namespace

TEST_CASE("`rak provision check` on an open radio: the base, the battery, the port, the chip, the device id, the line to type") {
  ProvUnit u(Factory::kImprovedOpen);
  const std::string out = u.runLine("rak provision check");
  INFO(out);
  CHECK(bench::hasLine(out, "rak provision check: slot 0, rak4630-single; nothing is written"));
  CHECK(bench::hasLine(out, "  base: sha256 "));
  CHECK(bench::hasLine(out, "bootloader 0x000f4000, S140 6.1.1"));
  CHECK(bench::hasLine(out, "check 5p again over the stored bytes: ok"));
  CHECK(bench::hasLine(out, "  battery: unknown (no reading over the link); accepted, as a first install accepts it"));
  CHECK(bench::hasLine(out, "  swd: DPIDR 0x2ba01477; control access port IDR 0x02880000; protection: open"));
  CHECK(bench::hasLine(out, "  chip: nRF52840, variant AAF0: improved APPROTECT (build code F, revision 3): UICR.APPROTECT = 0x5A"));
  CHECK(bench::hasLine(out, "  device id 0badf00ddeadbeef"));
  CHECK(bench::nextLineOf(out) == "rak provision 0badf00ddeadbeef confirm");
  u.checkNothingWritten();
  CHECK(!u.v().board.swdHeld());
}

TEST_CASE("`rak provision check` on a locked radio: the identity is readable only after the erase; the link decides the word") {
  ProvUnit u(Factory::kImprovedLocked);
  std::string out = u.runLine("rak provision check");
  INFO(out);
  CHECK(bench::hasLine(out, "protection: CLOSED (the identity is readable only after the erase)"));
  CHECK(bench::hasLine(out, "  link: no application answers HELLO"));
  CHECK(bench::nextLineOf(out) == "rak provision locked confirm");
  u.world.applicationAnswers();
  out = u.runLine("rak provision check");
  CHECK(bench::hasLine(out, "  link: an application answers HELLO: public key 7F3A9C02E1B44D9A... ('rak id'); it is named before it is erased"));
  CHECK(bench::nextLineOf(out) == "rak provision locked:7F3A9C02E1B44D9A confirm");
  u.checkNothingWritten();
}

TEST_CASE("`rak provision check` says what the command would refuse: no base, a variant not in the table, a low battery") {
  {
    ProvUnit u(Factory::kImprovedOpen, true, false);
    const std::string out = u.runLine("rak provision check");
    CHECK(bench::hasLine(out, "  base: no base in the store for this slot"));
    CHECK(bench::hasLine(out, "  the command would be refused at step 1: no base in the store"));
    CHECK(bench::hasLine(out, "  device id 0badf00ddeadbeef"));  // the radio is looked at all the same
  }
  {
    ProvUnit u(Factory::kImprovedOpen);
    u.chip().info_variant = bench::kVariantUnknown;
    const std::string out = u.runLine("rak provision check");
    CHECK(bench::hasLine(out, "variant AAE0: NOT IN THE TABLE"));
    CHECK(bench::hasLine(out, "  the command would be refused at step 3: INFO.VARIANT is not in the table; nothing would be erased"));
  }
  {
    ProvUnit u(Factory::kImprovedOpen);
    u.world.applicationAnswers();
    u.world.s.u.radio.override_power = true;
    u.world.s.u.radio.battery_mv = 3600;
    const std::string out = u.runLine("rak provision check");
    CHECK(bench::hasLine(out, "  battery: 3600 mV: the battery is below the update's threshold"));
    CHECK(bench::hasLine(out, "  the command would be refused at step 1: the battery is below"));
  }
}

TEST_CASE("without the word it says what it would do and the line to type; a malformed radio is refused") {
  ProvUnit u(Factory::kImprovedOpen);
  std::string out = u.runLine("rak provision 0badf00ddeadbeef");
  CHECK(bench::hasLine(out, "rak provision 0badf00ddeadbeef: would erase the radio whole - RUI3, any identity, UICR - through the CTRL-AP"));
  CHECK(bench::hasLine(out, "to do it, type: rak provision 0badf00ddeadbeef confirm"));
  u.checkNothingWritten();
  CHECK(u.v().target.counters().transfers == 0);
  out = u.runLine("rak provision 0badf00ddeadbee confirm");
  CHECK(bench::hasLine(out, "error: rak provision: not a radio: type the DEVICEID (16 hex digits, as 'rak swd id' prints it), 'locked', or 'locked:<key16>'; 'rak provision check' says what to type"));
  CHECK(u.v().target.counters().transfers == 0);
  out = u.runLine("rak provision");
  CHECK(bench::hasLine(out, "error: rak provision needs a radio"));
  out = u.runLine("rak provision 0badf00ddeadbeef yes");
  CHECK(bench::hasLine(out, "error: rak provision: the only word after the radio is 'confirm'"));
  out = u.runLine("rak provision check now");
  CHECK(bench::hasLine(out, "error: rak provision check takes no argument"));
  u.checkNothingWritten();
}

TEST_CASE("a refusal is one error line naming the step, and whether the radio was erased") {
  ProvUnit u(Factory::kImprovedOpen);
  std::string out = u.runLine("rak provision 0badf00ddeadbeee confirm");
  CHECK(bench::hasLine(out, "error: rak provision: refused at step 2 (attach): the DEVICEID typed is not this radio's"));
  CHECK(bench::hasLine(out, "  nothing was erased"));
  CHECK(bench::hasLine(out, "  next: rak provision 0badf00ddeadbeef confirm"));
  // Erased and stopped: the variant outside the table on a locked radio.
  ProvUnit locked(Factory::kImprovedLocked);
  locked.chip().info_variant = bench::kVariantUnknown;
  out = locked.runLine("rak provision locked confirm");
  INFO(out);
  CHECK(bench::hasLine(out, "error: rak provision: refused at step 3 (variant): INFO.VARIANT is not in the table: stopped with the radio erased"));
  CHECK(bench::hasLine(out, "  the radio is erased: run the command again from the top ('rak provision check' says what to type)"));
}

TEST_CASE("the seven steps are printed as they end, and the outcome says what to do next") {
  ProvUnit u(Factory::kLegacyLocked);
  const std::string out = u.runLine("rak provision locked confirm");
  INFO(out);
  CHECK(bench::hasLine(out, "rak provision: slot 0, locked: S-5a's seven steps; nothing is written before step 4"));
  CHECK(bench::hasLine(out, "  [1/7] checked: the base sha256 "));
  CHECK(bench::hasLine(out, "  [2/7] attached: DPIDR 0x2ba01477, protection CLOSED; the link: no application answers HELLO, as 'locked' says"));
  CHECK(bench::hasLine(out, "  [3/7] variant AAC0: legacy APPROTECT (build code A-D): UICR.APPROTECT left erased"));
  CHECK(bench::hasLine(out, "  [4/7] erased: CTRL-AP ERASEALL; the port open after one CTRL-AP soft reset (the only reset until step 7)"));
  CHECK(bench::hasLine(out, "  [5/7] written: "));
  CHECK(bench::hasLine(out, "UICR.APPROTECT left erased (legacy)"));
  CHECK(bench::hasLine(out, "  [6/7] verified: "));
  CHECK(bench::hasLine(out, "UICR.APPROTECT reads 0xffffffff"));
  CHECK(bench::hasLine(out, "  [7/7] reset: one pin reset (waited 0 ms for the spacing), the bootloader given 1500 ms; the port after it open, NRFFW[0] 0x000f4000"));
  CHECK(bench::hasLine(out, "rak provision: PROVISIONED slot 0: device id 0badf00ddeadbeef, variant AAC0, base sha256 "));
  CHECK(bench::hasLine(out, "port after the reset open"));
  CHECK(bench::hasLine(out, "  the slot awaits its first install: the supervisor leaves the radio alone until 'rak update --first-install' commits"));
  CHECK(bench::hasLine(out, "  next: python tools/walterctl.py push --port <port> --as candidate <release-dir>"));
  CHECK(bench::hasLine(out, "        rak update --first-install"));
  // And on a revision 3 part whose bootloader does not open the port: said, not failed.
  ProvUnit rev3(Factory::kImprovedLocked);
  rev3.chip().bootloader_opens_port = false;
  const std::string closed = rev3.runLine("rak provision locked confirm");
  CHECK(bench::hasLine(closed, "the port after it CLOSED"));
  CHECK(bench::hasLine(closed, "PROVISIONED slot 0"));
  CHECK(bench::hasLine(closed, "the access port is CLOSED after the reset: this bootloader does not copy UICR.APPROTECT into APPROTECT.DISABLE"));
}

TEST_CASE("docs/provisioning.md's *A factory radio* is a session this console answers, line for line") {
  const fs::path path = fs::path(TWINOAK_REPO_ROOT) / "docs" / "provisioning.md";
  REQUIRE(fs::is_regular_file(path));
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::vector<Exchange> session = sessionOf(buffer.str(), "```console-provision");
  // The session's unit: an improved (revision 3) module as RAK ships it, open, with the base and
  // a candidate pushed over the cable already; the radio answers on the link once it runs.
  ProvUnit u(Factory::kImprovedOpen);
  u.world.wholeArray();
  u.world.putCandidate();
  if (session.size() < 4) {
    // The document's block is missing or empty: the transcript it should hold, for whoever
    // writes it, and a failed test.
    std::string transcript;
    for (const char* line : {"rak swd id", "rak provision check", "rak provision 0badf00ddeadbeef confirm",
                             "rak update --first-install", "rak watch"}) {
      transcript += "> " + std::string(line) + "\n" + u.runLine(line);
    }
    FAIL("docs/provisioning.md has no ```console-provision session; this console would answer:\n" << transcript);
  }
  for (const Exchange& exchange : session) {
    const std::string answer = u.runLine(exchange.typed.c_str());
    const std::vector<std::string> got = linesOf(answer);
    INFO("docs/provisioning.md:" << exchange.at << ": > " << exchange.typed << "\nanswered:\n" << answer);
    REQUIRE(got.size() == exchange.expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK_MESSAGE(matches(exchange.expected[i], got[i]),
                    "line " << (i + 1) << ": wanted \"" << exchange.expected[i] << "\", got \"" << got[i] << "\"");
    }
  }
  // The session ends with a radio that runs its first application and a slot that no longer
  // awaits one.
  CHECK(u.world.s.u.radio_runs);
  CHECK(!u.v().supervisor.awaitingFirstInstall());
  u.checkChipHoldsBase();
}
