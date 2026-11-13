// No allocation in the duty cycle (task p3-04) or the wake cycle (task p3-05), and a list of
// what they are allowed to know about: every source under `core/power/` and `core/cycle/` read
// line by line for a word that allocates or an include that could bring one in
// (host_test/checks/no_allocation.h).
//
// The include list is the second half of this and the more interesting one. The duty cycle is
// the thing that runs *last* in a wake, with the least battery left, so a heap is the last thing
// it should want; and it is also the thing most tempted to reach for everything else in the
// firmware, so what it may include is written down here and a tenth header is a failed test.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

// The pure half: the band, the interval and what a wake may do. It knows nothing but the
// freestanding headers - not even an interface - because it keeps nothing across a sleep.
const char* const kPowerDirectories[] = {
    "components/core/power",
    "components/core/include/core/power",
};

const char* const kPowerIncludes[] = {
    "cstddef",
    "cstdint",
    "core/power/interval_policy.h",
};

// The rest: what a wake means, the bound on one, the way out of one, and the object that
// composes the four.
const char* const kCycleDirectories[] = {
    "components/core/cycle",
    "components/core/include/core/cycle",
};

const char* const kCycleIncludes[] = {
    "cstddef",
    "cstdint",
    // The wake record is one formatted line and the journal's payload is bytes (task p3-05):
    // <cstdio> for snprintf, <cstdarg> for the one variadic line printer, <cstring> for strlen.
    // None of the three allocates; the words that do are what this test is really about.
    "cstdarg",
    "cstdio",
    "cstring",
    // Its own, and the adapter.
    "core/cycle/attention_source.h",
    "core/cycle/duty_cycle.h",
    "core/cycle/network_clock.h",
    "core/cycle/null_backend.h",
    "core/cycle/sleep.h",
    "core/cycle/telemetry_ring.h",
    "core/cycle/wake_counters.h",
    "core/cycle/wake_cycle.h",
    "core/cycle/wake_guard.h",
    "core/cycle/wake_reason.h",
    // The pure policy.
    "core/power/interval_policy.h",
    // Why this boot happened, and §8's attention: the board, the link's words, the supervisor
    // (the one place an EV_ATTENTION is decoded), the settings' two wake keys.
    "core/board/radio_pins.h",
    "core/health/health.h",
    "core/link/client/client.h",
    "core/link/codec/wire.h",
    "core/settings/settings.h",
    "core/supervisor/supervisor.h",
    // What a sleep has to put down before it goes: the modem, the two update machines, and the
    // journal it writes a line in.
    "core/update/journal.h",
    "core/update/radio_update.h",
    "core/update/self_update.h",
    "core/uplink/policy.h",
    // And what one wake runs besides them (task p3-05): the store it reports and shortens a
    // sleep for, the fetch of a release, the window a network time is believed inside, and this
    // firmware's own version and descriptor.
    "core/image/store.h",
    "core/image/this_image.h",
    "core/link/client/words.h",
    "core/net/download.h",
    "core/update/fetch.h",
    "core/uplink/clock_window.h",
    "core/version.h",
    // The seam, which is plain data and pure virtual like every other interface.
    "interfaces/backend.h",
    "interfaces/text_out.h",
    "interfaces/uplink.h",
    // The world, each pure virtual. **No ESP-IDF**: that is tools/cmake/no_idf_include.cmake's
    // business as well, and this is the second lock on the same door.
    "interfaces/clock.h",
    "interfaces/key_value.h",
    "interfaces/restart.h",
    "interfaces/retained.h",
    "interfaces/sleeper.h",
    "interfaces/wall_clock.h",
};

}  // namespace

TEST_CASE("nothing under components/core/power/ allocates") {
  checks::checkNoAllocation(
      "the interval policy",
      checks::filesUnder(kPowerDirectories,
                         sizeof kPowerDirectories / sizeof kPowerDirectories[0]),
      kPowerIncludes, sizeof kPowerIncludes / sizeof kPowerIncludes[0], 2, 400);
}

TEST_CASE("nothing under components/core/cycle/ allocates") {
  checks::checkNoAllocation(
      "the duty cycle",
      checks::filesUnder(kCycleDirectories,
                         sizeof kCycleDirectories / sizeof kCycleDirectories[0]),
      kCycleIncludes, sizeof kCycleIncludes / sizeof kCycleIncludes[0], 14, 2500);
}

TEST_CASE("nothing in interfaces/ allocates, and it includes only the freestanding headers") {
  // The seam the whole of core/ is written against, this task's `interfaces/sleeper.h` included:
  // every one of them is pure virtual and plain data, and the only headers any of them needs are
  // <cstddef> and <cstdint>. A new interface that reached for anything else would be a seam with
  // the world on the wrong side of it.
  const char* const kDirectories[] = {"components/interfaces/include/interfaces"};
  const char* const kIncludes[] = {"cstddef", "cstdint"};
  checks::checkNoAllocation("the interfaces", checks::filesUnder(kDirectories, 1), kIncludes,
                            sizeof kIncludes / sizeof kIncludes[0], 10, 600);
}
