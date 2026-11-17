// Where a wake is, written down as it goes (task p15-06).
//
// In the night of 17 November 2026 this unit took the hourly `status` job of its own repeater at
// 01:29 UTC and again at 02:05. Both times it sent its heartbeat, claimed the job, and was never
// heard again in that wake: five minutes later the bound on a wake ended it by reset
// (core/cycle/wake_guard.h), the next boot read that as a watchdog, and the unit slept half an
// hour instead of a quarter. **What those two wakes were doing for five minutes, nobody could
// say.** The journal had *woke by watchdog*; the list of ten had *it never reached sleep (a
// reset, a crash or the bound)*. The log, the claimed job and the letters of the wake were in
// RAM, and the reset took them.
//
// So: one small record, in memory a reset leaves and a power cut takes, holding **where this
// wake is** - written when it changes, and read by the boot after. It is not a log and it is not
// a trace: it is one line, overwritten, so that the one wake that matters - the one that did not
// come back - leaves word.
//
//   the step        of the ten (core/cycle/wake_cycle.h). Written by the wake cycle as each
//                   begins, so a wake ended before its uplink came up names step 6.
//   what is out     a call to the server (`observations`, `heartbeat`, `claim`, a result, an
//                   extend, progress, config), or a claimed job being run and of which type.
//                   One field of text, because the names are the agent's and core/ must not hold
//                   a table of them: `components/agent/` writes the word it already prints.
//   a job in hand   a flag that is **not** cleared by a call, so a result posted inside a job
//                   still reads as *in a job*, with the job's requests beside it.
//   its requests    how many requests of the radio or the mesh the job in hand has made
//                   (`agent::LinkMeshCounters::requests`), saturating at 255.
//   waiting         that the modem or the link is being waited on, when this layer knows it.
//   the seconds     the second of the wake `what` began at, and the second the record was last
//                   written in - so the boot after can say *for how long*. Neither is read off
//                   a clock after the fact: the wake that is being measured is over.
//
// -- what it is **not** -------------------------------------------------------------------------
//
// It does not say *why* the wake hung - that is the trace build's and the bench's (docs/bench.md
// §13) - and it does not survive a power cut, which RTC memory does not. It cannot see inside the
// copied library either: a job that is spinning between two of its own steps, with no request on
// the link and no call out, reads as the job and the second it began, which is the honest answer
// this board has without changing a library it copies (tasks/phase-15/reports/06.md says what
// else it could not know).
//
// Logic: no ESP-IDF include, no heap, every byte of state a member of this object. The record is
// encoded to bytes by hand, as the journal's payloads are, so that no compiler's padding is part
// of the layout.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/update/journal.h"
#include "interfaces/clock.h"
#include "interfaces/retained.h"

