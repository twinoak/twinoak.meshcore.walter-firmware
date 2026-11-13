// The backend: the one seam between this firmware and whoever tells it what to do (task p3-05;
// spec/SYSTEM.md §8, docs/cycle.md).
//
//     exchange(what we know, a deadline) -> what is asked | nothing | failed
//
// **The implementation in this repository does nothing** (core/cycle/null_backend.h). That is
// deliberate and is the whole point of the seam: the server side is three proposals and a
// decision taken on 14 October 2026 about where it will be built, not a protocol
// (spec/proposals/, spec/SYSTEM.md §8). Code written here against a guess would have to be found
// and removed later, so nothing here names a server, a URL path, a JSON field, a job type or a
// job id - those belong to whoever implements this interface, which is the Walter's phase 4.
//
// -- what crosses it ---------------------------------------------------------------------------
//
// **One direction is a plain structure this firmware fills.** Everything the unit knows about
// itself that somebody far away could want: who it is as the radio gave it, what both MCUs are
// running, what is in the image store, what the supervisor has had to do, the readings kept
// since the last time anybody took them, how this wake went, and how good the signal was. It is
// plain data - fixed arrays, no pointers, no heap - because it is filled once a wake by a
// firmware that has no allocator and may be read by an implementation that keeps none of it.
//
// **The other direction is a closed set of requests.** Each one is something *this firmware
// already knows how to carry out by itself*, and there is nothing else:
//
//   fetch a release from this base URL into the candidate (or scanner) slot   core/update/fetch.h
//   run the radio's update                                            core/update/radio_update.h
//   run the Walter's own update                                        core/update/self_update.h
//   set these settings, non-secret keys only                           core/settings/settings.h
//   climb the supervisor's ladder                                    core/supervisor/supervisor.h
//   nothing
//
// A request outside that set cannot be expressed, which is the point: an implementer of this
// interface cannot make this firmware do something it has no tested procedure for. An answer may
// ask for more than one of them; the wake cycle carries out at most one update step per wake
// (S-7) and records what it refused, which the next exchange is told.
//
// -- what this is not --------------------------------------------------------------------------
//
// Not a transport: `exchange()` may be HTTPS, the mesh, a cable or nothing at all. Not a
// protocol: there are no field names here, only this firmware's own words. Not a scheduler: when
// an exchange happens is the wake cycle's business (docs/cycle.md step 8), and the deadline is
// the wake's budget, not a timeout the implementation picks.
//
// Plain data and pure virtual, like every other file of interfaces/: <cstddef> and <cstdint> and
// nothing else (host_test/core/cycle/test_cycle_no_allocation.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

// -- the sizes, each with a sentence -----------------------------------------------------------
//
// Every one of them bounds a fixed array, because this structure is a member of an object in
// static storage and is filled by a firmware with no heap.

// A version string: `0.3.0+abc1234` is 13 characters, and the link's HELLO allows longer ones
// which the client keeps truncated (core/link/client/client.h, kVersionMax).
constexpr std::size_t kBackendVersionMax = 24;
// A board name: `rak4630-single` is 14.
constexpr std::size_t kBackendBoardMax = 20;
// An advert name: MeshCore's own limit is shorter than this.
constexpr std::size_t kBackendNameMax = 24;
// A MeshCore public key (spec/LINK-PROTOCOL.md §5.6).
constexpr std::size_t kBackendPubkeySize = 32;
// How much of an image's SHA-256 travels: the first eight bytes name an image among the handful a
// unit has ever held, and the full hash is the store's own business (core/image/store.h). A
// backend that wants to know *which* release a unit holds has the version and the commit.
constexpr std::size_t kBackendHashShown = 8;
// How many of the supervisor's incidents travel: the three it keeps in retained storage
// (core/supervisor/supervisor.h, kIncidentsKept). The journal keeps the rest.
constexpr std::size_t kBackendIncidents = 3;
// How many readings travel: the whole of the telemetry ring (core/cycle/telemetry_ring.h).
constexpr std::size_t kBackendReadings = 10;
// How many settings one answer may ask to change. Four: a unit is provisioned at a console
// (docs/provisioning.md), and this is for the one key a fleet changes its mind about.
constexpr std::size_t kBackendSettingsMax = 4;
// A base URL to fetch a release from: `core/settings/keys.h`'s own bound for a URL a person
// types, which is shorter than the HTTPS client's (a redirect's signed URL is the client's
// business, not an answer's).
constexpr std::size_t kBackendUrlMax = 128;
// A setting's name and value: `core/settings/keys.h`'s own bounds.
constexpr std::size_t kBackendKeyMax = 24;
constexpr std::size_t kBackendValueMax = 160;
// The wake record's letters, as text (docs/cycle.md): one per step and a terminator. Text and
// not an enum array, so that this seam does not have to agree with core/ about how many steps
// there are; core/cycle/wake_cycle.h static_asserts that its own count fits.
constexpr std::size_t kBackendStepLetters = 16;

