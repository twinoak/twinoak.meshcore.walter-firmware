// The uplink's policy (task p3-01; docs/ARCHITECTURE.md §9, §10): the sequence of getting onto
// the LTE-M network, the deadline of every step of it, the budget the whole of it may spend in
// one wake, and - when it did not work - which of the five named failures it was and when this
// firmware may try again (core/uplink/failure.h).
//
// **This is what everything above phase 3 sees of the modem**: bring it up, what time is it, how
// good is the signal, who am I, what power saving did the network grant, take it down. The modem
// library is behind interfaces::Uplink and nowhere else, so that the question of whether the
// Walter stays (spec/SYSTEM.md §10 item 6) costs a port and not a rewrite.
//
// -- the sequence ------------------------------------------------------------------------------
//
// The order is the one QuickSpot's own examples use (managed_components/dptechnics__walter-modem/
// examples/https/main/https.cpp, `lteConnect`), with the SIM and the RAT read in the state where
// the modem will answer about them:
//
//   1  is the modem there       `AT`. Silence is the one failure with a remedy inside the wake:
//                               the modem is reset, once, and asked again.          modem-silent
//   2  awake, no radio          `AT+CFUN=4`: the SIM is reachable and nothing transmits.
//                                                                                   modem-silent
//   3  the RAT                  LTE-M, unless a setting says otherwise. Set only when it is
//                               wrong, and then the modem is reset (WalterModem.h, softReset:
//                               "required when switching from RAT") - which is why it comes
//                               *before* the SIM: a reset loses a card that was just unlocked.
//   4  the SIM                  ready, absent, or locked. A PIN is sent at most once
//                               per PIN - see *The PIN* below.                   no-sim/sim-locked
//   5  the context              `AT+CGDCONT` with the APN from settings - empty means "let the
//                               network choose".                                        no-data
//   6  power saving             asked for here, before the radio goes on, as the library's own
//                               example does. What the network *granted* only arrives later.
//   7  the radio on             `AT+CFUN=1`.                                       modem-silent
//   8  attach                   `AT+CGATT=1`.                                   not-registered
//   9  wait for registration    a poll of the modem's buffered +CEREG state, which costs nothing
//                               and is therefore the one step whose deadline is exact. `denied`
//                               is reported at once: waiting out the deadline would not help.
//                                                                                not-registered
//  10  packet data              a PDP address. Registered with no address is a network that let
//                               us in and gave us nothing.                              no-data
//  11  the time                 `AT+CCLK?`, a few times. **Never a failure of the uplink**: an
//                               uplink that came up without a time is up, and the time is
//                               reported beside it (core/uplink/clock_window.h).
//
// -- the budget --------------------------------------------------------------------------------
//
// One number, handed in by the wake cycle (task p3-05), is how long this wake may spend on the
// modem **at all** - the sequence above and every question after it. Each step's deadline is the
// smaller of its own patience and what is left of the budget; a step that cannot be begun inside
// the budget is not begun, the modem is taken down, and the result says `abandoned`. A unit that
// spends its whole battery on a modem that will not attach is the failure this number exists to
// make impossible.
//
// What it cannot do is interrupt a call that has already started. Against the fake that is moot
// and the tests hold it exactly; against `walter-modem` a blocking call runs for up to
// kModemCallCeilingMs whatever the deadline says, and the policy's wall is checked before and
// after it and reports the overrun (`Result::overran`). components/modem/walter_uplink.h holds
// the arithmetic to the library's own configuration with a static_assert, so that a Kconfig
// change cannot quietly make the budget a lie.
//
// -- the PIN -----------------------------------------------------------------------------------
//
// Three refused PINs block a SIM card, and opening it again needs the operator's PUK and a
// person. So: a PIN is sent only when the card asks for one, only when a PIN is set, and
// **never again once it has been refused**. The refusal is written to retained storage before
// anything else can go wrong, and is forgotten only when pinChanged() says the setting is a
// different PIN - which is task p3-03's call to make when a person provisions one.
//
// No function of the PIN is stored anywhere: a digest of a four-digit number is not a secret.
// What is stored is a flag and the `pin_epoch` the caller gives, and that is enough to tell "the
// same PIN" from "a new one".
//
// Retained storage does not survive a power cut (interfaces/retained.h), so a unit that loses
// power forgets a refusal and may spend a second of the card's three tries. That is the lesser
// evil of the two available here - the other is a unit whose SIM can never be unlocked again
// after a flat battery - and the durable answer is task p3-03's settings store in NVS. Said in
// tasks/phase-3/reports/01.md under *Decisions taken without asking*.
//
// -- Privacy -----------------------------------------------------------------------------------
//
// The IMEI, the ICCID and the IMSI identify a subscription. identity() hands them to whoever
// asked - `lte id`, a person at the console - and **they are in no other output of this class**:
// not in Status, not in a console line, not in anything the firmware sends by itself. A host
// test (host_test/core/uplink/test_uplink_privacy.cpp) runs a whole wake through the console and
// greps everything it printed for the fake's numbers.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/uplink/clock_window.h"
#include "core/uplink/failure.h"
#include "core/uplink/psm.h"
#include "interfaces/clock.h"
#include "interfaces/retained.h"
#include "interfaces/uplink.h"

