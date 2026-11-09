// The Walter's own update (task p2-04): spec/SYSTEM.md S-6 and S-7, docs/ARCHITECTURE.md §7,
// docs/update.md. A new image of this firmware goes into the other application slot through the
// same six checks a radio image passes (spec/FIRMWARE-MANIFEST.md §5), is made the next to boot,
// and then has **one boot** to prove it can do the unit's job: reach its main loop with the board
// in the safe state, exchange HELLO with the radio, and complete one exchange with the backend
// over the uplink. All three inside the trial window and it confirms itself; otherwise it rejects
// itself and the bootloader returns to the image that worked. "No good" is not "it crashed": a
// Walter that boots perfectly and cannot attach to the network is as lost as one that does not
// boot, and this is what makes that image roll back by itself.
//
//   self receive ─► received ─► self switch ─► switching ─► (restart) ─► on trial ─► confirmed
//                                                                            │
//                                                 the window passes, a condition fails, a reset
//                                                                            ▼
//                                                                       rolled back
//
// Everything is journalled (core/update/journal.h, kind::kSelfUpdate) before it is done, so that
// the image that boots *after* a restart - whichever of the two it is - can read what was in the
// air: the old image finding *switching* or *on trial* in the journal knows the new one was
// tried and is gone, writes the rollback entry with the reason, and puts the new image's hash on
// the bad list, so it is refused if offered again. The radio's update reads the same entries to
// keep S-7: never both at once.
//
// -- the trial's three conditions ---------------------------------------------------------------
//
//   1. the main loop, with the board in the safe state: main/ calls mainLoopReached() on its
//      first turn, after RadioPins has put the seven pins where they belong;
//   2. HELLO exchanged with the radio - or the radio was in a state the unit already knew about
//      before the update, journalled in the *switching* entry (the switch asks HELLO and records
//      whether it was answered; a caller that has classified the radio as down says so), so that a
//      dead radio does not condemn a good Walter image, while a radio that answered before the
//      update and is silent after it does;
//   3. one exchange with the backend over the uplink (core/update/uplink_check.h). Until phase 3
//      the uplink is an interface whose firmware implementation says no server is configured, and
//      the condition is then *not applicable*: that is the rule that keeps a factory image, or
//      any unit that has never been given a server, from rejecting itself for want of one.
//
// -- what the platform gives ---------------------------------------------------------------------
//
// interfaces/own_update.h: the two slots and ESP-IDF's rollback states. The bootloader's rule
// that a slot still pending-verify at a reset is aborted is what makes "a trial is one boot"
// true whatever the reason for the reset - this machine only has to notice, at the next boot,
// that the trial it journalled did not end, and write down why.
//
// No ESP-IDF include, no heap: one SelfUpdate is a few hundred bytes of state and a 256-byte
// read buffer, and belongs in static storage.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/health/coredump.h"
#include "core/health/health.h"
#include "core/image/crypto/sha256.h"
#include "core/image/descriptor.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/verdict.h"
#include "core/link/client/client.h"
#include "core/update/battery.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/uplink_check.h"
#include "interfaces/clock.h"
#include "interfaces/own_update.h"
#include "interfaces/restart.h"

