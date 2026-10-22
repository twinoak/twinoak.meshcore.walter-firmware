// The supervisor (task p2-05): docs/ARCHITECTURE.md §8 and §2, spec/SYSTEM.md §5 ("the RAK's
// heartbeat; the ladder from 'ask again' to 'reflash the golden image', with daily limits"),
// spec/LINK-PROTOCOL.md §5.1-§5.4, §8 and §9, and the hardware repository's
// software/software-goals.md at b798aab must-haves 2 and 6 - an autonomous watchdog with a
// power-cycle, and boot-loop detection.
//
// When the radio goes quiet the Walter does the least thing that might work, then the next, each
// after a wait and each a limited number of times a day; when nothing works it stops and says so.
// A supervisor that does too little leaves a repeater dead for weeks; one that does too much
// power-cycles a radio every minute and drains the battery the two share. Both are failures and
// the second is the one a bug produces, which is why every expensive rung is counted, capped,
// journalled and refusable - and why the refusal is itself reported.
//
// -- what silence means, before anything is done (LINK-PROTOCOL §9) -----------------------------
//
//   the scanner is running   NoiseScope speaks NDJSON lines on the same UART and answers no
//                            frame (§9). Not a fault: reported and left alone.
//   the Walter just woke     A power-up of the ESP32-S3 glitches GPIO1-14 low for about 60 us and
//     from a power-up        that reaches nRESET (ARCHITECTURE §2 rule 5): our own power-up reset
//                            the radio and it is still booting. Wait.
//   an update is running     the radio's update machine is active, or the journal says a run of
//                            it is unfinished, or the Walter's own update is unconfirmed (S-7).
//                            Not ours to disturb.
//   it really is not         the incident, and the ladder.
//     answering
//
// -- the ladder ---------------------------------------------------------------------------------
//
//   1. ask again later       the link's own three tries (§3), a pause, three more
//   2. a reset pulse         RadioPins::resetPulse() - open-drain low, 100 ms, a second's spacing
//   3. a power cycle         RadioPins::killCycle() - everything tristate first (§2 rule 3)
//   4. what is on the chip   Flasher::verify() of the golden image against the flash, changing
//                            nothing: it differs (or the bootloader would not start it) -> flash
//                            the golden image through the update machine's rollback path; it
//                            matches -> the firmware is intact and flashing would change
//                            nothing, so rung 5; protected or unreadable -> rung 5 with that
//                            reason
//   5. stop                  journal "radio unresponsive after <what was tried>", raise it for
//                            the next check-in, and do nothing more until a person or the server
//                            says otherwise - or until kQuietPeriodMs has passed, after which
//                            the ladder may be tried once more
//
// Each rung is tried only after the one below it failed and each is followed by a wait before the
// radio is judged again. A radio that comes back at any point ends the incident there: the rungs
// above it are not used.
//
// **Boot loops** (software-goals must-have 6): several EV_BOOTs inside kBootLoopWindowMs, or a
// STATUS whose uptime never gets past kBootLoopUptimeS, is a radio restarting itself. It takes
// the ladder from rung 3 - a reset will not cure what a reset is already doing - and the reset
// reasons it reported (§5.3) are kept for the report.
//
// **The battery decides too.** No power cycle below kPowerCycleMinMv and no flash below
// kFlashMinMv: a radio that browned out because the battery is empty needs sun, not a reflash,
// and POWER_GET's last good reading (core/update/last_good_battery.h) with the radio's reset
// reason are what tell the two apart. A refusal by the battery is recorded in the incident.
//
// **Attention** (§8). An EV_ATTENTION is acknowledged by the next request this object makes -
// §8: the radio repeats it until any valid request arrives - recorded with its reason, and handed
// to whoever runs the wake (phase 3) by takeAttention().
//
// -- what survives a restart --------------------------------------------------------------------
//
// The counts, the window they are counted in, a stop in force and the incidents are in retained
// storage (interfaces/retained.h: RTC memory, which a power cut clears) **and** in the journal
// (core/update/journal.h, kind::kSupervisor), which a power cut does not. Every counted action
// writes a journal entry carrying the counts after it, so begin() can rebuild them from flash
// when the retained record is gone. A cap is forgiven only when a whole day can be *shown* to
// have passed; a clock that cannot be held against the record - a power cut, a clock that
// restarted at zero after a deep sleep (interfaces/clock.h) - is read as "assume the worst": the
// window is re-anchored and the counts are kept. `rak watch reset-caps` is the bench's way out.
//
// It is logic: no ESP-IDF include, no heap, one step per call, every loop bounded.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/flasher/flasher.h"
#include "core/health/health.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/update/battery.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "interfaces/clock.h"
#include "interfaces/retained.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace supervisor {

