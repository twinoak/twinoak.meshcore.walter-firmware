// The codec allocates nothing - task p1-01's third acceptance criterion, as a test and not a
// promise - and neither does the client above it (task p1-02: core/ has no heap, and a client
// whose buffers are members is one a reader can size). The check itself is checks/no_allocation.h
// (task p2-01 moved it there, unchanged, so that the image checks could share it). Each part has
// two directories - its sources and its headers - because `core/` keeps its public headers under
// `include/core/`.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kCodecDirectories[] = {
    "components/core/link/codec",
    "components/core/include/core/link/codec",
};

const char* const kClientDirectories[] = {
    "components/core/link/client",
    "components/core/include/core/link/client",
};

// What the codec may include: the two freestanding headers it needs, its own, and the one
// interface the frame writer takes (a clock, for §2's idle 0x00).
const char* const kCodecIncludes[] = {
    "cstddef",
    "cstdint",
    "interfaces/clock.h",
    "core/link/codec/cobs.h",
    "core/link/codec/crc16.h",
    "core/link/codec/frame.h",
    "core/link/codec/messages.h",
    "core/link/codec/packet.h",
    "core/link/codec/reader.h",
    "core/link/codec/view.h",
    "core/link/codec/wire.h",
    "core/link/codec/writer.h",
};

// What the client may include: the codec's public headers, the four interfaces it is written
// against, the board object that owns its two pins, its own headers, and the version string it
// puts in a HELLO. Nothing of ESP-IDF, nothing of the standard library but the two freestanding
// headers (tools/cmake/no_idf_include.cmake checks the first half of that for all of core/).
const char* const kClientIncludes[] = {
    "cstddef",
    "cstdint",
    "core/board/radio_pins.h",
    "core/link/client/client.h",
    "core/link/client/port.h",
    "core/link/client/words.h",
    "core/link/codec/frame.h",
    "core/link/codec/messages.h",
    "core/link/codec/packet.h",
    "core/link/codec/view.h",
    "core/link/codec/wire.h",
    "core/version.h",
    "interfaces/byte_stream.h",
    "interfaces/clock.h",
    "interfaces/wall_clock.h",
};

}  // namespace

TEST_CASE("nothing under components/core/link/codec/ allocates") {
  // The codec is about a thousand lines in thirteen files (task p1-01).
  checks::checkNoAllocation("the codec",
                            checks::filesUnder(kCodecDirectories,
                                               sizeof kCodecDirectories / sizeof kCodecDirectories[0]),
                            kCodecIncludes, sizeof kCodecIncludes / sizeof kCodecIncludes[0], 10, 500);
}

TEST_CASE("nothing under components/core/link/client/ allocates") {
  // The client is six files: the port, the client, the words and their headers (task p1-02).
  checks::checkNoAllocation("the client",
                            checks::filesUnder(kClientDirectories,
                                               sizeof kClientDirectories / sizeof kClientDirectories[0]),
                            kClientIncludes, sizeof kClientIncludes / sizeof kClientIncludes[0], 6, 500);
}
