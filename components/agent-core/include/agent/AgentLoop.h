// The agent's loop: the part of EXIT-NODES §5 and §6 a board with no jobs does.
//
// Once the link is up: GET /agent/v1/config once, then POST /agent/v1/heartbeat every
// `pollIntervalS` the server last answered (30 when work waits, 120 when none does), and
// between heartbeats POST /agent/v1/observations with the adverts the radio heard - a batch when
// twenty are waiting or when the oldest has waited a minute, whichever comes first.
//
// Since task 03 a heartbeat that answers `jobsWaiting` is followed by a claim: up to two jobs,
// run one at a time at the budget's pace, each result posted as its job ends (JobRunner.h).
//
// Since task 06 the order inside one pass is the .NET agent's, in its own words
// (docs/ops/agent.md §4): the heartbeat, then **the offline buffer** - anything that waited is
// posted first, in order, with the timestamps it was made with - then the claim, then the
// observation batch. And the wait after a failure carries jitter (agent/Backoff.h), so a hundred
// boards that lost the same server do not all come back at the same second; past
// `kRebootAfterFailures` consecutive unanswered calls **with the link up**, over
// `kRebootAfterFailingMs`, the board restarts itself and says why (IPlatform::rebootForHealth,
// which is src/Health.h on the Heltec), because a TLS stack that has failed for an hour and a
// half against a live link is not going to recover by being asked again - and the hour and a
// half is a condition of its own since task p1.2-10 lowered the proven link's backoff ceiling
// to two minutes.
//
// What the server's answers do here:
//   200   the budget, the poll interval and the clock are taken, and `jobsWaiting` makes a claim
//         due before the next heartbeat
//   401   everything stops. The token was revoked or is wrong, and no amount of retrying fixes
//         either; the console still works and the OLED's fourth line says so, and a
//         `set meshmaster.token`
//         (or a reboot) is what starts the loop again
//   429   the wait is `Retry-After`, to the second
//   502, 503, 504 with a Retry-After
//         the same: a deploy of meshmaster.dk says how long it will be, and that is believed
//         rather than doubled past it (task p1.2-10)
//   5xx, no reply
//         backoff, 30 s doubling to **two minutes** once this board's own link is proven - the
//         link up and a time off the network - and to ten minutes while it is not, reset by the
//         first answer
//
// `jobTypes` is the thirteen types JobRunner has a handler for - every type of API §3 since
// task 05 - less any the mesh says it cannot carry (IMesh::supports, task p1.6-23; the Heltec's
// mesh carries all thirteen), and then the types of the extension the board set, if any
// (IJobExtension.h); routing sends nothing else, because it requires the type advertised.
//
// Host-portable since task p1.5-15: the server, the platform and the mesh are the three seams
// (IUplink.h, IPlatform.h, IMesh.h), and what the heartbeat says about this board - the version,
// the board, the companion firmware, the radio - comes in as values (HeartbeatIdentity) that the
// board fills and refreshes, rather than being read from its settings here.
//
// **Two ways to be driven, since task p1.5-18.** Everything above is `loop()`, called for ever
// from the Arduino loop with the link up: that is the Heltec, and nothing of it changed. A device
// that sleeps - the Walter of a repeater-v2 unit, on a battery and a panel, attaching to LTE a
// few times a day - calls `wake()` instead: one pass of the same steps in the same order, each
// bounded in time and in energy, and then it returns and the caller sleeps for the interval the
// report names. The two share every rule they can: the same heartbeat, the same offline buffer,
// the same claim, the same JobRunner, the same ServerPolicy. What one wake adds is bounds (a
// Budget), a promise to the server (a WakeIntent, the two optional heartbeat fields of
// EXIT-NODES §6), a record of what it did (a WakeReport), and a state that survives a deep
// sleep (a WakeState, which the caller keeps in retained storage and hands back). Two things it
// subtracts: a BackOff returns rather than spins, and the reboot guard does not fire - a unit
// has its own supervisor and its own idea of what a failure costs.
//
// **What a sleep keeps, since task p1.6-22.** The WakeState is small on purpose and carries no
// result and no advert, so a result a wake kept for its budget - or one the server would not
// take - was lost with the RAM at the next deep sleep, and its job stayed leased until the
// server's lease lapsed. A sleeping consumer may now hand the loop a store (IRetained.h) and a
// scratch: at the end of every wake the loop writes **one image** of the kept results and the
// ring's waiting adverts into it, only when that would change what the store holds, and
// `restoreKept()` reads it back after the next boot. The Heltec hands none and nothing of it
// runs there.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <agent/Envelopes.h>
#include <agent/IMesh.h>
#include <agent/IPlatform.h>
#include <agent/IRetained.h>
#include <agent/IUplink.h>
#include <agent/JobRunner.h>
#include <agent/Observations.h>
#include <agent/ServerPolicy.h>
#include <agent/TextOut.h>
#include <agent/UplinkMeter.h>

