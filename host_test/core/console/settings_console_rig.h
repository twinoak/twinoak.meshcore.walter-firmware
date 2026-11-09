// The whole console over everything a setting can reach (task p3-03): fake pins that check the
// pin rules, a clock whose delays take no time, a scripted radio on the UART, a settings store
// over a map, a fake modem with the uplink policy over it, an image store over a simulated
// flash, a journal over another, a supervisor, and the seam a self-update's trial reaches for.
//
// It is the rig for five tests: the settings commands themselves (test_console_settings.cpp),
// the secrets (test_console_secrets.cpp), the grammar held to the table and the document
// (test_console_grammar.cpp), the provisioning session of docs/provisioning.md
// (test_provisioning_doc.cpp) and the heap's four figures (test_console_heap.cpp). Everything the console has ever printed through it is kept in
// `everything`, which is what the secret test greps.
#pragma once

#include <doctest.h>

#include <memory>
#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/cycle/duty_cycle.h"
#include "core/health/health.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/trust.h"
#include "core/image/verdict.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/journal.h"
#include "core/update/link_battery.h"
#include "core/update/uplink_check.h"
#include "core/uplink/policy.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_heap_report.h"
#include "fakes/fake_modem_trace.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_sleeper.h"
#include "fakes/fake_wall_clock.h"
#include "fakes/sim_key_value.h"
#include "sim/fake_modem.h"
#include "sim/scripted_radio.h"
#include "sim/sim_flash.h"

namespace bench {

struct SettingsConsole {
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

  // The settings, over a map a test can corrupt, make read-only or take away.
  fakes::SimKeyValue kv;
  core::settings::Settings settings{kv};

  // The modem, so that `lte.apn`, `lte.pin` and `lte.psm` reach something.
  sim::FakeModem modem{clock};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{modem, clock, retained, modem_settings};

  // The server a self-update's trial must reach: the seam `server.url` sets. The unconfigured
  // one, because what is tested here is that the host it is given is the host it reports -
  // reaching one needs a modem (core/update/tls_check.h).
  core::update::UnconfiguredUplink server;

  // The store and the journal, so that `erase confirm` can be shown to leave them alone and
  // `journal` and `status` have something to print.
  sim::SimFlash store_flash{core::image::kStoreBytes};
  core::image::Store store{store_flash, wall};
  // The verdict and the USB transfer: the console's `image ...` commands answer in one line
  // without a transfer, and what is tested here is the commands, not the absence of one.
  core::image::ImageVerdict verdict{core::image::buildTrust(), core::image::thisUnit()};
  core::image::Receiver receiver{store, verdict, out, clock};
  sim::SimFlash journal_flash{core::update::kJournalPartitionBytes};
  core::update::Journal journal{journal_flash};
  core::update::LinkBattery battery{link};
  // The modem library's trace behind its seam (task p10-06): `trace` and `trace modem on|off`
  // reach something here, so that the test which runs every command of the table runs those two
  // as well - and so that the secrets test can switch the trace on with both secrets set and
  // grep the transcript for them.
  fakes::FakeModemTrace trace;
  // The heap behind its seam (task p11-02): `heap` reaches four numbers here, so that the test
  // which runs every command of the table runs it as well.
  fakes::FakeHeapReport heap_report;
  // The duty cycle (task p3-04), so that `wake`, `power`, `sleep` and `stay` reach something and
  // so that `status`'s last two lines are the ones a real unit prints - docs/provisioning.md is
  // replayed against this console, and a document that showed a line no unit prints would be
  // worse than no document.
  fakes::FakeSleeper sleeper{clock, pins};
  std::unique_ptr<core::supervisor::Supervisor> supervisor;
  std::unique_ptr<core::cycle::DutyCycle> duty;
  std::unique_ptr<core::console::Console> console;

  // Everything this console has ever printed, run by run.
  std::string everything;

  SettingsConsole() {
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    pins.record_log = false;
    REQUIRE(store.begin() == core::image::StoreError::kNone);
    REQUIRE(journal.begin() == core::update::JournalError::kNone);
    uplink.begin();
    supervisor.reset(new core::supervisor::Supervisor(link, radio, slots, health, battery, clock,
                                                      wall, retained, &store, nullptr, nullptr,
                                                      &journal));
    supervisor->begin();
    duty.reset(new core::cycle::DutyCycle(sleeper, radio, link, uplink, health, clock, wall,
                                          restart, retained, supervisor.get(), nullptr, nullptr,
                                          &journal, &settings));
    duty->begin();
    build();
  }
  ~SettingsConsole() { CHECK(pins.violations.empty()); }

  // The console over the settings as they are now. begin() is main's first call and is what puts
  // a setting in force, so a test that wants a reboot calls this again.
  void build() {
    console.reset(new core::console::Console(out, radio, health, clock, restart, link, slots,
                                             &store, &receiver, nullptr, &journal, nullptr,
                                             supervisor.get(), &uplink, nullptr, nullptr, nullptr,
                                             &settings, &server, duty.get(),
                                             // cycle, battery, mesh, agent, identity, loop,
                                             // sockets, wall: none of them here.
                                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, &trace, &heap_report));
  }

  // A console that was handed no heap seam, as a build whose main/ gave it none has (task
  // p11-02): everything else is as `build()` leaves it, so what the test sees is the one line
  // `heap` says instead of four figures.
  void withoutHeap() {
    console.reset(new core::console::Console(out, radio, health, clock, restart, link, slots,
                                             &store, &receiver, nullptr, &journal, nullptr,
                                             supervisor.get(), &uplink, nullptr, nullptr, nullptr,
                                             &settings, &server, duty.get(),
                                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, &trace));
  }

  // A console with no settings store at all, as a build whose `nvs` partition would not open has.
  void withoutSettings() {
    console.reset(new core::console::Console(out, radio, health, clock, restart, link, slots,
                                             &store, &receiver, nullptr, &journal, nullptr,
                                             supervisor.get(), &uplink));
  }

  std::string begin() {
    out.clear();
    console->begin();
    everything += out.text;
    return out.text;
  }

  std::string run(const char* line) {
    out.clear();
    console->runLine(line);
    everything += out.text;
    return out.text;
  }

  // As a person types it, one character at a time: what the port echoed and then printed.
  std::string type(const char* text) {
    out.clear();
    for (const char* p = text; *p != 0; ++p) console->feed(*p);
    everything += out.text;
    return out.text;
  }
};

// Named for this rig alone: two other benches define a `bench::has` of their own, and a test
// that pulled in both would be the same function defined twice.
inline bool hasText(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

inline std::size_t countText(const std::string& text, const char* what) {
  std::size_t n = 0;
  for (std::size_t at = text.find(what); at != std::string::npos;
       at = text.find(what, at + 1)) {
    ++n;
  }
  return n;
}

}  // namespace bench