// -- the waits, the caps, the windows and the thresholds ----------------------------------------
//
// Every one of them is named here, with a sentence, and every one is a PLACEHOLDER until a radio
// has been watched on the bench: docs/supervisor.md tabulates them and docs/bench.md §11 is how
// they are measured. Change a number and its sentence together.

// How often the heartbeat asks, at most. The wake cycle (phase 3) calls step() once a wake, so
// this only bounds a caller that steps in a loop - the console's `rak ladder`, a test: a PING a
// minute is nothing to the link and nothing to the battery, and it keeps a loop from becoming a
// flood.
constexpr std::uint32_t kHeartbeatIntervalMs = 60000;

// Rung 1. §9 is "three attempts, then the link is down"; this is the pause before the second
// three. Half a minute is long enough for a repeater busy with a mesh burst to come back to the
// UART and short enough that a dead radio is not waited on.
constexpr std::uint32_t kAskAgainPauseMs = 30000;

// The wait after each acting rung, before the radio is judged again. They grow, because what
// each rung asks of the radio grows: a reset is a boot (a repeater boots in seconds; thirty is
// the boot window's own room), a power cycle is the rail collapsing, the radio's RAM going and a
// boot from cold, and a reflash is the update machine's business - it proves the image itself
// (core/update/radio_update.h: kBootWindowMs and kProvingPeriodMs), so the supervisor's own wait
// after it is only the room between the machine's end and the question.
constexpr std::uint32_t kAfterResetMs = 30000;
constexpr std::uint32_t kAfterPowerCycleMs = 60000;
constexpr std::uint32_t kAfterGoldenImageMs = 120000;

// How long the rail is held off at rung 3. The bounds and the settle time are the pins'
// (core/board/radio_pins.h kKillTiming: 1 s, 30 s, 200 ms, themselves placeholders); five
// seconds is comfortably past the rail's collapse at any plausible draw and short enough that a
// repeater is back on the mesh within a minute.
constexpr std::uint32_t kKillOffMs = 5000;

// The caps, per day. Each is "how many times may this cost the battery before a person should be
// looking at it instead": six resets is one every four hours, which no healthy unit needs; three
// power cycles is the rail collapsing three times, which a radio that is merely hung does not
// need; one reflash a day is enough - a golden image that did not take once will not take twice.
constexpr std::uint32_t kResetsPerDay = 6;
constexpr std::uint32_t kPowerCyclesPerDay = 3;
constexpr std::uint32_t kFlashesPerDay = 1;

// The day the caps are counted in: 24 hours from the first counted action of the window, not
// midnight - a Walter has no time of day until phase 3's modem reads it from the network
// (interfaces/wall_clock.h), and a cap that only works once the modem does is not a cap.
constexpr std::uint32_t kCapWindowMs = 86400000;
constexpr std::uint32_t kCapWindowS = kCapWindowMs / 1000;

// After rung 5 the ladder is not climbed again for this long: long enough that a mast with a
// hardware fault is not visited by the supervisor every hour, short enough that a radio which
// recovers on its own - a cell that warmed up, a connector that settled - is found again without
// a person. Three days.
constexpr std::uint32_t kQuietPeriodMs = 3u * 24 * 60 * 60 * 1000;

// A radio that restarts by itself this many times inside this window is in a boot loop
// (software-goals must-have 6) and the ladder starts at rung 3. Three boots in ten minutes is
// well past what a repeater does on purpose; the first batch has no RTC, so every boot is also a
// clock to set (§5.4), which is how the Walter notices them even while it sleeps (§8: EV_BOOT is
// one of the two events a sleeping Walter still gets).
constexpr std::uint32_t kBootLoopWindowMs = 600000;
constexpr std::uint32_t kBootLoopBoots = 3;

// The other way a boot loop shows: a STATUS whose `uptime_s` (§5.3) never gets past this, asked
// twice at least this far apart. Two minutes - longer than a repeater's own start-up, shorter
// than anything a healthy node does.
constexpr std::uint32_t kBootLoopUptimeS = 120;

