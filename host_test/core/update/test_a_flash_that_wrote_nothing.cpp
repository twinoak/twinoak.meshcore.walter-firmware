// **A flash that wrote nothing** (task p18-05).
//
// The second NoiseScope session on the unit at Bellevue (23 November, meshmaster.dk job 41878)
// measured nothing. Its journal: *flashing-scanner* at 11:37:41Z, **`rolling-back: the flasher did
// not finish`** twelve seconds later, the whole golden image flashed back in 6 min 26 s,
// *restored* at 11:47:25Z. Nine minutes off the mesh, no line measured, every step `notRun`, and
// `rak update` saying of the scanner image that it *failed its health check (the flasher did not
// finish) and is on the bad list*.
//
// By the times of every flash that unit has made, that flash **changed nothing on the chip**: the
// restore was a compare and a read-back of the whole image with no page programmed, and twelve
// seconds is the connect, the checks before the scan, or the scan's first pages
// (`tasks/phase-18/FOLLOWUPS.md` has the arithmetic). The golden image's flash began two seconds
// later on the same chip, with the repeater still running, and went through.
//
// Four things this file holds, each of them wrong whatever made that flash fail:
//
//   1. **it says what failed**: the step, the status of the layer under it, the address and
//      whether the chip was changed - in the journal's entry, at the console and in the note that
//      goes to the server with the job (the server's half is `host_test/agent/`);
//   2. **once more**: a flash that ended having written nothing is made a second time, after a
//      pause and after the radio's HELLO where the first try had halted it. One second try and
//      never a third, on every kind of flash;
//   3. **twice, and the radio is as it was**: no flash back, nothing on the bad list, no outage,
//      and an apply or a session that ends `refused`;
//   4. **the bad list is for an image that is wrong or that ran and failed**: a page that would
//      not take is the unit's and its image may be flashed again; a digest that is not the one
//      given is the image's own and never is.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/update/radio_update.h"
#include "core/update/scanner_session.h"
#include "session_bench.h"
#include "update_bench.h"

using bench::Boot;
using bench::Ending;
using bench::Kind;
using bench::ReadyUnit;
using bench::Reason;
using bench::SessionUnit;
using bench::Stage;
using bench::State;
using bench::Which;
using bench::programOf;
namespace upd = core::update;
namespace fl = core::flasher;

