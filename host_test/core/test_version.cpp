// The version string (task p0-01): what the console prints first and what the bench reads.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/version.h"

TEST_CASE("the banner is the product, a space, and the version the build gave") {
  const std::string banner = core::banner();
  const std::string version = core::version();

  CHECK(banner == std::string(core::kProduct) + " " + version);
  CHECK(std::strcmp(core::kProduct, "twinoak-walter") == 0);
  // The macro the build passes is what comes out - here the host build's (CMakeLists.txt).
  CHECK(version == TWINOAK_VERSION);
}

TEST_CASE("the version is never empty and the banner never ends in a space") {
  // A build that forgot PROJECT_VER gets "0.0.0-dev" (version.cpp), never "".
  CHECK(std::strlen(core::version()) > 0);
  const std::string banner = core::banner();
  CHECK(banner.back() != ' ');
  // One line on the console: no newline inside it, and a terminator the console can print.
  CHECK(banner.find('\n') == std::string::npos);
}