namespace core {
namespace uplink {

// -- the deadlines, each with a sentence ---------------------------------------------------------
//
// Every one of them is a guess until a modem on a bench with a SIM in it says otherwise; the
// report lists them as "to be set on the bench" and `docs/bench.md` §12 is how they are measured
// (the third item there - how long an attach takes from cold, three times - is the one that
// matters most, because kRegistrationDeadlineMs is most of the budget).

// The first `AT`. The modem is on the same board and answers in milliseconds when it is there at
// all; ten seconds is "it is not there", not "it is busy".
constexpr std::uint32_t kPresentDeadlineMs = 10000;

// A modem reset and its `+SYSSTART`. The library pulses the reset pin for 10 ms, waits a whole
// second before it even listens, and then waits for `+SYSSTART` for one command timeout
// (WalterModem.cpp, reset()); the Sequans module's own start-up is a few seconds. So the call
// itself can take kModemCallCeilingMs plus that second, and a deadline shorter than the sum
// would be reported as an overrun at every reset of a modem that is really gone.
constexpr std::uint32_t kModemResetDeadlineMs = 32000;

// `AT+CFUN=4` or `=1`: the modem's radio coming up or going down. Seconds, not milliseconds -
// it is a radio - and fifteen is past anything the module does when it is healthy.
constexpr std::uint32_t kOpStateDeadlineMs = 15000;

// `AT+CPIN?` and `AT+CPIN=<pin>`: the modem talking to the card over its own serial link.
constexpr std::uint32_t kSimDeadlineMs = 10000;

// Reading the RAT is one command; setting it is one command and then a modem reset, so the
// deadline is the reset's.
constexpr std::uint32_t kRatDeadlineMs = 10000;

// `AT+CGDCONT`: a write to the modem's own context table, no air time.
constexpr std::uint32_t kPdpDeadlineMs = 10000;

// `AT+CPSMS` / `AT+SQNEDRX`: two writes, no air time. What the network grants is another matter.
constexpr std::uint32_t kPowerSavingDeadlineMs = 10000;

// `AT+CGATT=1`.
constexpr std::uint32_t kAttachDeadlineMs = 15000;

// The wait for the network to register us, and how often the modem's buffered state is looked
// at. Two minutes: QuickSpot's own example waits five (examples/https, `waitForNetwork`'s
// default of 300 s), and five minutes of searching per wake is a battery this unit does not
// have. **The first number the bench changes** - `docs/bench.md` §12, "how long an attach takes
// from cold, three times".
constexpr std::uint32_t kRegistrationDeadlineMs = 120000;
constexpr std::uint32_t kRegistrationPollMs = 1000;

// A PDP address, asked for a few times: the context is activated by the network a moment after
// registration, so the first ask can be early rather than wrong.
constexpr std::uint32_t kDataDeadlineMs = 15000;
constexpr std::uint32_t kDataTries = 5;
constexpr std::uint32_t kDataRetryMs = 2000;

// The clock, asked for a few times: NITZ arrives when the network feels like it, and the
// library's positioning example asks five times at two-second intervals for the same reason.
constexpr std::uint32_t kTimeDeadlineMs = 10000;
constexpr std::uint32_t kTimeTries = 3;
constexpr std::uint32_t kTimeRetryMs = 2000;

// `AT+CSQ`, `AT+CESQ` and `AT+SQNMONI`: three commands, no air time.
constexpr std::uint32_t kSignalDeadlineMs = 10000;

// `AT+CGSN`, `AT+CCID`, `AT+CIMI`: three commands to the card and the module.
constexpr std::uint32_t kIdentityDeadlineMs = 15000;

// The way out: the radio off. Short, and a take-down that fails is not reported as anything -
// it is what happens on the way out of a wake, including the ways out that are failures.
constexpr std::uint32_t kTakeDownDeadlineMs = 10000;

// How long one wake may spend on the modem at all, when the wake cycle does not say. Four
// minutes: the sequence above, once, with the registration wait in it, plus room for one
// worst-case library call. TO BE SET ON THE BENCH against the current drawn while searching
// (`docs/bench.md` §12, the last item) - this number times that current is what a wake with no
// coverage costs the battery.
constexpr std::uint32_t kWakeBudgetMs = 240000;

// The longest one blocking call of the modem library can take, whatever deadline it was given:
// CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS (30 s, the library's default) times
// CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS, which `sdkconfig.defaults` sets to 1 and says why
// (a retried `AT+CPIN=` is a blocked SIM card). The library waits on a condition variable with
// no timeout of its own (WalterDefines.h, `_returnAfterReply`) and its command task is what
// gives up, so this is the real ceiling. The budget has to be larger than it or a single stuck
// command would eat a whole wake; the static_assert in components/modem/include/modem/
// walter_uplink.h holds the library's own configuration to this number, so that raising either
// Kconfig value fails the build rather than making the budget a lie.
constexpr std::uint32_t kModemCallCeilingMs = 30000;
static_assert(kWakeBudgetMs > kModemCallCeilingMs,
              "a wake's budget must leave room for one worst-case modem call");

// -- the settings ------------------------------------------------------------------------------
//
// Three of these are now `lte.apn`, `lte.pin` and `lte.psm` at the console
// (core/settings/keys.h, task p3-03): the console hands them over at boot and at every change
// (apnChanged, pinChanged, powerSavingChanged), and what is left here is what a unit does
// before anybody has provisioned it - which is every unit out of the box, and which has to
// work. The PSM mode's fallback in the settings table is held to kPowerSaving below by a
// static_assert, so a unit with no setting behaves exactly as task p3-01 left it.

// The APN. Empty is "let the network choose", which is what a Danish LTE-M SIM wants and what
// QuickSpot's own examples use (`#define CELLULAR_APN ""`). `lte.apn` sets it.
constexpr const char* kApn = "";

// The SIM's PIN. Empty is "none is set", and a card that asks for one then gets nothing and is
// reported `sim-locked` - which is correct: sending a guess is how a card gets blocked.
// `lte.pin` sets it, and it is **never a default**: no PIN is ever written down here.
constexpr const char* kSimPin = "";

// LTE-M only (task p3-01). NB-IoT exists in the modem and the setting can ask for it; nothing
// in this firmware does.
constexpr interfaces::Rat kRat = interfaces::Rat::kLteM;

// Power saving: **off between wakes is the default** and PSM is the `lte.psm` setting
// (spec/SYSTEM.md §10 item 5 - which of PSM, eDRX or an off modem costs least is a bench
// question). "Off between wakes" here means the modem's radio in its minimum state and the
// ESP32 asleep; the library's own `sleep()` would take the whole board down with it and that is
// task p3-04's decision, not this one's (tasks/phase-3/reports/01.md).
constexpr interfaces::PowerSavingMode kPowerSaving = interfaces::PowerSavingMode::kOff;

// The timers to ask for when `lte.psm` says on. An hour of sleep and a minute of being
// reachable after each transmission: TO BE SET ON THE BENCH, and only once task p3-04's interval
// table exists - a TAU shorter than the wake interval buys nothing, and one much longer makes
// the network drop us between wakes anyway. **These two are not settings**: what the network
// grants is its own business and a typo here buys nothing a bench measurement would not.
constexpr std::uint32_t kPsmTauSeconds = 3600;
constexpr std::uint32_t kPsmActiveSeconds = 60;

struct Settings {
  const char* apn = kApn;
  const char* pin = kSimPin;  // empty: none is set
  // Which PIN this is: a counter the settings store keeps beside the keys and bumps when a
  // person provisions a different PIN (core/settings/settings.h), which is what lets a refusal
  // be forgotten without this class ever holding a function of the PIN.
  std::uint32_t pin_epoch = 0;
  interfaces::Rat rat = kRat;
  interfaces::PowerSavingMode power_saving = kPowerSaving;
  std::uint32_t psm_tau_s = kPsmTauSeconds;
  std::uint32_t psm_active_s = kPsmActiveSeconds;
};

// -- how far the sequence got --------------------------------------------------------------------

enum class Step : std::uint8_t {
  kNone = 0,
  kModem = 1,
  kAwake = 2,
  // The RAT before the SIM, and not after: setting it costs a modem reset, and a reset loses a
  // card that had just been unlocked (WalterModem.cpp, reset(): `_simPIN = NULL`). A PIN that
  // has to be sent twice in a wake is one of the card's three tries thrown away.
  kRat = 3,
  kSim = 4,
  kContext = 5,
  kPowerSaving = 6,
  kRadio = 7,
  kAttach = 8,
  kRegistration = 9,
  kData = 10,
  kTime = 11,
  kUp = 12,
};

const char* describe(Step step);

struct Result {
  Failure failure = Failure::kNone;  // kNone and `up`: it worked
  bool up = false;
  Step reached = Step::kNone;
  bool abandoned = false;   // the wake's budget ran out: the modem was taken down, cleanly
  bool overran = false;     // a call came back after its deadline (the real modem can)
  bool modem_reset = false; // the one reset this wake was allowed
  bool rat_changed = false;
  std::uint32_t spent_ms = 0;
  // `no-time` is not a failure of the uplink: it is reported here, beside it.
  interfaces::NetworkTime time{};
};

// -- the retained record -------------------------------------------------------------------------
//
// The backoff, in retained storage, "so that a unit with no coverage does not spend its battery
// searching at every wake". Plain data: it is stored as bytes (interfaces/retained.h).
//
// Counted in wakes, not seconds: see core/uplink/failure.h.
constexpr std::uint8_t kRecordVersion = 1;

struct Record {
  std::uint8_t version = kRecordVersion;
  std::uint8_t last_failure = 0;   // a Failure
  std::uint8_t pin_refused = 0;
  std::uint8_t spare = 0;
  std::uint32_t failures = 0;          // consecutive failures of a kind that backs off
  std::uint32_t interval_wakes = 1;    // what the backoff last worked out
  std::uint32_t wakes_since_try = 0;   // wakes counted since the last attempt
  std::uint32_t pin_epoch = 0;         // which PIN `pin_refused` is about
  std::uint32_t wakes = 0;             // every wake ever counted (a gloss for the console)
  std::uint32_t tries = 0;             // every attempt ever made
};
static_assert(sizeof(Record) <= interfaces::kRetainedRecordMax, "the record has to fit a slot");

// -- what `lte` prints ---------------------------------------------------------------------------
//
// Note what is **not** here: the IMEI, the ICCID and the IMSI. See *Privacy* above.
struct Status {
  interfaces::UplinkState state = interfaces::UplinkState::kDown;
  bool ever_up = false;
  // The last attempt.
  bool tried_this_boot = false;
  Failure last_failure = Failure::kNone;
  Step reached = Step::kNone;
  std::uint64_t last_try_ms = 0;
  std::uint32_t last_spent_ms = 0;
  bool last_abandoned = false;
  bool last_overran = false;
  bool modem_reset_this_wake = false;
  bool rat_changed = false;
  // The backoff.
  std::uint32_t failures = 0;
  std::uint32_t interval_wakes = 1;
  std::uint32_t wakes_since_try = 0;
  std::uint32_t wakes_until_next_try = 0;
  bool may_try = true;
  Retry retry = Retry::kNotNeeded;
  std::uint32_t wakes = 0;
  std::uint32_t tries = 0;
  bool record_lost = false;  // retained storage had nothing: a first boot, or a power cut
  // The PIN.
  bool pin_set = false;
  bool pin_refused = false;
  // The budget of the wake in hand.
  std::uint32_t budget_ms = 0;
  std::uint32_t budget_left_ms = 0;
  // The last readings, as they were taken.
  bool signal_known = false;
  interfaces::Signal signal{};
  interfaces::NetworkTime time{};
  interfaces::PowerSavingGrant power_saving{};
  interfaces::Rat rat = interfaces::Rat::kUnknown;
};

class UplinkPolicy {
 public:
  UplinkPolicy(interfaces::Uplink& link, interfaces::Clock& clock, interfaces::Retained& retained,
               const Settings& settings = Settings{});

