#include "core/uplink/policy.h"

namespace core {
namespace uplink {

namespace {

using interfaces::OpState;
using interfaces::Registration;
using interfaces::SimState;
using interfaces::SimUnlock;

bool isSet(const char* text) { return text != nullptr && text[0] != 0; }

}  // namespace

const char* describe(Step step) {
  switch (step) {
    case Step::kNone: return "nothing was tried";
    case Step::kModem: return "is the modem there";
    case Step::kAwake: return "the modem awake, no radio";
    case Step::kSim: return "the SIM";
    case Step::kRat: return "the radio access technology";
    case Step::kContext: return "the packet data context";
    case Step::kPowerSaving: return "power saving";
    case Step::kRadio: return "the radio on";
    case Step::kAttach: return "attach";
    case Step::kRegistration: return "waiting for the network";
    case Step::kData: return "packet data";
    case Step::kTime: return "the network's time";
    case Step::kUp: return "up";
  }
  return "?";
}

UplinkPolicy::UplinkPolicy(interfaces::Uplink& link, interfaces::Clock& clock,
                           interfaces::Retained& retained, const Settings& settings)
    : link_(link), clock_(clock), retained_(retained), settings_(settings) {}

void UplinkPolicy::load() {
  Record stored{};
  if (retained_.load(interfaces::RetainedSlot::kUplink, &stored, sizeof stored) &&
      stored.version == kRecordVersion) {
    record_ = stored;
    status_.record_lost = false;
  } else {
    // A first boot, a power cut, or a firmware whose record had another shape. The backoff
    // starts again - the forgiving side to err on for coverage, and the dangerous side for a
    // refused PIN; policy.h's *The PIN* says why that is the lesser of the two evils available.
    record_ = Record{};
    status_.record_lost = true;
  }
}

void UplinkPolicy::save() {
  record_.version = kRecordVersion;
  retained_.store(interfaces::RetainedSlot::kUplink, &record_, sizeof record_);
}

void UplinkPolicy::refreshBackoff() {
  const Failure last = static_cast<Failure>(record_.last_failure);
  const Retry retry = row(last).retry;
  record_.interval_wakes = intervalAfter(retry, record_.failures);
  status_.retry = retry;
  status_.failures = record_.failures;
  status_.interval_wakes = record_.interval_wakes;
  status_.wakes_since_try = record_.wakes_since_try;
  status_.wakes = record_.wakes;
  status_.tries = record_.tries;
  status_.pin_refused = record_.pin_refused != 0 && record_.pin_epoch == settings_.pin_epoch;
  status_.pin_set = isSet(settings_.pin);
  status_.may_try = mayTry();
  status_.wakes_until_next_try = wakesUntilNextTry();
  status_.budget_left_ms = budgetLeftMs();
}

void UplinkPolicy::begin() {
  load();
  status_.state = link_.state();
  status_.last_failure = static_cast<Failure>(record_.last_failure);
  refreshBackoff();
  begun_ = true;
}

void UplinkPolicy::beginWake() {
  if (!begun_) begin();
  record_.wakes += 1;
  record_.wakes_since_try += 1;
  reset_used_ = false;
  overran_ = false;
  budget_deadline_ms_ = 0;
  status_.modem_reset_this_wake = false;
  status_.budget_ms = 0;
  save();
  refreshBackoff();
}

bool UplinkPolicy::mayTry() const {
  if (record_.interval_wakes == 0) return false;  // never automatically (a refused PIN)
  return record_.wakes_since_try >= record_.interval_wakes;
}

std::uint32_t UplinkPolicy::wakesUntilNextTry() const {
  if (record_.interval_wakes == 0) return 0;  // never: the console says so in words, not a number
  if (record_.wakes_since_try >= record_.interval_wakes) return 0;
  return record_.interval_wakes - record_.wakes_since_try;
}

void UplinkPolicy::openBudget(std::uint32_t budget_ms) {
  if (!begun_) begin();
  budget_deadline_ms_ = clock_.nowMs() + budget_ms;
  status_.budget_ms = budget_ms;
  status_.budget_left_ms = budget_ms;
}

std::uint32_t UplinkPolicy::budgetLeftMs() const {
  if (budget_deadline_ms_ == 0) return 0;
  const std::uint64_t now = clock_.nowMs();
  return now >= budget_deadline_ms_ ? 0 : static_cast<std::uint32_t>(budget_deadline_ms_ - now);
}

std::uint64_t UplinkPolicy::deadlineFor(std::uint32_t want_ms) const {
  const std::uint64_t own = clock_.nowMs() + want_ms;
  if (budget_deadline_ms_ == 0) return own;
  return own < budget_deadline_ms_ ? own : budget_deadline_ms_;
}

bool UplinkPolicy::roomFor(std::uint32_t want_ms) const {
  if (budget_deadline_ms_ == 0) return true;
  const std::uint64_t now = clock_.nowMs();
  if (now >= budget_deadline_ms_) return false;
  // There has to be room for *something*: a step begun with one millisecond left is a call that
  // overruns by its whole length. A tenth of its own patience is the smallest useful attempt.
  const std::uint64_t least = want_ms / 10u + 1u;
  return budget_deadline_ms_ - now >= least;
}

void UplinkPolicy::noteReturn(std::uint64_t deadline) {
  if (clock_.nowMs() > deadline) overran_ = true;
}

void UplinkPolicy::fail(Result* result, Failure failure, Step reached) {
  result->failure = failure;
  result->reached = reached;
  result->up = false;
  takeDown();
}

void UplinkPolicy::abandon(Result* result, Failure failure, Step reached) {
  fail(result, failure, reached);
  result->abandoned = true;
}

void UplinkPolicy::finish(Result* result) {
  result->overran = overran_;
  result->modem_reset = reset_used_;
  result->spent_ms = static_cast<std::uint32_t>(clock_.nowMs() - wake_began_ms_);
  status_.tried_this_boot = true;
  status_.last_failure = result->failure;
  status_.reached = result->reached;
  status_.last_try_ms = wake_began_ms_;
  status_.last_spent_ms = result->spent_ms;
  status_.last_abandoned = result->abandoned;
  status_.last_overran = result->overran;
  status_.modem_reset_this_wake = result->modem_reset;
  status_.rat_changed = result->rat_changed;
  status_.time = result->time;
  status_.state = link_.state();
  if (result->up) status_.ever_up = true;

  // The backoff. A failure of the same kind as the last lengthens the run of them; a success, or
  // a failure of another kind, starts the count again.
  if (result->failure == Failure::kNone) {
    record_.failures = 0;
  } else if (static_cast<Failure>(record_.last_failure) == result->failure) {
    if (record_.failures < 0xFFFFFFFFu) record_.failures += 1;
  } else {
    record_.failures = 1;
  }
  record_.last_failure = static_cast<std::uint8_t>(result->failure);
  record_.wakes_since_try = 0;
  record_.tries += 1;
  save();
  refreshBackoff();
}

// The SIM, with the PIN's rule in one place: a PIN is sent only when the card asks for one, only
// when one is set, and never once it has been refused (policy.h, *The PIN*).
Failure UplinkPolicy::simStep() {
  const std::uint64_t deadline = deadlineFor(kSimDeadlineMs);
  const SimState state = link_.sim(deadline);
  noteReturn(deadline);
  switch (state) {
    case SimState::kReady:
      return Failure::kNone;
    case SimState::kAbsent:
    case SimState::kUnknown:
      // "no card, or it does not answer": both of them, and both mean a person.
      return Failure::kNoSim;
    case SimState::kBlocked:
      return Failure::kSimLocked;
    case SimState::kPinRequired:
      break;
  }
  if (!isSet(settings_.pin)) return Failure::kSimLocked;
  if (record_.pin_refused != 0 && record_.pin_epoch == settings_.pin_epoch) {
    return Failure::kSimLocked;
  }
  if (!roomFor(kSimDeadlineMs)) return Failure::kSimLocked;
  const std::uint64_t unlock_deadline = deadlineFor(kSimDeadlineMs);
  const SimUnlock unlocked = link_.unlockSim(settings_.pin, unlock_deadline);
  noteReturn(unlock_deadline);
  switch (unlocked) {
    case SimUnlock::kAccepted:
      return Failure::kNone;
    case SimUnlock::kRefused:
      // Written down before anything else can go wrong: this PIN is never sent again.
      record_.pin_refused = 1;
      record_.pin_epoch = settings_.pin_epoch;
      save();
      return Failure::kSimLocked;
    case SimUnlock::kFailed:
      // The command did not get through: the card said nothing and nothing was spent of its
      // three tries. Reported as no-sim - "it does not answer" - so that the next wake tries
      // again, which sim-locked's "never" would not.
      return Failure::kNoSim;
  }
  return Failure::kSimLocked;
}

void UplinkPolicy::sequence(Result* result) {
  // 1. Is the modem there. The one failure with a remedy inside the wake.
  result->reached = Step::kModem;
  if (!roomFor(kPresentDeadlineMs)) {
    abandon(result, Failure::kModemSilent, Step::kModem);
    return;
  }
  std::uint64_t deadline = deadlineFor(kPresentDeadlineMs);
  bool there = link_.present(deadline);
  noteReturn(deadline);
  if (!there && !reset_used_ && roomFor(kModemResetDeadlineMs)) {
    reset_used_ = true;
    deadline = deadlineFor(kModemResetDeadlineMs);
    link_.resetModem(deadline);
    noteReturn(deadline);
    if (roomFor(kPresentDeadlineMs)) {
      deadline = deadlineFor(kPresentDeadlineMs);
      there = link_.present(deadline);
      noteReturn(deadline);
    }
  }
  if (!there) {
    fail(result, Failure::kModemSilent, Step::kModem);
    return;
  }

  // 2. Awake, no radio: where the SIM answers and nothing transmits.
  result->reached = Step::kAwake;
  if (!roomFor(kOpStateDeadlineMs)) {
    abandon(result, Failure::kModemSilent, Step::kAwake);
    return;
  }
  deadline = deadlineFor(kOpStateDeadlineMs);
  const bool awake = link_.opState(OpState::kNoRf, deadline);
  noteReturn(deadline);
  if (!awake) {
    fail(result, Failure::kModemSilent, Step::kAwake);
    return;
  }

  // 3. The radio access technology: LTE-M unless a setting says otherwise. Read always, set only
  // when it is wrong - and then the modem is reset, because the library's softReset says a RAT
  // switch needs one. **Before the SIM, for that reason**: a reset loses a card that was just
  // unlocked, and a PIN sent twice in one wake is one of its three tries thrown away. A RAT that
  // will not be set is recorded and the sequence goes on: the module's own default is LTE-M, so
  // the likely outcome is an attach that works anyway, and refusing to attach over a setting
  // would be a unit off the air for no gain.
  result->reached = Step::kRat;
  if (roomFor(kRatDeadlineMs)) {
    deadline = deadlineFor(kRatDeadlineMs);
    interfaces::Rat now = interfaces::Rat::kUnknown;
    const bool read = link_.rat(deadline, &now);
    noteReturn(deadline);
    if (read) status_.rat = now;
    if (read && now != settings_.rat && roomFor(kRatDeadlineMs)) {
      deadline = deadlineFor(kRatDeadlineMs);
      const bool changed = link_.setRat(settings_.rat, deadline);
      noteReturn(deadline);
      if (changed) {
        result->rat_changed = true;
        status_.rat = settings_.rat;
        if (!reset_used_ && roomFor(kModemResetDeadlineMs)) {
          reset_used_ = true;
          deadline = deadlineFor(kModemResetDeadlineMs);
          link_.resetModem(deadline);
          noteReturn(deadline);
          if (roomFor(kOpStateDeadlineMs)) {
            deadline = deadlineFor(kOpStateDeadlineMs);
            link_.opState(OpState::kNoRf, deadline);
            noteReturn(deadline);
          }
        }
      }
    }
  }

  // 4. The SIM.
  result->reached = Step::kSim;
  if (!roomFor(kSimDeadlineMs)) {
    abandon(result, Failure::kNoSim, Step::kSim);
    return;
  }
  const Failure sim_failure = simStep();
  if (sim_failure != Failure::kNone) {
    fail(result, sim_failure, Step::kSim);
    return;
  }

  // 5. The packet data context.
  result->reached = Step::kContext;
  if (!roomFor(kPdpDeadlineMs)) {
    abandon(result, Failure::kNoData, Step::kContext);
    return;
  }
  deadline = deadlineFor(kPdpDeadlineMs);
  const bool defined = link_.definePdp(settings_.apn, deadline);
  noteReturn(deadline);
  if (!defined) {
    fail(result, Failure::kNoData, Step::kContext);
    return;
  }

  // 6. Power saving, asked for before the radio goes on - which is where the library's own
  // example asks for it. Never a failure: what the network grants is the network's business, and
  // a modem that will not take the request is a modem that stays awake.
  result->reached = Step::kPowerSaving;
  if (roomFor(kPowerSavingDeadlineMs)) {
    interfaces::PowerSavingRequest request;
    request.mode = settings_.power_saving;
    if (request.mode == interfaces::PowerSavingMode::kPsm) {
      request.tau_octet = tauOctet(settings_.psm_tau_s);
      request.active_octet = activeTimeOctet(settings_.psm_active_s);
    }
    interfaces::PowerSavingGrant grant;
    deadline = deadlineFor(kPowerSavingDeadlineMs);
    link_.powerSaving(request, deadline, &grant);
    noteReturn(deadline);
    status_.power_saving = grant;
  }

  // 7. The radio on.
  result->reached = Step::kRadio;
  if (!roomFor(kOpStateDeadlineMs)) {
    abandon(result, Failure::kModemSilent, Step::kRadio);
    return;
  }
  deadline = deadlineFor(kOpStateDeadlineMs);
  const bool on = link_.opState(OpState::kFull, deadline);
  noteReturn(deadline);
  if (!on) {
    fail(result, Failure::kModemSilent, Step::kRadio);
    return;
  }

  // 8. Attach.
  result->reached = Step::kAttach;
  if (!roomFor(kAttachDeadlineMs)) {
    abandon(result, Failure::kNotRegistered, Step::kAttach);
    return;
  }
  deadline = deadlineFor(kAttachDeadlineMs);
  const bool attached = link_.attach(deadline);
  noteReturn(deadline);
  if (!attached) {
    fail(result, Failure::kNotRegistered, Step::kAttach);
    return;
  }

  // 9. Wait for the network. The modem's registration state is buffered and instant, so this
  // wait is a poll and its deadline is exact - the one step of the sequence of which that is
  // true. `denied` is answered at once: the network has decided, and waiting out two minutes of
  // radio would be two minutes of battery for the same answer.
  result->reached = Step::kRegistration;
  if (!roomFor(kRegistrationDeadlineMs)) {
    abandon(result, Failure::kNotRegistered, Step::kRegistration);
    return;
  }
  const std::uint64_t registration_deadline = deadlineFor(kRegistrationDeadlineMs);
  bool registered = false;
  for (;;) {
    const Registration where = link_.registration();
    if (where == Registration::kRegistered) {
      registered = true;
      break;
    }
    if (where == Registration::kDenied) break;
    const std::uint64_t now = clock_.nowMs();
    if (now >= registration_deadline) break;
    std::uint32_t wait = kRegistrationPollMs;
    const std::uint64_t left = registration_deadline - now;
    if (left < wait) wait = static_cast<std::uint32_t>(left);
    clock_.delayMs(wait);
  }
  if (!registered) {
    if (budget_deadline_ms_ != 0 && clock_.nowMs() >= budget_deadline_ms_) {
      abandon(result, Failure::kNotRegistered, Step::kRegistration);
    } else {
      fail(result, Failure::kNotRegistered, Step::kRegistration);
    }
    return;
  }

  // 10. Packet data: an address, asked for a few times.
  result->reached = Step::kData;
  bool data = false;
  for (std::uint32_t attempt = 0; attempt < kDataTries && !data; ++attempt) {
    if (!roomFor(kDataDeadlineMs)) {
      abandon(result, Failure::kNoData, Step::kData);
      return;
    }
    deadline = deadlineFor(kDataDeadlineMs);
    data = link_.dataReady(deadline);
    noteReturn(deadline);
    if (!data && attempt + 1 < kDataTries) clock_.delayMs(kDataRetryMs);
  }
  if (!data) {
    fail(result, Failure::kNoData, Step::kData);
    return;
  }

  // 11. The time. Never a failure of the uplink - reported beside it.
  result->reached = Step::kTime;
  for (std::uint32_t attempt = 0; attempt < kTimeTries; ++attempt) {
    if (!roomFor(kTimeDeadlineMs)) break;
    deadline = deadlineFor(kTimeDeadlineMs);
    interfaces::NetworkTime when;
    const bool answered = link_.networkTime(deadline, &when);
    noteReturn(deadline);
    if (answered) result->time = when;
    if (answered && when.origin == interfaces::TimeOrigin::kNetwork) break;
    if (attempt + 1 < kTimeTries) clock_.delayMs(kTimeRetryMs);
  }
  // A time that is not the network's is not a time: the epoch goes with the verdict, so that
  // nothing downstream can read a 1970 out of a result that said it was a default.
  if (result->time.origin != interfaces::TimeOrigin::kNetwork) result->time.epoch = 0;

  result->reached = Step::kUp;
  result->up = true;
}

Result UplinkPolicy::bringUp(std::uint32_t budget_ms) {
  if (!begun_) begin();
  wake_began_ms_ = clock_.nowMs();
  openBudget(budget_ms);
  Result result;
  sequence(&result);
  finish(&result);
  return result;
}

void UplinkPolicy::takeDown() {
  link_.takeDown();
  status_.state = link_.state();
}

interfaces::UplinkState UplinkPolicy::state() const { return link_.state(); }

bool UplinkPolicy::networkTime(interfaces::NetworkTime* out) {
  if (out == nullptr || !roomFor(kTimeDeadlineMs)) return false;
  const std::uint64_t deadline = deadlineFor(kTimeDeadlineMs);
  interfaces::NetworkTime when;
  const bool answered = link_.networkTime(deadline, &when);
  noteReturn(deadline);
  if (!answered) return false;
  if (when.origin != interfaces::TimeOrigin::kNetwork) when.epoch = 0;
  status_.time = when;
  *out = when;
  return when.origin == interfaces::TimeOrigin::kNetwork;
}

bool UplinkPolicy::signal(interfaces::Signal* out) {
  if (out == nullptr || !roomFor(kSignalDeadlineMs)) return false;
  const std::uint64_t deadline = deadlineFor(kSignalDeadlineMs);
  interfaces::Signal reading;
  const bool answered = link_.signal(deadline, &reading);
  noteReturn(deadline);
  if (!answered) return false;
  status_.signal = reading;
  status_.signal_known = true;
  if (reading.rat != interfaces::Rat::kUnknown) status_.rat = reading.rat;
  *out = reading;
  return true;
}

bool UplinkPolicy::identity(interfaces::Identity* out) {
  // Nothing of what comes back is kept in Status or printed by this class: policy.h, *Privacy*.
  if (out == nullptr || !roomFor(kIdentityDeadlineMs)) return false;
  const std::uint64_t deadline = deadlineFor(kIdentityDeadlineMs);
  const bool answered = link_.identity(deadline, out);
  noteReturn(deadline);
  return answered;
}

bool UplinkPolicy::powerSaving(interfaces::PowerSavingGrant* out) {
  if (out == nullptr || !roomFor(kPowerSavingDeadlineMs)) return false;
  interfaces::PowerSavingRequest request;
  request.mode = settings_.power_saving;
  if (request.mode == interfaces::PowerSavingMode::kPsm) {
    request.tau_octet = tauOctet(settings_.psm_tau_s);
    request.active_octet = activeTimeOctet(settings_.psm_active_s);
  }
  const std::uint64_t deadline = deadlineFor(kPowerSavingDeadlineMs);
  interfaces::PowerSavingGrant grant;
  const bool answered = link_.powerSaving(request, deadline, &grant);
  noteReturn(deadline);
  if (!answered) return false;
  status_.power_saving = grant;
  *out = grant;
  return true;
}

void UplinkPolicy::pinChanged(const char* pin, std::uint32_t pin_epoch) {
  settings_.pin = pin;
  const bool different = record_.pin_epoch != pin_epoch;
  settings_.pin_epoch = pin_epoch;
  if (!different) {
    refreshBackoff();
    return;
  }
  record_.pin_refused = 0;
  record_.pin_epoch = pin_epoch;
  // A refusal that was about another PIN is also a run of sim-locked failures that was about
  // another PIN: a new PIN deserves the attempt the old one's "never" would have denied it.
  if (static_cast<Failure>(record_.last_failure) == Failure::kSimLocked) {
    record_.last_failure = static_cast<std::uint8_t>(Failure::kNone);
    record_.failures = 0;
    record_.wakes_since_try = 1;
  }
  save();
  refreshBackoff();
}

void UplinkPolicy::forgetBackoff() {
  record_.last_failure = static_cast<std::uint8_t>(Failure::kNone);
  record_.failures = 0;
  record_.interval_wakes = 1;
  record_.wakes_since_try = 1;
  save();
  refreshBackoff();
}

}  // namespace uplink
}  // namespace core
