// The wake cycle on a bench of simulators (task p3-05), built the way task p3-04's duty-cycle
// bench is and with everything a whole wake touches on it: one **world** that outlives a Walter -
// the clock, the retained map that is RTC memory, the journal's flash, the image store's flash,
// the two application slots, the scripted radio, the fake modem, the scripted HTTPS server - and
// a **Walter** over it that is destroyed and built again, which is what a wake from deep sleep
// *is*. `boot()` is a wake and `oneWake()` is a whole pass.
//
// What a test turns: which source woke the board, the reset reason, whether a USB host is there,
// what the radio answers POWER_GET and ENV_GET with, whether it answers at all, what the modem
// does, what the server serves - and **what the backend asks for**, which is `ScriptedBackend`
// below and is the other half of this task.
//
// No simulated nRF52 and no flasher here: a real flash is task p2-03's test and costs a fifth of
// a second of bit-level SWD simulation per run, which a simulated month of wakes cannot pay for.
// The radio's update machine is on the bench all the same - over a flasher whose pins nothing
// answers - so that its refusals, its journal and S-7 are exercised by the wake that drives it.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/cycle/duty_cycle.h"
#include "core/cycle/network_clock.h"
#include "core/cycle/null_backend.h"
#include "core/cycle/telemetry_ring.h"
#include "core/cycle/wake_cycle.h"
#include "core/flasher/flasher.h"
#include "core/health/health.h"
#include "core/image/store.h"
#include "core/image/store_bench.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/net/net_bench.h"
#include "core/power/interval_policy.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/fetch.h"
#include "core/update/journal.h"
#include "core/update/last_good_battery.h"
#include "core/update/link_battery.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "core/update/update_bench.h"
#include "core/uplink/policy.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_own_update.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_sleeper.h"
#include "fakes/sim_key_value.h"
#include "sim/fake_modem.h"
#include "sim/scripted_radio.h"
#include "sim/sim_flash.h"
#include "core/update/self_bench.h"

namespace bench {

// Where the network's clock starts, in Unix seconds: the fake modem's own default, which is a day
// after this firmware's floor (core/uplink/clock_window.h). It **runs** - see WakeWalter's
// constructor - because a network that answered the same epoch at every wake while the world
// moved would have the radio's clock drift ahead of the Walter's, and the Walter would set it
// again at every wake for ever.
constexpr std::uint32_t kNetworkEpoch = 1791936000u + 86400u;

namespace cyc = core::cycle;
namespace pwr = core::power;
namespace ifs = interfaces;

// A backend a test writes the answer of: what it was asked, and what it asks for in return. It is
// the **tests'** implementation of the seam; the firmware's does nothing
// (core/cycle/null_backend.h).
class ScriptedBackend final : public ifs::Backend {
 public:
  // The script.
  ifs::Exchanged answers = ifs::Exchanged::kNothing;
  ifs::BackendAnswer answer{};
  std::uint32_t cost_ms = 50;  // what an exchange costs, so the budget can be seen to be spent

  // What it saw. A copy and not a pointer: the firmware's structure is a member of the WakeCycle
  // and is filled again at the next wake.
  std::vector<ifs::BackendKnowledge> seen;
  int exchanges = 0;

  explicit ScriptedBackend(fakes::FakeClock& clock) : clock_(clock) {}

  ifs::Exchanged exchange(const ifs::BackendKnowledge& known, std::uint64_t deadline_ms,
                          ifs::BackendAnswer* out) override {
    ++exchanges;
    seen.push_back(known);
    // The deadline is the wake's, and an implementation that sat past it would be the failure the
    // budget exists for. This one spends what it was told to and no more.
    const std::uint64_t now = clock_.nowMs();
    const std::uint32_t spend = static_cast<std::uint32_t>(
        deadline_ms > now && cost_ms > deadline_ms - now ? deadline_ms - now : cost_ms);
    clock_.advance(spend);
    if (answers == ifs::Exchanged::kAsked && out != nullptr) *out = answer;
    return answers;
  }

  // The script, as a test writes it.
  void askFor(ifs::Ask ask) {
    answers = ifs::Exchanged::kAsked;
    answer.want(ask);
  }
  void takeReadings() {
    answers = ifs::Exchanged::kAsked;
    answer.took_readings = true;
  }
  void setSetting(const char* key, const char* value) {
    answers = ifs::Exchanged::kAsked;
    answer.want(ifs::Ask::kSetSettings);
    REQUIRE(answer.settings < ifs::kBackendSettingsMax);
    ifs::BackendSetting& s = answer.setting[answer.settings++];
    std::size_t i = 0;
    for (; i + 1 < sizeof s.key && key[i] != 0; ++i) s.key[i] = key[i];
    s.key[i] = 0;
    i = 0;
    for (; i + 1 < sizeof s.value && value[i] != 0; ++i) s.value[i] = value[i];
    s.value[i] = 0;
  }
  void forget() {
    answers = ifs::Exchanged::kNothing;
    answer = ifs::BackendAnswer{};
  }
  const ifs::BackendKnowledge& last() const {
    REQUIRE(!seen.empty());
    return seen.back();
  }

