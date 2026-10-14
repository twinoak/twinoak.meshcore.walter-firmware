// The radio's update (task p2-03), on the bench of update_bench.h: the good path end to end;
// each failure of the health check in turn, every one ending restored with the candidate on the
// bad list and refused a second time; the golden image that does not come back; each
// precondition refused with nothing touched; the first install; the scanner round trip; and the
// order that matters most - the bad list before the rollback flash.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/link/codec/wire.h"
#include "core/update/radio_update.h"
#include "update_bench.h"

using bench::Boot;
using bench::Kind;
using bench::ReadyUnit;
using bench::Reason;
using bench::State;
using bench::Which;
namespace upd = core::update;

namespace {

std::vector<State> statesOf(const bench::Walter& w) {
  std::vector<State> out;
  for (const upd::StepResult& s : w.transitions) out.push_back(s.after);
  return out;
}

unsigned flashOps(const sim::Nrf52Chip& chip) {
  return chip.countLog(sim::FlashEvent::Kind::kErasePage) + chip.countLog(sim::FlashEvent::Kind::kWrite);
}

// The last radio-update entry of the journal, decoded.
upd::Payload lastEntry(const bench::Walter& w) {
  upd::Entry e;
  upd::Payload p;
  REQUIRE(w.journal.lastOf(upd::kind::kRadioUpdate, &e));
  REQUIRE(upd::decode(e, &p));
  return p;
}

}  // namespace

TEST_CASE("the good path: the candidate is flashed, proves itself and becomes golden") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  REQUIRE(unit.goldenVersion() == unit.golden.version);
  REQUIRE(unit.world.running_version == unit.golden.version);

  REQUIRE(w.update.start(unit.request()));
  CHECK(w.update.state() == State::kCheck);
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);
  CHECK(statesOf(w) == std::vector<State>{State::kFlashing, State::kBooting, State::kProving,
                                          State::kCommitting, State::kDone});

  // The candidate is the golden image, the candidate slot is empty, and the radio runs it.
  CHECK(unit.goldenVersion() == unit.candidate.version);
  CHECK_FALSE(unit.read(Which::kCandidate).present);
  CHECK(unit.world.store.store.verify(Which::kGolden, bench::Slot{0}) == bench::StoreError::kNone);
  CHECK(unit.world.running_version == unit.candidate.version);
  CHECK(unit.world.chipHolds(unit.candidate));
  CHECK(unit.world.store.store.badCount() == 0);
  // The proof took the whole period, with a PING every interval, and the clock was set.
  const upd::Status& st = w.update.status();
  CHECK(st.pings >= upd::kProvingPeriodMs / upd::kProvingPingIntervalMs);
  CHECK(st.prove_elapsed_ms >= upd::kProvingPeriodMs);
  CHECK_FALSE(st.time_skipped);
  CHECK(w.client.counters().time_sets >= 1);
  // Seven journal entries: check, flashing, booting, proving, committing, done - and the
  // committing's entry carries the proving's time.
  CHECK(w.journal.validEntries() == 6);
  const upd::Payload last = lastEntry(w);
  CHECK(last.state == State::kDone);
  CHECK(last.kind == Kind::kUpdate);
  CHECK(core::image::sha256Equal(last.sha256, unit.read(Which::kGolden).sha256));
  // One flash, one reset of the radio; the chip touched nowhere it should not (the chip itself
  // fails the test otherwise, and keeps no tripwire count).
  CHECK(unit.world.chip.counters().tripwire == 0);
  CHECK(unit.world.chip.errors().empty());
  CHECK(unit.world.boots == 2);  // the golden image at the start, the candidate after the flash
  CHECK_FALSE(w.update.active());
  MESSAGE("the good path's transitions:\n" << w.transitionText());
}

TEST_CASE("the good path with no time to give: the clock check is skipped and said so, TIME_GET answers") {
  ReadyUnit unit;
  unit.world.store.wall.forget();
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  CHECK(w.run() == State::kDone);
  CHECK(w.update.status().time_skipped);
  CHECK(w.client.counters().time_sets == 0);
}

