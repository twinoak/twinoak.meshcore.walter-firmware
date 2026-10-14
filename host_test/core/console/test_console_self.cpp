// The console's `self status`, `self receive`, `self switch`, `self confirm` and `self reject`
// (task p2-04): the lines a person reads, the boot line of a trial and of a rollback, the trial
// driven by tick() as main drives it, and what a build with no self-update says.
#include <doctest.h>

#include <string>

#include "core/console/console.h"
#include "core/update/self_bench.h"
#include "core/update/self_update.h"

using bench::Candidate;
using bench::SelfBoot;
using bench::SelfState;
using bench::SelfUnit;
namespace upd = core::update;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// What main does: begin() at boot, then tick() every turn of the loop, the clock moving between.
std::string bootAndTick(SelfUnit& unit, std::uint64_t for_ms) {
  bench::SelfWalter& w = *unit.walter;
  w.out.clear();
  w.console.begin();
  std::string text = w.out.text;
  const std::uint64_t until = unit.world.clock.nowMs() + for_ms;
  while (unit.world.clock.nowMs() < until) {
    w.out.clear();
    w.console.tick();
    text += w.out.text;
    if (!w.self.onTrial() && text.find("self-update: on trial ->") != std::string::npos) break;
    unit.world.clock.delayMs(100);
  }
  return text;
}

}  // namespace

TEST_CASE("self status on a fresh unit, and the help") {
  SelfUnit unit;
  bench::SelfWalter& w = *unit.walter;
  w.console.begin();
  const std::string text = w.runLine("self status");
  INFO(text);
  // A unit flashed over USB: the bootloader gave ota_0 a valid entry at its first boot.
  CHECK(has(text, "running: slot 0 at 0x010000 (64 KB): valid (confirmed), version 0.0.0-dev"));
  CHECK(has(text, "other:   slot 1 at 0x310000 (64 KB): undefined (flashed over USB, not by an update; rollback does not apply), version (no image)"));
  CHECK(has(text, "the running image is confirmed"));
  CHECK(has(text, "self status: no self-update has been journalled on this unit"));
  const std::string help = w.runLine("help");
  CHECK(has(help, "self status"));
  CHECK(has(help, "self receive"));
  CHECK(has(help, "self switch [radio-down]"));
  CHECK(has(help, "self confirm"));
  CHECK(has(help, "self reject"));
  CHECK(has(w.runLine("self"), "error: self needs a subcommand"));
  CHECK(has(w.runLine("self dance"), "error: unknown self subcommand (status, receive, switch, confirm, reject): dance"));
  CHECK(has(w.runLine("self status now"), "error: self status takes no argument"));
  CHECK(has(w.runLine("self switch now"), "error: self switch: unknown word (self switch, self switch radio-down): now"));
  CHECK(has(w.runLine("self confirm"), "error: self confirm: the running image is not on trial"));
  CHECK(has(w.runLine("self reject"), "error: self reject: the running image is not on trial"));
  CHECK(has(w.runLine("self switch"), "error: self switch: no image has been received into the other slot"));
}

TEST_CASE("self receive hands the port over and says what it will take; the transfer ends with the receive line") {
  SelfUnit unit;
  bench::SelfWalter& w = *unit.walter;
  w.console.begin();
  const std::string began = w.runLine("self receive");
  INFO(began);
  CHECK(has(began, "self receive: this Walter's other application slot (1); this port is now a framed binary transfer"));
  CHECK(has(began, "tools/walterctl.py push --as self"));
  CHECK(has(began, "what will be accepted: role manager, board walter, band lte-m, an ota file that fits the slot"));
  CHECK(w.receiver.active());
  // A second receive while one runs.
  w.out.clear();
  w.console.runLine("self receive");
  CHECK(has(w.out.text, "error: self receive: a transfer is already running"));
  // The other end goes quiet: the console comes back with the self line.
  unit.world.clock.advance(core::image::transfer::kIdleTimeoutMs + 1);
  w.out.clear();
  w.console.tick();
  CHECK(has(w.out.text, "self receive: the other end went quiet: nothing stored"));
  CHECK_FALSE(w.receiver.active());
  // The whole push, through feed(): the bytes go to the transfer, and the console says stored.
  const Candidate image("self-a");
  bool stored = false;
  (void)w.pushSelf(image, &stored);
  REQUIRE(stored);
  // pushSelf drives the receiver directly; the console's own line comes from feed()'s handover.
  // Repeat the last frame's effect through feed(): the receiver is idle, so the bytes are a line.
  const std::string status = w.runLine("self status");
  INFO(status);
  CHECK(has(status, "received: 0.4.0 in slot 1 (12288 bytes), waiting for 'self switch'"));
  CHECK(has(status, "1. seq 1: received 0.4.0 into slot 1 (12288 bytes, sha256 "));
}