 private:
  fakes::FakeClock& clock_;
};

// The Walter's own monotonic clock: ESP-IDF's `esp_timer` restarts at zero at every wake from
// deep sleep (interfaces/clock.h), so a Walter's nowMs() is the world's minus the moment its own
// boot began. The same object task p3-04's bench has, for the same reason.
class WakeBootClock final : public interfaces::Clock {
 public:
  explicit WakeBootClock(fakes::FakeClock& world) : world_(world), base_(world.nowMs()) {}
  std::uint64_t nowMs() override { return world_.nowMs() - base_; }
  void delayMs(std::uint32_t ms) override { world_.delayMs(ms); }
  void delayUs(std::uint32_t us) override { world_.delayUs(us); }

 private:
  fakes::FakeClock& world_;
  const std::uint64_t base_;
};

struct WakeWorld {
  StoreBench images;  // the image store over a simulated flash, the test key's trust table
  fakes::FakeClock& clock = images.clock;
  fakes::FakeRetained retained;  // RTC memory: a sleep keeps it, a power cut does not
  sim::SimFlash journal_flash{core::update::kJournalPartitionBytes};
  fakes::FakeOwnUpdate own{64 * 1024};
  sim::ScriptedRadio rak{clock};
  sim::FakeModem modem{clock};
  sim::FakeHttp http{clock};
  fakes::SimKeyValue kv;
  ScriptedBackend backend{clock};

  // What the next boot looks like.
  interfaces::WokeBy woke = interfaces::WokeBy::kNotFromSleep;
  core::health::ResetReason reset = core::health::ResetReason::kPowerOn;
  bool usb = false;
  int boots = 0;
  bool restart_asked = false;

  WakeWorld() {
    rak.override_power = true;
    rak.battery_mv = 4100;  // `plenty` by default: a test that wants another band says so
    rak.battery_ma = 0;
    rak.fw_version = "0.3.0+golden0";
    rak.board = "rak4630-single";
    // The world settles whenever a Walter waits and whenever it touches the UART, so that the
    // radio's own timing - §8's repeat window, a boot after a reset - is a function of the clock
    // and not of a counter a test winds on.
    rak.on_access = [this] { settle(); };
    clock.on_delay = [this](std::uint32_t) { settle(); };
    // The store dates its records from its own wall clock; a unit that has never attached
    // stores a 0 there, which is a case of its own and not the one most tests want.
    images.wall.give(1760000000);
  }

  // Anything the world has to do as time passes. Nothing yet beyond the radio's own queue, which
  // ripens inside its own read(); the hook is here because the scripted radio expects one and
  // because a later test may want a modem that drops at a time.
  void settle() {}