namespace {

// One failure of the health check: the candidate boots as the knob says, fails, the golden image
// is put back and proved, and the candidate is on the bad list and refused a second time.
void failsAndIsRestored(Boot boot, Reason reason, upd::ProveStep step,
                        const std::function<void(ReadyUnit&, const upd::StepResult&)>& on_step = nullptr) {
  ReadyUnit unit;
  unit.world.boot = boot;
  unit.world.old_version = unit.golden.version;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run([&](const upd::StepResult& s) {
    if (on_step) on_step(unit, s);
    // The golden image must boot normally when its turn comes.
    if (s.changed && s.after == State::kRollingBack) unit.world.boot = Boot::kNormal;
  });
  INFO(w.transitionText());
  CHECK(end == State::kRestored);
  CHECK(w.update.reason() == reason);
  const upd::Payload last = lastEntry(w);
  CHECK(last.state == State::kRestored);
  // The rolling-back entry says why and where.
  upd::Cursor c;
  upd::Entry e;
  bool found = false;
  REQUIRE(w.journal.first(&c, &e));
  do {
    upd::Payload p;
    if (upd::decode(e, &p) && p.state == State::kRollingBack) {
      found = true;
      CHECK(p.reason == reason);
      CHECK(p.step == step);
    }
  } while (w.journal.previous(&c, &e));
  CHECK(found);
  // The golden image is golden still, back on the chip and running; the candidate is still in
  // the store but on the bad list.
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.store.store.isBad(unit.read(Which::kCandidate).sha256));
  CHECK(unit.world.store.store.badCount() == 1);

  // A second update with the same candidate is refused at check, and nothing is flashed.
  const unsigned ops = flashOps(unit.world.chip);
  const unsigned resets = unit.world.chip.counters().resets;
  REQUIRE(w.update.start(unit.request()));
  CHECK(w.run() == State::kRefused);
  CHECK(w.update.reason() == Reason::kOnBadList);
  CHECK(flashOps(unit.world.chip) == ops);
  CHECK(unit.world.chip.counters().resets == resets);
  CHECK(unit.world.chip.counters().tripwire == 0);
}

}  // namespace

TEST_CASE("proving fails: no HELLO at all") {
  failsAndIsRestored(Boot::kSilent, Reason::kNoBoot, upd::ProveStep::kHello);
}

TEST_CASE("proving fails: HELLO with the old version") {
  failsAndIsRestored(Boot::kOldVersion, Reason::kWrongVersion, upd::ProveStep::kHello);
}

TEST_CASE("proving fails: the wrong role") {
  failsAndIsRestored(Boot::kWrongRole, Reason::kRoleMismatch, upd::ProveStep::kHello);
}

TEST_CASE("proving fails: the wrong board") {
  failsAndIsRestored(Boot::kWrongBoard, Reason::kBoardMismatch, upd::ProveStep::kHello);
}

TEST_CASE("proving fails: TIME_SET refused") {
  failsAndIsRestored(Boot::kRefuseTime, Reason::kTimeRefused, upd::ProveStep::kTime);
}

TEST_CASE("proving fails: the radio not initialised") {
  failsAndIsRestored(Boot::kNotInitialised, Reason::kNotInitialised, upd::ProveStep::kStatus);
}

TEST_CASE("proving fails: POWER_GET does not answer though the role has telemetry") {
  failsAndIsRestored(Boot::kNoPower, Reason::kNoTelemetry, upd::ProveStep::kPower);
}

TEST_CASE("proving fails: a restart inside the proving period") {
  failsAndIsRestored(Boot::kNormal, Reason::kRestarted, upd::ProveStep::kPing,
                     [](ReadyUnit& unit, const upd::StepResult& s) {
                       // Two minutes into the proving period, the candidate restarts itself.
                       if (s.changed && s.after == State::kProving) {
                         unit.world.restart_at_ms = unit.world.clock.nowMs() + 120000;
                       }
                     });
}