namespace core {
namespace cycle {

// What a wake was in the middle of. The record's `doing` byte.
enum class Doing : std::uint8_t {
  kNothing = 0,  // nothing was written down: a record of another shape, or a wake before this task
  kStep = 1,     // a step of the ten, and nothing finer than that
  kCall = 2,     // a call to the server was out: `what` names it
  kJob = 3,      // a claimed job was being run: `what` is its type
  kCount,
};

const char* describe(Doing doing);

// How much of the name is kept: sixteen bytes, which is `mm::kMaxJobTypeLen` exactly - the
// library's own bound on a job type, and wider than the longest call name
// (`observations`, twelve). The field is **not** terminated when it is full, so every reader
// prints it with a width.
constexpr std::size_t kProgressWhatBytes = 16;

// A job in hand, and the two waits. The record's `flags` byte; the bit that says the wake reached
// sleep is the one the boot after tests before it says anything at all.
constexpr std::uint8_t kProgressReachedSleep = 0x01;  // step 10 was taken: there is no word to say
constexpr std::uint8_t kProgressInJob = 0x02;         // a claimed job was being run
constexpr std::uint8_t kProgressOnModem = 0x04;       // the modem is being waited on
constexpr std::uint8_t kProgressOnLink = 0x08;        // the link is being waited on

// The record's layout, which is what the size-keyed retained slot is keyed on
// (interfaces/retained.h: a record of another size is not the same record).
//
//    0  flags     u8   kProgress* above
//    1  step      u8   0 for none, else the step's number, 1 to 10
//    2  doing     u8   a Doing
//    3  requests  u8   requests of the radio or the mesh by the job in hand, saturating
//    4  since_s   u16  the second of the wake `doing` and `what` began at
//    6  seen_s    u16  the second of the wake this record was last written in
//    8  what[16]       the call's name or the job's type, NUL-padded, not always terminated
constexpr std::size_t kProgressBytes = 8 + kProgressWhatBytes;
static_assert(kProgressBytes <= interfaces::retainedSlotBytes(interfaces::RetainedSlot::kWakeProgress),
              "where a wake is has to fit its retained slot");

// The ceiling the two second-counts are clamped to: 18 hours, the same one the wake record's
// `lasted_s` uses (core/cycle/wake_reason.h), and no wake comes near it - the bound on one is
// five minutes and the longest a person's `stay` holds one open is twelve hours.
constexpr std::uint32_t kProgressAtMostS = 0xFFFFu;

// Where a wake was, as a reader gets it.
struct WakeWhere {
  bool said = false;          // a record was read back at all
  bool reached_sleep = false;
  bool in_job = false;
  bool on_modem = false;
  bool on_link = false;
  std::uint8_t step = 0;      // 0 for none, else 1 to 10
  Doing doing = Doing::kNothing;
  std::uint8_t requests = 0;
  std::uint16_t since_s = 0;
  std::uint16_t seen_s = 0;
  char what[kProgressWhatBytes + 1] = {};  // terminated here, whatever the record held

  // **There is something to say about this wake**: it was written down, it did not reach sleep,
  // and it had reached a step.
  //
  // The step is part of the question and not a nicety. A boot that never ran the wake cycle at
  // all - a USB host on the port, so a person is here and the console is the wake
  // (main/main.cpp); a reset inside §8's attention window before step 1 - leaves a record with no
  // step in it, and *the wake before did not reach sleep, and nothing had begun* is not a fault:
  // it is a bench session whose cable came out, which the bound ends by design
  // (core/cycle/wake_guard.h). Those leave no line, no journal entry and nothing counted; what
  // the boot log says about them is `restarts`' business and already true.
  bool endedMidway() const { return said && !reached_sleep && step != 0; }
  // For how long it was where it was, in seconds: 0 when nothing moved between the two counts.
  std::uint32_t forS() const { return seen_s > since_s ? seen_s - since_s : 0u; }
};

// -- the journal entry: kind::kWakeMidway, 26 bytes --------------------------------------------
//
//    0  wake      u8   the core::cycle::Wake the boot read (a watchdog, a crash, a reset)
//    1  flags     u8   kProgress* above, as the record held them
//    2  step      u8   0 for none, else the step's number, 1 to 10
//    3  doing     u8   a Doing
//    4  requests  u8
//    5  spare     u8   0
//    6  since_s   u16
//    8  seen_s    u16
//   10  what[16]
//
// The wake's own reason is in it because the entry has to stand on its own: `journal` prints what
// a line says and nothing beside it, and *step 8, a `status` job, 142 s* read without *and the
// bound ended it* is half an answer.
constexpr std::size_t kMidwayPayloadBytes = 10 + kProgressWhatBytes;
static_assert(kMidwayPayloadBytes <= update::kPayloadMax, "the payload has to fit a journal entry");

struct MidwayPayload {
  std::uint8_t wake = 0;  // a core::cycle::Wake as a byte
  WakeWhere where{};
};

void encode(std::uint8_t wake, const WakeWhere& where, std::uint8_t out[kMidwayPayloadBytes]);
bool decode(const update::Entry& entry, MidwayPayload* out);  // false unless it is one of these

class WakeProgress {
 public:
  WakeProgress(interfaces::Retained& retained, interfaces::Clock& clock);

  // Once a boot, before the wake does anything: the record of the wake **before** is read out and
  // then this wake's own begins. Both at once and in that order, because there is one record and
  // the wake before it is the only reader of it there can be.
  void begin();
  bool begun() const { return begun_; }

