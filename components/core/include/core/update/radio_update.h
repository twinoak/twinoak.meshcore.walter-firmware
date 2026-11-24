// The radio's update (task p2-03): spec/SYSTEM.md S-5, S-7 and §6, docs/ARCHITECTURE.md §7,
// docs/update.md. The procedure that replaces the radio's firmware and cannot leave the unit
// worse off than it found it: flash the candidate, make it prove itself, keep it - or put the
// golden image back and never try that candidate again. Every state is written to the journal
// (core/update/journal.h) before its action starts, so a Walter that loses power in the middle
// picks up where it was (resume()) and never guesses.
//
//   idle ─► check ─► flashing ─► booting ─► proving ─► committing ─► done
//             │          │           │          │  └─► node-fault (the image is fine, the node is not)
//             │          └───────────┴──────────┴──► rolling-back ─► (booting, proving) ─► restored
//             └─► refused (a precondition; nothing was touched)              └──► stuck
//
// A state machine over interfaces - the image store, the flasher, the link client, the journal, a
// clock, the battery - one step per call, so that the wake cycle of phase 3 can run it in slices
// and every transition is testable on the host (host_test/core/update/). It is logic: no ESP-IDF
// include, no heap.
//
// The same machine, with another source and no promotion, is the switch to another role and
// back (flashRole): `rak role scanner` flashes the stored scanner image and proves only that
// something that calls itself a scanner is there (the text-mode detection of task p1-02);
// `rak role repeater` puts the golden image back and proves it as an update would.
//
// -- the health check (proving), all of ---------------------------------------------------------
//
//   1. HELLO answers, and its fw_version, role and board are the flashed image's descriptor's
//      (spec/FIRMWARE-MANIFEST.md §4: "After a flash, the Walter's health check holds the HELLO
//      it gets against the descriptor it flashed"), and its link major is one this Walter speaks
//      (the client refuses a HELLO for a major it does not speak: kRefused);
//   1a. **the node is there, and not refusing** (task p10-03): one IDENTITY. A radio that has
//      lost its identity answers it with ERR_STATE and advertises caps 0x21, and *that is not a
//      failed image* - the image booted, answered and said what it is. It ends the run in
//      kNodeFault: no rollback is spent, the candidate's hash goes on no bad list, the journal
//      says why, and a person's `erase` and `reboot` at the radio are the way out
//      (core/link/radio_identity.h; the repeater firmware's docs/flashing.md §3a). A radio with
//      **no** identity yet is not a fault at all: a first install is exactly that, and the check
//      goes on;
//   2. the clock can be set: TIME_SET accepted (spec/LINK-PROTOCOL.md §5.4, the first thing a
//      healthy repeater must take). A Walter with no time to give (interfaces::WallClock answers
//      false until phase 3's modem) cannot make this check; it is recorded as skipped, not as a
//      failure, and TIME_GET must answer instead;
//   3. STATUS says the radio initialised (§5.3 flag bit 4);
//   4. the link stays up - a PING every kProvingPingIntervalMs - for kProvingPeriodMs, with no
//      restart in it (boot_id unchanged, no EV_BOOT);
//   5. where the role has telemetry (HELLO's POWER capability), POWER_GET answers.
//
// Anything else is a failure, and a failure ends in rolling back: the candidate's hash goes on
// the store's bad list FIRST, then the golden image is flashed, booted and proved by the same
// check with its own descriptor. If the golden image does not come back either: stuck - the
// supervisor's last rung, a report, and no further attempt by this machine.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/flasher/flasher.h"
#include "core/image/crypto/sha256.h"
#include "core/image/descriptor.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/update/battery.h"
#include "core/update/journal.h"
#include "interfaces/clock.h"