// How long after this Walter's own power-up a silent radio is "still booting" and not a fault:
// the power-up glitch (ARCHITECTURE §2 rule 5) reset the radio at the same moment we started, so
// it has a repeater's whole boot ahead of it. The same twenty seconds the update machine gives a
// freshly flashed radio to say HELLO (core/update/radio_update.h kBootWindowMs), and measured
// from this Walter's own boot to the moment the asking *began* - asking a dead radio takes the
// link's three attempts twice over, which is seconds of it.
constexpr std::uint32_t kWalterPowerUpGraceMs = 20000;

// The battery below which a rung is refused. A power cycle costs the radio its RAM and a cold
// boot; a reflash costs ten minutes of two MCUs awake and ends with a radio that must prove
// itself, so it asks for more (core/update/radio_update.h kBatteryMinMv, which the update
// machine checks again for itself). The pack is one Li-ion cell (the hardware repository's
// CURRENT-SENSE.md: CV 4.200 V, precharge below 3.1 V).
constexpr std::uint16_t kPowerCycleMinMv = 3500;
constexpr std::uint16_t kFlashMinMv = update::kBatteryMinMv;

// What a caller that drives the ladder to its end sleeps between steps that only waited (the
// console's `rak ladder`). The same tenth of a second the update's console loop uses.
constexpr std::uint32_t kStepIdleMs = update::kStepIdleMs;

// How many journal entries one climb may write before something is looping. A full climb writes
// at most: the incident, five rungs, five judgements and the outcome; each restart of the Walter
// adds one. Thirty-two is generous and bounded.
constexpr std::uint32_t kEntriesPerClimbMax = 32;

// -- the words ----------------------------------------------------------------------------------

// The ladder's rungs. The numbers are docs/ARCHITECTURE.md §8's own numbering and are stored in
// the journal, so they do not move.
enum class Rung : std::uint8_t {
  kNone = 0,
  kAskAgain = 1,     // the link's three tries, a pause, three more
  kReset = 2,        // a reset pulse
  kPowerCycle = 3,   // the kill line
  kGoldenImage = 4,  // what is on the chip, and the golden image back if it is not
  kStop = 5,         // journalled, raised, and left alone
};
constexpr std::size_t kRungCount = 5;
const char* describe(Rung rung);
// The bit of a rung in an incident's `rungs_tried`.
constexpr std::uint8_t rungBit(Rung rung) {
  return static_cast<std::uint8_t>(1u << (static_cast<std::uint8_t>(rung) - 1));
}

// What a silence turned out to mean (§9). Only kNotAnswering is a fault.
enum class Silence : std::uint8_t {
  kNone = 0,          // the radio answered: there was no silence
  kScanner = 1,       // NoiseScope is running: reported, left alone
  kWalterPoweredUp = 2,  // our own power-up reset it; it is still booting
  kUpdateRunning = 3, // an update is in progress: not ours to disturb
  kNotAnswering = 4,  // the radio really is not answering
  // Task p5-03 (spec/SYSTEM.md S-5a): `rak provision` gave the radio its base and no
  // application yet, so it is silent on the link by design - exactly what the ladder would
  // answer with a reset, a power cycle or a golden image. Left alone until `rak update
  // --first-install` commits.
  kAwaitingFirstInstall = 5,
};
const char* describe(Silence silence);

// Why a climb was begun.
enum class Cause : std::uint8_t {
  kNone = 0,
  kSilence = 1,   // no answer, and none of §9's other three explanations
  kBootLoop = 2,  // the radio is restarting itself: the ladder from rung 3
};
// Whether a person asked for the climb is a flag on its journal entries (kFlagByHand), not a
// cause of its own: the ladder is climbed for what the radio is doing, never for who asked.
const char* describe(Cause cause);

// How one rung went.
enum class RungOutcome : std::uint8_t {
  kNotTried = 0,
  kDone = 1,             // it was done, and the radio did not come back
  kCured = 2,            // it was done, and the radio came back
  kIntact = 3,           // rung 4 only: the chip holds the golden image; flashing would change nothing
  kRefusedCap = 4,       // the day's cap for this rung is reached
  kRefusedBattery = 5,   // the battery is below this rung's threshold
  kFailed = 6,           // the pins, the flasher or the update machine refused or failed
  kSkipped = 7,          // a boot loop skips rungs 1 and 2
};
const char* describe(RungOutcome outcome);

// How a climb ended.
enum class Outcome : std::uint8_t {
  kRunning = 0,   // still climbing
  kCured = 1,     // the radio answers again
  kStopped = 2,   // rung 5: unresponsive after everything that was tried
  kRefused = 3,   // the climb ran out of rungs it was allowed to try (a cap, the battery)
};
const char* describe(Outcome outcome);

