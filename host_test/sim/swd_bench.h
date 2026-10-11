// A Walter and a simulated SWD target on a bench of fakes (task p1-03): the fake pins that check
// the pin rules on every transition, the target standing in front of them watching the two SWD
// lines, a clock whose delays take no time, RadioPins over the target, and the wire and the
// debug port under test. Every test of core/swd/ starts from one of these.
#pragma once

#include "core/board/radio_pins.h"
#include "core/swd/dp.h"
#include "core/swd/wire.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "sim/swd_target.h"

namespace sim {

struct SwdBench {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  SwdTarget target{pins};
  fakes::FakeRetained retained;
  core::board::RadioPins board{target, clock, retained};
  core::swd::Wire wire{board.swd(), clock};
  core::swd::Dp dp{wire};

  SwdBench() {
    // The rail is "unknown" for the settle time after boot (ARCHITECTURE section 2 rule 3): let it
    // pass, as the firmware does by the time a console is open.
    clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
  }

  core::board::Outcome acquire() { return board.swdAcquire(); }
  void release() { board.swdRelease(); }
};

}  // namespace sim
