#include "core/update/scanner_session.h"

#include <cstdio>

#include "core/flasher/board_profile.h"
#include "core/link/client/text_line.h"

namespace core {
namespace update {

namespace lc = link::client;

const char* const kVerbs[kVerbCount] = {"stat", "sweep", "scope", "hist", "dwell", "cad"};

namespace {

bool sameText(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  for (; *a != 0 && *b != 0; ++a, ++b) {
    if (*a != *b) return false;
  }
  return *a == 0 && *b == 0;
}

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::size_t textLength(const char* text) {
  std::size_t n = 0;
  while (text != nullptr && text[n] != 0) ++n;
  return n;
}

// A byte a command line may carry: `a`-`z`, `0`-`9`, a space and the full stop. Nothing else -
// not an upper-case letter (the scanner's words are lower case), not a semicolon, not a byte
// outside printable ASCII. A line of the scanner's grammar is all of these and a line that could
// be read as two commands is none of them.
bool allowedByte(char c) {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= '0' && c <= '9') return true;
  return c == ' ' || c == '.';
}

// Which stage of the session a state of the radio's update machine is, while the golden image is
// going back. `kIdle` for the states that have no line of their own: the machine's own `check`
// and `committing` are a store read and cost nothing beside the flash.
Stage backStageOf(State state) {
  switch (state) {
    case State::kRollingBack:
    case State::kFlashing: return Stage::kFlashingGolden;
    case State::kBooting:
    case State::kRollbackBooting: return Stage::kBooting;
    case State::kProving:
    case State::kRollbackProving: return Stage::kProving;
    default: return Stage::kIdle;
  }
}

// Whether the machine is putting the golden image back by itself: a `kRoleScanner` run whose
// scanner never showed up rolls back, which **is** the switch back and is picked up rather than
// started again.
bool isRollingBack(State state) {
  return state == State::kRollingBack || state == State::kRollbackBooting ||
         state == State::kRollbackProving;
}

}  // namespace

// ------------------------------------------------------------------------------- the words

const char* name(Stage stage) {
  switch (stage) {
    case Stage::kIdle: return "idle";
    case Stage::kCheck: return "check";
    case Stage::kFlashingScanner: return "flashing-scanner";
    case Stage::kScanner: return "scanner";
    case Stage::kStep: return "step";
    case Stage::kRoom: return "room";
    case Stage::kFlashingGolden: return "flashing-golden";
    case Stage::kBooting: return "booting";
    case Stage::kProving: return "proving";
    case Stage::kRestored: return "restored";
    case Stage::kStuck: return "stuck";
    case Stage::kRefused: return "refused";
  }
  return "?";
}

const char* describe(Stage stage) {
  switch (stage) {
    case Stage::kIdle: return "no session";
    case Stage::kCheck: return "the preconditions held: a session is open, nothing is flashed yet";
    case Stage::kFlashingScanner: return "the scanner image is going on";
    case Stage::kScanner: return "a scanner is there";
    case Stage::kStep: return "a step is running";
    case Stage::kRoom: return "what the room cost, before the switch back";
    case Stage::kFlashingGolden: return "the golden image is going back";
    case Stage::kBooting: return "the radio was reset: waiting for it to answer";
    case Stage::kProving: return "the golden image's health check";
    case Stage::kRestored: return "the repeater is back and proved: the session is closed";
    case Stage::kStuck: return "the golden image did not come back: the session is still open";
    case Stage::kRefused:
      return "the scanner image's flash never began: the radio was not touched and the session is "
             "closed";
  }
  return "?";
}

const char* word(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone: return "none";
    case Refusal::kBattery: return "battery";
    case Refusal::kLink: return "link";
    case Refusal::kClock: return "clock";
    case Refusal::kInProgress: return "inProgress";
    case Refusal::kNoGolden: return "noGolden";
    case Refusal::kNoScanner: return "noScanner";
    case Refusal::kProgram: return "program";
    case Refusal::kNoMemory: return "noMemory";
    case Refusal::kJournal: return "journal";
  }
  return "?";
}

const char* describe(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone: return "nothing";
    case Refusal::kBattery:
      return "the battery: it could not be read, is below the update threshold, or is falling fast";
    case Refusal::kLink: return "the radio does not answer, and nobody has said why";
    case Refusal::kClock:
      return "this board does not know what time it is: a scanner would be given no clock";
    case Refusal::kInProgress:
      return "something is already in the air: a radio update is journalled unfinished, or this "
             "firmware is on trial (S-7)";
    case Refusal::kNoGolden: return "there is no golden image to come back to";
    case Refusal::kNoScanner:
      return "there is no scanner image this slot may hold: none stored, one that does not "
             "verify, or one of another board or band";
    case Refusal::kProgram: return "the program is not one this firmware will run";
    case Refusal::kNoMemory:
      return "the heap has no block for a step's lines: nothing was touched";
    case Refusal::kJournal:
      return "the journal would not take the entry that opens a session: nothing was touched";
  }
  return "?";
}

const char* name(StepEnd end) {
  switch (end) {
    case StepEnd::kNone: return "none";
    case StepEnd::kDone: return "done";
    case StepEnd::kClock: return "clock";
    case StepEnd::kFull: return "full";
    case StepEnd::kRefused: return "refused";
    case StepEnd::kSilent: return "silent";
  }
  return "?";
}

const char* describe(StepEnd end) {
  switch (end) {
    case StepEnd::kNone: return "it has not ended";
    case StepEnd::kDone: return "the scanner said it had finished the command";
    case StepEnd::kClock: return "its seconds were up, and `stop` was said";
    case StepEnd::kFull: return "the room was used up, and `stop` was said";
    case StepEnd::kRefused: return "the scanner's ack said not ok";
    case StepEnd::kSilent: return "no ack came";
  }
  return "?";
}

