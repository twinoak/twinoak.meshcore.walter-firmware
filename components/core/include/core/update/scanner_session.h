// A session: the scanner image, a program of steps, and the golden image back (task p16-03).
// spec/SYSTEM.md S-5, S-7 and S-12 and §6; spec/LINK-PROTOCOL.md §9; docs/update.md's *A
// session*, docs/cycle.md, docs/supervisor.md.
//
// `rak role scanner` makes the radio a scanner and it stays one until a person types `rak role
// repeater`. On a bench that is a person with a cable; on a mast it is nobody, and a board that
// restarts between the two - or a server that stops answering - leaves a site with no repeater.
//
// So the two switches are **one thing**, with a program between them and rails that end it
// whatever happens in the middle:
//
//   check ─► flashing-scanner ─► scanner ─► step-1 … step-6 ─► room ─► flashing-golden
//                     │                          │                        │
//                     └──────────────────────────┴───► (the ceiling) ──────┤
//                                                                          ▼
//                                              booting ─► proving ─► restored
//                                                                 └─► stuck ──┐
//                                                                             │
//    (the journal still says open: the next wake takes it up and tries again) ─┘
//
// **The repeater coming back depends on nothing but this board, its store and its journal.** No
// server, no uplink, no console: the journal says a session is open before the first flash and
// says closed only when the golden image is back and proved, and step 3 of every wake takes up an
// open session nobody is running (`core/cycle/wake_cycle.h`).
//
// -- what this is, and what it is not -----------------------------------------------------------
//
// It is a state machine over the radio's update machine (`core/update/radio_update.h`, whose two
// role kinds are **unchanged**), the link client's §9 half (`say()` and the lent room, task
// p16-01), the journal, a clock, the battery and one room seam. One step per call, so that a
// caller - the console's `rak session`, task p16-04's job - can drive it in slices and every
// transition is testable on the host (host_test/core/update/test_scanner_session*.cpp). It is
// logic: no ESP-IDF include, and the only memory it ever asks for is the one room below, through
// `interfaces::Scratch`.
//
// It begins for nobody but a person or a job: `start()` has two callers and rail R20 holds it
// there. This firmware never opens one by itself; what it does by itself is **end** one.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/console/heap_report.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/update/battery.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "interfaces/clock.h"
#include "interfaces/scratch.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace update {

// -- the program's rules, held here whoever gives the program -----------------------------------

// A step is a command and its seconds, and a program is one to six of them. The bounds are the
// task's and are held in one place (`judge()` below), so that a person at a console and a payload
// from the server are judged by the same lines.
constexpr std::size_t kStepsMax = 6;
constexpr std::size_t kCommandMax = 40;
constexpr std::uint32_t kStepSecondsMin = 5;
constexpr std::uint32_t kStepSecondsMax = 300;
constexpr std::uint32_t kProgramSecondsMax = 600;

// The first word a command may begin with: the six of NoiseScope's own table that measure
// something (the repeater repository's `docs/noisescope.md` §5). Not `stop` - the session says
// that itself - and not `reboot`, `dfu`, `hang`, `home`, `boost`, `auto` or `time`, which change
// what the scanner is or what it does to itself and are a person's at a cable.
constexpr std::size_t kVerbCount = 6;
extern const char* const kVerbs[kVerbCount];

// -- the room a step's lines are laid down in ---------------------------------------------------
//
// One buffer, taken from `interfaces::Scratch` when the session begins and given back when the
// steps are over - no static storage of that size, and nothing at all when no session is running.
//
// **Why 8 192 and no more.** The unit in service has said what it has: its report's `restarts`
// section, in November, *heap: 27472 bytes free, largest free block 12288, least free since this
// boot 25300 (internal, 8-bit)*. A room of 16 kB is not there to be had, and one of 12 kB would
// leave nothing whole for anybody else. A `sweep` line of the band is about 4 kB and the longest
// line the scanner can send about 7.6 kB, so one of those fits whole with room beside it.
//
// It is **measured at every session** (the `room` entry below), so that the figure can be raised
// later from what units say rather than from what this comment guesses.
constexpr std::size_t kStepBytes = 8192;

