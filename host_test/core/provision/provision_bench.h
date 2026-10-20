// `rak provision` on a bench (task p5-03): task p2-05's world - the store's flash, the journal's
// flash, the simulated nRF52840, the scripted radio, RTC memory, one clock - with the chip in
// one of its factory states (task p5-02: RUI3 in flash, both kinds of APPROTECT) and the base in
// the store (task p5-01), and a Walter over it that has everything main/ constructs: the console
// with the sequence inside it, the supervisor, the update machine, the duty cycle and the wake
// cycle - so that a provisioned radio can be left alone across a simulated week of wakes and then
// given its first application, on one bench.
//
// A Walter is destroyed and built again for a restart, a power cut or a wake, as the other
// benches do it; the world outlives it.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/console/console.h"
#include "core/cycle/duty_cycle.h"
#include "core/cycle/network_clock.h"
#include "core/cycle/null_backend.h"
#include "core/cycle/telemetry_ring.h"
#include "core/cycle/wake_cycle.h"
#include "core/image/base.h"
#include "core/provision/provision.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/supervisor/supervisor_bench.h"
#include "core/update/last_good_battery.h"
#include "core/update/link_battery.h"
#include "core/uplink/policy.h"
#include "fakes/fake_sleeper.h"
#include "fakes/sim_key_value.h"
#include "sim/fake_modem.h"

namespace bench {

namespace prov = core::provision;

// The four factory states of task p5-02's simulated chip, by INFO.VARIANT and UICR.APPROTECT.
enum class Factory { kLegacyOpen, kLegacyLocked, kImprovedOpen, kImprovedLocked };

constexpr std::uint32_t kVariantLegacy = 0x41414330;    // "AAC0": build code C, hardware-only APPROTECT
constexpr std::uint32_t kVariantImproved = 0x41414630;  // "AAF0": build code F, revision 3
constexpr std::uint32_t kVariantUnknown = 0x41414530;   // "AAE0": not in the table
// The simulated chip's DEVICEID as `rak swd id` prints it: %08x%08x of DEVICEID[1], DEVICEID[0].
constexpr const char* kDeviceId = "0badf00ddeadbeef";
constexpr const char* kDeviceIdUpper = "0BADF00DDEADBEEF";
constexpr const char* kOtherDeviceId = "0badf00ddeadbeee";
// The scripted radio's IDENTITY (spec/fixtures/link/link-v1.json): its first 16 hex digits.
constexpr const char* kFixtureKey16 = "7F3A9C02E1B44D9A";

inline bool isLocked(Factory f) { return f == Factory::kLegacyLocked || f == Factory::kImprovedLocked; }
inline bool isImproved(Factory f) { return f == Factory::kImprovedOpen || f == Factory::kImprovedLocked; }
inline const char* describe(Factory f) {
  switch (f) {
    case Factory::kLegacyOpen: return "legacy, open";
    case Factory::kLegacyLocked: return "legacy, locked by its firmware";
    case Factory::kImprovedOpen: return "improved, open";
    case Factory::kImprovedLocked: return "improved, locked by default";
  }
  return "?";
}

struct ProvWorld {
  SupWorld s;
  const core::flasher::BoardProfile& profile = core::flasher::kRak4630Single;

  sim::Nrf52Chip& chip() { return s.u.chip; }
  fakes::FakeClock& clock() { return s.u.clock; }

