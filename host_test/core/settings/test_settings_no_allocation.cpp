// No allocation in the settings or in the console's log ring (task p3-03), as a test: every
// source read line by line for a word that allocates or an include that could bring one in
// (host_test/checks/no_allocation.h, task p1-01's check).
//
// One Settings is eleven fixed buffers and a few words in static storage, which is what main/
// gives it; one LogRing is 4 KB of char. Both are read and written in a wake that has the least
// battery to spare, and the settings are read before anything else at boot - a heap is the last
// thing either should want.
//
// Two things are deliberately not checked this way. The **console** has used `printf` and a
// `va_list` since task p0-03; what keeps it honest is that every buffer it owns is a fixed
// member - its 12 KB of flasher pages and the 4 KB ring among them - and that main/ gives it
// static storage. The **log ring** says `= delete` to make its Quiet guard non-copyable, and
// `delete` is on the check's word list; it is one `char[4096]` member and fifty lines of index
// arithmetic, with no include but its own header and `interfaces/text_out.h`.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kSettingsDirectories[] = {
    "components/core/settings",
    "components/core/include/core/settings",
};

// What core/settings/ may include: the freestanding headers, its own two, the key-value store it
// is written against, the URL parser that takes `server.url` apart, the SWD wire whose default
// `swd.khz` is said in kHz, and the uplink policy whose own default `lte.psm`'s fallback is held
// to by a static_assert. No ESP-IDF, which is tools/cmake/no_idf_include.cmake's business as
// well, and this is the second lock on that door.
const char* const kSettingsIncludes[] = {
    "cstddef",
    "cstdint",
    "cstdio",
    "cstring",
    "core/net/url.h",
    "core/settings/keys.h",
    "core/settings/settings.h",
    "core/swd/wire.h",
    "core/uplink/policy.h",
    "interfaces/key_value.h",
};

}  // namespace

TEST_CASE("nothing under components/core/settings/ allocates") {
  checks::checkNoAllocation(
      "the settings",
      checks::filesUnder(kSettingsDirectories,
                         sizeof kSettingsDirectories / sizeof kSettingsDirectories[0]),
      kSettingsIncludes, sizeof kSettingsIncludes / sizeof kSettingsIncludes[0], 4, 600);
}