// Why rung 5 was reached, when there is more to say than "nothing worked".
enum class StopReason : std::uint8_t {
  kNone = 0,
  kEverythingTried = 1,    // every rung was tried and the radio is still silent
  kFirmwareIntact = 2,      // the chip holds the golden image: this is not a firmware fault
  kChipProtected = 3,       // the debug port says the chip is protected: a person's business (p1 rule)
  kChipUnreadable = 4,      // the chip could not be read at all
  kFlashFailed = 5,         // the golden image could not be put back
  kNoGoldenImage = 6,       // there is none to put back
  kCapReached = 7,          // the next rung's cap is reached
  kBatteryTooLow = 8,       // the next rung's battery threshold is not met
  kNoStore = 9,             // this build has no image store: rung 4 cannot be climbed
};
const char* describe(StopReason reason);

// -- the incident record: what is reported ------------------------------------------------------
//
// One compact record per incident, bounded in number (kIncidentsKept), kept in retained storage
// so that the next check-in has it after a sleep or a reset. Plain data: it is stored as bytes.
//
// Three of them: a retained record is at most kRetainedRecordMax (128 bytes, interfaces/
// retained.h - RTC memory is 8 KB and shared) and three incidents with their count is 124. The
// journal keeps the whole history; this is what survives a sleep without a flash read.
constexpr std::size_t kIncidentsKept = 3;

struct Incident {
  bool present = false;
  Cause cause = Cause::kNone;
  Outcome outcome = Outcome::kRunning;
  Rung cured_at = Rung::kNone;         // the rung after which the radio answered again
  StopReason stop = StopReason::kNone;
  std::uint8_t rungs_tried = 0;        // rungBit() of each rung reached
  RungOutcome rung[kRungCount] = {};   // index 0 is rung 1
  std::uint32_t began_epoch = 0;       // 0 when the Walter had no time of day
  std::uint64_t began_ms = 0;          // the monotonic time it began, this boot's epoch
  std::uint32_t lasted_ms = 0;         // 0 while it is running
  std::uint16_t battery_mv = 0;        // the last good POWER_GET reading, 0 when there was none
  std::uint8_t reset_reason = 0;       // the radio's own code, §5.3, last known
  std::uint8_t restarts = 0;           // restarts of the radio seen in the boot-loop window
  std::uint32_t last_uptime_s = 0;     // the last uptime STATUS reported, for a boot loop
};

// §8's attention, kept for whoever runs the wake.
constexpr std::size_t kAttentionsKept = 4;

// §8: the radio sends an attention, waits 300 ms, and then repeats it every 1000 ms until a
// request arrives or it has sent it ten times. Every one of those is the *same* attention, so a
// repeat inside this window updates the record rather than making a new one; §8's own rate
// limit - at most one new attention per reason per hour - makes that safe.
constexpr std::uint32_t kAttentionRepeatWindowMs = 11000;

struct Attention {
  bool present = false;
  std::uint8_t reason = 0;   // link::attention::*
  std::uint16_t detail = 0;
  std::uint8_t repeat = 0;
  std::uint32_t at_epoch = 0;
  std::uint64_t at_ms = 0;
  bool acknowledged = false;  // a request of ours has gone out since: §8's "until a request arrives"
};

// -- the counts and the caps --------------------------------------------------------------------

struct Counts {
  std::uint32_t resets = 0;
  std::uint32_t power_cycles = 0;
  std::uint32_t flashes = 0;
};

struct Limits {
  std::uint32_t resets = kResetsPerDay;
  std::uint32_t power_cycles = kPowerCyclesPerDay;
  std::uint32_t flashes = kFlashesPerDay;
};

// -- the machine --------------------------------------------------------------------------------

enum class State : std::uint8_t {
  kIdle = 0,      // nothing in hand: the heartbeat, when it is due
  kClassify = 1,  // a silence to work out (§9) before anything is done
  kClimbing = 2,  // the next rung is due
  kWaiting = 3,   // the wait after a rung
  kJudging = 4,   // the question after the wait: did that cure it
  kFlashing = 5,  // rung 4's flash: the update machine, one of its steps per step of ours
  kStopped = 6,   // rung 5 in force: nothing until the quiet period has passed
};
const char* describe(State state);