TEST_CASE("the boot line of a trial, the trial driven by tick(), confirmed") {
  SelfUnit unit;
  unit.walter->console.begin();
  REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
  unit.reboot();
  CHECK(unit.world.own.runningIndex() == 1);
  const std::string text = bootAndTick(unit, upd::kTrialWindowMs);
  INFO(text);
  CHECK(has(text, "boot 2: software reset, the Walter's own update, switching to the other slot"));
  CHECK(has(text, "self-update: this image (0.4.0, slot 1) is on trial: 300 s to reach the main loop, HELLO the radio"));
  CHECK(has(text, "self-update: on trial -> confirmed: all three conditions hold: confirmed"));
  CHECK(unit.world.own.slot(1).state == interfaces::OwnImageState::kValid);
  const std::string status = unit.walter->runLine("self status");
  INFO(status);
  CHECK(has(status, "running: slot 1 at 0x310000 (64 KB): valid (confirmed), version 0.4.0+w1"));
  CHECK(has(status, "the running image is confirmed"));
  CHECK(has(status, "4. seq 4: confirmed: all three conditions"));
}

TEST_CASE("self status during a trial, and self confirm ends it") {
  SelfUnit unit;
  unit.walter->console.begin();
  REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
  unit.reboot();
  unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;  // the uplink keeps the trial open
  const std::string text = bootAndTick(unit, 40000);
  INFO(text);
  CHECK(unit.walter->self.onTrial());
  const std::string status = unit.walter->runLine("self status");
  INFO(status);
  CHECK(has(status, "running: slot 1 at 0x310000 (64 KB): pending-verify (on trial"));
  CHECK(has(status, "on trial for "));
  CHECK(has(status, " s of 300: main loop met; radio met (1 HELLO); uplink failed, trying again (2 attempts)"));
  CHECK(has(status, "3. seq 3: on trial: 0.4.0 in slot 1"));
  CHECK(has(unit.walter->runLine("self confirm"), "self confirm: the running image is confirmed (valid); the trial is over"));
  CHECK_FALSE(unit.walter->self.onTrial());
  CHECK(unit.world.own.slot(1).state == interfaces::OwnImageState::kValid);
  CHECK(has(unit.walter->runLine("self status"), "4. seq 4: confirmed: on the console"));
}

TEST_CASE("the trial fails on the console: the rollback line, and the old image's boot line after it") {
  SelfUnit unit;
  unit.walter->console.begin();
  REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
  unit.reboot();
  unit.world.radio.deaf = true;
  const std::string text = bootAndTick(unit, upd::kTrialWindowMs + 10000);
  INFO(text);
  CHECK(has(text, "self-update: on trial -> rolled back: the radio answered before the update and not after it"));
  CHECK(has(text, "  restarting into the previous image"));
  CHECK(unit.world.own.restart_asked);
  unit.reboot();
  CHECK(unit.world.own.runningIndex() == 0);
  unit.walter->out.clear();
  unit.walter->console.begin();
  INFO(unit.walter->out.text);
  CHECK(has(unit.walter->out.text, "boot 3: software reset, the Walter's own update, the trial image rejected itself"));
  CHECK_FALSE(has(unit.walter->out.text, "self-update:"));  // the story was closed by the trial image itself
  const std::string status = unit.walter->runLine("self status");
  INFO(status);
  CHECK(has(status, "4. seq 4: rolled back: the radio answered before the update and not after it"));
}

TEST_CASE("a reset during the trial: the old image's boot line records the rollback") {
  SelfUnit unit;
  unit.walter->console.begin();
  REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
  unit.reboot();
  unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;
  (void)bootAndTick(unit, 20000);
  REQUIRE(unit.walter->self.onTrial());
  unit.reboot();
  unit.walter->out.clear();
  unit.walter->console.begin();
  INFO(unit.walter->out.text);
  CHECK(has(unit.walter->out.text, "self-update: rolled back: the image restarted during its trial (the bootloader gave up on it)"));
  CHECK(has(unit.walter->out.text, "  the image 0.4.0 in slot 1 is on the bad list; this is 0.0.0-dev again"));
  CHECK(unit.world.store.store.badCount() == 1);
}

TEST_CASE("self reject on the console during a trial") {
  SelfUnit unit;
  unit.walter->console.begin();
  REQUIRE(unit.receiveAndSwitch(Candidate("self-a")));
  unit.reboot();
  unit.walter->uplink.verdict = upd::UplinkVerdict::kFailed;
  (void)bootAndTick(unit, 5000);
  REQUIRE(unit.walter->self.onTrial());
  CHECK(has(unit.walter->runLine("self reject"), "self reject: the running image is rejected; restarting into the previous image"));
  CHECK(unit.world.own.restart_asked);
  CHECK(unit.world.store.store.badCount() == 1);
  unit.reboot();
  CHECK(unit.world.own.runningIndex() == 0);
  CHECK(has(unit.walter->runLine("self status"), "rolled back: rejected on the console"));
}

TEST_CASE("a build with no self-update says so") {
  SelfUnit unit;
  bench::SelfWalter& w = *unit.walter;
  core::console::Console bare(w.out, w.board, w.health, unit.world.clock, w.restart, w.client, w.slots,
                              &unit.world.store.store, &w.receiver);
  for (const char* line : {"self status", "self receive", "self switch", "self confirm", "self reject"}) {
    w.out.clear();
    bare.runLine(line);
    CHECK_MESSAGE(has(w.out.text, "error: there is no self-update in this build"), line);
  }
}