TEST_CASE("proving fails: silence after a good start") {
  failsAndIsRestored(Boot::kNormal, Reason::kSilence, upd::ProveStep::kPing,
                     [](ReadyUnit& unit, const upd::StepResult& s) {
                       if (s.changed && s.after == State::kProving) {
                         unit.world.silence_at_ms = unit.world.clock.nowMs() + 30000;
                       }
                     });
}

TEST_CASE("the bad list is written before the rollback flash, and the failed candidate is never flashed twice") {
  ReadyUnit unit;
  unit.world.boot = Boot::kSilent;
  bench::Walter& w = *unit.walter;
  const bench::Record candidate_record = unit.read(Which::kCandidate);
  std::uint8_t hash[32];
  for (int i = 0; i < 32; ++i) hash[i] = candidate_record.sha256[i];

  // A progress sink sees the rollback's flash begin: at its first page the hash is on the bad
  // list already and the chip has not been touched since the candidate's flash ended.
  struct Watch final : core::flasher::ProgressSink {
    ReadyUnit& unit;
    const std::uint8_t* hash;
    bool seen_rollback_scan = false;
    bool bad_at_first_scan = false;
    unsigned ops_at_first_scan = 0;
    unsigned ops_when_rollback_entered = 0;
    Watch(ReadyUnit& u, const std::uint8_t* h) : unit(u), hash(h) {}
    void onProgress(const core::flasher::Progress& p) override {
      if (unit.walter->update.state() != State::kRollingBack || seen_rollback_scan) return;
      if (p.step != core::flasher::Step::kScan) return;
      seen_rollback_scan = true;
      bad_at_first_scan = unit.world.store.store.isBad(hash);
      ops_at_first_scan = flashOps(unit.world.chip);
    }
  } watch(unit, hash);
  w.update.setProgress(&watch);

  unsigned candidate_flashes = 0;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run([&](const upd::StepResult& s) {
    if (s.changed && s.after == State::kBooting) ++candidate_flashes;
    if (s.changed && s.after == State::kRollingBack) {
      unit.world.boot = Boot::kNormal;
      watch.ops_when_rollback_entered = flashOps(unit.world.chip);
      // The bad list is in the store's flash before anything else of the rollback happens.
      CHECK(unit.world.store.store.isBad(hash));
    }
  });
  CHECK(end == State::kRestored);
  CHECK(watch.seen_rollback_scan);
  CHECK(watch.bad_at_first_scan);
  CHECK(watch.ops_at_first_scan == watch.ops_when_rollback_entered);
  CHECK(candidate_flashes == 1);
  // The store's flash log: the index write of markBad comes before the rollback's first chip
  // operation - shown by the two counters above; and here that the bad list survived a reboot.
  unit.reboot();
  CHECK(unit.world.store.store.isBad(hash));
  REQUIRE(unit.walter->update.start(unit.request()));
  CHECK(unit.walter->run() == State::kRefused);
  CHECK(unit.walter->update.reason() == Reason::kOnBadList);
}

TEST_CASE("the golden image does not come back either: stuck, once, journalled, nothing retried") {
  ReadyUnit unit;
  unit.world.boot = Boot::kSilent;  // every boot, the golden image's included
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kStuck);
  CHECK(w.update.reason() == Reason::kNoBoot);
  CHECK(statesOf(w) == std::vector<State>{State::kFlashing, State::kBooting, State::kRollingBack,
                                          State::kRollbackBooting, State::kStuck});
  CHECK(lastEntry(w).state == State::kStuck);
  // Two flashes - the candidate's and the golden image's - and no third.
  CHECK(unit.world.boots == 3);  // the start, the candidate, the golden image
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.store.store.isBad(unit.read(Which::kCandidate).sha256));
  CHECK(unit.goldenVersion() == unit.golden.version);  // the store's golden image is untouched
  // Stepping a stuck machine does nothing; starting again is refused at check (the bad list).
  const unsigned ops = flashOps(unit.world.chip);
  for (int i = 0; i < 10; ++i) {
    const upd::StepResult s = w.update.step();
    CHECK(s.waited);
    CHECK_FALSE(s.changed);
  }
  REQUIRE(w.update.start(unit.request()));
  CHECK(w.run() == State::kRefused);
  CHECK(w.update.reason() == Reason::kOnBadList);
  CHECK(flashOps(unit.world.chip) == ops);
  // And after a reboot there is nothing to resume: stuck is terminal.
  unit.reboot();
  CHECK_FALSE(unit.walter->update.resume());
}