namespace mm {

// What earns a reboot: consecutive server failures - with the link up, so the link is not the
// reason - for long enough that the stack and not the server is the suspect. Task 06 wrote the
// count alone, and twelve of them at a backoff doubling to ten minutes *was* about an hour and a
// half; task p1.2-10 lowered that ceiling to two minutes for a board whose link is proven, which
// would have brought twelve within about twenty - and twenty minutes of a server being down is
// an ordinary deploy, not a wedged stack. So both have to be true, and the sentence task 06
// wrote stays true of either backoff: a board reboots at most about once an hour and a half
// through a long outage, and once is usually all a wedged stack needs. What a reboot costs is
// the buffers (the advert ring and the results waiting to be posted late), which is exactly what
// a server outage fills. (Moved here from src/Health.h by task p1.5-15; the restart itself is
// the platform's, IPlatform::rebootForHealth.)
constexpr uint32_t kRebootAfterFailures = 12;
constexpr uint32_t kRebootAfterFailingMs = 90u * 60000u;

// A claim's own transaction price, as EXIT-NODES §7 and the server's `JobDefaults.TransactionsOf`
// price it: what the type costs at this class and for this payload (Airtime.h's transactionsFor),
// plus the login it needs and the ACL restore an admin session on a repeater with a grant ends
// with. It is the estimate a wake sizes a job by, deliberately an over-estimate where it cannot
// know - which is the right way round for a battery.
int transactionsOf(const ClaimedJob& job);

// The cheapest job there is on the air that needs a session: one login and one request
// (EXIT-NODES §7 - a trace at class `none` is one transaction and is the only thing below it). A
// wake asks for no more jobs than its transaction cap can hold at this price, because the jobs'
// types are not known until the claim has answered.
constexpr int kTransactionsPerJobAtLeast = 2;

// What one wake promises the server, in the heartbeat (EXIT-NODES §6, *An agent that paces
// itself*). Both fields are 0 for an agent that proposes nothing - the Heltec, whose heartbeat
// bytes are then exactly what they were - and `loop()` never sets either.
struct WakeIntent {
  // How long until this agent will be back, in seconds. The server accepts it between 60 and
  // 86 400, clamping rather than refusing, and the reply's `pollIntervalS` is what was accepted:
  // **that** is the interval to sleep by, and it is in the WakeReport.
  uint32_t nextCheckInS = 0;
  // What this wake has the airtime and the energy for. The server hands out jobs until the next
  // one's price would take the wake past it (task p4.14-121); this loop asks for no more than it
  // can run and prices every job it is given against what is left.
  int maxTransactionsThisWake = 0;
};

// What one wake may spend. Everything here is a bound and 0 is "no bound", so a Budget nobody
// filled in is a wake that does as much as it can - which is what a test of the steps alone
// wants. The transaction cap is **not** here: it is the intent's `maxTransactionsThisWake`, one
// number that is stated to the server and enforced here, rather than two that can disagree.
struct Budget {
  // The whole wake, from the first call to the return. A step that would not fit is skipped and
  // said so in the report (the Walter's *no time*, docs/ARCHITECTURE.md §10).
  uint32_t wakeSeconds = 0;
  // Of that, what the calls to meshmaster.dk together may take: an LTE attach that went badly
  // must not eat the slice the radio needs. **Every** call the wake makes is in it - the config,
  // the heartbeat, the claim, every result and every batch - because they all go through one
  // ledger (agent/UplinkMeter.h, task p1.6-21). When it is spent the wake stops calling the
  // server and keeps what is left for the next wake.
  uint32_t uplinkSeconds = 0;
  // The bytes of body this wake may post, because an LTE plan is metered in bytes: **every**
  // body it posts, from every step - the offline buffer, every result of a job it claimed, every
  // observation batch - against one number (task p1.6-21). What does not fit stays where it is,
  // with the timestamps it was made with, for the next wake. The bytes of the config call, the
  // heartbeat and the claim are not counted here: those bodies are the uplink implementation's
  // to write and are not countable from inside the library. Their seconds are counted.
  size_t postBytes = 0;
};

// The loop's own counters. At namespace scope, like MeshCounters, because a WakeReport and a
// WakeState both carry a copy and neither is a member of the loop; `AgentLoop::Counters` is
// still the name every caller uses.
struct AgentCounters {
  uint32_t heartbeats = 0;           // answered 200
  uint32_t httpFailures = 0;         // a call that did not come back 2xx, 401 and 429 apart
  uint32_t wifiReconnects = 0;       // links after the first
  uint32_t batches = 0;              // observation batches the server took
  uint32_t observationsPosted = 0;
  uint32_t observationsRefused = 0;  // a batch the server refused for good, and the ring dropped
  uint32_t claimFailures = 0;        // a claim that did not come back
  uint32_t resultsPosted = 0;        // results that went through from the offline buffer, late
  uint32_t consecutiveFailures = 0;  // calls that got no answer since the last one that did
};

// What one step of a wake came to.
enum class StepOutcome : uint8_t {
  Skipped,   // its precondition was not met: there was nothing for it to do
  Done,
  Failed,    // the server did not answer it, or the mesh did not
  NoTime,    // the wake's seconds, or the uplink's, had run out
  NoBudget,  // the transaction cap or the byte cap had run out
};
const char* stepOutcomeName(StepOutcome outcome);

// What one wake did, for the caller that has to sleep after it. `sleepAtLeastS` and
// `acceptedIntervalS` are the two numbers a duty cycle needs: the first is a wait this agent
// owes the server (a backoff, or a Retry-After it was given) and the second is the interval the
// server accepted, which is what the next check-in is due in.
struct WakeReport {
  // The six steps, in the order they run.
  StepOutcome config = StepOutcome::Skipped;
  StepOutcome heartbeat = StepOutcome::Skipped;
  StepOutcome results = StepOutcome::Skipped;
  StepOutcome claim = StepOutcome::Skipped;
  StepOutcome jobs = StepOutcome::Skipped;
  StepOutcome observations = StepOutcome::Skipped;
  // One sentence when the wake did nothing at all, a static string; "" when it ran its steps.
  const char* why = "";

