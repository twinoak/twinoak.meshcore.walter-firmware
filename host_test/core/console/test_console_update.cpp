// The console's `rak update`, `rak update --first-install`, `rak update status` and `rak role`
// (task p2-03): the machine driven to its end inside the command with every transition printed,
// the story read back out of the journal, the refusals, and a resumed update driven by tick().
#include <doctest.h>

#include <string>

#include "core/console/console.h"
#include "core/update/radio_update.h"
#include "core/update/update_bench.h"
#include "fakes/capture.h"

using bench::Boot;
using bench::ReadyUnit;
using bench::State;
using bench::Which;
namespace upd = core::update;

namespace {

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

}  // namespace

TEST_CASE("rak update prints each transition and ends with the candidate golden") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  const std::string text = w.runLine("rak update");
  INFO(text);
  CHECK(has(text, "rak update: slot 0; every transition is printed"));
  CHECK(has(text, "update: check -> flashing-candidate: the preconditions hold"));
  CHECK(has(text, "reading the pages the image covers: page 1/1"));
  CHECK(has(text, "update: flashing-candidate -> booting: flashed; the radio was reset"));
  CHECK(has(text, "update: booting -> proving: the radio answered"));
  CHECK(has(text, "update: proving -> committing: the health check passed"));
  CHECK(has(text, "update: committing -> done: the candidate is the golden image"));
  CHECK(has(text, "rak update: done: 0.3.1+candid0 (sha256 "));
  CHECK(has(text, "'image list' shows it, 'rak hello' its version"));
  CHECK(!has(text, "error:"));
  CHECK(unit.goldenVersion() == unit.candidate.version);
  CHECK(has(w.runLine("image list"), "golden     repeater rak4630-single 868 0.3.1+candid0"));
  CHECK(has(w.runLine("rak hello"), "firmware 0.3.1+candid0"));

  const std::string story = w.runLine("rak update status");
  INFO(story);
  CHECK(has(story, "the last update's story (6 entries"));
  CHECK(has(story, "1. seq 1: check - update, slot 0, image sha256 "));
  CHECK(has(story, "2. seq 2: flashing-candidate"));
  CHECK(has(story, "3. seq 3: booting"));
  CHECK(has(story, "4. seq 4: proving"));
  CHECK(has(story, "5. seq 5: committing (the health check passed; "));
  CHECK(has(story, "6. seq 6: done"));
  CHECK(has(story, "the last update ended: done"));

  // A second rak update: there is no candidate any more.
  CHECK(has(w.runLine("rak update"), "error: rak update: refused at check: the store holds no image to flash (nothing was touched)"));
  // The story is now the refused run's.
  CHECK(has(w.runLine("rak update status"), "the last update ended: refused: the store holds no image to flash"));
}

TEST_CASE("rak update on a broken candidate: the rollback printed, the bad list, and refused a second time") {
  ReadyUnit unit;
  unit.world.boot = Boot::kSilent;
  bench::Walter& w = *unit.walter;
  // The golden image must boot normally: flip the knob when the rollback begins, from inside
  // the world's own settle (the console drives the machine, not the test).
  unit.world.clock.on_delay = [&](std::uint32_t) {
    unit.world.settle();
    if (w.update.state() == State::kRollingBack || w.update.state() == State::kRollbackBooting) {
      unit.world.boot = Boot::kNormal;
    }
  };
  const std::string text = w.runLine("rak update");
  INFO(text);
  CHECK(has(text, "update: booting -> rolling-back: no HELLO inside the boot window"));
  CHECK(has(text, "update: rolling-back -> rolling-back (booting the golden image): flashed; the radio was reset"));
  CHECK(has(text, "-> restored: the golden image is back and proved"));
  CHECK(has(text, "rak update: restored: the candidate failed its health check (no HELLO inside the boot window at HELLO against the descriptor) and is on the bad list;"));
  CHECK(has(text, "the golden image 0.3.0+golden0 is back and proved"));
  CHECK(has(w.runLine("image bad"), "image bad: 1 of at most 8 hashes"));
  CHECK(has(w.runLine("rak hello"), "firmware 0.3.0+golden0"));
  CHECK(has(w.runLine("rak update"), "error: rak update: refused at check: this image failed a health check once and is never tried again"));
}

TEST_CASE("rak update --first-install on a unit with no golden image, and rak update refused without it") {
  ReadyUnit unit(true, false);
  bench::Walter& w = *unit.walter;
  CHECK(has(w.runLine("rak update"), "refused at check: there is no golden image to return to (a first install needs saying so)"));
  const std::string text = w.runLine("rak update --first-install");
  INFO(text);
  CHECK(has(text, "update: committing -> done"));
  CHECK(unit.goldenVersion() == unit.candidate.version);
}

