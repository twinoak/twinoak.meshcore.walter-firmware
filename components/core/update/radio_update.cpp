#include "core/update/radio_update.h"

#include "core/flasher/board_profile.h"
#include "core/link/codec/wire.h"

namespace core {
namespace update {

namespace {

bool sameText(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  for (; *a != 0 && *b != 0; ++a, ++b) {
    if (*a != *b) return false;
  }
  return *a == 0 && *b == 0;
}

void copyText(const char* from, char* to, std::size_t room) {
  std::size_t i = 0;
  for (; from != nullptr && from[i] != 0 && i + 1 < room; ++i) to[i] = from[i];
  for (; i < room; ++i) to[i] = 0;
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

// The descriptor's role byte against the link's (spec/LINK-PROTOCOL.md §5.1 and
// FIRMWARE-MANIFEST.md §4 use the same numbers: 1 repeater, 2 bridge, 3 scanner).
std::uint8_t linkRoleOf(image::Role role) { return static_cast<std::uint8_t>(role); }

}  // namespace

// ------------------------------------------------------------------------------- the words

const char* describe(State state) {
  switch (state) {
    case State::kIdle: return "idle";
    case State::kCheck: return "check";
    case State::kFlashing: return "flashing-candidate";
    case State::kBooting: return "booting";
    case State::kProving: return "proving";
    case State::kCommitting: return "committing";
    case State::kDone: return "done";
    case State::kRollingBack: return "rolling-back";
    case State::kRollbackBooting: return "rolling-back (booting the golden image)";
    case State::kRollbackProving: return "rolling-back (proving the golden image)";
    case State::kRestored: return "restored";
    case State::kStuck: return "stuck";
    case State::kRefused: return "refused";
  }
  return "?";
}

bool isTerminal(State state) {
  return state == State::kDone || state == State::kRestored || state == State::kStuck ||
         state == State::kRefused;
}

const char* describe(Kind kind) {
  switch (kind) {
    case Kind::kUpdate: return "update";
    case Kind::kFirstInstall: return "first install";
    case Kind::kRoleScanner: return "switch to the scanner";
    case Kind::kRoleRepeater: return "switch back to the repeater";
  }
  return "?";
}

const char* describe(Reason reason) {
  switch (reason) {
    case Reason::kNone: return "";
    case Reason::kNoStore: return "this build has no image store";
    case Reason::kNoProfile: return "no board profile for the slot";
    case Reason::kNoCandidate: return "the store holds no image to flash";
    case Reason::kImageUnreadable: return "the stored image does not verify against its record";
    case Reason::kOnBadList: return "this image failed a health check once and is never tried again";
    case Reason::kWrongRole: return "the image's role is not one this slot may hold";
    case Reason::kWrongBoard: return "the image's board is not the board in this slot";
    case Reason::kWrongBand: return "the image's band is not the golden image's";
    case Reason::kRadioUpdateInProgress: return "the journal says a radio update is in progress: it is resumed, not restarted (S-7)";
    case Reason::kSelfUpdateUnconfirmed: return "the Walter's own update is unconfirmed: never both at once (S-7)";
    case Reason::kLinkDown: return "the link is down and nobody has said why";
    case Reason::kBatteryUnknown: return "the battery could not be read";
    case Reason::kBatteryLow: return "the battery is below the update threshold";
    case Reason::kBatteryFalling: return "the battery is falling fast";
    case Reason::kNoGolden: return "there is no golden image to return to (a first install needs saying so)";
    case Reason::kBusy: return "an update is already running";
    case Reason::kFlashFailed: return "the flasher did not finish";
    case Reason::kNoBoot: return "no HELLO inside the boot window";
    case Reason::kWrongVersion: return "HELLO's version is not the flashed image's";
    case Reason::kRoleMismatch: return "HELLO's role is not the flashed image's";
    case Reason::kBoardMismatch: return "HELLO's board is not the flashed image's";
    case Reason::kLinkMajor: return "the radio speaks a link major this Walter does not";
    case Reason::kTimeRefused: return "the radio would not take the clock";
    case Reason::kNotInitialised: return "STATUS does not say the radio initialised";
    case Reason::kRestarted: return "the radio restarted inside the proving period";
    case Reason::kSilence: return "the radio went silent inside the proving period";
    case Reason::kNoTelemetry: return "POWER_GET did not answer though the role advertises it";
    case Reason::kNotAScanner: return "nothing that calls itself a scanner appeared";
    case Reason::kJournal: return "the journal could not be written";
    case Reason::kStore: return "the image store refused";
    case Reason::kNoImageToReturnTo: return "no image to return to";
  }
  return "?";
}

const char* describe(ProveStep step) {
  switch (step) {
    case ProveStep::kNone: return "-";
    case ProveStep::kHello: return "HELLO against the descriptor";
    case ProveStep::kTime: return "the clock set";
    case ProveStep::kStatus: return "STATUS says initialised";
    case ProveStep::kPing: return "the link steady through the proving period";
    case ProveStep::kPower: return "POWER_GET answers";
    case ProveStep::kScanner: return "a scanner shows itself";
    case ProveStep::kPassed: return "passed";
  }
  return "?";
}

// ------------------------------------------------------------------------------- the payload

void encode(const Payload& p, std::uint8_t out[kPayloadBytes]) {
  for (std::size_t i = 0; i < kPayloadBytes; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(p.state);
  out[1] = static_cast<std::uint8_t>(p.kind);
  out[2] = p.slot;
  out[3] = static_cast<std::uint8_t>(p.reason);
  out[4] = static_cast<std::uint8_t>(p.step);
  out[5] = p.flags;
  for (std::size_t i = 0; i < image::kSha256Size; ++i) out[8 + i] = p.sha256[i];
  put32(out + 40, p.detail);
}

bool decode(const Entry& e, Payload* out) {
  if (e.kind != kind::kRadioUpdate || e.size < kPayloadBytes || out == nullptr) return false;
  if (e.payload[0] > static_cast<std::uint8_t>(State::kRefused)) return false;
  if (e.payload[1] > static_cast<std::uint8_t>(Kind::kRoleRepeater)) return false;
  out->state = static_cast<State>(e.payload[0]);
  out->kind = static_cast<Kind>(e.payload[1]);
  out->slot = e.payload[2];
  out->reason = static_cast<Reason>(e.payload[3]);
  out->step = static_cast<ProveStep>(e.payload[4]);
  out->flags = e.payload[5];
  for (std::size_t i = 0; i < image::kSha256Size; ++i) out->sha256[i] = e.payload[8 + i];
  out->detail = get32(e.payload + 40);
  return true;
}

// ------------------------------------------------------------------------------- the machine

RadioUpdate::RadioUpdate(image::Store& store, flasher::Flasher& flasher,
                         link::client::Client& radio, Journal& journal, interfaces::Clock& clock,
                         BatterySource& battery)
    : store_(store), flasher_(flasher), radio_(radio), journal_(journal), clock_(clock),
      battery_(battery) {
  radio_.subscribeRestart(this);
}

void RadioUpdate::clear() {
  if (active()) return;
  state_ = State::kIdle;
  reason_ = Reason::kNone;
  status_ = Status{};
  image_loaded_ = false;
  journal_.unprotect();
}

void RadioUpdate::onRadioRestart(std::uint32_t, std::uint8_t) {
  // Inside the proving period a restart is the failure the period exists to catch. Elsewhere it
  // is what booting waits for. Recorded; acted on at the next step - this is called from inside
  // a client call and must not talk on the link.
  restart_seen_ = true;
}

// The entry before the action (S-5, §6: "Every step is journalled ... before it is taken").
bool RadioUpdate::journal(State state, Reason reason, ProveStep step, std::uint32_t detail) {
  Payload p;
  p.state = state;
  p.kind = request_.kind;
  p.slot = request_.slot.index;
  p.reason = reason;
  p.step = step;
  p.flags = static_cast<std::uint8_t>((request_.link_known_down ? kFlagLinkKnownDown : 0) |
                                      (status_.resumed ? kFlagResumed : 0));
  for (std::size_t i = 0; i < image::kSha256Size; ++i) p.sha256[i] = status_.sha256[i];
  p.detail = detail;
  std::uint8_t bytes[kPayloadBytes];
  encode(p, bytes);
  std::uint32_t seq = 0;
  if (journal_.append(kind::kRadioUpdate, bytes, sizeof bytes, &seq) != JournalError::kNone) return false;
  ++status_.entries;
  if (first_seq_ == 0) {
    first_seq_ = seq;
    journal_.protect(seq);
  }
  if (status_.entries > kEntriesPerRunMax) return false;  // something is looping; stop writing
  return true;
}

StepResult RadioUpdate::enter(State next, const char* note, Reason reason, ProveStep step,
                              std::uint32_t detail) {
  StepResult r;
  r.before = state_;
  r.note = note;
  r.reason = reason;
  r.step = step;
  if (!journal(next, reason, step, detail)) {
    // Without the journal the machine does not act: it stops in the state it is in, says so, and
    // a later resume() finds the last entry it did write.
    reason_ = Reason::kJournal;
    status_.reason = reason_;
    r.reason = Reason::kJournal;
    r.note = describe(Reason::kJournal);
    r.after = state_;
    return r;
  }
  state_ = next;
  if (reason != Reason::kNone) reason_ = reason;
  status_.state = next;
  status_.reason = reason_;
  status_.step = step;
  r.after = next;
  r.changed = true;
  if (isTerminal(next)) journal_.unprotect();
  return r;
}

StepResult RadioUpdate::waiting(const char* note) {
  StepResult r;
  r.before = state_;
  r.after = state_;
  r.waited = true;
  r.note = note;
  return r;
}

StepResult RadioUpdate::refuse(Reason reason) {
  reason_ = reason;
  return enter(State::kRefused, describe(reason), reason);
}

// A failure of the flash, the boot or the proof. From the candidate's path: rolling back - the
// hash on the bad list first, then the golden image. From the golden image's own path (a
// rollback, or a switch back): stuck. From a first install: stuck, "no image to return to".
StepResult RadioUpdate::fail(Reason reason, ProveStep step, std::uint32_t detail) {
  reason_ = reason;
  if (!rollback_image_) {
    status_.fail_reason = reason;
    status_.fail_step = step;
  }
  if (rollback_image_) {
    return enter(State::kStuck, describe(reason), reason, step, detail);
  }
  // The image that failed is never tried again (S-5): first thing, before any flash.
  const image::StoreError bad = store_.markBad(status_.sha256);
  if (request_.kind == Kind::kFirstInstall) {
    return enter(State::kStuck, describe(Reason::kNoImageToReturnTo), Reason::kNoImageToReturnTo,
                 step, detail);
  }
  if (bad != image::StoreError::kNone) {
    return enter(State::kStuck, describe(Reason::kStore), Reason::kStore, step, detail);
  }
  return enter(State::kRollingBack, describe(reason), reason, step, detail);
}

image::Which RadioUpdate::sourceOf(Kind kind) const {
  switch (kind) {
    case Kind::kUpdate:
    case Kind::kFirstInstall: return image::Which::kCandidate;
    case Kind::kRoleScanner: return image::Which::kScanner;
    case Kind::kRoleRepeater: return image::Which::kGolden;
  }
  return image::Which::kCandidate;
}

// The record and the bytes of the image a state is about: what is flashed, and the descriptor
// the HELLO is held against.
bool RadioUpdate::loadImage(image::Which which, bool for_rollback) {
  image_loaded_ = false;
  if (store_.read(which, request_.slot, &image_) != image::StoreError::kNone || !image_.present) return false;
  if (store_.open(which, request_.slot, &bytes_) != image::StoreError::kNone) return false;
  image_loaded_ = true;
  rollback_image_ = for_rollback;
  copyText(image_.descriptor.version, status_.version, sizeof status_.version);
  return true;
}

bool RadioUpdate::start(const Request& request) {
  if (active()) {
    reason_ = Reason::kBusy;
    return false;
  }
  clear();
  request_ = request;
  if (request.kind == Kind::kFirstInstall) request_.link_known_down = true;
  status_.kind = request_.kind;
  status_.slot = request_.slot;
  first_seq_ = 0;
  // S-7, before anything else: a radio update journalled as in progress is resumed, never
  // started over; the Walter's own update journalled as unconfirmed means never both.
  Entry e;
  Payload p;
  if (journal_.lastOf(kind::kRadioUpdate, &e) && decode(e, &p) && !isTerminal(p.state)) {
    reason_ = Reason::kRadioUpdateInProgress;
    return false;
  }
  if (journal_.lastOf(kind::kSelfUpdate, &e) && e.size >= 1 && self_update::unconfirmed(e.payload[0])) {
    reason_ = Reason::kSelfUpdateUnconfirmed;
    return false;
  }
  const StepResult r = enter(State::kCheck, "the preconditions");
  return r.changed;
}

bool RadioUpdate::resume() {
  if (active()) return true;
  Entry e;
  Payload p;
  if (!journal_.lastOf(kind::kRadioUpdate, &e) || !decode(e, &p) || isTerminal(p.state)) return false;
  clear();
  request_.kind = p.kind;
  request_.slot = board::Slot{p.slot};
  request_.link_known_down = (p.flags & kFlagLinkKnownDown) != 0;
  status_.kind = p.kind;
  status_.slot = request_.slot;
  status_.resumed = true;
  for (std::size_t i = 0; i < image::kSha256Size; ++i) status_.sha256[i] = p.sha256[i];
  reason_ = p.reason;
  first_seq_ = 0;
  // Where to take it up: never further along than the journal says, and never assuming a proof
  // passed - booting or proving start the proof over.
  State from = p.state;
  switch (p.state) {
    case State::kCheck: from = State::kCheck; break;
    case State::kFlashing: from = State::kFlashing; break;
    case State::kBooting:
    case State::kProving: from = State::kBooting; break;
    case State::kCommitting: from = State::kCommitting; break;
    case State::kRollingBack: from = State::kRollingBack; break;
    case State::kRollbackBooting:
    case State::kRollbackProving: from = State::kRollbackBooting; break;
    default: return false;
  }
  const bool rollback = from == State::kRollingBack || from == State::kRollbackBooting;
  // The image this run is about, from the store again: the candidate (or the scanner) unless the
  // run is already rolling back, when it is the golden image.
  const image::Which which = rollback ? image::Which::kGolden : sourceOf(p.kind);
  if (from == State::kCommitting) {
    // A reset between the promotion and its `done` entry leaves the journal saying committing
    // while the candidate is the golden image already and the candidate slot is empty - so the
    // candidate is not loaded here (it may be gone, rightly), and stepCommitting tells the two
    // cases apart by the hash the entry carries. The version, for the story, from whichever
    // record carries that hash.
    image::Record r;
    const image::Which places[2] = {image::Which::kGolden, image::Which::kCandidate};
    for (image::Which place : places) {
      if (store_.read(place, request_.slot, &r) == image::StoreError::kNone && r.present &&
          image::sha256Equal(r.sha256, status_.sha256)) {
        copyText(r.descriptor.version, status_.version, sizeof status_.version);
        break;
      }
    }
  } else if (!loadImage(which, rollback || p.kind == Kind::kRoleRepeater)) {
    // Gone from the store (a resume long after, or flash rot): the run cannot go on. For a
    // candidate that means rolling back, if it was ever flashed; for the golden image, stuck.
    state_ = from;
    status_.state = from;
    if (from == State::kCheck) {
      (void)refuse(Reason::kNoCandidate);
    } else {
      (void)fail(rollback ? Reason::kNoImageToReturnTo : Reason::kImageUnreadable, ProveStep::kNone);
    }
    return true;
  }
  if (from != State::kCommitting && !rollback && p.kind != Kind::kRoleRepeater) {
    for (std::size_t i = 0; i < image::kSha256Size; ++i) status_.sha256[i] = image_.sha256[i];
  }
  // The resumed state is journalled again, with the flag, before its action starts.
  state_ = from;
  status_.state = from;
  const StepResult r = enter(from, "resumed after a restart");
  if (!r.changed) return true;  // the journal failed: active, and the next step says so
  if (from == State::kBooting || from == State::kRollbackBooting) {
    window_started_ms_ = clock_.nowMs();
    next_hello_ms_ = window_started_ms_;
    restart_seen_ = false;
    radio_.radioWasReset();  // what this Walter believed about the radio is from before its own restart
  }
  return true;
}

StepResult RadioUpdate::step() {
  switch (state_) {
    case State::kIdle:
    case State::kDone:
    case State::kRestored:
    case State::kStuck:
    case State::kRefused:
      return waiting(isTerminal(state_) ? describe(state_) : "idle");
    case State::kCheck: return stepCheck();
    case State::kFlashing: return stepFlashing(false);
    case State::kBooting: return stepBooting(false);
    case State::kProving: return stepProving(false);
    case State::kCommitting: return stepCommitting();
    case State::kRollingBack: return stepFlashing(true);
    case State::kRollbackBooting: return stepBooting(true);
    case State::kRollbackProving: return stepProving(true);
  }
  return waiting("?");
}

// ---------------------------------------------------------------------------- check

StepResult RadioUpdate::stepCheck() {
  if (reason_ == Reason::kJournal) return waiting(describe(reason_));
  const flasher::BoardProfile* profile = flasher::profileForSlot(request_.slot);
  if (profile == nullptr) return refuse(Reason::kNoProfile);

  // The image: in the store, verifying, not on the bad list, for this slot.
  const image::Which which = sourceOf(request_.kind);
  const bool switching_back = request_.kind == Kind::kRoleRepeater;
  if (!loadImage(which, switching_back)) return refuse(Reason::kNoCandidate);
  for (std::size_t i = 0; i < image::kSha256Size; ++i) status_.sha256[i] = image_.sha256[i];
  if (store_.verify(which, request_.slot) != image::StoreError::kNone) return refuse(Reason::kImageUnreadable);
  if (store_.isBad(image_.sha256)) return refuse(Reason::kOnBadList);
  const image::Role wanted = request_.kind == Kind::kRoleScanner ? image::Role::kScanner : image::Role::kRepeater;
  if (image_.descriptor.role != wanted) return refuse(Reason::kWrongRole);
  if (!sameText(image_.descriptor.board, profile->name)) return refuse(Reason::kWrongBoard);
  if (image_.descriptor.slot != image::kAnySlot && image_.descriptor.slot != request_.slot.index) {
    return refuse(Reason::kWrongBoard);
  }

  // A golden image to return to - unless a person said first-install. For a switch back, the
  // golden image is the image itself. Its band is the band the slot is known to be.
  image::Record golden;
  const bool have_golden = store_.read(image::Which::kGolden, request_.slot, &golden) == image::StoreError::kNone && golden.present;
  if (!have_golden && request_.kind != Kind::kFirstInstall) return refuse(Reason::kNoGolden);
  if (have_golden && !switching_back && !sameText(image_.descriptor.band, golden.descriptor.band)) {
    return refuse(Reason::kWrongBand);
  }

  // The link: up, or down in a way the caller has classified. A switch back from the scanner
  // finds the scanner's text, which is up in its own way.
  const link::client::Answer<link::HelloResponse> hello = radio_.hello();
  const bool link_up = hello.ok();
  const bool scanner_there = radio_.mode() == link::client::Mode::kText;
  if (!link_up && !(switching_back && scanner_there) && !request_.link_known_down) {
    return refuse(Reason::kLinkDown);
  }

  // The battery: above the threshold and not falling fast. Read over the link; a first install
  // has no radio to ask and a person at the console is the gauge.
  //
  // A reading that cannot be had at all is a refusal - except for the two cases where there is no
  // gauge to be had and never will be in that moment. The INA3221 is on the *radio's* I2C
  // (spec/SYSTEM.md §2, §5.8's POWER_GET), so a caller that has classified the link as down -
  // task p2-05's supervisor putting the golden image back on a radio that will not answer - is
  // asking for the one flash that exists to unbrick a radio, and demanding a reading over the
  // dead link it has just classified would make that flash impossible. What protects the battery
  // there is the supervisor's own daily cap and its threshold on the last good reading
  // (core/update/last_good_battery.h), not a gauge this Walter cannot reach.
  BatteryReading battery;
  if (battery_.read(&battery)) {
    if (battery.millivolts < kBatteryMinMv) return refuse(Reason::kBatteryLow);
    if (battery.trend_known && battery.mv_per_hour <= kBatteryFallingMvPerHour) return refuse(Reason::kBatteryFalling);
  } else if (request_.kind != Kind::kFirstInstall && !request_.link_known_down) {
    return refuse(Reason::kBatteryUnknown);
  }

  return enter(switching_back ? State::kRollingBack : State::kFlashing, "the preconditions hold");
}

// ---------------------------------------------------------------------------- flashing

StepResult RadioUpdate::stepFlashing(bool rollback) {
  if (reason_ == Reason::kJournal) return waiting(describe(reason_));
  const flasher::BoardProfile* profile = flasher::profileForSlot(request_.slot);
  if (profile == nullptr) return fail(Reason::kNoProfile, ProveStep::kNone);
  if (rollback) {
    // The bad list first - again on a resume, where it is a no-op - then the golden image.
    if (request_.kind != Kind::kRoleRepeater) {
      if (store_.markBad(status_.sha256) != image::StoreError::kNone) {
        return enter(State::kStuck, describe(Reason::kStore), Reason::kStore);
      }
    }
    if (!loadImage(image::Which::kGolden, true)) {
      return enter(State::kStuck, describe(Reason::kNoImageToReturnTo), Reason::kNoImageToReturnTo);
    }
  } else if (!image_loaded_) {
    if (!loadImage(sourceOf(request_.kind), false)) return fail(Reason::kNoCandidate, ProveStep::kNone);
  }
  bytes_.rewind();
  flasher::Image image;
  image.source = &bytes_;
  image.load_address = image_.load_address;
  image.sha256 = image_.sha256;
  const flasher::Result r = flasher_.flash(image, *profile, progress_, request_.slot);
  if (!r.ok()) {
    return fail(Reason::kFlashFailed, ProveStep::kNone, static_cast<std::uint32_t>(r.step));
  }
  // The flasher reset the radio through the debug port: everything the client believed is gone.
  radio_.radioWasReset();
  window_started_ms_ = clock_.nowMs();
  next_hello_ms_ = window_started_ms_;
  restart_seen_ = false;
  return enter(rollback ? State::kRollbackBooting : State::kBooting, "flashed; the radio was reset");
}

// ---------------------------------------------------------------------------- booting

StepResult RadioUpdate::stepBooting(bool rollback) {
  if (reason_ == Reason::kJournal) return waiting(describe(reason_));
  const bool scanner = request_.kind == Kind::kRoleScanner && !rollback;
  const std::uint64_t now = clock_.nowMs();
  const std::uint32_t window = scanner ? kScannerWindowMs : kBootWindowMs;
  // An EV_BOOT may already be in: the main loop polls, and so does this.
  radio_.poll();
  bool up = radio_.session().up;
  bool text = radio_.mode() == link::client::Mode::kText;
  if (!up && !text && now >= next_hello_ms_) {
    next_hello_ms_ = now + kBootHelloIntervalMs;
    const link::client::Answer<link::HelloResponse> hello = radio_.hello();
    up = hello.ok();
    text = radio_.mode() == link::client::Mode::kText;
    if (!up && hello.fault == link::client::Fault::kRefused) {
      // §5.1: the radio does not speak this Walter's major. It answered, so it booted; it is
      // not a radio this Walter can run.
      return fail(Reason::kLinkMajor, ProveStep::kHello, hello.status);
    }
  }
  if (up || (scanner && text)) {
    beginProving();
    return enter(rollback ? State::kRollbackProving : State::kProving, "the radio answered");
  }
  if (clock_.nowMs() - window_started_ms_ >= window) {
    return fail(scanner ? Reason::kNotAScanner : Reason::kNoBoot, scanner ? ProveStep::kScanner : ProveStep::kHello);
  }
  return waiting("waiting for the radio to boot");
}

void RadioUpdate::beginProving() {
  window_started_ms_ = clock_.nowMs();
  next_ping_ms_ = window_started_ms_;
  boot_id_ = radio_.session().boot_id;
  restart_seen_ = false;
  prove_step_ = ProveStep::kHello;
  status_.pings = 0;
  status_.prove_elapsed_ms = 0;
  status_.time_skipped = false;
}

// Check 1: what HELLO says against the descriptor flashed (FIRMWARE-MANIFEST §4).
bool RadioUpdate::helloMatches(Reason* why) {
  const link::client::Session& s = radio_.session();
  if (!sameText(s.fw_version, image_.descriptor.version)) {
    *why = Reason::kWrongVersion;
    return false;
  }
  if (s.role != linkRoleOf(image_.descriptor.role)) {
    *why = Reason::kRoleMismatch;
    return false;
  }
  if (!sameText(s.board, image_.descriptor.board)) {
    *why = Reason::kBoardMismatch;
    return false;
  }
  if (s.proto_major != link::kProtoMajor) {
    *why = Reason::kLinkMajor;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------- proving

StepResult RadioUpdate::stepProving(bool rollback) {
  if (reason_ == Reason::kJournal) return waiting(describe(reason_));
  namespace lc = link::client;
  const std::uint64_t now = clock_.nowMs();
  status_.prove_elapsed_ms = static_cast<std::uint32_t>(now - window_started_ms_);
  status_.step = prove_step_;

  // The scanner's proof is what booting saw: text lines, or a HELLO that calls itself a scanner.
  if (request_.kind == Kind::kRoleScanner && !rollback) {
    const bool text = radio_.mode() == lc::Mode::kText;
    const bool says_so = radio_.session().up && radio_.session().role == link::role::kScanner;
    if (!text && !says_so) return fail(Reason::kNotAScanner, ProveStep::kScanner);
    return enter(State::kDone, "a scanner is there; golden and candidate untouched", Reason::kNone, ProveStep::kPassed);
  }

  if (restart_seen_ || (radio_.session().up && radio_.session().boot_id != boot_id_)) {
    return fail(Reason::kRestarted, prove_step_, status_.prove_elapsed_ms);
  }

  switch (prove_step_) {
    case ProveStep::kHello: {
      const lc::Answer<link::HelloResponse> hello = radio_.hello();
      if (!hello.ok()) {
        return fail(hello.fault == lc::Fault::kRefused ? Reason::kLinkMajor : Reason::kSilence, ProveStep::kHello);
      }
      if (restart_seen_) return fail(Reason::kRestarted, ProveStep::kHello);
      boot_id_ = radio_.session().boot_id;
      Reason why = Reason::kNone;
      if (!helloMatches(&why)) return fail(why, ProveStep::kHello);
      prove_step_ = ProveStep::kTime;
      return waiting("HELLO is the descriptor's");
    }
    case ProveStep::kTime: {
      // §5.4: the first thing a healthy repeater must take. The client sends it when there is a
      // time to give; kNoSession with the clock still pending means there is none, and the step
      // is skipped and said so rather than failed - a Walter with no modem cannot make it.
      const bool pending = radio_.timePending();
      const lc::Fault sent = radio_.serviceTime();
      if (sent == lc::Fault::kNone) {
        prove_step_ = ProveStep::kStatus;
        return waiting("the clock was set");
      }
      if (sent == lc::Fault::kNoSession && pending && radio_.timePending()) {
        // No time to give. TIME_GET must answer: the clock machinery is there.
        const lc::Answer<link::TimeGetResponse> got = radio_.timeGet();
        if (!got.ok()) return fail(Reason::kTimeRefused, ProveStep::kTime);
        status_.time_skipped = true;
        prove_step_ = ProveStep::kStatus;
        return waiting("no time to give: the clock check is skipped, TIME_GET answered");
      }
      if (sent == lc::Fault::kNoSession && !pending) {
        // Nothing pending: the clock was set already in this session (the client does it on
        // the first call after a restart). TIME_GET must still answer.
        const lc::Answer<link::TimeGetResponse> got = radio_.timeGet();
        if (!got.ok()) return fail(Reason::kTimeRefused, ProveStep::kTime);
        prove_step_ = ProveStep::kStatus;
        return waiting("the clock was already set");
      }
      return fail(Reason::kTimeRefused, ProveStep::kTime, sent == lc::Fault::kRefused ? 1 : 0);
    }
    case ProveStep::kStatus: {
      const lc::Answer<link::StatusResponse> status = radio_.status();
      if (!status.ok()) return fail(Reason::kSilence, ProveStep::kStatus);
      if ((status.message.flags & link::status_flag::kRadioUp) == 0) {
        return fail(Reason::kNotInitialised, ProveStep::kStatus, status.message.flags);
      }
      prove_step_ = ProveStep::kPing;
      next_ping_ms_ = now;
      return waiting("the radio initialised");
    }
    case ProveStep::kPing: {
      if (now < next_ping_ms_) return waiting("the proving period");
      next_ping_ms_ = now + kProvingPingIntervalMs;
      const lc::Answer<link::PingResponse> ping = radio_.ping(static_cast<std::uint32_t>(now));
      if (!ping.ok()) return fail(Reason::kSilence, ProveStep::kPing, status_.prove_elapsed_ms);
      ++status_.pings;
      if (restart_seen_ || radio_.session().boot_id != boot_id_) {
        return fail(Reason::kRestarted, ProveStep::kPing, status_.prove_elapsed_ms);
      }
      if (now - window_started_ms_ < kProvingPeriodMs) return waiting("the proving period");
      prove_step_ = ProveStep::kPower;
      return waiting("the link stayed up through the proving period");
    }
    case ProveStep::kPower: {
      if (radio_.hasCapability(link::cap::kPower)) {
        const lc::Answer<link::PowerGetResponse> power = radio_.powerGet();
        if (!power.ok()) return fail(Reason::kNoTelemetry, ProveStep::kPower);
      }
      prove_step_ = ProveStep::kPassed;
      // A switch back to the repeater proves the golden image on the rollback path and is done,
      // not restored: nothing failed.
      if (request_.kind == Kind::kRoleRepeater) {
        return enter(State::kDone, "the repeater is back and proved", Reason::kNone,
                     ProveStep::kPassed, status_.prove_elapsed_ms);
      }
      if (rollback) {
        return enter(State::kRestored, "the golden image is back and proved", Reason::kNone,
                     ProveStep::kPassed, status_.prove_elapsed_ms);
      }
      return enter(State::kCommitting, "the health check passed", Reason::kNone, ProveStep::kPassed,
                   status_.prove_elapsed_ms);
    }
    case ProveStep::kNone:
    case ProveStep::kScanner:
    case ProveStep::kPassed:
      break;
  }
  return waiting("?");
}

// ---------------------------------------------------------------------------- committing

StepResult RadioUpdate::stepCommitting() {
  if (reason_ == Reason::kJournal) return waiting(describe(reason_));
  // The promotion, or the evidence that a reset landed after it: the golden image already is
  // this hash and the candidate slot is empty.
  image::Record golden;
  if (store_.read(image::Which::kGolden, request_.slot, &golden) == image::StoreError::kNone &&
      golden.present && image::sha256Equal(golden.sha256, status_.sha256)) {
    return enter(State::kDone, "the candidate is the golden image", Reason::kNone, ProveStep::kPassed);
  }
  // A first install has no golden image to swap with; promote() moves the pointer all the same
  // (the golden area is empty, and becomes the candidate area).
  image::Record candidate;
  if (store_.read(image::Which::kCandidate, request_.slot, &candidate) != image::StoreError::kNone ||
      !candidate.present || !image::sha256Equal(candidate.sha256, status_.sha256)) {
    return enter(State::kStuck, describe(Reason::kStore), Reason::kStore);
  }
  const image::StoreError promoted = store_.promote(request_.slot);
  if (promoted != image::StoreError::kNone) {
    return enter(State::kStuck, describe(Reason::kStore), Reason::kStore, ProveStep::kPassed,
                 static_cast<std::uint32_t>(promoted));
  }
  return enter(State::kDone, "the candidate is the golden image", Reason::kNone, ProveStep::kPassed);
}

}  // namespace update
}  // namespace core
