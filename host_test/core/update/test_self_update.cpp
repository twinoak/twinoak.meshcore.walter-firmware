// The Walter's own update (task p2-04), on the bench of self_bench.h: the good path end to end -
// received, switched, rebooted, on trial, confirmed; the trial failing on each condition alone,
// on a restart during it, on the window passing - each rolled back by the bootloader's rules and
// this machine's, the hash on the bad list, the same image refused if offered again; a radio
// known dead before the update not condemning the image, a radio alive before and silent after
// condemning it; every precondition; the refusals of an image at the door; the journal's story
// after each outcome; and the power going while the other slot is written. Phase 2's rule: a test
// in which nothing goes wrong proves very little here.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/update/self_update.h"
#include "self_bench.h"

using bench::Candidate;
using bench::Condition;
using bench::SelfBoot;
using bench::SelfPayload;
using bench::SelfReason;
using bench::SelfState;
using bench::SelfUnit;
namespace upd = core::update;
using Own = interfaces::OwnImageState;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

std::uint8_t sha16Match(const std::uint8_t* full, const SelfPayload& p) {
  for (std::size_t i = 0; i < upd::kHash16; ++i) {
    if (full[i] != p.hash16[i]) return 0;
  }
  return 1;
}

}  // namespace

TEST_CASE("the good path: received into the other slot, switched, one trial with all three conditions, confirmed") {
  SelfUnit unit;
  const Candidate image("self-a");
  bench::SelfWalter* w = unit.walter.get();
  CHECK(w->boot() == SelfBoot::kNothing);
  CHECK(unit.world.own.runningIndex() == 0);
  CHECK(unit.world.own.slot(0).state == Own::kValid);  // the bootloader's own entry for ota_0 at the first boot

  // The transfer, through the six checks, into slot 1; the journal says received.
  bool stored = false;
  const std::string text = w->pushSelf(image, &stored);
  INFO(text);
  REQUIRE(stored);
  CHECK(unit.world.own.slot(1).has_image);
  CHECK(unit.world.own.slot(1).version == image.version);
  SelfPayload p = unit.lastEntry();
  CHECK(p.state == SelfState::kReceived);
  CHECK(p.slot == 1);
  CHECK(p.size == image.file.size());
  CHECK(std::string(p.to) == "0.4.0");
  std::uint8_t digest[32];
  core::image::sha256(image.file.data(), image.file.size(), digest);
  CHECK(sha16Match(digest, p) == 1);
  SelfPayload got;
  CHECK(w->self.received(&got));
  CHECK(got.slot == 1);

  // The switch: journalled, the boot pointer, SLEEP_NOTICE, the restart.
  const std::size_t requests_before = unit.world.radio.requests.size();
  const std::string switched = w->runLine("self switch");
  INFO(switched);
  CHECK(has(switched, "self switch: making 0.4.0 (slot 1) the next to boot"));
  CHECK(w->restart.asked == 1);
  CHECK(unit.world.own.slot(1).state == Own::kNew);
  p = unit.lastEntry();
  CHECK(p.state == SelfState::kSwitching);
  CHECK((p.flags & upd::kFlagRadioUp) != 0);
  bool sleep_notice = false;
  for (std::size_t i = requests_before; i < unit.world.radio.requests.size(); ++i) {
    if (unit.world.radio.requests[i].type == core::link::type::kSleepNotice) sleep_notice = true;
  }
  CHECK(sleep_notice);
  CHECK(w->health.thisBoot().reason == core::health::ResetReason::kPowerOn);

  // The restart: the bootloader marks slot 1 pending-verify and boots it. One trial.
  unit.reboot();
  w = unit.walter.get();
  CHECK(unit.world.own.runningIndex() == 1);
  CHECK(unit.world.own.slot(1).state == Own::kPendingVerify);
  CHECK(w->health.thisBoot().requested == core::health::Restart::kSelfUpdateSwitch);
  CHECK(w->boot() == SelfBoot::kTrialBegun);
  CHECK(w->self.onTrial());
  CHECK(unit.lastEntry().state == SelfState::kOnTrial);
  const upd::SelfStep end = w->runTrial();
  INFO(end.note);
  CHECK(end.changed);
  CHECK(end.after == SelfState::kConfirmed);
  CHECK_FALSE(w->self.onTrial());
  CHECK(unit.world.own.slot(1).state == Own::kValid);
  CHECK(unit.world.own.confirms == 1);
  CHECK(w->uplink.checks == 1);
  const upd::SelfStatus& st = w->self.status();
  CHECK(st.main_loop == Condition::kMet);
  CHECK(st.radio == Condition::kMet);
  CHECK(st.uplink == Condition::kMet);
  CHECK(st.trial_elapsed_ms < upd::kTrialWindowMs);
  p = unit.lastEntry();
  CHECK(p.state == SelfState::kConfirmed);
  CHECK((p.flags & upd::kFlagRadioNotApplicable) == 0);
  CHECK((p.flags & upd::kFlagUplinkNotApplicable) == 0);
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(unit.story() == std::vector<std::pair<SelfState, SelfReason>>{
                            {SelfState::kReceived, SelfReason::kNone}, {SelfState::kSwitching, SelfReason::kNone},
                            {SelfState::kOnTrial, SelfReason::kNone}, {SelfState::kConfirmed, SelfReason::kNone}});

  // And the next boot is the confirmed image, with nothing to do.
  unit.reboot();
  CHECK(unit.world.own.runningIndex() == 1);
  CHECK(unit.walter->boot() == SelfBoot::kNothing);
  CHECK_FALSE(unit.walter->self.received(&got));  // consumed
}