namespace {

using FailAt = sim::Nrf52Chip::FailFlashAt;

// Every radio-update entry the journal holds, oldest first.
std::vector<upd::Payload> storyOf(const bench::Walter& w) {
  std::vector<upd::Payload> out;
  upd::Cursor cursor;
  upd::Entry entry;
  bool more = w.journal.first(&cursor, &entry);
  while (more) {
    upd::Payload p;
    if (entry.kind == upd::kind::kRadioUpdate && upd::decode(entry, &p)) out.insert(out.begin(), p);
    more = w.journal.previous(&cursor, &entry);
  }
  return out;
}

std::string storyText(const std::vector<upd::Payload>& story) {
  std::string text;
  for (const upd::Payload& p : story) {
    text += std::string(upd::describe(p.state));
    if (p.reason != Reason::kNone) text += std::string(": ") + upd::describe(p.reason);
    if (p.reason == Reason::kFlashFailed || p.reason == Reason::kFlashOnceMore ||
        p.reason == Reason::kFlashDidNotBegin) {
      char line[upd::kFlashFailureLineMax];
      upd::flashFailureLine(upd::decodeFlashFailure(p.detail), line, sizeof line);
      text += std::string(" - ") + line;
    }
    text += "\n";
  }
  return text;
}

// The newest entry carrying one reason, and whether there was one.
bool entryWith(const std::vector<upd::Payload>& story, Reason reason, upd::Payload* out) {
  bool found = false;
  for (const upd::Payload& p : story) {
    if (p.reason != reason) continue;
    found = true;
    if (out != nullptr) *out = p;
  }
  return found;
}

unsigned countWith(const std::vector<upd::Payload>& story, Reason reason) {
  unsigned n = 0;
  for (const upd::Payload& p : story) {
    if (p.reason == reason) ++n;
  }
  return n;
}

bool has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

// ------------------------------------------------- 1. the second session of 23 November

TEST_CASE("the second session of 23 November: the scanner's flash fails before its halt, is made once more, and the session ends restored") {
  SessionUnit unit;
  // The flash of 11:37:43Z, as the arithmetic has it: the scan's reads stalling on a bus the
  // application is using, twelve seconds in and nothing written. Once, and only for that flash.
  unit.world.chip.failFlashAt(FailAt::kScanRead, 1);
  const upd::Program program =
      programOf({{"sweep 863 870 25", 30}, {"hist 869.418 869.818 25", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  const Stage end = unit.run();
  INFO(unit.stageText());
  const std::vector<upd::Payload> story = storyOf(*unit.walter);
  INFO(storyText(story));

  // **The journal's line before the second try**, and what it says failed.
  upd::Payload again;
  REQUIRE(entryWith(story, Reason::kFlashOnceMore, &again));
  CHECK(again.state == State::kFlashing);
  const upd::FlashFailure failure = upd::decodeFlashFailure(again.detail);
  CHECK(failure.detailed);
  CHECK(failure.step == fl::Step::kScan);
  CHECK(failure.status == core::nrf52::Status::kSwd);
  CHECK(failure.swd == core::swd::Status::kWaitTimeout);
  CHECK(failure.wrote_nothing);
  CHECK_FALSE(failure.core_halted);
  CHECK_FALSE(failure.second_try);
  CHECK(failure.address_known);
  CHECK(failure.address == unit.world.profile.app.begin);

  // **The second try went through**, and everything after it is the session of 21 November: the
  // scanner there, both steps measured, the golden image back and proved.
  CHECK(end == Stage::kRestored);
  CHECK(unit.session().status().ending == Ending::kRestored);
  CHECK(unit.sink.steps.size() == 2);
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK_FALSE(unit.world.radio.scanner);
  // Three debug sessions: the scanner's two tries and the golden image's one flash.
  CHECK(unit.world.chip.debugSessions() == 3);
  CHECK(countWith(story, Reason::kFlashOnceMore) == 1);
  CHECK(countWith(story, Reason::kFlashFailed) == 0);
  // **Nothing on the bad list**, and no golden flash for a flash that wrote nothing: the one
  // outage of this session is the scanner's own, which is what a session costs.
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(unit.world.outages == 1);
  CHECK(unit.world.dark_periods == 2);  // one a flash, and the first try was not one
  MESSAGE("23 November with this task: " << storyText(story));
}

TEST_CASE("the same flash failing every time: the session ends refused, nothing is flashed, the repeater never leaves the mesh, and the next session is taken") {
  SessionUnit unit;
  unit.world.chip.failFlashAt(FailAt::kScanRead, 0);  // every flash, for ever
  const upd::Program program = programOf({{"sweep 863 870 25", 30}});
  REQUIRE(unit.session().start(program, bench::Slot{0}, &unit.sink));
  const Stage end = unit.run();
  INFO(unit.stageText());
  const std::vector<upd::Payload> story = storyOf(*unit.walter);
  INFO(storyText(story));

  // **The radio is as it was.** No erase, no write, no image flashed back, no outage at all - and
  // the scanner image on no bad list, because it was never on the chip.
  CHECK(end == Stage::kRefused);
  CHECK(unit.session().status().ending == Ending::kRefused);
  CHECK(unit.world.chip.counters().erases == 0);
  CHECK(unit.world.chip.counters().writes == 0);
  CHECK(unit.world.outages == 0);
  CHECK(unit.world.dark_periods == 0);
  CHECK(unit.world.radio_runs);
  CHECK_FALSE(unit.world.radio.scanner);
  CHECK(unit.world.running_version == unit.golden.version);
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(unit.sink.steps.empty());
  // Two tries at the one flash and never a third.
  CHECK(unit.world.chip.debugSessions() == 2);
  CHECK(countWith(story, Reason::kFlashOnceMore) == 1);
  upd::Payload refused;
  REQUIRE(entryWith(story, Reason::kFlashDidNotBegin, &refused));
  CHECK(refused.state == State::kRefused);
  CHECK(upd::decodeFlashFailure(refused.detail).second_try);

  // **And the session is closed**: the mark says so, so no later wake has anything to end - and
  // the next session is taken, which is the whole point of not having been left `stuck`.
  upd::Mark mark;
  REQUIRE(upd::readMark(unit.walter->journal, &mark));
  CHECK_FALSE(mark.open);
  unit.world.chip.failFlashAt(FailAt::kNone);
  const upd::Program again = programOf({{"stat", 10}});
  REQUIRE(unit.session().start(again, bench::Slot{0}, &unit.sink));
  CHECK(unit.run() == Stage::kRestored);
  CHECK(unit.sink.steps.size() == 1);
  MESSAGE("a session whose flash never began: " << storyText(storyOf(*unit.walter)));
}

// --------------------------------------------------------------- 2. an apply, the same two ways

TEST_CASE("an apply whose flash wrote nothing: once more, and committed") {
  ReadyUnit unit;
  unit.world.chip.failFlashAt(FailAt::kScanRead, 1);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(end == State::kDone);
  CHECK(w.update.status().flash_tries == 2);
  CHECK(unit.goldenVersion() == unit.candidate.version);
  CHECK(unit.world.chipHolds(unit.candidate));
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(countWith(story, Reason::kFlashOnceMore) == 1);
  CHECK(unit.world.chip.debugSessions() == 2);
  CHECK(unit.world.outages == 1);  // the one the flash itself costs
}

TEST_CASE("an apply whose flash never begins: refused, the candidate still offerable, and no outage") {
  ReadyUnit unit;
  unit.world.chip.failFlashAt(FailAt::kScanRead, 0);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(end == State::kRefused);
  CHECK(w.update.reason() == Reason::kFlashDidNotBegin);
  CHECK(w.update.status().flash_tries == 2);
  CHECK(w.update.status().units_fault);
  // Nothing touched: the golden image is golden, the candidate is still there to be offered
  // again, and the chip was not written to at all.
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.read(Which::kCandidate).present);
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(unit.world.chip.counters().erases == 0);
  CHECK(unit.world.chip.counters().writes == 0);
  CHECK(unit.world.outages == 0);
  CHECK(unit.world.dark_periods == 0);
  CHECK(unit.world.chip.debugSessions() == 2);
  // And the same apply may be run again: nothing refuses it.
  unit.world.chip.failFlashAt(FailAt::kNone);
  w.update.clear();
  REQUIRE(w.update.start(unit.request()));
  CHECK(w.run() == State::kDone);
}

// ------------------------------------------------------- 3. a flash that failed after a write

TEST_CASE("a page that would not take: the golden image goes back as it always did, and the image is on no bad list") {
  ReadyUnit unit;
  // The chip takes the erase and the writes and the page never reads back: the flash fails at the
  // pages, **having changed the chip**. One flash only, so the golden image's own goes through.
  unit.world.chip.failFlashAt(FailAt::kPageWillNotTake, 1);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));

  // The ending is today's: the golden image back and proved.
  CHECK(end == State::kRestored);
  CHECK(w.update.status().fail_reason == Reason::kFlashFailed);
  CHECK(unit.goldenVersion() == unit.golden.version);
  CHECK(unit.world.chipHolds(unit.golden));
  // **The chip was changed, so the flash is not made again** - not one line of the story says a
  // flash was made twice, on either of this run's two flashes.
  CHECK(countWith(story, Reason::kFlashOnceMore) == 0);
  // **And the fault was the unit's**: a page that would not take is not the image, so the
  // candidate is on no bad list and may be flashed again.
  CHECK(w.update.status().units_fault);
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK_FALSE(unit.world.store.store.isBad(unit.read(Which::kCandidate).sha256));
  upd::Payload failed;
  REQUIRE(entryWith(story, Reason::kFlashFailed, &failed));
  const upd::FlashFailure failure = upd::decodeFlashFailure(failed.detail);
  CHECK_FALSE(failure.wrote_nothing);
  CHECK(failure.core_halted);
  CHECK((failure.step == fl::Step::kFirstPage || failure.step == fl::Step::kPages));
  // And the entry says so, both at the console and in a report, in those words.
  const std::string status = w.runLine("rak update status");
  INFO(status);
  CHECK(has(status, "the chip was changed, the core was halted and started again"));
  CHECK((failed.flags & upd::kFlagUnitsFault) != 0);
}

TEST_CASE("a digest that is not the one given: the image's own proof failed, and its hash goes on the bad list") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  // A bit of the stored candidate flipped **after** the store's own verify at `check` and before
  // the flash: the flasher then writes what the store holds, reads back exactly that, and the
  // SHA-256 of it is not the one the record gave. That is the image's proof and nothing of this
  // unit's, so the hash is never tried again (S-5, and §6's drawing).
  const std::uint32_t data =
      core::image::areaOffset(0, unit.world.store.store.areaOf(Which::kCandidate, bench::Slot{0})) +
      core::image::kSectorSize;
  bool rotted = false;
  const State end = w.run([&](const upd::StepResult& s) {
    if (!rotted && s.after == State::kFlashing) {
      rotted = true;
      unit.world.store.flash.rot(data + 600, 0x20);
    }
  });
  REQUIRE(rotted);
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(end == State::kRestored);
  CHECK(w.update.status().fail_reason == Reason::kFlashFailed);
  upd::Payload failed;
  REQUIRE(entryWith(story, Reason::kFlashFailed, &failed));
  CHECK(upd::decodeFlashFailure(failed.detail).step == fl::Step::kDigest);
  CHECK_FALSE(w.update.status().units_fault);
  CHECK(unit.world.store.store.badCount() == 1);
  CHECK(unit.world.store.store.isBad(w.update.status().sha256));
  CHECK(unit.goldenVersion() == unit.golden.version);
  // No second try: the chip was written to.
  CHECK(countWith(story, Reason::kFlashOnceMore) == 0);
}