// -- what we know ------------------------------------------------------------------------------

// The unit's identity, as the radio gave it (§5.6). The repeater's key *is* the unit's key
// (spec/SYSTEM.md S-1), so there is one of these and not two.
struct BackendIdentity {
  bool known = false;  // false: the radio did not answer IDENTITY in this wake or any before it
  std::uint8_t pubkey[kBackendPubkeySize] = {};
  char name[kBackendNameMax] = {};
};

// One MCU's firmware: what it says it is, and the descriptor of the image it was flashed from
// where this firmware has one (spec/FIRMWARE-MANIFEST.md §4).
struct BackendFirmware {
  bool known = false;
  char version[kBackendVersionMax] = {};
  char board[kBackendBoardMax] = {};
  std::uint8_t role = 0;         // core::link::role::*
  std::uint8_t link_major = 0;   // the link version this MCU speaks (§5.1)
  std::uint8_t link_minor = 0;
  bool descriptor_known = false;
  char descriptor_version[kBackendVersionMax] = {};
  std::uint8_t hash[kBackendHashShown] = {};
};

// One slot of the image store (core/image/store.h): what it holds, or how far an interrupted
// transfer into it got.
struct BackendImage {
  bool present = false;
  char version[kBackendVersionMax] = {};
  std::uint8_t hash[kBackendHashShown] = {};
  std::uint32_t size = 0;
  std::uint32_t stored_at = 0;    // epoch seconds; 0 when the unit had no time of day then
  bool transfer = false;          // not present, and a transfer into it was interrupted
  std::uint32_t transferred = 0;  // how many of its bytes are in flash
  std::uint32_t transfer_size = 0;
};

// What the supervisor had to do about the radio (core/supervisor/supervisor.h). Compact on
// purpose: the whole incident is in the journal, and this is what a check-in can afford.
struct BackendIncident {
  bool present = false;
  std::uint8_t cause = 0;        // core::supervisor::Cause
  std::uint8_t outcome = 0;      // core::supervisor::Outcome
  std::uint8_t rungs_tried = 0;  // a bit per rung
  std::uint8_t stop = 0;         // core::supervisor::StopReason
  std::uint32_t began_epoch = 0; // 0 when the unit had no time of day
  std::uint32_t lasted_ms = 0;
  std::uint16_t battery_mv = 0;
};

// One reading of the radio's telemetry (§5.8), kept since the last time anybody took them.
// `time_known` false is a reading taken before this unit knew what day it was: it is **marked as
// such and never guessed at**, and its order in the array is all there is to go by.
struct BackendReading {
  bool time_known = false;
  std::uint32_t at_epoch = 0;
  bool battery_known = false;
  std::uint16_t battery_mv = 0;
  bool current_known = false;
  std::int16_t battery_ma = 0;  // positive = charging (§5.8)
  bool temp_known = false;
  std::int16_t temp_c = 0;      // whole degrees Celsius
  std::uint8_t wake = 0;        // core::cycle::Wake: why the unit was awake when it was taken
};

// How this wake went: the compact record docs/cycle.md documents, as fields.
struct BackendWake {
  std::uint8_t reason = 0;                      // core::cycle::Wake
  std::uint8_t attention = 0;                   // §8's reason when the radio asked for us
  std::uint8_t radio = 0;                       // core::supervisor::Health
  std::uint8_t band = 0;                        // core::power::Band
  char steps[kBackendStepLetters] = {};         // one letter per step, in order
  std::uint32_t wakes = 0;                      // wakes counted since the retained record began
  std::uint32_t awake_s = 0;
  std::uint32_t next_interval_s = 0;
  std::uint32_t faults_in_a_row = 0;
  bool on_trial = false;                        // this firmware is proving itself (S-6)
  bool update_running = false;                  // an update of either MCU is mid-run
};

// What the uplink managed. No IMEI, no ICCID, no IMSI: they identify a subscription and are read
// only when a person asks (core/uplink/policy.h, *Privacy*).
struct BackendUplink {
  bool up = false;
  std::uint8_t failure = 0;     // core::uplink::Failure when it is not up
  std::uint8_t reached = 0;     // core::uplink::Step: how far the sequence got
  bool signal_known = false;
  bool rsrp_known = false;
  std::int16_t rsrp_dbm = 0;
  bool rsrq_known = false;
  std::int16_t rsrq_tenth_db = 0;
  bool rssi_known = false;
  std::int16_t rssi_dbm = 0;
  std::uint8_t band = 0;        // the LTE band, 0 when unknown
  std::uint32_t spent_ms = 0;
};