namespace {

// A trial that fails: the image is received and switched to, boots, and the knob makes one
// condition fail. Expected: rolled back with `reason`, the hash on the bad list, the old image
// running after the reboot, the same image refused if offered again.
struct FailedTrial {
  SelfUnit unit;
  Candidate image{"self-a"};
  std::uint8_t digest[32];

  FailedTrial() {
    core::image::sha256(image.file.data(), image.file.size(), digest);
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    REQUIRE(unit.receiveAndSwitch(image));
    unit.reboot();
    REQUIRE(unit.world.own.runningIndex() == 1);
  }

  void checkRolledBack(SelfReason reason, bool restart_by_image = true) {
    const SelfPayload p = unit.lastEntry();
    CHECK(p.state == SelfState::kRolledBack);
    CHECK(p.reason == reason);
    CHECK((p.flags & upd::kFlagNotOnBadList) == 0);
    CHECK(unit.world.store.store.isBad(digest));
    if (restart_by_image) {
      CHECK(unit.world.own.slot(1).state == Own::kInvalid);
      CHECK(unit.world.own.restart_asked);
    }
    // After the restart the old image runs; nothing is left to do; the image is refused again.
    unit.reboot();
    CHECK(unit.world.own.runningIndex() == 0);
    CHECK(unit.walter->boot() == SelfBoot::kNothing);
    CHECK_FALSE(unit.walter->self.onTrial());
    bool stored = true;
    const int erases = unit.world.own.erases;
    const std::string again = unit.walter->pushSelf(image, &stored);
    INFO(again);
    CHECK_FALSE(stored);
    CHECK(has(again, "failed its trial once and is never tried again"));
    CHECK(unit.world.own.erases == erases);  // refused at the door: the slot was not touched
    SelfPayload got;
    CHECK_FALSE(unit.walter->self.received(&got));
  }
};

}  // namespace

TEST_CASE("the trial fails on the radio alone: alive before the switch, silent after it") {
  FailedTrial t;
  t.unit.world.radio.deaf = true;  // the new image cannot talk to the radio
  bench::SelfWalter& w = *t.unit.walter;
  CHECK(w.boot() == SelfBoot::kTrialBegun);
  const upd::SelfStep end = w.runTrial();
  CHECK(end.changed);
  CHECK(end.after == SelfState::kRolledBack);
  CHECK(end.reason == SelfReason::kNoRadio);
  CHECK(w.self.status().trial_elapsed_ms >= upd::kTrialWindowMs);
  CHECK(w.self.status().radio == Condition::kPending);
  CHECK(w.self.status().hello_tries >= upd::kTrialWindowMs / (upd::kTrialHelloIntervalMs + 3000) / 2);
  CHECK(w.uplink.checks == 0);  // the uplink is not even asked while the radio is open
  t.checkRolledBack(SelfReason::kNoRadio);
}

TEST_CASE("the trial fails on the uplink alone: the server is configured and not reached") {
  FailedTrial t;
  bench::SelfWalter& w = *t.unit.walter;
  w.uplink.verdict = upd::UplinkVerdict::kFailed;
  CHECK(w.boot() == SelfBoot::kTrialBegun);
  const upd::SelfStep end = w.runTrial();
  CHECK(end.after == SelfState::kRolledBack);
  CHECK(end.reason == SelfReason::kNoUplink);
  CHECK(w.self.status().radio == Condition::kMet);
  CHECK(w.self.status().uplink == Condition::kFailed);
  // Tried again every kTrialUplinkRetryMs through the window.
  CHECK(w.uplink.checks >= upd::kTrialWindowMs / upd::kTrialUplinkRetryMs - 1);
  t.checkRolledBack(SelfReason::kNoUplink);
}