TEST_CASE("whose fault a failed flash was, step by step") {
  // The two steps that are statements about the file, and everything else which is the unit's.
  const fl::Step image_is[] = {fl::Step::kImage, fl::Step::kDigest};
  const fl::Step units[] = {fl::Step::kSelect,  fl::Step::kPins,       fl::Step::kConnect,
                            fl::Step::kProtected, fl::Step::kIdentify, fl::Step::kSoftDevice,
                            fl::Step::kScan,     fl::Step::kHalt,      fl::Step::kSettingsInvalidate,
                            fl::Step::kFirstPageErase, fl::Step::kPages, fl::Step::kFirstPage,
                            fl::Step::kReadBack, fl::Step::kSettingsValidate, fl::Step::kReset};
  for (fl::Step step : image_is) {
    fl::Result r;
    r.outcome = fl::Outcome::kRefused;
    r.step = step;
    CHECK(fl::blamesTheImage(r));
  }
  for (fl::Step step : units) {
    fl::Result r;
    r.outcome = fl::Outcome::kFailed;
    r.step = step;
    CHECK_FALSE(fl::blamesTheImage(r));
  }
  // A flash that worked blames nobody.
  fl::Result ok;
  CHECK_FALSE(fl::blamesTheImage(ok));
}

// --------------------------------------------------------- 4. the golden image, and the halt

