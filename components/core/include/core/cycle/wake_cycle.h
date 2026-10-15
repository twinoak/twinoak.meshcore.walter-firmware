// One wake, start to finish (task p3-05): docs/ARCHITECTURE.md §10, docs/cycle.md,
// spec/SYSTEM.md S-7 and §5.
//
// Everything built in phases 0 to 3 exists and is tested alone. This is **the order they run
// in**, what happens when one of them fails, and the two guarantees that make a unit on a mast
// predictable: *nothing is skipped silently* and *nothing runs twice*.
//
//   1  why           the wake's reason (core/cycle/wake_reason.h)
//   2  the radio     the supervisor's check - and its ladder when the radio really is silent
//   3  resume        what the journal says was in progress (core/update/journal.h)
//   4  the state     STATUS, POWER_GET, ENV_GET, and §8's attention acknowledged
//   5  the band      the interval policy with the fresh reading (core/power/interval_policy.h)
//   6  the uplink    the modem, inside this wake's budget (core/uplink/policy.h)
//   7  the clock     the radio's, from the network's time (spec/LINK-PROTOCOL.md §5.4)
//   8  the backend   the seam, and nothing beyond it (interfaces/backend.h)
//   9  one update    at most one, of one MCU (S-7)
//  10  sleep         task p3-04's one way out (core/cycle/sleep.h)
//
// -- what a step is ----------------------------------------------------------------------------
//
// A name, a precondition, a time bound, and an outcome that is written down. The preconditions
// are **data** (`kSteps` below: which step needs which), so "the clock was not set because the
// uplink did not come up" is a table and not a comment, and a host test walks the table. A step
// that fails records why and the wake goes on to the next step that does not depend on it; a
// step whose dependency failed is *skipped*, which is a different word from *failed* and prints
// as a different letter.
//
// Every wake ends in step 10. That is the invariant the whole of this file exists for: a wake
// that cannot sleep is a battery the repeater needed, so sleep depends on nothing, is never
// refused for want of time, and a wake that reaches it having done nothing else has still done
// the one thing that matters.
//
// -- the budget --------------------------------------------------------------------------------
//
// `kWakeBudgetMs` is what a wake may spend in all. Each step has its own bound and its own
// *least*: a step is begun only when what is left of the budget is at least its least, and its
// deadline is the sooner of its bound and what is left. A step that cannot be begun is recorded
// as **no time** - skipped, deliberately, with a reason a person can read - and never half-run.
//
// Two things are allowed to hold a wake open past the budget, each up to its own bound, and both
// raise the bound on the wake (`core/cycle/wake_guard.h`, `allow()`):
//
//   a climb    the supervisor's ladder waits thirty, sixty and a hundred and twenty seconds
//              between rungs and **the state of a climb does not survive a sleep** - only its
//              counts and a stop do (core/supervisor/supervisor.h). So a climb runs to its end
//              inside its wake or starts again at rung 1 in the next one, and the caps are what
//              keep that from becoming a loop.
//   an update  "a flash may take several wakes' worth of time and is allowed to hold the wake
//              open up to its own bound" (the task). A megabyte over a 100 kHz debug port, a
//              boot window and a three-minute proving period do not fit in four minutes. The
//              journal is what makes the alternative safe: a wake that overruns even the raised
//              bound is reset, and `resume()` takes the run up again at the next boot.
//
// -- nothing runs twice ------------------------------------------------------------------------
//
// Each step runs once per pass, in order, and `run()` is not re-entrant (a second call while one
// is running answers the pass in hand). At most one update step happens in a wake, of one MCU,
// and the other is refused with `kOneUpdateOnly` and told to the backend at the next exchange.
// The radio's update and the Walter's own already refuse each other through the journal (S-7);
// this is the third lock on the same door, and the one that reports it.
//
// -- the seam ----------------------------------------------------------------------------------
//
// Step 8 talks to `interfaces::Backend` and to nothing else. In this repository the only
// implementation does nothing (`core/cycle/null_backend.h`). **Nothing here invents a server, a
// URL path, a JSON field or a job**: what crosses the seam is a plain structure this firmware
// fills and a closed set of requests it already knows how to carry out (spec/SYSTEM.md §8,
// spec/proposals/). The mesh client's calls are not used at all - no job is begun, no
// observation is pulled, nothing is sent on the mesh - and a host test runs a simulated month of
// wakes and holds the radio to it.
//
// Logic: no ESP-IDF include, no heap. One WakeCycle is about a kilobyte of state (most of it the
// structure that crosses the seam) and belongs in static storage.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/cycle/duty_cycle.h"
#include "core/cycle/network_clock.h"
#include "core/cycle/telemetry_ring.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/power/interval_policy.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/fetch.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "core/uplink/policy.h"
#include "interfaces/backend.h"
#include "interfaces/clock.h"
#include "interfaces/text_out.h"