TEST_CASE("the trial fails on the main loop alone: begun, never ticked, the window passes") {
  FailedTrial t;
  bench::SelfWalter& w = *t.unit.walter;
  CHECK(w.self.begin() == SelfBoot::kTrialBegun);  // and no mainLoopReached()
  const upd::SelfStep end = w.runTrial();
  CHECK(end.after == SelfState::kRolledBack);
  CHECK(end.reason == SelfReason::kNoMainLoop);
  CHECK(w.self.status().main_loop == Condition::kPending);
  CHECK(w.self.status().hello_tries == 0);  // nothing is asked of the world before the loop turns
  t.checkRolledBack(SelfReason::kNoMainLoop);
}

TEST_CASE("the trial is cut short by a restart: the bootloader aborts the image and the old one records why") {
  FailedTrial t;
  bench::SelfWalter& w = *t.unit.walter;
  CHECK(w.boot() == SelfBoot::kTrialBegun);
  // A minute in, with the radio answered and the uplink not yet, the Walter resets (a panic, a
  // watchdog, a power glitch): no confirmation was written.
  w.uplink.verdict = upd::UplinkVerdict::kFailed;
  w.runTrial(60000);
  CHECK(w.self.onTrial());
  CHECK(t.unit.lastEntry().state == SelfState::kOnTrial);
  t.unit.reboot();
  // The bootloader: pending -> aborted; the old image in slot 0 runs again.
  CHECK(t.unit.world.own.slot(1).state == Own::kAborted);
  CHECK(t.unit.world.own.runningIndex() == 0);
  CHECK(t.unit.walter->boot() == SelfBoot::kRollbackRecorded);
  CHECK(t.unit.walter->self.bootReason() == SelfReason::kRestartedDuringTrial);
  t.checkRolledBack(SelfReason::kRestartedDuringTrial, false);
}

TEST_CASE("the new image never reaches its main loop: switching in the journal, the slot aborted, the old image records it") {
  FailedTrial t;
  // The reboot after the switch booted slot 1 pending-verify; the image crashes before begin()
  // is ever called (nothing journalled), and the next reset aborts it.
  CHECK(t.unit.lastEntry().state == SelfState::kSwitching);
  t.unit.reboot();
  CHECK(t.unit.world.own.slot(1).state == Own::kAborted);
  CHECK(t.unit.world.own.runningIndex() == 0);
  CHECK(t.unit.walter->boot() == SelfBoot::kRollbackRecorded);
  CHECK(t.unit.walter->self.bootReason() == SelfReason::kNeverBooted);
  t.checkRolledBack(SelfReason::kNeverBooted, false);
}

TEST_CASE("the window passes with two conditions missing: the first missing one names the rollback") {
  FailedTrial t;
  t.unit.world.radio.deaf = true;
  t.unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;
  CHECK(t.unit.walter->boot() == SelfBoot::kTrialBegun);
  const upd::SelfStep end = t.unit.walter->runTrial();
  CHECK(end.reason == SelfReason::kNoRadio);
  t.checkRolledBack(SelfReason::kNoRadio);
}

TEST_CASE("self reject on the console: rolled back by hand, on the bad list, the old image back") {
  FailedTrial t;
  bench::SelfWalter& w = *t.unit.walter;
  CHECK(w.boot() == SelfBoot::kTrialBegun);
  CHECK(w.self.rejectNow() == SelfReason::kNone);
  CHECK((t.unit.lastEntry().flags & upd::kFlagByHand) != 0);
  t.checkRolledBack(SelfReason::kRejectedByHand);
}

