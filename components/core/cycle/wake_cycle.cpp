#include "core/cycle/wake_cycle.h"

#include "core/cycle/agent_attention.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/image/this_image.h"
#include "core/link/client/words.h"
#include "core/link/codec/wire.h"
#include "core/version.h"

namespace core {
namespace cycle {

namespace {

void put32(std::uint8_t* at, std::uint32_t value) {
  at[0] = static_cast<std::uint8_t>(value);
  at[1] = static_cast<std::uint8_t>(value >> 8);
  at[2] = static_cast<std::uint8_t>(value >> 16);
  at[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t get32(const std::uint8_t* at) {
  return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
         (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

// A fixed field, copied and terminated. Nothing in the structure that crosses the seam is a
// pointer, so every string in it is one of these.
void copyInto(char* out, std::size_t cap, const char* text) {
  if (cap == 0) return;
  out[0] = 0;
  if (text == nullptr) return;
  std::size_t i = 0;
  for (; i + 1 < cap && text[i] != 0; ++i) out[i] = text[i];
  out[i] = 0;
}

void copyInto(char* out, std::size_t cap, link::Text text) {
  if (cap == 0) return;
  out[0] = 0;
  if (text.data == nullptr) return;
  std::size_t i = 0;
  for (; i + 1 < cap && i < text.size; ++i) out[i] = text.data[i];
  out[i] = 0;
}

std::uint32_t elapsed(std::uint64_t from, std::uint64_t to) {
  return to > from ? static_cast<std::uint32_t>(to - from) : 0;
}

}  // namespace

// -- the words ---------------------------------------------------------------------------------

const char* name(Step step) {
  switch (step) {
    case Step::kWhy: return "why";
    case Step::kRadio: return "radio";
    case Step::kResume: return "resume";
    case Step::kState: return "state";
    case Step::kBand: return "band";
    case Step::kUplink: return "uplink";
    case Step::kClock: return "clock";
    case Step::kBackend: return "backend";
    case Step::kUpdate: return "update";
    case Step::kSleep: return "sleep";
    case Step::kCount: break;
  }
  return "?";
}

const char* describe(Step step) {
  switch (step) {
    case Step::kWhy: return "why this wake happened";
    case Step::kRadio: return "the radio: the supervisor's check, and its ladder when it is silent";
    case Step::kResume: return "what the journal says was in progress";
    case Step::kState: return "the radio's state: STATUS, POWER_GET, ENV_GET";
    case Step::kBand: return "the band the battery allows";
    case Step::kUplink: return "the modem, inside this wake's budget";
    case Step::kClock: return "the radio's clock, from the network's time";
    case Step::kBackend: return "the backend, behind its seam";
    case Step::kUpdate: return "at most one update step, of one MCU";
    case Step::kSleep: return "sleep, which is the one way out of a wake";
    case Step::kCount: break;
  }
  return "?";
}

const char* describe(Outcome outcome) {
  switch (outcome) {
    case Outcome::kNotRun: return "not reached";
    case Outcome::kDone: return "done";
    case Outcome::kNothing: return "nothing to do";
    case Outcome::kSkipped: return "skipped: a precondition";
    case Outcome::kNoTime: return "no time: the wake's budget";
    case Outcome::kRefused: return "refused";
    case Outcome::kFailed: return "failed";
    case Outcome::kCount: break;
  }
  return "?";
}

char letter(Outcome outcome) {
  switch (outcome) {
    case Outcome::kNotRun: return '.';
    case Outcome::kDone: return 'y';
    case Outcome::kNothing: return '-';
    case Outcome::kSkipped: return 's';
    case Outcome::kNoTime: return 't';
    case Outcome::kRefused: return 'r';
    case Outcome::kFailed: return 'x';
    case Outcome::kCount: break;
  }
  return '?';
}

const char* describe(Ending ending) {
  switch (ending) {
    case Ending::kRunning: return "still running";
    case Ending::kSlept: return "asleep";
    case Ending::kSleepRefused: return "the sleep was refused";
    case Ending::kRestarting: return "restarting";
    case Ending::kOverran: return "the bound on the wake ended it";
    case Ending::kDry: return "a dry run: nothing was done";
  }
  return "?";
}

// -- the step table, which is the dependency table ---------------------------------------------

const StepDef kSteps[kStepCount] = {
    // Step 1 depends on nothing and is never refused: a wake that does not know why it woke
    // cannot judge anything else. It costs nothing on a timer wake and §8's window on a line one.
    {Step::kWhy, "why", kWhyBoundMs, 0, 0},
    // Step 2 depends on nothing either. **A scanner, a silent radio and a radio mid-update are
    // outcomes of this step, not failures of it**: it succeeds when the supervisor has worked out
    // what the silence means (spec/LINK-PROTOCOL.md §9), whatever the answer was.
    {Step::kRadio, "radio", kRadioBoundMs, kLeastMs, 0},
    // Step 3 needs step 2: nothing of this firmware touches the radio before the supervisor has
    // looked at it (an update in progress is one of §9's four silences, and resuming one while
    // the supervisor was about to reset the radio is how a flash gets interrupted).
    {Step::kResume, "resume", kUpdateBoundMs, kLeastMs, stepBit(Step::kRadio)},
    // Step 4 needs step 2, for the same reason.
    {Step::kState, "state", kStateBoundMs, kLeastMs, stepBit(Step::kRadio)},
    // Step 5 needs **nothing**, and that is deliberate: "no reading at all" is its own band
    // (core/power/interval_policy.h, kUnknown) and a Walter whose radio has died is exactly the
    // Walter that has to be able to reach its uplink and say so.
    {Step::kBand, "band", 1000, 0, 0},
    // Step 6 needs step 5: the band is what says whether the modem may be used at all.
    {Step::kUplink, "uplink", kUplinkBoundMs, kUplinkLeastMs, stepBit(Step::kBand)},
    // Step 7 needs step 4 (the radio's own clock, as STATUS reported it) and step 6 (a time to
    // give it). Either missing and there is nothing to do that §5.4 would allow.
    {Step::kClock, "clock", kClockBoundMs, kLeastMs,
     stepBit(Step::kState) | stepBit(Step::kUplink)},
    // Step 8 needs step 6: the seam is reached over the uplink.
    {Step::kBackend, "backend", kBackendBoundMs, kBackendLeastMs, stepBit(Step::kUplink)},
    // Step 9 needs step 5: the band is what says whether an update may be started or a flash
    // made. It does **not** need step 8 - a console command is the other way an update is asked
    // for, and a trial that must finish before this wake may sleep is asked for by nobody.
    {Step::kUpdate, "update", kUpdateBoundMs, kLeastMs, stepBit(Step::kBand)},
    // Step 10 needs nothing, has no least, and is the invariant: **every wake ends here**.
    {Step::kSleep, "sleep", kSleepBoundMs, 0, 0},
};

const StepDef& def(Step step) {
  const std::size_t i = static_cast<std::size_t>(step);
  return kSteps[i < kStepCount ? i : 0];
}

// -- the record ---------------------------------------------------------------------------------

void stepLetters(const Pass& pass, char* out, std::size_t cap) {
  if (cap == 0) return;
  std::size_t i = 0;
  for (; i < kStepCount && i + 1 < cap; ++i) {
    out[i] = letter(pass.step[i].outcome);
  }
  out[i] = 0;
}

std::size_t recordLine(const Pass& pass, char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  char letters[interfaces::kBackendStepLetters] = {};
  stepLetters(pass, letters, sizeof letters);
  // The Walter's own attention, as one or two letters (task p4-04): `t` the token was refused,
  // `h` a restart was asked for and not taken. docs/cycle.md has the table beside the steps'.
  char agent[3] = {};
  agentAttentionLetters(pass.agent_attention, agent);
  // Why nothing was sent, when nothing was and the exchange said so (task p10-02). Neither
  // *answered* nor *it did not say* prints a clause: the first is the ordinary wake and the
  // second is a step that never ran or a backend with no word for it.
  const bool silent = pass.reached != interfaces::Reached::kAnswered &&
                      pass.reached != interfaces::Reached::kUnsaid;
  const unsigned minutes = static_cast<unsigned>(pass.interval_s / 60u);
  const int n = std::snprintf(
      out, cap, "wake %lu %s: radio %s, band %s, steps %s, %lu s awake, next %u min%s%s%s%s%s%s%s",
      static_cast<unsigned long>(pass.wakes), cycle::name(pass.reason),
      supervisor::describe(pass.radio), power::name(pass.band), letters,
      static_cast<unsigned long>(pass.awake_s), minutes,
      pass.attention != 0 ? ", attention " : "",
      pass.attention != 0 ? link::client::attentionName(pass.attention) : "",
      agent[0] != 0 ? ", agent " : "", agent,
      pass.carried != interfaces::Ask::kNothing ? ", one update step" : "",
      silent ? ", nothing sent: " : "", silent ? interfaces::describe(pass.reached) : "");
  if (n <= 0) return 0;
  return static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
}

void encode(const WakePayload& p, std::uint8_t out[kWakePayloadBytes]) {
  for (std::size_t i = 0; i < kWakePayloadBytes; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(p.wake);
  out[1] = static_cast<std::uint8_t>(p.band);
  out[2] = static_cast<std::uint8_t>(p.radio);
  out[3] = p.flags;
  for (std::size_t i = 0; i < kStepCount; ++i) {
    out[4 + i] = static_cast<std::uint8_t>(p.step[i]);
  }
  out[14] = p.asks;
  out[15] = static_cast<std::uint8_t>(p.carried);
  put32(out + 16, p.awake_s);
  put32(out + 20, p.interval_s);
  put32(out + 24, p.at_epoch);
  put32(out + 28, p.wakes);
  out[32] = static_cast<std::uint8_t>(p.reached);
  put32(out + 33, p.owed_s);
}

bool decode(const update::Entry& e, WakePayload* out) {
  // **The shorter payload is read too** (task p10-02): a unit's journal holds the entries its
  // earlier firmware wrote, and the whole point of the log is that an update does not blind it.
  // What the two fields past byte 32 say about such an entry is *nothing was said*, because
  // nothing was.
  if (e.kind != update::kind::kWakeCycle || e.size < kWakePayloadBytesV1 || out == nullptr) {
    return false;
  }
  if (e.payload[0] >= static_cast<std::uint8_t>(Wake::kCount)) return false;
  if (e.payload[1] >= static_cast<std::uint8_t>(power::Band::kCount)) return false;
  if (e.payload[15] >= static_cast<std::uint8_t>(interfaces::Ask::kCount)) return false;
  out->wake = static_cast<Wake>(e.payload[0]);
  out->band = static_cast<power::Band>(e.payload[1]);
  out->radio = static_cast<supervisor::Health>(e.payload[2]);
  out->flags = e.payload[3];
  for (std::size_t i = 0; i < kStepCount; ++i) {
    const std::uint8_t byte = e.payload[4 + i];
    if (byte >= static_cast<std::uint8_t>(Outcome::kCount)) return false;
    out->step[i] = static_cast<Outcome>(byte);
  }
  out->asks = e.payload[14];
  out->carried = static_cast<interfaces::Ask>(e.payload[15]);
  out->awake_s = get32(e.payload + 16);
  out->interval_s = get32(e.payload + 20);
  out->at_epoch = get32(e.payload + 24);
  out->wakes = get32(e.payload + 28);
  out->reached = interfaces::Reached::kUnsaid;
  out->owed_s = 0;
  if (e.size >= kWakePayloadBytes) {
    if (e.payload[32] >= static_cast<std::uint8_t>(interfaces::Reached::kCount)) return false;
    out->reached = static_cast<interfaces::Reached>(e.payload[32]);
    out->owed_s = get32(e.payload + 33);
  }
  return true;
}

// -- the object ---------------------------------------------------------------------------------

WakeCycle::WakeCycle(DutyCycle& duty, link::client::Client& radio, uplink::UplinkPolicy& uplink,
                     interfaces::Backend& backend, TelemetryRing& readings, NetworkClock& wall,
                     interfaces::Clock& clock, supervisor::Supervisor* supervisor,
                     update::RadioUpdate* update, update::SelfUpdate* self, update::Fetch* fetch,
                     image::Store* store, update::Journal* journal, settings::Settings* settings,
                     interfaces::TextOut* out)
    : duty_(duty),
      radio_(radio),
      uplink_(uplink),
      backend_(backend),
      readings_(readings),
      wall_(wall),
      clock_(clock),
      supervisor_(supervisor),
      update_(update),
      self_(self),
      fetch_(fetch),
      store_(store),
      journal_(journal),
      settings_(settings),
      out_(out) {}

void WakeCycle::askFor(interfaces::Ask ask) {
  if (ask == interfaces::Ask::kNothing || ask >= interfaces::Ask::kCount) return;
  wanted_ = static_cast<std::uint8_t>(wanted_ | interfaces::askBit(ask));
}

void WakeCycle::say(const char* text) {
  if (out_ == nullptr || text == nullptr) return;
  out_->write(text, std::strlen(text));
}

void WakeCycle::sayf(const char* format, ...) {
  if (out_ == nullptr) return;
  char line[160];
  std::va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(line, sizeof line, format, args);
  va_end(args);
  if (n > 0) out_->write(line, static_cast<std::size_t>(n) < sizeof line
                                   ? static_cast<std::size_t>(n)
                                   : sizeof line - 1);
}

std::uint32_t WakeCycle::leftMs() const {
  // The time a climb or an update took is **outside** the budget: both are bounded and capped by
  // their own machines (the supervisor's caps, the update's journal), and a wake that spent six
  // minutes on a ladder must still be able to reach its uplink and say so - which is what the
  // task asks for in so many words ("silent: the supervisor's ladder within its caps, the uplink
  // still used to say so").
  const std::uint32_t spent = elapsed(began_ms_, clock_.nowMs());
  const std::uint32_t ordinary = spent > outside_ms_ ? spent - outside_ms_ : 0;
  return kWakeBudgetMs > ordinary ? kWakeBudgetMs - ordinary : 0;
}

std::uint64_t WakeCycle::deadlineFor(Step step) const {
  const StepDef& d = def(step);
  const std::uint32_t left = leftMs();
  const std::uint32_t give = d.bound_ms < left ? d.bound_ms : left;
  return clock_.nowMs() + give;
}

bool WakeCycle::begin(Step step) {
  const StepDef& d = def(step);
  StepRecord& r = pass_.step[static_cast<std::size_t>(step)];
  step_began_ms_ = clock_.nowMs();
  // The preconditions, from the table: every step it needs must have succeeded.
  for (std::size_t i = 0; i < kStepCount; ++i) {
    const Step other = static_cast<Step>(i);
    if ((d.needs & stepBit(other)) == 0) continue;
    if (pass_.ok(other)) continue;
    r.outcome = Outcome::kSkipped;
    r.why = cycle::name(other);  // which step it was waiting for: the table names it
    return false;
  }
  // And the budget. `least_ms` of 0 is a step that is never refused for want of time, which is
  // step 10 and the two that cost nothing.
  if (d.least_ms != 0 && leftMs() < d.least_ms) {
    r.outcome = Outcome::kNoTime;
    r.why = "the wake's budget";
    return false;
  }
  r.deadline_ms = d.bound_ms < leftMs() ? d.bound_ms : leftMs();
  return true;
}

void WakeCycle::finish(Step step, Outcome outcome, const char* why) {
  StepRecord& r = pass_.step[static_cast<std::size_t>(step)];
  r.outcome = outcome;
  if (why != nullptr && why[0] != 0) r.why = why;
  r.spent_ms = elapsed(step_began_ms_, clock_.nowMs());
}

void WakeCycle::allow(std::uint32_t bound_ms) {
  duty_.guard().allow(bound_ms);
  pass_.bound_ms = duty_.guard().bound();
}

void WakeCycle::refuse(Step step, interfaces::Ask ask, interfaces::Refusal why,
                       const char* phrase) {
  readings_.refused(ask, why);
  finish(step, Outcome::kRefused, phrase);
  if (out_ != nullptr) {
    sayf("cycle: %s was asked for and refused: %s\n", interfaces::describe(ask),
         interfaces::describe(why));
  }
}

// -- one pass ------------------------------------------------------------------------------------

const Pass& WakeCycle::run() {
  if (running_) return pass_;  // nothing runs twice: a re-entrant call answers the pass in hand
  running_ = true;
  pass_ = Pass{};
  outside_ms_ = 0;
  update_ran_ = false;
  radio_time_known_ = false;
  radio_time_state_ = 0;
  radio_epoch_ = 0;
  began_ms_ = clock_.nowMs();
  pass_.budget_ms = kWakeBudgetMs;
  // The bound on the wake, which task p3-04 built and left disarmed because nothing reached
  // sleep by itself. This is what reaches sleep.
  duty_.guard().arm();
  pass_.bound_ms = duty_.guard().bound();
  // A wake is counted whether or not the modem is touched: the uplink's backoff is in wakes
  // (core/uplink/policy.h).
  uplink_.beginWake();
  readings_.begin();

  stepWhy();
  stepRadio();
  stepResume();
  stepState();
  stepBand();
  stepUplink();
  stepClock();
  stepBackend();
  stepUpdate();
  stepSleep();

  running_ = false;
  return pass_;
}

const Pass& WakeCycle::dry() {
  // Nothing is done: every precondition is evaluated against what is known *now*, every bound is
  // shown, and no byte goes anywhere. What a person wants of `cycle dry` is "what would this
  // wake do", and the honest answer is the table and the state, not a rehearsal that spends the
  // battery it is asking about.
  Pass dry{};
  dry.dry = true;
  dry.ending = Ending::kDry;
  dry.budget_ms = kWakeBudgetMs;
  dry.left_ms = kWakeBudgetMs;
  dry.bound_ms = duty_.guard().bound();
  dry.reason = duty_.wake().wake;
  dry.attention = duty_.wake().attention.reason;
  dry.wakes = duty_.wake().wakes;
  dry.radio = supervisor_ != nullptr ? supervisor_->health() : supervisor::Health::kUnknown;
  dry.silence = supervisor_ != nullptr ? supervisor_->status().silence_seen
                                       : supervisor::Silence::kNone;
  dry.on_trial = self_ != nullptr && self_->onTrial();
  dry.time_known = wall_.known();
  // The band, from the readings this wake already has: `decide()` is a pure function and takes
  // nothing. Not remembered - `decided()` is left as it was, so a dry run does not change what
  // the next real sleep would use.
  dry.decision = duty_.battery().decide(power::Inputs{});
  dry.band = dry.decision.band;
  dry.interval_s = dry.decision.interval_seconds;

  for (std::size_t i = 0; i < kStepCount; ++i) {
    const StepDef& d = kSteps[i];
    StepRecord& r = dry.step[i];
    r.deadline_ms = d.bound_ms;
    bool blocked = false;
    for (std::size_t k = 0; k < kStepCount; ++k) {
      if ((d.needs & stepBit(static_cast<Step>(k))) == 0) continue;
      // In a dry run a step's dependency is read as "would it be likely to succeed": the radio's
      // state as the supervisor last knew it, the band, and the uplink's backoff. It is a
      // forecast and says so.
      const Step other = static_cast<Step>(k);
      if (other == Step::kRadio && dry.radio == supervisor::Health::kUnknown) blocked = true;
      if (other == Step::kState && dry.radio != supervisor::Health::kHealthy) blocked = true;
      if (other == Step::kUplink && !dry.decision.modem_this_wake) blocked = true;
    }
    if (blocked) {
      r.outcome = Outcome::kSkipped;
      r.why = "a precondition would not hold";
      continue;
    }
    if (static_cast<Step>(i) == Step::kUplink && !dry.decision.modem_this_wake) {
      r.outcome = Outcome::kSkipped;
      r.why = "the band does not allow the modem in this wake";
      continue;
    }
    if (static_cast<Step>(i) == Step::kUplink && !uplink_.mayTry()) {
      r.outcome = Outcome::kSkipped;
      r.why = "the uplink's backoff";
      continue;
    }
    r.outcome = Outcome::kDone;
    r.why = "it would run";
  }
  pass_ = dry;
  return pass_;
}

// -- step 1: why ---------------------------------------------------------------------------------

void WakeCycle::stepWhy() {
  (void)begin(Step::kWhy);  // it needs nothing and has no least: it always runs
  if (!duty_.begun()) {
    // main/ calls begin() before the console so that §8's window is listened to as early as
    // possible; a test, and `cycle now`, can reach a pass that has not.
    duty_.begin();
  }
  const WakeInfo& w = duty_.wake();
  pass_.reason = w.wake;
  pass_.attention = w.attention.reason;
  pass_.wakes = w.wakes;
  pass_.time_known = wall_.known();
  finish(Step::kWhy, Outcome::kDone, cycle::describe(w.wake));
  if (out_ != nullptr) sayf("cycle: wake %lu, %s\n", static_cast<unsigned long>(w.wakes),
                            cycle::describe(w.wake));
}

// -- step 2: the radio ---------------------------------------------------------------------------

void WakeCycle::stepRadio() {
  if (!begin(Step::kRadio)) return;
  if (supervisor_ == nullptr) {
    finish(Step::kRadio, Outcome::kNothing, "this build has no supervisor");
    return;
  }
  // The heartbeat, and whatever follows from it. The first step() asks; a silence is classified
  // (§9) and only `kNotAnswering` begins a climb.
  std::uint64_t deadline = deadlineFor(Step::kRadio);
  std::uint32_t slices = 0;
  bool raised = false;
  for (;;) {
    const supervisor::Step s = supervisor_->step();
    (void)s;
    if (supervisor_->climbing() && !raised) {
      // Something is in hand: a silence to classify (§9), or a climb. Either way this wake is
      // allowed to hold open for the ladder's own bound - a bound is a maximum and a
      // classification does not reach it - and whether it really was a *climb* is read off the
      // incident below, not off this.
      raised = true;
      allow(kLadderBoundMs);
      deadline = step_began_ms_ + kLadderBoundMs;
    }
    if (!supervisor_->climbing()) break;
    if (clock_.nowMs() >= deadline) break;
    if (++slices >= kSlicesMax) break;
    if (duty_.tick()) {  // the bound on the wake ended it
      pass_.ending = Ending::kOverran;
      break;
    }
    clock_.delayMs(supervisor::kStepIdleMs);
  }
  pass_.radio = supervisor_->health();
  pass_.silence = supervisor_->status().silence_seen;
  // **A climb is an incident, not a silence.** Three of §9's four silences are explanations and
  // the supervisor does nothing about them; only `kNotAnswering` begins a climb, and the
  // incident's cause is what says one was begun.
  pass_.climbed = supervisor_->status().cause != supervisor::Cause::kNone ||
                  supervisor_->status().rung != supervisor::Rung::kNone;
  if (pass_.climbed) {
    outside_ms_ += elapsed(step_began_ms_, clock_.nowMs());
  }
  // **Every answer is an outcome of this step and not a failure of it.** Only "nothing was
  // asked" is a failure: that is a supervisor that could not even put a PING on the wire.
  if (pass_.radio == supervisor::Health::kUnknown) {
    finish(Step::kRadio, Outcome::kFailed, "nothing was asked of the radio");
    return;
  }
  finish(Step::kRadio, Outcome::kDone, supervisor::describe(pass_.radio));
  if (out_ != nullptr) {
    sayf("cycle: the radio is %s (%s)\n", supervisor::describe(pass_.radio),
         supervisor::describe(pass_.silence));
  }
}

// -- step 3: resume ------------------------------------------------------------------------------

void WakeCycle::stepResume() {
  if (!begin(Step::kResume)) return;
  if (update_ == nullptr && self_ == nullptr) {
    finish(Step::kResume, Outcome::kNothing, "this build has no update machine");
    return;
  }
  // This firmware's own trial: noted here, run through the steps below, and finished before this
  // wake may sleep - a deep sleep during a trial is a rollback of the image being tried
  // (core/cycle/sleep.h, task p3-04's finding).
  if (self_ != nullptr && self_->onTrial()) {
    pass_.on_trial = true;
    allow(kTrialBoundMs);
  }
  if (update_ == nullptr) {
    finish(Step::kResume, pass_.on_trial ? Outcome::kDone : Outcome::kNothing,
           pass_.on_trial ? "this firmware is on trial: its conditions run through the steps below"
                          : "nothing of the radio's update was journalled");
    return;
  }
  // The journal. `resume()` is idempotent: with a terminal last entry it answers false and the
  // machine stays idle, so a console that has already resumed (main/main.cpp) is not a problem.
  if (!update_->active()) (void)update_->resume();
  if (!update_->active()) {
    finish(Step::kResume, pass_.on_trial ? Outcome::kDone : Outcome::kNothing,
           pass_.on_trial ? "this firmware is on trial: its conditions run through the steps below"
                          : "nothing of the radio's update was journalled");
    return;
  }
  allow(kUpdateBoundMs);
  const std::uint64_t deadline = step_began_ms_ + kUpdateBoundMs;
  const bool finished = driveUpdate(deadline);
  update_ran_ = true;  // S-7: this wake's one update step
  outside_ms_ += elapsed(step_began_ms_, clock_.nowMs());
  if (!finished) {
    finish(Step::kResume, Outcome::kNoTime,
           "the update is mid-run: the journal carries it to the next wake");
    return;
  }
  finish(Step::kResume, Outcome::kDone, update::describe(update_->state()));
  if (out_ != nullptr) {
    sayf("cycle: the resumed update ended %s\n", update::describe(update_->state()));
  }
}

// -- step 4: the radio's state -------------------------------------------------------------------

void WakeCycle::stepState() {
  if (!begin(Step::kState)) return;
  // A radio mid-update is not asked anything: "mid-update (resumed, nothing else touches the
  // radio)". The supervisor's own classification is what says so.
  if (pass_.silence == supervisor::Silence::kUpdateRunning) {
    finish(Step::kState, Outcome::kSkipped, "an update is in progress: not ours to disturb");
    return;
  }
  if (pass_.radio == supervisor::Health::kScanner) {
    // §9: NoiseScope speaks lines, not frames. No clock to set, no telemetry to read - and the
    // wake still reports and sleeps, which is the whole point of its being a state and not a
    // fault.
    finish(Step::kState, Outcome::kSkipped, "the scanner is running: it answers no frame");
    return;
  }
  const link::client::Answer<link::StatusResponse> status = radio_.status();
  if (!status.ok()) {
    finish(Step::kState, Outcome::kFailed, link::client::describe(status.fault));
    return;
  }
  radio_time_state_ = status.message.time_state;
  radio_epoch_ = status.message.epoch;
  radio_time_known_ = true;
  known_.radio_uptime_s = status.message.uptime_s;
  known_.radio_reset_reason = status.message.reset_reason;
  known_.radio_time_state = status.message.time_state;
  known_.radio_epoch = status.message.epoch;
  known_.radio_flags = status.message.flags;
  known_.obs_pending = status.message.obs_pending;
  known_.obs_dropped = status.message.obs_dropped;
  // §8: a pending attention is visible in STATUS until the Walter has made a request, so a wake
  // that woke for its own reasons sees it too. The request above is what acknowledged it; the
  // reason is kept.
  if (status.message.attention != link::attention::kNone && pass_.attention == 0) {
    pass_.attention = status.message.attention;
  }
  if (supervisor_ != nullptr) supervisor_->listen();

  // The battery, through the duty cycle's own ring (it is what the band is decided from).
  const bool reading = duty_.readPower();
  Taken taken{};
  taken.wake = static_cast<std::uint8_t>(pass_.reason);
  interfaces::WallTime now{};
  if (wall_.now(&now)) {
    taken.time_known = true;
    taken.at_epoch = now.epoch;
  }
  if (reading && duty_.battery().sampleCount() != 0) {
    const power::Sample& s = duty_.battery().sampleAt(0);
    taken.battery_known = true;
    taken.millivolts = s.millivolts;
    taken.current_known = s.current_known;
    taken.current_ma = s.current_ma;
  }
  // ENV_GET where the role has it (§5.8). Not a failure of this step: a board without a sensor
  // is a board without a sensor.
  if (radio_.hasCapability(link::cap::kEnv)) {
    const link::client::Answer<link::EnvGetResponse> env = radio_.envGet();
    if (env.ok() && (env.message.valid & 0x01u) != 0) {
      taken.temp_known = true;
      taken.temp_centi = env.message.temp_centi;
    }
  }
  readings_.add(taken);
  finish(Step::kState, Outcome::kDone,
         reading ? "STATUS, the battery and the environment" : "STATUS; no battery reading");
}

// -- step 5: the band ----------------------------------------------------------------------------

void WakeCycle::stepBand() {
  if (!begin(Step::kBand)) return;
  pass_.decision = duty_.decide();
  pass_.band = pass_.decision.band;
  pass_.interval_s = pass_.decision.interval_seconds;
  finish(Step::kBand, Outcome::kDone, pass_.decision.why);
  if (out_ != nullptr) {
    sayf("cycle: band %s (%s), next wake in %lu min\n", power::name(pass_.band),
         pass_.decision.why, static_cast<unsigned long>(pass_.decision.interval_minutes));
  }
}

// -- step 6: the uplink --------------------------------------------------------------------------

void WakeCycle::stepUplink() {
  if (!begin(Step::kUplink)) {
    // A wake that does not use the modem still has to say so, or `saving`'s every-Nth counter
    // would never move (core/cycle/wake_reason.h).
    duty_.modemWasSkipped();
    return;
  }
  if (!pass_.decision.may.modem) {
    duty_.modemWasSkipped();
    finish(Step::kUplink, Outcome::kSkipped, "the band does not allow the modem");
    return;
  }
  if (!pass_.decision.modem_this_wake) {
    duty_.modemWasSkipped();
    finish(Step::kUplink, Outcome::kSkipped, "`saving` uses the modem every fourth wake");
    return;
  }
  if (!uplink_.mayTry()) {
    duty_.modemWasSkipped();
    finish(Step::kUplink, Outcome::kSkipped, "the uplink's backoff");
    return;
  }
  const std::uint32_t left = leftMs();
  const std::uint32_t give = kUplinkBoundMs < left ? kUplinkBoundMs : left;
  const uplink::Result r = uplink_.bringUp(give);
  duty_.modemWasUsed();  // it was used, whether or not it worked: the energy is spent either way
  pass_.step[static_cast<std::size_t>(Step::kUplink)].spent_ms = r.spent_ms;
  known_.uplink.up = r.up;
  known_.uplink.failure = static_cast<std::uint8_t>(r.failure);
  known_.uplink.reached = static_cast<std::uint8_t>(r.reached);
  known_.uplink.spent_ms = r.spent_ms;
  if (!r.up) {
    finish(Step::kUplink, Outcome::kFailed, uplink::describe(r.failure));
    return;
  }
  // The uplink came up: the first of `wake`'s three letters, into this wake's own entry in
  // retained storage so that a boot later the list says the modem was up (task p10-02).
  duty_.reason().didThisWake(kDidModem);
  // The network's time, if it gave one. `no-time` is not a failure of the uplink (task p3-01):
  // an uplink that came up without a time is up.
  if (wall_.give(r.time)) pass_.time_known = true;
  interfaces::Signal signal{};
  if (uplink_.signal(&signal)) {
    known_.uplink.signal_known = true;
    known_.uplink.rsrp_known = signal.rsrp_known;
    known_.uplink.rsrp_dbm = signal.rsrp_dbm;
    known_.uplink.rsrq_known = signal.rsrq_known;
    known_.uplink.rsrq_tenth_db = signal.rsrq_tenth_db;
    known_.uplink.rssi_known = signal.rssi_known;
    known_.uplink.rssi_dbm = signal.rssi_dbm;
    known_.uplink.band = signal.band;
  }
  finish(Step::kUplink, Outcome::kDone,
         pass_.time_known ? "up, with the network's time" : "up, with no time");
}

// -- step 7: the radio's clock -------------------------------------------------------------------

void WakeCycle::stepClock() {
  if (!begin(Step::kClock)) return;
  interfaces::WallTime now{};
  if (!wall_.now(&now)) {
    finish(Step::kClock, Outcome::kSkipped, "the network gave no time");
    return;
  }
  // §5.4: the Walter sets the radio's clock after every restart it sees, and otherwise when the
  // radio's own is unset, restored, or out by more than a named threshold. A clock that is `set`
  // or `rtc` and close enough is left alone: TIME_SET is not free and §5.4 refuses a backwards
  // epoch anyway.
  const bool pending = radio_.timePending();
  const bool unset = radio_time_state_ == link::time_state::kUnset ||
                     radio_time_state_ == link::time_state::kRestored;
  const std::uint32_t skew = radio_epoch_ > now.epoch ? radio_epoch_ - now.epoch
                                                      : now.epoch - radio_epoch_;
  if (!pending && !unset && skew <= kClockSkewS) {
    finish(Step::kClock, Outcome::kNothing, "the radio's clock is set and close enough");
    return;
  }
  const link::client::Answer<link::TimeSetResponse> a =
      radio_.timeSet(now.epoch, link::time_source::kCellular, false);
  if (a.ok()) {
    pass_.clock_set = true;
    finish(Step::kClock, Outcome::kDone, "the radio's clock is set from the network's time");
    return;
  }
  if (a.fault == link::client::Fault::kRefused) {
    // §5.4's own refusal: the radio will not move its clock backwards unless *force* is set, and
    // force is for a person's instruction only. Recorded, and nothing is forced.
    finish(Step::kClock, Outcome::kRefused, "the radio refused the time (§5.4: never backwards)");
    return;
  }
  finish(Step::kClock, Outcome::kFailed, link::client::describe(a.fault));
}

// -- step 8: the backend -------------------------------------------------------------------------

void WakeCycle::stepBackend() {
  if (!begin(Step::kBackend)) return;
  fillKnowledge();
  pass_.readings_offered = readings_.offerTo(&known_);
  answer_ = interfaces::BackendAnswer{};
  // The exchange is given an update's own deadline (task p4-05: the agent's jobs are minutes, and
  // a firmware job is one of the update machines run inside it). What it spends inside a machine of
  // its own comes back as `outside_ms` and is outside the ordinary budget, as a climb's and an
  // update's time are; the rest - a heartbeat, a claim - is inside it. Nothing of this moves a null
  // backend: it answers at once.
  const std::uint64_t deadline = step_began_ms_ + kBackendBoundMs;
  pass_.exchanged = backend_.exchange(known_, deadline, &answer_);
  // Who was reached, or why nobody was (task p10-02). One reading of the pair, here and nowhere
  // else: the journal's flag, the record's clause and `wake`'s letters all come off these three
  // lines, so they cannot tell different stories about the same wake.
  // An exchange that failed says nothing about who was reached - *the exchange failed* is the
  // step's own outcome and the whole of the news - so whatever it left in `out` is dropped here.
  pass_.reached = pass_.exchanged == interfaces::Exchanged::kFailed ? interfaces::Reached::kUnsaid
                                                                   : answer_.reached;
  pass_.jobs_ran = answer_.jobs_ran;
  pass_.owed_s = pass_.reached == interfaces::Reached::kWait ? answer_.sleep_at_least_s : 0;
  if (interfaces::wasAnswered(pass_.exchanged, answer_)) duty_.reason().didThisWake(kDidAnswered);
  if (answer_.jobs_ran > 0) duty_.reason().didThisWake(kDidJobs);
  outside_ms_ += answer_.outside_ms;
  // A firmware job's machine ran inside the exchange for up to those same twenty minutes: that wall
  // time is outside the ordinary budget but still counts against the guard's ceiling, so the bound
  // is raised to cover it - and only when a machine did run (`outside_ms`), so a wake that merely
  // heartbeated keeps the ordinary bound, or the ladder's if the radio was silent and it climbed.
  if (answer_.outside_ms > 0) allow(kBackendBoundMs);
  // What the exchange carried out itself is the wake's business from here: S-7's one update
  // step a wake is held by `update_ran_`, which step 9 reads, and the record says what went.
  pass_.did = answer_.did;
  if (answer_.carried == interfaces::Ask::kUpdateRadio ||
      answer_.carried == interfaces::Ask::kUpdateSelf) {
    update_ran_ = true;
    pass_.carried = answer_.carried;
  } else if (answer_.carried != interfaces::Ask::kNothing &&
             pass_.carried == interfaces::Ask::kNothing) {
    pass_.carried = answer_.carried;
  }
  if (pass_.exchanged == interfaces::Exchanged::kFailed) {
    finish(Step::kBackend, Outcome::kFailed, "the exchange failed");
    return;
  }
  // It worked, so whatever this unit had been refusing has been said. The readings go only when
  // the answer says they were taken.
  readings_.refusalsTold();
  if (answer_.took_readings) {
    readings_.wereTaken(pass_.readings_offered);
    pass_.readings_taken = true;
  }
  if (pass_.exchanged == interfaces::Exchanged::kNothing) {
    finish(Step::kBackend, Outcome::kNothing, "nothing to do");
    return;
  }
  pass_.asks = answer_.asks;
  wanted_ = static_cast<std::uint8_t>(wanted_ | answer_.asks);
  // The settings are applied here and not in step 9: they cost nothing, touch no pin, no modem
  // and no flash, and they are part of the answer rather than a thing to be done with it.
  if (answer_.wants(interfaces::Ask::kSetSettings)) {
    (void)applySettings();
    wanted_ = static_cast<std::uint8_t>(wanted_ & ~interfaces::askBit(interfaces::Ask::kSetSettings));
  }
  finish(Step::kBackend, Outcome::kDone,
         answer_.asks != 0 ? "it asked for something" : "the exchange did something itself");
}

bool WakeCycle::applySettings() {
  bool any = false;
  for (std::size_t i = 0; i < answer_.settings && i < interfaces::kBackendSettingsMax; ++i) {
    const interfaces::BackendSetting& want = answer_.setting[i];
    if (settings_ == nullptr) {
      readings_.refused(interfaces::Ask::kSetSettings, interfaces::Refusal::kNotBuilt);
      continue;
    }
    const settings::KeyDef* key = settings::findKey(want.key);
    if (key == nullptr) {
      // No such setting. **A threshold that protects the hardware is not a setting at all** -
      // the battery floors, the supervisor's caps, the proving period and a wake's budget are
      // build constants (core/settings/keys.h, *What is not here*) - so asking for one lands
      // here, which is the refusal the task asks for.
      readings_.refused(interfaces::Ask::kSetSettings, interfaces::Refusal::kUnknownKey);
      if (out_ != nullptr) sayf("cycle: refused to change `%s`: no such setting\n", want.key);
      continue;
    }
    if (key->secret) {
      // A secret is a person's, at the console, over a cable (docs/console.md). Nothing that
      // comes over the air sets one.
      readings_.refused(interfaces::Ask::kSetSettings, interfaces::Refusal::kSecretKey);
      if (out_ != nullptr) sayf("cycle: refused to change `%s`: it is a secret\n", key->name);
      continue;
    }
    char err[96] = {};
    if (!settings_->put(*key, want.value, err, sizeof err)) {
      readings_.refused(interfaces::Ask::kSetSettings, interfaces::Refusal::kBadValue);
      if (out_ != nullptr) sayf("cycle: refused to change `%s`: %s\n", key->name, err);
      continue;
    }
    any = true;
    if (out_ != nullptr) sayf("cycle: `%s` changed\n", key->name);
  }
  return any;
}

// -- step 9: at most one update step -------------------------------------------------------------

void WakeCycle::stepUpdate() {
  if (!begin(Step::kUpdate)) return;

  // The trial first, and it is not optional: a wake that found itself on trial has to finish the
  // trial before it may sleep (core/cycle/sleep.h refuses a sleep while on trial).
  if (pass_.on_trial && self_ != nullptr) {
    allow(kTrialBoundMs);
    const std::uint64_t deadline = step_began_ms_ + kTrialBoundMs;
    const bool ended = runTrial(deadline);
    outside_ms_ += elapsed(step_began_ms_, clock_.nowMs());
    update_ran_ = true;
    pass_.carried = interfaces::Ask::kUpdateSelf;
    if (!self_->onTrial()) {
      finish(Step::kUpdate, Outcome::kDone, update::describe(self_->status().state));
      if (self_->status().state == update::SelfState::kRolledBack) {
        pass_.ending = Ending::kRestarting;
      }
      return;
    }
    finish(Step::kUpdate, ended ? Outcome::kFailed : Outcome::kNoTime,
           "the trial has not ended: this wake cannot sleep until it does");
    return;
  }

  if (wanted_ == 0) {
    finish(Step::kUpdate, Outcome::kNothing, "nothing was asked for");
    return;
  }

  bool did = false;

  // A fetch is a download, not an update step: it may happen in the same wake as one, and the
  // budget is what decides whether it does.
  if ((wanted_ & interfaces::askBit(interfaces::Ask::kFetchRelease)) != 0) {
    if (!pass_.decision.may.modem || !pass_.ok(Step::kUplink)) {
      refuse(Step::kUpdate, interfaces::Ask::kFetchRelease,
             pass_.decision.may.modem ? interfaces::Refusal::kFailed
                                      : interfaces::Refusal::kBandForbids,
             "a fetch needs the uplink");
      return;
    }
    if (fetch_ == nullptr || store_ == nullptr) {
      refuse(Step::kUpdate, interfaces::Ask::kFetchRelease, interfaces::Refusal::kNotBuilt,
             "this build has no image store");
      return;
    }
    const std::uint64_t deadline = deadlineFor(Step::kUpdate);
    if (!runFetch(deadline)) return;  // runFetch records its own outcome on a refusal
    did = true;
  }

  // The ladder, when the backend or a person asked for one.
  if ((wanted_ & interfaces::askBit(interfaces::Ask::kRunLadder)) != 0) {
    if (supervisor_ == nullptr) {
      refuse(Step::kUpdate, interfaces::Ask::kRunLadder, interfaces::Refusal::kNotBuilt,
             "this build has no supervisor");
      return;
    }
    if (!pass_.decision.may.power_cycle) {
      refuse(Step::kUpdate, interfaces::Ask::kRunLadder, interfaces::Refusal::kBandForbids,
             "the band does not allow the rail to be taken away");
      return;
    }
    if (supervisor_->runLadder()) {
      pass_.climbed = true;
      allow(kLadderBoundMs);
      const std::uint64_t deadline = clock_.nowMs() + kLadderBoundMs;
      const std::uint64_t from = clock_.nowMs();
      (void)driveLadder(deadline);
      outside_ms_ += elapsed(from, clock_.nowMs());
      pass_.radio = supervisor_->health();
      did = true;
    }
  }

  // S-7: **one update step a wake, of one MCU.** The Walter goes first when both are asked,
  // because a link-version change has it lead (docs/ARCHITECTURE.md §7) and because its own
  // update ends in a restart - so the radio's happens at the next wake, if this one confirmed.
  const bool wants_self = (wanted_ & interfaces::askBit(interfaces::Ask::kUpdateSelf)) != 0;
  const bool wants_radio = (wanted_ & interfaces::askBit(interfaces::Ask::kUpdateRadio)) != 0;
  if (wants_self || wants_radio) {
    if (update_ran_) {
      refuse(Step::kUpdate, wants_self ? interfaces::Ask::kUpdateSelf
                                       : interfaces::Ask::kUpdateRadio,
             interfaces::Refusal::kOneUpdateOnly, "one update step a wake (S-7)");
      return;
    }
    if (!pass_.decision.may.update) {
      refuse(Step::kUpdate, wants_self ? interfaces::Ask::kUpdateSelf
                                       : interfaces::Ask::kUpdateRadio,
             interfaces::Refusal::kBandForbids, "the band does not allow an update to be started");
      return;
    }
    if (wants_self && wants_radio) {
      // The one that does not go is recorded and told at the next exchange; a backend that still
      // wants it asks again, and the next wake carries it out.
      readings_.refused(interfaces::Ask::kUpdateRadio, interfaces::Refusal::kOneUpdateOnly);
    }
    const bool ran = wants_self ? runSelfUpdate() : runRadioUpdate();
    if (!ran) return;  // each records its own outcome
    did = true;
  }

  if (!did) {
    finish(Step::kUpdate, Outcome::kNothing, "nothing that this wake could carry out");
    return;
  }
  finish(Step::kUpdate, Outcome::kDone, interfaces::describe(pass_.carried));
}

bool WakeCycle::runTrial(std::uint64_t deadline_ms) {
  std::uint32_t slices = 0;
  while (self_->onTrial()) {
    const update::SelfStep s = self_->step();
    if (s.changed) {
      if (out_ != nullptr) sayf("cycle: the trial %s\n", s.note);
      return true;
    }
    if (clock_.nowMs() >= deadline_ms) return false;
    if (++slices >= kSlicesMax) return false;
    if (duty_.tick()) {
      pass_.ending = Ending::kOverran;
      return false;
    }
    clock_.delayMs(update::kStepIdleMs);
  }
  return true;
}

bool WakeCycle::runSelfUpdate() {
  update::SwitchRequest request{};
  // The supervisor has classified the radio; a radio it knows is down must not condemn a good
  // Walter image (core/update/self_update.h, the trial's second condition). The rule is the
  // supervisor's own and is written once (core/supervisor/supervisor.h).
  request.radio_known_down = supervisor::radioKnownDown(pass_.radio);
  const update::SelfReason why = self_->switchNow(request);
  update_ran_ = true;
  if (why != update::SelfReason::kNone) {
    refuse(Step::kUpdate, interfaces::Ask::kUpdateSelf, interfaces::Refusal::kFailed,
           update::describe(why));
    return false;
  }
  // On a board this is not reached: switchNow() restarts. On the host the restart is recorded.
  pass_.carried = interfaces::Ask::kUpdateSelf;
  pass_.ending = Ending::kRestarting;
  finish(Step::kUpdate, Outcome::kDone, "the other slot is the next to boot");
  return true;
}

bool WakeCycle::runRadioUpdate() {
  update::Request request{};
  request.kind = update::Kind::kUpdate;
  request.slot = radio_.slot();
  request.link_known_down = supervisor::linkKnownDown(pass_.radio);
  if (!update_->start(request)) {
    refuse(Step::kUpdate, interfaces::Ask::kUpdateRadio, interfaces::Refusal::kFailed,
           update::describe(update_->reason()));
    return false;
  }
  update_ran_ = true;
  pass_.carried = interfaces::Ask::kUpdateRadio;
  allow(kUpdateBoundMs);
  const std::uint64_t from = clock_.nowMs();
  const std::uint64_t deadline = from + kUpdateBoundMs;
  const bool finished = driveUpdate(deadline);
  outside_ms_ += elapsed(from, clock_.nowMs());
  if (!finished) {
    finish(Step::kUpdate, Outcome::kNoTime,
           "the update is mid-run: the journal carries it to the next wake");
    return false;
  }
  finish(Step::kUpdate, Outcome::kDone, update::describe(update_->state()));
  return true;
}

bool WakeCycle::runFetch(std::uint64_t deadline_ms) {
  // The base URL is the answer's, or `fetch.url` when a console command asked. Nothing here
  // knows a path: the fetch appends `manifest.json` itself (core/update/fetch.h).
  const char* base = answer_.base_url[0] != 0 ? answer_.base_url
                                              : (settings_ != nullptr ? settings_->fetchUrl() : "");
  if (base == nullptr || base[0] == 0) {
    refuse(Step::kUpdate, interfaces::Ask::kFetchRelease, interfaces::Refusal::kBadUrl,
           "no base URL to fetch from");
    return false;
  }
  net::Budget budget{};
  const std::uint64_t now = clock_.nowMs();
  budget.seconds = deadline_ms > now ? static_cast<std::uint32_t>((deadline_ms - now) / 1000u) : 0;
  const image::Which which = answer_.into_scanner ? image::Which::kScanner
                                                  : image::Which::kCandidate;
  const update::FetchResult r = fetch_->run(base, which, radio_.slot(), budget);
  pass_.carried = interfaces::Ask::kFetchRelease;
  if (r.verdict == update::FetchVerdict::kStored) {
    finish(Step::kUpdate, Outcome::kDone, "a release is in the candidate slot");
    return true;
  }
  if (r.verdict == update::FetchVerdict::kBudget) {
    // Half a download is not a failure: the bytes and the store's progress record stay, and the
    // next wake continues where this one ended - which is also what shortens the next sleep when
    // the band is `plenty` (step 10).
    finish(Step::kUpdate, Outcome::kNoTime, "the download stopped on its budget and will resume");
    return false;
  }
  refuse(Step::kUpdate, interfaces::Ask::kFetchRelease, interfaces::Refusal::kFailed,
         update::describe(r.verdict, r.own_image));
  return false;
}

bool WakeCycle::driveLadder(std::uint64_t deadline_ms) {
  std::uint32_t slices = 0;
  while (supervisor_->climbing()) {
    const supervisor::Step s = supervisor_->step();
    if (s.changed && out_ != nullptr) sayf("cycle: the ladder: %s\n", s.note);
    if (!supervisor_->climbing()) return true;
    if (clock_.nowMs() >= deadline_ms) return false;
    if (++slices >= kSlicesMax) return false;
    if (duty_.tick()) {
      pass_.ending = Ending::kOverran;
      return false;
    }
    if (s.waited) clock_.delayMs(supervisor::kStepIdleMs);
  }
  return true;
}

bool WakeCycle::driveUpdate(std::uint64_t deadline_ms) {
  std::uint32_t slices = 0;
  while (update_->active()) {
    const update::StepResult s = update_->step();
    if (s.changed && out_ != nullptr) sayf("cycle: the update: %s\n", s.note);
    if (!update_->active()) return true;
    if (clock_.nowMs() >= deadline_ms) return false;
    if (++slices >= kSlicesMax) return false;
    if (duty_.tick()) {
      pass_.ending = Ending::kOverran;
      return false;
    }
    if (s.waited) clock_.delayMs(update::kStepIdleMs);
  }
  return true;
}

// -- step 10: sleep ------------------------------------------------------------------------------

void WakeCycle::stepSleep() {
  (void)begin(Step::kSleep);  // it needs nothing and has no least: every wake ends here
  // The Walter's own attention, read **here** and not at step 1: what raises it is step 8's
  // agent, so a 401 of this wake belongs in this wake's record (task p4-04).
  pass_.agent_attention = duty_.reason().agentAttention();
  pass_.awake_s = static_cast<std::uint32_t>(clock_.nowMs() / 1000u);
  // Unless a restart has been asked for. On the board the call that asked for one does not
  // return - the Walter's own update switching slots, a trial rejecting itself
  // (core/update/self_update.h) - and on the host it does, so this is where the difference is
  // made: **a wake that is restarting does not also sleep.** The next boot is the next wake,
  // and it is a boot and not a wake from deep sleep.
  if (pass_.ending == Ending::kRestarting) {
    finish(Step::kSleep, Outcome::kNothing, "a restart was asked for: this wake ends in a boot");
    journalWake();
    return;
  }
  // What the wake spent, counted **here and not after the sleeper**: on the board the sleeper
  // does not return, and on the host it advances the clock by the interval it was asked for -
  // so a figure taken after it would be the sleep and not the wake.
  pass_.spent_ms = elapsed(began_ms_, clock_.nowMs());
  pass_.outside_ms = outside_ms_;
  pass_.left_ms = leftMs();
  pass_.interval_s = pass_.decision.interval_seconds;

  // Shortened when a download is half done and the band is `plenty`, and **never in `saving` or
  // `critical`** (the task). A release that arrives in a morning instead of a day costs a few
  // more wakes of a pack that can pay for them.
  std::uint32_t seconds = pass_.interval_s;
  if (pass_.band == power::Band::kPlenty && store_ != nullptr) {
    image::Record record{};
    const bool half =
        store_->read(image::Which::kCandidate, radio_.slot(), &record) == image::StoreError::kNone &&
        record.transfer && record.transferred != 0 && record.transferred < record.transfer_size;
    if (half && kDownloadMinutes * 60u < seconds) {
      seconds = kDownloadMinutes * 60u;
      pass_.shortened = true;
    }
  }
  // Lengthened when whoever is behind the seam owes a wait (task p4-05): a backoff or a
  // Retry-After the agent's loop carried out of its wake, or an interval the server accepted that
  // is longer than the policy's own. Never shortened by it: the battery wins, and a week is the
  // most a sleep may be asked for (core/settings/keys.h's own ceiling for `wake.interval`).
  bool lengthened = false;
  if (pass_.exchanged != interfaces::Exchanged::kFailed || answer_.sleep_at_least_s != 0) {
    const std::uint32_t at_least = answer_.sleep_at_least_s < kSleepAtMostS ? answer_.sleep_at_least_s : kSleepAtMostS;
    if (at_least > seconds) {
      seconds = at_least;
      lengthened = true;
    }
  }
  pass_.lengthened = lengthened;
  pass_.interval_s = seconds;

  // This wake ran to its end, so its letters are a record and not a silence: `kDidSaid` is what
  // tells `wake`'s `---` (it did none of the three) from its `???` (nobody wrote it down) - a
  // wake ended by a reset, a crash or the bound keeps only the bits it had reached (task p10-02).
  duty_.reason().didThisWake(kDidSaid);
  // The wake's record goes to the journal **before** the sleep, because a sleep does not return.
  // A sleep that is then refused writes its own entry after this one (core/cycle/sleep.h), and
  // the two together are the story.
  pass_.step[static_cast<std::size_t>(Step::kSleep)].outcome = Outcome::kDone;
  journalWake();
  if (out_ != nullptr) {
    char line[224] = {};
    (void)recordLine(pass_, line, sizeof line);
    say(line);
    say("\n");
  }
  // The record has carried the Walter's own attention, so one raise is reported once; a
  // condition that is still there raises it again at the next wake
  // (core/cycle/agent_attention.h). A dry run does not reach this function and clears nothing.
  if (pass_.agent_attention != 0) duty_.reason().clearAgentAttention();

  const SleepResult r = (pass_.shortened || pass_.lengthened) ? duty_.sleepFor(seconds, false) : duty_.sleepNow();
  pass_.sleep = r;
  if (r.refusal != SleepRefusal::kNone) {
    finish(Step::kSleep, Outcome::kFailed, describe(r.refusal));
    pass_.ending = Ending::kSleepRefused;
    if (out_ != nullptr) sayf("cycle: the sleep was refused: %s\n", describe(r.refusal));
    return;
  }
  // On the board this is not reached. On the host the fake sleeper returns, which is how a test
  // walks a month of wakes.
  duty_.guard().disarm();
  finish(Step::kSleep, Outcome::kDone, "asleep");
  if (pass_.ending == Ending::kRunning) pass_.ending = Ending::kSlept;
}

void WakeCycle::journalWake() {
  if (journal_ == nullptr) return;
  // News, and nothing else: the arithmetic is in the header and is task p3-04's.
  bool news = false;
  for (std::size_t i = 0; i < kStepCount; ++i) {
    const Outcome o = pass_.step[i].outcome;
    if (o == Outcome::kFailed || o == Outcome::kRefused || o == Outcome::kNoTime) news = true;
  }
  if (pass_.decision.changed) news = true;
  if (pass_.climbed || pass_.on_trial) news = true;
  // **The radio's clock being set is not news.** §5.4 has the Walter set it "after every HELLO
  // whose boot_id it has not seen", and a Walter out of deep sleep has seen none - its RAM is
  // gone - so step 7 sets the clock at *every* wake. It is in the entry's flags when there is an
  // entry, and it is not a reason for one.
  if (pass_.carried != interfaces::Ask::kNothing) news = true;
  if (pass_.exchanged == interfaces::Exchanged::kAsked) news = true;
  if (pass_.reason == Wake::kCrash || pass_.reason == Wake::kWatchdog) news = true;
  // **A timer wake that still owes a wait is news** (task p10-02), and nothing else about a wake
  // that sent nothing is.
  //
  // Since task p10-01 a sleep takes its own length off the wait, and step 10 above lengthens the
  // sleep to cover one the seam carried out - so a wake that comes round on its timer and *still*
  // owes a wait means a sleep did not cover its hold. That is the failure of the night of
  // 8 November 2026, it must leave a trace, and it costs at most one entry per failed server call
  // (a hold under ninety per cent of the interval is spent whole by one sleep; a longer one costs
  // a second wake and then it too is spent). The worst a night of them could cost is one entry a
  // wake - 48 at the quarter-hour interval, so the 1024-entry log would still take three weeks of
  // nothing but these to wrap.
  //
  // The other reasons are **not** news, each for its own reason: a unit with no server
  // configured would write one entry every wake for ever, which is the log wrapped in ten days;
  // *no uplink* is already news through step 6's failure; and a halt on a 401 raises the Walter's
  // own attention, which the record carries and `status` says. A wake on the radio's line inside
  // a hold is expected and p10-01 documented it, so it is not news either.
  if (pass_.reason == Wake::kTimer && pass_.reached == interfaces::Reached::kWait) news = true;
  if (!news) return;

  WakePayload p{};
  p.wake = pass_.reason;
  p.band = pass_.band;
  p.radio = pass_.radio;
  p.flags = static_cast<std::uint8_t>(
      (pass_.on_trial ? kWakeFlagOnTrial : 0) | (pass_.clock_set ? kWakeFlagClockSet : 0) |
      (interfaces::wasAnswered(pass_.exchanged, answer_) ? kWakeFlagAnswered : 0) |
      (pass_.readings_taken ? kWakeFlagReadingsTaken : 0) |
      (pass_.decision.changed ? kWakeFlagBandChanged : 0) |
      (pass_.climbed ? kWakeFlagClimbed : 0) | (pass_.shortened ? kWakeFlagShortened : 0) |
      kWakeFlagSlept);
  for (std::size_t i = 0; i < kStepCount; ++i) p.step[i] = pass_.step[i].outcome;
  p.asks = pass_.asks;
  p.carried = pass_.carried;
  p.reached = pass_.reached;
  p.owed_s = pass_.owed_s;
  p.awake_s = pass_.awake_s;
  p.interval_s = pass_.interval_s;
  interfaces::WallTime now{};
  p.at_epoch = wall_.now(&now) ? now.epoch : 0;
  p.wakes = pass_.wakes;
  std::uint8_t bytes[kWakePayloadBytes];
  encode(p, bytes);
  (void)journal_->append(update::kind::kWakeCycle, bytes, kWakePayloadBytes);
}

// -- what crosses the seam -----------------------------------------------------------------------

void WakeCycle::fillKnowledge() {
  fillFirmware();
  fillStore();
  fillSupervisor();

  interfaces::BackendWake& w = known_.wake;
  w.reason = static_cast<std::uint8_t>(pass_.reason);
  w.attention = pass_.attention;
  w.radio = static_cast<std::uint8_t>(pass_.radio);
  w.band = static_cast<std::uint8_t>(pass_.band);
  stepLetters(pass_, w.steps, sizeof w.steps);
  w.wakes = pass_.wakes;
  w.awake_s = static_cast<std::uint32_t>(clock_.nowMs() / 1000u);
  w.next_interval_s = pass_.decision.interval_seconds;
  w.faults_in_a_row = duty_.wake().faults_in_a_row;
  w.on_trial = pass_.on_trial;
  w.update_running = update_ != nullptr && update_->active();

  interfaces::WallTime now{};
  known_.time_known = wall_.now(&now);
  known_.epoch = known_.time_known ? now.epoch : 0;
  known_.battery_known = pass_.decision.reading_known;
  known_.battery_mv = pass_.decision.median_mv;
  known_.battery_ma = pass_.decision.current_known ? pass_.decision.mean_ma : 0;
  known_.charging = pass_.decision.charging;
}

void WakeCycle::fillFirmware() {
  interfaces::BackendFirmware& us = known_.walter;
  us.known = true;
  copyInto(us.version, sizeof us.version, core::version());
  const image::Fields self = image::this_image();
  us.descriptor_known = self.version[0] != 0;
  copyInto(us.descriptor_version, sizeof us.descriptor_version, self.version);
  copyInto(us.board, sizeof us.board, self.board);
  us.role = static_cast<std::uint8_t>(self.role);
  us.link_major = self.link_major;
  us.link_minor = self.link_minor;

  interfaces::BackendFirmware& them = known_.radio;
  const link::client::Session& session = radio_.session();
  them.known = session.up;
  copyInto(them.version, sizeof them.version, session.fw_version);
  copyInto(them.board, sizeof them.board, session.board);
  them.role = session.role;
  them.link_major = session.proto_major;
  them.link_minor = session.proto_minor;
  // The radio's descriptor is the one the store kept of the image it was flashed from, which is
  // what the update's health check holds a HELLO against (spec/FIRMWARE-MANIFEST.md §4).
  if (store_ != nullptr) {
    image::Record golden{};
    if (store_->read(image::Which::kGolden, radio_.slot(), &golden) == image::StoreError::kNone &&
        golden.present) {
      them.descriptor_known = true;
      copyInto(them.descriptor_version, sizeof them.descriptor_version, golden.descriptor.version);
      for (std::size_t i = 0; i < interfaces::kBackendHashShown; ++i) {
        them.hash[i] = golden.sha256[i];
      }
    }
  }

  // The unit's identity, as the radio gave it (§5.6, S-1: the repeater's key is the unit's).
  // Asked once a boot, and only on a wake that has an uplink to tell anybody over.
  if (!known_.identity.known && session.up) {
    const link::client::Answer<link::IdentityResponse> a = radio_.identity();
    if (a.ok() && a.message.pubkey.size == interfaces::kBackendPubkeySize) {
      known_.identity.known = true;
      for (std::size_t i = 0; i < interfaces::kBackendPubkeySize; ++i) {
        known_.identity.pubkey[i] = a.message.pubkey.data[i];
      }
      copyInto(known_.identity.name, sizeof known_.identity.name, a.message.name);
    }
  }
}

void WakeCycle::fillStore() {
  known_.store_there = store_ != nullptr;
  if (store_ == nullptr) return;
  const image::Which which[3] = {image::Which::kGolden, image::Which::kCandidate,
                                 image::Which::kScanner};
  interfaces::BackendImage* into[3] = {&known_.golden, &known_.candidate, &known_.scanner};
  for (std::size_t i = 0; i < 3; ++i) {
    image::Record record{};
    *into[i] = interfaces::BackendImage{};
    if (store_->read(which[i], radio_.slot(), &record) != image::StoreError::kNone) continue;
    interfaces::BackendImage& out = *into[i];
    out.present = record.present;
    if (record.present) {
      copyInto(out.version, sizeof out.version, record.version);
      for (std::size_t k = 0; k < interfaces::kBackendHashShown; ++k) out.hash[k] = record.sha256[k];
      out.size = record.size;
      out.stored_at = record.stored_at;
    } else if (record.transfer) {
      out.transfer = true;
      out.transferred = record.transferred;
      out.transfer_size = record.transfer_size;
    }
  }
}

void WakeCycle::fillSupervisor() {
  known_.incidents = 0;
  known_.supervisor_stopped = false;
  if (supervisor_ == nullptr) return;
  known_.supervisor_stopped = supervisor_->status().stopped;
  const std::size_t n = supervisor_->incidentCount() < interfaces::kBackendIncidents
                            ? supervisor_->incidentCount()
                            : interfaces::kBackendIncidents;
  for (std::size_t i = 0; i < n; ++i) {
    const supervisor::Incident& in = supervisor_->incidentAt(i);
    interfaces::BackendIncident& out = known_.incident[i];
    out.present = in.present;
    out.cause = static_cast<std::uint8_t>(in.cause);
    out.outcome = static_cast<std::uint8_t>(in.outcome);
    out.rungs_tried = in.rungs_tried;
    out.stop = static_cast<std::uint8_t>(in.stop);
    out.began_epoch = in.began_epoch;
    out.lasted_ms = in.lasted_ms;
    out.battery_mv = in.battery_mv;
  }
  known_.incidents = static_cast<std::uint32_t>(n);
}

}  // namespace cycle
}  // namespace core
