// The update journal (task p2-03): entries whole or absent, the last valid one governing
// whatever a power cut did to the one after it, and a ring that never loses the operation in
// progress.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/update/journal.h"
#include "sim/sim_flash.h"

using core::update::Cursor;
using core::update::Entry;
using core::update::Journal;
using core::update::JournalError;
using core::update::kEntryBytes;
namespace kind = core::update::kind;

namespace {

std::vector<std::uint8_t> payloadOf(std::uint8_t first, std::size_t size = 8) {
  std::vector<std::uint8_t> p(size, 0);
  p[0] = first;
  return p;
}

JournalError add(Journal& j, std::uint16_t k, std::uint8_t first, std::size_t size = 8) {
  const std::vector<std::uint8_t> p = payloadOf(first, size);
  return j.append(k, p.data(), p.size());
}

// Every entry walking backwards from the newest, as (seq, payload[0]).
std::vector<std::pair<std::uint32_t, std::uint8_t>> walk(const Journal& j) {
  std::vector<std::pair<std::uint32_t, std::uint8_t>> out;
  Cursor c;
  Entry e;
  if (!j.first(&c, &e)) return out;
  out.emplace_back(e.seq, e.payload[0]);
  while (j.previous(&c, &e)) out.emplace_back(e.seq, e.payload[0]);
  return out;
}

}  // namespace

TEST_CASE("an empty journal, then entries in order, each the last") {
  sim::SimFlash flash(core::update::kJournalPartitionBytes);
  Journal j(flash);
  REQUIRE(j.begin() == JournalError::kNone);
  CHECK(j.empty());
  CHECK(j.sectors() == 16);
  CHECK(j.nextSeq() == 1);
  Entry e;
  CHECK_FALSE(j.last(&e));
  CHECK_FALSE(j.lastOf(kind::kRadioUpdate, &e));

  REQUIRE(add(j, kind::kRadioUpdate, 0x11) == JournalError::kNone);
  REQUIRE(add(j, kind::kSelfUpdate, 0x22) == JournalError::kNone);
  REQUIRE(add(j, kind::kRadioUpdate, 0x33, 48) == JournalError::kNone);
  REQUIRE(j.last(&e));
  CHECK(e.seq == 3);
  CHECK(e.kind == kind::kRadioUpdate);
  CHECK(e.size == 48);
  CHECK(e.payload[0] == 0x33);
  REQUIRE(j.lastOf(kind::kSelfUpdate, &e));
  CHECK(e.seq == 2);
  CHECK(e.payload[0] == 0x22);
  CHECK(j.validEntries() == 3);
  // Each entry is two writes: the body with its CRC, then the magic word, nothing else.
  CHECK(flash.log.size() == 6);
  CHECK(flash.log[0].offset == 0);
  CHECK(flash.log[0].count == core::update::kMagicOffset);
  CHECK(flash.log[1].offset == core::update::kMagicOffset);
  CHECK(flash.log[1].count == 4);
  CHECK(flash.log[2].offset == kEntryBytes);

  // Read again from flash: the same.
  Journal again(flash);
  REQUIRE(again.begin() == JournalError::kNone);
  CHECK(again.nextSeq() == 4);
  CHECK(again.validEntries() == 3);
  REQUIRE(again.last(&e));
  CHECK(e.seq == 3);
  const auto walked = walk(again);
  REQUIRE(walked.size() == 3);
  CHECK(walked[0].first == 3);
  CHECK(walked[1].first == 2);
  CHECK(walked[2].first == 1);
  CHECK(walked[2].second == 0x11);
}