  int acceptedIntervalS = 0;   // the heartbeat reply's pollIntervalS, or the last one known
  uint32_t serverTime = 0;     // UNIX seconds, as the heartbeat reply gave them; 0 when none did
  uint32_t sleepAtLeastS = 0;  // a backoff or a Retry-After: do not come back before this

  int resultsFlushed = 0;      // results that had been waiting and went through
  // Results this wake could not post inside its byte cap or its uplink seconds: **kept** in the
  // offline buffer, behind whatever was already there, for the next wake to post with the
  // timestamps they were made with (*kept: budget*). A bound of ours is not a failure of the
  // server's, so nothing backs off for one; what must never happen is a claimed job left leased
  // and silent, and a kept result is the buffer's answer to that (EXIT-NODES §5, *offline is
  // normal*).
  int resultsKept = 0;
  int jobsClaimed = 0;
  int jobsRun = 0;
  int jobsNotRun = 0;          // claimed, never on the air: reported `failed`, *wake budget*
  int batchesPosted = 0;
  int observationsPosted = 0;
  int observationsLeft = 0;    // still in the ring when the wake ended
  int transactionsSpent = 0;   // what went on the air, against the intent's cap
  size_t bytesPosted = 0;      // every byte of body this wake posted, from every step
  uint32_t uplinkMs = 0;       // what the server calls took together, which uplinkSeconds bounds
  uint32_t elapsedMs = 0;