  // At boot: the retained record, or the defaults when there is none (a first boot or a power
  // cut, which the status says). Reads nothing of the modem and spends nothing.
  void begin();
  bool begun() const { return begun_; }

  // A wake begins. **Counted whether or not the modem is touched**: the backoff is in wakes, so
  // a wake that the backoff skips still has to be a wake that was skipped. Clears the per-wake
  // flags (the one modem reset, the budget).
  void beginWake();

  // Does the backoff allow an attempt in this wake, and how many wakes until it does. A person
  // at the console is not the backoff's business: the console calls bringUp() regardless and
  // says what the backoff would have said.
  bool mayTry() const;
  std::uint32_t wakesUntilNextTry() const;

  // The sequence, inside `budget_ms` from now. kNone and `up` is up; anything else is one of the
  // five failures, the modem taken down, and the backoff moved on. Opens the budget the
  // questions after it spend from as well.
  Result bringUp(std::uint32_t budget_ms = kWakeBudgetMs);

  // Open a budget for whatever comes next without bringing anything up: what the console does
  // for a single `lte time` or `lte id`, so that one command cannot sit on the modem for ever.
  // bringUp() opens its own.
  void openBudget(std::uint32_t budget_ms);
  std::uint32_t budgetLeftMs() const;

  // The radio off. Always safe to call, and called on every way out of a wake.
  void takeDown();

