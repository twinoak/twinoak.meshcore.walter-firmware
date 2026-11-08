// No allocation under core/trace/ (task p10-06), as a test: every source read line by line for a
// word that allocates or an include that could bring one in (host_test/checks/no_allocation.h).
//
// It matters more here than in most places. The redactor runs **inside a log hook**, on whichever
// task logged the line - the modem library's command task for every `TX:` and `RX:` - and a hook
// that took the heap would take it under whatever lock the caller happened to hold. Everything it
// needs is the caller's buffer and its own two copies of a secret, both bounded by a setting's
// longest value; the one allocation of the whole feature is the 4 KB line buffer, made once by
// `components/modem/modem_trace.cpp` when the trace is first switched on and never freed.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kTraceDirectories[] = {
    "components/core/trace",
    "components/core/include/core/trace",
};

// What core/trace/ may include: the freestanding headers, <cstring> (the search and the blanking),
// <cstdio> and <cstdarg> (vsnprintf into the caller's buffer, which is the hook's own body: no
// allocation), and the settings table, for the longest value a secret can be.
const char* const kTraceIncludes[] = {
    "cstdarg",
    "cstddef",
    "cstdio",
    "cstring",
    "core/settings/keys.h",
    "core/trace/redact.h",
};

}  // namespace

TEST_CASE("nothing under components/core/trace/ allocates") {
  checks::checkNoAllocation(
      "the trace's redaction",
      checks::filesUnder(kTraceDirectories, sizeof kTraceDirectories / sizeof kTraceDirectories[0]),
      kTraceIncludes, sizeof kTraceIncludes / sizeof kTraceIncludes[0], 2, 150);
}