// An ask of an earlier wake that this firmware refused: what was asked, and why. Kept across the
// sleep and told at the next exchange (core/cycle/telemetry_ring.h), because "I did not do what
// you asked" is the one thing a backend cannot work out for itself.
struct BackendRefusal {
  std::uint8_t ask = 0;  // an Ask
  std::uint8_t why = 0;  // a Refusal
};

// Everything above, filled once a wake.
struct BackendKnowledge {
  // This firmware, and the radio's.
  BackendFirmware walter;
  BackendFirmware radio;
  BackendIdentity identity;
  // The store: the golden image, the candidate and the scanner image of the one radio a unit has
  // (spec/SYSTEM.md S-11).
  BackendImage golden;
  BackendImage candidate;
  BackendImage scanner;
  bool store_there = false;  // false: this build has no image store
  // What the radio has been doing.
  std::uint32_t radio_uptime_s = 0;
  std::uint8_t radio_reset_reason = 0;  // §5.3
  std::uint8_t radio_time_state = 0;    // §5.4
  std::uint32_t radio_epoch = 0;
  std::uint16_t radio_flags = 0;        // §5.3
  std::uint8_t obs_pending = 0;         // §7: observations waiting in the radio's ring
  std::uint16_t obs_dropped = 0;
  BackendIncident incident[kBackendIncidents];
  std::uint32_t incidents = 0;          // how many of incident[] are real
  bool supervisor_stopped = false;      // rung 5 is in force
  // The readings nobody has taken yet, oldest first, and how many were dropped to make room.
  BackendReading reading[kBackendReadings];
  std::size_t readings = 0;
  std::uint32_t readings_dropped = 0;
  // This wake, the uplink, and what was refused since the last exchange.
  BackendWake wake;
  BackendUplink uplink;
  BackendRefusal refusal[kBackendSettingsMax];
  std::size_t refusals = 0;
  // The time of day, when this unit has one, and the battery's own figures.
  bool time_known = false;
  std::uint32_t epoch = 0;
  bool battery_known = false;
  std::uint16_t battery_mv = 0;
  std::int16_t battery_ma = 0;
  bool charging = false;
};

// -- what is asked -----------------------------------------------------------------------------

// The closed set. A value outside it cannot be built, and `kCount` is not one of them.
enum class Ask : std::uint8_t {
  kNothing = 0,
  kFetchRelease = 1,  // `base_url`, into the candidate or the scanner slot
  kUpdateRadio = 2,   // the stored candidate onto the radio, health-checked, kept or rolled back
  kUpdateSelf = 3,    // the image in the other application slot made the next to boot (S-6)
  kSetSettings = 4,   // `setting[0 .. settings)`, non-secret keys only
  kRunLadder = 5,     // the supervisor's ladder, now
  kCount,
};

inline const char* describe(Ask ask) {
  switch (ask) {
    case Ask::kNothing: return "nothing";
    case Ask::kFetchRelease: return "fetch a release";
    case Ask::kUpdateRadio: return "update the radio";
    case Ask::kUpdateSelf: return "update the walter";
    case Ask::kSetSettings: return "change a setting";
    case Ask::kRunLadder: return "climb the ladder";
    case Ask::kCount: break;
  }
  return "?";
}

// The bit of an ask in `BackendAnswer::asks`.
constexpr std::uint8_t askBit(Ask ask) {
  return static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(ask));
}

// Why this firmware did not do what was asked. Each one is a thing the firmware decides for
// itself and the backend cannot: the battery, S-7, the table of keys, what this build has.
enum class Refusal : std::uint8_t {
  kNone = 0,
  kBandForbids = 1,    // the battery's band does not allow it (core/power/interval_policy.h)
  kOneUpdateOnly = 2,  // S-7: the other MCU's update went first this wake
  kUnconfirmed = 3,    // an update of the other MCU is journalled as unconfirmed (S-7)
  kSecretKey = 4,      // a secret is never set from here: it is a person's at the console
  kUnknownKey = 5,     // no such setting - a threshold that protects the hardware is not one
  kBadValue = 6,       // the value is outside what the key's row allows
  kNotBuilt = 7,       // this build has nothing to do it with (no store, no journal, no modem)
  kNoTime = 8,         // the wake's budget ran out before it could be begun
  kFailed = 9,         // it was begun and did not work; the journal says what happened
  kBadUrl = 10,        // not an https base URL, or longer than kBackendUrlMax
  kCount,
};