  // What this wake left in retained storage (IRetained.h, task p1.6-22), when the caller gave
  // the loop a store: whether an image was written at all - at most one a wake, and none when
  // the store already holds what it would say - and what the store was too small for, dropped
  // from RAM and counted in the heartbeat's `resultsDropped` and `observationsDropped`.
  bool keptImageWritten = false;
  int resultsNotKept = 0;
  int observationsNotKept = 0;

  bool halted = false;         // a 401: nothing more is sent until a person fixes it
  // The reboot guard **would** have fired (AgentLoop's twelve unanswered calls over ninety
  // minutes). In one-wake mode it does not: a unit has a supervisor of its own and its own idea
  // of what a failure costs, so this is said and not done.
  bool rebootWanted = false;
  char rebootWhy[64] = {0};

  AgentCounters counters;      // the six the heartbeat carries, and the ones beside them
};

// What a wake must not forget while the device is asleep. The Walter deep-sleeps between wakes,
// so this object - and the millisecond clock - start again from nothing every time; everything
// the loop keeps that matters across a sleep is here, as a POD the caller writes to retained
// storage (or an NVS blob) and hands back with `restore()` after the next boot.
//
// Every duration in it is a **duration** and not a deadline, because `millis()` restarts at zero:
// `holdForMs` is what was left of a wait when the wake returned, and `failingForMs` is how long
// the present run of unanswered calls has lasted. `restore()` re-bases both against the clock it
// finds. A caller that hands back a state from a different build is caught by `version`.
struct WakeState {
  static constexpr uint32_t kVersion = 1;

  uint32_t version = kVersion;
  uint32_t backoffMs = 0;             // the step the next failure doubles from
  uint32_t consecutiveFailures = 0;   // the run the reboot guard counts, and the backoff with it
  uint32_t holdForMs = 0;             // what is left of a backoff or a Retry-After
  uint32_t failingForMs = 0;          // how long that run of failures has been going
  int pollIntervalS = 30;             // the interval the server last accepted
  AgentBudget budget;                 // the airtime budget the server last named
  bool halted = false;                // a 401 is still a 401 after a sleep
  bool everHeartbeated = false;
  uint32_t lastHeartbeatAtEpoch = 0;
  AgentCounters counters;
};
static_assert(std::is_trivially_copyable<WakeState>::value, "a wake's state goes to retained storage as bytes");

// What the heartbeat says about this board, as values the board fills (main.cpp) and refreshes
// when a `radio.*` setting changes: the library reads no setting of its own.
struct HeartbeatIdentity {
  const char* agentVersion = "";           // meshmaster_agent_version()
  // How this exit node reaches its radio, in meshmaster.dk's words (EXIT-NODES §2, the server's
  // `AgentTransports`): `serial` and `tcp` are a companion on a cable, `native` is an exit node
  // that *is* its repeater (the unit of D-45: the Walter over its box link), and `onboard` is a
  // board that is its own exit node (D-28) - this one, so the default is its word and the Heltec
  // sets nothing. The library validates nothing here: the vocabulary is the server's, a word it
  // does not know is its to refuse, and a hand-written list in this library would be a second
  // copy of it that could drift (task p1.6-23).
  const char* transport = "onboard";
  const char* board = nullptr;             // boardName(); nullptr leaves the field out
  const char* companionFirmware = nullptr; // meshcoreTag() on the Heltec, whose companion firmware is the one it is built from
  RadioParameters radio;                   // what the radio is programmed with; sent while there is an identity
};

class AgentLoop {
 public:
  using Counters = AgentCounters;

  // `identity` is read at every heartbeat, so the board keeps the object and refreshes it.
  void begin(IUplink& server, IPlatform& platform, IMesh& mesh, JobRunner& jobs, const HeartbeatIdentity& identity);

  // The board's own identity, as 64 lowercase hex, or nullptr while the radio has made none.
  void setIdentity(const char* publicKeyHex);

  void loop();