TEST_CASE("refusals: a payload too large, no kind, not begun") {
  sim::SimFlash flash(core::update::kJournalPartitionBytes);
  Journal j(flash);
  std::uint8_t p[49] = {};
  CHECK(j.append(kind::kRadioUpdate, p, 8) == JournalError::kNotBegun);
  REQUIRE(j.begin() == JournalError::kNone);
  CHECK(j.append(kind::kRadioUpdate, p, 49) == JournalError::kTooLarge);
  CHECK(j.append(kind::kNone, p, 8) == JournalError::kBadKind);
  CHECK(j.append(kind::kRadioUpdate, p, 48) == JournalError::kNone);
  CHECK(j.append(kind::kRadioUpdate, nullptr, 0) == JournalError::kNone);  // an entry with no payload
  CHECK(flash.log.size() == 4);
  sim::SimFlash tiny(4096);
  Journal t(tiny);
  CHECK(t.begin() == JournalError::kRegion);  // one sector is no ring
  sim::SimFlash odd(8192, 1000);
  Journal o(odd);
  CHECK(o.begin() == JournalError::kRegion);
}

TEST_CASE("the power goes at every write of an entry: the entry before it governs, and the torn slot is skipped") {
  // Two entries cleanly, then a third with the power going at each of its two writes in turn.
  for (std::uint32_t op = 1; op <= 2; ++op) {
    CAPTURE(op);
    sim::SimFlash flash(core::update::kJournalPartitionBytes);
    Journal j(flash);
    REQUIRE(j.begin() == JournalError::kNone);
    REQUIRE(add(j, kind::kRadioUpdate, 0x01) == JournalError::kNone);
    REQUIRE(add(j, kind::kRadioUpdate, 0x02) == JournalError::kNone);
    flash.powerOn();
    flash.lose_power_at_op = op;
    CHECK(add(j, kind::kRadioUpdate, 0x03) == JournalError::kFlashWrite);
    CHECK_FALSE(flash.powered());

    // The Walter comes back.
    flash.powerOn();
    Journal again(flash);
    REQUIRE(again.begin() == JournalError::kNone);
    Entry e;
    REQUIRE(again.last(&e));
    CHECK(e.seq == 2);
    CHECK(e.payload[0] == 0x02);
    CHECK(again.validEntries() == 2);
    // The torn slot is used: the next entry goes past it, with the next seq.
    CHECK(again.headSlot() == 3);
    REQUIRE(add(again, kind::kRadioUpdate, 0x04) == JournalError::kNone);
    REQUIRE(again.last(&e));
    CHECK(e.seq == 3);
    CHECK(e.payload[0] == 0x04);
    const auto walked = walk(again);
    REQUIRE(walked.size() == 3);  // the torn slot stepped over
    CHECK(walked[0].second == 0x04);
    CHECK(walked[1].second == 0x02);
    CHECK(walked[2].second == 0x01);
  }
}

TEST_CASE("a torn entry at the very first slot, and one with a bad CRC") {
  sim::SimFlash flash(core::update::kJournalPartitionBytes);
  {
    Journal j(flash);
    REQUIRE(j.begin() == JournalError::kNone);
    flash.lose_power_at_op = 1;
    CHECK(add(j, kind::kRadioUpdate, 0x01) != JournalError::kNone);
  }
  flash.powerOn();
  Journal j(flash);
  REQUIRE(j.begin() == JournalError::kNone);
  CHECK(j.empty());
  CHECK(j.headSector() == 0);
  CHECK(j.headSlot() == 1);
  REQUIRE(add(j, kind::kRadioUpdate, 0x02) == JournalError::kNone);
  Entry e;
  REQUIRE(j.last(&e));
  CHECK(e.seq == 1);
  // A bit flipped in a stored entry's body: its CRC fails, it is not an entry any more, and the
  // one before it governs.
  REQUIRE(add(j, kind::kRadioUpdate, 0x03) == JournalError::kNone);
  flash.rot(2 * kEntryBytes + 10, 0x04);
  Journal again(flash);
  REQUIRE(again.begin() == JournalError::kNone);
  REQUIRE(again.last(&e));
  CHECK(e.seq == 1);
  CHECK(e.payload[0] == 0x02);
}