// **And 65 536 bytes where this board has external RAM** (task p16-05). The module has 2 MB of
// PSRAM, which this firmware leaves switched off until a session asks for a room: 64 kB of it is
// the room then, and the 8 192 bytes above are what a board with no PSRAM, with PSRAM of the
// other kind, or with one that does not answer its pattern still gets.
//
// **Why 64 kB and no more**, with 2 MB to choose from: what bounds the room is what leaves the
// board. Six steps of it are 384 kB, inside the 512 kB meshmaster.dk keeps of one session, and
// every 8 kB of it is a post over LTE-M (task p16-04's `postSomeLines`). It is also what a
// minute of `scope` costs: a frame a second at 250 points is about 30 kB, so a minute of it fits
// whole with room beside it, where the small room ends `full` after about thirteen frames.
constexpr std::size_t kStepBytesExternal = 65536;

// -- the ceiling, and the bound on a wake that runs a session -----------------------------------

// Twenty minutes as a scanner at most, counted from the moment the scanner is found. Past it the
// session goes to the switch back, whatever step it is in and whatever its lines are waiting for.
// It is the one number that makes "a site with no repeater" bounded without a person: a program of
// six steps may ask for 600 s and a caller may sit on a step's lines for as long as it likes, and
// neither can hold the radio off the mesh longer than this.
constexpr std::uint32_t kSessionCeilingMs = 20u * 60 * 1000;

// What a wake that runs a session may be held open for: two passes of the radio's update machine
// (the scanner flashed and proved, the golden image flashed and proved - `kWholeRunMs`, 38 min
// 40 s by the unit's own journal) and the ceiling, and not a second more. `core/cycle/wake_guard.h`
// caps what a wake may ask for at `kRaisedBoundMax`, and this is the number that set it.
constexpr std::uint32_t kSessionBoundMs = kWholeRunMs + kSessionCeilingMs;

// How often the port is read while a step's lines are coming in. A tenth of a second, as the
// update machine's own idle step is: a `scope` frame a second and a `sweep` line every few
// seconds are nowhere near it, and a tighter loop would only spin.
constexpr std::uint32_t kListenIdleMs = 100;

// The most journal entries one session may write, for `protect()`: the twelve of a good run
// (check, flashing-scanner, scanner, six steps, room, flashing-golden, booting, proving, restored)
// with room for a switch back retried at a wake after. A sector holds 64.
constexpr std::uint32_t kEntriesPerSessionMax = 40;

// -- the stages, which are the journal's own words ----------------------------------------------

enum class Stage : std::uint8_t {
  kIdle = 0,
  kCheck = 1,            // the preconditions held and a session is **open**; nothing is flashed yet
  kFlashingScanner = 2,  // the scanner image, through the radio's update machine (kRoleScanner)
  kScanner = 3,          // a scanner is there; the greeting is said and the ceiling starts here
  kStep = 4,             // a step is running (`status().step` says which, 1..6)
  kRoom = 5,             // the figures of the room, written before the switch back
  kFlashingGolden = 6,   // the golden image (kRoleRepeater, or the scanner run's own rollback)
  kBooting = 7,          // the radio was reset: waiting for its HELLO
  kProving = 8,          // the golden image's health check, as an update's
  kRestored = 9,         // terminal, and the one stage that means **closed**
  kStuck = 10,           // the golden image did not come back: the session stays open
};

// One word, which is what the journal line says: `check`, `flashing-scanner`, `step`, …
const char* name(Stage stage);
// One phrase for a person.
const char* describe(Stage stage);
// Whether this stage, read back out of the journal, means a session is open and has to be ended.
// Everything but `idle` and `restored` - **`stuck` included**: a switch back that failed is a
// session the next wake tries again (rail 5).
constexpr bool isOpen(Stage stage) { return stage != Stage::kIdle && stage != Stage::kRestored; }

// -- why a session was refused: one word each, and the radio untouched --------------------------