namespace core {
namespace update {

// -- the windows: named, with a sentence each; TO BE SET ON THE BENCH -----------------------------

// From the trial image's main loop until all three conditions must hold, or the image rejects
// itself. Long enough for a slow LTE attach (a cold attach with a network search can take two
// or three minutes; QuickSpot's own examples wait up to that), short enough that a unit on a
// mast with a bad image is back on the old one before its wake is over. Five minutes.
constexpr std::uint32_t kTrialWindowMs = 300000;

// How often HELLO is tried during the trial while the radio has not answered. One try is up to
// three seconds of silence (§3's three attempts); two seconds between tries.
constexpr std::uint32_t kTrialHelloIntervalMs = 2000;

// How long after a failed uplink attempt the next is made. An attach that failed is not retried
// at once; half a minute lets the modem's own backoff run.
constexpr std::uint32_t kTrialUplinkRetryMs = 30000;

// What SLEEP_NOTICE tells the radio before the restart into the new image (spec/LINK-PROTOCOL.md
// §8: expect_back_s): a Walter boot and a HELLO, with room. The radio holds its events meanwhile.
constexpr std::uint32_t kSwitchExpectBackS = 60;

// -- the states, as byte 0 of a kind::kSelfUpdate entry -------------------------------------------
//
// The numbers are radio_update.h's self_update:: constants, shared so that the radio's machine
// reads "unconfirmed" off the same values (S-7).

enum class SelfState : std::uint8_t {
  kNone = 0,
  kReceived = self_update::kReceived,      // an image passed the six checks into the other slot
  kSwitching = self_update::kSwitching,    // the other slot is the next to boot; the restart follows
  kOnTrial = self_update::kOnTrial,        // this boot is the new image's one trial
  kConfirmed = self_update::kConfirmed,    // all three conditions: the image is valid
  kRolledBack = self_update::kRolledBack,  // the new image is rejected and its hash on the bad list
};

const char* describe(SelfState state);

// Why something was refused, or why an image was rolled back. Each a named refusal; each
// rollback reason its own journal line.
enum class SelfReason : std::uint8_t {
  kNone = 0,
  // -- refusals: nothing was done
  kNoSlots = 1,               // the platform cannot say which slot runs (no OTA data, one slot)
  kRunningUnconfirmed = 2,    // the running image is itself on trial: never stack an update on one
  kRadioUpdateInProgress = 3, // the journal says the radio's update is in progress (S-7)
  kTransferRunning = 4,       // a transfer is open on the other slot
  kBatteryUnknown = 5,
  kBatteryLow = 6,            // below kBatteryMinMv (radio_update.h): a brown-out in the trial is a wasted attempt
  kBatteryFalling = 7,
  kNothingReceived = 8,       // self switch with no received image journalled
  kOtherSlotChanged = 9,      // the other slot does not hold the received image any more
  kLinkDown = 10,             // the radio does not answer and nobody has said why
  kNotOnTrial = 11,           // self confirm / self reject when the running image is not on trial
  kPlatform = 12,             // the platform refused (the other slot, the boot pointer, the rollback)
  kJournal = 13,              // the journal could not be written: nothing is done without it
  // -- refusals of an image offered to the other slot (self receive), after the six checks' first three
  kOnBadList = 20,            // this image failed its trial once and is never tried again
  kTooLarge = 21,             // the file is larger than the other application slot
  kLoadAddress = 22,          // the file's load_address is not an application slot's offset
  kLinkMajor = 23,            // speaks another link major than the radio and does not say it tolerates the radio's (S-7)
  kAlreadyRunning = 24,       // the file is the running image itself
  // -- rollback reasons: which of the three was missing, or what cut the trial short
  kNoMainLoop = 32,           // (1) the main loop was not reached inside the window
  kNoRadio = 33,              // (2) the radio answered before the update and not after it
  kNoUplink = 34,             // (3) the backend was not reached inside the window
  kRestartedDuringTrial = 35, // the journal says on trial; this boot is the old image again
  kNeverBooted = 36,          // the journal says switching; the new image never reached its main loop
  kSwitchNotTaken = 37,       // the restart came before the other slot was made the next to boot: nothing was tried
  kRejectedByHand = 38,       // self reject on the console
};

const char* describe(SelfReason reason);

// How a trial condition stands.
enum class Condition : std::uint8_t {
  kPending = 0,
  kMet = 1,
  kNotApplicable = 2,  // (2) the radio was known down before the update; (3) no server configured
  kFailed = 3,         // (3) the last attempt failed; tried again until the window passes
};

const char* describe(Condition condition);

// -- the journal entry's payload: 48 bytes -------------------------------------------------------
//
//     0  state    1  slot (the new image's)   2  reason   3  flags
//     4  size     u32: the new image's bytes, so the slot can be hashed again
//     8  hash16   the first 16 bytes of the new image's SHA-256: enough to know it again; the
//                 full hash for the bad list is recomputed from the slot's bytes and held to these
//    24  from     the version switched from, 11 characters and a terminator
//    36  to       the version switched to, the same
//
// Versions are kept as `X.Y.Z` (the part before `+`, which is a commit's seven characters that the
// slot's own application description still carries in full): 12 bytes each is what a 48-byte
// entry leaves beside the hash and the size.
constexpr std::size_t kSelfPayloadBytes = 48;
constexpr std::size_t kHash16 = 16;
constexpr std::size_t kShortVersion = 12;

constexpr std::uint8_t kFlagRadioUp = 0x01;          // switching: the radio answered HELLO before the restart
constexpr std::uint8_t kFlagRadioKnownDown = 0x02;   // switching: the caller said the radio is known down
constexpr std::uint8_t kFlagByHand = 0x04;           // confirmed / rolled back by a console command
constexpr std::uint8_t kFlagUnjournalled = 0x08;     // on trial: the running image was pending-verify with no switching entry
constexpr std::uint8_t kFlagRadioNotApplicable = 0x10;   // confirmed: condition 2 was not applicable
constexpr std::uint8_t kFlagUplinkNotApplicable = 0x20;  // confirmed: condition 3 was not applicable
constexpr std::uint8_t kFlagNotOnBadList = 0x40;     // rolled back: the hash could not be put on the bad list (unknown, or the slot's bytes differ)

struct SelfPayload {
  SelfState state = SelfState::kNone;
  std::uint8_t slot = 0;
  SelfReason reason = SelfReason::kNone;
  std::uint8_t flags = 0;
  std::uint32_t size = 0;
  std::uint8_t hash16[kHash16] = {};
  char from[kShortVersion] = {};
  char to[kShortVersion] = {};
};

void encode(const SelfPayload& payload, std::uint8_t out[kSelfPayloadBytes]);
bool decode(const Entry& entry, SelfPayload* out);  // false unless the entry is a self-update entry

// `X.Y.Z` of a version, into `out` of kShortVersion bytes.
void shortVersion(const char* version, char out[kShortVersion]);

// -- what ended the last trial: a kind::kTrialEvidence entry's payload, 48 bytes ----------------
//
// The *rolled back* entry says **that** the bootloader gave up on the image. This says **why**,
// and it can only be written at the boot that makes the finding: the reset that ended the trial
// is the reset that started this boot, and the next wake's reset reason says nothing about it
// (task p11-03; tasks/phase-11/evidence/README.md is the trial this was written for).
//
//     0  reason   the reset reason of this boot, as health::ResetReason (kPanic, 0.1.3's)
//     1  dump     what the `coredump` partition held, as health::DumpState
//     2  flags    kEvidence*
//     3  task     13 bytes: the crashed task's name, 12 characters and a terminator
//    16  panic    32 bytes: the panic line, 31 characters and a terminator
//
// **The panic line is cut here, and that is what 48 bytes leave.** The line of 9 November is 53
// characters and the entry would have to carry nothing else to hold it, so the journal keeps as
// much as fits (with kEvidencePanicCut set) and the whole line is read back out of the dump,
// which nothing erases - SelfUpdate::lastTrial() is where the two are put together, and it is
// what the console, the apply's result and `twinoak.fw.state` all print.
constexpr std::size_t kEvidencePayloadBytes = 48;
constexpr std::size_t kEvidenceTask = 13;   // 12 characters and a terminator
constexpr std::size_t kEvidencePanic = 32;  // 31 characters and a terminator

constexpr std::uint8_t kEvidenceAttributed = 0x01;  // the dump is the trial image's own
constexpr std::uint8_t kEvidencePanicCut = 0x02;    // the panic line did not fit this entry
constexpr std::uint8_t kEvidenceTaskKnown = 0x04;   // the crashed task's name was read

struct TrialEvidence {
  health::ResetReason reason = health::ResetReason::kUnknown;
  health::DumpState dump = health::DumpState::kNoRegion;
  std::uint8_t flags = 0;
  char task[kEvidenceTask] = {};
  char panic[kEvidencePanic] = {};