TEST_CASE("the ring wraps, erasing the oldest sector first, and never the operation in progress") {
  // A small ring: two sectors of 64 entries. 200 entries fit only by wrapping twice over.
  sim::SimFlash flash(2 * 4096);
  Journal j(flash);
  REQUIRE(j.begin() == JournalError::kNone);
  CHECK(j.sectors() == 2);
  for (std::uint32_t i = 1; i <= 200; ++i) {
    REQUIRE(add(j, kind::kRadioUpdate, static_cast<std::uint8_t>(i)) == JournalError::kNone);
  }
  Entry e;
  REQUIRE(j.last(&e));
  CHECK(e.seq == 200);
  // Entries 1-64 fill sector 0 and 65-128 sector 1, both blank and erased by nobody; entry 129
  // erases sector 0 for the second lap and entry 193 erases sector 1.
  CHECK(j.erases() == 2);
  // Walked back: 200 down to the oldest the ring still holds, in order, no gap.
  const auto walked = walk(j);
  REQUIRE(walked.size() >= 64);
  for (std::size_t i = 0; i < walked.size(); ++i) CHECK(walked[i].first == 200 - i);
  // The same after a reboot.
  Journal again(flash);
  REQUIRE(again.begin() == JournalError::kNone);
  REQUIRE(again.last(&e));
  CHECK(e.seq == 200);
  CHECK(again.nextSeq() == 201);
  CHECK(walk(again).size() == walked.size());

  // Protect the newest entry and fill the ring: the append that would erase its sector is
  // refused, and nothing was erased.
  REQUIRE(again.protect(200));
  const std::uint32_t erases_before = again.erases();
  JournalError last = JournalError::kNone;
  int appended = 0;
  for (int i = 0; i < 200 && last == JournalError::kNone; ++i) {
    last = add(again, kind::kRadioUpdate, 0xAA);
    if (last == JournalError::kNone) ++appended;
  }
  CHECK(last == JournalError::kFull);
  CHECK(appended < 200);
  CHECK(appended >= 64);  // the other sector could still be filled (and erased, as it held nothing protected)
  REQUIRE(again.last(&e));
  // Seq 200 is still there.
  bool found = false;
  for (const auto& one : walk(again)) {
    if (one.first == 200) found = true;
  }
  CHECK(found);
  // Unprotected, it goes on.
  again.unprotect();
  CHECK(add(again, kind::kRadioUpdate, 0xBB) == JournalError::kNone);
  CHECK(again.erases() > erases_before);
  // Protecting a seq the ring no longer holds is reported false.
  CHECK_FALSE(again.protect(1));
  CHECK_FALSE(again.protect(0));
  CHECK_FALSE(again.protect(again.nextSeq()));
}

TEST_CASE("the power goes at every write and erase of a wrap: afterwards the newest entry is the last one written, or the one before it") {
  sim::SimFlash probe(2 * 4096);
  Journal pj(probe);
  REQUIRE(pj.begin() == JournalError::kNone);
  for (std::uint32_t i = 1; i <= 64; ++i) REQUIRE(add(pj, kind::kRadioUpdate, 1) == JournalError::kNone);
  const std::uint32_t before = probe.operations();
  REQUIRE(add(pj, kind::kRadioUpdate, 2) == JournalError::kNone);  // the 65th: erase sector 1, then the two writes
  const std::uint32_t ops = probe.operations() - before;
  CHECK(ops == 3);

  for (std::uint32_t op = 1; op <= ops; ++op) {
    CAPTURE(op);
    sim::SimFlash flash(2 * 4096);
    Journal j(flash);
    REQUIRE(j.begin() == JournalError::kNone);
    for (std::uint32_t i = 1; i <= 64; ++i) REQUIRE(add(j, kind::kRadioUpdate, 1) == JournalError::kNone);
    flash.powerOn();
    flash.lose_power_at_op = op;
    CHECK(add(j, kind::kRadioUpdate, 2) != JournalError::kNone);
    flash.powerOn();
    Journal again(flash);
    REQUIRE(again.begin() == JournalError::kNone);
    Entry e;
    REQUIRE(again.last(&e));
    CHECK(e.seq == 64);
    CHECK(again.nextSeq() == 65);
    REQUIRE(add(again, kind::kRadioUpdate, 3) == JournalError::kNone);
    REQUIRE(again.last(&e));
    CHECK(e.seq == 65);
    CHECK(e.payload[0] == 3);
  }
}