namespace core {
namespace update {

// -- the windows and thresholds: named, with a sentence each; TO BE SET ON THE BENCH ----------

// From the flasher's reset until the radio must have answered HELLO (or sent EV_BOOT). A
// repeater boots in a few seconds; twenty is room for a bootloader that checks its settings page
// and a SoftDevice that takes its time.
constexpr std::uint32_t kBootWindowMs = 20000;

// How often HELLO is tried inside that window. One try is up to three seconds of silence (§3's
// three attempts), so this is the pause between tries, not their period.
constexpr std::uint32_t kBootHelloIntervalMs = 2000;

// The proving period: how long the link must stay up after a good start, with no restart in it.
// Minutes, not seconds - a candidate that boots, answers and then restarts itself two minutes
// later has failed. Three minutes is longer than any boot-time self-test a repeater runs and
// shorter than a wake cycle can afford twice.
constexpr std::uint32_t kProvingPeriodMs = 180000;

// A PING every few seconds through the proving period. Three seconds: a restart is noticed
// within one interval, and sixty pings over the period are nothing to the link.
constexpr std::uint32_t kProvingPingIntervalMs = 3000;

// For the scanner's proof: how long after the reset NoiseScope must have shown itself (a HELLO
// unanswered three times and a line beginning with `{`, task p1-02). The scanner prints its
// `info` line at boot; twenty seconds is the same room a repeater gets.
constexpr std::uint32_t kScannerWindowMs = 20000;

// **The look, and the window that begins again** (task p18-04). A flash left by a pulse on the
// reset line gets no confirmation that the chip reset; an empty boot window is the first sign
// that it did not, and the chip is asked once (Flasher::startAgain()). Where the core is still
// halted where the flash left it, the image is started through the debug port and the radio gets
// a second window of exactly the same length to answer in. One look a flash, never two: a second
// empty window is the ending it always was.
constexpr std::uint32_t kStartAgainWindowMs = kBootWindowMs;

// **The watchdog a scanner leaves behind** (task p18-04, deliverable 4). The nRF52840's WDT
// cannot be stopped and is not cleared by a soft reset (PS 5.3.6.8's "Reset behavior" table marks
// the WDT column for a watchdog, a pin, a brownout and a power-on reset, and for nothing else),
// and the scanner starts it at WDT_TIMEOUT_S = 60 and feeds it from its own loop (the repeater
// firmware's src/roles/scanner/config.h at commit eaa19a7; its comment: "which survives the soft
// reset into the bootloader with the same timeout"). So an image the **debug port** started over
// a scanner inherits that watchdog, feeds nothing, and is reset once inside its time - which is
// exactly the fault of 21 November, with the reset coming 59 s into the proving period and a
// restart there being a failure by rule (task p18-01).
//
// The proof therefore waits: after such a flash the proving period begins at the boot that
// follows the watchdog's reset, or when this long has passed since the image started without one.
// It does not forgive a restart - the proving period is what it always was once it begins.
//
// Sixty seconds and a quarter of it: the number is the scanner's, and the margin is for the boot
// the watchdog's reset begins and for a clock that is not the radio's. The scanner does not say
// its watchdog's seconds on the link yet (tasks/phase-18/FOLLOWUPS.md), so this is a constant
// copied from that repository and not a reading, and it is the one place it is written down.
constexpr std::uint32_t kScannerWatchdogMs = 60000;
constexpr std::uint32_t kScannerWatchdogWaitMs = kScannerWatchdogMs + kScannerWatchdogMs / 4;

// **The pause between the two tries at a flash that wrote nothing** (task p18-05, deliverable 3).
// Not a reset, not a power cycle and not a reconnect: on 23 November the scanner's flash failed
// before it had written anything and the golden image's flash met the same chip two seconds later
// and went through, so whatever was in the way cleared itself in seconds. What a pause is for is
// the flash controller: an application that erases one of its own pages holds the bus for
// tERASEPAGE, 85 ms typical (PS "NVMC > Electrical specification"), and a settings write is a few
// more. Five seconds is that with room, it is inside every bound a caller gives a flash
// (`kOnePassMs` below counts it), and it is short enough that a radio off the mesh is not off it
// any longer for this.
constexpr std::uint32_t kFlashAgainPauseMs = 5000;

// **What a flash that wrote nothing costs before it gives up**, which is what makes the second
// try affordable. Every way out before the first write is either a bounded wait of milliseconds -
// the debug port's two connect attempts and ten WAIT retries a transfer (`core/swd/dp.h`), the
// power-up's hundred polls (100 ms), the chip's identity, the flash controller's 300 ms
// (`nrf52::kErasePageTimeoutMs`) - or **the scan**, which is the only long one: every page the
// image covers read once, 2048 SWD transfers a page and about 0.9 s at 125 kHz, so the 424 kB
// image of 23 November is 106 pages and a little over a minute and a half. Three minutes is that
// with room for a bigger image and for the idle pauses between the reads.
//
// So a pass that makes a second try is the first try (at most this), the pause, the window the
// radio gets to say HELLO when the first try had halted it, and then a whole flash.
constexpr std::uint32_t kFlashWroteNothingMs = 3u * 60 * 1000;

// The battery below which no update starts. The pack is a single Li-ion cell of 10 Ah (the
// hardware repository's CURRENT-SENSE.md: CV 4.200 V, precharge below 3.1 V): 3.7 V is a cell
// with more than half its charge, enough that two MCUs awake for ten minutes of flashing and
// proving cannot brown it out. The battery is read over the link (POWER_GET, channel 2).
//
// **Ten minutes was a guess, and the unit has since said what it really is** (task p14-04): the
// two updates meshmaster.dk has driven took `check` to `done` in 17 min 51 s (13 November,
// 0.1.2 -> 0.1.4, job 28560) and 17 min 58 s (14 November, 0.1.4 -> 0.1.5, job 29475), of which
// the flash alone was 14 min 44 s and 14 min 51 s - and a run that rolls back is that twice, near
// forty minutes with both MCUs awake. Whether 3.7 V is still the right floor for a unit awake
// that long is Thomas's to decide with a meter on the pack (tasks/phase-14/FOLLOWUPS.md); the
// number is unchanged here. kFlashWindowMs below is where those runs are written down.
constexpr std::uint16_t kBatteryMinMv = 3700;

// "Not falling fast": a reported trend steeper than this refuses the update. 100 mV per hour on a
// Li-ion cell is a load the panel is not covering at all; phase 3's power ring supplies the
// trend, and until then the reading has none and this does not apply.
constexpr std::int16_t kBatteryFallingMvPerHour = -100;

// What a caller that drives the machine to completion sleeps between steps that only waited
// (the console's `rak update`). A tenth of a second keeps the proving's PING cadence honest
// without spinning.
constexpr std::uint32_t kStepIdleMs = 100;

// How many journal entries one run of this machine may write before something is wrong: the
// bound protect() holds the journal to (core/update/journal.h). A good path is 6 entries (check,
// flashing, booting, proving, committing, done), a rollback 7 (check, flashing, booting,
// rolling-back, its booting, its proving, restored); each resume adds one. Measured by
// host_test/core/update/test_radio_update_power_loss.cpp.
constexpr std::uint32_t kEntriesPerRunMax = 48;

// -- what a run takes, from two measured runs: the bound a caller has to give one ----------------
//
// **These are measurements and not guesses** (task p14-04), which is why they are not in the
// section above and have no row in docs/bench.md's *What to bring back*. The unit at Bellevue has
// been updated from meshmaster.dk twice and its own journal says what it cost:
//
//   | | 13 Nov, 0.1.2 -> 0.1.4 (job 28560) | 14 Nov, 0.1.4 -> 0.1.5 (job 29475) |
//   | flashing the candidate | 14 min 44 s | 14 min 51 s |
//   | booting                | 3 s         | 3 s         |
//   | proving                | 3 min 03 s  | 3 min 03 s  |
//   | `check` to `done`      | 17 min 51 s | 17 min 58 s |
//
// The three windows above were written before anybody knew that, and a caller that runs this
// machine inside something bounded - the wake cycle's step, an apply claimed from the server -
// needs one number to bound it by. These are it, and they are arithmetic over the windows: change
// a window and they follow.

// What one flash of the radio's application region costs, end to end. The two runs were seven
// seconds apart for an image seven seconds bigger, so the figure is not the debug port's clock
// alone and the next image will be longer again: sixteen minutes is those runs with room for the
// growth, and it is the number to raise when a release flashes slower than this.
constexpr std::uint32_t kFlashWindowMs = 16u * 60 * 1000;

// One pass of the machine: a flash, the boot window after the reset, and the proving period -
// **and, since task p18-04, the look and the window that begins again** (kStartAgainWindowMs),
// because on a unit whose reset line does not reach its radio that second window is spent at
// every flash and not as an exception. `check` and `committing` are a store read and seven flash
// operations and cost nothing beside it.
// **And since task p18-05 the second try**, which is a pass's worst case and not an exception: a
// flash that wrote nothing is made once more, and a pass that does that spends the first try
// (`kFlashWroteNothingMs`), the pause, the boot window the radio gets to answer in when the first
// try had halted it and started it again, and then the flash. A flash that gets as far as writing
// is never tried again, so this is added to the flash window rather than doubling it.
constexpr std::uint32_t kSecondTryMs = kFlashWroteNothingMs + kFlashAgainPauseMs + kBootWindowMs;

constexpr std::uint32_t kOnePassMs =
    kFlashWindowMs + kSecondTryMs + kBootWindowMs + kStartAgainWindowMs + kProvingPeriodMs;

// **And a whole run, which is a rollback**: the candidate flashed, booted and failed at the end of
// its proving period, then the golden image flashed, booted and proved by the same check. Two
// passes, 38 min 40 s - and *that*, not one pass, is what a caller must allow an apply, because a
// rollback is the path this machine exists for.
constexpr std::uint32_t kWholeRunMs = 2u * kOnePassMs;

// -- the states ---------------------------------------------------------------------------------

enum class State : std::uint8_t {
  kIdle = 0,
  kCheck = 1,            // the preconditions; nothing is touched
  kFlashing = 2,         // the candidate (or the role's image) through the flasher
  kBooting = 3,          // the radio was reset by the flasher: waiting for HELLO or EV_BOOT
  kProving = 4,          // the health check
  kCommitting = 5,       // the store's promotion
  kDone = 6,             // terminal: the candidate is golden (or the role is switched)
  kRollingBack = 7,      // the hash on the bad list, then the golden image flashed
  kRollbackBooting = 8,  // the golden image was reset into: waiting for HELLO
  kRollbackProving = 9,  // the golden image's health check
  kRestored = 10,        // terminal: the golden image is back and proved
  kStuck = 11,           // terminal: the golden image did not come back, or there was none
  kRefused = 12,         // terminal: a precondition failed at check; nothing was touched
  // Terminal: the image proved what an image can and the **node** is the fault - today that is
  // one thing, a radio that has lost its identity and refuses to make a key (task p10-03). No
  // rollback was spent and nothing is on the bad list, so the same candidate may be flashed
  // again once a person has typed `erase` and `reboot` at the radio.
  kNodeFault = 13,
};

const char* describe(State state);
bool isTerminal(State state);

// What a run is for.
enum class Kind : std::uint8_t {
  kUpdate = 0,        // the candidate becomes golden if it proves itself
  kFirstInstall = 1,  // the same, with no golden image to return to (a person's console command)
  kRoleScanner = 2,   // the stored scanner image flashed; proven only as "a scanner is there"
  kRoleRepeater = 3,  // the golden image flashed back and proved
};

const char* describe(Kind kind);

// Why a run was refused, rolled back or stuck. Each a named refusal, as the task asks.
enum class Reason : std::uint8_t {
  kNone = 0,
  // -- refusals at check: nothing was touched
  kNoStore = 1,               // this build has no image store
  kNoProfile = 2,             // no board profile for the slot
  kNoCandidate = 3,           // the store holds no candidate (or no scanner image)
  kImageUnreadable = 4,       // the stored image does not verify against its record (flash rot)
  kOnBadList = 5,             // this hash failed a health check once; never again
  kWrongRole = 6,             // the image's role is not one this slot may hold
  kWrongBoard = 7,            // the image's board is not the slot's
  kWrongBand = 8,             // the image's band is not the golden image's
  kRadioUpdateInProgress = 9, // the journal says a radio update is in progress: resume() first (S-7)
  kSelfUpdateUnconfirmed = 10,// the Walter's own update is journalled as unconfirmed (S-7)
  kLinkDown = 11,             // no HELLO answered and the caller did not say the link is known down
  kBatteryUnknown = 12,       // the battery could not be read
  kBatteryLow = 13,           // below kBatteryMinMv
  kBatteryFalling = 14,       // falling faster than kBatteryFallingMvPerHour
  kNoGolden = 15,             // no golden image and not a first install
  kBusy = 16,                 // the machine is not idle
  // -- failures while flashing, booting or proving
  kFlashFailed = 32,          // the flasher did not finish (its step is in `detail`)
  kNoBoot = 33,               // no HELLO (and no EV_BOOT) inside kBootWindowMs
  kWrongVersion = 34,         // HELLO's fw_version is not the descriptor's
  kRoleMismatch = 35,         // HELLO's role is not the descriptor's
  kBoardMismatch = 36,        // HELLO's board is not the descriptor's
  kLinkMajor = 37,            // the radio refused HELLO for its major (or answered one we do not speak)
  kTimeRefused = 38,          // TIME_SET refused (or TIME_GET unanswered, when there is no time)
  kNotInitialised = 39,       // STATUS does not say the radio initialised
  kRestarted = 40,            // a restart inside the proving period
  kSilence = 41,              // a PING unanswered inside the proving period
  kNoTelemetry = 42,          // POWER_GET unanswered though the role advertises it
  kNotAScanner = 43,          // nothing that calls itself a scanner appeared
  kJournal = 44,              // the journal could not be written: the machine stops where it is
  kStore = 45,                // the store refused the promotion or the bad list
  kNoImageToReturnTo = 46,    // a first install failed: stuck, there is no golden image
  // -- the node, not the image
  kIdentityLost = 47,         // IDENTITY is ERR_STATE: the radio refuses, and no image mends it
  // -- **what the look and the wait of task p18-04 found**. None of these ends a run: they are
  // what a `booting` entry says about how the image was started and what the proof is waiting
  // for, so that the journal tells the story a person has to read off a unit whose reset line
  // may or may not reach its radio.
  kStartedThroughPort = 48,   // the look found the core halted: the pulse reached nothing
  kPulseStarted = 49,         // the look found the core running: the pulse worked, the image is quiet
  kLookFailed = 50,           // the look could not be made: the port was closed or silent
  kAfterTheWatchdog = 51,     // the proof began at the boot that followed the scanner's watchdog
  kWatchdogTimePassed = 52,   // ... or when its longest time had passed with no reset
  // -- **a flash that wrote nothing** (task p18-05). The first of the two is a line of the story
  // written in `flashing` before the second try; the second ends the run, and the ending is
  // `refused` - nothing was touched, so there is nothing to put back and nothing to blame the
  // image for. Both carry the failure's bits in `detail` (`FlashFailure` below).
  kFlashOnceMore = 53,        // the flash did not begin: the same flash is made a second time
  kFlashDidNotBegin = 54,     // twice, and nothing was written: the radio is as it was
};

const char* describe(Reason reason);

// Which step of the health check is being made or failed, for the story.
enum class ProveStep : std::uint8_t {
  kNone = 0,
  kHello = 1,
  kTime = 2,
  kStatus = 3,
  kPing = 4,
  kPower = 5,
  kScanner = 6,
  kPassed = 7,
  // The numbers are the journal's, so they only ever grow: check 1a came after the rest (task
  // p10-03) and is made between kHello and kTime.
  kIdentity = 8,
};

const char* describe(ProveStep step);
// The same step as one word, for a journal note that carries a reason beside it (task p18-06).
const char* name(ProveStep step);

// What the caller asks for.
struct Request {
  Kind kind = Kind::kUpdate;
  board::Slot slot{};
  // The supervisor (task p2-05) has classified the link as down for a reason it knows, and says
  // so: the link precondition then passes without a HELLO. A first install implies it: there is
  // no application on the radio to answer.
  bool link_known_down = false;
};

// -- the journal entry's payload ----------------------------------------------------------------
//
// Every entry of kind::kRadioUpdate carries this, 48 bytes: the state entered, what the run is
// for, the image it is about (the candidate's, the scanner's or - for a switch back - the golden
// image's SHA-256), and on a terminal state why. resume() reads the newest one; the console's
// `rak update status` reads them back as a story.
constexpr std::size_t kPayloadBytes = 48;
constexpr std::uint8_t kFlagLinkKnownDown = 0x01;
constexpr std::uint8_t kFlagResumed = 0x02;  // this entry was written by resume(), after a restart
// **The fault was the unit's and not the image's** (task p18-05, deliverable 5): a debug port
// that gave up, a page that would not take, a core that would not halt. The bad list does not
// take the image, and because a resumed run reads this flag back out of the journal it does not
// take it at the resume either.
constexpr std::uint8_t kFlagUnitsFault = 0x04;

struct Payload {
  State state = State::kIdle;
  Kind kind = Kind::kUpdate;
  std::uint8_t slot = 0;
  Reason reason = Reason::kNone;
  ProveStep step = ProveStep::kNone;
  std::uint8_t flags = 0;
  std::uint8_t sha256[image::kSha256Size] = {};
  std::uint32_t detail = 0;  // the flasher's step on kFlashFailed; the proving's elapsed ms on kRestored/kDone
};

void encode(const Payload& payload, std::uint8_t out[kPayloadBytes]);
bool decode(const Entry& entry, Payload* out);  // false unless the entry is a radio-update entry

// **What the `booting` entry's `detail` carries** (task p18-04): what started the image this flash
// wrote, and what `UICR.PSELRESET` said while the port was open for it. A payload is 48 bytes and
// full; this is the one field a `booting` entry had spare, and it is what makes the journal say -
// after the fact, on a unit nobody was watching - which of the two started the image and whether
// the module has a reset pin at all. `rak update status` prints it.
struct StartRecord {
  flasher::Started started = flasher::Started::kNothing;
  bool uicr_read = false;
  bool pin_enabled = false;
  std::uint8_t pin = 0;
  std::uint8_t port = 0;
};
std::uint32_t encodeStart(const StartRecord& record);
StartRecord decodeStart(std::uint32_t detail);

// **What a failed flash's `detail` carries** (task p18-05, deliverable 2). Until this task the
// whole of it was `static_cast<std::uint32_t>(r.step)` - the flasher's step, written by
// `stepFlashing` and printed nowhere, which is why nobody could say what the flash of
// 23 November 11:37:43Z had failed at. The thirty-two bits now hold the step, the status of the
// layer under it, the address when there is one, and whether the chip was changed:
//
//   bits [5:0]    the flasher's step (`flasher::Step`, twenty values)
//   bits [11:6]   the nRF52 layer's status (`nrf52::Status`, fourteen values)
//   bits [15:12]  the debug port's status (`swd::Status`, ten values); meaningful when the
//                 nRF52's is `kSwd`, zero otherwise
//   bit  16       **nothing was written**: the chip is as the flash found it
//   bit  17       the core was halted, and so had to be started again
//   bit  18       refused (a check this flasher makes) rather than failed (the chip, the port
//                 or the lines)
//   bit  19       this was the **second** try at the same flash
//   bits [27:20]  the page the address lies in (`address >> 12`; the chip is 1 MB in 4 KB pages)
//   bit  28       there is an address
//   bits [30:29]  spare, written zero
//   bit  31       **this detail carries the layout above**
//
// Bit 31 is what makes an older entry read as it did: every `kFlashFailed` entry written before
// this task has it clear and a whole detail under 32, which `decodeFlashFailure` returns as a
// step and nothing else (`detailed` false), and `flashFailureLine` then says the step alone -
// exactly what such an entry has always meant.
struct FlashFailure {
  flasher::Step step = flasher::Step::kNone;
  nrf52::Status status = nrf52::Status::kOk;
  swd::Status swd = swd::Status::kOk;
  bool wrote_nothing = false;
  bool core_halted = false;
  bool refused = false;
  bool second_try = false;
  bool address_known = false;
  std::uint32_t address = 0;  // the page the failure named, to the page
  bool detailed = false;      // bit 31: this came from an entry of task p18-05 or later
};
std::uint32_t encodeFlashFailure(const FlashFailure& failure);
FlashFailure decodeFlashFailure(std::uint32_t detail);
// The same, straight off a flasher result (`second_try` is the caller's to set).
FlashFailure failureOf(const flasher::Result& result);

// The longest line either of the two below writes, with its terminator. The long form's worst
// case is the second try at `kSettingsInvalidate` with a `kSwd` status whose debug-port phrase is
// the longest there is: 250 characters, which is more than the console's own printf buffer holds -
// so the console prints it with `print()` and not with a format (task p18-05).
constexpr std::size_t kFlashFailureLineMax = 256;

// **One phrase for a person**, for the journal's line at the console, `rak update status` and a
// report: `while reading the pages the image covers: the debug port failed (no target: nothing
// answered on the line) at 0x00027000; nothing was written`. Returns the characters written.
std::size_t flashFailureLine(const FlashFailure& failure, char* out, std::size_t cap);
// **The same in the words seventy characters hold**, for the note of the line that goes to the
// server with a job, so that the job's page says what failed without a report being asked for:
// `scan: swd no-target at 0x027000; nothing written`.
std::size_t flashFailureNote(const FlashFailure& failure, char* out, std::size_t cap);

// -- what the Walter's own update writes (task p2-04, core/update/self_update.h) ------------------
//
// S-7: the machine refuses to start while the Walter's own update is journalled as unconfirmed.
// Byte 0 of a kind::kSelfUpdate entry's payload is its state; the values are shared here so that
// this machine and the self-update cannot disagree about which of them mean "unconfirmed".
namespace self_update {
constexpr std::uint8_t kReceived = 1;    // an image passed the six checks into the other slot
constexpr std::uint8_t kSwitching = 2;   // the other slot was made the next to boot: a trial begins at the restart
constexpr std::uint8_t kOnTrial = 3;     // the new image booted pending-verify and is running its trial
constexpr std::uint8_t kConfirmed = 4;
constexpr std::uint8_t kRolledBack = 5;
// Switching and on trial: the Walter's own update is in the air, and the radio's must wait.
inline bool unconfirmed(std::uint8_t state) { return state == kSwitching || state == kOnTrial; }
}  // namespace self_update

// -- the machine's answer to one step ----------------------------------------------------------

struct StepResult {
  State before = State::kIdle;
  State after = State::kIdle;
  bool changed = false;   // a transition happened
  bool waited = false;    // nothing to do yet (a window or an interval has not passed)
  Reason reason = Reason::kNone;
  ProveStep step = ProveStep::kNone;
  const char* note = "";  // one phrase for a person, static text
  // What the journal entry of this transition carries (task p18-05): on the three flash reasons
  // the `FlashFailure` above, so that a caller writing the line that goes to the server does not
  // have to read the journal back to say what failed.
  std::uint32_t detail = 0;
};

// What the machine is doing now, for the console.
struct Status {
  State state = State::kIdle;
  Kind kind = Kind::kUpdate;
  board::Slot slot{};
  Reason reason = Reason::kNone;
  ProveStep step = ProveStep::kNone;
  // The candidate's failure, when there was one: why it was rolled back and at which step of the
  // health check. `reason` and `step` follow the machine (after a rollback they are the golden
  // image's proof), these two stay the candidate's, for the console's last line.
  Reason fail_reason = Reason::kNone;
  ProveStep fail_step = ProveStep::kNone;
  std::uint8_t sha256[image::kSha256Size] = {};
  char version[image::kVersionSize] = {};  // of the image being flashed or proved
  std::uint32_t prove_elapsed_ms = 0;
  std::uint32_t pings = 0;
  std::uint32_t entries = 0;  // journal entries this run has written
  bool resumed = false;
  bool time_skipped = false;  // the TIME_SET check could not be made: no time to give
  // Task p18-04: what started the image being proved, whether the chip had to be asked what the
  // pulse did, and whether the proof waited a scanner's watchdog out first.
  flasher::Started started = flasher::Started::kNothing;
  bool looked = false;
  nrf52::CoreState core_at_look = nrf52::CoreState::kUnknown;
  bool waited_for_watchdog = false;
  // **The flash, and what it did** (task p18-05). `fail_detail` is the `FlashFailure` of the
  // flash that ended the candidate's run - the pair of `fail_reason` and `fail_step`, and like
  // them it stays the candidate's across a rollback; `flash_tries` is 1 or 2, so a person can
  // see that a flash was made twice; `units_fault` is why the bad list did not take the image.
  std::uint32_t fail_detail = 0;
  std::uint8_t flash_tries = 0;
  bool units_fault = false;
};

class RadioUpdate final : public link::client::RestartWatcher {
 public:
  RadioUpdate(image::Store& store, flasher::Flasher& flasher, link::client::Client& radio,
              Journal& journal, interfaces::Clock& clock, BatterySource& battery);