// What the radio is believed to be.
enum class Health : std::uint8_t {
  kUnknown = 0,  // nothing has been asked yet
  kHealthy = 1,  // a PING was answered
  kScanner = 2,  // NoiseScope is running (§9): not a fault, and not a repeater either
  kSilent = 3,   // the question went unanswered
  // Unanswered, and expected to be: the slot awaits its first install after `rak provision`
  // (task p5-03). Not kSilent on purpose - the wake cycle reads kSilent as "the link is known
  // down" for an update, and a radio with a bootloader and no application is not that.
  kAwaitingFirstInstall = 4,
};
const char* describe(Health health);

struct Step {
  State before = State::kIdle;
  State after = State::kIdle;
  bool changed = false;  // a transition happened
  bool waited = false;   // nothing to do yet (an interval or a wait has not passed)
  Rung rung = Rung::kNone;
  RungOutcome rung_outcome = RungOutcome::kNotTried;
  Silence silence = Silence::kNone;
  const char* note = "";  // one phrase for a person, static text
};

// What `rak watch` prints.
struct Status {
  State state = State::kIdle;
  Health health = Health::kUnknown;
  board::Slot slot{};
  // The heartbeat.
  bool ever_answered = false;
  std::uint64_t healthy_since_ms = 0;   // when the radio was first found healthy in this run
  std::uint64_t last_answer_ms = 0;     // when it last answered
  std::uint64_t last_asked_ms = 0;
  std::uint32_t pings = 0;
  std::uint32_t uptime_s = 0;           // what STATUS last said
  std::uint32_t boot_id = 0;
  std::uint8_t reset_reason = 0;
  bool time_set = false;                // TIME_SET was taken since the last restart
  bool time_skipped = false;            // no time to give (no modem yet): §5.4 could not be done
  // The window and the counts.
  Counts counts;
  Limits caps;
  std::uint64_t window_began_ms = 0;
  std::uint32_t window_began_epoch = 0;
  bool counts_from_journal = false;     // the retained record was gone and flash was read instead
  bool window_reanchored = false;       // the clock could not be held against the record
  // The stop, when there is one.
  bool stopped = false;
  StopReason stop_reason = StopReason::kNone;
  std::uint8_t stopped_after = 0;       // the rungs that had been tried, as bits
  std::uint64_t stopped_ms = 0;
  std::uint32_t quiet_left_ms = 0;      // until the ladder may be tried once more
  // The radio's restarts, for a boot loop.
  std::uint32_t restarts = 0;           // inside the boot-loop window
  std::uint32_t restarts_total = 0;
  bool boot_loop = false;
  // The battery, as the incident would record it.
  bool battery_known = false;
  std::uint16_t battery_mv = 0;
  std::uint32_t battery_age_ms = 0;
  // The climb in hand.
  Cause cause = Cause::kNone;
  Rung rung = Rung::kNone;
  Silence silence_seen = Silence::kNone;  // what the last silence turned out to mean
  std::uint32_t entries = 0;            // journal entries this climb has written
  std::uint32_t attentions = 0;         // §8 attentions recorded and not yet taken
  // The slot awaits its first install (task p5-03): `rak provision` erased the radio and gave
  // it its base, and nothing runs on it until `rak update --first-install` commits. Kept where
  // the supervisor's own state is kept - the retained record and the journal - so that a
  // Walter restart keeps it; `awaiting_since_seq` is the journal entry that marked it, and the
  // first-install run that clears it must be newer.
  bool awaiting_first_install = false;
  std::uint32_t awaiting_since_seq = 0;
};

// -- the journal entry's payload, 48 bytes ------------------------------------------------------
//
// Every entry of kind::kSupervisor carries this. The counts are in *every* entry, so that a
// Walter whose retained memory a power cut cleared can rebuild the day's counts from flash alone.
//
//    0  what       u8   which kind of entry this is (Wrote, below)
//    1  rung       u8
//    2  cause      u8
//    3  outcome    u8   a RungOutcome, an Outcome or a StopReason, depending on `what`
//    4  resets     u8
//    5  cycles     u8
//    6  flashes    u8
//    7  flags      u8
//    8  window_epoch   u32  the epoch the cap window began (0: no time of day)
//   12  window_ms      u32  the monotonic ms it began, truncated: a gloss, never arithmetic
//   16  at_epoch       u32
//   20  at_ms          u32  the monotonic ms of this entry, truncated
//   24  battery_mv     u16
//   26  reset_reason   u8
//   27  restarts       u8
//   28  uptime_s       u32  what STATUS last said
//   32  rungs_tried    u8
//   33  silence        u8
//   34  health         u8
//   35  spare          u8
//   36  detail         u32  the flasher's step, the update machine's reason: whatever the
//                           outcome has to add
constexpr std::size_t kPayloadBytes = 48;

