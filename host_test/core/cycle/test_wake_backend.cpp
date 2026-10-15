// The seam, and what this firmware will and will not do with what comes over it (task p3-05):
// S-7's one update step a wake; a band that forbids what was asked; a secret key and a threshold
// that protects the hardware; the budget running out; and what the structure that crosses the
// seam actually carries.
#include <doctest.h>

#include <string>

#include "core/cycle/wake_bench.h"
#include "core/image/this_image.h"
#include "core/version.h"

using bench::WakeBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;
using interfaces::Ask;
using interfaces::Refusal;

namespace {

// Is this (ask, why) among the refusals the ring kept for the next exchange?
bool refused(bench::WakeBench& bench, Ask ask, Refusal why) {
  const core::cycle::TelemetryRing& ring = bench.v().readings;
  for (std::size_t i = 0; i < ring.refusals(); ++i) {
    const interfaces::BackendRefusal r = ring.refusalAt(i);
    if (r.ask == static_cast<std::uint8_t>(ask) && r.why == static_cast<std::uint8_t>(why)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("S-7: a backend that asks for both updates in one wake gets one, and says so") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.askFor(Ask::kUpdateSelf);
  bench.world.backend.askFor(Ask::kUpdateRadio);

  const Pass& pass = bench.oneWake();

  // Both were asked for; the band is `plenty`, so an update may be started.
  CHECK((pass.asks & interfaces::askBit(Ask::kUpdateSelf)) != 0);
  CHECK((pass.asks & interfaces::askBit(Ask::kUpdateRadio)) != 0);
  CHECK(pass.decision.may.update);
  // The Walter's own goes first (docs/ARCHITECTURE.md §7: it leads a link-version change), and
  // **the radio's is refused with the reason, kept, and told at the next exchange.**
  CHECK(refused(bench, Ask::kUpdateRadio, Refusal::kOneUpdateOnly));
  // This unit has nothing received into its other slot, so the self-update refuses itself - and
  // that refusal is reported too. What must not happen is two update steps in one wake.
  CHECK(pass.outcomeOf(Step::kUpdate) != Outcome::kDone);
  CHECK(bench.v().update.state() == core::update::State::kIdle);
  CHECK(pass.ending == Ending::kSlept);

  // The next wake: the refusal of the wake before it is in what crosses the seam, so a backend
  // that still wants the radio's update knows this unit did not do it and why.
  bench.world.backend.forget();
  (void)bench.oneWake();
  const interfaces::BackendKnowledge& known = bench.world.backend.last();
  MESSAGE("the next exchange carried " << known.refusals << " refusal(s)");
  bool told = false;
  for (std::size_t i = 0; i < known.refusals; ++i) {
    if (known.refusal[i].ask == static_cast<std::uint8_t>(Ask::kUpdateRadio)) told = true;
  }
  CHECK(told);
  // And an exchange that worked is what clears them: the wake after that one says nothing.
  (void)bench.oneWake();
  CHECK(bench.world.backend.last().refusals == 0);
}

TEST_CASE("a backend that asks for something in `critical`: refused, recorded, told") {
  WakeBench bench;
  bench.modemWorks();
  // The band sinks below the floor, where the modem is not used at all - so the ask cannot even
  // arrive over the uplink. A console command is the other way one arrives (the task's own "or a
  // console command asked for it"), and that is what this walks.
  bench.world.rak.battery_mv = 3300;
  bench.boot();
  bench.v().begin();
  bench.cycle().askFor(Ask::kUpdateRadio);
  const Pass& pass = bench.v().run();

  CHECK(pass.band == core::power::Band::kCritical);
  CHECK_FALSE(pass.decision.may.update);
  CHECK(pass.outcomeOf(Step::kUpdate) == Outcome::kRefused);
  CHECK(refused(bench, Ask::kUpdateRadio, Refusal::kBandForbids));
  CHECK(bench.v().update.state() == core::update::State::kIdle);
  CHECK(pass.ending == Ending::kSlept);

  // The pack recovers; the next wake reaches the uplink and **tells** the backend what it
  // refused while it could not.
  bench.world.rak.battery_mv = 4100;
  (void)bench.oneWake();
  const interfaces::BackendKnowledge& known = bench.world.backend.last();
  REQUIRE(known.refusals >= 1);
  bool found = false;
  for (std::size_t i = 0; i < known.refusals; ++i) {
    if (known.refusal[i].ask == static_cast<std::uint8_t>(Ask::kUpdateRadio) &&
        known.refusal[i].why == static_cast<std::uint8_t>(Refusal::kBandForbids)) {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("a backend that asks to set a secret key is refused, and the secret is unchanged") {
  WakeBench bench;
  bench.modemWorks();
  // A PIN set by a person at the console, over a cable.
  char err[96] = {};
  REQUIRE(bench.v().settings.put(core::settings::def(core::settings::Key::kLtePin), "4321", err,
                                 sizeof err));
  const std::string before = bench.v().settings.stored(core::settings::Key::kLtePin);

  bench.world.backend.setSetting("lte.pin", "9999");
  const Pass& pass = bench.oneWake();

  CHECK(pass.ok(Step::kBackend));  // the exchange worked; the ask did not
  CHECK(refused(bench, Ask::kSetSettings, Refusal::kSecretKey));
  CHECK(std::string(bench.v().settings.stored(core::settings::Key::kLtePin)) == before);
}

TEST_CASE("a backend that asks to change a protective threshold is refused: it is not a setting") {
  WakeBench bench;
  bench.modemWorks();
  // The battery floors, the supervisor's caps, the proving period and a wake's budget are build
  // constants and not keys (core/settings/keys.h, *What is not here*), so an ask for one is an
  // ask for a setting that does not exist.
  bench.world.backend.setSetting("supervisor.flashes_per_day", "99");
  const Pass& pass = bench.oneWake();

  CHECK(pass.ok(Step::kBackend));
  CHECK(refused(bench, Ask::kSetSettings, Refusal::kUnknownKey));
  // And the cap itself is where it was.
  CHECK(core::supervisor::kFlashesPerDay == 1);
  CHECK(core::settings::findKey("supervisor.flashes_per_day") == nullptr);
  CHECK(pass.ending == Ending::kSlept);
}

TEST_CASE("a backend that asks to change a non-secret setting is obeyed") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.setSetting("unit.name", "Window at Grenaa");
  const Pass& pass = bench.oneWake();

  CHECK(pass.ok(Step::kBackend));
  CHECK(std::string(bench.v().settings.unitName()) == "Window at Grenaa");
  CHECK(bench.v().readings.refusals() == 0);
}

TEST_CASE("a value the key's own row refuses is refused here too") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.backend.setSetting("wake.interval", "not a number");
  (void)bench.oneWake();
  CHECK(refused(bench, Ask::kSetSettings, Refusal::kBadValue));
  CHECK_FALSE(bench.v().settings.isSet(core::settings::Key::kWakeInterval));
}

TEST_CASE("the budget: a slow modem eats the time, the backend step is `no time`, sleep is reached") {
  WakeBench bench;
  // A modem that comes up, but only just inside its own registration deadline
  // (core/uplink/policy.h, kRegistrationDeadlineMs): two minutes of a two-and-a-half-minute
  // budget, which leaves less than the backend step's least.
  bench.modemWorks();
  bench.world.modem.registers_after_ms = core::uplink::kRegistrationDeadlineMs - 5000;
  bench.world.modem.cost_ms = 1500;  // and every command of the sequence is slow with it
  bench.world.backend.askFor(Ask::kFetchRelease);

  const Pass& pass = bench.oneWake();

  // The uplink came up, late; what is left of the budget is less than the backend's own least,
  // so the backend is **no time** - recorded, with a reason, and never half-run.
  CHECK(pass.ok(Step::kUplink));
  CHECK(pass.step[static_cast<std::size_t>(Step::kUplink)].spent_ms > 100000);
  const Outcome backend = pass.outcomeOf(Step::kBackend);
  const bool out_of_time = backend == Outcome::kNoTime;
  CHECK(out_of_time);
  CHECK(std::string(pass.step[static_cast<std::size_t>(Step::kBackend)].why) ==
        "the wake's budget");
  CHECK(bench.world.backend.exchanges == 0);
  // And sleep is still reached, inside the bound on the wake.
  CHECK(pass.ending == Ending::kSlept);
  CHECK(pass.awake_s * 1000u < core::cycle::kWakeBoundMs);
  CHECK(bench.v().restart.asked == 0);
}

TEST_CASE("what crosses the seam: both firmwares, the store, the wake, the signal - and no secret") {
  WakeBench bench;
  bench.modemWorks();
  char err[96] = {};
  REQUIRE(bench.v().settings.put(core::settings::def(core::settings::Key::kLtePin), "86421357",
                                 err, sizeof err));
  REQUIRE(bench.v().settings.put(core::settings::def(core::settings::Key::kServerToken),
                                 "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV", err, sizeof err));
  (void)bench.oneWake();

  const interfaces::BackendKnowledge& known = bench.world.backend.last();
  // This firmware, by its own version and the descriptor inside its image.
  CHECK(known.walter.known);
  CHECK(std::string(known.walter.version) == core::version());
  CHECK(known.walter.descriptor_known);
  CHECK(known.walter.link_major == core::image::kLinkMajor);
  // The radio, as HELLO gave it.
  CHECK(known.radio.known);
  CHECK(std::string(known.radio.version) == "0.3.0+golden0");
  CHECK(std::string(known.radio.board) == "rak4630-single");
  CHECK(known.radio.role == core::link::role::kRepeater);
  // Its state, as STATUS gave it, and the band.
  CHECK(known.radio_uptime_s != 0);
  CHECK(known.battery_known);
  CHECK(known.battery_mv == 4100);
  // This wake, with its letters.
  CHECK(known.wake.steps[0] != 0);
  CHECK(known.wake.band == static_cast<std::uint8_t>(core::power::Band::kPlenty));
  CHECK(known.wake.next_interval_s == core::power::kPlentyMinutes * 60u);
  // The uplink, with the signal and **nothing that identifies a subscription**.
  CHECK(known.uplink.up);
  CHECK(known.uplink.signal_known);
  // The store is there and empty: no golden image has been put in this bench's flash.
  CHECK(known.store_there);
  CHECK_FALSE(known.golden.present);
  // And the whole structure, read as bytes, holds neither secret and no IMEI, ICCID or IMSI.
  const interfaces::Identity numbers = sim::FakeModem::makeIdentity();
  const char* const forbidden[] = {"86421357", "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV",
                                   numbers.imei, numbers.iccid, numbers.imsi};
  const char* bytes = reinterpret_cast<const char*>(&known);
  const std::string blob(bytes, sizeof known);
  for (const char* secret : forbidden) {
    if (secret[0] == 0) continue;
    CHECK_MESSAGE(blob.find(secret) == std::string::npos, std::string(secret));
  }
}

TEST_CASE("the mesh client is not used: the backend cannot ask for a job, and nothing sends one") {
  // The closed set has no member that reaches the mesh, by construction: `Ask` has six values and
  // every one of them is something this firmware does by itself. That is the acceptance criterion
  // "contains nothing that names MeshMaster, a URL path, JSON or a job", as a test.
  CHECK(static_cast<std::uint8_t>(Ask::kCount) == 6);
  for (std::uint8_t a = 0; a < static_cast<std::uint8_t>(Ask::kCount); ++a) {
    const std::string what = interfaces::describe(static_cast<Ask>(a));
    CHECK(what.find("job") == std::string::npos);
    CHECK(what.find("mesh") == std::string::npos);
    CHECK(what.find("json") == std::string::npos);
  }
  // And a wake that is asked for everything in the set sends no mesh request.
  WakeBench bench;
  bench.modemWorks();
  for (std::uint8_t a = 1; a < static_cast<std::uint8_t>(Ask::kCount); ++a) {
    bench.world.backend.askFor(static_cast<Ask>(a));
  }
  (void)bench.oneWake();
  for (std::uint8_t type : bench.requestTypes()) {
    // The whole MESH_CLIENT and OBSERVATIONS range: §4's own capability mapping names it, so
    // this cannot drift from the message table.
    const std::uint32_t needs = core::link::capabilityOf(type);
    CHECK_MESSAGE(needs != core::link::cap::kMeshClient, static_cast<unsigned>(type));
    CHECK_MESSAGE(needs != core::link::cap::kObservations, static_cast<unsigned>(type));
  }
}