  // The flasher's progress goes here (the console prints it). Optional.
  void setProgress(flasher::ProgressSink* progress) { progress_ = progress; }

  // Begins a run: the machine goes to check and journals it. False with reason() when the
  // machine is not idle, or when the journal cannot be written.
  bool start(const Request& request);

  // At start-up: reads the journal and, when the last radio-update entry is not terminal, takes
  // the run up again from the state the journal names - mid-flash continues the flash, booting
  // or proving starts the proving over, committing finishes the promotion, rolling back
  // continues rolling back. True when there was something to resume (the machine is then active
  // and step() drives it). False, and idle, otherwise.
  bool resume();

  // One step. Each call does at most one thing that takes time (a flash, a HELLO, a PING) and
  // returns; a state's action is journalled before it starts.
  StepResult step();

  bool active() const { return !isTerminal(state_) && state_ != State::kIdle; }
  State state() const { return state_; }
  Reason reason() const { return reason_; }
  const Status& status() const { return status_; }
  const Request& request() const { return request_; }
  // After a terminal state the machine stays there until a caller clears it, so that `rak
  // update status` can say what just happened; start() clears it too.
  void clear();

  // link::client::RestartWatcher: a restart of the radio while proving is a failure.
  void onRadioRestart(std::uint32_t boot_id, std::uint8_t reset_reason) override;