TEST_CASE("the golden image going back, failing once before its halt: tried once more, and restored") {
  ReadyUnit unit;
  unit.world.boot = Boot::kSilent;  // the candidate boots and never answers: a rollback
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  // Once, at the transition and not at the line of the story written in the same state: the
  // second try's own entry is a `kRollingBack` answer too (`noted()`), and arming twice would
  // make the chip fail the second try as well.
  bool armed = false;
  const State end = w.run([&](const upd::StepResult& s) {
    if (armed || s.after != State::kRollingBack || s.before == s.after) return;
    armed = true;
    unit.world.boot = Boot::kNormal;
    // The golden image's own flash is the one that fails before its halt, once.
    unit.world.chip.failFlashAt(FailAt::kScanRead, 1);
  });
  REQUIRE(armed);
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(end == State::kRestored);
  CHECK(unit.world.chipHolds(unit.golden));
  CHECK(unit.world.running_version == unit.golden.version);
  // One second try, at the golden image's flash: the candidate's own went through.
  CHECK(countWith(story, Reason::kFlashOnceMore) == 1);
  CHECK(w.update.status().flash_tries == 2);
  // And the candidate is on the bad list, as it has always been: it booted nowhere.
  CHECK(w.update.status().fail_reason == Reason::kNoBoot);
  CHECK(unit.world.store.store.badCount() == 1);
}

TEST_CASE("the golden image going back, failing twice before its halt: stuck, as it is today, with the step said") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request(Kind::kRoleRepeater)));  // the golden image, by itself
  unit.world.chip.failFlashAt(FailAt::kScanRead, 0);
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  // There is nothing to be refused into on the golden image's own path: `stuck` is the ending it
  // has always been, and now it says what failed.
  CHECK(end == State::kStuck);
  CHECK(w.update.reason() == Reason::kFlashFailed);
  CHECK(countWith(story, Reason::kFlashOnceMore) == 1);
  CHECK(unit.world.chip.debugSessions() == 2);
  upd::Payload stuck;
  REQUIRE(entryWith(story, Reason::kFlashFailed, &stuck));
  const upd::FlashFailure failure = upd::decodeFlashFailure(stuck.detail);
  CHECK(failure.second_try);
  CHECK(failure.wrote_nothing);
  CHECK(failure.step == fl::Step::kScan);
}