  // One wake, for a device that sleeps (task p1.5-18; proposal 01 parts B and C of the
  // repeater-v2 design, the Walter's docs/ARCHITECTURE.md §10). One pass of the same steps
  // `loop()` spreads over minutes, in the same order, each bounded by the budget, and then it
  // returns: the caller sleeps for `report.acceptedIntervalS` - or at least
  // `report.sleepAtLeastS`, when the server asked for a wait - and calls this again.
  //
  //   the config     when this uplink has none yet, which after a deep sleep is every boot
  //   the heartbeat  with the intent; its reply's `pollIntervalS` is the accepted interval, and
  //                  the clock takes the server's time here, before any job needs a timestamp
  //   the results    the offline buffer, oldest first, with the timestamps they were made with
  //   the claim      when the heartbeat said work waits: `max` sized from what the cap can hold
  //   the jobs       each priced before it goes on the air; one this wake has no room for is
  //                  reported `failed` with *wake budget* and **never left leased and silent**
  //   the batches    the ring drained oldest first, as many batches as the byte cap allows
  //
  // A failure does not spin here: a BackOff is a wait the report carries back as
  // `sleepAtLeastS`, and the state that keeps it (`state()`) survives the sleep. The reboot
  // guard does not fire; `report.rebootWanted` says it would have.
  WakeReport wake(const WakeIntent& intent, const Budget& budget);

  // The state a wake leaves behind, for a caller that is about to lose its RAM, and the way back
  // in after the sleep. `restore()` re-bases every duration against the clock it finds now, and
  // refuses a state of another version (returning false and changing nothing).
  WakeState state() const;
  bool restore(const WakeState& state);

  // What a sleep keeps (task p1.6-22; agent/IRetained.h, agent/KeptImage.h). Optional, and for
  // a device that sleeps: the Heltec calls neither.
  //
  // `setRetained` hands the loop a store and a scratch the image is built in - the library
  // allocates nothing, so the RAM is yours, sized to the most you want kept: a full image is
  // ResultBuffer::kCapacityBytes plus ObservationRing::kCapacity adverts at kKeptAdvertLen and a
  // few hundred bytes of headers, about 19.5 kB; the image is never larger than the smaller of
  // `scratchLen` and the store's `capacity()`, and what does not fit is dropped and counted. A
  // scratch too small for a header, or a null store, is "no store".
  //
  // At the end of every `wake()` the loop writes **one image** - the kept results first, oldest
  // first, as many whole ones as fit, then the ring's waiting adverts with the moment each was
  // heard, as many as fit after them - and only when that image is not the one the store already
  // holds; a unit whose wakes post everything writes nothing. `restoreKept()` is the way back in:
  // call it once a boot, after `begin()` and before the first wake, and it reads the image into
  // the offline buffer and the ring, with the two dropped counters the image carried. True when
  // an image was read, false when the store held none or the image was refused - a torn, short,
  // foreign-version or bad-checksum image is refused **whole**, never half-read, counted
  // (`keptImagesRefused()`, which the next image carries across the sleep too) and logged with
  // the reason.
  void setRetained(IRetained* store, uint8_t* scratch, size_t scratchLen);
  bool restoreKept();
  uint32_t keptImagesRefused() const { return imagesRefused_; }

  // One heartbeat, from inside a job (task 06). A claim is run to the end before `claimAndRun`
  // returns and a job is paced by the airtime budget, so a config.read of twenty-one transactions
  // at two a minute is eleven minutes and a region load is thirteen - during which, before this, no
  // heartbeat could go and the exit node's page went *quiet* and then *offline* while the board
  // was working perfectly (the note tasks 03, 04 and 05 each left for this one). The job's own
  // waits are where the time is, so the pump that keeps the console and the OLED alive through
  // them sends the heartbeat that is due as well. Nothing else of the loop runs there: no claim,
  // no batch, one blocking call inside a wait that was going to be spent waiting.
  void heartbeatIfOverdue();

  // A new link (WiFi associated, on the Heltec): the config call is due again, and the heartbeat
  // schedule starts over.
  void onLinkUp();

  // After `set meshmaster.*` or `erase confirm`: forget a 401 and start again.
  void reconfigure();

  ObservationRing& ring() { return ring_; }
  const Counters& counters() const { return counters_; }
  bool halted() const { return halted_; }