 private:
  bool journal(State state, Reason reason = Reason::kNone, ProveStep step = ProveStep::kNone,
               std::uint32_t detail = 0);
  StepResult enter(State next, const char* note, Reason reason = Reason::kNone,
                   ProveStep step = ProveStep::kNone, std::uint32_t detail = 0);
  // `blame_the_image` false is task p18-05's deliverable 5: the fault was the unit's, so the
  // hash does not go on the store's bad list and the flag in the entry says so for the resume.
  StepResult fail(Reason reason, ProveStep step, std::uint32_t detail = 0,
                  bool blame_the_image = true);
  StepResult refuse(Reason reason, std::uint32_t detail = 0);
  // A fault of the node and not of the image (task p10-03): terminal at once, no bad list and no
  // rollback - from the candidate's path and from the golden image's alike, because the one thing
  // a rollback could do about it is flash an image the same file system will refuse as well.
  StepResult nodeFault(Reason reason, ProveStep step, std::uint32_t detail = 0);
  // One §5.6 request, read in the one place that decides the three states
  // (core/link/radio_identity.h): true when the radio **refuses** - it has lost its identity.
  // False for a radio that has no key yet, which is what every first install starts as, and for
  // one that answered a key. `status` takes §3's own byte, for the journal's detail.
  bool nodeRefusesIdentity(std::uint8_t* status);
  // Check 2 has failed. Whether that is the image or the node, and the run ends accordingly.
  StepResult failTime(std::uint32_t detail);
  StepResult waiting(const char* note);