inline const char* describe(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone: return "no refusal";
    case Refusal::kBandForbids: return "the battery's band does not allow it";
    case Refusal::kOneUpdateOnly: return "one update a wake, and the other one went";
    case Refusal::kUnconfirmed: return "an update of the other MCU is unconfirmed";
    case Refusal::kSecretKey: return "a secret is never set from here";
    case Refusal::kUnknownKey: return "no such setting";
    case Refusal::kBadValue: return "the value is not one that key takes";
    case Refusal::kNotBuilt: return "this build cannot do it";
    case Refusal::kNoTime: return "the wake's budget ran out";
    case Refusal::kFailed: return "it was tried and failed";
    case Refusal::kBadUrl: return "not an https base url, or too long";
    case Refusal::kCount: break;
  }
  return "?";
}

// -- was anybody there? (task p10-02) -----------------------------------------------------------
//
// `Exchanged` has three values and they answer two questions at once, which is how the journal
// came to say *the backend was reached* about wakes that sent nothing. `kNothing` is both "it
// talked and there was nothing to do" and "it never opened its mouth", and those are not the
// same fact: the first means a server heard from this unit, the second means it did not.
//
// So this is the second half of the answer, and the three the firmware's words have to tell
// apart are **answered**, **sent nothing (and why)** and **failed**:
//
//   answered       kAsked or kNothing, with `reached` == kAnswered
//   sent nothing   kNothing, with `reached` saying which of the reasons it was
//   failed         kFailed
//
// **A reason beside `kNothing`, and not a fourth `Exchanged`**, for two reasons. One value more
// could not carry *why* - and the why is the whole point: a hold, no uplink, nothing configured
// and a halt on a 401 are four different things to do about it. And `Exchanged`'s three values
// are what the wake cycle's rails are written against (S-7's one update step, the budget, the
// sleep a wait lengthens: all of them turn on kFailed and nothing else), so widening the enum
// would move code that has nothing to do with this.
//
// **kUnsaid is the zero and it is not a yes.** `out` is zeroed by the caller, so an
// implementation that says nothing is never credited with having reached anybody - which is the
// rail: the journal's *reached* is a claim about a server, and a claim nobody made is false.
enum class Reached : std::uint8_t {
  kUnsaid = 0,          // the implementation did not say: nothing may be claimed
  kAnswered = 1,        // a heartbeat went out and the backend answered it
  kWait = 2,            // nothing was sent: a wait is still owed (`sleep_at_least_s` is what is left)
  kNoUplink = 3,        // nothing was sent: there was no uplink to send it over
  kNotConfigured = 4,   // nothing was sent: this unit has no server to send it to
  kHalted = 5,          // nothing was sent: a 401, and a person has to act
  kElse = 6,            // nothing was sent, for a reason this seam has no word for
  // nothing was sent: the **uplink's own** backoff skipped the modem this wake
  // (core/uplink/policy.h, `mayTry()`), so the seam was never reached at all. **Not a value this
  // seam ever answers** - a backend that was not called says nothing - but one the wake cycle
  // writes for it (task p13-02, core/cycle/wake_cycle.cpp): step 8 needs step 6, and a wake the
  // backoff skipped left *it did not say* where a person was looking for a reason. It is in this
  // enum and not beside it so that the journal, the wake record and `wake`'s list have one
  // vocabulary for *why nothing was sent* and not two.
  kUplinkBackoff = 7,
  kCount,
};

// One phrase, for the journal, the wake record and the console.
inline const char* describe(Reached reached) {
  switch (reached) {
    case Reached::kUnsaid: return "it did not say";
    case Reached::kAnswered: return "the backend answered";
    case Reached::kWait: return "a wait still owed";
    case Reached::kNoUplink: return "no uplink";
    case Reached::kNotConfigured: return "not configured";
    case Reached::kHalted: return "halted on a 401";
    case Reached::kElse: return "nothing to send";
    case Reached::kUplinkBackoff: return "the uplink's backoff";
    case Reached::kCount: break;
  }
  return "?";
}

// One setting an answer asks for. The key is the name a person would type at the console
// (`core/settings/keys.h`), because that table is the only list of settings there is.
struct BackendSetting {
  char key[kBackendKeyMax] = {};
  char value[kBackendValueMax] = {};
};

// What is asked. `asks` is a set, not one request: a backend may want a release fetched and the
// radio updated in the same breath, and the wake cycle is what decides the order and what fits
// in this wake (S-7: at most one update step).
struct BackendAnswer {
  std::uint8_t asks = 0;  // askBit(...) of each
  bool wants(Ask ask) const { return (asks & askBit(ask)) != 0; }
  void want(Ask ask) { asks = static_cast<std::uint8_t>(asks | askBit(ask)); }