namespace {

void refusedWithNothingTouched(ReadyUnit& unit, const upd::Request& request, Reason reason) {
  bench::Walter& w = *unit.walter;
  const unsigned ops = flashOps(unit.world.chip);
  const unsigned resets = unit.world.chip.counters().resets;
  REQUIRE(w.update.start(request));
  CHECK(w.update.state() == State::kCheck);
  const upd::StepResult s = w.update.step();
  CHECK(s.changed);
  CHECK(s.after == State::kRefused);
  CHECK(s.reason == reason);
  CHECK(w.update.reason() == reason);
  CHECK_FALSE(w.update.active());
  CHECK(flashOps(unit.world.chip) == ops);
  CHECK(unit.world.chip.counters().resets == resets);
  const upd::Payload last = lastEntry(w);
  CHECK(last.state == State::kRefused);
  CHECK(last.reason == reason);
}

}  // namespace

TEST_CASE("each precondition refused, with the flash log empty") {
  SUBCASE("no candidate") {
    ReadyUnit unit(false);
    refusedWithNothingTouched(unit, unit.request(), Reason::kNoCandidate);
  }
  SUBCASE("the candidate does not verify (flash rot)") {
    ReadyUnit unit;
    const std::uint32_t data = core::image::areaOffset(0, unit.world.store.store.areaOf(Which::kCandidate, bench::Slot{0})) +
                               core::image::kSectorSize;
    unit.world.store.flash.rot(data + 100, 0x10);
    refusedWithNothingTouched(unit, unit.request(), Reason::kImageUnreadable);
  }
  SUBCASE("on the bad list") {
    ReadyUnit unit;
    REQUIRE(unit.world.store.store.markBad(unit.read(Which::kCandidate).sha256) == bench::StoreError::kNone);
    refusedWithNothingTouched(unit, unit.request(), Reason::kOnBadList);
  }
  SUBCASE("the wrong role: a scanner image as the candidate") {
    ReadyUnit unit(false);
    const bench::Candidate scanner("store-scanner");
    REQUIRE(unit.world.store.put(Which::kCandidate, bench::Slot{0}, scanner) == bench::StoreError::kNone);
    refusedWithNothingTouched(unit, unit.request(), Reason::kWrongRole);
  }
  SUBCASE("the battery low") {
    ReadyUnit unit;
    unit.walter->battery.millivolts = upd::kBatteryMinMv - 1;
    refusedWithNothingTouched(unit, unit.request(), Reason::kBatteryLow);
  }
  SUBCASE("the battery falling fast") {
    ReadyUnit unit;
    unit.walter->battery.trend_known = true;
    unit.walter->battery.mv_per_hour = upd::kBatteryFallingMvPerHour;
    refusedWithNothingTouched(unit, unit.request(), Reason::kBatteryFalling);
  }
  SUBCASE("the battery unknown") {
    ReadyUnit unit;
    unit.walter->battery.known = false;
    refusedWithNothingTouched(unit, unit.request(), Reason::kBatteryUnknown);
  }
  SUBCASE("the link down, and nobody has said why") {
    ReadyUnit unit;
    unit.world.radio.deaf = true;
    refusedWithNothingTouched(unit, unit.request(), Reason::kLinkDown);
  }
  SUBCASE("the link down, but the caller has classified it: allowed through to the battery") {
    ReadyUnit unit;
    unit.world.radio.deaf = true;
    upd::Request r = unit.request();
    r.link_known_down = true;
    // The battery is read over the link too, and the fake says it is known: so this one gets
    // past the link and would flash. Stop it at the battery instead, to show the link let it by.
    unit.walter->battery.known = false;
    refusedWithNothingTouched(unit, r, Reason::kBatteryUnknown);
  }
  SUBCASE("the Walter's own update unconfirmed (S-7)") {
    ReadyUnit unit;
    std::uint8_t self[8] = {upd::self_update::kSwitching, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(unit.walter->journal.append(upd::kind::kSelfUpdate, self, sizeof self) == upd::JournalError::kNone);
    CHECK_FALSE(unit.walter->update.start(unit.request()));
    CHECK(unit.walter->update.reason() == Reason::kSelfUpdateUnconfirmed);
    CHECK(flashOps(unit.world.chip) == 0);
    // Confirmed: allowed.
    self[0] = upd::self_update::kConfirmed;
    REQUIRE(unit.walter->journal.append(upd::kind::kSelfUpdate, self, sizeof self) == upd::JournalError::kNone);
    CHECK(unit.walter->update.start(unit.request()));
  }
  SUBCASE("a radio update journalled as in progress: resumed, never started over (S-7)") {
    ReadyUnit unit;
    upd::Payload p;
    p.state = State::kFlashing;
    p.kind = Kind::kUpdate;
    std::uint8_t bytes[upd::kPayloadBytes];
    upd::encode(p, bytes);
    REQUIRE(unit.walter->journal.append(upd::kind::kRadioUpdate, bytes, sizeof bytes) == upd::JournalError::kNone);
    CHECK_FALSE(unit.walter->update.start(unit.request()));
    CHECK(unit.walter->update.reason() == Reason::kRadioUpdateInProgress);
    CHECK(flashOps(unit.world.chip) == 0);
  }
  SUBCASE("no golden image and no first-install") {
    ReadyUnit unit(true, false);
    refusedWithNothingTouched(unit, unit.request(), Reason::kNoGolden);
  }
  SUBCASE("no scanner image for rak role scanner") {
    ReadyUnit unit;
    refusedWithNothingTouched(unit, unit.request(Kind::kRoleScanner), Reason::kNoCandidate);
  }
  SUBCASE("a slot this adapter has no radio in") {
    ReadyUnit unit;
    upd::Request r = unit.request();
    r.slot = bench::Slot{1};
    refusedWithNothingTouched(unit, r, Reason::kNoProfile);
  }
}

TEST_CASE("a first install: allowed with no golden image, and the candidate becomes golden") {
  ReadyUnit unit(true, false);
  CHECK_FALSE(unit.world.radio_runs);  // a blank chip: nothing answers
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request(Kind::kFirstInstall)));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);
  CHECK(unit.goldenVersion() == unit.candidate.version);
  CHECK_FALSE(unit.read(Which::kCandidate).present);
  CHECK(unit.world.running_version == unit.candidate.version);
}