  // A line of the story written in the state the machine is already in: what the look found, and
  // what the proof is waiting for (task p18-04). False when the journal refused, after which the
  // machine stops where it is, as every other journalled step does.
  bool note(Reason reason, ProveStep step, std::uint32_t detail = 0);
  // The same as a step's answer, for a line the server is told about (task p18-05): `changed` is
  // true with `before` and `after` the same state, which is a caller's cue to post the line
  // without reading the journal back.
  StepResult noted(Reason reason, const char* note_text, std::uint32_t detail = 0);
  // The look of task p18-04, made at most once a flash, when a boot window after a pulse ended
  // with nothing from the radio: the chip is asked what the pulse did, and the window begins
  // again where the answer is that it did nothing.
  StepResult lookAtTheCore(bool scanner);
  // **The second try** (task p18-05, deliverable 3), both halves of it: what a flash that wrote
  // nothing leaves behind, and the pause (and the HELLO) that stand between it and the one more
  // try this firmware ever makes.
  StepResult flashWroteNothing(const flasher::Result& r);
  StepResult waitBeforeTheSecondTry();

  StepResult stepCheck();
  StepResult stepFlashing(bool rollback);
  StepResult stepBooting(bool rollback);
  StepResult stepProving(bool rollback);
  StepResult stepCommitting();

