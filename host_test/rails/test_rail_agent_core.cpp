// The agent core's pin, in the suite (task p4-01).
//
// `components/agent-core/` and `host_test/agent-core/` are one checked copy of
// twinoak/twinoak.meshmaster.exitnode-firmware at one commit (docs/upstream.md,
// docs/ARCHITECTURE.md §13), and the commit is written down in **exactly two places** - the
// `MANIFEST.txt` at the root of each tree. One version in two places is how a pin drifts, so this
// file is the test that holds them equal, and it is here rather than only in
// `tools/sync-agent-core.ps1 -Verify` so that a checkout which never runs that script is held to
// it too: the library compiled into the board's image and the fakes the library's own suites run
// against have to be the same release of the same thing.
//
// Beside that it holds what else cannot drift silently: the library's three statements of its own
// version, the one dependency it brings and the version this repository resolved for it, and that
// the test support the suites need is all there. The hashing half - every file of both trees
// against its manifest line - is `-Verify`, which `tools/verify.ps1` runs before the container is
// started; the boundary half - nothing of Arduino, the ESP-IDF, WiFi, NVS or MeshCore in the
// library - is `tools/check_rails.py`'s rail A.
#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const fs::path kRepo = fs::path(TWINOAK_REPO_ROOT);
const fs::path kComponent = kRepo / "components" / "agent-core";
const fs::path kHost = kRepo / "host_test" / "agent-core";

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The commit out of a manifest's header: `# source: <what> at commit <40 hex>`. Empty when the
// line is not there, which fails the check that reads it rather than throwing here.
std::string manifestCommit(const fs::path& manifest) {
  std::istringstream lines(readFile(manifest));
  std::string line;
  const std::string marker = " at commit ";
  while (std::getline(lines, line)) {
    if (line.rfind("# source: ", 0) != 0) continue;
    const std::size_t at = line.find(marker);
    if (at == std::string::npos) continue;
    std::string commit = line.substr(at + marker.size());
    while (!commit.empty() && (commit.back() == '\r' || commit.back() == ' ')) commit.pop_back();
    return commit;
  }
  return std::string();
}

bool isSha1(const std::string& s) {
  if (s.size() != 40) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    return digit || lower;
  });
}

// The value of a `"name": "value"` member of a small JSON manifest, read as text: these two files
// are one line each of interest and a JSON parser for them would be a dependency for nothing.
std::string jsonMember(const std::string& text, const std::string& name) {
  const std::string key = "\"" + name + "\"";
  std::size_t at = text.find(key);
  if (at == std::string::npos) return std::string();
  at = text.find(':', at + key.size());
  if (at == std::string::npos) return std::string();
  const std::size_t open = text.find('"', at);
  if (open == std::string::npos) return std::string();
  const std::size_t close = text.find('"', open + 1);
  if (close == std::string::npos) return std::string();
  return text.substr(open + 1, close - open - 1);
}

// The `#define AGENT_CORE_VERSION "x.y.z"` of the library's own header.
std::string versionHeaderSays() {
  const std::string text = readFile(kComponent / "include" / "agent" / "Version.h");
  const std::string marker = "#define AGENT_CORE_VERSION \"";
  const std::size_t at = text.find(marker);
  if (at == std::string::npos) return std::string();
  const std::size_t from = at + marker.size();
  const std::size_t to = text.find('"', from);
  if (to == std::string::npos) return std::string();
  return text.substr(from, to - from);
}

// A `key: "value"` or `key: value` line of a small YAML file, first match, quotes stripped.
std::string yamlValue(const std::string& text, const std::string& key) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t at = line.find(key + ":");
    if (at == std::string::npos) continue;
    if (line.find('#') < at) continue;  // a line that only talks about it
    std::string value = line.substr(at + key.size() + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '"')) value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '"')) value.pop_back();
    if (!value.empty()) return value;
  }
  return std::string();
}

std::size_t countFiles(const fs::path& dir, const std::string& extension) {
  if (!fs::is_directory(dir)) return 0;
  std::size_t n = 0;
  for (const auto& item : fs::directory_iterator(dir)) {
    if (item.is_regular_file() && item.path().extension() == extension) n++;
  }
  return n;
}

}  // namespace

TEST_CASE("the agent core's commit is in two places and they are the same") {
  const fs::path componentManifest = kComponent / "MANIFEST.txt";
  const fs::path hostManifest = kHost / "MANIFEST.txt";
  REQUIRE_MESSAGE(fs::is_regular_file(componentManifest),
                  "components/agent-core/MANIFEST.txt is gone: the copy has no pin");
  REQUIRE_MESSAGE(fs::is_regular_file(hostManifest),
                  "host_test/agent-core/MANIFEST.txt is gone: the test support has no pin");

  const std::string library = manifestCommit(componentManifest);
  const std::string support = manifestCommit(hostManifest);
  CHECK_MESSAGE(isSha1(library),
                "components/agent-core/MANIFEST.txt names no commit; it says: " << library);
  CHECK_MESSAGE(isSha1(support),
                "host_test/agent-core/MANIFEST.txt names no commit; it says: " << support);
  CHECK_MESSAGE(library == support,
                "the library is at " << library << " and its fakes and fixtures are at " << support
                                    << " - the board would run one release of the agent and the "
                                       "suites would test another. Re-run "
                                       "tools/sync-agent-core.ps1 -Ref <tag> for both trees.");

  // And both say where they came from, so that the pin is readable without this test.
  const std::string upstream = "twinoak/twinoak.meshmaster.exitnode-firmware";
  CHECK(readFile(componentManifest).find(upstream) != std::string::npos);
  CHECK(readFile(hostManifest).find(upstream) != std::string::npos);
}

