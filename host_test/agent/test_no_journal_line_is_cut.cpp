// **No line this firmware writes into a journal is ever cut** (task p18-06, deliverable 4).
//
// On 24 November the one line that said why a session had been refused reached meshmaster.dk as
// *the flash did not begin, twice: nothing was written and the radio is a* - seventy of the
// seventy-eight characters of a sentence, cut mid-word, with the rest of it on the unit. A journal
// note is `agent::kJournalNoteBytes` bytes, terminator and all, which is seventy-one characters;
// what this file does is work out the **longest text every line can take** and hold it under that
// bound, for the lines task p18-05 added and for every line there was before them.
//
// A line that does not fit is mended by writing the sentence shorter, never by cutting it: task
// p18-06 shortened eight of the radio update machine's reasons, six of the self-update's, five of a
// session's refusals, one stage's phrase and the trial's uplink line, and gave the rolling-back
// note the step's **word** where it used to carry the step's phrase cut at twenty-four characters.
//
// The one exception, and it is marked rather than silent: a **panic line** comes off a coredump and
// is a person's text and not a sentence of ours. It is carried whole in the result's own `panic`
// member with `panicCut` beside it, and the journal line ends in `...` when there was more.
#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/health/coredump.h"
#include "core/health/health.h"
#include "core/update/radio_update.h"
#include "core/update/scanner_session.h"
#include "core/update/self_update.h"
#include "walter_agent/firmware_jobs.h"

namespace upd = core::update;

namespace {

// What a note may be, in characters: the bytes a line holds, less the terminator.
constexpr std::size_t kLongest = agent::kJournalNoteBytes - 1;

struct Longest {
  std::string kind;
  std::string text;
};

void hold(std::vector<Longest>& longest, const std::string& kind, const std::string& text) {
  CAPTURE(kind);
  CAPTURE(text);
  CHECK_MESSAGE(text.size() <= kLongest,
                "a journal line of " << text.size() << " characters, where " << kLongest
                                     << " fit: " << kind << ": " << text);
  for (Longest& one : longest) {
    if (one.kind != kind) continue;
    if (text.size() > one.text.size()) one.text = text;
    return;
  }
  longest.push_back(Longest{kind, text});
}

}  // namespace