const char* name(Ending ending) {
  switch (ending) {
    case Ending::kRunning: return "running";
    case Ending::kRestored: return "restored";
    case Ending::kRefused: return "refused";
    case Ending::kStuck: return "stuck";
  }
  return "?";
}

const char* describe(Ending ending) {
  switch (ending) {
    case Ending::kRunning: return "the session is still running";
    case Ending::kRestored: return "the repeater is back and proved, and the session is closed";
    case Ending::kRefused: return "a precondition: nothing was touched";
    case Ending::kStuck:
      return "the golden image did not come back: the session stays open and the next wake tries "
             "again";
  }
  return "?";
}

// ------------------------------------------------------------------------------- the program

// One pass over the program, answering the first rule it breaks as a phrase. `judge()` is the
// same pass reading only whether there was one, so that a console's line and a job's refusal
// cannot disagree about which programs run.
const char* whyProgram(const Program& program) {
  if (program.count == 0) return "a program needs at least one step";
  if (program.count > kStepsMax) return "a program is at most six steps";
  std::uint32_t total = 0;
  for (std::size_t i = 0; i < program.count; ++i) {
    const ProgramStep& s = program.step[i];
    // **Bounded, not `strlen`**: the field is 41 bytes and a caller that filled all of them has
    // handed over a 41-character command with nowhere to put its terminator. Reading past it to
    // find that out would be the bug this check is about.
    std::size_t n = 0;
    while (n < sizeof s.command && s.command[n] != 0) ++n;
    if (n == 0) return "a step's command cannot be empty";
    if (n > kCommandMax) return "a step's command is at most 40 characters";
    for (std::size_t k = 0; k < n; ++k) {
      if (!allowedByte(s.command[k])) {
        return "a command is lower-case letters, digits, spaces and full stops and nothing else";
      }
    }
    if (s.command[0] == ' ' || s.command[n - 1] == ' ') {
      return "a command neither begins nor ends with a space";
    }
    // The first word, against the six that measure something.
    std::size_t end = 0;
    while (end < n && s.command[end] != ' ') ++end;
    bool known = false;
    for (std::size_t v = 0; v < kVerbCount && !known; ++v) {
      const char* verb = kVerbs[v];
      std::size_t k = 0;
      for (; k < end && verb[k] != 0 && verb[k] == s.command[k]; ++k) {
      }
      known = k == end && verb[k] == 0;
    }
    if (!known) {
      return "a command's first word is one of stat, sweep, scope, hist, dwell, cad";
    }
    if (s.seconds < kStepSecondsMin) return "a step is given at least 5 seconds";
    if (s.seconds > kStepSecondsMax) return "a step is given at most 300 seconds";
    total += s.seconds;
  }
  if (total > kProgramSecondsMax) return "a program asks for at most 600 seconds in all";
  return nullptr;
}

Refusal judge(const Program& program) {
  return whyProgram(program) == nullptr ? Refusal::kNone : Refusal::kProgram;
}

// ------------------------------------------------------------------------------- the room's line

std::size_t roomLine(const RoomFigures& f, char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  // Four shapes, two of which are task p16-05's. Where the room came out of comes first, because
  // it is the thing a person reading a session's journal on meshmaster.dk is looking for; then
  // what the **internal** heap had while it was out, which says whether anything else on this
  // board was left short.
  //
  //   65536 psram of 2097152; internal 27472, block 12288; least 25300
  //   65536 psram of 2097152; this build reports no heap figures
  //   8192 of 27472, block 12288; left block 4096; least free 17108
  //   8192 taken; this build reports no heap figures
  //
  // Every figure but the PSRAM's own is under a megabyte on this chip, so six digits each is the
  // worst case and the longest of the four is 67 bytes: inside kRoomLineMax. A build with no heap
  // seam says that instead - the figures decide nothing, so a build without them is not a build
  // that refuses anything.
  int n = 0;
  if (f.external != 0 && f.known) {
    n = std::snprintf(out, cap, "%lu psram of %lu; internal %lu, block %lu; least %lu",
                      static_cast<unsigned long>(f.bytes),
                      static_cast<unsigned long>(f.external),
                      static_cast<unsigned long>(f.free_bytes),
                      static_cast<unsigned long>(f.block),
                      static_cast<unsigned long>(f.least_free));
  } else if (f.external != 0) {
    n = std::snprintf(out, cap, "%lu psram of %lu; this build reports no heap figures",
                      static_cast<unsigned long>(f.bytes),
                      static_cast<unsigned long>(f.external));
  } else if (f.known) {
    n = std::snprintf(out, cap, "%lu of %lu, block %lu; left block %lu; least free %lu",
                      static_cast<unsigned long>(f.bytes), static_cast<unsigned long>(f.free_bytes),
                      static_cast<unsigned long>(f.block),
                      static_cast<unsigned long>(f.left_block),
                      static_cast<unsigned long>(f.least_free));
  } else {
    n = std::snprintf(out, cap, "%lu taken; this build reports no heap figures",
                      static_cast<unsigned long>(f.bytes));
  }
  if (n <= 0) {
    out[0] = 0;
    return 0;
  }
  return static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
}

// ---------------------------------------------------------------------- the room, and its pattern
//
// Task p16-05. The room is 64 kB of the module's PSRAM where there is PSRAM that answers, and
// task p16-03's 8 192 bytes of the internal heap where there is not. Both doors take it through
// this one function - a session's `start()` and the console's `heap room` - so that what a person
// proves on a cable is byte for byte what a session gets.

namespace {

// The 32-bit word at index `at32`, derived from its own index: the FNV-1a prime times the index,
// exclusive-ored with 0xA5A5A5A5. Two addresses that answer for each other read back as each
// other's value and are caught; a bus that answers a constant is caught; a stuck bit is caught.
std::uint32_t patternWord(std::size_t at32) {
  return (static_cast<std::uint32_t>(at32) * 0x01000193u) ^ 0xA5A5A5A5u;
}

char patternByte(std::size_t at) {
  const std::uint32_t word = patternWord(at / 4);
  return static_cast<char>((word >> (8u * (at % 4))) & 0xFFu);
}

}  // namespace

