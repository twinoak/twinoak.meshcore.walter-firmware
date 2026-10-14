#include "core/update/self_update.h"

#include "core/image/this_image.h"
#include "core/version.h"

namespace core {
namespace update {

namespace {

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

bool prefixMatches(const std::uint8_t full[image::kSha256Size], const std::uint8_t hash16[kHash16]) {
  for (std::size_t i = 0; i < kHash16; ++i) {
    if (full[i] != hash16[i]) return false;
  }
  return true;
}

bool knownState(std::uint8_t byte) {
  return byte == self_update::kReceived || byte == self_update::kSwitching ||
         byte == self_update::kOnTrial || byte == self_update::kConfirmed ||
         byte == self_update::kRolledBack;
}

}  // namespace

// ------------------------------------------------------------------------------- the words

const char* describe(SelfState state) {
  switch (state) {
    case SelfState::kNone: return "nothing";
    case SelfState::kReceived: return "received";
    case SelfState::kSwitching: return "switching";
    case SelfState::kOnTrial: return "on trial";
    case SelfState::kConfirmed: return "confirmed";
    case SelfState::kRolledBack: return "rolled back";
  }
  return "?";
}

const char* describe(SelfReason reason) {
  switch (reason) {
    case SelfReason::kNone: return "";
    case SelfReason::kNoSlots: return "the platform cannot say which application slot is running";
    case SelfReason::kRunningUnconfirmed: return "the running image is itself on trial: never stack an update on an unconfirmed one";
    case SelfReason::kRadioUpdateInProgress: return "the journal says a radio update is in progress: never both at once (S-7)";
    case SelfReason::kTransferRunning: return "a transfer into the other slot is open";
    case SelfReason::kBatteryUnknown: return "the battery could not be read";
    case SelfReason::kBatteryLow: return "the battery is below the update threshold (a brown-out in the trial is a wasted attempt)";
    case SelfReason::kBatteryFalling: return "the battery is falling fast";
    case SelfReason::kNothingReceived: return "no image has been received into the other slot ('self receive' first)";
    case SelfReason::kOtherSlotChanged: return "the other slot does not hold the image that was received";
    case SelfReason::kLinkDown: return "the radio does not answer HELLO and nobody has said why ('self switch radio-down' says so)";
    case SelfReason::kNotOnTrial: return "the running image is not on trial";
    case SelfReason::kPlatform: return "the platform refused";
    case SelfReason::kJournal: return "the journal could not be written";
    case SelfReason::kOnBadList: return "this image failed its trial once and is never tried again";
    case SelfReason::kTooLarge: return "the file is larger than the other application slot";
    case SelfReason::kLoadAddress: return "the file's load_address is not an application slot's offset";
    case SelfReason::kLinkMajor: return "the image speaks another link major than the radio and does not say it tolerates the radio's (S-7: the Walter goes first and speaks both)";
    case SelfReason::kAlreadyRunning: return "the file is the image that is running";
    case SelfReason::kNoMainLoop: return "the main loop was not reached inside the trial window";
    case SelfReason::kNoRadio: return "the radio answered before the update and not after it";
    case SelfReason::kNoUplink: return "the backend was not reached inside the trial window";
    case SelfReason::kRestartedDuringTrial: return "the image restarted during its trial (the bootloader gave up on it)";
    case SelfReason::kNeverBooted: return "the image never reached its main loop (the bootloader gave up on it)";
    case SelfReason::kSwitchNotTaken: return "the restart came before the other slot was made the next to boot: nothing was tried";
    case SelfReason::kRejectedByHand: return "rejected on the console";
  }
  return "?";
}

const char* describe(Condition condition) {
  switch (condition) {
    case Condition::kPending: return "pending";
    case Condition::kMet: return "met";
    case Condition::kNotApplicable: return "not applicable";
    case Condition::kFailed: return "failed, trying again";
  }
  return "?";
}

const char* describe(UplinkVerdict verdict) {
  switch (verdict) {
    case UplinkVerdict::kReached: return "the server was reached";
    case UplinkVerdict::kFailed: return "the server was not reached";
    case UplinkVerdict::kNotConfigured: return "no server is configured";
  }
  return "?";
}

// ------------------------------------------------------------------------------- the payload

void shortVersion(const char* version, char out[kShortVersion]) {
  std::size_t i = 0;
  for (; version != nullptr && version[i] != 0 && version[i] != '+' && i + 1 < kShortVersion; ++i) {
    out[i] = version[i];
  }
  for (; i < kShortVersion; ++i) out[i] = 0;
}

void encode(const SelfPayload& p, std::uint8_t out[kSelfPayloadBytes]) {
  for (std::size_t i = 0; i < kSelfPayloadBytes; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(p.state);
  out[1] = p.slot;
  out[2] = static_cast<std::uint8_t>(p.reason);
  out[3] = p.flags;
  put32(out + 4, p.size);
  for (std::size_t i = 0; i < kHash16; ++i) out[8 + i] = p.hash16[i];
  for (std::size_t i = 0; i < kShortVersion; ++i) {
    out[24 + i] = static_cast<std::uint8_t>(p.from[i]);
    out[36 + i] = static_cast<std::uint8_t>(p.to[i]);
  }
}

bool decode(const Entry& e, SelfPayload* out) {
  if (e.kind != kind::kSelfUpdate || e.size < kSelfPayloadBytes || out == nullptr) return false;
  if (!knownState(e.payload[0])) return false;
  out->state = static_cast<SelfState>(e.payload[0]);
  out->slot = e.payload[1];
  out->reason = static_cast<SelfReason>(e.payload[2]);
  out->flags = e.payload[3];
  out->size = get32(e.payload + 4);
  for (std::size_t i = 0; i < kHash16; ++i) out->hash16[i] = e.payload[8 + i];
  for (std::size_t i = 0; i < kShortVersion; ++i) {
    out->from[i] = static_cast<char>(e.payload[24 + i]);
    out->to[i] = static_cast<char>(e.payload[36 + i]);
  }
  out->from[kShortVersion - 1] = 0;
  out->to[kShortVersion - 1] = 0;
  return true;
}

// ------------------------------------------------------------------------------- the machine

SelfUpdate::SelfUpdate(interfaces::OwnUpdate& slots, image::Store& store, Journal& journal,
                       link::client::Client& radio, UplinkCheck& uplink, interfaces::Clock& clock,
                       interfaces::Restart& restart, BatterySource& battery, health::Health& health)
    : slots_(slots), store_(store), journal_(journal), radio_(radio), uplink_(uplink), clock_(clock),
      restart_(restart), battery_(battery), health_(health) {
  // What an offered image must be: this image's own three words (spec/FIRMWARE-MANIFEST.md §2
  // and §5 check 3), read out of the descriptor in this very image rather than written a second
  // time. Slot 0: the Walter is not a radio position, and §4's `slot` byte of its descriptor is
  // "any".
  self_ = image::this_image();
  needs_.slot = board::Slot{0};
  needs_.role = image::role_name(self_.role);
  needs_.board = self_.board;
  needs_.band = self_.band;
}

bool SelfUpdate::lastEntry(SelfPayload* out) const {
  Entry e;
  return journal_.lastOf(kind::kSelfUpdate, &e) && decode(e, out);
}

bool SelfUpdate::journal(SelfState state, SelfReason reason, std::uint8_t flags, const SelfPayload& about) {
  SelfPayload p = about;
  p.state = state;
  p.reason = reason;
  p.flags = flags;
  std::uint8_t bytes[kSelfPayloadBytes];
  encode(p, bytes);
  if (journal_.append(kind::kSelfUpdate, bytes, sizeof bytes) != JournalError::kNone) return false;
  boot_ = p;
  status_.state = state;
  status_.reason = reason;
  return true;
}

// -- boot ---------------------------------------------------------------------------------------

SelfBoot SelfUpdate::begin() {
  begun_ = true;
  status_ = SelfStatus{};
  interfaces::OwnSlot run;
  if (!slots_.running(&run)) {
    status_.reason = SelfReason::kNoSlots;
    return SelfBoot::kNoSlots;
  }
  SelfPayload last;
  const bool have = lastEntry(&last);
  const bool journalled_trial = have && self_update::unconfirmed(static_cast<std::uint8_t>(last.state)) &&
                                last.slot == run.index;

  // This image is on trial: the bootloader marked it pending-verify (interfaces/own_update.h),
  // or the journal says it is the one switched to. One boot, three conditions.
  if (run.state == interfaces::OwnImageState::kPendingVerify || journalled_trial) {
    SelfPayload about;
    std::uint8_t flags = 0;
    if (journalled_trial) {
      about = last;
      flags = static_cast<std::uint8_t>(last.flags & (kFlagRadioUp | kFlagRadioKnownDown));
    } else {
      // Pending-verify with no switching entry: somebody set the boot slot by other means. The
      // trial runs all the same - the bootloader would abort the image otherwise - with nothing
      // known about the radio before, so HELLO is asked of it, and nothing to put on a bad list.
      about.slot = run.index;
      shortVersion(run.version, about.to);
      flags = kFlagUnjournalled;
    }
    state_ = SelfState::kOnTrial;
    status_.on_trial = true;
    status_.trial = about;
    status_.trial.slot = run.index;
    status_.main_loop = Condition::kPending;
    status_.radio = (flags & kFlagRadioKnownDown) != 0 ? Condition::kNotApplicable : Condition::kPending;
    status_.uplink = Condition::kPending;
    trial_started_ms_ = clock_.nowMs();
    next_hello_ms_ = trial_started_ms_;
    next_uplink_ms_ = trial_started_ms_;
    main_loop_ = false;
    const bool wrote = journal(SelfState::kOnTrial, SelfReason::kNone, flags, status_.trial);
    status_.state = SelfState::kOnTrial;
    status_.trial.flags = flags;
    return wrote ? SelfBoot::kTrialBegun : SelfBoot::kJournalFailed;
  }

  // Not on trial. Does the journal say a new image was in the air? Then this is the old image
  // back after the bootloader gave up on the new one, and the story has to be closed: why, and
  // the hash on the bad list.
  if (have && self_update::unconfirmed(static_cast<std::uint8_t>(last.state)) && last.slot != run.index) {
    interfaces::OwnSlot other;
    const bool other_known = slots_.other(&other);
    SelfReason why;
    if (last.state == SelfState::kOnTrial) {
      why = SelfReason::kRestartedDuringTrial;
    } else if (other_known && (other.state == interfaces::OwnImageState::kAborted ||
                               other.state == interfaces::OwnImageState::kInvalid)) {
      why = SelfReason::kNeverBooted;
    } else {
      // The switching entry is there but the other slot was never booted and never given up on:
      // the restart came between the entry and the boot pointer. Nothing was tried, so nothing
      // goes on the bad list, and `self switch` may be said again.
      why = SelfReason::kSwitchNotTaken;
    }
    std::uint8_t flags = 0;
    if (why != SelfReason::kSwitchNotTaken && !markBad(last)) flags |= kFlagNotOnBadList;
    state_ = SelfState::kRolledBack;
    const bool wrote = journal(SelfState::kRolledBack, why, flags, last);
    status_.state = SelfState::kRolledBack;
    status_.reason = why;
    return wrote ? SelfBoot::kRollbackRecorded : SelfBoot::kJournalFailed;
  }

  state_ = have ? last.state : SelfState::kNone;
  status_.state = state_;
  status_.reason = have ? last.reason : SelfReason::kNone;
  boot_ = have ? last : SelfPayload{};
  return SelfBoot::kNothing;
}

void SelfUpdate::mainLoopReached() {
  main_loop_ = true;
  if (onTrial()) status_.main_loop = Condition::kMet;
}

// -- the trial -----------------------------------------------------------------------------------

bool SelfUpdate::hashSlot(std::uint8_t index, std::uint32_t size, std::uint8_t out[image::kSha256Size]) {
  image::Sha256 hash;
  for (std::uint32_t at = 0; at < size;) {
    const std::uint32_t left = size - at;
    const std::uint32_t n = left < sizeof piece_ ? left : static_cast<std::uint32_t>(sizeof piece_);
    if (!slots_.readSlot(index, at, piece_, n)) return false;
    hash.update(piece_, n);
    at += n;
  }
  hash.finish(out);
  return true;
}

bool SelfUpdate::markBad(const SelfPayload& about) {
  if (about.size == 0) return false;  // an unjournalled trial: nothing is known of the image
  std::uint8_t full[image::kSha256Size];
  if (!hashSlot(about.slot, about.size, full)) return false;
  if (!prefixMatches(full, about.hash16)) return false;  // the slot holds something else now
  return store_.markBad(full) == image::StoreError::kNone;
}

SelfStep SelfUpdate::reject(SelfReason reason, std::uint8_t flags) {
  SelfStep s;
  s.reason = reason;
  // The hash on the bad list first (S-5's rule, kept here too), then the journal, then the
  // rollback itself. A reset between the entry and the rollback is caught by the bootloader
  // (the image is still pending) and by begin() at the next boot.
  if (!markBad(status_.trial)) flags |= kFlagNotOnBadList;
  if (!journal(SelfState::kRolledBack, reason, flags, status_.trial)) {
    s.reason = SelfReason::kJournal;
    s.note = describe(SelfReason::kJournal);
    s.waited = true;
    return s;
  }
  state_ = SelfState::kRolledBack;
  status_.on_trial = false;
  s.changed = true;
  s.after = SelfState::kRolledBack;
  s.note = describe(reason);
  health_.noteRestart(health::Restart::kSelfUpdateRollback);
  if (!slots_.rejectAndRestart()) {
    // The platform would not roll back: the other slot holds nothing bootable. The image stays
    // where it is - said, journalled above as rolled back all the same, so the story is honest
    // about what was decided; a person reads it on the console.
    s.reason = SelfReason::kPlatform;
    s.note = "rejected, but the platform would not roll back: the other slot holds nothing bootable";
    status_.reason = SelfReason::kPlatform;
  }
  return s;
}

SelfStep SelfUpdate::confirm(std::uint8_t flags) {
  SelfStep s;
  if (status_.radio == Condition::kNotApplicable) flags |= kFlagRadioNotApplicable;
  if (status_.uplink == Condition::kNotApplicable) flags |= kFlagUplinkNotApplicable;
  if (!slots_.confirm()) {
    s.reason = SelfReason::kPlatform;
    s.note = "the platform would not mark the running image valid";
    s.waited = true;
    status_.reason = SelfReason::kPlatform;
    return s;
  }
  if (!journal(SelfState::kConfirmed, SelfReason::kNone, flags, status_.trial)) {
    // Valid in the bootloader's eyes already, which is what matters for the next boot; the
    // story lacks its last line and says so.
    s.reason = SelfReason::kJournal;
  }
  state_ = SelfState::kConfirmed;
  status_.on_trial = false;
  status_.state = SelfState::kConfirmed;
  s.changed = true;
  s.after = SelfState::kConfirmed;
  s.note = (flags & kFlagByHand) != 0 ? "confirmed on the console" : "all three conditions hold: confirmed";
  return s;
}

SelfStep SelfUpdate::step() {
  SelfStep s;
  if (!onTrial()) {
    s.waited = true;
    s.after = state_;
    s.note = describe(state_);
    return s;
  }
  const std::uint64_t now = clock_.nowMs();
  status_.trial_elapsed_ms = static_cast<std::uint32_t>(now - trial_started_ms_);
  status_.main_loop = main_loop_ ? Condition::kMet : Condition::kPending;

  // 2. The radio, while condition 1 holds and 2 is open: HELLO when due. A scanner answers no
  // frame, but its text is the radio in a known state (task p1-02's detection), and counts.
  if (main_loop_ && status_.radio == Condition::kPending && now >= next_hello_ms_) {
    next_hello_ms_ = now + kTrialHelloIntervalMs;
    ++status_.hello_tries;
    const link::client::Answer<link::HelloResponse> hello = radio_.hello();
    if (hello.ok() || radio_.mode() == link::client::Mode::kText) status_.radio = Condition::kMet;
  }

  // 3. The uplink, once 1 and 2 are settled: one attempt, another after kTrialUplinkRetryMs.
  const bool radio_settled = status_.radio == Condition::kMet || status_.radio == Condition::kNotApplicable;
  if (main_loop_ && radio_settled && status_.uplink != Condition::kMet &&
      status_.uplink != Condition::kNotApplicable && now >= next_uplink_ms_) {
    next_uplink_ms_ = now + kTrialUplinkRetryMs;
    ++status_.uplink_tries;
    switch (uplink_.check()) {
      case UplinkVerdict::kReached: status_.uplink = Condition::kMet; break;
      case UplinkVerdict::kNotConfigured: status_.uplink = Condition::kNotApplicable; break;
      case UplinkVerdict::kFailed: status_.uplink = Condition::kFailed; break;
    }
  }

  const bool uplink_settled = status_.uplink == Condition::kMet || status_.uplink == Condition::kNotApplicable;
  if (main_loop_ && radio_settled && uplink_settled) return confirm(0);

  if (status_.trial_elapsed_ms >= kTrialWindowMs) {
    // The window passed: which of the three was missing, in order.
    if (!main_loop_) return reject(SelfReason::kNoMainLoop, 0);
    if (!radio_settled) return reject(SelfReason::kNoRadio, 0);
    return reject(SelfReason::kNoUplink, 0);
  }
  s.waited = true;
  s.after = SelfState::kOnTrial;
  s.note = !main_loop_ ? "waiting for the main loop"
           : !radio_settled ? "waiting for the radio's HELLO"
                            : "waiting for the uplink";
  return s;
}

// -- the console's commands -----------------------------------------------------------------------

SelfReason SelfUpdate::preconditions(bool with_battery) {
  interfaces::OwnSlot run;
  if (!slots_.running(&run)) return SelfReason::kNoSlots;
  if (run.state == interfaces::OwnImageState::kPendingVerify || onTrial()) return SelfReason::kRunningUnconfirmed;
  // S-7: the radio's update journalled as in progress, or the Walter's own switch in the air.
  Entry e;
  Payload radio;
  if (journal_.lastOf(kind::kRadioUpdate, &e) && update::decode(e, &radio) && !isTerminal(radio.state)) {
    return SelfReason::kRadioUpdateInProgress;
  }
  if (open_ || slots_.writing()) return SelfReason::kTransferRunning;
  if (with_battery) {
    BatteryReading battery;
    if (!battery_.read(&battery)) return SelfReason::kBatteryUnknown;
    if (battery.millivolts < kBatteryMinMv) return SelfReason::kBatteryLow;
    if (battery.trend_known && battery.mv_per_hour <= kBatteryFallingMvPerHour) return SelfReason::kBatteryFalling;
  }
  return SelfReason::kNone;
}

SelfReason SelfUpdate::canReceive() { return preconditions(false); }

bool SelfUpdate::received(SelfPayload* out) const {
  // Backwards from the newest self-update entry: a *received* that nothing has consumed. A
  // switch not taken, and the switching entry before it, leave the received image where it is.
  Cursor c;
  Entry e;
  bool more = journal_.first(&c, &e);
  while (more) {
    SelfPayload p;
    if (decode(e, &p)) {
      switch (p.state) {
        case SelfState::kReceived:
          if (out != nullptr) *out = p;
          return true;
        case SelfState::kSwitching: break;
        case SelfState::kRolledBack:
          if (p.reason == SelfReason::kSwitchNotTaken) break;
          return false;
        case SelfState::kOnTrial:
        case SelfState::kConfirmed:
        case SelfState::kNone:
          return false;
      }
    }
    more = journal_.previous(&c, &e);
  }
  return false;
}

SelfReason SelfUpdate::switchNow(const SwitchRequest& request) {
  const SelfReason refused = preconditions(true);
  if (refused != SelfReason::kNone) return refused;
  SelfPayload got;
  if (!received(&got)) return SelfReason::kNothingReceived;
  interfaces::OwnSlot other;
  if (!slots_.other(&other)) return SelfReason::kNoSlots;
  if (other.index != got.slot || got.size == 0 || got.size > other.size) return SelfReason::kOtherSlotChanged;
  std::uint8_t full[image::kSha256Size];
  if (!hashSlot(other.index, got.size, full) || !prefixMatches(full, got.hash16)) return SelfReason::kOtherSlotChanged;

  // The radio, before the restart: what it answers now is what the trial holds it to.
  const link::client::Answer<link::HelloResponse> hello = radio_.hello();
  const bool radio_up = hello.ok() || radio_.mode() == link::client::Mode::kText;
  if (!radio_up && !request.radio_known_down) return SelfReason::kLinkDown;
  std::uint8_t flags = 0;
  if (radio_up) flags |= kFlagRadioUp;
  if (!radio_up && request.radio_known_down) flags |= kFlagRadioKnownDown;

  // Journalled before it is done (S-6, §6): "self-update: switching to <to>, from <from>".
  shortVersion(core::version(), got.from);
  if (!journal(SelfState::kSwitching, SelfReason::kNone, flags, got)) return SelfReason::kJournal;
  state_ = SelfState::kSwitching;
  if (!slots_.setNextBoot()) {
    // Said in the journal too, so the story does not end on "switching" with nothing in the air.
    (void)journal(SelfState::kRolledBack, SelfReason::kPlatform, kFlagNotOnBadList, got);
    state_ = SelfState::kRolledBack;
    return SelfReason::kPlatform;
  }
  // The radio is told (spec/LINK-PROTOCOL.md §8); a radio that does not answer is one the switch
  // was allowed past, and the trial will say so.
  (void)radio_.sleepNotice(kSwitchExpectBackS);
  health_.noteRestart(health::Restart::kSelfUpdateSwitch);
  restart_.restart();
  return SelfReason::kNone;
}

SelfReason SelfUpdate::confirmNow() {
  if (!onTrial()) return SelfReason::kNotOnTrial;
  const SelfStep s = confirm(kFlagByHand);
  return s.changed ? SelfReason::kNone : s.reason;
}

SelfReason SelfUpdate::rejectNow() {
  if (!onTrial()) return SelfReason::kNotOnTrial;
  const SelfStep s = reject(SelfReason::kRejectedByHand, kFlagByHand);
  return s.changed ? s.reason == SelfReason::kPlatform ? SelfReason::kPlatform : SelfReason::kNone : s.reason;
}

// -- the other slot as a transfer target ---------------------------------------------------------

bool SelfUpdate::SlotSource::read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const {
  if (slots_ == nullptr || offset > size_ || count > size_ - offset) return false;
  return slots_->readSlot(index_, offset, out, count);
}

// S-7: "when the link's major version changes, the Walter goes first and speaks both majors until
// the RAK has followed". With the manifest's fields: `link.major` is the major the image speaks
// (its descriptor's, check 5 holds them equal); `requires.link_major` is what must already be
// there - the radio's major. An image whose link major is not the radio's current one is let in
// only if its requires names the radio's: that is how "speaks both" is said with the fields the
// specification has (docs/update.md, "the order of a link-version change"). The radio's current
// major is the session's when a HELLO has been answered, else the major this build speaks, which
// is the one the radio was last proved against.
const char* SelfUpdate::linkMajorRule(const image::ExpectedFile& expected) const {
  const link::client::Session& s = radio_.session();
  const std::uint32_t radio_major = s.up ? s.proto_major : image::kLinkMajor;
  if (expected.linkMajor() == radio_major) return nullptr;
  if (expected.hasRequiresLinkMajor() && expected.requiresLinkMajor() == radio_major) return nullptr;
  return describe(SelfReason::kLinkMajor);
}

const char* SelfUpdate::beginWrite(const image::ExpectedFile& expected) {
  const SelfReason refused = preconditions(false);
  if (refused != SelfReason::kNone) return describe(refused);
  if (store_.isBad(expected.sha256())) return describe(SelfReason::kOnBadList);
  interfaces::OwnSlot run;
  interfaces::OwnSlot other;
  if (!slots_.running(&run) || !slots_.other(&other)) return describe(SelfReason::kNoSlots);
  if (expected.size() > other.size) return describe(SelfReason::kTooLarge);
  // An application image loads at a slot's offset (the release writes ota_0's, tools/release/
  // build_release.py; the ESP32-S3 maps whichever slot the bootloader chose, so either is right).
  if (expected.loadAddress() != run.address && expected.loadAddress() != other.address) {
    return describe(SelfReason::kLoadAddress);
  }
  const char* major = linkMajorRule(expected);
  if (major != nullptr) return major;
  if (!slots_.beginWrite(expected.size())) {
    return "the platform would not open the other slot (the running image is unconfirmed, or the erase failed)";
  }
  open_ = true;
  expected_size_ = expected.size();
  written_ = 0;
  other_index_ = other.index;
  for (std::size_t i = 0; i < image::kVersionSize; ++i) expected_version_[i] = expected.version()[i];
  expected_version_[image::kVersionSize - 1] = 0;
  return nullptr;
}

const char* SelfUpdate::write(const std::uint8_t* data, std::uint32_t size) {
  if (!open_ || !slots_.writing()) return "no write is open on the other slot";
  if (written_ + size > expected_size_) return "more bytes than the manifest promised";
  if (!slots_.write(data, size)) return "the other slot could not be written";
  written_ += size;
  return nullptr;
}

const char* SelfUpdate::endWrite() {
  if (!open_ || !slots_.writing()) return "no write is open on the other slot";
  if (written_ != expected_size_) return "fewer bytes than the manifest promised";
  if (!slots_.endWrite()) {
    open_ = false;
    return "the platform did not accept the bytes as an application image";
  }
  ended_ = true;
  return nullptr;
}

const flasher::ImageSource* SelfUpdate::staged(const char** why) {
  if (!open_ || !ended_ || written_ != expected_size_) {
    *why = "the image is not all in the other slot";
    return nullptr;
  }
  staged_.slots_ = &slots_;
  staged_.index_ = other_index_;
  staged_.size_ = expected_size_;
  return &staged_;
}

const char* SelfUpdate::commit(const image::AcceptedImage& token) {
  if (!open_ || !ended_) return "the image is not all in the other slot";
  // The journal's *received* entry is what makes the bytes in the other slot an image this
  // firmware will switch to: the slot, the size, the hash's first sixteen bytes, the versions.
  SelfPayload p;
  p.slot = other_index_;
  p.size = expected_size_;
  for (std::size_t i = 0; i < kHash16; ++i) p.hash16[i] = token.sha256()[i];
  shortVersion(core::version(), p.from);
  shortVersion(token.version(), p.to);
  if (!journal(SelfState::kReceived, SelfReason::kNone, 0, p)) {
    open_ = false;
    ended_ = false;
    return describe(SelfReason::kJournal);
  }
  state_ = SelfState::kReceived;
  // The platform's handle closed at endWrite(); the transfer closes here. written_ stays for
  // the console's "N bytes".
  open_ = false;
  ended_ = false;
  return nullptr;
}

void SelfUpdate::abandonWrite() {
  if (slots_.writing()) slots_.abortWrite();
  open_ = false;
  ended_ = false;
  written_ = 0;
}

}  // namespace update
}  // namespace core