TEST_CASE("the longest line of every kind a journal note is made of, whole") {
  std::vector<Longest> longest;

  // ---- the radio update machine's reasons, every value of the enum ----------------------------
  //
  // `firmware_jobs.cpp` writes `describe(reason)` as a note at an apply's refusal at check, at the
  // ending of a flash that never began, and for every line of the look and the wait task p18-04
  // added. The enum is walked whole rather than the reasons that can refuse, because a reason that
  // becomes a note later must not have to be found by a person first.
  for (int v = 0; v <= 255; ++v) {
    const upd::Reason reason = static_cast<upd::Reason>(v);
    const std::string text = upd::describe(reason);
    if (text == "?" || text.empty()) continue;  // not a value of the enum
    hold(longest, "a reason of the radio's update machine", text);
    // ---- and the rolling-back line, which is a reason **and** the step it failed at -----------
    for (int s = 0; s <= 8; ++s) {
      const upd::ProveStep step = static_cast<upd::ProveStep>(s);
      char note[agent::kJournalNoteBytes];
      const int n = std::snprintf(note, sizeof note, "%s at %s", text.c_str(), upd::name(step));
      CAPTURE(v);
      CAPTURE(s);
      // snprintf would have written this many: more than the buffer is a line that was cut.
      // Named first, because doctest will not take a `&&` inside a CHECK.
      const bool fits = n >= 0 && static_cast<std::size_t>(n) <= kLongest;
      CHECK_MESSAGE(fits, "a rolling-back line of " << n << " characters: " << text << " at "
                                                    << upd::name(step));
      hold(longest, "a rolling-back line: the reason and the step", std::string(note));
    }
  }

  // ---- the Walter's own machine's reasons ----------------------------------------------------
  //
  // Written as a note at a self-apply's refusal and as one line per entry of the trial's story
  // (`postOwedResult`).
  for (int v = 0; v <= 255; ++v) {
    const std::string text = upd::describe(static_cast<upd::SelfReason>(v));
    if (text == "?" || text.empty()) continue;
    hold(longest, "a reason of the Walter's own update machine", text);
  }

  // ---- a session's refusals and its stages ---------------------------------------------------
  for (int v = 0; v <= static_cast<int>(upd::Refusal::kFlash); ++v) {
    hold(longest, "a session's refusal", upd::describe(static_cast<upd::Refusal>(v)));
  }
  for (int v = 0; v <= static_cast<int>(upd::Stage::kRefused); ++v) {
    hold(longest, "a session's stage", upd::describe(static_cast<upd::Stage>(v)));
  }

  // ---- what a flash that did not finish says -------------------------------------------------
  //
  // Every shape of `flashFailureNote`: the step, the layer under it, the address and whether the
  // chip was changed, with `2nd try ` in front of it. The longest of each field together, which is
  // a line no run makes but a bound has to cover.
  for (int step = 0; step <= 8; ++step) {
    for (int status = 0; status <= 12; ++status) {
      for (int swd = 0; swd <= 6; ++swd) {
        for (int flags = 0; flags < 16; ++flags) {
          upd::FlashFailure f;
          f.detailed = (flags & 1) != 0;
          f.second_try = (flags & 2) != 0;
          f.wrote_nothing = (flags & 4) != 0;
          f.core_halted = (flags & 8) != 0;
          f.refused = (flags & 8) != 0;
          f.address_known = true;
          f.address = 0xFFFFFF;
          f.step = static_cast<core::flasher::Step>(step);
          f.status = static_cast<core::nrf52::Status>(status);
          f.swd = static_cast<core::swd::Status>(swd);
          // Into a buffer **wider** than a note, so that what is measured is the text the
          // function would write and not what a note cut it to: it stops at the cap it is given.
          char wide[upd::kFlashFailureLineMax];
          const std::size_t n = upd::flashFailureNote(f, wide, sizeof wide);
          CAPTURE(step);
          CAPTURE(status);
          CAPTURE(swd);
          CAPTURE(flags);
          hold(longest, "a flash that did not finish", std::string(wide, n));
        }
      }
    }
  }

  // ---- a session's `room` line ---------------------------------------------------------------
  //
  // The five figures of task p16-05, at their widest: the bound is the task's own and the
  // static_assert in `firmware_jobs.h` holds a note to it.
  //
  // The figures at the widest **this chip** can make them, which is the bound the line was sized
  // to (`core/update/scanner_session.h`, kRoomLineMax: *every figure of either is under a megabyte
  // on this chip bar the PSRAM's own*): eight megabytes of external RAM, and six digits for each
  // figure of a heap that is 512 kB. Into a buffer wider than a note, for the reason above.
  {
    upd::RoomFigures f;
    f.known = true;
    f.bytes = 999999;
    f.free_bytes = 999999;
    f.block = 999999;
    f.left_block = 999999;
    f.least_free = 999999;
    f.external = 8 * 1024 * 1024;
    char wide[256];
    const std::size_t n = upd::roomLine(f, wide, sizeof wide);
    hold(longest, "a session's room line", std::string(wide, n));
    // And the other shape, where the room came out of the internal heap.
    f.external = 0;
    const std::size_t m = upd::roomLine(f, wide, sizeof wide);
    hold(longest, "a session's room line", std::string(wide, m));
  }

  // ---- the lines made of numbers and of text from outside ------------------------------------
  {
    char note[agent::kJournalNoteBytes];
    std::snprintf(note, sizeof note, "battery %u mV, link up, clock set", 65535u);
    hold(longest, "the check line of a session", std::string(note));
    // A step's losses are counted inside one step of at most `kStepSecondsMax`, so five digits
    // are more than the port can lose in it.
    std::snprintf(note, sizeof note, "the port lost bytes %lu time(s): lines are missing", 99999ul);
    hold(longest, "a step that lost bytes at the port", std::string(note));
    // The trial's window is minutes and its heartbeats are one a wake, so five digits of each are
    // wider than a trial can be. Written into a buffer wider than a note, so that what is measured
    // is the sentence and not what a note cut it to.
    char wide[128];
    std::snprintf(wide, sizeof wide, "no heartbeat was sent in the %lu s the window lasted",
                  99999ul);
    hold(longest, "the trial's uplink line", std::string(wide));
    for (int v = 0; v <= 4; ++v) {
      std::snprintf(wide, sizeof wide, "%lu heartbeat(s) in %lu s; the last: %s", 99999ul, 99999ul,
                    upd::describe(static_cast<upd::UplinkVerdict>(v)));
      hold(longest, "the trial's uplink line", std::string(wide));
    }
    // A step's command, which is a person's text bounded by the contract at kCommandMax.
    std::string command(upd::kCommandMax, 'x');
    hold(longest, "a step's command", command);
    // The reset that ended a trial, by name.
    for (int v = 0; v <= 12; ++v) {
      const std::string text = core::health::name(static_cast<core::health::ResetReason>(v));
      if (text.empty()) continue;
      hold(longest, "the reset that ended a trial", text);
    }
  }

  // ---- the one note that is a person's text and may be longer than a line --------------------
  //
  // A panic line of `kPanicMax` characters: marked, not cut mid-word, and the result carries it
  // whole in its own member.
  {
    const std::string panic(core::health::kPanicMax, 'P');
    char note[agent::kJournalNoteBytes];
    if (panic.size() > agent::kJournalNoteBytes - 1) {
      std::snprintf(note, sizeof note, "%.68s...", panic.c_str());
    } else {
      std::snprintf(note, sizeof note, "%.71s", panic.c_str());
    }
    const std::string text(note);
    CHECK(text.size() <= kLongest);
    CHECK(text.substr(text.size() - 3) == "...");
    hold(longest, "a panic line (marked where there was more)", text);
  }

  // What each kind's longest actually is, for the report.
  std::string said;
  for (const Longest& one : longest) {
    char line[256];
    std::snprintf(line, sizeof line, "%3zu  %-44s %s\n", one.text.size(), one.kind.c_str(),
                  one.text.c_str());
    said += line;
  }
  MESSAGE("the longest line of every kind, of the " << kLongest << " characters a note holds:\n"
                                                    << said);
}

TEST_CASE("a journal note holds a session's room line and the words the contract bounds") {
  // The static_assert of `firmware_jobs.h`, said out loud: the note was sized for the room line.
  CHECK(agent::kJournalNoteBytes >= upd::kRoomLineMax);
  // And the server's own bounds on a journal, which are about the whole array and not one line
  // (meshmaster.dk's `FirmwareJournalBounds`).
  CHECK(agent::kJournalMaxEntries == 40);
  CHECK(agent::kJournalMaxChars == 2000);
}