  // What the OLED's four lines need of this loop (src/ui/DisplayLines.h). Every one of them is
  // a plain read of state this object already keeps, safe to call before begin() has run - the
  // first frame is drawn from main.cpp's setup() and the panel has no reason to know.
  bool everHeartbeated() const { return everHeartbeated_; }
  uint32_t lastHeartbeatAtEpoch() const { return lastHeartbeatAtEpoch_; }
  // *Online* is the last call to MeshMaster having been answered and nothing standing between
  // this board and the next call: a heartbeat has happened, nothing has gone unanswered since,
  // the token has not been refused, and there is no wait running. The last of those is what a
  // deploy's 503 makes: a believed `Retry-After` is deliberately **not** a consecutive failure
  // (task p1.2-10, and the heartbeat's counters are unchanged by this), but a server that has
  // said *come back in 45 s* has not answered either, so the panel must not read *Online: yes*
  // through it (task p1.3-12).
  bool online() const;
  // Seconds until the next attempt while backing off (or holding a 429's Retry-After); 0 when
  // the wait is over or there is none.
  uint32_t retrySeconds() const;

  // `server` on the console: the last heartbeat, the next, the last batch, the counters.
  void report(TextOut& out) const;

  // `errors` on the console: every counter the heartbeat carries, and the ones beside them.
  void reportErrors(TextOut& out) const;

  // One line for `status`.
  void describe(char* out, size_t cap) const;

 private:
  bool doConfig();
  bool doHeartbeat();
  void doPendingResults();
  // One observation batch, bounded by what a wake may still post (the ledger's `bytesLeft`): a
  // batch that will not fit is shortened until it does, and `loop()`, whose ledger has no
  // bounds, sends the batch it always sent.
  // `capped` says the batch would not fit in the bytes left rather than the server having
  // refused it, which are the two ways this returns false and two different things to report.
  bool doObservations(bool* capped = nullptr);
  void doJobs();
  void buildHeartbeat(Heartbeat& hb);
  // The steps of one wake, each of which records its own outcome in the report.
  void wakeConfig(WakeReport& report);
  void wakeHeartbeat(WakeReport& report);
  void wakeResults(WakeReport& report);
  void wakeJobs(WakeReport& report);
  void wakeObservations(WakeReport& report);
  // The end of a wake: the kept results and the waiting adverts into the store as one image, if
  // there is a store and the image would differ from what it holds (task p1.6-22).
  void keepAcrossSleep(WakeReport& report);
  // The most bytes an image may be here: the smaller of the scratch and the store's capacity.
  size_t imageCap() const;
  // The checksum an image of nothing - with the counters as they stand - would have: what the
  // store "holds" when it holds no image at all, so that a wake with nothing kept writes none.
  uint32_t emptyImageCrc();
  // How many jobs this wake may ask the claim for: never more than kClaimMax, and never more
  // than the intent's transaction cap can hold at kTransactionsPerJobAtLeast apiece.
  int claimMax() const;
  // About how long a job will take: its transactions at the budget's pace, with one reply wait
  // each. The pace is what dominates - twenty-one transactions at two a minute is eleven
  // minutes and the replies are seconds - so an estimate that is right about the pace is right
  // about the job.
  uint32_t estimateJobMs(const ClaimedJob& job) const;
  // What is left of the wake's seconds, of the uplink's, and of the transaction cap. Each is
  // the whole of it when the budget set no bound.
  uint32_t wakeMsLeft() const;
  bool uplinkTimeLeft() const;
  int transactionsLeft() const;
  // Acts on what the answer means (ServerPolicy.h) and says which action that was.
  ServerAction noteFailure(const IUplink::Call& call);
  // The link up and a time that came off the network: the ceiling the backoff doubles to
  // (ServerPolicy.h's maxBackoffMs) is the lower one when this is true.
  bool linkProven() const;
  void succeeded();
  // `maxItems` is the ceiling on how many of the ring's entries go into this batch; a wake with
  // a byte cap comes back down through it until the body fits.
  size_t buildBatch(uint32_t nowMs, char* out, size_t cap, size_t maxItems = kObservationBatchSize);
  uint32_t epochOf(uint32_t heardAtMillis, uint32_t nowMs) const;
  // The platform's millisecond clock, under the name every line of AgentLoop.cpp already used
  // when it was Arduino's.
  uint32_t millis() const { return platform_->millis(); }

