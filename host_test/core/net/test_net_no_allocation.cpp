// No allocation under core/net/ and in the fetch (task p3-02), as a test: every source read line
// by line for a word that allocates or an include that could bring one in
// (host_test/checks/no_allocation.h). The client holds two 1.7 KB URLs, the transport a 2.5 KB
// request head and a 1.5 KB receive buffer, the fetch an 8 KB manifest: all of it static storage,
// none of it the heap, and none of it the main task's 3.5 KB stack.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kNetDirectories[] = {
    "components/core/net",
    "components/core/include/core/net",
};

// What core/net/ may include: the freestanding headers, <cstring> and <cstdio> (snprintf into a
// stack buffer: no allocation), its own headers, SHA-256 for the roots' record, the version for
// the User-Agent, and the interfaces it needs of the world - a clock, retained storage, a TLS
// socket, HTTP, bytes out - each pure virtual. No ESP-IDF, no WalterModem: that is
// tools/cmake/no_idf_include.cmake's and tools/verify.sh's business too.
const char* const kNetIncludes[] = {
    "cstddef",
    "cstdint",
    "cstring",
    "cstdio",
    "core/image/crypto/sha256.h",
    "core/net/client.h",
    "core/net/download.h",
    "core/net/http1.h",
    "core/net/http_over_tls.h",
    "core/net/trust.h",
    "core/net/url.h",
    "core/version.h",
    "interfaces/clock.h",
    "interfaces/http.h",
    "interfaces/retained.h",
    "interfaces/text_out.h",
    "interfaces/tls_socket.h",
    "tls_roots.h",  // generated from keys/tls/*.pem (tools/gen_tls_roots.py)
};

}  // namespace

TEST_CASE("nothing under components/core/net/ allocates") {
  checks::checkNoAllocation(
      "the HTTPS client",
      checks::filesUnder(kNetDirectories, sizeof kNetDirectories / sizeof kNetDirectories[0]),
      kNetIncludes, sizeof kNetIncludes / sizeof kNetIncludes[0], 12, 1500);
}
