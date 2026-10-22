// The library's FakePlatform with the link's clock tied to it (task p4-02).
//
// The agent library's suites move time by calling `IPlatform::delayMs` (host_test/agent-core/
// test/support/FakePlatform.h: "that call *is* the passage of time"), and the link client, the
// scripted radio and the mesh client move it by calling `interfaces::Clock::delayMs`
// (host_test/fakes/fake_clock.h). When the runner's scenarios run through the framing, both
// clocks have to be one clock: a reply the simulated repeater delivers 400 ms after the send on
// the platform's clock must be 400 ms away on the link's too, or the Walter's own timer over the
// event means nothing. So this header shadows `fake::FakePlatform` with a subclass that carries a
// `fakes::FakeClock` and moves both whichever is asked - the platform's delay advances the link's
// clock, and the link's delay (through the clock's hook) advances the platform's.
//
// How the shadowing works: this directory comes first on the include path of the two binaries
// that run the library's scenarios through the link (host_test/CMakeLists.txt), so
// `<support/FakePlatform.h>` resolves here; `#include_next` then reads the library's own file
// with its class renamed to `FakePlatformUpstream`, and the subclass below takes the name every
// test and every other support header uses. Nothing under host_test/agent-core/ is edited.
#pragma once

#define FakePlatform FakePlatformUpstream
#include_next <support/FakePlatform.h>
#undef FakePlatform

#include "fakes/fake_clock.h"

namespace fake {

class FakePlatform : public FakePlatformUpstream {
 public:
  explicit FakePlatform(uint32_t seed = 0x5eed1234u) : FakePlatformUpstream(seed) {
    // The link's clock moving (the client's kPollMs slices, the mesh client's own timer) moves
    // the platform's: the simulated repeater's latency elapses while the Walter waits.
    linkClock.on_delay = [this](std::uint32_t ms) { FakePlatformUpstream::advance(ms); };
  }

  // The platform's clock moving (the runner's idle loop, the airtime budget's gap) moves the
  // link's: the RAK's 120 s job timeout and the event's delay are measured on it.
  void delayMs(uint32_t ms) override {
    FakePlatformUpstream::delayMs(ms);
    linkClock.advance(ms == 0 ? 1 : ms);
  }
  // And a test moving the platform's clock by hand moves the link's with it.
  void advance(uint32_t ms) {
    FakePlatformUpstream::advance(ms);
    linkClock.advance(ms);
  }
  void advanceSeconds(uint32_t s) { advance(s * 1000u); }

  // What the link runs on: the scripted radio, the port, the client and LinkMesh.
  fakes::FakeClock linkClock;
};

}  // namespace fake