enum class Refusal : std::uint8_t {
  kNone = 0,
  kBattery = 1,     // the battery could not be read, is below the figure, or is falling fast
  kLink = 2,        // the radio does not answer and nobody has said why
  kClock = 3,       // this board does not know what time it is: the greeting has no epoch
  kInProgress = 4,  // a radio update is journalled unfinished, or this firmware is on trial
  kNoGolden = 5,    // no golden image to come back to, or it does not verify
  kNoScanner = 6,   // no scanner image, one that does not verify, or one of another board or band
  kProgram = 7,     // the program breaks one of the rules above
  kNoMemory = 8,    // the heap has no kStepBytes block
  // The journal would not take the entry that opens a session. **Not one of the task's eight**
  // (see tasks/phase-16/reports/03.md, *Deviations*): the eight are preconditions a person or a
  // server can do something about, and a journal that will not take a byte is not one of them -
  // but a session that cannot be journalled as open must not begin, because the entry is the
  // only thing that gets the repeater back after a reset.
  kJournal = 9,
};

// The word, for the journal, the console and a job's result: `battery`, `link`, …
const char* word(Refusal refusal);
// One phrase for a person.
const char* describe(Refusal refusal);

// -- how a step ended, which is the only thing a step is judged by ------------------------------

enum class StepEnd : std::uint8_t {
  kNone = 0,
  kDone = 1,      // a `done` line for the command's first word
  kClock = 2,     // its seconds are up, and `stop` was said
  kFull = 3,      // the room is used up, and `stop` was said
  kRefused = 4,   // the `ack` said not ok
  kSilent = 5,    // no `ack` came
};

const char* name(StepEnd end);       // `done`, `clock`, `full`, `refused`, `silent`
const char* describe(StepEnd end);   // one phrase

// -- how the session ended, for the caller ------------------------------------------------------

enum class Ending : std::uint8_t {
  kRunning = 0,
  kRestored = 1,  // the golden image is back and proved, and the journal says closed
  kRefused = 2,   // a precondition; nothing was touched. `refusal()` says which
  kStuck = 3,     // the golden image did not come back. The session stays open
};

const char* name(Ending ending);
const char* describe(Ending ending);

// -- the program ---------------------------------------------------------------------------------

struct ProgramStep {
  char command[kCommandMax + 1] = {};
  std::uint32_t seconds = 0;
};

// **The program is the caller's.** The session keeps a pointer to it for the length of the run
// (as the link client keeps one to the lent room, task p16-01) and not a copy, because six
// commands are 252 bytes and every byte of `.bss` moves the heap the main task's stack comes out
// of (`tools/heap_at_start.py`, rail R22). It must outlive the session - which it does for both
// callers: a console command drives the session to its end inside the command, and a job drives it
// to its end inside the job.
struct Program {
  ProgramStep step[kStepsMax];
  std::size_t count = 0;
};

// The rules of deliverable 3, in one place: 1 to 6 steps; a command of 1 to 40 characters of
// `a`-`z`, `0`-`9`, space and the full stop whose first word is one of `kVerbs`; seconds 5 to 300
// and 600 at most over the program. `kNone` when the program is one this session will run, and
// `kProgram` otherwise - `whyProgram()` says which rule, as one phrase, for the console's line.
Refusal judge(const Program& program);
const char* whyProgram(const Program& program);

// -- what a step hands over ----------------------------------------------------------------------

// The lines of one step, handed to whoever runs the session the moment the step ends. `lines` is
// the room itself: the whole lines as they arrived, each ended with `\n`, valid until the call
// returns. Nothing of it is copied anywhere by this firmware (S-12) and nothing of it is logged.
struct StepReport {
  std::size_t index = 0;        // 1..6
  const char* command = "";     // the step's own, from the caller's program
  std::uint32_t seconds = 0;    // what it was given
  StepEnd end = StepEnd::kNone;
  const char* lines = nullptr;
  std::size_t used = 0;         // bytes, the line endings included
  std::size_t count = 0;        // whole lines
  bool first = false;           // the greeting's `ack`s and its `info` line are in this one
};

// Whoever runs the session. The next step begins when this returns, and the ceiling is read again
// before it does - so a caller that sits on a step's lines past the ceiling gets no further step
// and the switch back starts (rail 3).
class SessionSink {
 public:
  virtual ~SessionSink() = default;
  virtual void onStepLines(const StepReport& report) = 0;
};

// -- the room's five figures, measured at every session ------------------------------------------

