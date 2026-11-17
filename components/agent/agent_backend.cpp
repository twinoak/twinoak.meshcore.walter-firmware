#include "walter_agent/agent_backend.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <agent/Log.h>
#include <agent/TextOut.h>

#include "core/cycle/wake_cycle.h"
#include "core/link/client/words.h"
#include "core/power/interval_policy.h"

namespace agent {

namespace {

std::uint32_t saturate(std::uint32_t value) { return value == 0xFFFFFFFFu ? value : value + 1; }
std::uint16_t saturate16(std::uint16_t value) { return value == 0xFFFFu ? value : static_cast<std::uint16_t>(value + 1); }

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The library's TextOut over this firmware's: what `jobs.report()` prints goes to the console.
class OutAdapter final : public mm::TextOut {
 public:
  explicit OutAdapter(interfaces::TextOut& out) : out_(out) {}
  void write(const char* text, std::size_t len) override { out_.write(text, len); }

 private:
  interfaces::TextOut& out_;
};

void say(interfaces::TextOut* out, const char* text) {
  if (out != nullptr) out->write(text, std::strlen(text));
}

void sayf(interfaces::TextOut* out, const char* format, ...) __attribute__((format(printf, 2, 3)));
void sayf(interfaces::TextOut* out, const char* format, ...) {
  if (out == nullptr) return;
  char line[200];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(line, sizeof line, format, args);
  va_end(args);
  if (n > 0) out->write(line, static_cast<std::size_t>(n) < sizeof line ? static_cast<std::size_t>(n) : sizeof line - 1);
}

// A key as a line about a job shows it: the first six bytes as twelve lowercase hex characters
// (task p9-04, `agent::kKeyShownBytes`).
void shortKey(const std::uint8_t* key, char* out) {
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < kKeyShownBytes; ++i) {
    out[i * 2] = digits[key[i] >> 4];
    out[i * 2 + 1] = digits[key[i] & 0x0F];
  }
  out[2 * kKeyShownBytes] = 0;
}

// The one backend whose pump the library was given (task p15-06). One on a board and one in a
// test at a time; a backend begun later takes it, which is what a test with two lives of the
// Walter over one world wants - the same arrangement `WalterPlatform`'s log sink uses.
AgentBackend* g_pump_owner = nullptr;

}  // namespace

AgentBackend::AgentBackend(const Parts& parts, interfaces::Clock& clock, interfaces::WallClock& wall)
    : p_(parts), clock_(clock), wall_(wall) {}

AgentBackend::~AgentBackend() {
  if (g_pump_owner == this) g_pump_owner = nullptr;
}

// **Where the wake is, at every wait the library takes.** `mm::JobRunner::openJob()` is the job
// `claimAndRun` has in hand - the library sets it before it dispatches and clears it after, so it
// covers a mesh job, a local-door job and a firmware job of this board's own extension alike -
// and `LinkMeshCounters::requests` is every §6 request that went out. Neither is a change to the
// copied library: both are what it already offers.
void AgentBackend::noteProgress() {
  if (p_.duty == nullptr) return;
  core::cycle::WakeProgress& progress = p_.duty->progress();
  const mm::ClaimedJob* job = p_.jobs != nullptr ? p_.jobs->openJob() : nullptr;
  if (job == nullptr) {
    progress_job_ = 0;
    progress.jobEnded();
    return;
  }
  const std::uint32_t made = p_.mesh != nullptr ? p_.mesh->linkCounters().requests : 0;
  const std::uint32_t id = static_cast<std::uint32_t>(static_cast<unsigned long long>(job->id));
  if (id != progress_job_) {
    progress_job_ = id;
    progress_requests_at_ = made;
  }
  progress.jobIsRunning(job->type, made >= progress_requests_at_ ? made - progress_requests_at_ : 0);
}

void AgentBackend::pumpAndNote() {
  AgentBackend* owner = g_pump_owner;
  if (owner == nullptr) return;
  owner->noteProgress();
  if (owner->pump_ != nullptr) owner->pump_();
}

void AgentBackend::begin(void (*pump)()) {
  pump_ = pump;
  g_pump_owner = this;
  pump = &AgentBackend::pumpAndNote;
  if (p_.server != nullptr && p_.duty != nullptr) {
    p_.server->writeProgressTo(&p_.duty->progress());
  }
  if (p_.mesh != nullptr && p_.duty != nullptr) {
    p_.mesh->writeProgressTo(&p_.duty->progress());
  }
  if (p_.jobs == nullptr || p_.loop == nullptr || p_.mesh == nullptr || p_.server == nullptr ||
      p_.platform == nullptr || p_.identity == nullptr) {
    return;
  }
  // docs/library.md §6: the runner, then the loop over it, then the identity - and the extension
  // and the store before the first wake.
  p_.platform->pumpWith(pump);
  p_.jobs->begin(*p_.mesh, *p_.server, *p_.platform, pump);
  p_.loop->begin(*p_.server, *p_.platform, *p_.mesh, *p_.jobs, p_.identity->heartbeatIdentity());
  if (p_.firmware != nullptr) {
    p_.jobs->setExtension(p_.firmware);
    p_.server->setResultGate(p_.firmware);
  }
  p_.server->setClock(&p_.platform->clock());
  // The last eight failed calls, read back from the store and handed to the uplink (task p13-02):
  // `ModemUplink::call()` is the one place every call to the server passes, so it is the one
  // place a failure is written down.
  if (p_.calls != nullptr) {
    p_.calls->begin();
    p_.server->recordFailuresIn(p_.calls);
  }
  // And the three counts of a flooded transaction, the same way (task p14-02): read back from the
  // store here, and handed to the uplink - which is the one seam the heartbeat's bytes pass
  // through, and therefore the one place the kept totals can replace the library's own counts
  // since boot. Handed over **by pointer**, so that every heartbeat carries the totals as they
  // stand when it is built.
  if (p_.floods != nullptr) {
    p_.floods->begin();
    p_.server->countFloodsFrom(&p_.floods->totals());
  }
  refreshIdentity();
  if (p_.kept != nullptr && p_.kept_scratch != nullptr) {
    p_.loop->setRetained(p_.kept, p_.kept_scratch, p_.kept_scratch_len);
  }
  state_survived_ = loadState();
  if (p_.kept != nullptr && p_.loop->restoreKept()) counters_.kept_restored = saturate(counters_.kept_restored);
  begun_ = true;
}

void AgentBackend::refreshIdentity() {
  (void)p_.identity->refresh();
  handOverIdentity();
}

// Before a wake claims anything: the radio known (task p8-04). `Identity::open()` is one HELLO
// when there is no session and then the §5.6 read, so a timer wake - whose step 2 opened the
// session - asks nothing extra here, and the console's `agent wake`, which runs no step 2, gets
// its session here instead of needing a `rak hello` typed first. What follows from the key is
// the whole point: the local door is armed (`setOwnKey`), and `LinkMesh::supports()` answers for
// every mesh type, which is the list the heartbeat carries and the only thing that decides what
// a claim may hand over.
bool AgentBackend::knowTheRadio() {
  const bool known = p_.identity->open();
  handOverIdentity();
  return known;
}

void AgentBackend::handOverIdentity() {
  p_.loop->setIdentity(p_.identity->publicKeyHex());
  p_.mesh->setOwnKey(p_.identity->publicKey());
  if (p_.observations != nullptr) p_.observations->useOwnKey(p_.identity->publicKey());
  // §6.5 (link 1.3): the radio's default flood scope, read once a session, **here** - where the
  // unit's own key is read and therefore before the heartbeat is built, which is where the scope
  // and its three counts go. One FLOOD_SCOPE when the session can scope a flood and the name is
  // not known yet, and not a frame otherwise; a radio of 1.2 is untouched.
  (void)p_.mesh->askFloodScope();
}