TEST_CASE("a radio known dead before the update does not condemn the image; one alive before and silent after does") {
  SUBCASE("known down: the switch is told, the trial's second condition does not apply") {
    SelfUnit unit;
    unit.world.radio.deaf = true;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    // Without the word, the switch is refused: the radio is silent and nobody has said why.
    const Candidate image("self-a");
    bool stored = false;
    (void)unit.walter->pushSelf(image, &stored);
    REQUIRE(stored);
    CHECK(has(unit.walter->runLine("self switch"), "error: self switch: the radio does not answer HELLO and nobody has said why"));
    CHECK(unit.walter->restart.asked == 0);
    CHECK(unit.lastEntry().state == SelfState::kReceived);
    // With it: switching, flagged.
    CHECK(has(unit.walter->runLine("self switch radio-down"), "self switch: restarting"));
    SelfPayload p = unit.lastEntry();
    CHECK(p.state == SelfState::kSwitching);
    CHECK((p.flags & upd::kFlagRadioKnownDown) != 0);
    CHECK((p.flags & upd::kFlagRadioUp) == 0);
    unit.reboot();
    CHECK(unit.walter->boot() == SelfBoot::kTrialBegun);
    CHECK(unit.walter->self.status().radio == Condition::kNotApplicable);
    const upd::SelfStep end = unit.walter->runTrial();
    CHECK(end.after == SelfState::kConfirmed);
    CHECK(unit.walter->self.status().hello_tries == 0);
    p = unit.lastEntry();
    CHECK((p.flags & upd::kFlagRadioNotApplicable) != 0);
  }
  SUBCASE("alive before, silent after: rolled back for the radio") {
    FailedTrial t;
    CHECK((t.unit.lastEntry().flags & upd::kFlagRadioUp) != 0);  // the switching entry saw it answer
    t.unit.world.radio.deaf = true;
    CHECK(t.unit.walter->boot() == SelfBoot::kTrialBegun);
    CHECK(t.unit.walter->runTrial().reason == SelfReason::kNoRadio);
    t.checkRolledBack(SelfReason::kNoRadio);
  }
}

TEST_CASE("no server configured: the third condition does not apply and the image confirms on the first two") {
  // The rule that keeps a factory image - and every unit until phase 3 - from rejecting itself
  // for want of a server (core/update/uplink_check.h).
  SelfUnit unit;
  REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
  REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
  unit.reboot();
  unit.walter->uplink.verdict = upd::UplinkVerdict::kNotConfigured;
  CHECK(unit.walter->boot() == SelfBoot::kTrialBegun);
  const upd::SelfStep end = unit.walter->runTrial();
  CHECK(end.after == SelfState::kConfirmed);
  CHECK(unit.walter->self.status().uplink == Condition::kNotApplicable);
  CHECK((unit.lastEntry().flags & upd::kFlagUplinkNotApplicable) != 0);
  // And the firmware's own implementation says exactly that, until a host is configured.
  upd::UnconfiguredUplink none;
  CHECK(none.check() == upd::UplinkVerdict::kNotConfigured);
  CHECK(std::string(upd::kSelfUpdateServerHost).empty());
  upd::UnconfiguredUplink named("server.example", 443);
  CHECK(named.check() == upd::UplinkVerdict::kFailed);  // a server named, no way to reach it: not confirmed blind
}

TEST_CASE("a factory image's first boot: no trial, nothing journalled, nothing rejected") {
  // Flashed over USB with blank otadata: the bootloader's first boot gives ota_0 an entry of its
  // own in the state VALID (bootloader_utility.c, set_actual_ota_seq) - never NEW, which is the
  // only state it turns into a trial (interfaces/own_update.h). Whatever the uplink says, this
  // image is not judged: the rule is "a trial runs only for an image an update made the next to
  // boot", and the uplink's "not configured" is the second guard behind it.
  SelfUnit unit;
  unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;
  CHECK(unit.world.own.slot(0).state == Own::kValid);
  CHECK(unit.walter->boot() == SelfBoot::kNothing);
  CHECK_FALSE(unit.walter->self.onTrial());
  for (int i = 0; i < 100; ++i) {
    const upd::SelfStep s = unit.walter->self.step();
    CHECK(s.waited);
    CHECK_FALSE(s.changed);
    unit.world.clock.delayMs(10000);
  }
  upd::Entry e;
  CHECK_FALSE(unit.walter->journal.lastOf(upd::kind::kSelfUpdate, &e));
  CHECK(unit.world.own.confirms == 0);
  CHECK_FALSE(unit.world.own.restart_asked);
  // Three resets later it is still the same image, still valid, still no trial.
  for (int i = 0; i < 3; ++i) {
    unit.reboot();
    CHECK(unit.world.own.runningIndex() == 0);
    CHECK(unit.world.own.slot(0).state == Own::kValid);
    CHECK(unit.walter->boot() == SelfBoot::kNothing);
  }
}

TEST_CASE("an image pending-verify with no switching entry runs a trial all the same") {
  SelfUnit unit;
  unit.world.own.flashOverUsb(1, {}, "0.9.9+byhand");
  REQUIRE(unit.world.own.setNextBoot());
  unit.reboot();
  CHECK(unit.world.own.runningIndex() == 1);
  CHECK(unit.walter->boot() == SelfBoot::kTrialBegun);
  CHECK((unit.lastEntry().flags & upd::kFlagUnjournalled) != 0);
  CHECK(std::string(unit.lastEntry().to) == "0.9.9");
  unit.world.radio.deaf = true;
  const upd::SelfStep end = unit.walter->runTrial();
  CHECK(end.after == SelfState::kRolledBack);
  CHECK(end.reason == SelfReason::kNoRadio);
  CHECK((unit.lastEntry().flags & upd::kFlagNotOnBadList) != 0);  // nothing is known of the image
  CHECK(unit.world.store.store.badCount() == 0);
}