  // The chip as RAK ships it, in one of the four states. `allow` arms it for the door (a test
  // about the sequence); a test that shows the tripwire holding leaves it armed as it is. The
  // windows declared are the base's (rail R4): below the application region, and from the
  // bootloader up.
  void factory(Factory f, bool allow = true) {
    sim::Nrf52Chip& c = chip();
    c.info_variant = isImproved(f) ? kVariantImproved : kVariantLegacy;
    if (isLocked(f)) {
      c.factoryLocked();
    } else {
      c.factoryOpen();
    }
    if (allow) c.allowProvisioning();
    c.onlyTouch(0, profile.app.begin);
    c.alsoTouch(profile.bootloader_start, c.flashBytes());
    s.u.radioBoots();  // RUI3 has no descriptor of ours: the radio is silent on the link
  }
  // A chip that will be provisioned *and* given its first application: the whole array is
  // honestly the window (the base's two spans and the application region between them).
  void wholeArray() {
    chip().onlyTouch(0, chip().flashBytes());
    chip().alsoTouch(0, 0);
  }
  void putBase(const char* name = "base-small") {
    REQUIRE(s.u.store.putBase(Slot{0}, BaseFile(name)) == StoreError::kNone);
  }
  void putCandidate(const char* name = "update-candidate") {
    REQUIRE(s.u.store.put(Which::kCandidate, Slot{0}, Candidate(name)) == StoreError::kNone);
  }
  // An application that answers on the link, on a chip whose port is closed (a locked radio
  // with an identity, which S-5a names before it is erased): the scripted radio answers as its
  // fixtures say, whatever the chip's flash holds.
  void applicationAnswers() {
    s.u.radio.deaf = false;
    s.u.radio_runs = true;
  }
};

// One life of the Walter: everything main/ constructs, with the fakes under it.
struct ProvWalter {
  SupWorld& s;
  UpdateWorld& w;
  fakes::FakePins pins{s.u.clock};
  sim::SwdTarget target{pins};
  core::board::RadioPins board{target, s.u.clock, s.retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::flasher::Flasher flasher{board, s.u.clock, slots};
  core::cycle::NetworkClock wall{s.u.clock};
  core::link::client::LinkPort port{board, s.u.radio};
  core::link::client::Client client{port, s.u.clock, wall, slots};
  upd::Journal journal{s.u.journal_flash};
  upd::LinkBattery link_battery{client};
  upd::LastGoodBattery battery{link_battery, s.u.clock};
  upd::RadioUpdate update{s.u.store.store, flasher, client, journal, s.u.clock, battery};
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::health::Health health{s.retained, s.u.clock};
  core::image::Receiver receiver{s.u.store.store, s.u.store.verdict, out, s.u.clock};
  sup::Supervisor supervisor{client, board, slots, health, battery, s.u.clock, wall, s.retained,
                             &s.u.store.store, &flasher, &update, &journal};
  fakes::SimKeyValue kv;
  core::settings::Settings settings{kv};
  sim::FakeModem modem{s.u.clock};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{modem, s.u.clock, s.retained, modem_settings};
  fakes::FakeSleeper sleeper{s.u.clock, pins};
  core::cycle::NullBackend backend;
  core::cycle::TelemetryRing readings{s.retained};
  core::cycle::DutyCycle duty{sleeper, board, client, uplink, health, s.u.clock, wall, restart,
                              s.retained, &supervisor, &update, nullptr, &journal, &settings};
  core::cycle::WakeCycle cycle{duty, client, uplink, backend, readings, wall, s.u.clock,
                               &supervisor, &update, nullptr, nullptr, &s.u.store.store,
                               &journal, &settings};
  core::console::Console console{out, board, health, s.u.clock, restart, client, slots,
                                 &s.u.store.store, &receiver, &update, &journal, nullptr,
                                 &supervisor, &uplink, nullptr, nullptr, nullptr, &settings,
                                 nullptr, &duty, &cycle, &battery};
  // The sequence by itself, for the tests that call it directly rather than through a line.
  prov::Provision provision{board, slots, s.u.clock, client, &s.u.store.store, &battery, &update, &supervisor};

  explicit ProvWalter(SupWorld& world,
                      core::health::ResetReason reason = core::health::ResetReason::kSoftware,
                      interfaces::WokeBy woke = interfaces::WokeBy::kNotFromSleep)
      : s(world), w(world.u) {
    w.dead = false;
    w.radio.refuse_open = false;
    w.journal_flash.powerOn();
    w.store.flash.powerOn();
    REQUIRE(w.store.store.begin() == StoreError::kNone);
    w.chip.attachTo(target);
    w.chip.on_power_loss = [this] { target.silent = true; };
    // The reset line reaches the chip (task p5-02), and the world sees each pulse and kill.
    target.on_pin_reset = [this] { w.chip.pinReset(); };
    pins.on_reset_pulse = [this] { s.onResetPulse(); };
    pins.on_kill_cycle = [this] { s.onKillCycle(); };
    s.pins = &pins;
    pins.record_log = false;
    (void)board.releaseHolds();
    board.safe();
    sleeper.wake_as = woke;
    sleeper.usb = false;
    health.begin(reason, woke == interfaces::WokeBy::kTimer ? core::health::WakeCause::kTimer
                                                             : core::health::WakeCause::kNotFromSleep);
    w.clock.advance(core::board::kKillTiming.settle_ms);
    settings.begin();
    REQUIRE(journal.begin() == upd::JournalError::kNone);
    uplink.begin();
    readings.begin();
    port.open();
    supervisor.begin();
    console.begin();
  }
  ~ProvWalter() {
    CHECK(pins.violations.empty());
    if (s.pins == &pins) s.pins = nullptr;
  }

  std::string runLine(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }

  // One wake, as a mast runs one: the wake worked out, the bound armed, one pass, which ends in
  // the fake sleeper's sleep (it advances the clock by the interval).
  const core::cycle::Pass& oneWake() {
    duty.begin();
    duty.guard().arm();
    return cycle.run();
  }
};

// A world and the Walter on it.
struct ProvUnit {
  ProvWorld world;
  std::unique_ptr<ProvWalter> walter;