  // Where the wake before this one was. `said` false is the first boot, a power cut, or a record
  // of another shape - and `reached_sleep` true is the ordinary wake, which has nothing to say.
  //
  // **By value**, because this object keeps the record's 24 bytes and not a reading of them: see
  // the members.
  WakeWhere before() const;

  // -- as the wake goes ------------------------------------------------------------------------

  // A step of the ten is being begun: 1 to 10, as `core::cycle::Step` numbers them. It clears
  // whatever finer thing the step before was doing, because a step that has ended is not where
  // this wake is any more.
  void atStep(std::uint8_t step);

  // A call to the server is out, by the name the agent prints for it (`agent::callKindName`), and
  // has come back. `whileOut` sets *the modem is being waited on* with it; `cameBack` clears
  // both, and leaves a job in hand reading as one.
  void callIsOut(const char* name);
  void callCameBack();

  // A claimed job is being run, of this type, having made this many requests of the radio or the
  // mesh. Called as often as the caller likes - nothing is written unless something moved - and
  // `jobEnded()` when the runner has no job open.
  void jobIsRunning(const char* type, std::uint32_t requests);
  void jobEnded();

  // The link is being waited on: a reply this end is pumping for. A flag and nothing else, and
  // cleared when the wait is over.
  void waitingOnLink(bool waiting);

  // The wake is still going, at this second of it: called as the loop turns (DutyCycle::tick()),
  // so that *for how long* is answered for a wake nothing else wrote down again. Nothing is
  // written unless the second moved.
  void stillAwake();

  // Step 10 was taken: this wake has nothing to say to the boot after it. `sleepRefused()` takes
  // it back, for the one path that says it and then does not sleep (core/cycle/sleep.h).
  void reachedSleep();
  void sleepRefused();

  // This wake's own record, read out of the slot it lives in: for the console while the wake is
  // in hand, and for a test. By value, for the reason `before()` is.
  WakeWhere now() const;

  // Everything forgotten and the record erased, as a power cut leaves it: the bench, and a test.
  void forget();

 private:
  // The slot, in both directions. Every change is one of these each way: read, change, write.
  bool read(WakeWhere* out) const;
  void write(const WakeWhere& where);
  static void unpack(const std::uint8_t* bytes, WakeWhere* out);
  static void pack(const WakeWhere& where, std::uint8_t* bytes);
  // The second of the wake, counted from `begin()` and clamped.
  std::uint16_t second() const;
  // A caller's string as the field holds it: at most kProgressWhatBytes bytes, NUL-padded, and
  // whether that differs from what a reading holds.
  static void cropName(const char* name, char* out);
  static bool nameIs(const char* name, WakeWhere* where);  // true when it changed

  interfaces::Retained& retained_;
  interfaces::Clock& clock_;
  // **The wake before, as the record's own bytes**, latched at `begin()` because the slot is
  // overwritten by this wake a moment later. 24 bytes and not a decoded `WakeWhere`, and
  // **this wake's own record is not kept here at all** - it lives in the slot and is read out of
  // it when anybody asks. Every byte of this object is static storage: it is a member of the
  // `DutyCycle` in `main/app_main`'s, so it moves `_heap_start` and with it the heap the 20 kB
  // kept scratch is carved out of (`tools/heap_at_start.py`, rail R22). A reading of each would
  // have cost 60 bytes there to save a 24-byte memcpy and a CRC of RTC memory per change, which
  // is the wrong way round.
  std::uint8_t before_[kProgressBytes] = {};
  // The job in hand: its type and the second it began at, so that a call made inside it can put
  // both back when it comes back (see `callCameBack()`). The record has one name field and this
  // is the other name - nothing needs it after a reset, so it is not in the record.
  char job_what_[kProgressWhatBytes + 1] = {};
  std::uint16_t job_since_s_ = 0;
  std::uint32_t began_ms_ = 0;  // the low 32 bits: a wake is minutes, and this wraps in 49 days
  bool before_said_ = false;
  bool begun_ = false;
};

}  // namespace cycle
}  // namespace core