// **One line says which of the two this wake is, and why** (task p8-04). It goes to the library's
// log, which on a board is this firmware's own console port and the four kilobytes `dump` prints
// (main/main.cpp), so a timer wake records it and `agent wake` prints it where a person is
// looking. The key itself is never a line's argument (spec/SYSTEM.md S-12): the boot id is what
// identifies the session.
void AgentBackend::sayRadio(bool radio_known, bool radio_up) const {
  if (radio_known) {
    LOG_I("agent: the radio is known (boot 0x%08lx): a job for this unit's own repeater goes "
          "through the local door, and every job type this unit serves is offered",
          static_cast<unsigned long>(p_.identity->bootId()));
    return;
  }
  // What is left to offer: the extension's own types, which need no key - a firmware job is about
  // this unit's store and its two MCUs, and a power cycle is exactly for a radio that is silent.
  char types[128];
  std::size_t at = 0;
  const int count = p_.firmware != nullptr ? p_.firmware->typeCount() : 0;
  for (int i = 0; i < count && at + 1 < sizeof types; ++i) {
    const int n = std::snprintf(types + at, sizeof types - at, "%s%s", at == 0 ? "" : ", ",
                                p_.firmware->type(i));
    if (n <= 0) break;
    at += static_cast<std::size_t>(n);
    if (at >= sizeof types) {
      at = sizeof types - 1;
      break;
    }
  }
  types[at] = 0;
  // **Two lines and not one since task p13-01**, when the fifth job type (`twinoak.report`) took
  // the one line past the library's own 239 characters (agent/Log.cpp) and cut it off exactly
  // where it said why. The reason is one line and the types are the next, so a sixth type costs
  // the list and nothing else.
  // **Three reasons and not two** (task p10-03): a radio that answered but refuses §5.6 is not a
  // radio awaiting its first install, and until this line both read as *has made no identity
  // yet*.
  const char* why = "the radio did not answer a HELLO";
  if (radio_up) {
    // Not a word longer than the reason it replaces: the library's line is 239 characters and
    // the four job types are in it, so a reason that grew by a clause would cut off *is not
    // claimed* - which the first run of this line's own test did. `agent id` is where the
    // evidence and the way out are written out.
    why = p_.identity->identityLost() ? "the radio refuses: its identity is lost"
                                      : "the radio answered but has made no identity yet";
  }
  LOG_I("agent: %s: a mesh job this end cannot tell is its own is not claimed", why);
  LOG_I("agent: only this unit's own job types are claimed: %s",
        at == 0 ? "none in this build" : types);
}

// -- the state across a sleep ----------------------------------------------------------------------

bool AgentBackend::saveState() {
  if (p_.state_store == nullptr || !p_.state_store->ready()) return false;
  const mm::WakeState s = p_.loop->state();
  // **The radio's reading belongs to the state it was taken after** (task p14-06). What a wait
  // still owes is counted from *this* moment, so a reading taken before it would measure from
  // the wrong one - and measuring from too early a moment believes more time passed than really
  // did, which is the one thing the figure may never do. The reading written by the sleep that
  // follows is the next one; until then there is none.
  //
  // Here and not after the `strcmp` below: a state that is what the store already holds is not
  // written again, and it is still a state taken now.
  if (p_.duty != nullptr) p_.duty->reason().forgetRadioReading();
  const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&s);
  const char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof s; ++i) {
    state_hex_[2 * i] = digits[bytes[i] >> 4];
    state_hex_[2 * i + 1] = digits[bytes[i] & 0x0F];
  }
  state_hex_[2 * sizeof s] = 0;
  // A state that is what the store holds is not written again: NVS wear is the one cost here.
  if (std::strcmp(state_hex_, last_hex_) == 0) return true;
  if (!p_.state_store->write(kStateKey, state_hex_)) return false;
  std::memcpy(last_hex_, state_hex_, sizeof last_hex_);
  counters_.state_saved = saturate(counters_.state_saved);
  return true;
}