TEST_CASE("preconditions: a radio update in progress, the running image unconfirmed, the battery") {
  SUBCASE("a radio update journalled as in progress refuses receive and switch (S-7)") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    upd::Payload p;
    p.state = upd::State::kFlashing;
    std::uint8_t bytes[upd::kPayloadBytes];
    upd::encode(p, bytes);
    REQUIRE(unit.walter->journal.append(upd::kind::kRadioUpdate, bytes, sizeof bytes) == upd::JournalError::kNone);
    CHECK(unit.walter->self.canReceive() == SelfReason::kRadioUpdateInProgress);
    CHECK(has(unit.walter->runLine("self receive"), "error: self receive: the journal says a radio update is in progress: never both at once (S-7)"));
    CHECK_FALSE(unit.walter->receiver.active());
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kRadioUpdateInProgress);
    // Ended: allowed again.
    p.state = upd::State::kDone;
    upd::encode(p, bytes);
    REQUIRE(unit.walter->journal.append(upd::kind::kRadioUpdate, bytes, sizeof bytes) == upd::JournalError::kNone);
    CHECK(unit.walter->self.canReceive() == SelfReason::kNone);
  }
  SUBCASE("the running image on trial refuses both: never stack an update on an unconfirmed one") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
    unit.reboot();
    CHECK(unit.walter->boot() == SelfBoot::kTrialBegun);
    CHECK(unit.walter->self.canReceive() == SelfReason::kRunningUnconfirmed);
    bool stored = true;
    const std::string text = unit.walter->pushSelf(Candidate("self-b"), &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "error: self receive: the running image is itself on trial"));
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kRunningUnconfirmed);
    // And the radio's update machine refuses too, reading the same entry (task p2-03's check).
    upd::Entry e;
    REQUIRE(unit.walter->journal.lastOf(upd::kind::kSelfUpdate, &e));
    CHECK(upd::self_update::unconfirmed(e.payload[0]));
  }
  SUBCASE("the battery: switch refused, receive allowed (an interrupted write is harmless)") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    unit.walter->battery.millivolts = upd::kBatteryMinMv - 1;
    bool stored = false;
    (void)unit.walter->pushSelf(Candidate("self-a"), &stored);
    CHECK(stored);
    CHECK(has(unit.walter->runLine("self switch"), "error: self switch: the battery is below the update threshold"));
    CHECK(unit.walter->restart.asked == 0);
    CHECK(unit.lastEntry().state == SelfState::kReceived);
    unit.walter->battery.millivolts = 4000;
    unit.walter->battery.trend_known = true;
    unit.walter->battery.mv_per_hour = upd::kBatteryFallingMvPerHour;
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kBatteryFalling);
    unit.walter->battery.known = false;
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kBatteryUnknown);
    unit.walter->battery.known = true;
    unit.walter->battery.trend_known = false;
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kNone);
    CHECK(unit.walter->restart.asked == 1);
  }
  SUBCASE("nothing received: switch refused; the other slot changed: switch refused") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kNothingReceived);
    bool stored = false;
    (void)unit.walter->pushSelf(Candidate("self-a"), &stored);
    REQUIRE(stored);
    unit.world.own.slot(1).bytes[100] ^= 0x01;  // a bit rotted in the other slot
    CHECK(unit.walter->self.switchNow(upd::SwitchRequest{}) == SelfReason::kOtherSlotChanged);
    CHECK(unit.walter->restart.asked == 0);
  }
  SUBCASE("self confirm and self reject when not on trial") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    CHECK(unit.walter->self.confirmNow() == SelfReason::kNotOnTrial);
    CHECK(unit.walter->self.rejectNow() == SelfReason::kNotOnTrial);
    CHECK(unit.world.own.confirms == 0);
  }
}

