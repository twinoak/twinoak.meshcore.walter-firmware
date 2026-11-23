#include "core/update/radio_update.h"

#include <cstdio>

#include "core/flasher/board_profile.h"
#include "core/link/codec/wire.h"
#include "core/link/radio_identity.h"

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
    case State::kNodeFault: return "node-fault";
  }
  return "?";
}

bool isTerminal(State state) {
  return state == State::kDone || state == State::kRestored || state == State::kStuck ||
         state == State::kRefused || state == State::kNodeFault;
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
    case Reason::kIdentityLost: return "the radio refuses: its identity is lost, and no image mends that";
    case Reason::kStartedThroughPort:
      return "the pulse reached nothing: the core was still halted, and the debug port started the image";
    case Reason::kPulseStarted: return "the pulse started the image: it is the image that is quiet";
    case Reason::kLookFailed: return "the chip could not be asked what the pulse did";
    case Reason::kAfterTheWatchdog: return "the proof begins at the boot after the scanner's watchdog";
    case Reason::kWatchdogTimePassed:
      return "the proof begins: the scanner's watchdog has had its longest time and reset nothing";
    case Reason::kFlashOnceMore: return "the flash did not begin: once more";
    case Reason::kFlashDidNotBegin:
      return "the flash did not begin, twice: nothing was written and the radio is as it was";
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
    case ProveStep::kIdentity: return "IDENTITY answers, or has no key yet";
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

// Task p18-04. Bits [1:0] what started the image, bit 2 whether UICR was read, bit 3 whether it
// said the pin reset is enabled, bits [12:8] the pin, bit 16 the port. Zero is "nothing is
// recorded here", which is every `booting` entry written before that task.
std::uint32_t encodeStart(const StartRecord& r) {
  std::uint32_t d = static_cast<std::uint32_t>(r.started) & 0x3u;
  if (r.uicr_read) d |= 1u << 2;
  if (r.pin_enabled) d |= 1u << 3;
  d |= static_cast<std::uint32_t>(r.pin & 0x1Fu) << 8;
  d |= static_cast<std::uint32_t>(r.port & 1u) << 16;
  return d;
}

StartRecord decodeStart(std::uint32_t detail) {
  StartRecord r;
  const std::uint32_t started = detail & 0x3u;
  r.started = started <= static_cast<std::uint32_t>(flasher::Started::kDebugPort)
                  ? static_cast<flasher::Started>(started)
                  : flasher::Started::kNothing;
  r.uicr_read = (detail & (1u << 2)) != 0;
  r.pin_enabled = (detail & (1u << 3)) != 0;
  r.pin = static_cast<std::uint8_t>((detail >> 8) & 0x1Fu);
  r.port = static_cast<std::uint8_t>((detail >> 16) & 1u);
  return r;
}

// ------------------------------------------------- what a failed flash says (task p18-05)
//
// The layout is in the header, field by field, and bit 31 is the whole of what makes an older
// entry read as it did.

std::uint32_t encodeFlashFailure(const FlashFailure& f) {
  std::uint32_t d = 1u << 31;
  d |= static_cast<std::uint32_t>(f.step) & 0x3Fu;
  d |= (static_cast<std::uint32_t>(f.status) & 0x3Fu) << 6;
  d |= (static_cast<std::uint32_t>(f.swd) & 0xFu) << 12;
  if (f.wrote_nothing) d |= 1u << 16;
  if (f.core_halted) d |= 1u << 17;
  if (f.refused) d |= 1u << 18;
  if (f.second_try) d |= 1u << 19;
  if (f.address_known) {
    d |= 1u << 28;
    d |= ((f.address >> 12) & 0xFFu) << 20;
  }
  return d;
}

FlashFailure decodeFlashFailure(std::uint32_t detail) {
  FlashFailure f;
  // An entry from before this task: the whole detail is the flasher's step. One written by it has
  // bit 31 set, and no `Step` reaches 0x80000000 by itself.
  const std::uint32_t step = (detail >> 31) != 0 ? (detail & 0x3Fu) : detail;
  if (step <= static_cast<std::uint32_t>(flasher::Step::kDone)) {
    f.step = static_cast<flasher::Step>(step);
  }
  if ((detail >> 31) == 0) return f;
  f.detailed = true;
  const std::uint32_t status = (detail >> 6) & 0x3Fu;
  if (status <= static_cast<std::uint32_t>(nrf52::Status::kSlotBusy)) {
    f.status = static_cast<nrf52::Status>(status);
  }
  const std::uint32_t swd_status = (detail >> 12) & 0xFu;
  if (swd_status <= static_cast<std::uint32_t>(swd::Status::kNoMemAp)) {
    f.swd = static_cast<swd::Status>(swd_status);
  }
  f.wrote_nothing = (detail & (1u << 16)) != 0;
  f.core_halted = (detail & (1u << 17)) != 0;
  f.refused = (detail & (1u << 18)) != 0;
  f.second_try = (detail & (1u << 19)) != 0;
  f.address_known = (detail & (1u << 28)) != 0;
  if (f.address_known) f.address = ((detail >> 20) & 0xFFu) << 12;
  return f;
}

FlashFailure failureOf(const flasher::Result& r) {
  FlashFailure f;
  f.detailed = true;
  f.step = r.step;
  f.status = r.nrf.status;
  f.swd = r.nrf.swd;
  f.wrote_nothing = r.wrote_nothing;
  f.core_halted = r.core_halted;
  f.refused = r.outcome == flasher::Outcome::kRefused;
  f.address_known = r.address != 0;
  f.address = r.address;
  return f;
}

std::size_t flashFailureLine(const FlashFailure& f, char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  // An entry from before this task carries the step and nothing else, and says so.
  if (!f.detailed) {
    const int n = std::snprintf(out, cap, "while %s (an entry from before this was recorded)",
                                flasher::describe(f.step));
    return n < 0 ? 0 : (static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1);
  }
  char at[24];
  at[0] = 0;
  if (f.address_known) {
    std::snprintf(at, sizeof at, " at 0x%08lx", static_cast<unsigned long>(f.address));
  }
  char under[80];
  under[0] = 0;
  if (f.status == nrf52::Status::kSwd) {
    std::snprintf(under, sizeof under, " (%s)", swd::describe(f.swd));
  }
  // A failure the layer under it called `kOk` is the flasher's own comparison: a page, the whole
  // image or the settings record read back and not matching (`flasher.cpp`, `programPage`,
  // `readBack`, `settingsWrite`). Saying *ok* there would be a lie about the only thing a person
  // reading this line wants.
  const char* status = f.refused               ? "refused by a check"
                       : f.status == nrf52::Status::kOk ? "what was read back did not match"
                                                        : nrf52::describe(f.status);
  const int n = std::snprintf(
      out, cap, "%swhile %s: %s%s%s; %s%s", f.second_try ? "the second try, " : "",
      flasher::describe(f.step), status, under, at,
      f.wrote_nothing ? "nothing was written" : "the chip was changed",
      f.core_halted ? ", the core was halted and started again" : "");
  return n < 0 ? 0 : (static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1);
}

std::size_t flashFailureNote(const FlashFailure& f, char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  if (!f.detailed) {
    const int n = std::snprintf(out, cap, "%s (step only)", flasher::name(f.step));
    return n < 0 ? 0 : (static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1);
  }
  char at[16];
  at[0] = 0;
  // Six hex digits: the chip is a megabyte, so a seventh would say nothing (PS "Memory").
  if (f.address_known) {
    std::snprintf(at, sizeof at, " at 0x%06lx", static_cast<unsigned long>(f.address) & 0xFFFFFFu);
  }
  char under[24];
  if (f.refused) {
    std::snprintf(under, sizeof under, "refused");
  } else if (f.status == nrf52::Status::kOk) {
    std::snprintf(under, sizeof under, "no-match");  // the flasher's own read-back comparison
  } else if (f.status == nrf52::Status::kSwd) {
    std::snprintf(under, sizeof under, "swd %s", swd::name(f.swd));
  } else {
    std::snprintf(under, sizeof under, "%s", nrf52::name(f.status));
  }
  const int n = std::snprintf(out, cap, "%s%s: %s%s; %s", f.second_try ? "2nd try " : "",
                              flasher::name(f.step), under, at,
                              f.wrote_nothing ? "nothing written" : "chip changed");
  return n < 0 ? 0 : (static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1);
}

bool decode(const Entry& e, Payload* out) {
  if (e.kind != kind::kRadioUpdate || e.size < kPayloadBytes || out == nullptr) return false;
  if (e.payload[0] > static_cast<std::uint8_t>(State::kNodeFault)) return false;
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
                                      (status_.resumed ? kFlagResumed : 0) |
                                      (units_fault_ ? kFlagUnitsFault : 0));
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
  r.detail = detail;  // task p18-05: a caller writing the server's line reads it from here
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
  // **A flashing state begun is a flash with no try spent** (task p18-05): the second try lives
  // inside `kFlashing` without a transition, so a transition *into* one - the candidate's, the
  // golden image's on a rollback, or either of them again at a resume - is a new flash, and one
  // of its own two tries.
  if (next == State::kFlashing || next == State::kRollingBack) {
    tries_ = 0;
    again_pending_ = false;
    again_wants_hello_ = false;
    status_.flash_tries = 0;
  }
  if (reason != Reason::kNone) reason_ = reason;
  status_.state = next;
  status_.reason = reason_;
  status_.step = step;
  r.after = next;
  r.changed = true;
  if (isTerminal(next)) journal_.unprotect();
  return r;
}