bool patternHolds(char* room, std::size_t bytes) {
  if (room == nullptr || bytes == 0) return false;
  // A byte at a time: what the seam hands over is bytes, and nothing here may assume the
  // alignment of it. The whole room is written before any of it is read back, which is what makes
  // an address line that answers for another one show up at all.
  for (std::size_t at = 0; at < bytes; ++at) room[at] = patternByte(at);
  for (std::size_t at = 0; at < bytes; ++at) {
    if (room[at] != patternByte(at)) return false;
  }
  return true;
}

Room takeStepRoom(interfaces::Scratch& scratch, console::HeapReport* heap, RoomFigures* figures) {
  RoomFigures measured;
  measured.bytes = kStepBytes;
  if (heap != nullptr) {
    const console::HeapReport::Figures f = heap->read();
    measured.known = true;
    measured.free_bytes = f.free_bytes;
    measured.block = f.largest_block;
  }
  Room room;
  // The big room first, which is what brings the PSRAM up at all. A board with none, a board with
  // the other kind of chip and a board whose chip does not answer all come back nullptr here and
  // go on to the small room below, which is the whole point of the arrangement.
  room.data = static_cast<char*>(scratch.takeExternal(kStepBytesExternal));
  if (room.data != nullptr) {
    // **Before the first line goes in.** A room that does not answer its own pattern is given
    // back rather than filled with measurements nobody could trust.
    if (patternHolds(room.data, kStepBytesExternal)) {
      room.bytes = kStepBytesExternal;
      room.external = true;
      const interfaces::Scratch::ExternalRoom outside = scratch.external();
      measured.bytes = static_cast<std::uint32_t>(kStepBytesExternal);
      measured.external = static_cast<std::uint32_t>(outside.bytes);
    } else {
      scratch.give(room.data);
      room.data = nullptr;
    }
  }
  if (room.data == nullptr) {
    room.data = static_cast<char*>(scratch.take(kStepBytes));
    room.bytes = room.data != nullptr ? kStepBytes : 0;
    measured.bytes = kStepBytes;
    measured.external = 0;
  }
  if (room.data == nullptr) {
    // Nothing to be had: what the internal heap does have in one piece is the one thing worth
    // saying, and it is what the refusal's line prints.
    const std::size_t largest = scratch.largestBlock();
    measured.left_block = largest > 0xFFFFFFFFu ? 0u : static_cast<std::uint32_t>(largest);
  } else if (heap != nullptr && measured.known) {
    const console::HeapReport::Figures f = heap->read();
    measured.left_block = f.largest_block;
    // The least free **so far**, so that a caller which takes a room and gives it straight back
    // (`heap room`) has the figure. A session reads it again when its steps are over, which is
    // the moment that number is worth having, and writes over this one.
    measured.least_free = f.min_free;
  }
  if (figures != nullptr) *figures = measured;
  return room;
}

void giveStepRoom(interfaces::Scratch& scratch, Room* room) {
  if (room == nullptr || room->data == nullptr) return;
  scratch.give(room->data);
  *room = Room{};
}

// ------------------------------------------------------------------------------- the journal entry

