// The provisioning session of docs/provisioning.md, replayed (task p3-03).
//
// The document is the session a person types to make a Walter theirs, and it is read **out of the
// document** and run against the firmware's own console: every line after a `> ` prompt is typed,
// and the lines that follow it in the document are what the console has to answer. So the
// document cannot rot - a reply that changes without the document changing fails here - and it
// cannot be a session the console would refuse.
//
// The only latitude: an expected line ending in `...` is a prefix, for the lines that carry a
// time, a count or a version. Everything else is exact.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "settings_console_rig.h"

namespace fs = std::filesystem;

using bench::SettingsConsole;

namespace {

struct Exchange {
  int at = 0;                        // the line of the document the prompt is on
  std::string typed;                 // what follows "> "
  std::vector<std::string> expected; // the lines the console has to answer with
};

// The fenced blocks marked ```console in the document, as exchanges. A block may hold several.
std::vector<Exchange> sessionOf(const std::string& doc) {
  std::vector<Exchange> out;
  std::istringstream in(doc);
  std::string line;
  int number = 0;
  bool inside = false;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("```", 0) == 0) {
      inside = line == "```console";
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
    if (line == ">") continue;  // the prompt with nothing typed: the end of a block
    if (out.empty()) continue;  // output before the first prompt: the boot's own lines
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
  if (expected.size() > kAny.size() &&
      expected.compare(expected.size() - kAny.size(), kAny.size(), kAny) == 0) {
    const std::string prefix = expected.substr(0, expected.size() - kAny.size());
    return got.rfind(prefix, 0) == 0;
  }
  return expected == got;
}

}  // namespace

TEST_CASE("docs/provisioning.md is a session this console answers, line for line") {
  const fs::path path = fs::path(TWINOAK_REPO_ROOT) / "docs" / "provisioning.md";
  REQUIRE(fs::is_regular_file(path));
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::vector<Exchange> session = sessionOf(buffer.str());
  // A session of a dozen lines or more: a document whose block somebody emptied must not pass.
  REQUIRE(session.size() >= 12);

  SettingsConsole rig;
  rig.begin();
  for (const Exchange& exchange : session) {
    const std::string answer = rig.run(exchange.typed.c_str());
    const std::vector<std::string> got = linesOf(answer);
    INFO("docs/provisioning.md:" << exchange.at << ": > " << exchange.typed << "\nanswered:\n"
                                 << answer);
    REQUIRE(got.size() == exchange.expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK_MESSAGE(matches(exchange.expected[i], got[i]),
                    "line " << (i + 1) << ": wanted \"" << exchange.expected[i] << "\", got \""
                            << got[i] << "\"");
    }
  }
  // The session ends with a provisioned unit: the keys a person came to set are set, and the two
  // secrets are set without either value having been printed.
  CHECK(rig.settings.isSet(core::settings::Key::kLteApn));
  CHECK(rig.settings.isSet(core::settings::Key::kLtePin));
  CHECK(rig.settings.isSet(core::settings::Key::kServerUrl));
  CHECK(rig.settings.isSet(core::settings::Key::kServerToken));
  CHECK(rig.settings.isSet(core::settings::Key::kUnitName));
  // The token the session set is in the settings and in nothing the port said.
  CHECK(rig.everything.find("mma_hRk") == std::string::npos);
  CHECK(std::string(rig.settings.serverToken()).rfind("mma_hRk", 0) == 0);
}

TEST_CASE("the replay would notice a document that no longer matches") {
  // The matcher itself, shown working and shown refusing: a test that compares nothing would
  // pass whatever the document said.
  CHECK(matches("ok lte.apn", "ok lte.apn"));
  CHECK_FALSE(matches("ok lte.apn", "ok lte.pin"));
  CHECK(matches("uptime 3 s...", "uptime 3 s, boot 1"));
  CHECK_FALSE(matches("uptime 3 s...", "booted: power-on"));
  const std::vector<Exchange> none = sessionOf("no fenced block here\n");
  CHECK(none.empty());
  const std::vector<Exchange> one = sessionOf("```console\n> get lte.apn\nlte.apn unset\n```\n");
  REQUIRE(one.size() == 1);
  CHECK(one[0].typed == "get lte.apn");
  REQUIRE(one[0].expected.size() == 1);
  CHECK(one[0].expected[0] == "lte.apn unset");
}
