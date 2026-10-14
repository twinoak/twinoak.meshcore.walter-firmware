// The rail (task p3-02), as a test over the tree and not a sentence: the library's TLS validation
// levels other than "the chain and the host name" are named nowhere in this firmware's sources;
// the one that is appears in exactly one file, where tlsConfigProfile is called, and that call
// appears exactly once; and no `http://` literal is in the firmware's code. Comment lines are
// dropped first, so that saying a name in a sentence is allowed - tools/verify.sh runs the same
// three greps as a second lock on the same door. Each is shown failing on purpose on a snippet.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Hit {
  std::string file;
  int line;
  std::string text;
};

// The source files under components/ and main/ (not managed_components/, which is the vendored
// library and names everything).
std::vector<fs::path> firmwareSources() {
  std::vector<fs::path> files;
  for (const char* dir : {"components", "main"}) {
    const fs::path root = fs::path(TWINOAK_REPO_ROOT) / dir;
    REQUIRE(fs::is_directory(root));
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) continue;
      const std::string ext = entry.path().extension().string();
      if (ext == ".cpp" || ext == ".h" || ext == ".c" || ext == ".hpp") files.push_back(entry.path());
    }
  }
  return files;
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool isCommentLine(const std::string& line) {
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  return line.compare(i, 2, "//") == 0 || line.compare(i, 2, "/*") == 0 || line.compare(i, 1, "*") == 0;
}

// Lines of `source` (code lines only) that contain `needle`.
std::vector<Hit> codeLinesWith(const std::string& source, const std::string& name, const std::string& needle) {
  std::vector<Hit> hits;
  std::istringstream in(source);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (isCommentLine(line)) continue;
    if (line.find(needle) != std::string::npos) hits.push_back(Hit{name, number, line});
  }
  return hits;
}

std::string repoRelative(const fs::path& p) {
  return fs::relative(p, fs::path(TWINOAK_REPO_ROOT)).generic_string();
}

}  // namespace

TEST_CASE("the library's validation levels: only URL_AND_CA is named, in one file, and tlsConfigProfile is called exactly once") {
  std::vector<Hit> validation;
  std::vector<Hit> configure;
  for (const fs::path& p : firmwareSources()) {
    const std::string source = readFile(p);
    for (const Hit& h : codeLinesWith(source, repoRelative(p), "WALTER_MODEM_TLS_VALIDATION_")) validation.push_back(h);
    for (const Hit& h : codeLinesWith(source, repoRelative(p), "tlsConfigProfile")) configure.push_back(h);
  }
  // Every mention of a validation level is the full one, and they are all in the one file.
  REQUIRE_FALSE(validation.empty());
  for (const Hit& h : validation) {
    CHECK_MESSAGE(h.text.find("WALTER_MODEM_TLS_VALIDATION_URL_AND_CA") != std::string::npos,
                  h.file << ":" << h.line << ": a TLS validation level other than URL_AND_CA: " << h.text);
    CHECK_MESSAGE(h.file == "components/modem/walter_tls.cpp",
                  h.file << ":" << h.line << ": a TLS validation level named outside walter_tls.cpp");
  }
  // And none of the lesser ones, by name, anywhere.
  for (const Hit& h : validation) {
    CHECK(h.text.find("VALIDATION_NONE") == std::string::npos);
    CHECK(h.text.find("VALIDATION_CA)") == std::string::npos);
    CHECK(h.text.find("VALIDATION_CA,") == std::string::npos);
    CHECK(h.text.find("VALIDATION_URL)") == std::string::npos);
    CHECK(h.text.find("VALIDATION_URL,") == std::string::npos);
  }
  // The profile is configured from exactly one place.
  REQUIRE_MESSAGE(configure.size() == 1, "tlsConfigProfile appears in " << configure.size() << " code lines");
  CHECK(configure[0].file == "components/modem/walter_tls.cpp");
  CHECK(configure[0].text.find("WALTER_MODEM_TLS_VALIDATION_URL_AND_CA") != std::string::npos);
  // The TLS version is 1.2, named beside it, and no older one is named anywhere.
  bool version_12 = false;
  for (const fs::path& p : firmwareSources()) {
    const std::string source = readFile(p);
    for (const Hit& h : codeLinesWith(source, repoRelative(p), "WALTER_MODEM_TLS_VERSION_")) {
      const bool modern = h.text.find("WALTER_MODEM_TLS_VERSION_12") != std::string::npos ||
                          h.text.find("WALTER_MODEM_TLS_VERSION_13") != std::string::npos;
      CHECK_MESSAGE(modern, h.file << ":" << h.line << ": a TLS version older than 1.2: " << h.text);
      version_12 = version_12 || h.text.find("WALTER_MODEM_TLS_VERSION_12") != std::string::npos;
    }
  }
  CHECK(version_12);
}

TEST_CASE("no http:// literal in the firmware's code (tests and comments may have one)") {
  std::vector<Hit> hits;
  // A comment that is not at the start of a line - `x = 1;  // see http://...` - would be code
  // to the line check; a literal is what is forbidden, so only the quoted form counts.
  for (const fs::path& p : firmwareSources()) {
    for (const Hit& h : codeLinesWith(readFile(p), repoRelative(p), "\"http://")) hits.push_back(h);
  }
  for (const Hit& h : hits) FAIL_CHECK(h.file << ":" << h.line << ": an http:// literal: " << h.text);
  CHECK(hits.empty());
}

TEST_CASE("the greps fail on purpose: a snippet with VALIDATION_NONE, a second tlsConfigProfile, an http:// literal") {
  const std::string bad =
      "// a comment naming WALTER_MODEM_TLS_VALIDATION_NONE is fine\n"
      "  tlsConfigProfile(2, WALTER_MODEM_TLS_VALIDATION_NONE, WALTER_MODEM_TLS_VERSION_12, 12);\n"
      "  const char* url = \"http://insecure.example/\";\n";
  const std::vector<Hit> validation = codeLinesWith(bad, "snippet", "WALTER_MODEM_TLS_VALIDATION_");
  REQUIRE(validation.size() == 1);
  CHECK(validation[0].line == 2);
  CHECK(validation[0].text.find("VALIDATION_URL_AND_CA") == std::string::npos);  // would fail the rail
  CHECK(codeLinesWith(bad, "snippet", "tlsConfigProfile").size() == 1);
  CHECK(codeLinesWith(bad, "snippet", "\"http://").size() == 1);
  // The comment line was dropped: the first line names NONE and is not a hit.
  CHECK(codeLinesWith("// WALTER_MODEM_TLS_VALIDATION_NONE\n", "snippet", "WALTER_MODEM_TLS_VALIDATION_").empty());
}