  // Which stored image a run flashes and proves: the candidate, the scanner, or the golden one.
  image::Which sourceOf(Kind kind) const;
  bool loadImage(image::Which which, bool for_rollback);
  bool helloMatches(Reason* why);
  void beginProving();

  image::Store& store_;
  flasher::Flasher& flasher_;
  link::client::Client& radio_;
  Journal& journal_;
  interfaces::Clock& clock_;
  BatterySource& battery_;
  flasher::ProgressSink* progress_ = nullptr;

  State state_ = State::kIdle;
  Reason reason_ = Reason::kNone;
  Request request_;
  Status status_;
  std::uint32_t first_seq_ = 0;  // the run's first journal entry: protected from a wrap

  // The image being flashed and proved: the record (its descriptor is what HELLO is held
  // against) and the store's view of its bytes.
  image::Record image_;
  image::StoredImage bytes_;
  bool image_loaded_ = false;
  bool rollback_image_ = false;  // image_ is the golden image (a rollback or a switch back)

  // The booting and proving windows.
  std::uint64_t window_started_ms_ = 0;
  std::uint64_t next_hello_ms_ = 0;
  std::uint64_t next_ping_ms_ = 0;
  std::uint32_t boot_id_ = 0;
  bool restart_seen_ = false;
  ProveStep prove_step_ = ProveStep::kNone;