TEST_CASE("a first install that fails ends stuck with the reason 'no image to return to'") {
  ReadyUnit unit(true, false);
  unit.world.boot = Boot::kSilent;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request(Kind::kFirstInstall)));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kStuck);
  CHECK(w.update.reason() == Reason::kNoImageToReturnTo);
  CHECK(statesOf(w) == std::vector<State>{State::kFlashing, State::kBooting, State::kStuck});
  CHECK(unit.world.store.store.isBad(unit.read(Which::kCandidate).sha256));
  CHECK_FALSE(unit.read(Which::kGolden).present);
  CHECK(unit.world.boots == 2);
}

TEST_CASE("the scanner round trip leaves golden and candidate untouched") {
  ReadyUnit unit;
  const bench::Candidate scanner("store-scanner");
  REQUIRE(unit.world.store.put(Which::kScanner, bench::Slot{0}, scanner) == bench::StoreError::kNone);
  const bench::Record golden_before = unit.read(Which::kGolden);
  const bench::Record candidate_before = unit.read(Which::kCandidate);
  bench::Walter& w = *unit.walter;

  REQUIRE(w.update.start(unit.request(Kind::kRoleScanner)));
  State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);
  CHECK(statesOf(w) == std::vector<State>{State::kFlashing, State::kBooting, State::kProving, State::kDone});
  CHECK(unit.world.chipHolds(scanner));
  CHECK(unit.world.radio.scanner);
  CHECK(w.client.mode() == core::link::client::Mode::kText);
  CHECK(w.update.status().kind == Kind::kRoleScanner);

  // Nothing of the store moved: the same records, verifying, no promotion, no bad list.
  const bench::Record golden_after = unit.read(Which::kGolden);
  const bench::Record candidate_after = unit.read(Which::kCandidate);
  CHECK(golden_after.present);
  CHECK(candidate_after.present);
  CHECK(core::image::sha256Equal(golden_after.sha256, golden_before.sha256));
  CHECK(core::image::sha256Equal(candidate_after.sha256, candidate_before.sha256));
  CHECK(unit.world.store.store.verify(Which::kGolden, bench::Slot{0}) == bench::StoreError::kNone);
  CHECK(unit.world.store.store.verify(Which::kCandidate, bench::Slot{0}) == bench::StoreError::kNone);
  CHECK(unit.world.store.store.badCount() == 0);

  // And back: the golden image flashed and proved as an update would prove it.
  w.transitions.clear();
  REQUIRE(w.update.start(unit.request(Kind::kRoleRepeater)));
  end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);
  CHECK(statesOf(w) == std::vector<State>{State::kRollingBack, State::kRollbackBooting,
                                          State::kRollbackProving, State::kDone});
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.read(Which::kCandidate).present);
  CHECK(unit.world.store.store.badCount() == 0);
}