// Through the seam the console's `heap` already has (`core/console/heap_report.h`): the free bytes
// and the largest free block just before the room is taken; the largest free block just after; and
// the least free since boot, read when the steps are over. **They decide nothing** - nothing here
// refuses anything because of them - and they exist so that `kStepBytes` can be raised later from
// what units say.
struct RoomFigures {
  bool known = false;       // false in a build that was handed no heap seam
  std::uint32_t bytes = 0;  // the room taken: kStepBytesExternal, or kStepBytes
  std::uint32_t free_bytes = 0;
  std::uint32_t block = 0;        // the largest free block before
  std::uint32_t left_block = 0;   // ... and after
  std::uint32_t least_free = 0;   // the least free since this boot, when the steps were over
  // The size of the external RAM the room came out of, or 0 when the room is the internal heap's
  // (task p16-05). It is the figure and not a flag because that is what the line prints, and
  // because an external room whose size is nought is not an external room.
  std::uint32_t external = 0;
};

// The one line the `room` journal entry, `rak session` and `heap room` all print, into `out`
// (always terminated). Two shapes, because the room came from one of two places:
//
//   65536 psram of 2097152; internal 27472, block 12288; least 25300    (task p16-05)
//   8192 of 27472, block 12288; left block 4096; least free 17108       (task p16-03)
//
// The external line says what the room is, where it came from and what the **internal** heap had
// while it was out - which is the figure that says whether anything else on this board was left
// short. The internal line says what it cost the one heap there was. Every figure of either is
// under a megabyte on this chip bar the PSRAM's own, so both fit the 72 bytes the task gives
// them; a build with no heap seam says so instead. Answers the bytes written.
constexpr std::size_t kRoomLineMax = 72;
std::size_t roomLine(const RoomFigures& figures, char* out, std::size_t cap);

// -- taking the room, which `heap room` does exactly as a session does --------------------------

// The room in hand: where it is, how big it is, and which of the two places it came from.
struct Room {
  char* data = nullptr;
  std::size_t bytes = 0;
  bool external = false;
};

// Takes the room a step's lines are laid down in and measures it, through one `interfaces::Scratch`
// (whose external half is the only way anything of this firmware asks for external memory, rail
// R24):
//
//   1. 65 536 bytes of external RAM, which brings the PSRAM up if nothing has yet;
//   2. the pattern of `patternHolds()` written over the whole of it and read back, **before the
//      first line goes in** - a room that fails is given back and is not used;
//   3. 8 192 bytes of the internal heap, when there was no external room or it failed.
//
// `figures` is filled either way - the internal heap's four figures through `heap` (which may be
// null in a build that was given none), and `external` set to the PSRAM's size when the room came
// from there - so that whoever took the room can print its line. `data` is null when neither room
// could be had; `figures->left_block` then says what the internal heap did have in one piece.
Room takeStepRoom(interfaces::Scratch& scratch, console::HeapReport* heap, RoomFigures* figures);

// Hands the room back and leaves `room` empty. Safe on a room that was never taken.
void giveStepRoom(interfaces::Scratch& scratch, Room* room);

// The pattern, written over the whole room and read back: the 32-bit word at index *n* is *n*
// times the FNV prime, exclusive-ored with 0xA5A5A5A5 - a value of its own index, so that two
// addresses which answer for each other (a dead address line, the commonest way a PSRAM is wrong)
// are caught rather than read back as each other's value. Written a byte at a time, because what
// the seam hands over is bytes and nothing here may assume its alignment. True when every byte
// read back is the byte written. **Leaves the room full of the pattern**: the caller writes over
// it before it reads, which every caller of a room does.
bool patternHolds(char* room, std::size_t bytes);

// -- what the machine is doing now ---------------------------------------------------------------