TEST_CASE("rak role scanner and rak role repeater") {
  ReadyUnit unit;
  const bench::Candidate scanner("store-scanner");
  REQUIRE(unit.world.store.put(Which::kScanner, bench::Slot{0}, scanner) == bench::StoreError::kNone);
  bench::Walter& w = *unit.walter;
  std::string text = w.runLine("rak role scanner");
  INFO(text);
  CHECK(has(text, "rak role scanner: slot 0"));
  CHECK(has(text, "update: proving -> done: a scanner is there; golden and candidate untouched"));
  CHECK(has(text, "rak role scanner: done: a scanner is running"));
  CHECK(has(w.runLine("rak hello"), "the radio is running the scanner"));
  text = w.runLine("rak role repeater");
  INFO(text);
  CHECK(has(text, "update: check -> rolling-back: the preconditions hold"));
  CHECK(has(text, "rak role repeater: done: the golden image 0.3.0+golden0 is back and proved"));
  CHECK(has(w.runLine("rak hello"), "firmware 0.3.0+golden0"));
  CHECK(has(w.runLine("image list"), "candidate  repeater rak4630-single 868 0.3.1+candid0"));
}

TEST_CASE("a resumed update is announced at boot and driven by tick()") {
  ReadyUnit unit;
  // The Walter dies right after the journal's 'flashing-candidate' entry (the second).
  std::uint32_t written = 0;
  unit.world.journal_flash.on_op = [&](const sim::FlashOp& op) {
    if (op.kind == sim::FlashOp::Kind::kWrite && op.offset % upd::kEntryBytes == upd::kMagicOffset && ++written == 2) {
      unit.walter->die();
    }
  };
  REQUIRE(unit.walter->update.start(unit.request()));
  (void)unit.walter->run();
  REQUIRE(unit.world.dead);
  unit.world.journal_flash.on_op = nullptr;
  unit.reboot();
  bench::Walter& w = *unit.walter;
  // What main does: begin() resumes; then tick() every turn of the loop.
  w.out.clear();
  w.console.begin();
  INFO(w.out.text);
  CHECK(has(w.out.text, "update: the journal says a run was in progress (update); resuming at flashing-candidate"));
  CHECK(w.update.active());
  CHECK(has(w.runLine("rak update status"), "now: flashing-candidate (resumed)"));
  CHECK(has(w.runLine("rak update"), "error: rak update: an update is already running (flashing-candidate)"));
  w.out.clear();
  int ticks = 0;
  while (w.update.active() && ticks++ < 20000) {
    w.console.tick();
    unit.world.clock.delayMs(100);
  }
  INFO(w.out.text);
  CHECK(w.update.state() == State::kDone);
  CHECK(has(w.out.text, "update: flashing-candidate -> booting"));
  CHECK(has(w.out.text, "update: committing -> done"));
  CHECK(has(w.out.text, "rak update: done: 0.3.1+candid0"));
  CHECK(unit.goldenVersion() == unit.candidate.version);
  const std::string story = w.runLine("rak update status");
  INFO(story);
  CHECK(has(story, "flashing-candidate (resumed after a restart)"));
  CHECK(has(story, "the last update ended: done"));
}

TEST_CASE("rak update status on a unit that has never updated, and the parser's refusals") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  CHECK(has(w.runLine("rak update status"), "rak update status: no update has been journalled on this unit"));
  CHECK(has(w.runLine("rak update now"), "error: rak update: unknown word (rak update, rak update --first-install, rak update status): now"));
  CHECK(has(w.runLine("rak role"), "error: rak role needs a role: rak role scanner | repeater"));
  CHECK(has(w.runLine("rak role bridge"), "error: rak role: not one of scanner, repeater: bridge"));
  CHECK(has(w.runLine("rak @1 update"), "error: slot 1: this adapter has 1 radio"));
  const std::string help = w.runLine("help");
  CHECK(has(help, "rak update          flash the candidate"));
  CHECK(has(help, "rak update --first-install"));
  CHECK(has(help, "rak update status"));
  CHECK(has(help, "rak role scanner|repeater"));
}

TEST_CASE("a build with no update machine says so") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  core::console::Console bare(w.out, w.board, w.health, unit.world.clock, w.restart, w.client, w.slots,
                              &unit.world.store.store, &w.receiver);
  w.out.clear();
  bare.runLine("rak update");
  CHECK(has(w.out.text, "error: there is no update machine in this build"));
  w.out.clear();
  bare.runLine("rak update status");
  CHECK(has(w.out.text, "error: there is no update machine in this build"));
  w.out.clear();
  bare.runLine("rak role scanner");
  CHECK(has(w.out.text, "error: there is no update machine in this build"));
}