  void powerCut() {
    retained.powerCut();
    rak.forgetPending();
  }
};

// One Walter over the world: everything main/ constructs, with the fakes under it.
struct WakeWalter {
  WakeWorld& w;
  WakeBootClock clock{w.clock};
  fakes::FakePins pins{w.clock};
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::board::RadioPins board{pins, clock, w.retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  cyc::NetworkClock wall{clock};
  core::link::client::LinkPort port{board, w.rak};
  core::link::client::Client link{port, clock, wall, slots};
  core::health::Health health{w.retained, clock};
  core::update::Journal journal{w.journal_flash};
  core::update::LinkBattery link_battery{link};
  core::update::LastGoodBattery battery{link_battery, clock};
  core::flasher::Flasher flasher{board, clock, slots};
  core::settings::Settings settings{w.kv};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{w.modem, clock, w.retained, modem_settings};
  fakes::FakeSleeper sleeper{w.clock, pins};
  FakeUplink server;  // the self-update trial's third condition (host_test/.../self_bench.h)
  core::net::Client http_client{w.http, clock};
  core::net::Download download{http_client, clock};
  core::update::Fetch fetch{http_client, download, w.images.store, w.images.verdict, clock,
                            nullptr};
  core::update::RadioUpdate update{w.images.store, flasher, link, journal, clock, battery};
  core::update::SelfUpdate self{w.own,   w.images.store, journal, link,
                                server, clock,          restart, battery,
                                health};
  cyc::TelemetryRing readings{w.retained};
  std::unique_ptr<core::supervisor::Supervisor> supervisor;
  std::unique_ptr<cyc::DutyCycle> duty;
  std::unique_ptr<cyc::WakeCycle> cycle;
  std::unique_ptr<core::console::Console> console;

  explicit WakeWalter(WakeWorld& world) : w(world) {
    ++w.boots;
    w.restart_asked = false;
    pins.record_log = false;
    w.journal_flash.powerOn();
    w.images.flash.powerOn();
    w.own.powerOn();
    REQUIRE(w.images.store.begin() == core::image::StoreError::kNone);
    // The modem is handed deadlines on *this* Walter's clock, which began now (sim/fake_modem.h).
    w.modem.walterBooted(w.clock.nowMs());
    // And the network's clock has been running while the Walter slept.
    w.modem.time_epoch = kNetworkEpoch + static_cast<std::uint32_t>(w.clock.nowMs() / 1000u);
    // main/'s own order: the pins safe, the holds released, the wake cause read, safe again.
    (void)board.releaseHolds();
    board.safe();
    sleeper.wake_as = w.woke;
    sleeper.usb = w.usb;
    health.begin(w.reset, w.woke == interfaces::WokeBy::kTimer
                              ? core::health::WakeCause::kTimer
                              : w.woke == interfaces::WokeBy::kRadioLine
                                    ? core::health::WakeCause::kPin
                                    : core::health::WakeCause::kNotFromSleep);
    w.clock.advance(core::board::kKillTiming.settle_ms);
    settings.begin();
    REQUIRE(journal.begin() == core::update::JournalError::kNone);
    uplink.begin();
    readings.begin();
    REQUIRE(port.open() == core::board::Outcome::kOk);
    supervisor.reset(new core::supervisor::Supervisor(link, board, slots, health, battery, clock,
                                                      wall, w.retained, &w.images.store, &flasher,
                                                      &update, &journal));
    supervisor->begin();
    duty.reset(new cyc::DutyCycle(sleeper, board, link, uplink, health, clock, wall, restart,
                                  w.retained, supervisor.get(), &update, &self, &journal,
                                  &settings));
    cycle.reset(new cyc::WakeCycle(*duty, link, uplink, w.backend, readings, wall, clock,
                                   supervisor.get(), &update, &self, &fetch, &w.images.store,
                                   &journal, &settings));
    console.reset(new core::console::Console(out, board, health, clock, restart, link, slots,
                                             &w.images.store, nullptr, &update, &journal, &self,
                                             supervisor.get(), &uplink, &http_client, &fetch,
                                             nullptr, &settings, &server, duty.get(),
                                             cycle.get()));
  }
  ~WakeWalter() { CHECK(pins.violations.empty()); }

  // What main/ does before the wake: work out why we are awake, arm the bound, one turn of the
  // loop so that the trial's first condition is met.
  void begin() {
    (void)self.begin();
    duty->begin();
    duty->guard().arm();
    self.mainLoopReached();
  }

  const cyc::Pass& run() { return cycle->run(); }
  std::string runLine(const char* line) {
    out.clear();
    console->runLine(line);
    return out.text;
  }
};

// A world and the Walter on it, with `boot()` for a wake.
struct WakeBench {
  WakeWorld world;
  std::unique_ptr<WakeWalter> walter;

  WakeBench() { walter = std::make_unique<WakeWalter>(world); }

  void boot(interfaces::WokeBy woke = interfaces::WokeBy::kTimer,
            core::health::ResetReason reset = core::health::ResetReason::kDeepSleepWake,
            core::health::Restart requested = core::health::Restart::kNotRequested) {
    if (walter && requested != core::health::Restart::kNotRequested) {
      walter->health.noteRestart(requested);
    }
    world.woke = woke;
    world.reset = reset;
    walter.reset();
    walter = std::make_unique<WakeWalter>(world);
  }

  WakeWalter& v() { return *walter; }
  cyc::WakeCycle& cycle() { return *walter->cycle; }
  cyc::DutyCycle& duty() { return *walter->duty; }

  // One whole wake, as a mast runs one: a boot, the wake worked out, the bound armed, one pass.
  const cyc::Pass& oneWake(interfaces::WokeBy woke = interfaces::WokeBy::kTimer) {
    boot(woke);
    walter->begin();
    return walter->run();
  }

  // The first wake of a unit that has just been powered up, which is a different thing: the
  // radio was reset by our own power-up and the supervisor knows it (Silence::kWalterPoweredUp).
  const cyc::Pass& firstWake() {
    world.woke = interfaces::WokeBy::kNotFromSleep;
    world.reset = core::health::ResetReason::kPowerOn;
    walter.reset();
    walter = std::make_unique<WakeWalter>(world);
    walter->begin();
    return walter->run();
  }

  // The modem works and the network knows the time: what most tests want of the uplink.
  void modemWorks() {
    world.modem.deaf = false;
    world.modem.never_registers = false;
    world.modem.time_answered = true;
  }
  // And the modem that is not there at all.
  void modemSilent() { world.modem.deaf = true; }

  // The journal's wake records, newest first.
  std::vector<cyc::WakePayload> wakeEntries() {
    std::vector<cyc::WakePayload> found;
    core::update::Cursor cursor;
    core::update::Entry e;
    bool more = walter->journal.first(&cursor, &e);
    while (more && found.size() < 1024) {  // the ring's whole length: never a cap that lies
      cyc::WakePayload p;
      if (e.kind == core::update::kind::kWakeCycle && cyc::decode(e, &p)) found.push_back(p);
      more = walter->journal.previous(&cursor, &e);
    }
    return found;
  }

  // Every request type the radio was asked, over the whole life of the world.
  std::vector<std::uint8_t> requestTypes() const {
    std::vector<std::uint8_t> types;
    for (const sim::ScriptedRadio::Seen& r : world.rak.requests) types.push_back(r.type);
    return types;
  }
};

}  // namespace bench