  bool attributed() const { return (flags & kEvidenceAttributed) != 0; }
  bool panicCut() const { return (flags & kEvidencePanicCut) != 0; }
};

void encode(const TrialEvidence& evidence, std::uint8_t out[kEvidencePayloadBytes]);
bool decode(const Entry& entry, TrialEvidence* out);  // false unless the entry is an evidence entry

// -- the machine's answers --------------------------------------------------------------------

// What begin() found this boot to be.
enum class SelfBoot : std::uint8_t {
  kNothing = 0,         // not on trial, nothing unfinished in the journal
  kTrialBegun,          // this image boots pending-verify (or the journal says it is the one switched to): the trial runs
  kRollbackRecorded,    // the old image again, with a switch or a trial in the journal: the rollback entry was written
  kNoSlots,             // the platform cannot say which slot runs: nothing of this works
  kJournalFailed,       // the entry could not be written; the trial runs on without it (the bootloader still enforces one boot)
};

struct SelfStep {
  bool changed = false;    // a transition happened (on trial -> confirmed / rolled back)
  bool waited = false;     // nothing to do yet
  SelfState after = SelfState::kNone;
  SelfReason reason = SelfReason::kNone;
  const char* note = "";   // one phrase for a person, static text
};

struct SelfStatus {
  SelfState state = SelfState::kNone;    // what this boot is doing or last did
  SelfReason reason = SelfReason::kNone;
  bool on_trial = false;
  std::uint32_t trial_elapsed_ms = 0;
  Condition main_loop = Condition::kPending;
  Condition radio = Condition::kPending;
  Condition uplink = Condition::kPending;
  std::uint32_t hello_tries = 0;
  std::uint32_t uplink_tries = 0;
  SelfPayload trial;  // the on-trial entry's payload: the image's slot, size, hash, versions
};

struct SwitchRequest {
  // The supervisor (task p2-05) has classified the radio as down for a reason it knows, or a
  // person at the console says so (`self switch radio-down`): the radio need not answer HELLO
  // before the switch, and the trial's second condition is not applicable.
  bool radio_known_down = false;
};

class SelfUpdate final : public image::TransferTarget {
 public:
  // `dumps` is the `coredump` partition, read (task p11-03): a pointer, because a build whose
  // partition table has no `coredump` - or a host test that is not about one - has none, and the
  // finding at boot then records the reset reason alone. It is never written to from here.
  SelfUpdate(interfaces::OwnUpdate& slots, image::Store& store, Journal& journal,
             link::client::Client& radio, UplinkCheck& uplink, interfaces::Clock& clock,
             interfaces::Restart& restart, BatterySource& battery, health::Health& health,
             health::CoreDump* dumps = nullptr);