// Which kind of supervisor entry it is.
enum class Wrote : std::uint8_t {
  kNone = 0,
  kIncident = 1,     // a climb begun: what was seen
  kRung = 2,         // a rung about to be done, or refused: `rung` and `outcome`
  kJudged = 3,       // the question after the wait: `outcome` kCured or kDone
  kOutcome = 4,      // the climb ended
  kStopped = 5,      // rung 5: unresponsive after <rungs_tried>
  kAttention = 6,    // §8: an attention recorded (reason in `rung`, detail in `detail`)
  kCapsReset = 7,    // `rak watch reset-caps`: a person emptied the day's counts
  kQuietOver = 8,    // the quiet period passed: the ladder may be climbed once more
  // Task p5-03: `rak provision` is about to erase the radio - the slot awaits its first
  // install from this entry on (a stop in force is lifted with it: the radio is a new radio);
  // and a `rak update --first-install` newer than that entry committed, so it no longer does.
  kAwaitingFirstInstall = 9,
  kFirstInstallDone = 10,
};
const char* describe(Wrote what);

constexpr std::uint8_t kFlagResumed = 0x01;       // written after a restart of the Walter
constexpr std::uint8_t kFlagByHand = 0x02;        // a person's command (`rak ladder`, `reset-caps`)
constexpr std::uint8_t kFlagBatteryKnown = 0x04;
constexpr std::uint8_t kFlagBatteryStale = 0x08;  // the reading is the last good one, not a fresh one
constexpr std::uint8_t kFlagBootLoop = 0x10;
constexpr std::uint8_t kFlagReanchored = 0x20;    // the cap window had to be re-anchored
// In *every* entry while the slot awaits its first install (task p5-03), as the counts are, so
// that a Walter whose retained record a power cut cleared reads the mark back from flash.
constexpr std::uint8_t kFlagAwaitingFirstInstall = 0x40;

struct Payload {
  Wrote what = Wrote::kNone;
  Rung rung = Rung::kNone;
  Cause cause = Cause::kNone;
  std::uint8_t outcome = 0;
  Counts counts;
  std::uint8_t flags = 0;
  std::uint32_t window_epoch = 0;
  std::uint32_t window_ms = 0;
  std::uint32_t at_epoch = 0;
  std::uint32_t at_ms = 0;
  std::uint16_t battery_mv = 0;
  std::uint8_t reset_reason = 0;
  std::uint8_t restarts = 0;
  std::uint32_t uptime_s = 0;
  std::uint8_t rungs_tried = 0;
  Silence silence = Silence::kNone;
  Health health = Health::kUnknown;
  std::uint32_t detail = 0;
};

void encode(const Payload& payload, std::uint8_t out[kPayloadBytes]);
bool decode(const update::Entry& entry, Payload* out);  // false unless it is a supervisor entry

// -- the object ---------------------------------------------------------------------------------

class Supervisor final : public link::client::RestartWatcher {
 public:
  // The store, the flasher and the update machine are pointers because a build can be without
  // them (no `rakfw` or no `journal` partition: main/main.cpp hands over nothing rather than a
  // store over flash that is not there). Rung 4 then refuses itself with kNoStore, and rungs 1
  // to 3 and the heartbeat work as they do. The journal is a pointer for the same reason; with
  // none, nothing is journalled and the retained record is all there is, which `rak watch` says.
  Supervisor(link::client::Client& radio, board::RadioPins& pins, board::SlotLock& slots,
             health::Health& health, update::BatterySource& battery, interfaces::Clock& clock,
             interfaces::WallClock& wall, interfaces::Retained& retained,
             image::Store* store = nullptr, flasher::Flasher* flasher = nullptr,
             update::RadioUpdate* update = nullptr, update::Journal* journal = nullptr,
             board::Slot slot = {});

  // At boot, after the pins are safe and the journal is read: the retained record, or the
  // journal when a power cut cleared it; the cap window held against the clock; a stop still in
  // force. Journals nothing unless the quiet period has passed (kQuietOver).
  void begin();
  bool begun() const { return begun_; }

  // One step. Each call does at most one thing that takes time (a question on the link, a reset,
  // a kill, one step of the update machine) and returns.
  Step step();

  // Whether there is something in hand: a climb, a wait, a flash. False when idle or stopped.
  bool climbing() const;

  State state() const { return state_; }
  Health health() const { return status_.health; }
  const Status& status() const { return status_; }
  const Counts& counts() const { return counts_; }
  const Limits& caps() const { return caps_; }
  board::Slot slot() const { return slot_; }