std::size_t encode(const SessionPayload& p, std::uint8_t out[kPayloadMax]) {
  for (std::size_t i = 0; i < kPayloadMax; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(p.stage);
  out[1] = p.step;
  out[2] = p.slot;
  out[3] = p.flags;
  put32(out + 4, p.at_epoch);
  if (p.stage == Stage::kStep) {
    if ((p.flags & kSessionFlagLost) != 0) {
      // **Task p18-03's entry carries the count and no command**: a slot's 48 bytes are eight of
      // header and forty of command exactly, with nothing left over - and the command is in the
      // entry that *opened* this step, two lines back, where a reader of the journal has it. What
      // this entry adds is which step (the header's own `step`), how it ended (the flags' bits
      // 4..6, where this entry's header has always said a `StepEnd` would be) and how many times
      // the port lost bytes while it ran.
      out[3] = static_cast<std::uint8_t>((out[3] & ~kSessionEndMask) |
                                         ((static_cast<std::uint8_t>(p.end) << kSessionEndShift) &
                                          kSessionEndMask));
      out[kSessionHeaderBytes + 0] = static_cast<std::uint8_t>(p.losses & 0xFF);
      out[kSessionHeaderBytes + 1] = static_cast<std::uint8_t>((p.losses >> 8) & 0xFF);
      return kSessionHeaderBytes + kSessionLostBytes;
    }
    const std::size_t n = textLength(p.command);
    const std::size_t take = n < kCommandMax ? n : kCommandMax;
    for (std::size_t i = 0; i < take; ++i) out[kSessionHeaderBytes + i] = static_cast<std::uint8_t>(p.command[i]);
    return kSessionHeaderBytes + take;
  }
  if (p.stage == Stage::kRoom) {
    put32(out + kSessionHeaderBytes + 0, p.room.bytes);
    put32(out + kSessionHeaderBytes + 4, p.room.free_bytes);
    put32(out + kSessionHeaderBytes + 8, p.room.block);
    put32(out + kSessionHeaderBytes + 12, p.room.left_block);
    put32(out + kSessionHeaderBytes + 16, p.room.least_free);
    put32(out + kSessionHeaderBytes + 20, p.room.external);
    return kSessionHeaderBytes + kRoomFigureBytes;
  }
  return kSessionHeaderBytes;
}

bool decode(const Entry& entry, SessionPayload* out) {
  if (entry.kind != kind::kScannerSession || entry.size < kSessionHeaderBytes || out == nullptr) {
    return false;
  }
  if (entry.payload[0] > static_cast<std::uint8_t>(Stage::kRefused)) return false;
  *out = SessionPayload{};
  out->stage = static_cast<Stage>(entry.payload[0]);
  out->step = entry.payload[1];
  out->slot = entry.payload[2];
  out->flags = entry.payload[3];
  out->at_epoch = get32(entry.payload + 4);
  const std::size_t body = entry.size - kSessionHeaderBytes;
  if (out->stage == Stage::kStep && (out->flags & kSessionFlagLost) != 0 &&
      body >= kSessionLostBytes) {
    // Task p18-03's entry: two bytes of count and no command (see `encode`). The step's end is in
    // the flags.
    out->losses = static_cast<std::uint16_t>(
        entry.payload[kSessionHeaderBytes] |
        (static_cast<std::uint16_t>(entry.payload[kSessionHeaderBytes + 1]) << 8));
    out->end = static_cast<StepEnd>((out->flags & kSessionEndMask) >> kSessionEndShift);
  } else if (out->stage == Stage::kStep) {
    const std::size_t take = body < kCommandMax ? body : kCommandMax;
    for (std::size_t i = 0; i < take; ++i) {
      out->command[i] = static_cast<char>(entry.payload[kSessionHeaderBytes + i]);
    }
    out->command[take] = 0;
  } else if (out->stage == Stage::kRoom && body >= kRoomFigureBytesBefore05) {
    out->room.known = true;
    out->room.bytes = get32(entry.payload + kSessionHeaderBytes + 0);
    out->room.free_bytes = get32(entry.payload + kSessionHeaderBytes + 4);
    out->room.block = get32(entry.payload + kSessionHeaderBytes + 8);
    out->room.left_block = get32(entry.payload + kSessionHeaderBytes + 12);
    out->room.least_free = get32(entry.payload + kSessionHeaderBytes + 16);
    // The sixth figure is task p16-05's. An entry a firmware before it wrote has twenty bytes of
    // body and no PSRAM behind it at all, which is what a nought here says.
    if (body >= kRoomFigureBytes) {
      out->room.external = get32(entry.payload + kSessionHeaderBytes + 20);
    }
    // A build with no heap seam wrote zeroes: the line then says so rather than claiming a heap
    // of nothing.
    if (out->room.free_bytes == 0 && out->room.block == 0 && out->room.least_free == 0) {
      out->room.known = false;
    }
  }
  return true;
}

bool readMark(const Journal& journal, Mark* out) {
  if (out == nullptr) return false;
  *out = Mark{};
  Entry e;
  SessionPayload p;
  if (!journal.lastOf(kind::kScannerSession, &e) || !decode(e, &p)) return false;
  out->found = true;
  out->open = isOpen(p.stage);
  out->stage = p.stage;
  out->slot = board::Slot{p.slot};
  out->at_epoch = p.at_epoch;
  out->step = p.step;
  return true;
}

// ------------------------------------------------------------------------------- the object

ScannerSession::ScannerSession(RadioUpdate& update, link::client::Client& radio,
                               image::Store& store, Journal& journal, interfaces::Clock& clock,
                               interfaces::WallClock& wall, BatterySource& battery,
                               interfaces::Scratch& scratch, console::HeapReport* heap)
    : update_(update),
      radio_(radio),
      store_(store),
      journal_(journal),
      clock_(clock),
      wall_(wall),
      battery_(battery),
      scratch_(scratch),
      heap_(heap) {}

ScannerSession::~ScannerSession() {
  // The client keeps a pointer to `lent_`, which is a member of this object: a session that is
  // destroyed mid-step takes its room back before the member goes.
  if (lending_) {
    radio_.takeText();
    lending_ = false;
  }
  giveRoom();
}

void ScannerSession::clear() {
  if (active()) return;
  if (lending_) {
    radio_.takeText();
    lending_ = false;
  }
  giveRoom();
  stage_ = Stage::kIdle;
  status_ = SessionStatus{};
  program_ = nullptr;
  sink_ = nullptr;
  first_seq_ = 0;
  back_started_ = false;
  back_stage_ = Stage::kIdle;
  step_over_ = false;
  said_ = false;
  losses_at_say_ = 0;
  stuck_ = false;
  scanner_at_ms_ = 0;
  step_until_ms_ = 0;
  lent_ = lc::TextRoom{};
}

// The entry before the action (S-5, §6: "Every step is journalled ... before it is taken").
bool ScannerSession::journal(Stage stage, const char* command, bool lost, StepEnd end,
                            std::uint32_t losses) {
  SessionPayload p;
  p.stage = stage;
  p.step = stage == Stage::kStep ? static_cast<std::uint8_t>(status_.step) : 0;
  p.slot = status_.slot.index;
  p.flags = static_cast<std::uint8_t>(status_.resumed ? kSessionFlagResumed : 0);
  if (lost) {
    p.flags = static_cast<std::uint8_t>(p.flags | kSessionFlagLost);
    p.end = end;
    p.losses = static_cast<std::uint16_t>(losses > 0xFFFFu ? 0xFFFFu : losses);
  }
  p.at_epoch = status_.at_epoch;
  if (command != nullptr) {
    std::size_t i = 0;
    for (; command[i] != 0 && i < kCommandMax; ++i) p.command[i] = command[i];
    p.command[i] = 0;
  }
  p.room = status_.room;
  std::uint8_t bytes[kPayloadMax];
  const std::size_t size = encode(p, bytes);
  std::uint32_t seq = 0;
  if (journal_.append(kind::kScannerSession, bytes, size, &seq) != JournalError::kNone) return false;
  if (first_seq_ == 0) {
    first_seq_ = seq;
    (void)journal_.protect(seq);
  }
  return true;
}

SessionStep ScannerSession::enter(Stage next, const char* note, const char* command) {
  SessionStep r;
  r.before = stage_;
  r.after = stage_;
  r.note = note;
  if (!journal(next, command)) {
    // Without the journal nothing is done: the machine stays where it is and says so, and a
    // later resume() finds the last entry it did write. The one exception is the entry that
    // opens a session, which start() refuses outright - there is nothing to resume from.
    r.note = describe(Refusal::kJournal);
    r.stalled = true;
    return r;
  }
  stage_ = next;
  status_.stage = next;
  if (next == Stage::kRestored) status_.ending = Ending::kRestored;
  // Task p18-05: the scanner's flash never began, so nothing was touched - the one ending besides
  // `restored` that closes a session.
  if (next == Stage::kRefused) status_.ending = Ending::kRefused;
  if (next == Stage::kStuck) {
    status_.ending = Ending::kStuck;
    stuck_ = true;
  }
  if (!isOpen(next)) journal_.unprotect();
  r.after = next;
  r.changed = true;
  return r;
}

SessionStep ScannerSession::waiting(const char* note) {
  SessionStep r;
  r.before = stage_;
  r.after = stage_;
  r.waited = true;
  r.note = note;
  return r;
}

// ------------------------------------------------------------------------------- the check

// Everything the switch to the scanner checks **and everything the switch back will**: rail 4,
// *not begun unless it can be ended*. Each answer is one word (deliverable 7), and nothing here
// touches the radio but the one HELLO that asks whether it is there.
Refusal ScannerSession::check(board::Slot slot) {
  // S-7 first, as the update machine's own start() does: a run journalled unfinished is resumed
  // and never started over, and this firmware's own update in the air means never both.
  Entry e;
  Payload radio_entry;
  if (journal_.lastOf(kind::kRadioUpdate, &e) && decode(e, &radio_entry) &&
      !isTerminal(radio_entry.state)) {
    return Refusal::kInProgress;
  }
  if (journal_.lastOf(kind::kSelfUpdate, &e) && e.size >= 1 &&
      self_update::unconfirmed(e.payload[0])) {
    return Refusal::kInProgress;
  }
  if (update_.active()) return Refusal::kInProgress;
  // An open session is ended, not begun again.
  Mark mark;
  if (readMark(journal_, &mark) && mark.open) return Refusal::kInProgress;

  if (program_ == nullptr) return Refusal::kProgram;
  const Refusal program = judge(*program_);
  if (program != Refusal::kNone) return program;

  const flasher::BoardProfile* profile = flasher::profileForSlot(slot);
  if (profile == nullptr) return Refusal::kNoScanner;

  // The scanner image: stored, verifying, not on the bad list, a scanner, this board's, this slot.
  image::Record scanner;
  if (store_.read(image::Which::kScanner, slot, &scanner) != image::StoreError::kNone ||
      !scanner.present) {
    return Refusal::kNoScanner;
  }
  if (store_.verify(image::Which::kScanner, slot) != image::StoreError::kNone) return Refusal::kNoScanner;
  if (store_.isBad(scanner.sha256)) return Refusal::kNoScanner;
  if (scanner.descriptor.role != image::Role::kScanner) return Refusal::kNoScanner;
  if (!sameText(scanner.descriptor.board, profile->name)) return Refusal::kNoScanner;
  if (scanner.descriptor.slot != image::kAnySlot && scanner.descriptor.slot != slot.index) {
    return Refusal::kNoScanner;
  }

  // The golden image: there, and verifying. It is what the session comes back to, so this is the
  // check the switch back would make - made now, before the scanner goes on.
  image::Record golden;
  if (store_.read(image::Which::kGolden, slot, &golden) != image::StoreError::kNone ||
      !golden.present) {
    return Refusal::kNoGolden;
  }
  if (store_.verify(image::Which::kGolden, slot) != image::StoreError::kNone) return Refusal::kNoGolden;
  // The band this slot is known to be is the golden image's (the update machine's own rule).
  if (!sameText(scanner.descriptor.band, golden.descriptor.band)) return Refusal::kNoScanner;

  // This board's clock: the greeting gives the scanner an epoch, and a scanner whose lines are
  // stamped `ts: 0` is a session nobody can line up afterwards.
  interfaces::WallTime now;
  if (!wall_.now(&now) || now.epoch == 0) return Refusal::kClock;

  // The link: the radio has to be there to be turned into a scanner. There is no classified
  // silence to be passed through here - a session is a person's or a server's choice, never the
  // one flash that exists to unbrick a radio.
  const lc::Answer<link::HelloResponse> hello = radio_.hello();
  if (!hello.ok()) return Refusal::kLink;

  // The battery's two figures, by the update machine's own rule and threshold. Unknown is a
  // refusal: a session is two flashes and twenty minutes with both MCUs awake.
  BatteryReading battery;
  if (!battery_.read(&battery)) return Refusal::kBattery;
  if (battery.millivolts < kBatteryMinMv) return Refusal::kBattery;
  if (battery.trend_known && battery.mv_per_hour <= kBatteryFallingMvPerHour) return Refusal::kBattery;

  return Refusal::kNone;
}

void ScannerSession::readFiguresAfter() {
  if (heap_ == nullptr || !status_.room.known) return;
  const console::HeapReport::Figures f = heap_->read();
  status_.room.least_free = f.min_free;
}

// The room, and its figures, through the one function the console's `heap room` also goes
// through: 64 kB of the module's PSRAM where there is PSRAM that answers its pattern, and 8 192
// bytes of the internal heap where there is not (task p16-05).
bool ScannerSession::takeRoom() {
  if (room_.data != nullptr) return true;
  room_ = takeStepRoom(scratch_, heap_, &status_.room);
  return room_.data != nullptr;
}

void ScannerSession::giveRoom() { giveStepRoom(scratch_, &room_); }

// ------------------------------------------------------------------------------- the two doors

bool ScannerSession::start(const Program& program, board::Slot slot, SessionSink* sink) {
  if (active()) {
    status_.refusal = Refusal::kInProgress;
    return false;
  }
  clear();
  program_ = &program;
  sink_ = sink;
  status_.slot = slot;
  status_.steps = program.count;
  const Refusal why = check(slot);
  if (why != Refusal::kNone) {
    program_ = nullptr;
    sink_ = nullptr;
    status_.refusal = why;
    status_.ending = Ending::kRefused;
    return false;
  }
  // The room, before anything is journalled and long before anything is flashed: a session that
  // cannot hold what a step says is refused rather than begun. This is also the one moment of a
  // wake that brings the PSRAM up - a wake with no session never touches it (task p16-05).
  status_.room = RoomFigures{};
  if (!takeRoom()) {
    program_ = nullptr;
    sink_ = nullptr;
    status_.refusal = Refusal::kNoMemory;
    status_.ending = Ending::kRefused;
    return false;
  }
  interfaces::WallTime now;
  if (wall_.now(&now)) status_.at_epoch = now.epoch;
  // **Open, before the first flash** (rail 1). Nothing after this moment may leave the radio a
  // scanner without something taking the session up again.
  const SessionStep opened = enter(Stage::kCheck, "the preconditions hold: a session is open");
  if (!opened.changed) {
    giveRoom();
    program_ = nullptr;
    sink_ = nullptr;
    status_.refusal = Refusal::kJournal;
    status_.ending = Ending::kRefused;
    return false;
  }
  return true;
}

bool ScannerSession::resume() {
  if (active()) return true;
  Mark mark;
  if (!readMark(journal_, &mark) || !mark.open) return false;
  clear();
  status_.resumed = true;
  status_.slot = mark.slot;
  status_.at_epoch = mark.at_epoch;
  status_.step = mark.step;
  status_.stage = mark.stage;
  stage_ = mark.stage;
  // A resumed session has no program, no sink and no room: what is left of it is the switch back.
  // The ceiling is counted from now so that a wake that takes one up is bounded like any other.
  scanner_at_ms_ = clock_.nowMs();
  back_started_ = update_.active();
  back_stage_ = Stage::kIdle;
  return true;
}

// ------------------------------------------------------------------------------- one step

SessionStep ScannerSession::step() {
  // **One wrapper, so that every answer says it** (task p18-03): whoever drives this machine reads
  // `listening` off the answer it holds and makes no call to a server while it is true, and there
  // are a dozen places below that answer.
  SessionStep r = stepOnce();
  r.listening = listening();
  return r;
}

std::uint32_t ScannerSession::stepPending() const {
  if (stage_ != Stage::kStep || said_ || step_over_) return 0;
  const ProgramStep* s = current();
  return s != nullptr ? s->seconds : 0;
}

SessionStep ScannerSession::stepOnce() {
  if (scanner_at_ms_ != 0) {
    const std::uint64_t now = clock_.nowMs();
    status_.scanner_ms = now > scanner_at_ms_ ? static_cast<std::uint32_t>(now - scanner_at_ms_) : 0;
  }
  // One line a try (rail 5): once this run has said `stuck` there is nothing left for it to do.
  if (stuck_) return waiting(describe(Stage::kStuck));
  // **A session taken up out of the journal has no program, no sink and no room** (rail 2): what
  // is left of it is the switch back, from whatever stage it was journalled in - a step that was
  // running, a scanner image half flashed, a `check` that never got as far as the flasher.
  if (status_.resumed && isOpen(stage_)) return stepBack();
  switch (stage_) {
    case Stage::kIdle:
    case Stage::kRestored:
    case Stage::kRefused:
      return waiting(describe(stage_));
    case Stage::kCheck: {
      // The scanner image. The journal says a session is open already, so a reset from here on is
      // a reset the next wake ends.
      Request request;
      request.kind = Kind::kRoleScanner;
      request.slot = status_.slot;
      if (!update_.start(request)) {
        // The machine would not start - a race with something that claimed it between the check
        // and here. The radio is untouched; the session is still open and the switch back is what
        // closes it.
        return goBack();
      }
      return enter(Stage::kFlashingScanner, "the scanner image is going on");
    }
    case Stage::kFlashingScanner: {
      if (update_.active()) {
        if (isRollingBack(update_.state())) {
          // The scanner never showed up and the machine is putting the golden image back by
          // itself. That **is** the switch back: it is picked up, not started again - and the
          // room goes back here as it does on every other way out of the steps, because no step
          // is ever going to run.
          back_started_ = true;
          return goBack();
        }
        const StepResult s = update_.step();
        SessionStep r = waiting(s.note);
        r.waited = s.waited;
        // Task p18-05: the one line of the update machine's story a session's caller has to hear -
        // the flash did not begin and is being made once more. Passed through without a stage of
        // this session's own, because the update machine has journalled it already.
        if (s.reason == Reason::kFlashOnceMore) {
          r.said = s.reason;
          r.detail = s.detail;
        }
        return r;
      }
      // **The scanner image's flash never began** (task p18-05, deliverable 4): twice, with
      // nothing written either time, so the radio is the repeater it was, the mesh never lost it
      // and there is nothing to switch back from. The session closes `refused` - no flash of any
      // kind, no outage, and the scanner image on no bad list - and the room goes back here as it
      // does on every other way out of the steps. No `room` entry: nothing was measured.
      if (update_.state() == State::kRefused && update_.reason() == Reason::kFlashDidNotBegin) {
        giveRoom();
        return enter(Stage::kRefused, describe(update_.reason()));
      }
      if (update_.state() == State::kDone) {
        scanner_at_ms_ = clock_.nowMs();
        status_.scanner_ms = 0;
        return enter(Stage::kScanner, "a scanner is there");
      }
      // Restored, stuck, refused, a node fault: the switch back is what this session owes,
      // whatever the scanner run made of itself, and `goldenIsBack()` is what tells a golden
      // image already proved from one that has to be flashed.
      return goBack();
    }
    case Stage::kScanner: return stepScanner();
    // Three things a step is, in this order: its entry journalled and nothing said (beginStep),
    // its command said (sayStep), its lines listened to (stepRunning). The middle one is task
    // p18-03's: the caller's *step N* reaches the server between the first and the second.
    case Stage::kStep:
      if (step_over_) return beginStep();
      return said_ ? stepRunning() : sayStep();
    case Stage::kRoom: return stepBack();
    case Stage::kFlashingGolden:
    case Stage::kBooting:
    case Stage::kProving:
    case Stage::kStuck: return stepBack();
  }
  return waiting("?");
}

// The greeting, said once a scanner is found: `time <epoch>`, `auto 0`, `info`. The room is lent
// **before** it, so the three `ack`s and the `info` line are laid down in it and are handed over
// with the first step's lines - they are that step's, and the session's lines begin with them
// (deliverable 2).
SessionStep ScannerSession::stepScanner() {
  if (!status_.greeted) {
    if (!lending_) {
      lent_ = lc::TextRoom{};
      lending_ = radio_.lendText(&lent_, room_.data, room_.bytes);
    }
    char line[24];
    // The epoch the check insisted on. `time` is answered with an `ack` like every other command
    // and the scanner writes it to its RV-3028 where one is fitted (noisescope.md §5).
    const int n = std::snprintf(line, sizeof line, "time %lu",
                                static_cast<unsigned long>(status_.at_epoch));
    if (n > 0 && static_cast<std::size_t>(n) < sizeof line) {
      (void)radio_.say(link::text(line, static_cast<std::size_t>(n)));
    }
    // `auto 0`: no unattended sweep of its own in the middle of a program whose steps are timed.
    (void)radio_.say(link::text("auto 0"));
    // `info`: what it is, which is the one line of a session worth keeping whatever the steps say.
    (void)radio_.say(link::text("info"));
    status_.greeted = true;
    return waiting("the greeting was said: time, auto 0, info");
  }
  return beginStep();
}

const ProgramStep* ScannerSession::current() const {
  if (program_ == nullptr || status_.step == 0 || status_.step > program_->count) return nullptr;
  return &program_->step[status_.step - 1];
}

bool ScannerSession::pastCeiling() const {
  if (scanner_at_ms_ == 0) return false;
  return status_.scanner_ms >= kSessionCeilingMs;
}

SessionStep ScannerSession::beginStep() {
  const std::size_t next = status_.step + 1;
  if (program_ == nullptr || next > program_->count) return goBack();
  // **Rail 3**: the ceiling is read again before every step, so a caller that sits on a step's
  // lines - a job posting them over LTE-M - cannot hold the radio off the mesh past it.
  if (pastCeiling()) return goBack();
  status_.step = next;
  step_over_ = false;
  const ProgramStep* s = current();
  if (s == nullptr) return goBack();
  // What the scanner said between steps is thrown away: the room is taken back and lent again
  // empty, so that the lines handed over are this step's own. The first step keeps the greeting.
  if (next > 1) {
    if (lending_) {
      radio_.takeText();
      lending_ = false;
    }
    lent_ = lc::TextRoom{};
  }
  if (!lending_) lending_ = radio_.lendText(&lent_, room_.data, room_.bytes);
  said_ = false;
  const SessionStep r = enter(Stage::kStep, "the step's command", s->command);
  if (!r.changed) return r;  // the journal: nothing is said
  // **And the command is said by the next call, not this one** (task p18-03). The entry is in the
  // flash and the stage is `kStep`, so a caller that talks to a server has this moment - and only
  // this moment - to say *step N* and its command and to see to its lease, with the scanner still
  // silent and the step's seconds not begun. On 21 November that call fell the other side of the
  // command and took nine lines with it.
  return r;
}

// The command of the step whose entry is already journalled. The step's seconds begin here, which
// is where they belong: a caller that spent a second telling a server has not spent a second of
// the step.
SessionStep ScannerSession::sayStep() {
  const ProgramStep* s = current();
  if (s == nullptr) return goBack();
  said_ = true;
  losses_at_say_ = radio_.counters().port_losses;
  step_until_ms_ = clock_.nowMs() + static_cast<std::uint64_t>(s->seconds) * 1000u;
  const lc::Said said = radio_.say(link::text(s->command));
  if (!said.answered()) {
    endStep(StepEnd::kSilent);
    return waiting(name(StepEnd::kSilent));
  }
  if (!said.ok) {
    endStep(StepEnd::kRefused);
    return waiting(name(StepEnd::kRefused));
  }
  return waiting("the step's command was said");
}

SessionStep ScannerSession::stepRunning() {
  const ProgramStep* s = current();
  if (s == nullptr) return goBack();
  // Whatever arrived: the client lays every line beginning with `{` down in the room whole.
  radio_.poll();
  const link::Text first = lc::firstWord(link::text(s->command));
  if (lent_.hasDone(first)) {
    endStep(StepEnd::kDone);
    return waiting(name(StepEnd::kDone));
  }
  if (lent_.full) {
    sayStop();
    endStep(StepEnd::kFull);
    return waiting(name(StepEnd::kFull));
  }
  // Its seconds, or the ceiling - which ends a step as its own clock does, because what the
  // ceiling is for is exactly that nothing may hold the radio past it.
  if (clock_.nowMs() >= step_until_ms_ || pastCeiling()) {
    sayStop();
    endStep(StepEnd::kClock);
    return waiting(name(StepEnd::kClock));
  }
  return waiting("the step's lines");
}

// `stop`, with the room already taken back so that its `ack` is not counted among the step's
// lines. Whatever it answers: the step has ended either way.
void ScannerSession::sayStop() {
  if (lending_) {
    radio_.takeText();
    lending_ = false;
  }
  (void)radio_.say(link::text("stop"));
}

void ScannerSession::endStep(StepEnd end) {
  if (lending_) {
    radio_.takeText();
    lending_ = false;
  }
  step_over_ = true;
  const std::size_t i = status_.step > 0 ? status_.step - 1 : 0;
  if (i < kStepsMax) status_.step_end[i] = end;
  // **What the port lost while this step ran** (task p18-03), as the difference of the client's
  // own count: the ring was full, or the hardware's FIFO ran over, and lines the scanner numbered
  // are simply not here. Nine of sixteen went that way on 21 November and nothing anywhere said
  // so, which is what this entry is for - the one journal line of a session that says a
  // measurement is incomplete. A step that lost nothing writes no entry, so an ordinary session's
  // journal is exactly what it always was.
  const std::uint32_t now_losses = radio_.counters().port_losses;
  const std::uint32_t losses = now_losses > losses_at_say_ ? now_losses - losses_at_say_ : 0;
  if (losses != 0) (void)journal(Stage::kStep, nullptr, true, end, losses);
  // Handed to whoever runs the session, as the room itself (S-12: nothing of it is copied into a
  // log, the journal or a report). The next step begins when this returns.
  if (sink_ != nullptr) {
    StepReport report;
    report.index = status_.step;
    const ProgramStep* s = current();
    report.command = s != nullptr ? s->command : "";
    report.seconds = s != nullptr ? s->seconds : 0;
    report.end = end;
    report.lines = room_.data;
    report.used = lent_.used;
    report.count = lent_.lines;
    report.first = status_.step == 1;
    report.losses = losses;
    sink_->onStepLines(report);
  }
}

// ------------------------------------------------------------------------------- the switch back

// The room's figures, the room given back, and then the golden image. The `room` entry is written
// **before the switch back** (deliverable 4) and only when there was a room to measure: a session
// taken up out of the journal never had one.
SessionStep ScannerSession::goBack() {
  if (lending_) {
    radio_.takeText();
    lending_ = false;
  }
  if (room_.data != nullptr) {
    readFiguresAfter();
    const SessionStep r = enter(Stage::kRoom, "what the room cost");
    giveRoom();
    if (r.changed) return r;
  }
  return stepBack();
}

// The golden image is back and proved already. Read off the machine in hand when there is one -
// this wake's step 3 resumes the radio's own update before it takes a session up - and off the
// journal when there is not, which is the boot whose power went between the switch back's `done`
// and the session's own `restored`.
bool ScannerSession::goldenIsBack() const {
  if (update_.state() == State::kRestored) return true;
  if (update_.status().kind == Kind::kRoleRepeater && update_.state() == State::kDone) return true;
  if (update_.state() != State::kIdle) return false;
  Entry e;
  Payload p;
  if (!journal_.lastOf(kind::kRadioUpdate, &e) || !decode(e, &p)) return false;
  if (p.state == State::kRestored) return true;
  return p.kind == Kind::kRoleRepeater && p.state == State::kDone;
}

SessionStep ScannerSession::stepBack() {
  if (!back_started_) {
    if (update_.active()) {
      // Something of the machine is still in the air - the scanner run's own rollback, or a run
      // this wake's step 3 resumed - and that **is** the switch back: it is picked up, not
      // started again.
      back_started_ = true;
    } else if (goldenIsBack()) {
      return enter(Stage::kRestored, "the repeater was already back and proved");
    } else {
      // **The battery, judged here and not by the machine below.** §9: a scanner answers no
      // frame, so POWER_GET cannot be sent while the radio is one and the kept reading
      // (core/update/last_good_battery.h) is all there is - and after a reset there is not even
      // that, because it is RAM. The machine's own check reads *no reading at all* as a refusal
      // (`kBatteryUnknown`), which on this path would mean a site whose repeater nothing would
      // ever put back: rail 2's whole point is that the switch back needs no server, no uplink
      // and no console, and a gauge this Walter cannot reach is none of those.
      //
      // So: a reading that says **too low, or falling fast** refuses the switch back, which is
      // what leaves the session open for the next wake to try again (rail 5, and the task's own
      // acceptance line). A reading that cannot be had does not, and the machine is told the
      // link is down in the one sense it means - which is what `supervisor::radioKnownDown`
      // already says of a scanner.
      BatteryReading reading;
      if (battery_.read(&reading) &&
          (reading.millivolts < kBatteryMinMv ||
           (reading.trend_known && reading.mv_per_hour <= kBatteryFallingMvPerHour))) {
        ++status_.tries;
        return enter(Stage::kStuck, describe(Refusal::kBattery));
      }
      Request request;
      request.kind = Kind::kRoleRepeater;
      request.slot = status_.slot;
      request.link_known_down = radio_.mode() == lc::Mode::kText;
      if (!update_.start(request)) {
        // **Rail 5**: a switch back that is refused leaves the session open, and every wake tries
        // again. One journal line a try - this one.
        ++status_.tries;
        return enter(Stage::kStuck, describe(update_.reason()));
      }
      back_started_ = true;
      // No entry yet: the machine is at its own `check` and has touched nothing, so a switch back
      // refused there writes **one** line and that line is `stuck`. `flashing-golden` is written
      // below, when the machine really is about to flash.
      return waiting("the golden image's preconditions");
    }
  }
  const Stage want = backStageOf(update_.state());
  if (want != Stage::kIdle && want != back_stage_) {
    back_stage_ = want;
    return enter(want, describe(want));
  }
  if (update_.active()) {
    const StepResult s = update_.step();
    SessionStep r = waiting(s.note);
    r.waited = s.waited;
    if (s.reason == Reason::kFlashOnceMore) {  // task p18-05, as above
      r.said = s.reason;
      r.detail = s.detail;
    }
    return r;
  }
  if (update_.state() == State::kDone || update_.state() == State::kRestored) {
    return enter(Stage::kRestored, "the repeater is back and proved");
  }
  ++status_.tries;
  return enter(Stage::kStuck, describe(update_.reason()));
}

}  // namespace update
}  // namespace core