  // kFetchRelease.
  char base_url[kBackendUrlMax] = {};
  bool into_scanner = false;  // the scanner slot rather than the candidate's

  // kSetSettings.
  BackendSetting setting[kBackendSettingsMax];
  std::size_t settings = 0;

  // The readings of `BackendKnowledge::reading` are now the backend's: this firmware may forget
  // them. **The ring is emptied only by this** - an exchange that failed, or one whose answer
  // does not say so, leaves every reading where it was (core/cycle/telemetry_ring.h).
  bool took_readings = false;

  // -- what the exchange carried out itself (task p4-05) ----------------------------------------
  //
  // An implementation may run one of the closed set *inside* the exchange rather than ask for it
  // - the agent's firmware jobs do: a release fetched, the radio or the Walter updated, each on
  // the same machine the wake cycle's step 9 would have driven, because the server wants the
  // result in the same wake. The wake cycle has to know, for the two rails it owns around the
  // seam: S-7's one update step a wake (`carried` is the update that went, and step 9 refuses a
  // second), and the budget (`outside_ms` is time a bounded machine of its own spent, outside the
  // wake's ordinary budget as a climb's and an update's are). `did` is askBit() of everything
  // carried out, for the record; nothing in it is asked for again.
  std::uint8_t did = 0;
  Ask carried = Ask::kNothing;  // the one update step that went, when one did
  std::uint32_t outside_ms = 0;
  // How long the next sleep must be at least, in seconds, when whoever is behind the seam owes
  // a wait: a backoff after a server that did not answer, a Retry-After it was given, or an
  // interval the server accepted that is *longer* than this firmware's own. Never shorter: the
  // interval is the battery's (core/power/interval_policy.h) and this can only lengthen it.
  std::uint32_t sleep_at_least_s = 0;

  // -- what the wake did (task p10-02) ----------------------------------------------------------
  //
  // Whether anybody was reached, and when nobody was, why nothing was sent. **Written whatever
  // the exchange answers**, kFailed apart: a wake that sent nothing is not a failure and the
  // reason it sent nothing is the one thing a reader cannot work out from the rest of the record.
  Reached reached = Reached::kUnsaid;
  // How much work of its own the exchange did this wake, saturating at 255 - the agent's jobs,
  // for an implementation that has jobs; 0 for one that has none, as this repository's has. One
  // letter of `wake`'s list is this (core/cycle/wake_reason.h, kDidJobs) and nothing else reads
  // it: it is a count for a person, not a figure anything decides on.
  std::uint8_t jobs_ran = 0;
};

// How an exchange went.
enum class Exchanged : std::uint8_t {
  kAsked = 0,    // it worked and the answer is in `out`
  kNothing = 1,  // it worked and there is nothing to do
  kFailed = 2,   // it did not work: no answer, and nothing is assumed
};

inline const char* describe(Exchanged outcome) {
  switch (outcome) {
    case Exchanged::kAsked: return "asked for something";
    case Exchanged::kNothing: return "nothing to do";
    case Exchanged::kFailed: return "the exchange failed";
  }
  return "?";
}

// **The one reading of the seam's answer** (task p10-02), so that the journal, the wake record,
// the console and a test cannot each have their own. Was a backend reached and did it answer?
// Nothing else is *reached*: not an exchange that failed, not one that sent nothing, and not one
// that did not say.
inline bool wasAnswered(Exchanged how, const BackendAnswer& answer) {
  return how != Exchanged::kFailed && answer.reached == Reached::kAnswered;
}

class Backend {
 public:
  virtual ~Backend() = default;

  // One exchange, which must return by `deadline_ms` on the same monotonic millisecond clock
  // core/ uses (interfaces/clock.h). `out` is zeroed by the caller; the asks are written only
  // when the answer is kAsked, and **`reached`, `did`, `carried`, `outside_ms`,
  // `sleep_at_least_s` and `jobs_ran` are written whenever the exchange did not fail** - they
  // say what happened rather than what is wanted, and a wake that sent nothing has those to say.
  //
  // An implementation may do nothing at all, which is what this repository's does. It must not
  // block past the deadline: the wake's budget is what the battery can pay for, and a backend
  // that sits on a modem is a unit that does not wake again (docs/cycle.md, *the budget*).
  virtual Exchanged exchange(const BackendKnowledge& known, std::uint64_t deadline_ms,
                             BackendAnswer* out) = 0;
};

}  // namespace interfaces
