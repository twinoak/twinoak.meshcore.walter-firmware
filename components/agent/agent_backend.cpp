#include "walter_agent/agent_backend.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <agent/Log.h>
#include <agent/TextOut.h>

#include "core/cycle/wake_cycle.h"
#include "core/power/interval_policy.h"

namespace agent {

namespace {

std::uint32_t saturate(std::uint32_t value) { return value == 0xFFFFFFFFu ? value : value + 1; }

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

}  // namespace

AgentBackend::AgentBackend(const Parts& parts, interfaces::Clock& clock, interfaces::WallClock& wall)
    : p_(parts), clock_(clock), wall_(wall) {}

void AgentBackend::begin(void (*pump)()) {
  pump_ = pump;
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
  // Inside the library's own 239-character line (agent/Log.cpp), with the longest of the three
  // reasons and the four types in it: a line that said why and was cut off would say neither.
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
  LOG_I("agent: %s: only this unit's own job types are claimed (%s); a mesh job this end cannot "
        "tell is its own is not claimed",
        why, at == 0 ? "none in this build" : types);
}

// -- the state across a sleep ----------------------------------------------------------------------

bool AgentBackend::saveState() {
  if (p_.state_store == nullptr || !p_.state_store->ready()) return false;
  const mm::WakeState s = p_.loop->state();
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
  // The library refuses a state of another version and changes nothing.
  if (!p_.loop->restore(s)) {
    counters_.state_refused = saturate(counters_.state_refused);
    return false;
  }
  std::memcpy(last_hex_, state_hex_, sizeof last_hex_);
  counters_.state_restored = saturate(counters_.state_restored);
  return true;
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

  // Before the wake: who this unit is - a session opened if there is none, and the identity read
  // (a radio that got one between two wakes reports it now) - the uplink's state and deadline,
  // what the jobs may do, and §7's ring drained into the loop's own so the batch goes with this
  // wake.
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
  }

  if (talk != nullptr) printReport(*talk);

  // How it went, in the seam's three words. A heartbeat the server did not answer is the
  // exchange failing (news for the journal); a wake that did nothing because it had nobody to
  // talk to - not configured, halted on a 401, a wait still running - is *nothing to do*, which
  // is the null backend's answer and is not news every wake.
  if (!ran) return interfaces::Exchanged::kNothing;
  if (report_.heartbeat != mm::StepOutcome::Done) return interfaces::Exchanged::kFailed;
  const bool did_something = report_.jobsRun > 0 || report_.jobsNotRun > 0 || report_.resultsFlushed > 0 ||
                             report_.batchesPosted > 0 || posted_owed ||
                             (p_.firmware != nullptr && p_.firmware->did().asks != 0);
  return did_something ? interfaces::Exchanged::kAsked : interfaces::Exchanged::kNothing;
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
  sayf(&out, "agent wake: %s%s\n", interfaces::describe(how),
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
  return how != interfaces::Exchanged::kFailed || report_.why[0] == 0;
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
    sayf(&out, "          %lu state(s); %lu power cycle(s), %lu refused; %lu progress, "
               "%lu extend(s); %lu bytes downloaded\n",
         static_cast<unsigned long>(f.states), static_cast<unsigned long>(f.power_cycles),
         static_cast<unsigned long>(f.power_cycles_refused), static_cast<unsigned long>(f.progress_posted),
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

void AgentBackend::state(interfaces::TextOut& out) {
  if (!begun_ || p_.loop == nullptr) {
    say(&out, "error: agent state: the loop has not begun\n");
    return;
  }
  const mm::WakeState s = p_.loop->state();
  sayf(&out, "agent state: %s across the last sleep (%lu restored, %lu saved, %lu refused)\n",
       state_survived_ ? "restored" : "not restored", static_cast<unsigned long>(counters_.state_restored),
       static_cast<unsigned long>(counters_.state_saved), static_cast<unsigned long>(counters_.state_refused));
  sayf(&out, "  version %lu; backoff %lu ms, %lu consecutive failure(s) for %lu ms, hold %lu ms left\n",
       static_cast<unsigned long>(s.version), static_cast<unsigned long>(s.backoffMs),
       static_cast<unsigned long>(s.consecutiveFailures), static_cast<unsigned long>(s.failingForMs),
       static_cast<unsigned long>(s.holdForMs));
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
  sayf(&out, "  the sleep keeps: %s, %lu image(s) written, %lu read back, %lu refused whole; %lu exchange(s), %lu wake(s) run, %lu not run\n",
       p_.kept != nullptr ? (p_.kept->capacity() != 0 ? "a flash window" : "a store of no capacity") : "no store",
       static_cast<unsigned long>(counters_.kept_written), static_cast<unsigned long>(counters_.kept_restored),
       static_cast<unsigned long>(p_.loop->keptImagesRefused()), static_cast<unsigned long>(counters_.exchanges),
       static_cast<unsigned long>(counters_.wakes), static_cast<unsigned long>(counters_.not_run));
}

}  // namespace agent
