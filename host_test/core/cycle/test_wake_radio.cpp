// The states of the radio, and the two machines a wake may find mid-run (task p3-05):
//
//   * a radio that is **silent**: the supervisor's ladder, inside its caps, and the uplink still
//     used to say so;
//   * a radio **mid-update**: resumed, and nothing else touches it;
//   * this firmware **on trial**: the trial's conditions run through the wake's steps and it is
//     finished before the wake may sleep (a deep sleep during a trial is a rollback);
//   * a download **half done**: the next sleep is shortened in `plenty` and in no other band.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/cycle/wake_bench.h"

using bench::WakeBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

namespace {

// A journal entry that says a radio update was flashing when the Walter stopped: what `resume()`
// finds at the next boot (core/update/radio_update.h).
void journalAFlash(bench::WakeBench& bench) {
  core::update::Payload p{};
  p.state = core::update::State::kFlashing;
  p.kind = core::update::Kind::kUpdate;
  p.slot = 0;
  std::uint8_t bytes[core::update::kPayloadBytes];
  core::update::encode(p, bytes);
  REQUIRE(bench.v().journal.append(core::update::kind::kRadioUpdate, bytes, sizeof bytes) ==
          core::update::JournalError::kNone);
}

}  // namespace

TEST_CASE("a silent radio: the ladder climbs inside its caps, and the uplink still says so") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.rak.deaf = true;

  const Pass& pass = bench.oneWake();

  CHECK(pass.radio == core::supervisor::Health::kSilent);
  CHECK(pass.silence == core::supervisor::Silence::kNotAnswering);
  CHECK(pass.climbed);
  // The climb held the wake open past the budget, and said so by raising the bound.
  CHECK(pass.bound_ms == core::cycle::kLadderBoundMs);
  CHECK(pass.outside_ms > 0);
  // **The uplink was still used**: that is how anybody ever learns the radio has gone quiet.
  CHECK(pass.ok(Step::kUplink));
  const Outcome backend = pass.outcomeOf(Step::kBackend);
  const bool reached = backend == Outcome::kDone || backend == Outcome::kNothing;
  CHECK(reached);
  // And the incident crossed the seam.
  const interfaces::BackendKnowledge& known = bench.world.backend.last();
  CHECK(known.incidents >= 1);
  CHECK(known.wake.radio == static_cast<std::uint8_t>(core::supervisor::Health::kSilent));
  // The caps held, and the wake ended in sleep inside its raised bound.
  const core::supervisor::Status& watch = bench.v().supervisor->status();
  CHECK(watch.counts.resets <= watch.caps.resets);
  CHECK(watch.counts.power_cycles <= watch.caps.power_cycles);
  CHECK(watch.counts.flashes <= watch.caps.flashes);
  CHECK(pass.ending == Ending::kSlept);
  CHECK(pass.spent_ms <= pass.bound_ms);
  CHECK(bench.v().restart.asked == 0);
}

TEST_CASE("a radio mid-update: the run is resumed and nothing else touches the radio") {
  WakeBench bench;
  bench.modemWorks();
  // The journal says a flash was in progress, and the radio is silent because it is being
  // flashed. §9's third silence: "an update is in progress: not ours to disturb".
  journalAFlash(bench);
  bench.world.rak.deaf = true;

  bench.boot();
  bench.v().begin();
  const Pass& pass = bench.v().run();

  CHECK(pass.silence == core::supervisor::Silence::kUpdateRunning);
  CHECK_FALSE(pass.climbed);  // the ladder is not climbed on a radio somebody else is flashing
  // Step 3 took the run up again; step 4 asked the radio nothing.
  const Outcome resume = pass.outcomeOf(Step::kResume);
  const bool resumed = resume == Outcome::kDone || resume == Outcome::kNoTime;
  CHECK(resumed);
  CHECK(pass.outcomeOf(Step::kState) == Outcome::kSkipped);
  CHECK(std::string(pass.step[static_cast<std::size_t>(Step::kState)].why) ==
        "an update is in progress: not ours to disturb");
  // No second update step in the same wake (S-7), whatever anybody asked for.
  CHECK(pass.outcomeOf(Step::kUpdate) != Outcome::kDone);
  // The run ended one way or another - this bench has no chip to flash, so it ends badly - and
  // **the wake still sleeps**, because a terminal machine is not a step in flight.
  CHECK(core::update::isTerminal(bench.v().update.state()));
  CHECK(pass.ending == Ending::kSlept);
}

TEST_CASE("this firmware on trial: the conditions run through the wake, and it is confirmed") {
  WakeBench bench;
  bench.modemWorks();
  // The other slot was made the next to boot and the board restarted into it: the bootloader
  // marks it pending-verify, which is what "a trial is one boot" means
  // (host_test/fakes/fake_own_update.h keeps the bootloader's own rules).
  bench.world.own.slot(1).has_image = true;
  bench.world.own.slot(1).version = "0.4.0+trial00";
  bench.world.own.slot(1).seq = 99;
  bench.world.own.slot(1).state = interfaces::OwnImageState::kNew;
  bench.world.own.reboot();
  REQUIRE(bench.world.own.runningIndex() == 1);

  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware,
             core::health::Restart::kSelfUpdateSwitch);
  bench.v().begin();
  REQUIRE(bench.v().self.onTrial());
  const Pass& pass = bench.v().run();

  CHECK(pass.on_trial);
  // Step 3 noted it and raised the bound; step 9 finished it.
  CHECK(pass.bound_ms >= core::cycle::kTrialBoundMs);
  CHECK(pass.ok(Step::kUpdate));
  CHECK(pass.carried == interfaces::Ask::kUpdateSelf);
  CHECK(bench.v().self.status().state == core::update::SelfState::kConfirmed);
  CHECK_FALSE(bench.v().self.onTrial());
  // Its three conditions were met by the wake's own steps: the main loop, the radio, the server.
  CHECK(bench.v().self.status().main_loop == core::update::Condition::kMet);
  CHECK(bench.v().self.status().radio == core::update::Condition::kMet);
  // And **only then** could the wake sleep: a deep sleep during a trial is a rollback.
  CHECK(pass.ending == Ending::kSlept);
}