namespace core {
namespace cycle {

// -- the steps ---------------------------------------------------------------------------------

enum class Step : std::uint8_t {
  kWhy = 0,       // 1
  kRadio = 1,     // 2
  kResume = 2,    // 3
  kState = 3,     // 4
  kBand = 4,      // 5
  kUplink = 5,    // 6
  kClock = 6,     // 7
  kBackend = 7,   // 8
  kUpdate = 8,    // 9
  kSleep = 9,     // 10
  kCount,
};

constexpr std::size_t kStepCount = static_cast<std::size_t>(Step::kCount);
static_assert(kStepCount + 1 <= interfaces::kBackendStepLetters,
              "the wake record's letters have to fit what crosses the backend seam");

const char* name(Step step);      // one word: `uplink`
const char* describe(Step step);  // one phrase: `the modem, inside this wake's budget`

// The bit of a step, for the dependency table.
constexpr std::uint16_t stepBit(Step step) {
  return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(step));
}

// How a step went. The letters are the wake record's and are documented in docs/cycle.md; they
// are what makes a week of wakes a page a person can read.
enum class Outcome : std::uint8_t {
  kNotRun = 0,    // '.' the pass did not reach it (it ended first)
  kDone = 1,      // 'y' it ran and did what it was for
  kNothing = 2,   // '-' it ran and there was nothing to do (no update journalled, no ask)
  kSkipped = 3,   // 's' a step it needs did not succeed: not attempted, and why is recorded
  kNoTime = 4,    // 't' the wake's budget would not cover it
  kRefused = 5,   // 'r' it was asked for and this firmware would not (the band, S-7, a secret)
  kFailed = 6,    // 'x' it was attempted and did not work
  kCount,
};

const char* describe(Outcome outcome);
char letter(Outcome outcome);

// -- the bounds, each with a sentence; every one a PLACEHOLDER until the bench ------------------

// What a whole wake may spend. Two and a half minutes, and the arithmetic: the uplink's
// registration wait is two of them (`core/uplink/policy.h`, kRegistrationDeadlineMs - the first
// number the bench changes), the rest of a wake is seconds, and `core/cycle/wake_guard.h`'s bound
// is five minutes - so there is twice the budget of margin before the rail that resets a wake,
// which is what one worst-case modem call that comes back late costs (kModemCallCeilingMs).
// **Deliberately less than the uplink policy's own default of four minutes**: that number is what
// the modem may have when nothing else wants the wake, and a wake has nine other steps. TO BE SET
// ON THE BENCH against what a wake with an attach actually costs the pack (docs/bench.md §14).
constexpr std::uint32_t kWakeBudgetMs = 150000;
static_assert(kWakeBudgetMs < kWakeBoundMs, "the budget has to fit inside the bound on a wake");

// The least that is left of the budget for a step with no other claim on it: a second. Below it
// nothing is begun, because a step begun with nothing left is a step that will be cut off.
constexpr std::uint32_t kLeastMs = 1000;

// Step 1. §8's whole repeat window plus a second, because that is what working out why we woke
// can cost when the radio's line is what woke us (core/cycle/wake_reason.h).
constexpr std::uint32_t kWhyBoundMs = kAttentionWindowMs + 1000;

// Step 2, the question. A PING is one exchange of up to three attempts (§3), and classifying a
// silence is the link's three tries twice over (core/supervisor/supervisor.h, kAskAgainPauseMs):
// forty seconds covers the pause and the asks around it.
constexpr std::uint32_t kRadioBoundMs = 40000;

// Step 2, a climb. The ladder's own waits add up to four minutes before a flash
// (core/supervisor/supervisor.h: 30 + 30 + 60 + 120 s); six is that with room for the asks
// between them. A climb that reaches rung 4's flash raises the bound again, to an update's.
constexpr std::uint32_t kLadderBoundMs = 360000;

// Steps 3 and 9, an update. The flasher's whole pass over the radio's application region at the
// debug port's clock, plus kBootWindowMs and kProvingPeriodMs (core/update/radio_update.h), plus
// the store's read-back. Twenty minutes is a guess with the right shape and is the number
// docs/bench.md §8 will replace.
constexpr std::uint32_t kUpdateBoundMs = 1200000;
static_assert(kUpdateBoundMs <= kRaisedBoundMax, "an update's bound has to be one a wake may ask for");

// Step 9, this firmware's own trial (S-6). The trial window and a minute: a trial that cannot
// confirm itself rejects itself inside `kTrialWindowMs` and restarts, and a wake that was reset
// before it could would be a trial nobody finished.
constexpr std::uint32_t kTrialBoundMs = update::kTrialWindowMs + 60000;
static_assert(kTrialBoundMs <= kRaisedBoundMax, "a trial's bound has to be one a wake may ask for");

// Step 4. Three exchanges on the link, each up to three attempts of a second (§3, §10).
constexpr std::uint32_t kStateBoundMs = 15000;

// Step 6. **The modem may spend the whole of the wake's budget**, which is what task p3-01's own
// design says of it ("one budget per wake that the whole of it may spend"); what the wake cycle
// changes is the number, not the rule. A wake whose modem really does take all of it then has
// *no time* for the steps after it, and that is recorded rather than hidden - which is the case
// the budget exists for.
constexpr std::uint32_t kUplinkBoundMs = kWakeBudgetMs;
// And the least worth beginning it for: a modem that cannot even be asked whether it is there
// (kPresentDeadlineMs) and reset once is a modem not worth waking.
constexpr std::uint32_t kUplinkLeastMs = 20000;

// Step 7. One TIME_SET, or a TIME_GET and a TIME_SET.
constexpr std::uint32_t kClockBoundMs = 10000;

// Step 8. One exchange with whoever is behind the seam. A minute: an implementation that wants
// longer is an implementation spending a battery it cannot see (interfaces/backend.h). And the
// least worth beginning one for is half of that: the unit is on LTE-M at a few kilobytes a
// second (spec/proposals/02, *Serving*), so an exchange given ten seconds would be an exchange
// cut off halfway, and half an exchange is worse than none - the readings would be offered and
// nobody would have taken them.
constexpr std::uint32_t kBackendBoundMs = 60000;
constexpr std::uint32_t kBackendLeastMs = 30000;

// Step 10. SLEEP_NOTICE, the modem down, the pads held (core/cycle/sleep.h). Fifteen seconds,
// and it is **never refused for want of time**: `kSteps`'s least for this step is zero.
constexpr std::uint32_t kSleepBoundMs = 15000;

// How far the radio's clock may be out before step 7 sets it (spec/LINK-PROTOCOL.md §5.4). A
// minute: MeshCore judges replays by timestamps and the radio's own clock drifts with its
// crystal, so a skew of a few seconds is not worth a frame and a skew of minutes is worth
// correcting. TO BE SET ON THE BENCH against a repeater's drift over a day.
constexpr std::uint32_t kClockSkewS = 60;

// How much shorter the next sleep is when a download is half done and the band is `plenty`: a
// quarter of an hour's wake becomes five minutes, so a release arrives in a morning rather than
// in a day. **Never in `saving` or `critical`** (the task), and never longer than the band's own
// interval.
constexpr std::uint32_t kDownloadMinutes = 5;

// How many slices one step may drive a state machine for before it gives up on it even inside
// its bound. A machine that answers `waited` for ever is a bug, and a bound in time alone would
// let it spin: five thousand slices of a tenth of a second is longer than any bound here.
constexpr std::uint32_t kSlicesMax = 5000;

// One row of the step table: what the step is, what it is allowed to spend, and which steps must
// have succeeded before it is worth attempting. **This is the dependency table**, and it is data:
// host_test/core/cycle/test_wake_steps.cpp walks it, and docs/cycle.md prints it.
struct StepDef {
  Step step;
  const char* name;
  std::uint32_t bound_ms;  // the most this step may take
  std::uint32_t least_ms;  // the least that must be left of the budget to begin it
  std::uint16_t needs;     // stepBit() of each step that must have succeeded
};

extern const StepDef kSteps[kStepCount];
const StepDef& def(Step step);

// -- what one pass did -------------------------------------------------------------------------

// How a wake ended. Only the first is the ordinary one.
enum class Ending : std::uint8_t {
  kRunning = 0,       // the pass is not over (or this is a dry run)
  kSlept = 1,         // step 10: on the board, the call never came back
  kSleepRefused = 2,  // step 10 refused: core/cycle/sleep.h says why, and the bound will end it
  kRestarting = 3,    // a restart was asked for: the Walter's own update switched slots, or the
                      // trial rejected itself (core/update/self_update.h)
  kOverran = 4,       // the bound on the wake ended it (core/cycle/wake_guard.h)
  kDry = 5,           // `cycle dry`: nothing was done
};

const char* describe(Ending ending);

struct StepRecord {
  Outcome outcome = Outcome::kNotRun;
  const char* why = "";      // one phrase, static text: never a buffer
  std::uint32_t spent_ms = 0;
  std::uint32_t deadline_ms = 0;  // what it was given, for the record and for `cycle dry`
};

struct Pass {
  bool dry = false;
  Wake reason = Wake::kUnknown;
  std::uint8_t attention = 0;                              // §8's reason, when the radio asked
  supervisor::Health radio = supervisor::Health::kUnknown;
  supervisor::Silence silence = supervisor::Silence::kNone;
  power::Band band = power::Band::kUnknown;
  power::Decision decision{};
  StepRecord step[kStepCount];
  Ending ending = Ending::kRunning;
  SleepResult sleep{};
  // The budget, as it was spent.
  std::uint32_t budget_ms = kWakeBudgetMs;
  std::uint32_t spent_ms = 0;
  // The time a climb, an update or a trial took, which is **outside** the budget: see the header
  // comment. `spent_ms - outside_ms` is what the ordinary steps cost.
  std::uint32_t outside_ms = 0;
  std::uint32_t left_ms = kWakeBudgetMs;
  std::uint32_t bound_ms = kWakeBoundMs;  // the guard's, raised or not
  std::uint32_t awake_s = 0;
  std::uint32_t interval_s = 0;
  std::uint32_t wakes = 0;
  // The seam.
  interfaces::Exchanged exchanged = interfaces::Exchanged::kFailed;
  std::uint8_t asks = 0;                                   // what the answer asked for, as bits
  interfaces::Ask carried = interfaces::Ask::kNothing;     // the one thing that was done
  std::size_t readings_offered = 0;
  bool readings_taken = false;
  bool climbed = false;        // step 2 climbed the ladder
  bool on_trial = false;       // this firmware was proving itself
  bool clock_set = false;      // the radio's clock was set in this wake
  bool time_known = false;     // this firmware knew what day it was
  bool shortened = false;      // step 10's interval was shortened for a half-done download