  // At boot, after the pins are safe and the journal is read: is this boot a trial, or does the
  // journal say one was in the air and did not end? Journals what it finds.
  SelfBoot begin();
  // Condition 1: main/ says its loop is turning (the console's first tick).
  void mainLoopReached();
  // One step of the trial: a HELLO when due, the uplink when due, the confirmation or the
  // rejection. Nothing when not on trial.
  SelfStep step();
  bool onTrial() const { return state_ == SelfState::kOnTrial; }
  const SelfStatus& status() const { return status_; }

  // -- the console's commands, each a named refusal or kNone -------------------------------
  // Whether an image may be received now (the preconditions that need no battery: an interrupted
  // write is harmless). The transfer itself goes through Receiver::begin(*this).
  SelfReason canReceive();
  // self switch: the preconditions with the battery, the received image still in the other slot,
  // HELLO (or the caller's word), the *switching* entry, the boot pointer, SLEEP_NOTICE, restart.
  // On a board this does not return when it succeeds.
  SelfReason switchNow(const SwitchRequest& request);
  // self confirm / self reject, for the bench. Both only while on trial.
  SelfReason confirmNow();
  SelfReason rejectNow();

  // -- what the console prints --------------------------------------------------------------
  bool runningSlot(interfaces::OwnSlot* out) { return slots_.running(out); }
  bool otherSlot(interfaces::OwnSlot* out) { return slots_.other(out); }
  // The image received into the other slot and not yet switched to (or whose switch was not
  // taken), from the journal: false when there is none.
  bool received(SelfPayload* out) const;
  // The entry begin() wrote this boot (on trial, or the rollback), for the boot line.
  const SelfPayload& bootEntry() const { return boot_; }
  SelfReason bootReason() const { return boot_.reason; }

  // -- what ended the last trial (task p11-03) -----------------------------------------------
  //
  // The journal's evidence entry, with the **whole** panic line when the dump it was attributed
  // to is still in the partition and is still the other slot's image. Nothing is attributed
  // here: the finding was made at the boot that could make it, and this reads the line again
  // rather than deciding again whose it is. False when the story has no evidence entry, or when
  // a self-update entry is newer than it - the evidence is the last trial's, not an old one's.
  struct LastTrial {
    health::ResetReason reason = health::ResetReason::kUnknown;
    health::DumpState dump = health::DumpState::kNoRegion;  // as the finding boot found it
    bool attributed = false;  // the dump was the trial image's own
    bool dump_now = false;    // and the partition still holds it
    bool panic_cut = false;   // the line printed below is cut (the entry's copy, 31 characters)
    char task[kEvidenceTask] = {};
    char panic[health::kPanicMax + 1] = {};
  };
  bool lastTrial(LastTrial* out);
  // The `coredump` partition itself, for `coredump` on the console: nullptr in a build without
  // one. The console reads and erases through it; nothing else erases it, ever.
  health::CoreDump* dumps() { return dumps_; }
  // This firmware's own descriptor fields: what an offered image must be.
  const image::Fields& self() const { return self_; }