  // **What started the image, and what is owed because of it** (task p18-04). All four are set
  // at the flash and read by the boot window; none survives the run, and a resumed run makes its
  // own look if its own window ends empty.
  bool looked_ = false;            // the look has been made for this flash: one, never two
  bool can_look_ = false;          // an empty window here is worth a look: the start is unconfirmed
  bool pulse_started_ = false;     // a pulse started this image, so an observation of the line is owed
  bool over_a_scanner_ = false;    // the radio was running a scanner when this flash began
  bool watchdog_owed_ = false;     // the debug port started an image over a scanner's watchdog
  bool watchdog_wait_ = false;     // ... and the radio has answered: waiting that watchdog out
  std::uint64_t started_ms_ = 0;   // when the image was started: the watchdog's time runs from it

  // **The one second try** (task p18-05). All four belong to the flash in hand: `tries_` is 1
  // after the first and 2 after the second, and a third is never made - for a candidate's flash,
  // a scanner's, the golden image going back, a first install's and the supervisor's alike,
  // because every flash this firmware makes comes through `stepFlashing`. None of them survives
  // a restart: a Walter that died between the two tries is resumed at `flashing` and its flash
  // begins again from nothing, which is one flash and may have one second try of its own.
  std::uint8_t tries_ = 0;
  bool again_pending_ = false;       // the pause (and the HELLO) are being waited out
  bool again_wants_hello_ = false;   // the first try halted the radio and started it again
  std::uint64_t again_at_ms_ = 0;    // when the pause is up
  std::uint64_t again_since_ms_ = 0; // when the first try ended: the HELLO's window runs from it
  std::uint32_t flash_detail_ = 0;   // the first try's FlashFailure, for the line before the second
  // **Whose fault the failure was** (deliverable 5), kept through the rollback and read back out
  // of the journal's flag at a resume, so that the bad list is not taken a second time either.
  bool units_fault_ = false;
};

}  // namespace update
}  // namespace core