struct SessionStatus {
  Stage stage = Stage::kIdle;
  Ending ending = Ending::kRunning;
  Refusal refusal = Refusal::kNone;
  board::Slot slot{};
  std::size_t steps = 0;       // how many the program has
  std::size_t step = 0;        // which one is running or last ran, 1..6
  // How each step ended, which is the one thing about a step this object keeps. **The bytes and
  // the line count are not here**: they go out with the step in its `StepReport`, which is where
  // the console prints them from and where task p16-04's job reads them, and forty-eight bytes of
  // `.bss` for a figure that has already been handed over is forty-eight bytes off the heap the
  // main task's stack comes out of (tools/heap_at_start.py, rail R22).
  StepEnd step_end[kStepsMax] = {};
  RoomFigures room;
  std::uint32_t at_epoch = 0;      // when the session was opened, when this board knew the time
  std::uint32_t scanner_ms = 0;    // how long the radio has been a scanner (the ceiling's clock)
  std::uint32_t tries = 0;         // switch-back tries, this run and the ones the journal holds
  bool resumed = false;            // this run was taken up out of the journal
  bool greeted = false;            // `time`, `auto 0` and `info` were said
};

// The machine's answer to one step().
struct SessionStep {
  Stage before = Stage::kIdle;
  Stage after = Stage::kIdle;
  bool changed = false;
  bool waited = false;
  // The journal would not take the entry this stage's action is announced by: nothing was done
  // and nothing will be until something else mends the flash. A caller's loop stops here rather
  // than spinning, exactly as the radio's update machine's `kJournal` reason makes it.
  bool stalled = false;
  const char* note = "";  // one phrase for a person, static text
};

// -- what the journal says, for whoever is not running the session -------------------------------

// Read by the supervisor (a scanner in a session is not a scanner a person left running:
// docs/supervisor.md) and by `rak session` with no words. One backwards look at the newest entry
// of the session's own kind.
struct Mark {
  bool found = false;
  bool open = false;
  Stage stage = Stage::kIdle;
  board::Slot slot{};
  std::uint32_t at_epoch = 0;
  std::size_t step = 0;
};

bool readMark(const Journal& journal, Mark* out);

// -- the journal entry: kind::kScannerSession, 8 bytes and a body --------------------------------
//
//    0  stage      u8   a Stage: the journal's own word for it
//    1  step       u8   1..6 on a step's entry, 0 otherwise
//    2  slot       u8
//    3  flags      u8   bit 0: this entry was written by resume(); bits 4..6: the step's StepEnd
//    4  at_epoch   u32  when the session was opened, 0 when this board had no time of day
//    8  body            the step's command (1..40 bytes, no terminator: `size` says), or the
//                       `room` entry's six figures (24 bytes, little-endian u32)
//
// The body is one or the other and never both: a step's entry carries no figures and the `room`
// entry carries no command, so the 48 bytes a journal slot gives are enough for a 40-byte command.
//
// The sixth figure is task p16-05's, the size of the external RAM the room came out of. An entry
// written before that task has a body of 20 bytes and is read with that figure nought, which is
// what the line it prints means anyway: the room was the internal heap's.
constexpr std::uint8_t kSessionFlagResumed = 0x01;
constexpr std::size_t kSessionHeaderBytes = 8;
constexpr std::size_t kRoomFigureBytes = 24;
constexpr std::size_t kRoomFigureBytesBefore05 = 20;

struct SessionPayload {
  Stage stage = Stage::kIdle;
  std::uint8_t step = 0;
  std::uint8_t slot = 0;
  std::uint8_t flags = 0;
  std::uint32_t at_epoch = 0;
  char command[kCommandMax + 1] = {};
  RoomFigures room;
};

std::size_t encode(const SessionPayload& payload, std::uint8_t out[kPayloadMax]);
bool decode(const Entry& entry, SessionPayload* out);  // false unless it is a session entry

// -- the object ----------------------------------------------------------------------------------

class ScannerSession {
 public:
  // The radio's update machine, the link client, the store, the journal, the two clocks, the
  // battery and the room seam. The heap report is a pointer because a build may have none (every
  // host test but the one that scripts it): the `room` line then says so.
  ScannerSession(RadioUpdate& update, link::client::Client& radio, image::Store& store,
                 Journal& journal, interfaces::Clock& clock, interfaces::WallClock& wall,
                 BatterySource& battery, interfaces::Scratch& scratch,
                 console::HeapReport* heap = nullptr);

  // The link client keeps a pointer to the book-keeping below, which is a member of this object.
  ~ScannerSession();