TEST_CASE("a first try that halted the core and wrote nothing: the radio is started again, answers, and the second try follows") {
  ReadyUnit unit;
  // Everything goes through, the core is halted, and the flash controller never becomes ready:
  // nothing is written and the radio has to be started again before the flash is made once more.
  unit.world.chip.failFlashAt(FailAt::kAfterTheHalt, 1);
  bench::Walter& w = *unit.walter;
  const int boots_before = unit.world.boots;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  const std::vector<upd::Payload> story = storyOf(w);
  INFO(storyText(story));
  CHECK(end == State::kDone);
  CHECK(w.update.status().flash_tries == 2);
  upd::Payload again;
  REQUIRE(entryWith(story, Reason::kFlashOnceMore, &again));
  const upd::FlashFailure failure = upd::decodeFlashFailure(again.detail);
  CHECK(failure.wrote_nothing);
  CHECK(failure.core_halted);  // and so it had to be started again
  CHECK(failure.step == fl::Step::kHalt);
  // The image the first try halted was started again by a pulse, and it booted before the second
  // try took the lines back: three boots in all - the golden image after the first try, and the
  // candidate after the second.
  CHECK(unit.world.chip.resets().pin == 2);
  CHECK(unit.world.boots >= boots_before + 2);
  CHECK(unit.world.store.store.badCount() == 0);
  CHECK(unit.goldenVersion() == unit.candidate.version);
}

// ------------------------------------------------- 5. every step before the halt, and only twice

TEST_CASE("every way out before the first write is made once more, and each says which it was") {
  struct Case {
    const char* name;
    FailAt where;
    fl::Step step;
  };
  const Case cases[] = {
      {"the power-up", FailAt::kPowerUp, fl::Step::kConnect},
      {"the control port", FailAt::kControlPort, fl::Step::kConnect},
      {"the protection", FailAt::kProtection, fl::Step::kProtected},
      {"the memory port", FailAt::kMemoryPort, fl::Step::kConnect},
      {"the identity", FailAt::kIdentity, fl::Step::kIdentify},
      {"the flash controller", FailAt::kFlashReady, fl::Step::kConnect},
      {"the SoftDevice", FailAt::kSoftDevice, fl::Step::kSoftDevice},
      {"a read that stalls while the application runs", FailAt::kScanRead, fl::Step::kScan},
      {"the flash controller after the halt", FailAt::kAfterTheHalt, fl::Step::kHalt},
  };
  for (const Case& one : cases) {
    CAPTURE(one.name);
    ReadyUnit unit;
    unit.world.chip.failFlashAt(one.where, 1);
    bench::Walter& w = *unit.walter;
    REQUIRE(w.update.start(unit.request()));
    const State end = w.run();
    INFO(w.transitionText());
    const std::vector<upd::Payload> story = storyOf(w);
    INFO(storyText(story));
    // The second try went through and the candidate is golden: nothing of this ends a run.
    CHECK(end == State::kDone);
    CHECK(w.update.status().flash_tries == 2);
    CHECK(unit.world.store.store.badCount() == 0);
    upd::Payload again;
    REQUIRE(entryWith(story, Reason::kFlashOnceMore, &again));
    const upd::FlashFailure failure = upd::decodeFlashFailure(again.detail);
    CHECK(failure.step == one.step);
    CHECK(failure.wrote_nothing);
    char line[upd::kFlashFailureLineMax];
    upd::flashFailureLine(failure, line, sizeof line);
    MESSAGE(std::string(one.name) << " -> " << std::string(line));
  }
}