  interfaces::UplinkState state() const;

  // The four questions worth asking once it is up. Each spends from the same budget and returns
  // false when there is none left or the modem would not answer.
  bool networkTime(interfaces::NetworkTime* out);
  bool signal(interfaces::Signal* out);
  bool identity(interfaces::Identity* out);   // `lte id` only: see *Privacy*
  bool powerSaving(interfaces::PowerSavingGrant* out);

  const Status& status() const { return status_; }
  const Settings& settings() const { return settings_; }

  // Task p3-03, when a person provisions a PIN: this is a different PIN, so a refusal recorded
  // against the old one is not about it. Also takes the new settings' pointer and epoch.
  void pinChanged(const char* pin, std::uint32_t pin_epoch);

  // The other two settings this class holds (task p3-03). The pointer has to stay alive as long
  // as the policy does, which the settings store's buffers do (core/settings/settings.h). Both
  // take effect the next time the uplink is brought up: the APN is sent when the packet context
  // is defined and power saving when it is asked for, and neither is worth a modem reset now -
  // a reset loses a card that had just been unlocked (policy.cpp, kRat).
  void apnChanged(const char* apn) { settings_.apn = apn != nullptr ? apn : ""; }
  void powerSavingChanged(interfaces::PowerSavingMode mode, std::uint32_t tau_s,
                          std::uint32_t active_s) {
    settings_.power_saving = mode;
    settings_.psm_tau_s = tau_s;
    settings_.psm_active_s = active_s;
  }

  // The bench, and `lte up` from a person: the backoff forgotten, so the next wake tries.
  // Never called by anything automatic.
  void forgetBackoff();

 private:
  // The deadline for a step that wants `want_ms`: the sooner of its own patience and what is
  // left of the wake's budget.
  std::uint64_t deadlineFor(std::uint32_t want_ms) const;
  // Is there room to begin a step that wants `want_ms` at all. False means abandon.
  bool roomFor(std::uint32_t want_ms) const;
  // A call has come back: note an overrun of `deadline`.
  void noteReturn(std::uint64_t deadline);
  void fail(Result* result, Failure failure, Step reached);
  void abandon(Result* result, Failure failure, Step reached);
  void finish(Result* result);
  void sequence(Result* result);
  Failure simStep();
  void load();
  void save();
  void refreshBackoff();

  interfaces::Uplink& link_;
  interfaces::Clock& clock_;
  interfaces::Retained& retained_;
  Settings settings_;
  Record record_{};
  Status status_{};
  bool begun_ = false;
  std::uint64_t budget_deadline_ms_ = 0;
  std::uint64_t wake_began_ms_ = 0;
  bool reset_used_ = false;
  bool overran_ = false;
};

}  // namespace uplink
}  // namespace core
