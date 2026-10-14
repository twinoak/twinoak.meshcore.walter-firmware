// No allocation in the radio's update or its journal (task p2-03), as a test: every source under
// core/update/ - the journal, the machine, the battery over the link - read line by line for a
// word that allocates or an include that could bring one in (host_test/checks/no_allocation.h,
// task p1-01's check). The journal is one 64-byte buffer and a few words of state; the machine's
// buffers (a record, the store's view of an image) are members; the console holds the story's
// sixteen entries in static storage. That stays true here.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kUpdateDirectories[] = {
    "components/core/update",
    "components/core/include/core/update",
};

// What core/update/ may include: the freestanding headers, its own headers, the slot, the
// flasher and its board profile, the store and the descriptor (the record's copy of it is what
// the health check holds HELLO against), SHA-256's size, the link client and the protocol's
// numbers, and the two interfaces the journal and the machine need of the world - a flash
// region and a clock, both pure virtual.
const char* const kUpdateIncludes[] = {
    "cstddef",
    "cstdint",
    "core/board/slot.h",
    "core/flasher/board_profile.h",
    "core/flasher/flasher.h",
    "core/image/crypto/sha256.h",
    "core/image/descriptor.h",
    "core/image/store.h",
    "core/link/client/client.h",
    "core/link/codec/wire.h",
    "core/update/battery.h",
    "core/update/journal.h",
    "core/update/last_good_battery.h",
    "core/update/link_battery.h",
    "core/update/radio_update.h",
    "core/update/self_update.h",
    "core/update/uplink_check.h",
    "interfaces/clock.h",
    "interfaces/flash_region.h",
    // The Walter's own update (task p2-04): this image's descriptor and the build's version
    // (what an offered image must be), health for the restart's reason, the verdict's token and
    // the transfer target, the slots, a restart.
    "core/health/health.h",
    "core/image/receive.h",
    "core/image/this_image.h",
    "core/image/verdict.h",
    "core/version.h",
    "interfaces/own_update.h",
    "interfaces/restart.h",
    // The fetch of a release and the trial's TLS handshake (task p3-02): the client, the download
    // and the roots, the URL's bound, bytes out for the console's lines (snprintf into a stack
    // buffer and the va_list it takes: no allocation), the TLS socket.
    "cstdarg",
    "cstdio",
    "cstring",
    "core/net/client.h",
    "core/net/download.h",
    "core/net/trust.h",
    "core/net/url.h",
    "core/update/fetch.h",
    "core/update/tls_check.h",
    "interfaces/text_out.h",
    "interfaces/tls_socket.h",
};

}  // namespace

TEST_CASE("nothing under components/core/update/ allocates") {
  // Twelve files: the journal, the radio's machine, the battery over the link, its last good
  // reading (task p2-05) and the Walter's own update, each a header and a source, and the
  // battery and uplink interfaces.
  checks::checkNoAllocation("the updates",
                            checks::filesUnder(kUpdateDirectories,
                                               sizeof kUpdateDirectories / sizeof kUpdateDirectories[0]),
                            kUpdateIncludes, sizeof kUpdateIncludes / sizeof kUpdateIncludes[0], 12, 1900);
}