TEST_CASE("an image at the door: the radio's offered as the Walter's, one too large, one whose load_address is not a slot's, the link majors") {
  SelfUnit unit;
  REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
  bool stored = true;
  SUBCASE("a radio image: refused at check 3") {
    const std::string text = unit.walter->pushSelf(Candidate("update-golden"), &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "check 3"));
    CHECK(has(text, "no image in the manifest has this slot's role, board and band"));
  }
  SUBCASE("too large for the other slot") {
    const std::string text = unit.walter->pushSelf(Candidate("self-big"), &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "the file is larger than the other application slot"));
    CHECK(unit.world.own.erases == 0);  // refused before the slot was touched
  }
  SUBCASE("load_address not a slot's offset: the specification's own walter fixture (0x20000)") {
    Candidate spec("self-a");
    spec.manifest = fixtures::readSpec("manifest-walter.json");
    spec.signature = fixtures::asText(fixtures::readSpec("manifest-walter.json.sig"));
    spec.file = fixtures::readSpec("image-walter-ota.bin");
    const std::string text = unit.walter->pushSelf(spec, &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "the file's load_address is not an application slot's offset"));
    CHECK(unit.world.own.erases == 0);
  }
  SUBCASE("load_address ota_1's: either slot's offset is right") {
    (void)unit.walter->pushSelf(Candidate("self-other-slot"), &stored);
    CHECK(stored);
  }
  SUBCASE("speaks link major 2 and says it tolerates the radio's 1: allowed (S-7, the Walter goes first)") {
    (void)unit.walter->pushSelf(Candidate("self-link2-both"), &stored);
    CHECK(stored);
    CHECK(std::string(unit.lastEntry().to) == "0.5.0");
  }
  SUBCASE("speaks link major 2 and requires 2: refused at check 3, this Walter does not speak it") {
    const std::string text = unit.walter->pushSelf(Candidate("self-link2-only"), &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "check 3"));
    CHECK(has(text, "requires a link major this build does not speak"));
  }
  SUBCASE("speaks link major 2 and says nothing of 1: refused by the self-update's rule") {
    const std::string text = unit.walter->pushSelf(Candidate("self-link2-silent"), &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "does not say it tolerates the radio's"));
    CHECK(unit.world.own.erases == 0);
  }
  SUBCASE("the radio's current major is the session's when it is up") {
    // A radio that speaks major 2 already (the Walter went first, the radio followed): an image
    // speaking 2 with no requires is for it; an image speaking 1 that says nothing of 2 is not.
    // The scripted radio's HELLO body comes from the fixtures (major 1), so the session is
    // arranged by hand through a radio that answers major 2 - not available; what can be shown is
    // the rule with the link down: this build's own major stands in.
    unit.world.radio.deaf = true;
    const std::string text = unit.walter->pushSelf(Candidate("self-link2-silent"), &stored);
    CHECK_FALSE(stored);
    CHECK(has(text, "does not say it tolerates the radio's"));
  }
  SUBCASE("a second image replaces a received one: the newer is what self switch takes") {
    (void)unit.walter->pushSelf(Candidate("self-a"), &stored);
    REQUIRE(stored);
    (void)unit.walter->pushSelf(Candidate("self-b"), &stored);
    REQUIRE(stored);
    SelfPayload got;
    REQUIRE(unit.walter->self.received(&got));
    CHECK(std::string(got.to) == "0.4.1");
    CHECK(got.size == Candidate("self-b").file.size());
  }
}