  Outcome outcomeOf(Step s) const { return step[static_cast<std::size_t>(s)].outcome; }
  bool ok(Step s) const { return outcomeOf(s) == Outcome::kDone; }
};

// The wake's record as one line, into `out` (at most `cap` bytes, always terminated). The format
// is docs/cycle.md's and a host test holds the two together. It carries no secret, no IMEI, no
// ICCID and no IMSI - a test greps a month of them.
std::size_t recordLine(const Pass& pass, char* out, std::size_t cap);

// The letters alone, one per step in order, terminated: what the record's `steps` field is and
// what crosses the backend seam.
void stepLetters(const Pass& pass, char* out, std::size_t cap);

// -- the journal entry: kind::kWakeCycle, 32 bytes ---------------------------------------------
//
//    0  wake        u8   a core::cycle::Wake
//    1  band        u8   a core::power::Band
//    2  radio       u8   a core::supervisor::Health
//    3  flags       u8
//    4  steps       10 B one Outcome each, in Step's order
//   14  asks        u8   what the backend asked for, as askBit() bits
//   15  carried     u8   the Ask that was carried out
//   16  awake_s     u32
//   20  interval_s  u32  what the next sleep was asked for
//   24  at_epoch    u32  0 when there was no time of day
//   28  wakes       u32  wakes counted since the retained record began
//
// **Not every wake**, and the reason is task p3-04's: the journal is 1024 entries in one 64 KB
// partition and it exists so that an update is never guessed at, and a unit checking in every
// quarter of an hour would wrap the whole log in ten days. So a wake with nothing to say is in
// the console's log and in retained storage, and the journal gets a line when the wake has
// **news** - a step failed, was refused or ran out of time; the band changed; an update or a
// fetch happened; an exchange happened; the wake was a crash or a watchdog. "A page a person can
// read" is what the task asks `journal` to be after a week on a mast, and seven hundred identical
// lines are not a page. Said again in tasks/phase-3/reports/05.md under *Deviations*.
constexpr std::size_t kWakePayloadBytes = 32;
static_assert(kWakePayloadBytes <= update::kPayloadMax, "the payload has to fit a journal entry");

constexpr std::uint8_t kWakeFlagOnTrial = 0x01;
constexpr std::uint8_t kWakeFlagClockSet = 0x02;
constexpr std::uint8_t kWakeFlagExchanged = 0x04;
constexpr std::uint8_t kWakeFlagReadingsTaken = 0x08;
constexpr std::uint8_t kWakeFlagBandChanged = 0x10;
constexpr std::uint8_t kWakeFlagClimbed = 0x20;
constexpr std::uint8_t kWakeFlagShortened = 0x40;
constexpr std::uint8_t kWakeFlagSlept = 0x80;

struct WakePayload {
  Wake wake = Wake::kUnknown;
  power::Band band = power::Band::kUnknown;
  supervisor::Health radio = supervisor::Health::kUnknown;
  std::uint8_t flags = 0;
  Outcome step[kStepCount] = {};
  std::uint8_t asks = 0;
  interfaces::Ask carried = interfaces::Ask::kNothing;
  std::uint32_t awake_s = 0;
  std::uint32_t interval_s = 0;
  std::uint32_t at_epoch = 0;
  std::uint32_t wakes = 0;
};

void encode(const WakePayload& payload, std::uint8_t out[kWakePayloadBytes]);
bool decode(const update::Entry& entry, WakePayload* out);  // false unless it is a wake entry

// -- the object --------------------------------------------------------------------------------

class WakeCycle {
 public:
  // The duty cycle, the radio and the uplink are references: there is no wake without them. The
  // rest are pointers because a build can be without them - no `rakfw` partition, no `journal`
  // partition, no settings store - and main/main.cpp hands over nothing rather than a machine
  // over flash that is not there. A step with nothing to do it with is recorded as `-` and the
  // wake goes on, which is what a factory image does.
  WakeCycle(DutyCycle& duty, link::client::Client& radio, uplink::UplinkPolicy& uplink,
            interfaces::Backend& backend, TelemetryRing& readings, NetworkClock& wall,
            interfaces::Clock& clock, supervisor::Supervisor* supervisor = nullptr,
            update::RadioUpdate* update = nullptr, update::SelfUpdate* self = nullptr,
            update::Fetch* fetch = nullptr, image::Store* store = nullptr,
            update::Journal* journal = nullptr, settings::Settings* settings = nullptr,
            interfaces::TextOut* out = nullptr);