  // **The one door** (rail 6): a person's `rak session` or task p16-04's job. Runs the whole check
  // - everything the switch to the scanner checks and everything the switch back will - and, when
  // it holds, takes the room, journals `check` (which is what *open* means) and leaves the machine
  // at `kCheck` for step() to drive. False with `refusal()` when it does not, and the radio,
  // the store and the heap are then exactly as they were.
  //
  // `program` is the caller's and must outlive the run; `sink` is where each step's lines go and
  // may be null (a caller that wants only the switch back).
  bool start(const Program& program, board::Slot slot, SessionSink* sink);

  // **An open session with nobody running it** (rail 2): the journal says one is open, so the
  // golden image is flashed back and proved and the journal is closed. It needs no server, no
  // uplink, no console, no room and no program. True when there was something to take up (the
  // machine is then active and step() drives it); false, and idle, otherwise.
  bool resume();

  // One step. Each call does at most one thing that takes time - a step of the radio's update
  // machine, a line said, a tenth of a second of listening - and returns.
  SessionStep step();

  // Whether this object has work in hand. **`stuck` is open and not active**: the journal still
  // says a session is open, so the next wake takes it up and tries the switch back again (rail
  // 5), but *this* run has nothing left to do and a caller's loop stops here rather than writing
  // a journal line a turn.
  bool active() const { return isOpen(stage_) && !stuck_; }
  Stage stage() const { return stage_; }
  Refusal refusal() const { return status_.refusal; }
  const SessionStatus& status() const { return status_; }
  // After a terminal stage the machine stays there until a caller clears it, so that `rak session`
  // can say what just happened; start() and resume() clear it too.
  void clear();

 private:
  bool journal(Stage stage, const char* command = nullptr);
  SessionStep enter(Stage next, const char* note, const char* command = nullptr);
  SessionStep waiting(const char* note);

  // The check's own pieces, each answering its refusal.
  Refusal check(board::Slot slot);
  bool takeRoom();
  void giveRoom();
  void readFiguresAfter();

  SessionStep stepScanner();   // the greeting, and the first step begun
  SessionStep stepRunning();   // one slice of the step in hand
  SessionStep stepBack();      // the switch back, mirrored out of the update machine
  SessionStep beginStep();     // the next step, or the room line and the switch back
  SessionStep goBack();        // the room line, the room given back, and kRoleRepeater started
  // Whether the golden image is back and proved **already**: the radio's own update machine was
  // resumed by this wake's step 3 and finished a switch back itself, or a scanner run rolled back
  // to it, or the journal says the last run of that machine did. Flashing it a second time would
  // be a quarter of an hour for nothing.
  bool goldenIsBack() const;
  void endStep(StepEnd end);
  void sayStop();
  bool pastCeiling() const;
  const ProgramStep* current() const;

  RadioUpdate& update_;
  link::client::Client& radio_;
  image::Store& store_;
  Journal& journal_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;
  BatterySource& battery_;
  interfaces::Scratch& scratch_;
  console::HeapReport* heap_;

  const Program* program_ = nullptr;
  SessionSink* sink_ = nullptr;
  Stage stage_ = Stage::kIdle;
  SessionStatus status_{};
  std::uint32_t first_seq_ = 0;

  // The borrowed buffer, given back when the steps are over: 64 kB of the PSRAM or 8 192 bytes of
  // the internal heap, and `room_.bytes` is which (task p16-05).
  Room room_{};
  link::client::TextRoom lent_{};    // the book-keeping about it, which is this object's
  bool lending_ = false;

  std::uint64_t scanner_at_ms_ = 0;  // when the scanner was found: the ceiling's own zero
  std::uint64_t step_until_ms_ = 0;  // when the step in hand runs out of its seconds
  std::uint64_t next_poll_ms_ = 0;
  Stage back_stage_ = Stage::kIdle;  // the last stage the switch back journalled
  bool back_started_ = false;        // kRoleRepeater has been started (or the rollback picked up)
  // The step in hand has ended and its lines have been handed over: the next step() begins the
  // next one. The stage stays `step` between the two, because that is what the journal says and
  // what a reset has to be read as.
  bool step_over_ = false;
  bool stuck_ = false;               // this run entered `stuck`: there is nothing left to try
};

}  // namespace update
}  // namespace core