  // The incidents, newest first. `count` is how many of kIncidentsKept are real.
  std::size_t incidentCount() const { return incident_count_; }
  const Incident& incidentAt(std::size_t index) const;

  // §8. Reads whatever the radio has sent and records any attention in it. **Climbs nothing,
  // sends nothing, judges nothing**: it is what a wake on the radio's line calls while it works
  // out why it woke (task p3-04, core/cycle/wake_reason.h), before anything is decided. The one
  // place an EV_ATTENTION is decoded in this firmware is here, which is why the wake asks rather
  // than reading the client's queue itself.
  void listen() { drainEvents(); }

  // §8. The oldest recorded attention, removed: what phase 3's wake hands on. False when there
  // is none.
  bool takeAttention(Attention* out);
  std::size_t attentionCount() const { return attention_count_; }
  const Attention& attentionAt(std::size_t index) const;

  // -- the console's commands ------------------------------------------------------------------

  // `rak ladder`: begin a climb now. False when one is already in hand (state() says which).
  // A radio that answers is found out at the first question and the climb ends there, which is
  // what the bench's "remove the jumper" item watches.
  bool runLadder();
  // `rak watch reset-caps`: the day's counts to zero and a stop lifted. For the bench.
  void resetCaps();

  // -- the radio's base (task p5-03; spec/SYSTEM.md S-5a) --------------------------------------

  // `rak provision` is about to erase the radio: from here until a `rak update --first-install`
  // newer than this mark commits, the radio is silent by design and the ladder is not climbed
  // for it - a silence is classified kAwaitingFirstInstall and left alone. Journalled, kept in
  // the retained record, and a stop in force is lifted (the radio is a new radio). Nothing else
  // clears the mark: the supervisor watches the journal for the first install's `done` entry
  // (core/update/radio_update.h, Kind::kFirstInstall, State::kDone) at begin() and every step().
  void markAwaitingFirstInstall();
  bool awaitingFirstInstall() const { return awaiting_first_install_; }
  // The journal looked at now for a committed first install newer than the mark - what begin()
  // and step() do by themselves, and what the console does the moment `rak update
  // --first-install` ends, so that the mark is gone before the next prompt.
  void noticeFirstInstall() { reconcileFirstInstall(); }

  // link::client::RestartWatcher: an EV_BOOT, or a HELLO with a boot_id we had not seen (§5.1).
  // Called from inside a client call, so it only records.
  void onRadioRestart(std::uint32_t boot_id, std::uint8_t reset_reason) override;

 private:
  // The record in retained storage: interfaces::RetainedSlot::kSupervisor. Plain data.
  struct Record {
    std::uint32_t window_epoch;
    std::uint64_t window_ms;
    std::uint32_t resets;
    std::uint32_t power_cycles;
    std::uint32_t flashes;
    std::uint8_t stopped;      // 0 or 1
    std::uint8_t stop_reason;  // a StopReason
    std::uint8_t stopped_after;  // the rungs that had been tried, as bits
    std::uint8_t restarts;     // restarts of the radio inside the boot-loop window
    std::uint64_t stopped_ms;
    std::uint64_t restart_window_ms;
    std::uint32_t restarts_total;
    std::uint8_t awaiting_first_install;  // 0 or 1 (task p5-03)
    std::uint8_t spare[3];
    std::uint32_t awaiting_since_seq;     // the journal entry that marked it
  };
  // And the incidents: interfaces::RetainedSlot::kSupervisorIncidents.
  struct IncidentRecord {
    std::uint8_t count;
    std::uint8_t spare[3];
    Incident items[kIncidentsKept];
  };
  static_assert(sizeof(Record) <= interfaces::kRetainedRecordMax, "the supervisor's record must fit");
  static_assert(sizeof(IncidentRecord) <= interfaces::kRetainedRecordMax,
                "the supervisor's incidents must fit a retained record");

  void loadRecord();
  void storeRecord();
  void loadIncidents();
  void storeIncidents();
  // The newest supervisor entry of the journal, for the counts after a power cut.
  bool lastEntry(Payload* out) const;
  bool journalEntry(Wrote what, const Payload& about, std::uint32_t* out_seq = nullptr);
  Payload payloadNow(Wrote what);
  // The mark's own entry, for its seq, when the retained record is gone (task p5-03).
  std::uint32_t lastMarkSeq() const;
  // Clears the mark when the journal holds a committed first install newer than it.
  void reconcileFirstInstall();