  // One pass. **On the board this does not return**: step 10 is a deep sleep. It comes back when
  // the sleep was refused, when the Walter is restarting into another slot, or when the bound on
  // the wake ended it - and `last().ending` says which.
  const Pass& run();

  // What a pass would do, doing none of it: every precondition evaluated, every bound shown,
  // nothing on the link, nothing to the modem, no byte written. `cycle dry` at the console.
  const Pass& dry();

  const Pass& last() const { return pass_; }
  bool running() const { return running_; }

  // What crossed the seam at the last exchange: what `cycle` prints, and what a test reads.
  const interfaces::BackendKnowledge& known() const { return known_; }
  const interfaces::BackendAnswer& answer() const { return answer_; }

  // A console command asked for one of the closed set, to be carried out by the next pass. The
  // backend's answer is the other way in; this is "or a console command asked for it" (the task,
  // step 9). Several may be wanted; S-7 still allows one update step a wake.
  void askFor(interfaces::Ask ask);
  std::uint8_t asked() const { return wanted_; }
  void forgetAsks() { wanted_ = 0; }

 private:
  // One step, run or not: the table's row, the preconditions, the budget, and the record.
  bool begin(Step step);                     // false: it was skipped or there was no time
  void finish(Step step, Outcome outcome, const char* why = "");
  std::uint64_t deadlineFor(Step step) const;
  std::uint32_t leftMs() const;
  // The journal, when the wake has news.
  void journalWake();
  void say(const char* text);
  void sayf(const char* format, ...) __attribute__((format(printf, 2, 3)));

