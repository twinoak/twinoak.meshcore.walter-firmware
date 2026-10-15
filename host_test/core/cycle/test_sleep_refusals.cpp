// The two refusals that are about an update in flight (task p3-04, core/cycle/sleep.h): a radio
// update with a step in the air, and - the one that is not a nicety - **this firmware on trial**.
//
// Why on trial is a refusal and not a wait. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on
// (sdkconfig.defaults, S-6) and `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` is off (its own
// default, and this build does not set it), so the second-stage bootloader runs after a
// deep-sleep wake and `select_partition_number()` turns every `PENDING_VERIFY` slot into
// `ABORTED` ($IDF_PATH/components/bootloader_support/src/bootloader_utility.c at v5.5.5). A
// sleep during a trial is a rollback of the image being tried - and the fake application slots
// of host_test/fakes/fake_own_update.h keep exactly those rules at a reboot, which is why the
// second test below can show it.
#include <doctest.h>

#include <string>

#include "core/cycle/sleep.h"
#include "core/cycle/wake_reason.h"
#include "core/power/interval_policy.h"
#include "core/update/self_bench.h"
#include "fakes/fake_sleeper.h"
#include "sim/fake_modem.h"

using core::cycle::SleepRefusal;
using core::cycle::SleepResult;

namespace {

// A decision in `plenty`: what the refusals are checked against, so that a refusal cannot be
// mistaken for a band that forbade something.
core::power::Decision plenty() {
  core::power::IntervalPolicy policy;
  policy.addSample(4100, -20);
  return policy.decide(core::power::Inputs{});
}

}  // namespace

TEST_CASE("no sleep while this firmware is on trial: it would be rolled back") {
  bench::SelfUnit unit;
  bench::SelfWalter* w = unit.walter.get();
  REQUIRE(w->boot() == bench::SelfBoot::kNothing);

  // The pieces a Sleep needs that the self-update bench does not have.
  sim::FakeModem modem{unit.world.clock};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{modem, unit.world.clock, unit.world.retained, modem_settings};
  uplink.begin();
  fakes::FakeSleeper sleeper{unit.world.clock, w->pins};
  core::cycle::WakeReason reason{unit.world.retained, unit.world.clock};
  reason.begin(w->health.thisBoot(), interfaces::WokeBy::kNotFromSleep, false, nullptr);

  {
    core::cycle::Sleep sleep{sleeper,          w->board,  w->client, uplink,  reason,
                             unit.world.clock, unit.world.store.wall, nullptr, &w->self,
                             &w->journal};
    // No trial: a sleep is allowed.
    CHECK(sleep.wouldRefuse() == SleepRefusal::kNone);
    CHECK(sleep.go(plenty(), true).refusal == SleepRefusal::kNone);
    CHECK(sleeper.sleeps == 1);
  }

  // An image received and switched to: the next boot is a trial.
  const bench::Candidate image("self-a");
  REQUIRE(unit.receiveAndSwitch(image));
  unit.reboot();
  w = unit.walter.get();
  REQUIRE(w->boot() == bench::SelfBoot::kTrialBegun);
  REQUIRE(w->self.onTrial());

  fakes::FakeSleeper sleeper2{unit.world.clock, w->pins};
  core::cycle::WakeReason reason2{unit.world.retained, unit.world.clock};
  reason2.begin(w->health.thisBoot(), interfaces::WokeBy::kNotFromSleep, false, nullptr);
  core::cycle::Sleep on_trial{sleeper2,         w->board,  w->client, uplink,  reason2,
                              unit.world.clock, unit.world.store.wall, nullptr, &w->self,
                              &w->journal};
  CHECK(on_trial.wouldRefuse() == SleepRefusal::kOnTrial);
  const SleepResult refused = on_trial.go(plenty(), true);
  CHECK(refused.refusal == SleepRefusal::kOnTrial);
  CHECK(sleeper2.sleeps == 0);
  CHECK(std::string(core::cycle::describe(SleepRefusal::kOnTrial)).find("roll") !=
        std::string::npos);
  // The refusal is journalled, so that a unit found awake on a mast can say why it did not sleep.
  core::update::Entry e;
  core::cycle::Payload p;
  REQUIRE(w->journal.lastOf(core::update::kind::kDutyCycle, &e));
  REQUIRE(core::cycle::decode(e, &p));
  CHECK(p.what == core::cycle::Wrote::kRefused);
  CHECK(p.refusal == SleepRefusal::kOnTrial);

  // The trial ended by a person: the sleep is allowed again.
  REQUIRE(w->self.confirmNow() == bench::SelfReason::kNone);
  CHECK_FALSE(w->self.onTrial());
  CHECK(on_trial.wouldRefuse() == SleepRefusal::kNone);
  CHECK(on_trial.go(plenty(), true).refusal == SleepRefusal::kNone);
  CHECK(sleeper2.sleeps == 1);
}