bool AgentBackend::loadState() {
  if (p_.state_store == nullptr || !p_.state_store->ready()) return false;
  if (!p_.state_store->read(kStateKey, state_hex_, sizeof state_hex_)) return false;
  if (std::strlen(state_hex_) != 2 * sizeof(mm::WakeState)) {
    counters_.state_refused = saturate(counters_.state_refused);
    return false;
  }
  mm::WakeState s;
  std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(&s);
  for (std::size_t i = 0; i < sizeof s; ++i) {
    const int hi = hexValue(state_hex_[2 * i]);
    const int lo = hexValue(state_hex_[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      counters_.state_refused = saturate(counters_.state_refused);
      return false;
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  // **How long the board was away**, which the library cannot know and this firmware can only
  // measure one way (task p10-01; the exit-node firmware's p1.10-42). `core/cycle/wake_reason.h`
  // is where the rule and the figure are: a timer wake has slept what the last sleep asked its
  // timer for, less the timer's own error, and every other wake passes 0. Without a duty cycle -
  // a test that has none - it is 0, which is the behaviour this call had before it was measured.
  //
  // Until this, a 30 s hold set by one failed call was put back whole at every wake and spent
  // five seconds at a time: the unit of 8 November 2026 woke 56 times in a night and
  // meshmaster.dk heard from it on about every sixth.
  const std::uint32_t away_ms = p_.duty != nullptr ? p_.duty->reason().timeAwayMs() : 0;
  // **And the one boot that owes no wait at all** (task p14-05).
  //
  // The wait is real time and the time away is measured, and for every ordinary boot that is the
  // whole rule. Two boots are not ordinary, and both of them follow a *restart*, which is a boot
  // that was away seconds and can prove none of them:
  //
  //   a boot on trial            it exists to answer one question - can this image reach the
  //                              server - and a wait an older image's failed call left behind
  //                              must not answer it *no* before the question has been asked. On
  //                              15 November 2026 it did: the wake that took v0.1.6's apply at
  //                              08:41:07 UTC left a hold, v0.1.6 booted owing all of it, sent
  //                              nothing, and five minutes later a good image was on the bad
  //                              list (meshmaster.dk job 30474).
  //   the old image, owing a     the rolled-back v0.1.4 of that same morning owed the server the
  //   rollback's result          apply's result and owed the same hold, so it too sent nothing;
  //                              the timer wake eleven minutes later is what finally told
  //                              meshmaster.dk what had happened. A result the server is waiting
  //                              for goes in the first wake that can.
  //
  // **The line is drawn here and nowhere else**: the wait is dropped from the state before it is
  // handed over, and the time away is handed over as measured. The library is told nothing untrue
  // - it is told there was no wait, which from this boot's point of view is the decision this
  // firmware has taken - and everything else of the state (the backoff, the run of failures, the
  // halt, the counters, the accepted interval) is restored exactly as it was saved.
  laid_aside_ = LaidAside::kNone;
  // `bootIsTrial()` and not `onTrial()`: main/main.cpp restores this state (`backend.begin()`)
  // before the console's `begin()` reaches `SelfUpdate::begin()`, so the machine has not decided
  // yet - but the bootloader and the journal have, and that is what both read.
  const bool on_trial = p_.self != nullptr && p_.self->bootIsTrial();
  const bool owes_result = !on_trial && p_.firmware != nullptr && p_.firmware->owedResultDue();
  const std::uint32_t laid_aside_ms = s.holdForMs > 0 && (on_trial || owes_result) ? s.holdForMs : 0;
  if (laid_aside_ms > 0) {
    laid_aside_ = on_trial ? LaidAside::kOnTrial : LaidAside::kResultOwed;
    s.holdForMs = 0;
  }
  // The library refuses a state of another version and changes nothing.
  if (!p_.loop->restore(s, away_ms)) {
    counters_.state_refused = saturate(counters_.state_refused);
    return false;
  }
  away_ms_ = away_ms;
  // What the store really carried across, whether it was put back or laid aside: the two figures
  // together are the whole of what happened to the wait at this boot.
  owed_before_ms_ = s.holdForMs + laid_aside_ms;
  std::memcpy(last_hex_, state_hex_, sizeof last_hex_);
  counters_.state_restored = saturate(counters_.state_restored);
  return true;
}

// -- the three counts of a flooded transaction, kept (task p14-02) -----------------------------
//
// The library counts one of the three per flooded transaction, **since boot**
// (`JobRunner::floodCounts()`, agent/FloodScope.h), it cannot be seeded, and `components/agent-core`
// is a hash-checked copy that is not changed here. A board that boots at every wake would
// therefore tell meshmaster.dk 0 / 0 / 0 with a 1 now and then, which answers nothing about
// whether the unscoped try still earns its airtime - so the totals are this board's, beside the
// library's, and this is where a wake's counts are added to them.
//
// `take()` adds the **difference** against what it has already taken in this boot, so calling it
// twice in a wake moves the totals once; `save()` writes the value only when a total moved. What
// crosses a boot is the store alone: a wake that dies between a transaction and here leaves the
// store as it was before that wake - its own counts are lost, at most one wake's - and the next
// boot starts with the library's counter at 0, so nothing is ever added twice.
void AgentBackend::takeFloodCounts() {
  if (p_.floods == nullptr || p_.jobs == nullptr) return;
  p_.floods->take(p_.jobs->floodCounts());
  (void)p_.floods->save();
}

// A wait a person's wake does not owe (task p14-06). The library's own state is the one place a
// hold lives, so the way to lay one aside mid-boot is the way it was laid aside at the boot: read
// the state, drop the hold, hand it straight back with no time away - which re-bases every
// duration against the clock it finds now, and that clock is now.
void AgentBackend::layAsideForPerson() {
  if (p_.loop == nullptr) return;
  mm::WakeState s = p_.loop->state();
  // Nothing owed, or already laid aside by a trial or an owed result: there is nothing to do and
  // nothing to say, and the reason already recorded is the one a person should read.
  if (s.holdForMs == 0) return;
  const std::uint32_t laid = s.holdForMs;
  s.holdForMs = 0;
  if (!p_.loop->restore(s, 0)) return;
  laid_aside_ = LaidAside::kPersonAsked;
  LOG_I("wake: a person asked for this Walter, so the %lu s of a wait still owed is laid aside",
        static_cast<unsigned long>(laid / 1000u));
}

// -- the exchange ------------------------------------------------------------------------------------

interfaces::Exchanged AgentBackend::exchange(const interfaces::BackendKnowledge& known, std::uint64_t deadline_ms,
                                             interfaces::BackendAnswer* out) {
  return run(known, deadline_ms, out, nullptr);
}

interfaces::Exchanged AgentBackend::run(const interfaces::BackendKnowledge& known, std::uint64_t deadline_ms,
                                        interfaces::BackendAnswer* out, interfaces::TextOut* talk) {
  counters_.exchanges = saturate(counters_.exchanges);
  if (!begun_) {
    say(talk, "agent: the loop has not begun\n");
    return interfaces::Exchanged::kFailed;
  }
  const core::power::Band band = static_cast<core::power::Band>(known.wake.band);
  // **A wake a person asked for owes no wait** (task p14-06; §8's attention, reason 4).
  //
  // On 13 November 2026 Thomas pressed *Wake it up* on meshmaster.dk (job 28399); the unit's
  // radio answered `ok` at 18:09:02 UTC and raised the attention; the board woke on the line -
  // and sent nothing, because the wake before it had ended in a failed call and the wait that
  // leaves was owed whole. Nothing came from the unit until its timer woke it.
  //
  // Here and not in `loadState()`, where task p14-05's two lines are drawn, because the reason
  // reaches this board two ways and only one of them is known at the boot: the event that woke
  // it (step 1), **or** a reason still pending in step 4's STATUS, which a timer wake finds (§8:
  // "a pending attention stays visible in STATUS until the Walter has made a request"). Both are
  // in `known.wake.attention` by the time the exchange is run, so both are answered by one line.
  //
  // What is laid aside is what the wait still owed at this moment - after the time away has come
  // off it - and nothing else of the state moves: the backoff, the run of failures, the halt on a
  // 401, the counters and the accepted interval are handed straight back. **This board adds no
  // limit of its own**: §8's is the radio's, one new attention per reason an hour, and a second
  // bound here would only make a person press the button twice.
  if (known.wake.attention == core::link::attention::kPersonAsked) layAsideForPerson();

  // Before the wake: who this unit is - a session opened if there is none, and the identity read
  // (a radio that got one between two wakes reports it now) - the uplink's state and deadline,
  // what the jobs may do, and §7's ring drained into the loop's own so the batch goes with this
  // wake.
  // The wake's number the failures of this wake are stamped with (task p13-02): the duty cycle's
  // own count, which `wake`'s list and a journal entry are read against.
  if (p_.calls != nullptr) p_.calls->atWake(known.wake.wakes);
  const bool radio_known = knowTheRadio();
  const bool radio_up = p_.identity->sessionUp();
  sayRadio(radio_known, radio_up);
  // What the heartbeat of this wake says about a radio that **refuses** (task p10-03): the state
  // the read above found, handed to the one seam the body's bytes pass through. A radio that lost
  // its identity reads on meshmaster.dk exactly like one awaiting its first install without it.
  p_.server->setRadioIdentityLost(p_.identity->identityLost());
  p_.server->setLinkUp(known.uplink.up);
  p_.server->setDeadline(deadline_ms);
  if (p_.firmware != nullptr) {
    WakeContext context;
    context.uplink_up = known.uplink.up;
    context.band = band;
    context.may = core::power::allowanceFor(band);
    context.on_trial = known.wake.on_trial;
    context.update_running = known.wake.update_running;
    // Nothing about the radio's health crosses here (task p9-03). What the HELLO above settled is
    // the *other* fact - whether there is a key to tell a job for this unit's own repeater by -
    // and `sayRadio()` has just reported it. *The link is known down* is a classification of the
    // supervisor's, which the jobs read off the supervisor itself
    // (walter_agent/firmware_jobs.h, `radioHealth()`): p8-04 made this line say it whenever there
    // was no frame session, and a scanner slot and a slot awaiting its first install have none
    // either.
    context.battery_known = known.battery_known;
    context.battery_mv = known.battery_mv;
    context.deadline_ms = deadline_ms;
    context.download_bytes = download_bytes_;
    p_.firmware->beginWake(context);
  }
  // §7's drain needs a session, and the one above is the freshest word on whether there is one
  // (`known.radio.known` is what step 2 found, a step earlier).
  if (p_.observations != nullptr && radio_up) {
    counters_.observations_drained += static_cast<std::uint32_t>(p_.observations->drain(p_.platform->millis()));
  }

  // The intent: the policy's own interval and the band's cap (task p4-04's table). The budget:
  // what is left to the deadline the wake cycle gave, and the bytes one wake may post.
  const mm::WakeIntent intent = intentFor(band, known.wake.next_interval_s);
  mm::Budget budget;
  const std::uint64_t now = clock_.nowMs();
  const std::uint32_t left_s = deadline_ms > now ? static_cast<std::uint32_t>((deadline_ms - now) / 1000u) : 0;
  budget.wakeSeconds = left_s;
  budget.uplinkSeconds = left_s;
  budget.postBytes = kWakePostBytes;

  report_ = p_.loop->wake(intent, budget);
  const bool ran = report_.why[0] == 0;
  if (ran) {
    counters_.wakes = saturate(counters_.wakes);
  } else {
    counters_.not_run = saturate(counters_.not_run);
  }
  if (report_.heartbeat == mm::StepOutcome::Done) counters_.heartbeats_ok = saturate(counters_.heartbeats_ok);
  if (report_.heartbeat == mm::StepOutcome::Failed) counters_.heartbeats_failed = saturate(counters_.heartbeats_failed);
  counters_.jobs_run += static_cast<std::uint32_t>(report_.jobsRun);
  counters_.jobs_not_run += static_cast<std::uint32_t>(report_.jobsNotRun);
  counters_.results_kept += static_cast<std::uint32_t>(report_.resultsKept);
  counters_.batches += static_cast<std::uint32_t>(report_.batchesPosted);
  counters_.observations_posted += static_cast<std::uint32_t>(report_.observationsPosted);
  if (report_.keptImageWritten) counters_.kept_written = saturate(counters_.kept_written);

  // After the wake: the heartbeat is the trial's third condition (S-6). A trial that only waited
  // for it is stepped to its end here, so that the result the Walter's own apply owes can go in
  // this same wake rather than the next; and whatever that result is, it is posted from the
  // journal when the trial has ended.
  bool posted_owed = false;
  // A heartbeat that went out and did not come back is news for the trial too (task p14-05):
  // *the server was not reached* is a finding, and the one `check()` must be able to tell from
  // *nothing was ever sent*.
  if (report_.heartbeat == mm::StepOutcome::Failed && p_.trial != nullptr) p_.trial->heartbeatFailed();
  if (report_.heartbeat == mm::StepOutcome::Done) {
    if (p_.trial != nullptr) p_.trial->heartbeatOk();
    if (p_.self != nullptr && p_.self->onTrial()) {
      for (int i = 0; i < kTrialStepsAfterHeartbeat && p_.self->onTrial(); ++i) {
        const core::update::SelfStep s = p_.self->step();
        if (s.changed) break;
        p_.platform->delayMs(core::update::kStepIdleMs);
      }
      if (!p_.self->onTrial()) counters_.trials_confirmed = saturate(counters_.trials_confirmed);
    }
    if (p_.firmware != nullptr) posted_owed = p_.firmware->postOwedResult();
  }
  (void)saveState();
  // ... the three counts of what flooded in this wake, added to the kept totals (task p14-02),
  // and the failed calls of this wake, **only when one failed** (task p13-02): a wake in which
  // nothing failed writes nothing here, which is the fourth deliverable of that task.
  takeFloodCounts();
  if (p_.calls != nullptr) (void)p_.calls->save();

  // The answer: what the jobs did, and how long to sleep at least.
  if (out != nullptr) {
    if (p_.firmware != nullptr) {
      const Did& did = p_.firmware->did();
      out->did = did.asks;
      out->carried = did.carried;
      out->outside_ms = did.outside_ms;
    }
    std::uint32_t at_least = report_.sleepAtLeastS;
    if (report_.heartbeat == mm::StepOutcome::Done && report_.acceptedIntervalS > 0 &&
        static_cast<std::uint32_t>(report_.acceptedIntervalS) > known.wake.next_interval_s) {
      // The server's accepted interval, only when it is longer: the battery wins.
      at_least = static_cast<std::uint32_t>(report_.acceptedIntervalS) > at_least
                     ? static_cast<std::uint32_t>(report_.acceptedIntervalS)
                     : at_least;
      counters_.longer_sleeps = saturate(counters_.longer_sleeps);
    }
    if (report_.sleepAtLeastS > 0) counters_.holds = saturate(counters_.holds);
    out->sleep_at_least_s = at_least;
    // **Whether anybody was reached, and when nobody was, why nothing was sent** (task p10-02).
    // The seam's three values could not tell *answered, nothing to do* from *never opened its
    // mouth*, so the journal said "the backend was reached" about the wakes of 8 November 2026
    // that sent nothing. `interfaces::Reached` is the second half of the answer.
    out->reached = reachedThisWake(known);
    out->jobs_ran = report_.jobsRun < 0 ? 0
                    : report_.jobsRun > 0xFF ? 0xFF
                                             : static_cast<std::uint8_t>(report_.jobsRun);
  }

  if (talk != nullptr) printReport(*talk);

  // The exchange is over, so no job is being run in it (task p15-06): the pump writes the job
  // down at every wait the library takes, and this is the edge where the last one ends. Without
  // it a wake that was ended after its exchange - at step 9 or step 10 - would read as a wake
  // still in the last job the runner had open.
  noteProgress();

  // How it went, in the seam's three words. A heartbeat the server did not answer is the
  // exchange failing (news for the journal); a wake that did nothing because it had nobody to
  // talk to - not configured, halted on a 401, a wait still running - is *nothing to do*, which
  // is the null backend's answer and is not news every wake. **Which of the two kinds of
  // *nothing* it was is `out->reached`**, written above.
  if (!ran) return interfaces::Exchanged::kNothing;
  if (report_.heartbeat != mm::StepOutcome::Done) return interfaces::Exchanged::kFailed;
  const bool did_something = report_.jobsRun > 0 || report_.jobsNotRun > 0 || report_.resultsFlushed > 0 ||
                             report_.batchesPosted > 0 || posted_owed ||
                             (p_.firmware != nullptr && p_.firmware->did().asks != 0);
  return did_something ? interfaces::Exchanged::kAsked : interfaces::Exchanged::kNothing;
}

// -- one heartbeat more, for a trial (task p14-05) ---------------------------------------------
//
// The seam's `beat()` (interfaces/backend.h): the wake cycle's trial loop asks for this while
// the trial's third condition is open, and for nothing else, ever. What it runs is **the
// library's own wake again** - the only thing in this firmware that posts a heartbeat - with a
// budget that fits a heartbeat and nothing else (kTrialBeatTransactions, kTrialBeatPostBytes).
//
// *When* is the library's: a wake inside a wait of its own answers at once, sends nothing, and
// comes back `kWaiting`, so the trial asking every tenth of a second costs a comparison. The
// first heartbeat after a failure therefore goes out when the backoff is over and not before,
// which is what *the library's own wait has run out* means.
//
// `report_` is not touched: it is step 8's wake, which is what `agent wake` and `agent state`
// print and what the wake's record was written from. This is a heartbeat the trial asked for,
// and `counters_.trial_beats` is where it is counted.
interfaces::Beat AgentBackend::beat(std::uint64_t deadline_ms) {
  if (!begun_ || p_.loop == nullptr) return interfaces::Beat::kNone;
  const std::uint64_t now = clock_.nowMs();
  if (now >= deadline_ms) return interfaces::Beat::kWaiting;
  if (p_.server != nullptr) p_.server->setDeadline(deadline_ms);

  // The interval this unit would have proposed anyway, so that a heartbeat the trial asked for
  // says of itself exactly what the wake's own said: the duty cycle's decision when it has one.
  core::power::Band band = core::power::Band::kUnknown;
  std::uint32_t interval_s = 0;
  if (p_.duty != nullptr) {
    band = p_.duty->decided() ? p_.duty->decision().band : p_.duty->reason().band();
    interval_s = p_.duty->decided() ? p_.duty->decision().interval_seconds
                                    : core::power::minutesFor(band) * 60u;
  }
  mm::WakeIntent intent = intentFor(band, interval_s);
  intent.maxTransactionsThisWake = kTrialBeatTransactions;
  mm::Budget budget;
  const std::uint32_t left_s = static_cast<std::uint32_t>((deadline_ms - now) / 1000u);
  budget.wakeSeconds = left_s;
  budget.uplinkSeconds = left_s;
  budget.postBytes = kTrialBeatPostBytes;

  const mm::WakeReport report = p_.loop->wake(intent, budget);
  if (report.why[0] != 0) {
    // The library sent nothing and said why in its own words - a wait, a halt, no uplink, not
    // configured. None of them is this trial's business to work around: it asks again next time.
    counters_.trial_beats_held = saturate16(counters_.trial_beats_held);
    return interfaces::Beat::kWaiting;
  }
  if (report.heartbeat == mm::StepOutcome::Done) {
    counters_.trial_beats = saturate16(counters_.trial_beats);
    counters_.heartbeats_ok = saturate(counters_.heartbeats_ok);
    if (p_.trial != nullptr) p_.trial->heartbeatOk();
    (void)saveState();
    takeFloodCounts();
    if (p_.calls != nullptr) (void)p_.calls->save();
    return interfaces::Beat::kAnswered;
  }
  if (report.heartbeat != mm::StepOutcome::Failed) {
    // The wake ran and did not reach the heartbeat - no seconds left of what this call was given.
    // Nothing went out, so the trial is told nothing.
    counters_.trial_beats_held = saturate16(counters_.trial_beats_held);
    return interfaces::Beat::kWaiting;
  }
  counters_.trial_beats = saturate16(counters_.trial_beats);
  counters_.heartbeats_failed = saturate(counters_.heartbeats_failed);
  if (p_.trial != nullptr) p_.trial->heartbeatFailed();
  (void)saveState();
  takeFloodCounts();
  if (p_.calls != nullptr) (void)p_.calls->save();
  return interfaces::Beat::kFailed;
}

// Why this wake sent nothing, or that it was answered (task p10-02).
//
// **The library decides whether anything was sent and this only names it.** `report_.why` is one
// of `mm::AgentLoop::wake`'s five static sentences and it is empty when the wake ran its steps,
// so *whether* is read off the library and never guessed at; what cannot be read off it is a
// code, because `why` is prose. So the naming walks the library's own chain, in the library's own
// order (`src/AgentLoop.cpp`: halted, then not configured, then the uplink, then the hold), over
// the same state the library read - and a host test walks every branch of it through the real
// library, so that an upstream sync which changed that order would fail here rather than mislabel
// a wake (host_test/agent/test_what_a_wake_did.cpp).
interfaces::Reached AgentBackend::reachedThisWake(const interfaces::BackendKnowledge& known) const {
  if (report_.why[0] == 0) {
    // It ran its steps. **Answered means the heartbeat came back** and nothing less: a heartbeat
    // that failed is the exchange failing, and the journal's *reached* is a claim about a server.
    return report_.heartbeat == mm::StepOutcome::Done ? interfaces::Reached::kAnswered
                                                      : interfaces::Reached::kUnsaid;
  }
  if (report_.halted) return interfaces::Reached::kHalted;
  if (p_.server == nullptr || !p_.server->configured()) return interfaces::Reached::kNotConfigured;
  if (!known.uplink.up) return interfaces::Reached::kNoUplink;
  if (report_.sleepAtLeastS > 0) return interfaces::Reached::kWait;
  // The loop said it sent nothing and none of the four fits: *the loop has not begun*, or a
  // sentence a later library added. Not labelled as one of ours.
  return interfaces::Reached::kElse;
}

// -- the console -------------------------------------------------------------------------------------

void AgentBackend::printReport(interfaces::TextOut& out) const {
  const mm::WakeReport& r = report_;
  if (r.why[0] != 0) {
    sayf(&out, "agent wake: nothing was sent: %s\n", r.why);
  }
  sayf(&out, "agent wake: config %s, heartbeat %s, results %s, claim %s, jobs %s, observations %s\n",
       mm::stepOutcomeName(r.config), mm::stepOutcomeName(r.heartbeat), mm::stepOutcomeName(r.results),
       mm::stepOutcomeName(r.claim), mm::stepOutcomeName(r.jobs), mm::stepOutcomeName(r.observations));
  sayf(&out, "  %d job(s) claimed, %d run, %d not run; %d result(s) flushed, %d kept; %d batch(es), %d advert(s) posted, %d left\n",
       r.jobsClaimed, r.jobsRun, r.jobsNotRun, r.resultsFlushed, r.resultsKept, r.batchesPosted,
       r.observationsPosted, r.observationsLeft);
  sayf(&out, "  %d transaction(s), %lu byte(s) posted, %lu ms at the server of %lu; accepted interval %d s, sleep at least %lu s%s%s\n",
       r.transactionsSpent, static_cast<unsigned long>(r.bytesPosted), static_cast<unsigned long>(r.uplinkMs),
       static_cast<unsigned long>(r.elapsedMs), r.acceptedIntervalS, static_cast<unsigned long>(r.sleepAtLeastS),
       r.halted ? "; HALTED on a 401" : "", r.keptImageWritten ? "; an image kept for the sleep" : "");
  if (p_.firmware != nullptr) {
    const FirmwareJobCounters& f = p_.firmware->counters();
    sayf(&out, "  firmware jobs: %lu offer(s) (%lu stored, %lu left leased), %lu apply(s) (%lu committed, %lu rolled back, %lu refused), %lu state(s), %lu power cycle(s)\n",
         static_cast<unsigned long>(f.offers), static_cast<unsigned long>(f.offers_stored),
         static_cast<unsigned long>(f.offers_left_leased), static_cast<unsigned long>(f.applies),
         static_cast<unsigned long>(f.committed), static_cast<unsigned long>(f.rolled_back),
         static_cast<unsigned long>(f.refused), static_cast<unsigned long>(f.states),
         static_cast<unsigned long>(f.power_cycles));
  }
}

bool AgentBackend::wake(interfaces::TextOut& out) {
  if (!begun_) {
    say(&out, "error: agent wake: the loop has not begun\n");
    return false;
  }
  // What the console has of a wake's knowledge: the uplink the console found up, the band the
  // duty cycle last decided (or the one it ended the last wake in), that band's own interval,
  // and S-7's state from the two machines. The radio's line is what the console knows **before**
  // the wake: `agent wake` runs no step 2, so `run()` opens the session itself and reads the
  // identity from there (task p8-04), and what it found is what the wake acted on.
  console_known_ = interfaces::BackendKnowledge{};
  console_answer_ = interfaces::BackendAnswer{};
  console_known_.uplink.up = true;
  core::power::Band band = core::power::Band::kUnknown;
  if (p_.duty != nullptr) {
    band = p_.duty->decided() ? p_.duty->decision().band : p_.duty->reason().band();
  }
  console_known_.wake.band = static_cast<std::uint8_t>(band);
  console_known_.wake.next_interval_s =
      p_.duty != nullptr && p_.duty->decided() ? p_.duty->decision().interval_seconds
                                                : core::power::minutesFor(band) * 60u;
  console_known_.wake.on_trial = p_.self != nullptr && p_.self->onTrial();
  console_known_.wake.update_running = p_.update != nullptr && p_.update->active();
  console_known_.radio.known = p_.identity->sessionUp();
  const std::uint64_t deadline = clock_.nowMs() + core::cycle::kBackendBoundMs;
  // The wake's bound is raised **for this command and no longer** (task p9-04): `allow()` only
  // ever raises and counts from the boot, so a bench session of several `agent wake`s used to
  // leave the rest of the boot bounded at twenty minutes from the boot - and the cable coming
  // out did not bring the drop to 8 mA with it. `raiseFor` answers the bound that was in force
  // and `putBack` returns to it below, whatever this command does in between.
  const std::uint32_t was =
      p_.duty != nullptr ? p_.duty->guard().raiseFor(core::cycle::kBackendBoundMs) : 0;
  const interfaces::Exchanged how = run(console_known_, deadline, &console_answer_, &out);
  // The seam's own answer, in its three words rather than two (task p10-02): *the backend
  // answered*, or why nothing was sent. `printReport` above has already said the library's
  // sentence; this is the line that says what the wake cycle was told.
  sayf(&out, "agent wake: %s (%s)%s\n", interfaces::describe(how),
       interfaces::describe(console_answer_.reached),
       console_answer_.carried != interfaces::Ask::kNothing ? "; one update step was carried out" : "");
  if (p_.duty != nullptr) {
    core::cycle::WakeGuard& guard = p_.duty->guard();
    guard.putBack(was);
    // And what that means for the cable, which is the thing a person on the bench wants to
    // know: how long this wake would last after it comes out, and why that is the figure.
    const std::uint32_t ends = guard.endsAfterCableMs();
    const core::cycle::GuardStatus& g = guard.status();
    if (!guard.armed()) {
      say(&out, "  the wake's bound is not being timed, so nothing here ends this wake\n");
    } else if (ends == 0) {
      sayf(&out, "  the bound is back to %lu s from the boot, but `stay` is on: 'stay off' is "
                 "what lets this wake end\n", static_cast<unsigned long>(g.bound_ms / 1000u));
    } else {
      // Two lines: one of these together is longer than the 200 bytes a line is printed into.
      sayf(&out, "  the wake's bound is back to %lu s from the boot (awake %lu s)\n",
           static_cast<unsigned long>(g.bound_ms / 1000u),
           static_cast<unsigned long>(g.awake_ms / 1000u));
      sayf(&out, "  pull the cable and this wake ends in %lu s - %s\n",
           static_cast<unsigned long>(ends / 1000u),
           g.awake_ms >= g.bound_ms ? "the USB grace, the bound having passed already"
                                    : "what is left of the bound, which outlasts the USB grace");
    }
  }
  // **"The loop ran"**, which is the one thing this bool has ever been asked (the console prints
  // *the loop did not run* on a false). The library's own answer to it is whether it had a
  // sentence to give for sending nothing, so that is what is read - rather than the old
  // expression over `how`, which could only be false for a stale report of a loop that had not
  // begun, a case the guard at the top of this function already answers (task p10-02).
  (void)how;
  return report_.why[0] == 0;
}

void AgentBackend::jobs(interfaces::TextOut& out) {
  if (!begun_ || p_.jobs == nullptr) {
    say(&out, "error: agent jobs: the loop has not begun\n");
    return;
  }
  OutAdapter adapter(out);
  p_.jobs->report(adapter);
  if (p_.firmware != nullptr) {
    const FirmwareJobCounters& f = p_.firmware->counters();
    const UplinkCounters& u = p_.server->uplinkCounters();
    // Three lines and not one: all of this in a single `sayf` is over the 200 bytes a line is
    // printed into, so it was cut at *0 pow* and lost its newline with the rest - which the
    // local door's block below then ran on from (task p9-04).
    sayf(&out, "firmware  %lu offer(s): %lu stored, %lu refused, %lu left leased "
               "(the result withheld, %lu)\n",
         static_cast<unsigned long>(f.offers), static_cast<unsigned long>(f.offers_stored),
         static_cast<unsigned long>(f.offers_refused), static_cast<unsigned long>(f.offers_left_leased),
         static_cast<unsigned long>(u.withheld));
    sayf(&out, "          %lu apply(s): %lu committed, %lu rolled back, %lu refused, %lu stuck, "
               "%lu switch(es), %lu owed result(s) posted\n",
         static_cast<unsigned long>(f.applies), static_cast<unsigned long>(f.committed),
         static_cast<unsigned long>(f.rolled_back), static_cast<unsigned long>(f.refused),
         static_cast<unsigned long>(f.stuck), static_cast<unsigned long>(f.switches),
         static_cast<unsigned long>(f.owed_posted));
    sayf(&out, "          %lu state(s), %lu report(s) (%lu cut); %lu power cycle(s), %lu refused; "
               "%lu progress (%lu refused), %lu extend(s); %lu bytes downloaded\n",
         static_cast<unsigned long>(f.states), static_cast<unsigned long>(f.reports),
         static_cast<unsigned long>(f.reports_cut), static_cast<unsigned long>(f.power_cycles),
         static_cast<unsigned long>(f.power_cycles_refused), static_cast<unsigned long>(f.progress_posted),
         static_cast<unsigned long>(f.progress_refused),
         static_cast<unsigned long>(f.extends), static_cast<unsigned long>(f.bytes_downloaded));
    JobEntry owed;
    if (p_.firmware->resultOwed(&owed)) {
      sayf(&out, "owed      the result of the Walter's own apply, job %lld, follows its trial\n", owed.job_id);
    }
  }
  sayLocalDoor(out);
}

// The local door, where the library's own report cannot see it (task p9-04): its counters, and
// the last ten jobs by the key they named. The library remembers the id, the type and the
// outcome; **which repeater a job was addressed to** is this end's to remember, and it is the
// thing that explains 7 November's telemetry job going on the air
// (`walter_agent/link_mesh.h`, `JobBegun`).
void AgentBackend::sayLocalDoor(interfaces::TextOut& out) const {
  if (p_.mesh == nullptr) return;
  const LinkMesh& mesh = *p_.mesh;
  const LinkMeshCounters& c = mesh.linkCounters();
  const std::uint8_t* own = mesh.ownKeyShown();
  char own_hex[2 * kKeyShownBytes + 1] = {};
  if (own != nullptr) shortKey(own, own_hex);
  sayf(&out, "local     %lu job(s) through the door, %lu request(s) made, %lu answered with "
             "silence, %lu with no request to make\n",
       static_cast<unsigned long>(c.local_jobs), static_cast<unsigned long>(c.local_requests),
       static_cast<unsigned long>(c.local_silent), static_cast<unsigned long>(c.local_unserved));
  if (own != nullptr) {
    sayf(&out, "          this radio's key %s...; a job naming it is answered here and nothing "
               "goes on the air\n", own_hex);
  } else {
    say(&out, "          this end has no key for its own repeater, so no mesh job is carried at "
              "all ('agent id')\n");
  }
  const std::size_t count = mesh.jobsBegun();
  if (count == 0) {
    say(&out, "begun     no job has been begun on the link in this boot\n");
    return;
  }
  say(&out, "begun     newest first: the repeater a job named, and where it went\n");
  for (std::size_t i = 0; i < count; ++i) {
    const JobBegun* j = mesh.jobBegunAt(i);
    if (j == nullptr) break;
    char hex[2 * kKeyShownBytes + 1] = {};
    shortKey(j->key, hex);
    if (j->local) {
      sayf(&out, "  %s...  the local door: nothing went on the air\n", hex);
    } else if (!j->own_key_known) {
      sayf(&out, "  %s...  the mesh; this end had no key of its own to tell it by\n", hex);
    } else if (own != nullptr) {
      // The line the 7 November job wanted: both keys, shortened, side by side.
      sayf(&out, "  %s...  the mesh; NOT this radio's key %s...\n", hex, own_hex);
    } else {
      sayf(&out, "  %s...  the mesh; not this radio's key, which this end no longer holds\n", hex);
    }
  }
}

// `agent calls`, and the report's `calls` section (task p13-02): the last eight calls to the
// server that did not come back 2xx, newest first. It is the answer to *httpFailures 512* - the
// library's counter is a total with no start date and leaves 401 and 429 out of it, and until
// this the reason for each failure was a log line a deep sleep forgot.
//
// **No token, no password and no byte of a body** is in a line here: an entry is a call's name, a
// status or an `interfaces::HttpOutcome`, a wake number and an epoch, and nothing else is kept.
//
// Since task p15-02 the list is followed by **what the modem said in the newest of them** - the
// receiving side of that call, event by event, from a key of its own (core/net/call_record.h).
// That record is numbers too: times, counts, a socket id, the modem's own codes.
//
// Task p15-03 put a line above the list - how often the modem had been asked a second time after
// a receive that failed, and how often that brought the answer back. It said *35 since this unit
// was made, 0 of them answered*, which is what settled the question; task p15-05 took the ask and
// the line out again. A record kept by that firmware still prints its ask, from the record.
void AgentBackend::calls(interfaces::TextOut& out) {
  if (p_.calls == nullptr) {
    say(&out, "agent calls: this build keeps no list of failed calls\n");
    return;
  }
  const CallLog& log = *p_.calls;
  sayf(&out, "agent calls: %lu of the last %lu that did not come back 2xx (401 and 429 included, "
             "which the heartbeat's httpFailures leaves out)\n",
       static_cast<unsigned long>(log.count()), static_cast<unsigned long>(kCallsKept));
  const CallLogCounters& c = log.counters();
  sayf(&out, "  %lu recorded since this unit was made, %lu pushed out, %lu written to the store, "
             "%lu refused; the list %s this boot\n",
       static_cast<unsigned long>(c.recorded), static_cast<unsigned long>(c.pushed_out),
       static_cast<unsigned long>(c.writes), static_cast<unsigned long>(c.refused),
       log.survived() ? "was read back at" : "starts at");
  if (log.count() == 0) {
    say(&out, "  no call has failed since the list began\n");
    // ... and the call that worked all the same, which is the whole of what this unit has to show
    // (task p15-07). The record kept in the store is not read here: there is no entry for it to
    // belong to, and a line about a record nobody has is noise.
    sayAnsweredCall(out, log);
    return;
  }
  for (std::size_t i = 0; i < log.count(); ++i) {
    const FailedCall call = log.at(i);
    char when[32];
    if (call.at_epoch != 0) {
      std::snprintf(when, sizeof when, "at %lu", static_cast<unsigned long>(call.at_epoch));
    } else {
      std::snprintf(when, sizeof when, "no clock then");
    }
    if (call.status != 0) {
      sayf(&out, "  %lu. %s, wake %lu, %s: http %lu\n", static_cast<unsigned long>(i + 1),
           callKindName(call.what), static_cast<unsigned long>(call.wake), when,
           static_cast<unsigned long>(call.status));
    } else {
      sayf(&out, "  %lu. %s, wake %lu, %s: nothing answered, %s\n",
           static_cast<unsigned long>(i + 1), callKindName(call.what),
           static_cast<unsigned long>(call.wake), when, interfaces::describe(call.outcome));
    }
  }
  // And under the list, **what the modem said in the newest of them** (task p15-02,
  // core/net/call_record.h): the receiving side of the newest call that did not come back 2xx,
  // read back out of a value of its own in the settings store. It is the same call as entry 1
  // above - the list and that record are written in the same moment - so the two are read
  // together, and this is the half the 8 November stall could not be read without.
  //
  // The record is a local and not a member: 79 bytes and a 159-byte local for the hex, wanted
  // when a person asks, against the same bytes of .bss that the main task's stack would have to
  // be found around (docs/ARCHITECTURE.md §14, rail R22).
  core::net::CallRecord receiving;
  if (!log.keptReceiving(receiving)) {
    say(&out, "  what the modem said in it: nothing kept yet - no call has failed since the "
              "firmware that writes it down was flashed\n");
  } else {
    const std::uint8_t which = receiving.callWas();
    receiving.describe(out, callKindName(which < static_cast<std::uint8_t>(CallKind::kCount)
                                             ? static_cast<CallKind>(which)
                                             : CallKind::kUnknown));
  }
  sayAnsweredCall(out, log);
}

// And **the call that worked**, under the one that did not (task p15-07). Until this there was
// nothing to lay a failed call beside: the config, the heartbeat, the claim and a job's result are
// answered at every wake by the same server through the same code, and nothing was kept of them -
// so whether a good answer comes in two pieces, whether the server closes behind it and when the
// receive is sent against the rings could not be said, and every cure for the call that fails was
// a guess.
//
// **From RAM, and from this boot only.** The failed call's record is one value of the settings
// store; this one is in no value at all, because keeping it would be a write to flash at every
// good wake. It does not need to be: a report is made in the middle of a wake, straight after the
// calls that worked (`walter_agent/call_log.h`).
//
// It is numbers and fixed words, like the block above it: times, counts, a socket id, a status,
// how the body was framed and whether the server said `Connection: close` - and no byte of a body,
// no header's value, no token and no name (spec/SYSTEM.md S-12).
void AgentBackend::sayAnsweredCall(interfaces::TextOut& out, const CallLog& log) const {
  const core::net::CallRecord* answered = log.answeredReceiving();
  if (answered == nullptr) {
    say(&out, "  no call has been answered in this boot, so there is nothing to lay beside it\n");
    return;
  }
  const std::uint8_t which = answered->callWas();
  answered->describe(out,
                     callKindName(which < static_cast<std::uint8_t>(CallKind::kCount)
                                      ? static_cast<CallKind>(which)
                                      : CallKind::kUnknown),
                     "in the latest call that was answered");
}

// `agent state`'s flood line, and the report's `agent` section with it (task p14-02): the scope
// and the rule in the **library's own words** and the three counts as **this board's totals**.
//
// The words are read back out of `JobRunner::describeFlood`, which is the one place the four
// readings of the rule are written - this mesh says nothing about scope, every flood goes out with
// no transport code, the second flood of a transaction goes unscoped, every flood is scoped - and
// which this task may not change (`components/agent-core` is a hash-checked copy). That line is
// `scope <name>, answered scoped A / unscoped B / none C since boot; <the rule>`, so the clause
// after the **last** `; ` is the rule and nothing else: not one of the four carries a semicolon,
// and a host test holds all four to it. A library whose line changed shape would put its whole
// line where the rule goes - nothing invented and nothing lost - and that test would say so
// first.
//
// Why the totals and not the library's counts: the library's are since boot and this board boots
// at every wake, so its own line would read 0 / 0 / 0 on a unit that has been flooding for a
// month (walter_agent/flood_counters.h).
void AgentBackend::sayFlood(interfaces::TextOut& out) const {
  if (p_.jobs == nullptr) return;
  char line[200] = {};
  p_.jobs->describeFlood(line, sizeof line);
  const char* rule = line;
  for (const char* at = line; *at != 0; ++at) {
    if (at[0] == ';' && at[1] == ' ') rule = at + 2;
  }
  const FloodTotals kept = p_.floods != nullptr ? p_.floods->totals() : FloodTotals();
  const char* scope = p_.mesh != nullptr ? p_.mesh->floodScope() : nullptr;
  // Two writes of one line, as the hold's two halves are: a 30-byte scope name, three totals and
  // the longest of the library's four rules are more together than the 200 bytes one line of this
  // console may be, and a line that truncated would lose its newline with its words.
  sayf(&out, "  flood: scope %s, answered scoped %lu / unscoped %lu / none %lu in all",
       scope == nullptr ? "none" : scope, static_cast<unsigned long>(kept.answered_scoped),
       static_cast<unsigned long>(kept.answered_unscoped),
       static_cast<unsigned long>(kept.unanswered));
  sayf(&out, "; %s\n", rule);
  // **And how often the three could be wrong** - a line only on a unit that has had one. The
  // library files a transaction under the mode its rule *asked* for, and a flood whose answer said
  // it went out in another one (a `region default` cleared under this end's feet) is counted under
  // the wrong of the three. This end is the only one that notices, and it cannot correct the
  // library's figure (`components/agent-core` is a copy): `LinkMeshCounters::scope_surprises` is
  // therefore how far the three above can be trusted, and on a healthy unit it is 0 and this line
  // is not printed at all (tasks/phase-14/FOLLOWUPS.md).
  const std::uint32_t surprises = p_.mesh != nullptr ? p_.mesh->linkCounters().scope_surprises : 0;
  if (surprises != 0) {
    sayf(&out, "         %lu flood(s) went out in a scope this end had not asked for, so that "
               "many of the three above may be under the wrong one\n",
         static_cast<unsigned long>(surprises));
  }
}

void AgentBackend::state(interfaces::TextOut& out) {
  if (!begun_ || p_.loop == nullptr) {
    say(&out, "error: agent state: the loop has not begun\n");
    return;
  }
  const mm::WakeState s = p_.loop->state();
  sayf(&out, "agent state: %s across the last sleep (%lu restored, %lu saved, %lu refused)\n",
       state_survived_ ? "restored" : "not restored", static_cast<unsigned long>(counters_.state_restored),
       static_cast<unsigned long>(counters_.state_saved), static_cast<unsigned long>(counters_.state_refused));
  // The hold, and - in a second write on the same line, because one line of this console is 200
  // bytes and the two halves together can be longer than that - **what the sleep took off it**
  // (task p10-01). Before this firmware measured the sleep, the figure after the semicolon was
  // always "0 s slept" and a wait the server asked for outlived five or six wakes.
  sayf(&out, "  version %lu; backoff %lu ms, %lu consecutive failure(s) for %lu ms, hold %lu ms left",
       static_cast<unsigned long>(s.version), static_cast<unsigned long>(s.backoffMs),
       static_cast<unsigned long>(s.consecutiveFailures), static_cast<unsigned long>(s.failingForMs),
       static_cast<unsigned long>(s.holdForMs));
  if (!state_survived_) {
    say(&out, "\n");
  } else {
    // **Where this boot's time away came from** (task p14-06). Until it, a wake that was not the
    // timer's read *no sleep this end could measure* - and that was the whole fault: the radio
    // never sleeps and could have been asked all along. The three answers are the timer's own
    // figure, the radio's uptime across the sleep, and nothing at all - the last being a radio
    // that restarted with us, or answered nothing, or had no reading written down, and no longer
    // simply "the timer did not wake us".
    const core::cycle::WakeReason::AwayFrom from =
        p_.duty != nullptr ? p_.duty->reason().awayFrom()
                           : core::cycle::WakeReason::AwayFrom::kNothing;
    sayf(&out, "; %lu s owed before the sleep, ",
         static_cast<unsigned long>(owed_before_ms_ / 1000u));
    switch (from) {
      case core::cycle::WakeReason::AwayFrom::kTimer:
        sayf(&out, "%lu s away, by the sleep timer\n",
             static_cast<unsigned long>(away_ms_ / 1000u));
        break;
      case core::cycle::WakeReason::AwayFrom::kRadio:
        sayf(&out, "%lu s away, by the radio's uptime\n",
             static_cast<unsigned long>(away_ms_ / 1000u));
        break;
      case core::cycle::WakeReason::AwayFrom::kNothing:
        say(&out, "nothing could measure how long this board was away\n");
        break;
    }
  }
  // **A wait that was not put back at all** (tasks p14-05 and p14-06): a line of its own, because
  // the figure is not the one above - a person's wake lays aside what is left of a wait *after*
  // the time away has come off it - and because why it was laid aside is the sentence a person
  // who is looking for their heartbeat wants.
  if (laid_aside_ != LaidAside::kNone) {
    sayf(&out, "  %lu s of a wait was laid aside: %s\n",
         static_cast<unsigned long>(holdLaidAsideMs() / 1000u), holdLaidAsideWhy());
  }
  sayf(&out, "  accepted interval %d s; budget %d/min, gap %d s; %s; %s, last heartbeat at %lu\n", s.pollIntervalS,
       s.budget.maxTransactionsPerMinute, s.budget.minGapSeconds,
       s.halted ? "HALTED on a 401 (set server.token)" : "not halted",
       s.everHeartbeated ? "has heartbeated" : "never heartbeated",
       static_cast<unsigned long>(s.lastHeartbeatAtEpoch));
  sayf(&out, "  counters: %lu heartbeat(s), %lu http failure(s), %lu reconnect(s), %lu batch(es), %lu observation(s) posted, %lu refused, %lu claim failure(s), %lu result(s) posted late\n",
       static_cast<unsigned long>(s.counters.heartbeats), static_cast<unsigned long>(s.counters.httpFailures),
       static_cast<unsigned long>(s.counters.wifiReconnects), static_cast<unsigned long>(s.counters.batches),
       static_cast<unsigned long>(s.counters.observationsPosted),
       static_cast<unsigned long>(s.counters.observationsRefused),
       static_cast<unsigned long>(s.counters.claimFailures), static_cast<unsigned long>(s.counters.resultsPosted));
  // The scope this unit's floods go out in, the rule, and the three totals (task p14-02): the one
  // line a person reads to know whether the unscoped try still earns its airtime, and the same
  // three numbers the heartbeat's `flood` object carries.
  sayFlood(out);
  // **Is this unit's clock set, and from where** (task p13-01). Every figure above that is an
  // epoch - the last heartbeat, a result's timestamps - is this clock's, and a unit that never
  // attached has none at all: `lastHeartbeatAtEpoch 0` then means *no clock*, not *never*. The
  // source is spec/LINK-PROTOCOL.md §5.4's: 1 the cellular network, 2 the server.
  interfaces::WallTime time{};
  if (wall_.now(&time)) {
    sayf(&out, "  the clock: %lu, from %s\n", static_cast<unsigned long>(time.epoch),
         core::link::client::timeSourceName(time.source));
  } else {
    say(&out, "  the clock: not set in this boot, so nothing this end says is dated (a sleep "
              "loses it: there is no RTC to keep it in)\n");
  }
  // The trial's own heartbeats (task p14-05): the ones it asked for beyond the wake's, and the
  // times it asked and the library's wait was still running. Both 0 in every boot but a trial's.
  sayf(&out, "  the trial asked for %lu heartbeat(s) of its own, held off %lu time(s)\n",
       static_cast<unsigned long>(counters_.trial_beats),
       static_cast<unsigned long>(counters_.trial_beats_held));
  sayf(&out, "  the sleep keeps: %s, %lu image(s) written, %lu read back, %lu refused whole; %lu exchange(s), %lu wake(s) run, %lu not run\n",
       p_.kept != nullptr ? (p_.kept->capacity() != 0 ? "a flash window" : "a store of no capacity") : "no store",
       static_cast<unsigned long>(counters_.kept_written), static_cast<unsigned long>(counters_.kept_restored),
       static_cast<unsigned long>(p_.loop->keptImagesRefused()), static_cast<unsigned long>(counters_.exchanges),
       static_cast<unsigned long>(counters_.wakes), static_cast<unsigned long>(counters_.not_run));
}

}  // namespace agent
