// The console with an uplink under it (task p3-01): the same rig
// host_test/core/console/test_console.cpp uses - fake pins that check the pin rules, a clock
// whose delays take no time, a scripted radio on the UART - plus a fake modem and the policy
// over it, and nothing else: no store, no journal, no update machine, no supervisor. A build
// without those is a build the console already has to survive (main/main.cpp hands over null
// pointers when a partition is missing), and the five `lte` lines are about none of them.
//
// It is in a header because two tests need it: the five commands themselves
// (test_console_lte.cpp) and the privacy test (test_uplink_privacy.cpp), which greps everything
// this console ever printed for the numbers that identify a subscription.
#pragma once

#include <doctest.h>

#include <memory>
#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/uplink/policy.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/fake_modem.h"
#include "sim/scripted_radio.h"

namespace bench {

struct LteConsole {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::board::RadioPins radio{pins, clock, retained};
  core::health::Health health{retained, clock};
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, clock, wall, slots};

  sim::FakeModem modem{clock};
  core::uplink::Settings settings{};
  // Pointers: the settings have to be settable before the policy is built over them, and the
  // console before the policy exists would have nothing to be given.
  std::unique_ptr<core::uplink::UplinkPolicy> policy;
  std::unique_ptr<core::console::Console> console;

  // Everything the console ever printed through this rig, run() by run(), for the privacy test.
  std::string everything;

  LteConsole() {
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    build();
  }

  // (Re)build the policy and the console over the settings as they are now.
  void build() {
    policy.reset(new core::uplink::UplinkPolicy(modem, clock, retained, settings));
    policy->begin();
    console.reset(new core::console::Console(out, radio, health, clock, restart, link, slots,
                                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             policy.get()));
  }

  // A console with no uplink at all, as a build whose main/ handed over nothing would have.
  void withoutUplink() {
    console.reset(new core::console::Console(out, radio, health, clock, restart, link, slots));
  }

  std::string run(const char* line) {
    out.clear();
    console->runLine(line);
    everything += out.text;
    return out.text;
  }
};

inline bool has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

}  // namespace bench