TEST_CASE("the journal's story reads correctly after a confirmed update and after each kind of rollback") {
  using Story = std::vector<std::pair<SelfState, SelfReason>>;
  SUBCASE("confirmed") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
    unit.reboot();
    REQUIRE(unit.walter->boot() == SelfBoot::kTrialBegun);
    REQUIRE(unit.walter->runTrial().after == SelfState::kConfirmed);
    unit.reboot();
    CHECK(unit.story() == Story{{SelfState::kReceived, SelfReason::kNone}, {SelfState::kSwitching, SelfReason::kNone},
                                {SelfState::kOnTrial, SelfReason::kNone}, {SelfState::kConfirmed, SelfReason::kNone}});
    const std::string text = unit.walter->runLine("self status");
    INFO(text);
    CHECK(has(text, "running: slot 1"));
    CHECK(has(text, "valid (confirmed), version 0.4.0+w1"));
    CHECK(has(text, "the running image is confirmed"));
    CHECK(has(text, "1. seq 1: received 0.4.0 into slot 1 ("));
    CHECK(has(text, "2. seq 2: switching to 0.4.0, from 0.0.0-dev (the radio answered)"));  // this build's own version
    CHECK(has(text, "3. seq 3: on trial: 0.4.0 in slot 1"));
    CHECK(has(text, "4. seq 4: confirmed: all three conditions"));
  }
  SUBCASE("rolled back for the radio, read by the old image") {
    FailedTrial t;
    t.unit.world.radio.deaf = true;
    REQUIRE(t.unit.walter->boot() == SelfBoot::kTrialBegun);
    REQUIRE(t.unit.walter->runTrial().after == SelfState::kRolledBack);
    t.unit.reboot();
    CHECK(t.unit.story() == Story{{SelfState::kReceived, SelfReason::kNone}, {SelfState::kSwitching, SelfReason::kNone},
                                  {SelfState::kOnTrial, SelfReason::kNone}, {SelfState::kRolledBack, SelfReason::kNoRadio}});
    CHECK(t.unit.walter->boot() == SelfBoot::kNothing);
    const std::string text = t.unit.walter->runLine("self status");
    INFO(text);
    CHECK(has(text, "running: slot 0"));
    CHECK(has(text, "other:   slot 1"));
    CHECK(has(text, "invalid (rejected by the image itself)"));
    CHECK(has(text, "4. seq 4: rolled back: the radio answered before the update and not after it"));
  }
  SUBCASE("rolled back for the uplink") {
    FailedTrial t;
    t.unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;
    REQUIRE(t.unit.walter->boot() == SelfBoot::kTrialBegun);
    REQUIRE(t.unit.walter->runTrial().after == SelfState::kRolledBack);
    t.unit.reboot();
    CHECK(has(t.unit.walter->runLine("self status"), "rolled back: the backend was not reached inside the trial window"));
  }
  SUBCASE("rolled back: restarted during the trial") {
    FailedTrial t;
    t.unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;
    REQUIRE(t.unit.walter->boot() == SelfBoot::kTrialBegun);
    t.unit.walter->runTrial(30000);
    t.unit.reboot();
    REQUIRE(t.unit.walter->boot() == SelfBoot::kRollbackRecorded);
    CHECK(t.unit.story() == Story{{SelfState::kReceived, SelfReason::kNone}, {SelfState::kSwitching, SelfReason::kNone},
                                  {SelfState::kOnTrial, SelfReason::kNone},
                                  {SelfState::kRolledBack, SelfReason::kRestartedDuringTrial}});
    CHECK(has(t.unit.walter->runLine("self status"), "rolled back: the image restarted during its trial"));
    CHECK(has(t.unit.walter->runLine("self status"), "aborted (was on trial at a reset"));
  }
  SUBCASE("rolled back: never booted") {
    FailedTrial t;
    t.unit.reboot();
    REQUIRE(t.unit.walter->boot() == SelfBoot::kRollbackRecorded);
    CHECK(t.unit.story() == Story{{SelfState::kReceived, SelfReason::kNone}, {SelfState::kSwitching, SelfReason::kNone},
                                  {SelfState::kRolledBack, SelfReason::kNeverBooted}});
    CHECK(has(t.unit.walter->runLine("self status"), "rolled back: the image never reached its main loop"));
  }
  SUBCASE("the platform refuses the boot pointer: journalled as rolled back (the platform refused), no restart") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    const Candidate image("self-a");
    bool stored = false;
    (void)unit.walter->pushSelf(image, &stored);
    REQUIRE(stored);
    // The slots' flash fails at the switch's one operation after the journal entry: setNextBoot.
    unit.world.own.powerOn();
    unit.world.own.lose_power_at_op = 1;
    const std::string text = unit.walter->runLine("self switch");
    INFO(text);
    CHECK(has(text, "error: self switch: the platform refused"));
    CHECK(unit.walter->restart.asked == 0);
    CHECK(unit.lastEntry().state == SelfState::kRolledBack);
    CHECK(unit.lastEntry().reason == SelfReason::kPlatform);
    CHECK(unit.world.store.store.badCount() == 0);
    unit.reboot();
    CHECK(unit.world.own.runningIndex() == 0);
    CHECK(unit.walter->boot() == SelfBoot::kNothing);
    SelfPayload got;
    CHECK_FALSE(unit.walter->self.received(&got));  // the story was closed; receive again
  }
  SUBCASE("the switch journalled and the power gone before the pointer: recorded as not taken, nothing on the bad list") {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    bool stored = false;
    (void)unit.walter->pushSelf(Candidate("self-a"), &stored);
    REQUIRE(stored);
    // Arrange the journal as a Walter that died right after writing *switching*: the entry there,
    // the other slot's state still undefined.
    SelfPayload p = unit.lastEntry();
    p.state = SelfState::kSwitching;
    p.flags = upd::kFlagRadioUp;
    std::uint8_t bytes[upd::kSelfPayloadBytes];
    upd::encode(p, bytes);
    REQUIRE(unit.walter->journal.append(upd::kind::kSelfUpdate, bytes, sizeof bytes) == upd::JournalError::kNone);
    unit.reboot();
    CHECK(unit.world.own.runningIndex() == 0);
    CHECK(unit.walter->boot() == SelfBoot::kRollbackRecorded);
    CHECK(unit.walter->self.bootReason() == SelfReason::kSwitchNotTaken);
    CHECK(unit.world.store.store.badCount() == 0);
    SelfPayload got;
    CHECK(unit.walter->self.received(&got));  // still there: say `self switch` again
    CHECK(std::string(got.to) == "0.4.0");
    CHECK(has(unit.walter->runLine("self status"), "rolled back: the restart came before the other slot was made the next to boot"));
    CHECK(has(unit.walter->runLine("self status"), "received: 0.4.0 in slot 1"));
  }
}