  void stepWhy();
  void stepRadio();
  void stepResume();
  void stepState();
  void stepBand();
  void stepUplink();
  void stepClock();
  void stepBackend();
  void stepUpdate();
  void stepSleep();

  // Step 9's two halves, each answering whether it did something.
  bool runTrial(std::uint64_t deadline_ms);
  bool runSelfUpdate();
  bool runRadioUpdate();
  bool runFetch(std::uint64_t deadline_ms);
  bool applySettings();
  // The ladder, and the update machine, in slices inside a deadline. True when the machine is
  // finished with; false when the deadline or the bound stopped it.
  bool driveLadder(std::uint64_t deadline_ms);
  bool driveUpdate(std::uint64_t deadline_ms);
  // The guard's bound raised for what is in hand.
  void allow(std::uint32_t bound_ms);
  // A refusal: recorded for the next exchange, and in this pass.
  void refuse(Step step, interfaces::Ask ask, interfaces::Refusal why, const char* phrase);

  // The structure that crosses the seam, filled from everything above it.
  void fillKnowledge();
  void fillFirmware();
  void fillStore();
  void fillSupervisor();

  DutyCycle& duty_;
  link::client::Client& radio_;
  uplink::UplinkPolicy& uplink_;
  interfaces::Backend& backend_;
  TelemetryRing& readings_;
  NetworkClock& wall_;
  interfaces::Clock& clock_;
  supervisor::Supervisor* supervisor_ = nullptr;
  update::RadioUpdate* update_ = nullptr;
  update::SelfUpdate* self_ = nullptr;
  update::Fetch* fetch_ = nullptr;
  image::Store* store_ = nullptr;
  update::Journal* journal_ = nullptr;
  settings::Settings* settings_ = nullptr;
  interfaces::TextOut* out_ = nullptr;

  Pass pass_{};
  interfaces::BackendKnowledge known_{};
  interfaces::BackendAnswer answer_{};
  // What a console command asked for and the next pass has not carried out yet: askBit() bits.
  std::uint8_t wanted_ = 0;
  bool running_ = false;
  std::uint64_t began_ms_ = 0;
  std::uint64_t step_began_ms_ = 0;
  // The radio's clock, as step 4's STATUS reported it.
  bool radio_time_known_ = false;
  std::uint8_t radio_time_state_ = 0;
  std::uint32_t radio_epoch_ = 0;
  // The time spent inside a climb, an update or a trial: outside the wake's budget.
  std::uint32_t outside_ms_ = 0;
  // Whether an update step has already happened in this wake (S-7).
  bool update_ran_ = false;
};

}  // namespace cycle
}  // namespace core
