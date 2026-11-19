// `heap` (task p11-02): the four figures of the internal heap, the sentence that says which of
// them decides whether an image starts, and the help entry.
//
// Why this command exists at all is `docs/ARCHITECTURE.md` §14: v0.1.3 was a correct image that
// could not create its main task, and no figure this firmware printed would have shown it coming
// - `agent id`'s free heap is every capability and is read *after* `main_task` has registered
// 22 kB that did not exist when the stack was asked for. `tools/heap_at_start.py` is what refuses
// such a build; this command is how a board says what it actually got, so that the model can be
// held to a real start (`docs/bench.md` §1).
//
// The figures themselves are three `heap_caps_*` calls and `uxTaskGetStackHighWaterMark`
// (components/platform/heap.cpp), none of which exists here; what is held here is everything the
// console does around them - the shape of the lines, that it reads them once, that it changes
// nothing, and that a build handed no seam says so instead of printing zeros.
#include <doctest.h>

#include <string>

#include "core/console/heap_report.h"
#include "core/update/scanner_session.h"
#include "settings_console_rig.h"

using bench::hasText;
using bench::SettingsConsole;

TEST_CASE("heap prints the four figures, in two lines, and names the one that decides") {
  SettingsConsole c;
  const std::string out = c.run("heap");

  // The free, the block and the least-free, in one line, with the capabilities they are of: a
  // free that is not of the internal 8-bit heap would be a different number about a different
  // thing.
  CHECK(hasText(out, "heap: 33872 bytes free, largest free block 18432, "
                     "least free since this boot 28104 (internal, 8-bit)"));
  // The stack, in the next: what it is and what of it has never been used.
  CHECK(hasText(out, "main task stack: 16384 bytes, 7560 never used (the high-water mark)"));
  // And the sentence that is the whole point of the command.
  CHECK(hasText(out, "the block is what decides whether an image starts"));
  CHECK(hasText(out, "tools/heap_at_start.py"));
  // Read once, as one moment's four numbers.
  CHECK(c.heap_report.reads == 1);
}

TEST_CASE("heap only reads: no setting, no write, nothing of the radio") {
  SettingsConsole c;
  const std::size_t keys = c.kv.size();
  const int writes = c.kv.writes;
  const std::size_t frames = c.rak.requests.size();
  c.run("heap");
  CHECK(c.kv.size() == keys);
  CHECK(c.kv.writes == writes);
  CHECK(c.rak.requests.size() == frames);
  // There is no key for it, and `heap` is not a thing to be set.
  CHECK(hasText(c.run("get heap"), "unknown key"));
}

TEST_CASE("heap takes no argument") {
  SettingsConsole c;
  const std::string out = c.run("heap 1024");
  CHECK(hasText(out, "error: heap takes no argument"));
  CHECK(c.heap_report.reads == 0);
}

TEST_CASE("the figures are the seam's, whatever they are") {
  SettingsConsole c;
  // A board in the arrangement that failed: tens of kilobytes free and no block for a stack.
  // The console reports it and refuses nothing - what refuses is the build.
  c.heap_report.figures = core::console::HeapReport::Figures{35636, 16308, 35636, 16384, 1024};
  const std::string out = c.run("heap");
  CHECK(hasText(out, "heap: 35636 bytes free, largest free block 16308"));
  CHECK(hasText(out, "main task stack: 16384 bytes, 1024 never used"));
  CHECK_FALSE(hasText(out, "error"));
}

TEST_CASE("a build that was handed no heap seam says so in one line") {
  SettingsConsole c;
  c.withoutHeap();
  const std::string out = c.run("heap");
  CHECK(hasText(out, "error: this build has no heap figures"));
  CHECK(hasText(out, "main/main.cpp"));
  CHECK(c.heap_report.reads == 0);
  // One line, and no figure of any kind in it.
  CHECK_FALSE(hasText(out, "largest free block"));
  CHECK_FALSE(hasText(out, "33872"));
}

// ------------------------------------------------------- the PSRAM, and `heap room` (task p16-05)