TEST_CASE("one second try at most, on every kind of flash this firmware makes") {
  struct Case {
    const char* name;
    Kind kind;
    State ends;
  };
  const Case cases[] = {
      {"a candidate", Kind::kUpdate, State::kRefused},
      {"a scanner image", Kind::kRoleScanner, State::kRefused},
      {"the golden image going back", Kind::kRoleRepeater, State::kStuck},
  };
  for (const Case& one : cases) {
    CAPTURE(one.name);
    // A scanner image in the store too, so that every kind has something to flash.
    SessionUnit unit;
    bench::Walter& w = *unit.walter;
    REQUIRE(w.update.start(unit.request(one.kind)));
    unit.world.chip.failFlashAt(FailAt::kScanRead, 0);
    const State end = w.run();
    INFO(w.transitionText());
    INFO(storyText(storyOf(w)));
    CHECK(end == one.ends);
    CHECK(w.update.status().flash_tries == 2);
    // Two debug sessions and no more: a third try is never made, whatever the kind.
    CHECK(unit.world.chip.debugSessions() == 2);
    CHECK(unit.world.chip.counters().erases == 0);
    CHECK(unit.world.chip.counters().writes == 0);
  }
  // And a first install, which has no golden image to return to.
  ReadyUnit first(true, false);
  first.world.chip.failFlashAt(FailAt::kScanRead, 0);
  REQUIRE(first.walter->update.start(first.request(Kind::kFirstInstall)));
  const State end = first.walter->run();
  INFO(first.walter->transitionText());
  INFO(storyText(storyOf(*first.walter)));
  CHECK(end == State::kRefused);
  CHECK(first.walter->update.status().flash_tries == 2);
  CHECK(first.world.chip.debugSessions() == 2);
  CHECK(first.world.store.store.badCount() == 0);
}

// ------------------------------------------------------------------ 6. the power, pulled

TEST_CASE("the power pulled between the two tries: the next wake ends it by the rails there are") {
  ReadyUnit unit;
  unit.world.chip.failFlashAt(FailAt::kScanRead, 1);
  bool died = false;
  REQUIRE(unit.walter->update.start(unit.request()));
  unit.walter->run([&](const upd::StepResult& s) {
    if (!died && s.reason == Reason::kFlashOnceMore) {
      died = true;
      unit.walter->die();
    }
  });
  CHECK(died);
  CHECK(unit.world.dead);
  // The journal holds `flashing-candidate` with the line that says the flash is to be made once
  // more, and nothing was written: the next boot takes the run up again and flashes from nothing.
  const std::vector<upd::Payload> before = storyOf(*unit.walter);
  INFO(storyText(before));
  CHECK(entryWith(before, Reason::kFlashOnceMore, nullptr));
  CHECK(unit.world.chip.counters().erases == 0);

  unit.reboot();
  REQUIRE(unit.walter->update.resume());
  const State end = unit.walter->run();
  INFO(unit.walter->transitionText());
  INFO(storyText(storyOf(*unit.walter)));
  CHECK(end == State::kDone);
  CHECK(unit.world.radio_runs);
  CHECK(unit.goldenVersion() == unit.candidate.version);
  // The resumed run is one flash with one second try of its own, and the knob was spent on the
  // first life: it went through at the first attempt.
  CHECK(unit.walter->update.status().flash_tries == 1);
}

// ------------------------------------------------------------------ 7. the words

TEST_CASE("what a person is told: the console's lines for a flash that did not begin") {
  ReadyUnit unit;
  unit.world.chip.failFlashAt(FailAt::kScanRead, 0);
  const std::string out = unit.walter->runLine("rak update");
  INFO(out);
  // The line of the story written before the second try, in the state the machine is already in.
  CHECK(has(out, "update: flashing-candidate: the flash did not begin: once more after a pause"));
  // The ending, and it is not "refused at check": nothing was touched, and the same update may be
  // run again.
  CHECK(has(out, "error: rak update: the flash did not begin, twice (the second try, while reading "
                 "the pages the image covers: the debug port failed (the target kept answering "
                 "WAIT) at 0x00026000; nothing was written)"));
  CHECK(has(out, "the radio is as it was and the same update can be run again"));
  CHECK_FALSE(has(out, "refused at check"));

  // And the journal's story, at the console and in a report: `rak update status` reads the same
  // bits back out of flash.
  const std::string status = unit.walter->runLine("rak update status");
  INFO(status);
  CHECK(has(status, "flashing-candidate: the flash did not begin: once more - while reading the "
                    "pages the image covers: the debug port failed (the target kept answering "
                    "WAIT) at 0x00026000; nothing was written"));
  CHECK(has(status, "refused: the flash did not begin, twice: nothing was written and the radio is "
                    "as it was - the second try, while reading the pages the image covers"));
  // The plain `journal` command prints the same entries.
  const std::string journal = unit.walter->runLine("journal");
  INFO(journal);
  CHECK(has(journal, "the flash did not begin"));
}