  // The cap window, held against the clock. True when it was rolled (the counts are zero again).
  bool rollWindow();
  bool capReached(Rung rung) const;
  void countAction(Rung rung);

  // The battery as a rung needs it: the live reading when the link gives one, else the last good
  // one. `fresh` says which.
  bool batteryFor(Rung rung, std::uint16_t* mv, bool* fresh);

  Step stepIdle();
  Step stepClassify();
  Step stepClimbing();
  Step stepWaiting();
  Step stepJudging();
  Step stepFlashing();
  Step stepStopped();

  // The heartbeat: a PING, and after a restart the clock and a fresh STATUS (§5.2, §5.4, §5.3).
  bool ask();
  void drainEvents();
  void recordAttention(std::uint8_t reason, std::uint16_t detail, std::uint8_t repeat);
  // §8: a request of ours has gone out, so the radio has stopped repeating. Marks what is kept.
  void acknowledgeAttentions();
  void noteUptime(std::uint32_t uptime_s);
  bool bootLooping() const;
  Silence classify();
  void refreshStatus();

  void beginIncident(Cause cause);
  void endIncident(Outcome outcome, Rung cured_at, StopReason stop);
  void noteRung(Rung rung, RungOutcome outcome);
  Incident* current();
  Step enter(State next, const char* note);
  Step waiting(const char* note);
  Step rungStep(Rung rung, RungOutcome outcome, const char* note, State next, Rung then);
  Step failedRung(Rung rung, const char* why, Rung then, std::uint32_t detail);
  Step stopNow(StopReason reason);

  // Rung 4's read-only look at the chip, and the flash when it differs.
  Step lookAtChip();

  link::client::Client& radio_;
  board::RadioPins& pins_;
  board::SlotLock& slots_;
  health::Health& health_;
  update::BatterySource& battery_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;
  interfaces::Retained& retained_;
  image::Store* store_;
  flasher::Flasher* flasher_;
  update::RadioUpdate* update_;
  update::Journal* journal_;
  board::Slot slot_;

  bool begun_ = false;
  State state_ = State::kIdle;
  Status status_;
  Counts counts_;
  Limits caps_;
  std::uint32_t window_epoch_ = 0;
  std::uint64_t window_ms_ = 0;
  bool window_open_ = false;  // a counted action has begun a window

  // The stop (rung 5).
  bool stopped_ = false;
  StopReason stop_reason_ = StopReason::kNone;
  std::uint8_t stopped_after_ = 0;
  std::uint64_t stopped_ms_ = 0;

  // The radio's restarts, for the boot loop.
  std::uint64_t boot_ms_ = 0;  // the clock at begin(): this Walter's own boot, near enough
  std::uint64_t restart_window_ms_ = 0;
  std::uint32_t restarts_ = 0;
  std::uint32_t restarts_total_ = 0;
  bool restart_seen_ = false;
  std::uint32_t boot_id_ = 0;
  bool boot_id_known_ = false;
  std::uint8_t reset_reason_ = 0;
  // The two STATUS readings a boot loop shows in: the last uptime and when it was asked.
  std::uint32_t last_uptime_s_ = 0;
  std::uint64_t last_status_ms_ = 0;
  bool last_uptime_known_ = false;
  bool short_uptime_twice_ = false;

  // The battery's last good reading.
  bool battery_known_ = false;
  std::uint16_t battery_mv_ = 0;
  std::uint64_t battery_at_ms_ = 0;

  // The climb in hand.
  Cause cause_ = Cause::kNone;
  Rung rung_ = Rung::kNone;     // the rung last done or refused
  Rung next_rung_ = Rung::kNone;  // the rung the next kClimbing will try
  std::uint64_t wait_until_ms_ = 0;
  std::uint32_t entries_ = 0;
  std::uint32_t first_seq_ = 0;
  bool by_hand_ = false;
  bool flash_started_ = false;

  std::uint64_t next_ask_ms_ = 0;
  bool asked_once_ = false;

  // The slot awaits its first install (task p5-03).
  bool awaiting_first_install_ = false;
  std::uint32_t awaiting_since_seq_ = 0;

  Incident incidents_[kIncidentsKept];
  std::size_t incident_count_ = 0;
  Attention attentions_[kAttentionsKept];
  std::size_t attention_count_ = 0;
  std::uint32_t attentions_dropped_ = 0;
  std::uint32_t events_dropped_ = 0;  // queued events that are nobody's business here (§4)
};

}  // namespace supervisor
}  // namespace core