  // -- image::TransferTarget: the other slot as the place a transfer's bytes go --------------
  const image::SlotNeeds& needs() const override { return needs_; }
  image::Use use() const override { return image::Use::kOta; }
  const flasher::BoardProfile* profile() const override { return nullptr; }
  const char* name() const override { return "the other application slot"; }
  // Open from beginWrite() until commit() or abandonWrite(): the platform's own handle closes at
  // endWrite() (esp_ota_end frees it), but the transfer is not over until the verdict has run.
  bool writing() const override { return open_; }
  std::uint32_t written() const override { return written_; }
  std::uint32_t expectedSize() const override { return expected_size_; }
  const char* beginWrite(const image::ExpectedFile& expected) override;
  const char* write(const std::uint8_t* data, std::uint32_t size) override;
  const char* endWrite() override;
  const flasher::ImageSource* staged(const char** why) override;
  const char* commit(const image::AcceptedImage& token) override;
  // The base's door is not overridden: a radio's base has no business in the Walter's own
  // application slot, and TransferTarget::commit(const AcceptedBase&) says so. The using
  // declaration keeps both in this class's overload set rather than hiding one behind the other.
  using image::TransferTarget::commit;
  void abandonWrite() override;

 private:
  // The other slot's bytes as an ImageSource, for the verdict's checks over the flash.
  class SlotSource final : public flasher::ImageSource {
   public:
    std::uint32_t size() const override { return size_; }
    bool read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const override;
    interfaces::OwnUpdate* slots_ = nullptr;
    std::uint8_t index_ = 0;
    std::uint32_t size_ = 0;
  };

  bool journal(SelfState state, SelfReason reason, std::uint8_t flags, const SelfPayload& about);
  bool lastEntry(SelfPayload* out) const;
  // Written straight after the *rolled back* entry begin() just made, and only there.
  void recordEvidence(const SelfPayload& about, const interfaces::OwnSlot* trial_slot);
  SelfReason preconditions(bool with_battery);
  // The SHA-256 of a slot's first `size` bytes; false when the slot cannot be read.
  bool hashSlot(std::uint8_t index, std::uint32_t size, std::uint8_t out[image::kSha256Size]);
  // The new image's hash recomputed from its slot and, if it is the one journalled, put on the
  // bad list. False when it could not be (and the rollback entry says so).
  bool markBad(const SelfPayload& about);
  SelfStep reject(SelfReason reason, std::uint8_t flags);
  SelfStep confirm(std::uint8_t flags);
  const char* linkMajorRule(const image::ExpectedFile& expected) const;

  interfaces::OwnUpdate& slots_;
  image::Store& store_;
  Journal& journal_;
  link::client::Client& radio_;
  UplinkCheck& uplink_;
  interfaces::Clock& clock_;
  interfaces::Restart& restart_;
  BatterySource& battery_;
  health::Health& health_;
  health::CoreDump* dumps_ = nullptr;

  image::Fields self_;
  image::SlotNeeds needs_;
  SelfState state_ = SelfState::kNone;
  SelfStatus status_;
  SelfPayload boot_;
  bool begun_ = false;
  std::uint64_t trial_started_ms_ = 0;
  std::uint64_t next_hello_ms_ = 0;
  std::uint64_t next_uplink_ms_ = 0;
  bool main_loop_ = false;

  // The open write into the other slot.
  bool open_ = false;
  bool ended_ = false;  // endWrite() succeeded: the bytes are all in flash and may be judged
  std::uint32_t expected_size_ = 0;
  std::uint32_t written_ = 0;
  std::uint8_t other_index_ = 0;
  char expected_version_[image::kVersionSize] = {};
  SlotSource staged_;
  std::uint8_t piece_[256];
};

}  // namespace update
}  // namespace core