TEST_CASE("a scanner image that does not show itself: rolled back to the golden image, the scanner on the bad list") {
  ReadyUnit unit;
  const bench::Candidate scanner("store-scanner");
  REQUIRE(unit.world.store.put(Which::kScanner, bench::Slot{0}, scanner) == bench::StoreError::kNone);
  unit.world.boot = Boot::kSilent;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request(Kind::kRoleScanner)));
  const State end = w.run([&](const upd::StepResult& s) {
    if (s.changed && s.after == State::kRollingBack) unit.world.boot = Boot::kNormal;
  });
  INFO(w.transitionText());
  CHECK(end == State::kRestored);
  CHECK(w.update.reason() == Reason::kNotAScanner);
  CHECK(unit.world.store.store.isBad(unit.read(Which::kScanner).sha256));
  CHECK(unit.world.running_version == unit.golden.version);
}

TEST_CASE("the link major: a radio that refuses HELLO for this Walter's major fails proving") {
  // When the candidate comes up, every HELLO is refused: §5.1's ERR_UNSUPPORTED, a radio that
  // does not speak this Walter's major. It answered, so it booted; it is not one this Walter can
  // run, and it goes on the bad list like any other failure.
  failsAndIsRestored(Boot::kRefuseHello, Reason::kLinkMajor, upd::ProveStep::kHello);
}

TEST_CASE("a flash that fails - the target goes silent - rolls back when the chip answers again") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  // The chip loses power at its 20th flash operation of the candidate's flash: the flasher
  // fails, the machine rolls back, and the golden image is flashed once the chip is back.
  unit.world.chip.lose_power_at_op = 20;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run([&](const upd::StepResult& s) {
    if (s.changed && s.after == State::kRollingBack) {
      unit.world.chip.powerOn();
      w.target.silent = false;
      unit.world.seen_resets = unit.world.chip.counters().resets;  // a power-on is not a boot the bench owes a HELLO for... it is
      unit.world.boot_pending = true;
      unit.world.boot_due_ms = unit.world.clock.nowMs() + unit.world.boot_delay_ms;
    }
  });
  INFO(w.transitionText());
  CHECK(end == State::kRestored);
  CHECK(w.update.reason() == Reason::kFlashFailed);
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.store.store.isBad(unit.read(Which::kCandidate).sha256));
}