// A line of the story in the state the machine is already in (task p18-04): what the look found,
// and what the proof is waiting for. The journal refusing is what it is anywhere else - the
// machine stops where it is and a resume() finds the last entry it did write.
bool RadioUpdate::note(Reason reason, ProveStep step, std::uint32_t detail) {
  if (journal(state_, reason, step, detail)) return true;
  reason_ = Reason::kJournal;
  status_.reason = reason_;
  return false;
}

// The same line, as a step's answer rather than as a bool: the caller that writes the server's
// line reads the reason and the detail off a `StepResult`, so a line of the story has to be one
// (task p18-05).
StepResult RadioUpdate::noted(Reason reason, const char* note_text, std::uint32_t detail) {
  if (!note(reason, ProveStep::kNone, detail)) return waiting(describe(Reason::kJournal));
  StepResult r;
  r.before = state_;
  r.after = state_;
  r.changed = true;  // nothing moved, and the journal has a line: a caller posts it
  r.reason = reason;
  r.detail = detail;
  r.note = note_text;
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

StepResult RadioUpdate::refuse(Reason reason, std::uint32_t detail) {
  reason_ = reason;
  return enter(State::kRefused, describe(reason), reason, ProveStep::kNone, detail);
}

// A failure of the flash, the boot or the proof. From the candidate's path: rolling back - the
// hash on the bad list first, then the golden image. From the golden image's own path (a
// rollback, or a switch back): stuck. From a first install: stuck, "no image to return to".
StepResult RadioUpdate::fail(Reason reason, ProveStep step, std::uint32_t detail,
                             bool blame_the_image) {
  reason_ = reason;
  // **Whose fault it was** (task p18-05, deliverable 5), set before the entry is written so that
  // the entry carries the flag and a resume reads it back.
  units_fault_ = !blame_the_image;
  if (!rollback_image_) {
    status_.fail_reason = reason;
    status_.fail_step = step;
    status_.fail_detail = detail;
    status_.units_fault = units_fault_;
  }
  if (rollback_image_) {
    return enter(State::kStuck, describe(reason), reason, step, detail);
  }
  // The image that failed is never tried again (S-5): first thing, before any flash - and only
  // where the fault was the image's own or its proof's, which is where the specification's
  // sentence sits (§6's drawing: it is under the health check's *no*). A debug port that gave up,
  // a page that would not take, a core that would not halt are the unit's: the golden image goes
  // back as it always did, and that image may be flashed again.
  const image::StoreError bad =
      units_fault_ ? image::StoreError::kNone : store_.markBad(status_.sha256);
  if (request_.kind == Kind::kFirstInstall) {
    return enter(State::kStuck, describe(Reason::kNoImageToReturnTo), Reason::kNoImageToReturnTo,
                 step, detail);
  }
  if (bad != image::StoreError::kNone) {
    return enter(State::kStuck, describe(Reason::kStore), Reason::kStore, step, detail);
  }
  return enter(State::kRollingBack, describe(reason), reason, step, detail);
}

// A fault of the **node** and not of the image (task p10-03). The candidate booted, answered a
// HELLO that is its descriptor's, and then refused to say who it is: no image can mend a file
// system that will not mount, so the one thing a rollback would achieve is a second flash with
// the same answer at the end of it. Terminal at once: nothing on the bad list (the candidate is
// still a good image and may be flashed again once a person has typed `erase` and `reboot` at the
// radio), no promotion (it has not proved itself), and the reason in the journal.
StepResult RadioUpdate::nodeFault(Reason reason, ProveStep step, std::uint32_t detail) {
  reason_ = reason;
  if (!rollback_image_) {
    status_.fail_reason = reason;
    status_.fail_step = step;
  }
  return enter(State::kNodeFault, describe(reason), reason, step, detail);
}

bool RadioUpdate::nodeRefusesIdentity(std::uint8_t* status) {
  const link::client::Answer<link::IdentityResponse> id = radio_.identity();
  link::IdentityReading reading;
  reading.key = id.ok() && link::keyIsReal(id.message.pubkey.data, id.message.pubkey.size);
  reading.refused_state =
      id.fault == link::client::Fault::kRefused && id.status == link::status::kState;
  reading.caps = radio_.session().caps;
  if (status != nullptr) *status = id.status;
  return link::identityState(reading) == link::IdentityState::kLost;
}

// Check 2 failed. **Before it costs a rollback: is this the node and not the image?** (task
// p10-03.) A radio in the lost mode brings no clock up either, so TIME_SET and TIME_GET are both
// ERR_UNSUPPORTED to it - and a run that stopped at *the radio would not take the clock* would put
// a good image on the bad list and flash the golden one into a file system that will refuse it in
// exactly the same words. One §5.6 request, on a path that was going to end the run anyway, so a
// candidate whose clock really is broken fails as it always did.
StepResult RadioUpdate::failTime(std::uint32_t detail) {
  std::uint8_t status = 0;
  if (nodeRefusesIdentity(&status)) {
    return nodeFault(Reason::kIdentityLost, ProveStep::kIdentity, status);
  }
  return fail(Reason::kTimeRefused, ProveStep::kTime, detail);
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
  // Task p18-04: nothing of the last run's start is owed by this one.
  looked_ = false;
  can_look_ = false;
  pulse_started_ = false;
  over_a_scanner_ = false;
  watchdog_owed_ = false;
  watchdog_wait_ = false;
  started_ms_ = 0;
  // Task p18-05: no try, no pause and no blame carried over from the last run.
  tries_ = 0;
  again_pending_ = false;
  again_wants_hello_ = false;
  again_at_ms_ = 0;
  again_since_ms_ = 0;
  flash_detail_ = 0;
  units_fault_ = false;
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
  // **The hash before the first entry** (task p9-04). Every entry carries `status_.sha256`, and
  // until this read that field was filled by `stepCheck`'s `loadImage` - one step *after* the
  // `check` entry was written, so the first entry of every run named an image of 64 zero bits.
  // On the first unit `rak update status` had the 6 November run beginning
  // `check - first install, slot 0, image sha256 0000000000000000...`: the journal's story of a
  // run that ended *stuck* never said which image it was about, which is the one thing a person
  // reading it afterwards wanted (tasks/phase-8/FOLLOWUPS.md). The record alone, not
  // `loadImage`: no reader is opened and nothing is verified here - that is `stepCheck`'s
  // business and its refusals are unchanged. A record that is not there leaves the hash zero and
  // `stepCheck` refuses with *no candidate*, which is the honest entry for a run about nothing.
  image::Record first;
  if (store_.read(sourceOf(request_.kind), request_.slot, &first) == image::StoreError::kNone &&
      first.present) {
    for (std::size_t i = 0; i < image::kSha256Size; ++i) status_.sha256[i] = first.sha256[i];
    copyText(first.descriptor.version, status_.version, sizeof status_.version);
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
  // **The flash begins again from nothing, and the blame does not** (task p18-05). A Walter that
  // died in a flash resumes at `flashing` with no try spent - one flash, one possible second try
  // - but whose fault the failure that put it on the rollback path was is the journal's, and the
  // bad list must not be taken at the resume where it was not taken at the failure.
  tries_ = 0;
  again_pending_ = false;
  again_wants_hello_ = false;
  again_at_ms_ = 0;
  again_since_ms_ = 0;
  flash_detail_ = 0;
  units_fault_ = (p.flags & kFlagUnitsFault) != 0;
  status_.units_fault = units_fault_;
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
    // **A resumed boot does not know what started the image** (task p18-04), and two things
    // follow. One: an empty window is worth the look, because a Walter that died between the
    // pulse and the look leaves exactly the radio the look is for - halted where the flash left
    // it - and the look sends nothing to a core that is running. Two: a watchdog a scanner
    // started may still be counting, and the one proof that cannot afford a restart it did not
    // cause is the golden image's - a failure there is `stuck` and the end of a session - so that
    // proof waits the watchdog out once. Nothing of this is owed on the candidate's own path: a
    // restart there costs a rollback, which is what a rollback is for.
    looked_ = false;
    can_look_ = true;
    pulse_started_ = false;  // no pulse of this run's: no observation of the line is owed
    over_a_scanner_ = rollback_image_;
    watchdog_owed_ = rollback_image_;
    watchdog_wait_ = false;
    started_ms_ = window_started_ms_;
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
    case State::kNodeFault:
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

// **A flash that did not finish** (task p18-05). Three answers, and which of the three is decided
// by what the flasher *did* and not by the name of the step it stopped at:
//
//   the chip was changed - a page erased, a word written, the settings page touched - and the run
//   goes on as it always did: the golden image back, and the hash on the bad list only where the
//   fault was the image's own or its proof's (deliverable 5);
//   nothing was written and this was the first try: the same flash is made **once more**, after a
//   pause and after the radio's HELLO where the first try had halted it, with a line of the story
//   in the journal before it (deliverable 3);
//   nothing was written **twice**: the radio is exactly as the run found it, so there is nothing
//   to put back and nothing to blame the image for. The run ends `refused` - *nothing was
//   touched*, which is what that word has always meant here - with no flash of any kind and
//   nothing on the bad list (deliverable 4).
//
// The golden image's own flash is the one exception to the third: a run that is already rolling
// back has nothing to be refused into, and its ending is the one it is today - `stuck` - with the
// step and the status now said. The second try is made for it just the same.
//
// On 23 November the scanner's flash of 11:37:43Z failed twelve seconds in, having written
// nothing, and the machine answered by flashing the whole golden image back: nine minutes off the
// mesh, no line measured, and the scanner image on the bad list although it had never been on the
// chip. That is the behaviour this function replaces.
StepResult RadioUpdate::flashWroteNothing(const flasher::Result& r) {
  FlashFailure f = failureOf(r);
  f.second_try = tries_ > 1;
  const std::uint32_t detail = encodeFlashFailure(f);
  if (!r.wrote_nothing) {
    return fail(Reason::kFlashFailed, ProveStep::kNone, detail, flasher::blamesTheImage(r));
  }
  if (tries_ >= 2) {
    if (rollback_image_) {
      // The golden image going back, twice, with nothing written: `stuck`, as it is today.
      return fail(Reason::kFlashFailed, ProveStep::kNone, detail, false);
    }
    units_fault_ = true;  // the image never reached the chip: it is not what is wrong
    status_.fail_reason = Reason::kFlashDidNotBegin;
    status_.fail_step = ProveStep::kNone;
    status_.fail_detail = detail;
    status_.units_fault = true;
    return refuse(Reason::kFlashDidNotBegin, detail);
  }
  // Once more. The chip is as the flash found it, so there is nothing to undo: what stands between
  // here and the second try is the pause, and the radio's own HELLO where the first try halted the
  // core and started it again - a flash must not take the debug lines back from a radio in the
  // middle of its boot.
  flash_detail_ = detail;
  again_pending_ = true;
  again_wants_hello_ = r.core_halted;
  again_since_ms_ = clock_.nowMs();
  again_at_ms_ = again_since_ms_ + kFlashAgainPauseMs;
  next_hello_ms_ = again_since_ms_;
  if (r.core_halted) {
    // The image was started again at the flasher's exit: nothing this end believed about the
    // radio is from this boot.
    radio_.radioWasReset();
  }
  return noted(Reason::kFlashOnceMore, "the flash did not begin: once more after a pause", detail);
}

// The pause, and the HELLO it may be owed. One step a call, as everything here is.
StepResult RadioUpdate::waitBeforeTheSecondTry() {
  const std::uint64_t now = clock_.nowMs();
  if (now < again_at_ms_) return waiting("the pause before the second try at the flash");
  if (again_wants_hello_) {
    radio_.poll();
    bool up = radio_.session().up;
    bool text = radio_.mode() == link::client::Mode::kText;
    if (!up && !text && now >= next_hello_ms_) {
      next_hello_ms_ = now + kBootHelloIntervalMs;
      up = radio_.hello().ok();
      text = radio_.mode() == link::client::Mode::kText;
    }
    if (!up && !text && now - again_since_ms_ < kBootWindowMs) {
      return waiting("waiting for the radio the first try halted and started again");
    }
    // Either it answered, or its window is up. The second try is made in both cases: a flash
    // needs no link, and a radio that will not speak is the thing a flash is for.
    again_wants_hello_ = false;
  }
  again_pending_ = false;
  return waiting("the flash, once more");
}

StepResult RadioUpdate::stepFlashing(bool rollback) {
  if (reason_ == Reason::kJournal) return waiting(describe(reason_));
  const flasher::BoardProfile* profile = flasher::profileForSlot(request_.slot);
  if (profile == nullptr) return fail(Reason::kNoProfile, ProveStep::kNone);
  // **The pause before the second try** (task p18-05): one step at a time, as everything here is.
  if (again_pending_) return waitBeforeTheSecondTry();
  if (rollback) {
    // The bad list first - again on a resume, where it is a no-op - then the golden image. Not
    // where the fault was the unit's (task p18-05, deliverable 5): `fail()` did not take it and
    // this must not either, on the first pass or on a resume that reads the flag back.
    if (request_.kind != Kind::kRoleRepeater && !units_fault_) {
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
  // **What the radio was running when this flash began** (task p18-04, deliverable 4), read
  // before the flash because afterwards there is nothing left to read it from: a scanner is the
  // one image that starts the chip's watchdog, and a watchdog it started outlives anything but a
  // pin, a brownout or a power-on reset.
  //
  // On a second try (task p18-05) the radio may be dark - the first try halted it and the pulse
  // that started it again may have reached nothing - and a dark radio reads as "not a scanner".
  // So the first try's answer stands and the second can only add to it.
  const bool scanner_now = radio_.mode() == link::client::Mode::kText ||
                           (radio_.session().up && radio_.session().role == link::role::kScanner);
  over_a_scanner_ = tries_ == 0 ? scanner_now : (over_a_scanner_ || scanner_now);
  const flasher::Result r = flasher_.flash(image, *profile, progress_, request_.slot);
  ++tries_;
  status_.flash_tries = tries_;
  if (!r.ok()) return flashWroteNothing(r);
  // **The boot that is proved is the boot the flasher started** (task p18-01), and since task
  // p18-04 the flash says which of the two started it. The flasher's last act is one reset and
  // only one - a pulse on the reset line where the module has a reset pin, a soft reset over the
  // debug port where UICR says it has not - so there is no earlier boot whose EV_BOOT could be
  // read here as this one's, and none whose late arrival inside the proving period would read as
  // a restart. The window begins now, which is after that reset, and everything the client
  // believed about the radio goes first.
  //
  // Two things are owed from here (task p18-04):
  //   a pulse gets no confirmation, so an empty window is worth one look at the chip;
  //   a soft reset leaves a scanner's watchdog counting, and the image that boots feeds nothing
  //   unless it is a scanner itself, so the proof waits that watchdog out.
  started_ms_ = clock_.nowMs();
  looked_ = false;
  pulse_started_ = r.started == flasher::Started::kResetLine;
  // A pulse is the start nothing confirms, so it is the start a look is for; a soft reset over
  // the debug port answered S_RESET_ST before it returned and needs none.
  can_look_ = pulse_started_;
  watchdog_owed_ = r.started == flasher::Started::kDebugPort && over_a_scanner_ &&
                   request_.kind != Kind::kRoleScanner;
  watchdog_wait_ = false;
  status_.started = r.started;
  status_.looked = false;
  status_.core_at_look = nrf52::CoreState::kUnknown;
  status_.waited_for_watchdog = false;
  radio_.radioWasReset();
  window_started_ms_ = started_ms_;
  next_hello_ms_ = window_started_ms_;
  restart_seen_ = false;
  // The entry carries what started the image and what UICR said about the reset pin, so that a
  // flash nobody watched can be read off the journal afterwards (task p18-04).
  StartRecord record;
  record.started = r.started;
  record.uicr_read = r.reset_pin.known;
  record.pin_enabled = r.reset_pin.enabled;
  record.pin = r.reset_pin.pin;
  record.port = r.reset_pin.port;
  return enter(rollback ? State::kRollbackBooting : State::kBooting, "flashed; the radio was reset",
               Reason::kNone, ProveStep::kNone, encodeStart(record));
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
    // **The pulse worked** (task p18-04, deliverable 5): the radio is there, and a boot inside
    // the window after a pulse is the only evidence this firmware can have that the reset line
    // reaches the chip. Said once, and the record is a person's to read.
    if (pulse_started_) {
      flasher_.noteResetLine(true);
      pulse_started_ = false;
    }
    // **And the watchdog a scanner left behind** (deliverable 4). The image that is answering was
    // started by a soft reset over a scanner, so the watchdog that scanner started is still
    // counting and this image feeds nothing: it will be reset once, inside kScannerWatchdogMs.
    // The proving period begins at the boot that follows that reset - a restart in the proof is a
    // failure, and this one is not the image's fault - or when the watchdog's longest time has
    // passed without one.
    if (watchdog_owed_) {
      watchdog_owed_ = false;
      watchdog_wait_ = true;
      status_.waited_for_watchdog = true;
      // The boot this is: its own EV_BOOT is a restart to everything that watches, and it is not
      // the one being waited for.
      restart_seen_ = false;
    }
    if (watchdog_wait_) {
      if (restart_seen_) {
        watchdog_wait_ = false;
        if (!note(Reason::kAfterTheWatchdog, ProveStep::kHello,
                  static_cast<std::uint32_t>(clock_.nowMs() - started_ms_))) {
          return waiting(describe(Reason::kJournal));
        }
        radio_.radioWasReset();
        window_started_ms_ = clock_.nowMs();
        next_hello_ms_ = window_started_ms_;
        restart_seen_ = false;
        return waiting("the scanner's watchdog reset the radio: the boot after it is the one to prove");
      }
      if (clock_.nowMs() - started_ms_ < kScannerWatchdogWaitMs) {
        return waiting("waiting out the watchdog the scanner left running");
      }
      watchdog_wait_ = false;
      if (!note(Reason::kWatchdogTimePassed, ProveStep::kHello,
                static_cast<std::uint32_t>(clock_.nowMs() - started_ms_))) {
        return waiting(describe(Reason::kJournal));
      }
    }
    beginProving();
    return enter(rollback ? State::kRollbackProving : State::kProving, "the radio answered");
  }
  if (clock_.nowMs() - window_started_ms_ >= window) {
    // **Nothing came, and the flash left by a pulse nothing confirms** (task p18-04, deliverable
    // 3): ask the chip what that pulse did, once a flash, before this is called a dead image.
    if (can_look_ && !looked_) return lookAtTheCore(scanner);
    return fail(scanner ? Reason::kNotAScanner : Reason::kNoBoot, scanner ? ProveStep::kScanner : ProveStep::kHello);
  }
  return waiting("waiting for the radio to boot");
}

// The look (task p18-04, deliverable 3). One a flash: `looked_` is set first, so every path out
// of here is a path that cannot come back - a second empty window is the ending it always was.
StepResult RadioUpdate::lookAtTheCore(bool scanner) {
  looked_ = true;
  status_.looked = true;
  const Reason empty = scanner ? Reason::kNotAScanner : Reason::kNoBoot;
  const ProveStep step = scanner ? ProveStep::kScanner : ProveStep::kHello;
  const flasher::BoardProfile* profile = flasher::profileForSlot(request_.slot);
  if (profile == nullptr) return fail(Reason::kNoProfile, ProveStep::kNone);
  const flasher::Result r = flasher_.startAgain(*profile, request_.slot);
  status_.core_at_look = r.core;
  if (!r.ok()) {
    // The port is closed, silent or protected: the same ending as before this task, and the
    // journal says that the question could not be put.
    if (!note(Reason::kLookFailed, step, static_cast<std::uint32_t>(r.step))) {
      return waiting(describe(Reason::kJournal));
    }
    return fail(empty, step);
  }
  if (r.started != flasher::Started::kDebugPort) {
    // The core is running, sleeping or locked up: the pulse did reset the chip, and it is the
    // image that is not speaking. Nothing was sent to it, and the ending is the one there is.
    flasher_.noteResetLine(true);
    pulse_started_ = false;
    if (!note(Reason::kPulseStarted, step, static_cast<std::uint32_t>(r.core))) {
      return waiting(describe(Reason::kJournal));
    }
    return fail(empty, step);
  }
  // The core is still halted where the flash left it: **the pulse reached nothing**. The image
  // has been started through the debug port, the line's record says so for a person, and the
  // radio gets a window of its own to answer in - with the scanner's watchdog now owed, because a
  // soft reset does not clear one.
  flasher_.noteResetLine(false);
  pulse_started_ = false;
  status_.started = flasher::Started::kDebugPort;
  if (!note(Reason::kStartedThroughPort, step, static_cast<std::uint32_t>(r.core))) {
    return waiting(describe(Reason::kJournal));
  }
  started_ms_ = clock_.nowMs();
  watchdog_owed_ = over_a_scanner_ && request_.kind != Kind::kRoleScanner;
  watchdog_wait_ = false;
  radio_.radioWasReset();
  window_started_ms_ = started_ms_;
  next_hello_ms_ = window_started_ms_;
  restart_seen_ = false;
  return waiting("the pulse reached nothing: the image was started through the debug port");
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
      // **Check 1a, and not a frame on the wire unless §5.1 says to look** (task p10-03).
      // Between this check and the clock's, this end may make no call of its own: the client
      // sends a session's pending TIME_SET before any other request (§5.4), so an IDENTITY
      // asked here would have check 2 reading a TIME_GET where it means to read a TIME_SET. The
      // lost mode advertises `caps 0x21` and nothing else, so that is where it is looked for,
      // and a healthy radio's proof is what it always was, request for request.
      if (link::capsSayIdentityLost(radio_.session().caps)) {
        std::uint8_t status = 0;
        if (nodeRefusesIdentity(&status)) {
          return nodeFault(Reason::kIdentityLost, ProveStep::kIdentity, status);
        }
      }
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
        if (!got.ok()) return failTime(0);
        status_.time_skipped = true;
        prove_step_ = ProveStep::kStatus;
        return waiting("no time to give: the clock check is skipped, TIME_GET answered");
      }
      if (sent == lc::Fault::kNoSession && !pending) {
        // Nothing pending: the clock was set already in this session (the client does it on
        // the first call after a restart). TIME_GET must still answer.
        const lc::Answer<link::TimeGetResponse> got = radio_.timeGet();
        if (!got.ok()) return failTime(0);
        prove_step_ = ProveStep::kStatus;
        return waiting("the clock was already set");
      }
      return failTime(sent == lc::Fault::kRefused ? 1u : 0u);
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
    // Check 1a is made inside the HELLO step and named only when it fails, as kScanner is.
    case ProveStep::kIdentity:
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
