// **The unit says what it waited** (task p19-01, deliverable 3).
//
// A WAIT the debug port waits out is the one thing the next NoiseScope session on the unit at
// Bellevue can say that the eight before it could not: either the first try goes through and the
// line says what it cost, or it fails and the line says how long the port waited before it gave
// up. So a flash whose port waited gets **a line of the story in `flashing`**, written before
// anything is made of that flash, and it carries the step, the milliseconds, the page and how many
// transfers waited - in the thirty-two bits of a `kFlashWaited` entry's detail.
//
// A flash that waited for nothing writes no line: that is every flash this suite and every bench
// has ever made, and an entry nobody needs is an entry the journal's ring does not have to hold.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/update/radio_update.h"
#include "update_bench.h"

using bench::Kind;
using bench::ReadyUnit;
using bench::Reason;
using bench::State;
using bench::Which;
namespace upd = core::update;
namespace fl = core::flasher;

namespace {

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

unsigned countWith(const std::vector<upd::Payload>& story, Reason reason) {
  unsigned n = 0;
  for (const upd::Payload& p : story) {
    if (p.reason == reason) ++n;
  }
  return n;
}

bool entryWith(const std::vector<upd::Payload>& story, Reason reason, upd::Payload* out) {
  bool found = false;
  for (const upd::Payload& p : story) {
    if (p.reason != reason) continue;
    found = true;
    if (out != nullptr) *out = p;
  }
  return found;
}

constexpr std::uint32_t kErasePageMs = 85;  // PS v1.11, 4.3.10.1: tERASEPAGE

}  // namespace

TEST_CASE("a flash whose port waited an operation of the radio's out: one line of the story, and the update goes on") {
  ReadyUnit unit;
  // The radio writing its clock's file under the scan, which is the reading of the eight failed
  // first tries: one page busy for a page erase and a shade, at the first page the scan reads.
  unit.world.chip.stallPageFor(unit.world.profile.app.begin, kErasePageMs + 1);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);  // the flash went through at its first try

  const std::vector<upd::Payload> story = storyOf(w);
  upd::Payload waited;
  REQUIRE(entryWith(story, Reason::kFlashWaited, &waited));
  CHECK(waited.state == State::kFlashing);
  CHECK(countWith(story, Reason::kFlashWaited) == 1);
  CHECK(countWith(story, Reason::kFlashOnceMore) == 0);
  CHECK(countWith(story, Reason::kFlashFailed) == 0);

  const upd::FlashWait what = upd::decodeFlashWait(waited.detail);
  CHECK(what.step == fl::Step::kScan);
  CHECK(what.transfers == 1);
  CHECK(what.address == unit.world.profile.app.begin);
  const bool long_enough = what.longest_ms >= kErasePageMs + 1;
  const bool not_too_long = what.longest_ms <= kErasePageMs + 1 + 2 * core::swd::kIdlePauseMs;
  CHECK(long_enough);
  CHECK(not_too_long);

  // **The line, whole** (task p18-06 holds that no journal line is ever cut): the note the story
  // prints and the phrase a person at the console reads.
  char note[upd::kFlashWaitLineMax];
  const std::size_t n = upd::flashWaitNote(what, note, sizeof note);
  CHECK(n < 70);
  char line[upd::kFlashWaitLineMax];
  upd::flashWaitLine(what, line, sizeof line);
  MESSAGE("the journal's note: " << std::string(note));
  MESSAGE("the console's line:  " << std::string(line));
}

TEST_CASE("an ordinary flash writes no such line: the story is the six entries it always was") {
  ReadyUnit unit;
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  CHECK(w.run() == State::kDone);
  const std::vector<upd::Payload> story = storyOf(w);
  CHECK(countWith(story, Reason::kFlashWaited) == 0);
}

TEST_CASE("an operation that outlasts the bound: the line says how long it waited, and the second try follows") {
  ReadyUnit unit;
  // Longer than the bound and shorter than the pause between the two tries: the first try waits
  // the bound out and gives up with nothing written, and by the time the second try reaches the
  // same page the operation is over - which is what happened on the unit on 23 November, where
  // the next flash met the same chip two seconds later and went through.
  unit.world.chip.stallPageFor(unit.world.profile.app.begin, 2 * core::swd::kWaitBoundMs);
  bench::Walter& w = *unit.walter;
  REQUIRE(w.update.start(unit.request()));
  const State end = w.run();
  INFO(w.transitionText());
  CHECK(end == State::kDone);

  const std::vector<upd::Payload> story = storyOf(w);
  // The first try's own two lines: what it waited, and that the flash is being made once more.
  CHECK(countWith(story, Reason::kFlashOnceMore) == 1);
  upd::Payload waited;
  REQUIRE(entryWith(story, Reason::kFlashWaited, &waited));
  const upd::FlashWait what = upd::decodeFlashWait(waited.detail);
  CHECK(what.step == fl::Step::kScan);
  CHECK(what.longest_ms >= core::swd::kWaitBoundMs);
  CHECK(what.address == unit.world.profile.app.begin);
  char note[upd::kFlashWaitLineMax];
  upd::flashWaitNote(what, note, sizeof note);
  MESSAGE("the first try's line: " << std::string(note));
}

TEST_CASE("the detail's thirty-two bits: every field back as it went in, and the ones that saturate") {
  upd::FlashWait w;
  w.step = fl::Step::kReadBack;
  w.longest_ms = 86;
  w.address = 0x036000;
  w.transfers = 3;
  const upd::FlashWait back = upd::decodeFlashWait(upd::encodeFlashWait(w));
  CHECK(back.step == fl::Step::kReadBack);
  CHECK(back.longest_ms == 86);
  CHECK(back.address == 0x036000);
  CHECK(back.transfers == 3);

  // The address is kept to the page, as the failure's detail keeps it.
  w.address = 0x0360F4;
  CHECK(upd::decodeFlashWait(upd::encodeFlashWait(w)).address == 0x036000);
  // No address at all: nought, and the note leaves it out.
  w.address = 0;
  CHECK(upd::decodeFlashWait(upd::encodeFlashWait(w)).address == 0);
  char note[upd::kFlashWaitLineMax];
  upd::flashWaitNote(upd::decodeFlashWait(upd::encodeFlashWait(w)), note, sizeof note);
  CHECK(std::string(note).find("0x") == std::string::npos);

  // The two fields that saturate, and say so.
  w.address = 0x036000;
  w.transfers = 4000;
  w.longest_ms = 99999;
  const upd::FlashWait wide = upd::decodeFlashWait(upd::encodeFlashWait(w));
  CHECK(wide.transfers == upd::kFlashWaitTransfersMax);
  CHECK(wide.longest_ms == upd::kFlashWaitMsMax);
  upd::flashWaitNote(wide, note, sizeof note);
  CHECK(std::string(note).find("or more") != std::string::npos);
  MESSAGE("the saturated note: " << std::string(note));

  // Every step encodes and comes back, which is what the six bits are for.
  for (unsigned i = 0; i <= static_cast<unsigned>(fl::Step::kDone); ++i) {
    upd::FlashWait one;
    one.step = static_cast<fl::Step>(i);
    one.transfers = 1;
    CHECK(upd::decodeFlashWait(upd::encodeFlashWait(one)).step == one.step);
  }
}