TEST_CASE("a flash that failed says so at the console, whether anything was written and whether the core was halted") {
  ReadyUnit unit;
  unit.world.chip.failFlashAt(FailAt::kScanRead, 1);
  // A typed flash of the test pattern: the first attempt fails at the scan and says what it did.
  const std::string out = unit.walter->runLine("rak flash-test confirm");
  INFO(out);
  CHECK(has(out, "failed while reading the pages the image covers"));
  CHECK(has(out, "nothing was written: the chip is as this flash found it"));
  CHECK_FALSE(has(out, "the core was halted"));

  ReadyUnit halted;
  halted.world.chip.failFlashAt(FailAt::kAfterTheHalt, 1);
  const std::string after = halted.walter->runLine("rak flash-test confirm");
  INFO(after);
  CHECK(has(after, "nothing was written: the chip is as this flash found it; the core was halted "
                   "and started again"));
}

TEST_CASE("the detail's thirty-two bits, and an entry from before this task reading as it did") {
  upd::FlashFailure f;
  f.detailed = true;
  f.step = fl::Step::kSettingsInvalidate;
  f.status = core::nrf52::Status::kNvmcTimeout;
  f.swd = core::swd::Status::kOk;
  f.wrote_nothing = false;
  f.core_halted = true;
  f.refused = false;
  f.second_try = true;
  f.address_known = true;
  f.address = 0x000FF000;
  const std::uint32_t bits = upd::encodeFlashFailure(f);
  const upd::FlashFailure back = upd::decodeFlashFailure(bits);
  CHECK(back.detailed);
  CHECK(back.step == f.step);
  CHECK(back.status == f.status);
  CHECK(back.wrote_nothing == f.wrote_nothing);
  CHECK(back.core_halted);
  CHECK(back.second_try);
  CHECK(back.address_known);
  CHECK(back.address == f.address);

  // Every step and every status survives the trip, so no layout change can lose one quietly.
  for (unsigned step = 0; step <= static_cast<unsigned>(fl::Step::kDone); ++step) {
    upd::FlashFailure one;
    one.detailed = true;
    one.step = static_cast<fl::Step>(step);
    one.status = core::nrf52::Status::kSwd;
    one.swd = core::swd::Status::kNoMemAp;
    const upd::FlashFailure round = upd::decodeFlashFailure(upd::encodeFlashFailure(one));
    CHECK(round.step == one.step);
    CHECK(round.status == one.status);
    CHECK(round.swd == one.swd);
  }
  for (unsigned status = 0; status <= static_cast<unsigned>(core::nrf52::Status::kSlotBusy);
       ++status) {
    upd::FlashFailure one;
    one.detailed = true;
    one.status = static_cast<core::nrf52::Status>(status);
    CHECK(upd::decodeFlashFailure(upd::encodeFlashFailure(one)).status == one.status);
  }

  // **An older entry**: the whole detail was the flasher's step, and it still reads as that step
  // and as nothing more.
  const upd::FlashFailure old = upd::decodeFlashFailure(static_cast<std::uint32_t>(fl::Step::kScan));
  CHECK_FALSE(old.detailed);
  CHECK(old.step == fl::Step::kScan);
  CHECK_FALSE(old.wrote_nothing);
  CHECK_FALSE(old.address_known);
  char line[upd::kFlashFailureLineMax];
  upd::flashFailureLine(old, line, sizeof line);
  CHECK(std::string(line) ==
        "while reading the pages the image covers (an entry from before this was recorded)");

  // And the two lines a failed flash is said in: the long one for a person, the short one for the
  // seventy characters a job's note holds.
  char note[72];
  upd::flashFailureNote(f, note, sizeof note);
  CHECK(std::string(note) == "2nd try settings-invalidate: nvmc-timeout at 0x0ff000; chip changed");
  CHECK(std::string(note).size() < sizeof note);
  upd::flashFailureLine(f, line, sizeof line);
  CHECK(std::string(line) ==
        "the second try, while marking bank 0 invalid in the bootloader's settings: the flash "
        "controller did not become ready in time; CONFIG is read-only again at 0x000ff000; the "
        "chip was changed, the core was halted and started again");
}