  IUplink* server_ = nullptr;
  IPlatform* platform_ = nullptr;
  IClock* clock_ = nullptr;  // platform_->clock(): read for the timestamps, offered the server's time
  IMesh* mesh_ = nullptr;
  JobRunner* jobs_ = nullptr;
  const HeartbeatIdentity* identity_ = nullptr;

  ObservationRing ring_;
  Counters counters_;
  const char* publicKey_ = nullptr;
  // The heartbeat's `jobTypes`: the thirteen the mesh carries (IMesh::supports), then the
  // extension's (IJobExtension.h).
  const char* jobTypes_[kJobTypeCount + kMaxExtensionTypes] = {};

  bool halted_ = false;         // a 401: nothing but the console from here
  bool inHeartbeat_ = false;    // a heartbeat is on the wire: the job pump may not start another
  bool configDue_ = false;
  uint32_t configAt_ = 0;
  bool heartbeatScheduled_ = false;
  uint32_t nextHeartbeatAt_ = 0;
  uint32_t holdUntil_ = 0;      // a Retry-After the answer named, or the backoff after a failure
  uint32_t backoffMs_ = 0;
  uint32_t firstFailureAtMillis_ = 0;  // the start of the present run of unanswered calls

  AgentBudget budget_;          // AgentBudget.Default until the server says otherwise (begin())
  int pollIntervalS_ = 30;
  bool lastJobsWaiting_ = false;
  bool claimDue_ = false;

  uint32_t lastHeartbeatAtMillis_ = 0;
  uint32_t lastHeartbeatAtEpoch_ = 0;
  bool everHeartbeated_ = false;
  uint32_t lastBatchAtEpoch_ = 0;
  int lastBatchCount_ = 0;

  // A batch keeps its client id across a retry: that id is what the server de-duplicates on.
  char batchId_[40] = {0};
  size_t batchCount_ = 0;

  // ---- one wake (task p1.5-18) ----------------------------------------------------------------
  // The intent is a member because the heartbeat is built in one place for both ways of driving
  // the loop: `wake()` sets it and `loop()` never does, so a board that only ever calls `loop()`
  // proposes nothing and its heartbeat bytes are what they always were.
  WakeIntent intent_;
  // The budget itself is not kept here: `wake()` hands it straight to the ledger below, which is
  // what every bound of it is read off (and the transaction cap is the intent's).
  bool inWake_ = false;             // the reboot guard is told, not taken; a BackOff returns
  uint32_t wakeStartedAtMs_ = 0;
  uint32_t transactionsAtWakeStart_ = 0;  // the gate's count when the wake began
  // What this wake spends at the server - its seconds and its bytes - and the one place every
  // call of the library is made from, this object's and the runner's alike (agent/UplinkMeter.h,
  // task p1.6-21). `begin()` lends it to the runner; `loop()` uses it with no bounds set.
  UplinkMeter meter_;
  WakeReport* wakeReport_ = nullptr;  // the report being filled, for noteFailure's reboot line
  // The server time the last heartbeat reply carried, 0 when none has.
  uint32_t lastServerTimeUnix_ = 0;

  // ---- what a sleep keeps (task p1.6-22) ------------------------------------------------------
  // The store and the scratch the caller lent (setRetained), or null: the Heltec's are null and
  // every branch below is skipped on it.
  IRetained* retained_ = nullptr;
  uint8_t* imageScratch_ = nullptr;
  size_t imageScratchLen_ = 0;
  // What the store holds, as far as this boot knows: the checksum of the image last read or
  // written, so that a wake whose image would be the same writes nothing. Not known until
  // `restoreKept()` has looked, or a write has succeeded; a write that failed makes it unknown
  // again, and an unknown store is written at the end of the next wake whatever it would hold.
  bool imageKnown_ = false;
  uint32_t lastImageCrc_ = 0;
  bool keptRestored_ = false;    // `restoreKept()` has run this boot: a second call reads nothing
  uint32_t imagesRefused_ = 0;   // images refused whole, this boot's and the image's own count
};

}  // namespace mm