TEST_CASE("no sleep while a transfer into the other slot is open") {
  bench::SelfUnit unit;
  bench::SelfWalter* w = unit.walter.get();
  REQUIRE(w->boot() == bench::SelfBoot::kNothing);

  sim::FakeModem modem{unit.world.clock};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{modem, unit.world.clock, unit.world.retained, modem_settings};
  uplink.begin();
  fakes::FakeSleeper sleeper{unit.world.clock, w->pins};
  core::cycle::WakeReason reason{unit.world.retained, unit.world.clock};
  reason.begin(w->health.thisBoot(), interfaces::WokeBy::kNotFromSleep, false, nullptr);
  core::cycle::Sleep sleep{sleeper,          w->board,  w->client, uplink,  reason,
                           unit.world.clock, unit.world.store.wall, nullptr, &w->self,
                           &w->journal};

  // A transfer of a release of this firmware, stopped half way through the image: the other slot
  // is open for writing and holds part of an image, and the verdict's checks 4 to 6 have not
  // run. A reset there is a slot with half an image in it.
  namespace transfer = core::image::transfer;
  const bench::Candidate image("self-b");
  REQUIRE(w->receiver.active() == false);
  w->runLine("self receive");
  REQUIRE(w->receiver.active());
  REQUIRE(w->sendFile(transfer::kKindManifest, image.manifest.data(), image.manifest.size())
              .status == transfer::kOk);
  REQUIRE(w->sendFile(transfer::kKindSignature,
                      reinterpret_cast<const std::uint8_t*>(image.signature.data()),
                      image.signature.size())
              .status == transfer::kOk);
  const std::uint32_t size = static_cast<std::uint32_t>(image.file.size());
  const std::uint8_t header[5] = {transfer::kKindImage, static_cast<std::uint8_t>(size),
                                  static_cast<std::uint8_t>(size >> 8),
                                  static_cast<std::uint8_t>(size >> 16),
                                  static_cast<std::uint8_t>(size >> 24)};
  REQUIRE(w->send(transfer::kFile, 0, header, sizeof header).status == transfer::kOk);
  REQUIRE(w->send(transfer::kData, 0, image.file.data(), 240).status == transfer::kOk);
  REQUIRE(w->self.writing());
  CHECK(sleep.wouldRefuse() == SleepRefusal::kTransferOpen);
  CHECK(sleep.go(plenty(), true).refusal == SleepRefusal::kTransferOpen);
  CHECK(sleeper.sleeps == 0);
  w->self.abandonWrite();
  CHECK_FALSE(w->self.writing());
  CHECK(sleep.wouldRefuse() == SleepRefusal::kNone);
}

TEST_CASE("no sleep while the radio's update has a step in the air") {
  bench::ReadyUnit unit;
  bench::Walter* u = unit.walter.get();

  sim::FakeModem modem{unit.world.clock};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{modem, unit.world.clock, u->retained, modem_settings};
  uplink.begin();
  fakes::FakeSleeper sleeper{unit.world.clock, u->pins};
  core::cycle::WakeReason reason{u->retained, unit.world.clock};
  reason.begin(u->health.thisBoot(), interfaces::WokeBy::kNotFromSleep, false, nullptr);
  core::cycle::Sleep sleep{sleeper,    u->board, u->client,             uplink,
                           reason,     unit.world.clock, unit.world.store.wall,
                           &u->update, nullptr,  &u->journal};

  CHECK(sleep.wouldRefuse() == SleepRefusal::kNone);
  // The machine started: it journals before every state it enters, so a reset *between* states
  // is safe - and a step in flight is not a journalled point. The caller steps it.
  REQUIRE(u->update.start(unit.request()));
  REQUIRE(u->update.active());
  CHECK(sleep.wouldRefuse() == SleepRefusal::kUpdateRunning);
  CHECK(sleep.go(plenty(), true).refusal == SleepRefusal::kUpdateRunning);
  CHECK(sleeper.sleeps == 0);
  // Run it to its end, and the sleep is allowed again.
  u->run();
  CHECK_FALSE(u->update.active());
  CHECK(sleep.wouldRefuse() == SleepRefusal::kNone);
  CHECK(sleep.go(plenty(), true).refusal == SleepRefusal::kNone);
  CHECK(sleeper.sleeps == 1);
}