  explicit ProvUnit(Factory f, bool allow = true, bool with_base = true) {
    if (with_base) world.putBase();
    world.factory(f, allow);
    world.clock().advance(sup::kWalterPowerUpGraceMs + 1);
    walter = std::make_unique<ProvWalter>(world.s);
  }

  ProvWalter& v() { return *walter; }
  sim::Nrf52Chip& chip() { return world.chip(); }
  fakes::FakeClock& clock() { return world.clock(); }

  void reboot(core::health::ResetReason reason = core::health::ResetReason::kSoftware) {
    walter.reset();
    walter = std::make_unique<ProvWalter>(world.s, reason);
  }
  // The Walter's power pulled: RTC memory gone, the flashes and the chip still there.
  void powerCut() {
    walter.reset();
    world.s.powerCut();
    walter = std::make_unique<ProvWalter>(world.s, core::health::ResetReason::kPowerOn);
    world.clock().advance(sup::kWalterPowerUpGraceMs + 1);
  }
  // The chip and the Walter both lose power, then both come back (task p5-03's power cut at
  // every step): the chip as a power-on reset leaves it, the target answering again, a new
  // Walter over the same world.
  void powerCutBoth() {
    chip().powerOn();  // while the target it is attached to still lives (a reset resets the DAP)
    walter.reset();
    world.s.powerCut();
    walter = std::make_unique<ProvWalter>(world.s, core::health::ResetReason::kPowerOn);
    // The target of the new Walter is a new object: silent is false there. The world's clock
    // moves past the grace so that a silence is judged, as every test but one wants.
    world.clock().advance(sup::kWalterPowerUpGraceMs + 1);
  }
  // A wake from deep sleep: a new Walter, woken by the timer, one pass.
  const core::cycle::Pass& wake() {
    walter.reset();
    walter = std::make_unique<ProvWalter>(world.s, core::health::ResetReason::kDeepSleepWake,
                                          interfaces::WokeBy::kTimer);
    return walter->oneWake();
  }

  std::string runLine(const char* line) { return walter->runLine(line); }
  prov::Result run(const char* typed, prov::StepListener* listener = nullptr) {
    return walter->provision.run(Slot{0}, core::nrf52::grantFromTypedCommand(typed), listener);
  }
  prov::Result check() { return walter->provision.check(Slot{0}); }

  // The base's words, as the file in the store gives them, held against the chip.
  void checkChipHoldsBase() {
    core::image::StoredImage file;
    REQUIRE(world.s.u.store.store.open(Which::kBase, Slot{0}, &file) == StoreError::kNone);
    core::image::BaseReader reader;
    const core::image::BaseResult judged = reader.read(file, world.profile.app.begin, world.profile.app.end);
    REQUIRE(judged.ok());
    core::image::BaseWords words;
    words.begin(file, judged.runs);
    std::uint64_t address = 0;
    std::uint32_t word = 0;
    unsigned compared = 0;
    unsigned wrong = 0;
    while (words.take(&address, &word)) {
      const std::uint32_t at = static_cast<std::uint32_t>(address);
      const std::uint32_t have = at >= core::image::kUicrFirst ? chip().uicrWord(at) : chip().flashWord(at);
      if (have != word) ++wrong;
      ++compared;
    }
    CHECK(!words.failed());
    CHECK(compared > 0);
    CHECK(wrong == 0);
  }
  void checkNothingWritten() {
    CHECK(chip().counters().writes == 0);
    CHECK(chip().counters().erases == 0);
    CHECK(chip().counters().uicr_writes == 0);
    CHECK(chip().counters().erase_alls == 0);
    CHECK(chip().counters().tripwire == 0);
  }
};

// What each step's end looked like: the clock, the target's transfer count, the chip's
// operation count - for the power-cut test, which cuts at every one of them.
struct StepRecorder final : prov::StepListener {
  struct Seen {
    prov::Step step;
    std::uint64_t at_ms;
    unsigned transfers;
  };
  std::vector<Seen> seen;
  sim::SwdTarget* target = nullptr;
  fakes::FakeClock* clock = nullptr;
  void onStep(prov::Step step, const prov::Result&) override {
    seen.push_back(Seen{step, clock != nullptr ? clock->nowMs() : 0, target != nullptr ? target->counters().transfers : 0});
  }
};

// The `next:` line of `rak provision check`'s output.
inline std::string nextLineOf(const std::string& text) {
  const std::size_t at = text.find("  next: ");
  if (at == std::string::npos) return "";
  const std::size_t end = text.find('\n', at);
  return text.substr(at + 8, end == std::string::npos ? std::string::npos : end - (at + 8));
}

inline bool hasLine(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

}  // namespace bench