TEST_CASE("heap says which of three things this board's PSRAM is") {
  SUBCASE("none at all") {
    SettingsConsole c;
    c.scratch.noExternal();
    const std::string out = c.run("heap");
    CHECK(hasText(out, "psram: none found - this build asks for none"));
  }
  SUBCASE("some, and nothing has brought it up in this boot") {
    SettingsConsole c;
    c.scratch.externalOf(2u * 1024u * 1024u);
    const std::string out = c.run("heap");
    CHECK(hasText(out, "psram: not brought up in this boot"));
    CHECK(hasText(out, "'heap room' brings it up"));
    // And asking does not bring it up: `heap` costs a board nothing.
    CHECK(c.scratch.bring_ups == 0);
  }
  SUBCASE("asked for in this boot, and it did not come up") {
    SettingsConsole c;
    c.scratch.externalWillNotComeUp(2u * 1024u * 1024u);
    c.run("heap room");
    const std::string out = c.run("heap");
    CHECK(hasText(out, "psram: none found - it was asked for in this boot and did not come up"));
  }
  SUBCASE("up, with its size and what is free of it") {
    SettingsConsole c;
    c.scratch.externalOf(2u * 1024u * 1024u);
    c.run("heap room");
    const std::string out = c.run("heap");
    CHECK(hasText(out, "psram: 2097152 bytes, 2097152 free"));
  }
}

TEST_CASE("heap room takes the room a session takes, prints its line and gives it back") {
  SettingsConsole c;
  c.scratch.externalOf(2u * 1024u * 1024u);
  const std::string out = c.run("heap room");
  // The session's own `room` line, with the PSRAM's figures in it.
  CHECK(hasText(out, "room: 65536 psram of 2097152; internal 33872, block 18432"));
  CHECK(hasText(out, "65536 bytes, external, written and read back; given back now"));
  // 64 kB asked for by name, which is what brought the PSRAM up at all.
  CHECK(c.scratch.bring_ups == 1);
  CHECK(c.scratch.external_asks == 1);
  CHECK(c.scratch.external_last_asked == core::update::kStepBytesExternal);
  // And nothing is left taken: the line printed after it says the whole of it is free again.
  CHECK(c.scratch.out == 0);
  CHECK(c.scratch.gives == 1);
  CHECK(hasText(out, "psram: 2097152 bytes, 2097152 free"));
  // Two reads of the heap seam, which is what taking a room costs: before, and after.
  CHECK(c.heap_report.reads == 2);
}

TEST_CASE("heap room on a board with no PSRAM prints task p16-03's own line") {
  SettingsConsole c;
  c.scratch.noExternal();
  const std::string out = c.run("heap room");
  CHECK(hasText(out, "room: 8192 of 33872, block 18432"));
  CHECK(hasText(out, "8192 bytes, the internal heap, written and read back; given back now"));
  CHECK_FALSE(hasText(out, "psram of"));
  CHECK(c.scratch.out == 0);
}

TEST_CASE("heap room with neither room to be had says so and takes nothing") {
  SettingsConsole c;
  c.scratch.noExternal();
  c.scratch.onlyHas(core::update::kStepBytes - 1);
  const std::string out = c.run("heap room");
  CHECK(hasText(out, "error: heap room: neither room could be had"));
  CHECK(hasText(out, "the largest is 8191"));
  CHECK(c.scratch.out == 0);
}

TEST_CASE("heap takes no argument but 'room'") {
  SettingsConsole c;
  CHECK(hasText(c.run("heap rooms"), "error: heap takes no argument, or 'room': rooms"));
  CHECK(c.scratch.external_asks == 0);
  CHECK(c.scratch.asks == 0);
}

TEST_CASE("help has the entry, and it says what the block is for") {
  SettingsConsole c;
  const std::string out = c.run("help");
  CHECK(hasText(out, "heap"));
  CHECK(hasText(out, "the internal heap now: free, the largest free block"));
  CHECK(hasText(out, "decides whether an image starts at all"));
  // Under *the unit*, where `reboots` and `dump` are: it is about this board and no radio.
  CHECK(hasText(out, "the unit"));
  // And `heap room`'s own entry, which is what a Walter alone proves its PSRAM with.
  CHECK(hasText(out, "heap room"));
  CHECK(hasText(out, "A Walter with no radio proves its"));
}