TEST_CASE("the power goes while the other slot is written: the running image is untouched and the write can be repeated") {
  const Candidate image("self-a");
  // The clean push's operations on the slots: the erase, then the writes.
  std::uint32_t ops = 0;
  {
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    bool stored = false;
    (void)unit.walter->pushSelf(image, &stored);
    REQUIRE(stored);
    // One erase and 240-byte writes: count them back from the fake's log of operations.
    ops = 1 + static_cast<std::uint32_t>((image.file.size() + 239) / 240);
  }
  REQUIRE(ops > 10);
  int cuts = 0;
  for (std::uint32_t op = 1; op <= ops; op += 7) {
    CAPTURE(op);
    SelfUnit unit;
    REQUIRE(unit.walter->boot() == SelfBoot::kNothing);
    const std::vector<std::uint8_t> running_before = unit.world.own.slot(0).bytes;
    unit.world.own.lose_power_at_op = op;
    unit.world.own.on_power_loss = [&] {
      unit.world.journal_flash.powerOff();
      unit.world.store.flash.powerOff();
    };
    bool stored = true;
    const std::string text = unit.walter->pushSelf(image, &stored);
    INFO(text);
    CHECK_FALSE(stored);
    ++cuts;
    // The running slot: byte for byte as it was, its state too.
    CHECK(unit.world.own.slot(0).bytes == running_before);
    CHECK(unit.world.own.slot(0).state == Own::kValid);
    CHECK_FALSE(unit.world.own.slot(1).has_image);
    // The Walter comes back; nothing is journalled as received; the same push succeeds.
    unit.world.own.on_power_loss = nullptr;
    unit.reboot();
    CHECK(unit.world.own.runningIndex() == 0);
    CHECK(unit.walter->boot() == SelfBoot::kNothing);
    SelfPayload got;
    CHECK_FALSE(unit.walter->self.received(&got));
    stored = false;
    (void)unit.walter->pushSelf(image, &stored);
    CHECK(stored);
    CHECK(unit.world.own.slot(1).has_image);
    CHECK(unit.world.own.slot(0).bytes == running_before);
  }
  MESSAGE("the power cut at " << cuts << " points of the other slot's write; the running image untouched each time");
}

TEST_CASE("the payload encodes and decodes, versions shortened to X.Y.Z") {
  SelfPayload p;
  p.state = SelfState::kSwitching;
  p.slot = 1;
  p.reason = SelfReason::kNone;
  p.flags = upd::kFlagRadioUp;
  p.size = 123456;
  for (std::size_t i = 0; i < upd::kHash16; ++i) p.hash16[i] = static_cast<std::uint8_t>(0xA0 + i);
  upd::shortVersion("0.3.0+abc1234", p.from);
  upd::shortVersion("10.20.30+def5678", p.to);
  CHECK(std::string(p.from) == "0.3.0");
  CHECK(std::string(p.to) == "10.20.30");
  std::uint8_t bytes[upd::kSelfPayloadBytes];
  upd::encode(p, bytes);
  upd::Entry e;
  e.kind = upd::kind::kSelfUpdate;
  e.size = upd::kSelfPayloadBytes;
  for (std::size_t i = 0; i < upd::kSelfPayloadBytes; ++i) e.payload[i] = bytes[i];
  SelfPayload q;
  REQUIRE(upd::decode(e, &q));
  CHECK(q.state == p.state);
  CHECK(q.slot == 1);
  CHECK(q.flags == p.flags);
  CHECK(q.size == 123456);
  CHECK(std::string(q.from) == "0.3.0");
  CHECK(std::string(q.to) == "10.20.30");
  CHECK(sha16Match(p.hash16, q) == 1);
  // A radio-update entry is not a self-update entry, and an unknown state byte is refused.
  e.kind = upd::kind::kRadioUpdate;
  CHECK_FALSE(upd::decode(e, &q));
  e.kind = upd::kind::kSelfUpdate;
  e.payload[0] = 9;
  CHECK_FALSE(upd::decode(e, &q));
  // A long version is cut at eleven characters, terminated.
  char cut[upd::kShortVersion];
  upd::shortVersion("123456789012345-dev", cut);
  CHECK(std::string(cut) == "12345678901");
}