TEST_CASE("the library states one version, in its header, its library.json and its idf_component.yml") {
  // Upstream's own test_library holds these three equal there; the copy is held to it here, so a
  // bump that moved one of them alone cannot land quietly (docs/upstream.md).
  const std::string header = versionHeaderSays();
  const std::string json = jsonMember(readFile(kComponent / "library.json"), "version");
  const std::string yaml = yamlValue(readFile(kComponent / "idf_component.yml"), "version");
  CHECK_MESSAGE(!header.empty(), "agent/Version.h does not define AGENT_CORE_VERSION");
  CHECK_MESSAGE(header == json,
                "agent/Version.h says " << header << " and library.json says " << json);
  CHECK_MESSAGE(header == yaml,
                "agent/Version.h says " << header << " and idf_component.yml says " << yaml);
}

TEST_CASE("ArduinoJson is the library's one dependency, at one exact version, and the lock file resolved it") {
  const std::string json = readFile(kComponent / "library.json");
  const std::string yaml = readFile(kComponent / "idf_component.yml");
  // library.json: "bblanchon/ArduinoJson": "7.4.3"
  const std::string wanted = jsonMember(json, "bblanchon/ArduinoJson");
  CHECK_MESSAGE(!wanted.empty(), "library.json names no ArduinoJson version");
  // idf_component.yml: the same number, as an exact pin and not a range - which is this
  // repository's rule for every dependency (tasks/README.md) and upstream's too.
  const bool exact = yaml.find("version: \"==" + wanted + "\"") != std::string::npos;
  CHECK_MESSAGE(exact, "idf_component.yml does not pin ArduinoJson at exactly " << wanted);
  // And dependencies.lock, which is what the component manager actually resolved for the board
  // and what host_test/ compiles the library against.
  const std::string lock = readFile(kRepo / "dependencies.lock");
  CHECK_MESSAGE(lock.find("bblanchon/arduinojson") != std::string::npos,
                "dependencies.lock does not carry ArduinoJson at all");
  CHECK_MESSAGE(lock.find("version: " + wanted) != std::string::npos,
                "dependencies.lock resolved an ArduinoJson that is not " << wanted);
}

TEST_CASE("the test support the library's suites need came across whole") {
  // The two suites, at upstream's paths - which is why the copy keeps them (the runner's
  // `Citations` case reads test/support/FakeMesh.h and platformio.ini relative to PROJECT_DIR).
  CHECK(fs::is_regular_file(kHost / "test" / "test_runner" / "test_runner.cpp"));
  CHECK(fs::is_regular_file(kHost / "test" / "test_loop" / "test_loop.cpp"));
  // The four fakes - one per interface the library stands on, `IRetained` included since v0.3.0 -
  // and the fixture reader and the job builders beside them.
  for (const char* fake : {"FakeMesh.h", "FakePlatform.h", "FakeRetained.h", "FakeUplink.h",
                           "Fixtures.h", "Jobs.h"}) {
    const fs::path p = kHost / "test" / "support" / fake;
    CHECK_MESSAGE(fs::is_regular_file(p), "host_test/agent-core/test/support/" << fake << " is gone");
  }

  // Both fixture sets, each with the manifest and the README that say which commit of
  // twinoak/meshmaster.dk they are: the envelopes move when the agent API does, the frames when
  // MeshCore does, and they have a commit each (docs/library.md §9).
  const fs::path contracts = kHost / "tests" / "fixtures" / "contracts";
  const fs::path companion = kHost / "tests" / "fixtures" / "companion";
  CHECK(fs::is_regular_file(contracts / "MANIFEST.txt"));
  CHECK(fs::is_regular_file(companion / "MANIFEST.txt"));
  const std::size_t envelopes = countFiles(contracts, ".json");
  const std::size_t frames = countFiles(companion, ".hex");
  CHECK_MESSAGE(envelopes >= 40, "only " << envelopes << " contract fixtures came across");
  CHECK_MESSAGE(frames >= 40, "only " << frames << " companion frames came across");

  // Upstream's platformio.ini, for the [meshcore] pin the `Citations` case reads: a fake that
  // cites a firmware function cites it at a commit, and the commit is in that file.
  const std::string ini = readFile(kHost / "platformio.ini");
  std::istringstream lines(ini);
  std::string line;
  std::string pin;
  while (std::getline(lines, line)) {
    if (line.rfind("pin = ", 0) != 0) continue;
    pin = line.substr(6);
    while (!pin.empty() && (pin.back() == '\r' || pin.back() == ' ')) pin.pop_back();
    break;
  }
  CHECK_MESSAGE(isSha1(pin), "host_test/agent-core/platformio.ini has no [meshcore].pin: " << pin);
}