TEST_CASE("a trial that cannot be confirmed: the wake does not sleep, and the bound ends it") {
  WakeBench bench;
  bench.modemWorks();
  bench.world.own.slot(1).has_image = true;
  bench.world.own.slot(1).version = "0.4.0+trial01";
  bench.world.own.slot(1).seq = 99;
  bench.world.own.slot(1).state = interfaces::OwnImageState::kNew;
  bench.world.own.reboot();

  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware,
             core::health::Restart::kSelfUpdateSwitch);
  bench.v().begin();
  // The radio answered before the update and does not now: condition 2 fails, so the trial runs
  // to the end of its window and rejects itself - which restarts the board.
  bench.world.rak.deaf = true;
  bench.v().server.verdict = core::update::UplinkVerdict::kFailed;
  const Pass& pass = bench.v().run();

  CHECK(pass.on_trial);
  // Either the trial rejected itself (a restart) or the bound on the wake ended it. Both are
  // ways back to a state this firmware understands, and **neither is a sleep during a trial**.
  const bool ended_safely = pass.ending == Ending::kRestarting ||
                            pass.ending == Ending::kOverran ||
                            pass.ending == Ending::kSleepRefused;
  CHECK(ended_safely);
  // **Nothing slept.** A deep sleep during a trial is a rollback of the image being tried, which
  // is the one thing this must never do (task p3-04's finding, core/cycle/sleep.h).
  CHECK(bench.v().sleeper.sleeps == 0);
}

TEST_CASE("a download half done shortens the next sleep in `plenty`, and in no other band") {
  WakeBench bench;
  bench.modemWorks();
  // A transfer into the candidate slot that was interrupted: the store's own progress record is
  // what says so (core/image/store.h), and it is what the next wake resumes from.
  // `store-resume` is the fixture made for this: 150 000 bytes, so a write that stops past
  // `kResumeStep` leaves a progress record behind - and a progress record is the only thing that
  // can say "this much of this file is in flash" after a reset.
  const bench::Candidate release{"store-resume"};
  REQUIRE(release.file.size() > core::image::kResumeStep);
  REQUIRE(bench.world.images.open(core::image::Which::kCandidate, core::board::Slot{0}, release) ==
          core::image::StoreError::kNone);
  const std::uint32_t half = core::image::kResumeStep + 4096;
  REQUIRE(bench.world.images.store.write(release.file.data(), half) ==
          core::image::StoreError::kNone);
  bench.world.images.store.abandonWrite();
  core::image::Record record{};
  REQUIRE(bench.world.images.store.read(core::image::Which::kCandidate, core::board::Slot{0},
                                        &record) == core::image::StoreError::kNone);
  REQUIRE(record.transfer);

  SUBCASE("in `plenty` it is shortened") {
    bench.world.rak.battery_mv = 4150;
    const Pass& pass = bench.oneWake();
    CHECK(pass.band == core::power::Band::kPlenty);
    CHECK(pass.shortened);
    CHECK(pass.interval_s == core::cycle::kDownloadMinutes * 60u);
    CHECK(pass.interval_s < core::power::kPlentyMinutes * 60u);
    CHECK(pass.ending == Ending::kSlept);
  }
  SUBCASE("in `saving` it is not") {
    bench.world.rak.battery_mv = 3500;
    const Pass& pass = bench.oneWake();
    CHECK(pass.band == core::power::Band::kSaving);
    CHECK_FALSE(pass.shortened);
    CHECK(pass.interval_s == core::power::kSavingMinutes * 60u);
  }
  SUBCASE("in `critical` it is not") {
    bench.world.rak.battery_mv = 3300;
    const Pass& pass = bench.oneWake();
    CHECK(pass.band == core::power::Band::kCritical);
    CHECK_FALSE(pass.shortened);
    CHECK(pass.interval_s == core::power::kCriticalMinutes * 60u);
  }
}

TEST_CASE("the first wake of a unit that has just been powered up does not judge the radio") {
  // §2 rule 5: our own power-up glitches nRESET, so the radio was reset with us and is still
  // booting. The supervisor knows it and the wake says the same thing.
  WakeBench bench;
  bench.modemWorks();
  bench.world.rak.deaf = true;
  const Pass& pass = bench.firstWake();

  CHECK(pass.reason == core::cycle::Wake::kPowerOn);
  CHECK(pass.silence == core::supervisor::Silence::kWalterPoweredUp);
  CHECK_FALSE(pass.climbed);
  CHECK(bench.v().supervisor->incidentCount() == 0);
  CHECK(pass.ending == Ending::kSlept);
}
