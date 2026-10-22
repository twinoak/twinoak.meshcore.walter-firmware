#include <agent/AgentLoop.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <agent/Backoff.h>
#include <agent/Iso8601.h>
#include <agent/KeptImage.h>
#include <agent/Log.h>

namespace mm {

namespace {

// A batch of twenty, each item about 290 bytes at the longest name and a three-hop path.
constexpr size_t kBatchJsonCap = 8192;
constexpr int kMaxHopsReported = 8;   // kMaxObservationPathBytes at the one-byte hash
constexpr int kHopHexCap = 7;         // three bytes as hex, and a NUL

// The scratch a batch is written from. Static, not automatic: the Arduino loop task has 8 KB of
// stack and this is more than that on its own.
char g_batchJson[kBatchJsonCap];
ObservationItem g_items[kObservationBatchSize];
char g_keys[kObservationBatchSize][kPublicKeyLen * 2 + 1];
char g_hops[kObservationBatchSize][kMaxHopsReported][kHopHexCap];
const char* g_hopPtrs[kObservationBatchSize][kMaxHopsReported];

const char kHex[] = "0123456789abcdef";

void toHex(const uint8_t* bytes, size_t len, char* out) {
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

bool elapsed(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

}  // namespace

const char* stepOutcomeName(StepOutcome outcome) {
  switch (outcome) {
    case StepOutcome::Done: return "done";
    case StepOutcome::Failed: return "failed";
    case StepOutcome::NoTime: return "no time";
    case StepOutcome::NoBudget: return "no budget";
    case StepOutcome::Skipped:
    default: return "skipped";
  }
}

int transactionsOf(const ClaimedJob& job) {
  // `items` is what the type is as long as: a config.read's named key list, a config.apply's
  // command count, and nothing for the rest - a neighbour list of unknown length is priced at
  // one page, as transactionsFor() prices it (the server, which has the last snapshot, prices
  // three; neither of us can know from the claim).
  int items = -1;
  if (std::strcmp(job.type, "config.read") == 0) {
    items = job.hasConfigKeys ? job.configKeyCount : -1;
  } else if (std::strcmp(job.type, "config.apply") == 0) {
    items = job.apply.commandCount;
  }
  int n = transactionsFor(job.type, job.credentialClass, items);
  // A status read the claim said needs no version spends nothing on it (meshmaster.dk's p4.3-68).
  if (std::strcmp(job.type, "status") == 0 && !readsFirmwareVersion(job)) n = 1;
  // The login every class but `none` needs, and the restore an admin session on a repeater that
  // holds a grant for this exit node ends with (task p4.1-60). The restore is counted wherever
  // there is a grant, which over-states a guest-class job by one: an estimate a battery spends
  // against may be too high and must never be too low.
  if (job.credentialClass != CredentialClass::None) n += kTransactionsForLogin;
  if (job.hasAclLevel && std::strcmp(job.type, "acl.set") != 0 && std::strcmp(job.type, "reboot") != 0) {
    n += kTransactionsForAclRestore;
  }
  return n;
}

void AgentLoop::begin(IUplink& server, IPlatform& platform, IMesh& mesh, JobRunner& jobs,
                      const HeartbeatIdentity& identity) {
  server_ = &server;
  platform_ = &platform;
  clock_ = &platform.clock();
  mesh_ = &mesh;
  jobs_ = &jobs;
  identity_ = &identity;
  mesh_->setRing(&ring_);
  // One ledger for the whole library's calls to the server: this object's and the runner's
  // (agent/UplinkMeter.h, task p1.6-21). It has no bounds until a wake sets them, so `loop()`
  // is what it always was and the Heltec posts the bytes it always posted.
  meter_.begin(platform);
  jobs.setMeter(&meter_);
  budget_.maxTransactionsPerMinute = kDefaultMaxTransactionsPerMinute;
  budget_.minGapSeconds = kDefaultMinGapSeconds;
  jobs_->setBudget(budget_);
}

void AgentLoop::setIdentity(const char* publicKeyHex) {
  publicKey_ = publicKeyHex;
  jobs_->setIdentity(publicKeyHex);
}

void AgentLoop::onLinkUp() {
  // The first link is not a reconnection; every one after it is.
  const IUplink::LinkState link = server_->link();
  counters_.wifiReconnects = link.connects > 0 ? link.connects - 1 : 0;
  configDue_ = true;
  configAt_ = millis() + 1500;  // a moment for DNS and SNTP to get going
  heartbeatScheduled_ = false;
  holdUntil_ = millis();
  backoffMs_ = 0;
}

void AgentLoop::reconfigure() {
  if (halted_) LOG_I("server: the settings changed; the loop starts again");
  halted_ = false;
  backoffMs_ = 0;
  holdUntil_ = millis();
  configDue_ = server_ != nullptr && server_->link().up;
  configAt_ = millis();
  heartbeatScheduled_ = false;
}

void AgentLoop::loop() {
  if (server_ == nullptr || halted_ || !server_->configured() || !server_->link().up) return;
  const uint32_t now = millis();
  if (!elapsed(now, holdUntil_)) return;

  if (configDue_) {
    if (!elapsed(now, configAt_)) return;
    doConfig();
    return;
  }
  if (!heartbeatScheduled_) return;
  if (elapsed(now, nextHeartbeatAt_)) {
    doHeartbeat();
    return;
  }
  // The offline buffer before anything new: what waited is posted first, in order, with the
  // timestamps and the client ids it was made with (EXIT-NODES §5).
  if (jobs_->queueDepth() > 0) {
    doPendingResults();
    return;
  }
  // A claim before the batch: the server said work was waiting, and a job on the air is what
  // this exit node is for. One claim per heartbeat that said so.
  if (claimDue_) {
    doJobs();
    return;
  }
  // The oldest pending entry carries the moment it was heard, so how long a batch has waited is
  // a fact about the ring and needs no timer of its own.
  const uint32_t waited = ring_.empty() ? 0 : now - ring_.at(0).heardAtMillis;
  if (observationBatchDue(ring_.size(), waited)) {
    doObservations();
  }
}

bool AgentLoop::doConfig() {
  configDue_ = false;
  if (meter_.spend(0, [&] { return server_->fetchConfig(); })) {
    budget_ = server_->config().budget;
    pollIntervalS_ = server_->config().pollIntervalS > 0 ? server_->config().pollIntervalS : 30;
  } else {
    // The config call is a startup nicety - the heartbeat answers the budget, the poll interval
    // and the clock every time - so a failure here is noted and the loop starts anyway, at
    // whatever pace the answer earned. A 401 is the exception: there is no loop to start.
    if (noteFailure(server_->lastCall()) == ServerAction::Halt) return false;
    LOG_W("server: the startup config call did not answer; the heartbeat asks the same things every %d s",
          pollIntervalS_);
    heartbeatScheduled_ = true;
    nextHeartbeatAt_ = millis();
    return false;
  }
  heartbeatScheduled_ = true;
  nextHeartbeatAt_ = millis();  // the first heartbeat goes as soon as the hold, if any, is over
  return true;
}

void AgentLoop::buildHeartbeat(Heartbeat& hb) {
  hb.agentVersion = identity_->agentVersion;
  hb.transport = identity_->transport;  // the identity's word: onboard by default (task p1.6-23)
  hb.at = clock_->now();
  hb.companionPublicKey = publicKey_;              // nullptr while the radio has made no identity
  hb.companionFirmware = publicKey_ == nullptr ? nullptr : identity_->companionFirmware;
  hb.hasRadio = publicKey_ != nullptr;
  if (hb.hasRadio) hb.radio = identity_->radio;
  // The thirteen this firmware has a handler for - every type of API §3 since task 05 - less
  // any the mesh says it cannot carry (IMesh::supports, task p1.6-23: a link that cannot carry a
  // trace), and after them the types of the extension the board compiled in, if any
  // (IJobExtension.h): routing requires the type advertised, so this list is what the server may
  // send and nothing else (task p3-53). The Heltec's mesh carries all thirteen and it sets no
  // extension, so it sends the thirteen.
  int count = 0;
  for (int i = 0; i < kJobTypeCount; i++) {
    if (mesh_->supports(kJobTypes[i])) jobTypes_[count++] = kJobTypes[i];
  }
  if (const IJobExtension* extension = jobs_->extension()) {
    const int extra = extension->typeCount() < kMaxExtensionTypes ? extension->typeCount() : kMaxExtensionTypes;
    for (int i = 0; i < extra; i++) jobTypes_[count++] = extension->type(i);
  }
  hb.hasJobTypes = true;
  hb.jobTypes = jobTypes_;
  hb.jobTypeCount = count;
  hb.hasBudget = true;
  hb.budget = budget_;
  // The radio is the companion here: "connected" is "the SX1262 came up", and a board whose
  // radio did not is an exit node that hears nothing, which is what the alert of §10 is for.
  hb.companionConnected = publicKey_ != nullptr;
  hb.queueDepth = jobs_->queueDepth();
  hb.board = identity_->board;
  // The two of an agent that paces itself (EXIT-NODES §6). `loop()` sets no intent, so both are
  // 0 here and neither field is written: the Heltec's heartbeat is byte for byte what it was.
  hb.hasNextCheckInS = intent_.nextCheckInS > 0;
  hb.nextCheckInS = static_cast<int>(intent_.nextCheckInS);
  hb.hasMaxTransactionsThisWake = intent_.maxTransactionsThisWake > 0;
  hb.maxTransactionsThisWake = intent_.maxTransactionsThisWake;
}

// The board's own link, proven: the link up *and* a time that came off the network. An RTC that
// held a plausible time across a software reset proves nothing about the network, so it is not
// enough; SNTP having answered, or the server's own time having been taken, means a packet of
// ours left this board and something answered it (task p1.2-10; IClock::fromNetwork).
bool AgentLoop::linkProven() const {
  if (server_ == nullptr || clock_ == nullptr || !server_->link().up) return false;
  return clock_->fromNetwork();
}

ServerAction AgentLoop::noteFailure(const IUplink::Call& call) {
  const ServerAction action = actionFor(call.reachable, call.httpStatus, call.retryAfterS);
  switch (action) {
    case ServerAction::Halt:
      halted_ = true;
      LOG_E("server: the token was refused (http 401): revoked, or wrong. Nothing more is sent until a person "
            "fixes it - `set meshmaster.token <token>` on this console.");
      break;
    case ServerAction::WaitRetryAfter: {
      // A 429, or the 502/503/504 of a deploy that said how long it would be: the wait is the
      // answer's own, to the second. The backoff does not double - a server that answered and
      // named a time has not gone unanswered - so this is neither a consecutive failure nor one
      // of the twelve the reboot guard counts. `httpFailures` is "a call that did not come back
      // 2xx, 401 and 429 apart", so a 5xx still shows up in the heartbeat's counters.
      const uint32_t wait = retryAfterMs(call.retryAfterS);
      holdUntil_ = millis() + wait;
      if (call.httpStatus != 429) counters_.httpFailures++;
      LOG_W("server: http %d; the wait is Retry-After's %lu s", call.httpStatus,
            static_cast<unsigned long>(wait / 1000));
      break;
    }
    case ServerAction::Discard:
      counters_.httpFailures++;
      LOG_W("server: http %d; this request will not get better by being sent again", call.httpStatus);
      break;
    case ServerAction::BackOff:
    default: {
      counters_.httpFailures++;
      counters_.consecutiveFailures++;
      if (counters_.consecutiveFailures == 1) firstFailureAtMillis_ = millis();
      // The sequence is 30 s, 60 s, 120 s ... up to its ceiling; what is waited is that plus or
      // minus a quarter, so two boards that lost the server together drift apart
      // (agent/Backoff.h). The ceiling is two minutes once this board's own link is proven and ten
      // while it is not: a server that is back deserves a call within minutes, and a WiFi that
      // associates and goes nowhere deserves the long wait it always had (task p1.2-10).
      backoffMs_ = nextBackoffMs(backoffMs_, linkProven());
      const uint32_t wait = withJitter(backoffMs_, platform_->random());
      holdUntil_ = millis() + wait;
      LOG_I("server: next attempt in %lu s (the %lu s step, jittered)", static_cast<unsigned long>(wait / 1000),
            static_cast<unsigned long>(backoffMs_ / 1000));
      // A link that is up and a server that has not answered for an hour and a half is a stack
      // that is wedged, not a server that is busy: the board restarts itself, having said why.
      // Both the count *and* the hour and a half, since task p1.2-10 brought the proven link's
      // ceiling down to two minutes: twelve failures can now be twenty minutes apart end to end,
      // and twenty minutes of a server being away is a deploy (src/Health.h).
      const uint32_t failingForMs = millis() - firstFailureAtMillis_;
      if (counters_.consecutiveFailures >= kRebootAfterFailures && failingForMs >= kRebootAfterFailingMs &&
          server_->link().up) {
        char why[64];
        // 41 characters and two counts of at most ten digits each: inside Health.h's
        // kMaxRebootWhyLen (63), which -Werror=format-truncation is what checks.
        std::snprintf(why, sizeof why, "%lu server calls unanswered in %lu min, wifi up",
                      static_cast<unsigned long>(counters_.consecutiveFailures),
                      static_cast<unsigned long>(failingForMs / 60000));
        // In one-wake mode the guard is **told and not taken** (task p1.5-18): a device that
        // sleeps has a supervisor of its own and its own idea of what a restart costs, and this
        // loop is one step of its wake and not the thing in charge of the board. The report says
        // the platform would have been asked.
        if (inWake_) {
          if (wakeReport_ != nullptr) {
            wakeReport_->rebootWanted = true;
            std::snprintf(wakeReport_->rebootWhy, sizeof wakeReport_->rebootWhy, "%s", why);
          }
          LOG_W("server: the platform would have been asked to reboot (%s); in one-wake mode it is not", why);
        } else {
          platform_->rebootForHealth(why);
        }
      }
      break;
    }
  }
  return action;
}

void AgentLoop::succeeded() {
  backoffMs_ = 0;
  counters_.consecutiveFailures = 0;
  holdUntil_ = millis();
}

void AgentLoop::heartbeatIfOverdue() {
  if (halted_ || inHeartbeat_ || server_ == nullptr || !server_->configured() || !heartbeatScheduled_) return;
  if (!server_->link().up) return;
  const uint32_t now = millis();
  if (!elapsed(now, holdUntil_) || !elapsed(now, nextHeartbeatAt_)) return;
  LOG_D("server: the heartbeat is due inside a job; sending it from the job's own wait");
  doHeartbeat();
}

bool AgentLoop::doHeartbeat() {
  inHeartbeat_ = true;
  Heartbeat hb;
  buildHeartbeat(hb);
  const ErrorCounter errors[] = {
      {"overheard", mesh_->overheard()},
      {"wifiReconnects", counters_.wifiReconnects},
      {"httpFailures", counters_.httpFailures},
      {"observationsDropped", ring_.dropped()},
      // Task 06's two: what the RAM buffers had to throw away while the server was gone, and
      // how many times this board restarted itself to clear a wedged stack (src/Health.h). The
      // reboot count is in RTC memory, so it survives the restart it counts.
      {"resultsDropped", jobs_->resultsDropped()},
      {"reboots", platform_->selfReboots()},
  };
  hb.errors = errors;
  hb.errorCount = static_cast<int>(sizeof errors / sizeof errors[0]);

  HeartbeatReply reply;
  const bool ok = meter_.spend(0, [&] { return server_->postHeartbeat(hb, reply); });
  inHeartbeat_ = false;
  // Whatever happened, the next one is due an interval from now: a failure backs off on top.
  nextHeartbeatAt_ = millis() + static_cast<uint32_t>(pollIntervalS_) * 1000u;
  if (!ok) {
    noteFailure(server_->lastCall());
    return false;
  }

  counters_.heartbeats++;
  lastHeartbeatAtMillis_ = millis();
  everHeartbeated_ = true;
  lastServerTimeUnix_ = reply.serverTimeUnix;
  int32_t difference = 0;
  const bool moved = clock_->takeServerTime(reply.serverTimeUnix, difference);
  lastHeartbeatAtEpoch_ = clock_->now();
  if (moved) LOG_W("clock: corrected by %ld s from the server's time", static_cast<long>(difference));

  if (reply.budget.maxTransactionsPerMinute != budget_.maxTransactionsPerMinute ||
      reply.budget.minGapSeconds != budget_.minGapSeconds) {
    budget_ = reply.budget;
    jobs_->setBudget(budget_);
    LOG_I("server: budget in force: %d/min, gap %d s", budget_.maxTransactionsPerMinute, budget_.minGapSeconds);
  }
  if (reply.pollIntervalS > 0 && reply.pollIntervalS != pollIntervalS_) {
    pollIntervalS_ = reply.pollIntervalS;
    LOG_I("server: poll interval %d s", pollIntervalS_);
  }
  nextHeartbeatAt_ = millis() + static_cast<uint32_t>(pollIntervalS_) * 1000u;
  lastJobsWaiting_ = reply.jobsWaiting;
  if (reply.jobsWaiting) claimDue_ = true;
  LOG_D("server: heartbeat reply: serverTime %s, budget %d/min gap %d s, poll %d s, jobs waiting %s",
        reply.serverTime, reply.budget.maxTransactionsPerMinute, reply.budget.minGapSeconds, reply.pollIntervalS,
        reply.jobsWaiting ? "yes" : "no");
  succeeded();
  return true;
}

void AgentLoop::doPendingResults() {
  const int posted = jobs_->flushResults();
  const int after = jobs_->queueDepth();
  if (posted > 0) {
    counters_.resultsPosted += static_cast<uint32_t>(posted);
    LOG_I("jobs: %d result(s) that waited went through; %d still waiting", posted, after);
  }
  if (after == 0) {
    succeeded();
    return;
  }
  // Still there: the same backoff the heartbeat uses, so the buffer is not hammered.
  noteFailure(server_->lastCall());
}

uint32_t AgentLoop::epochOf(uint32_t heardAtMillis, uint32_t nowMs) const {
  const uint32_t now = clock_->now();
  const uint32_t agoS = (nowMs - heardAtMillis + 500u) / 1000u;
  return agoS < now ? now - agoS : now;
}

size_t AgentLoop::buildBatch(uint32_t nowMs, char* out, size_t cap, size_t maxItems) {
  // A batch that is being retried is the same batch: the same items and the same client id, or
  // the server's de-duplication would take the id, store nothing, and the extra items would be
  // consumed unsent.
  size_t count = batchCount_ > 0 ? batchCount_ : ring_.size();
  if (count > kObservationBatchSize) count = kObservationBatchSize;
  if (count > maxItems) count = maxItems;
  if (count > ring_.size()) count = ring_.size();
  for (size_t i = 0; i < count; i++) {
    const HeardAdvert& a = ring_.at(i);
    ObservationItem& item = g_items[i];
    item = ObservationItem();
    // An advert restored from a kept image after a deep sleep carries the moment it was heard
    // (task p1.6-22); every other one is stamped from the uptime clock, as ever.
    item.at = a.heardAtEpoch != 0 ? a.heardAtEpoch : epochOf(a.heardAtMillis, nowMs);
    item.kind = "advert";
    toHex(a.publicKey, kPublicKeyLen, g_keys[i]);
    item.publicKey = g_keys[i];

    const int size = a.pathHashSize == 0 ? 1 : a.pathHashSize;
    int reported = a.pathByteLen / size;
    if (reported > kMaxHopsReported) reported = kMaxHopsReported;
    for (int h = 0; h < reported; h++) {
      toHex(&a.pathBytes[h * size], static_cast<size_t>(size), g_hops[i][h]);
      g_hopPtrs[i][h] = g_hops[i][h];
    }
    // The path goes only when all of it is there; the hop count is what the packet said either
    // way, so a longer path than this ring keeps is still reported as its length.
    item.hasPath = reported == a.hops;
    item.path = g_hopPtrs[i];
    item.pathCount = reported;
    item.hasHops = true;
    item.hops = a.hops;

    if (a.snrQ4 != kNoSnrQ4) {
      item.hasSnr = true;
      item.snr = static_cast<double>(a.snrQ4) / 4.0;
    }
    if (a.rssi != kNoRssi) {
      item.hasRssi = true;
      item.rssi = a.rssi;
    }

    const char* role = roleName(a.advType);
    if (a.name[0] != '\0' || a.hasPosition || role != nullptr) {
      item.hasFields = true;
      item.fields.name = a.name[0] != '\0' ? a.name : nullptr;
      item.fields.hasPosition = a.hasPosition;
      item.fields.latE6 = a.latE6;
      item.fields.lonE6 = a.lonE6;
      item.fields.role = role;
    }
  }
  batchCount_ = count;
  if (count == 0) return 0;
  if (batchId_[0] == '\0') {
    // "obs-" and 32 hex, the shape the .NET observer's Guid gives its batches.
    std::snprintf(batchId_, sizeof batchId_, "obs-%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
                  platform_->random(), platform_->random(), platform_->random(), platform_->random());
  }
  return writeObservationBatch(batchId_, g_items, static_cast<int>(count), out, cap);
}

bool AgentLoop::doObservations(bool* capped) {
  const uint32_t now = millis();
  // A batch already built and not yet accepted is pending: it must go out again exactly as it
  // was, under the client id it was built with, or the server would de-duplicate on that id and
  // the items this one does not carry would be consumed unsent.
  const bool retrying = batchCount_ > 0;
  size_t len = buildBatch(now, g_batchJson, sizeof g_batchJson);
  // A wake on a metered uplink says how many bytes it may send, and a batch goes whole: the
  // length is only known once the JSON is written, so a **fresh** batch's count comes down by
  // half until the body fits. What is left stays in the ring with the moment each entry was
  // heard, which is what the next wake posts (the ring's own rule, Observations.h).
  if (!retrying) {
    while (len > meter_.bytesLeft() && batchCount_ > 1) {
      const size_t fewer = batchCount_ / 2;
      batchCount_ = 0;
      len = buildBatch(now, g_batchJson, sizeof g_batchJson, fewer);
    }
  }
  if (len > meter_.bytesLeft()) {
    LOG_I("observations: the shortest batch is %u bytes and this wake has %u left; the ring keeps them",
          static_cast<unsigned>(len), static_cast<unsigned>(meter_.bytesLeft()));
    // Nothing went out under this id, so a batch that was built here is forgotten whole: the next
    // wake builds a full one again rather than inheriting a count a byte cap shortened.
    if (!retrying) {
      batchCount_ = 0;
      batchId_[0] = '\0';
    }
    if (capped != nullptr) *capped = true;
    return false;
  }
  if (len == 0) {
    if (batchCount_ > 0) {
      LOG_E("observations: %u of them did not fit in %u bytes and were dropped",
            static_cast<unsigned>(batchCount_), static_cast<unsigned>(sizeof g_batchJson));
      ring_.consume(batchCount_);
    }
    batchCount_ = 0;
    batchId_[0] = '\0';
    return false;
  }

  int accepted = 0;
  bool duplicate = false;
  const bool ok =
      meter_.spend(len, [&] { return server_->postObservations(g_batchJson, len, accepted, duplicate); });
  if (ok) {
    counters_.batches++;
    counters_.observationsPosted += static_cast<uint32_t>(batchCount_);
    lastBatchAtEpoch_ = clock_->now();
    lastBatchCount_ = static_cast<int>(batchCount_);
    LOG_I("observations: %u posted, %d accepted%s", static_cast<unsigned>(batchCount_), accepted,
          duplicate ? " (the server had this batch already)" : "");
    ring_.consume(batchCount_);
    batchCount_ = 0;
    batchId_[0] = '\0';
    succeeded();
    return true;
  }

  if (noteFailure(server_->lastCall()) == ServerAction::Discard) {
    counters_.observationsRefused++;
    LOG_W("observations: the server refused the batch for good; %u dropped", static_cast<unsigned>(batchCount_));
    ring_.consume(batchCount_);
    batchCount_ = 0;
    batchId_[0] = '\0';
    succeeded();
    return false;
  }
  // Anything else: the batch is kept with its client id for the next attempt, and the server
  // de-duplicates on that id - so a batch it took whose answer was lost costs nothing twice.
  return false;
}

void AgentLoop::doJobs() {
  // One claim per heartbeat that said work was waiting. Whatever it hands over is run here and
  // now, one transaction at a time at the budget's pace, and every result is posted as its job
  // ends - which is why this call can take minutes at a budget of two a minute, and why the
  // console and the OLED are pumped through it (JobRunner::idle).
  claimDue_ = false;
  const int ran = jobs_->claimAndRun();
  if (ran < 0) {
    counters_.claimFailures++;
    noteFailure(server_->lastCall());
    return;
  }
  succeeded();
}


// ---- one wake (task p1.5-18) -------------------------------------------------------------------
//
// The same steps as `loop()`, in the same order, inside one bounded slice of time and energy.
// Every bound below is read through these four, so a budget that set none is a wake that does as
// much as it can - and every one of them is measured on the platform's millisecond clock, which
// in a test is the clock the test moves (support/FakePlatform.h).

uint32_t AgentLoop::wakeMsLeft() const { return meter_.wakeMsLeft(); }

bool AgentLoop::uplinkTimeLeft() const { return meter_.timeLeft(); }

int AgentLoop::transactionsLeft() const {
  if (intent_.maxTransactionsThisWake <= 0) return 0x7FFFFFFF;
  const int spent = static_cast<int>(jobs_->gate().transmissions() - transactionsAtWakeStart_);
  return intent_.maxTransactionsThisWake > spent ? intent_.maxTransactionsThisWake - spent : 0;
}

int AgentLoop::claimMax() const {
  const int left = transactionsLeft();
  if (left >= kClaimMax * kTransactionsPerJobAtLeast) return kClaimMax;
  return left / kTransactionsPerJobAtLeast;
}

uint32_t AgentLoop::estimateJobMs(const ClaimedJob& job) const {
  const int n = transactionsOf(job);
  const uint32_t gapMs = static_cast<uint32_t>(budget_.minGapSeconds > 0 ? budget_.minGapSeconds : 0) * 1000u;
  const uint32_t replyMs = static_cast<uint32_t>(job.loginMs > 0 ? job.loginMs : 8000);
  return static_cast<uint32_t>(n > 0 ? n : 1) * (gapMs + replyMs);
}

void AgentLoop::wakeConfig(WakeReport& report) {
  // "If stale" is "if this uplink has none": a device that deep-sleeps loses the object with its
  // RAM, so the call happens once a boot and not once a wake - and `onLinkUp()` or
  // `reconfigure()` makes it due again, as they always did.
  if (!configDue_ && server_->hasConfig()) return;
  if (!uplinkTimeLeft()) {
    report.config = StepOutcome::NoTime;
    return;
  }
  report.config = doConfig() ? StepOutcome::Done : StepOutcome::Failed;
}

void AgentLoop::wakeHeartbeat(WakeReport& report) {
  if (!uplinkTimeLeft()) {
    report.heartbeat = StepOutcome::NoTime;
    return;
  }
  // The schedule exists for the pump: a job that outlasts the accepted interval sends the
  // heartbeat that fell due from inside its own wait (heartbeatIfOverdue), exactly as on the
  // board. At an interval of an hour that never happens, which is the point of proposing one.
  heartbeatScheduled_ = true;
  const bool ok = doHeartbeat();
  report.heartbeat = ok ? StepOutcome::Done : StepOutcome::Failed;
  if (ok) report.serverTime = lastServerTimeUnix_;
}

void AgentLoop::wakeResults(WakeReport& report) {
  if (jobs_->queueDepth() == 0) return;
  if (!uplinkTimeLeft()) {
    report.results = StepOutcome::NoTime;
    return;
  }
  const uint32_t failuresBefore = jobs_->postFailures();
  const int posted = jobs_->flushResults();
  report.resultsFlushed = posted;
  counters_.resultsPosted += static_cast<uint32_t>(posted);
  if (posted > 0) LOG_I("jobs: %d result(s) that waited went through", posted);
  if (jobs_->postFailures() > failuresBefore) {
    // The server, not the budget: the same backoff the heartbeat uses, so the buffer is not
    // hammered - and in one wake that backoff is a wait the caller is told to sleep through.
    report.results = StepOutcome::Failed;
    noteFailure(server_->lastCall());
  } else if (jobs_->queueDepth() > 0) {
    report.results = StepOutcome::NoBudget;
  } else {
    report.results = StepOutcome::Done;
    succeeded();
  }
}

void AgentLoop::wakeJobs(WakeReport& report) {
  claimDue_ = false;
  if (!lastJobsWaiting_) return;
  if (!uplinkTimeLeft()) {
    report.claim = StepOutcome::NoTime;
    return;
  }
  const int max = claimMax();
  if (max < 1) {
    report.claim = StepOutcome::NoBudget;
    LOG_I("jobs: this wake has %d transaction(s) left of its %d, which is less than a job: no claim is made",
          transactionsLeft(), intent_.maxTransactionsThisWake);
    return;
  }
  const int count = jobs_->claim(max);
  if (count < 0) {
    counters_.claimFailures++;
    report.claim = StepOutcome::Failed;
    noteFailure(server_->lastCall());
    return;
  }
  report.claim = StepOutcome::Done;
  report.jobsClaimed = count;
  succeeded();
  if (count == 0) {
    jobs_->endClaim();
    return;
  }

  for (int i = 0; i < count; i++) {
    const ClaimedJob& job = jobs_->claimed(i);
    const int price = transactionsOf(job);
    const int left = transactionsLeft();
    const uint32_t needMs = estimateJobMs(job);
    const uint32_t leftMs = wakeMsLeft();
    char why[128];
    // A job this wake cannot finish is reported `failed` with *wake budget* and nothing is sent
    // for it: what must never happen is a job claimed, leased, and silent (EXIT-NODES §6 as
    // built in meshmaster.dk's task 121 - what is left behind is queued and bound to nothing,
    // and a job the server did hand over is this agent's to answer). Its result goes through the
    // same budget as every other post (task p1.6-21), and when that has run out the result is
    // **kept** in the offline buffer rather than posted past the cap: "offline is normal" and a
    // late result is still taken (EXIT-NODES §5), which is the way of answering a lease that
    // costs no byte this wake has not got.
    if (price > left) {
      std::snprintf(why, sizeof why, "wake budget: %d transaction(s) left of this wake's %d and %s needs about %d",
                    left, intent_.maxTransactionsThisWake, job.type, price);
      jobs_->reportNotRun(i, why);
      report.jobsNotRun++;
      report.jobs = StepOutcome::NoBudget;
      continue;
    }
    if (needMs > leftMs) {
      std::snprintf(why, sizeof why, "wake budget: about %lu s left of this wake and %s needs about %lu s",
                    static_cast<unsigned long>(leftMs / 1000u), job.type,
                    static_cast<unsigned long>(needMs / 1000u));
      jobs_->reportNotRun(i, why);
      report.jobsNotRun++;
      report.jobs = StepOutcome::NoTime;
      continue;
    }
    jobs_->runClaimed(i);
    report.jobsRun++;
    if (report.jobs == StepOutcome::Skipped) report.jobs = StepOutcome::Done;
  }
  jobs_->endClaim();
}

void AgentLoop::wakeObservations(WakeReport& report) {
  // The ring, oldest first, in as many batches as the byte cap and the clock allow. What does
  // not go stays with the moment each advert was heard, which is the next wake's to post
  // (Observations.h: the ring keeps `heardAtMillis` and the batch turns it into a timestamp).
  while (!ring_.empty()) {
    if (!uplinkTimeLeft()) {
      report.observations = StepOutcome::NoTime;
      break;
    }
    const size_t before = ring_.size();
    bool capped = false;
    const bool ok = doObservations(&capped);
    if (!ok) {
      report.observations = capped ? StepOutcome::NoBudget : StepOutcome::Failed;
      break;
    }
    report.batchesPosted++;
    report.observationsPosted += static_cast<int>(before - ring_.size());
    report.observations = StepOutcome::Done;
  }
}

WakeReport AgentLoop::wake(const WakeIntent& intent, const Budget& budget) {
  WakeReport report;
  if (server_ == nullptr || platform_ == nullptr || jobs_ == nullptr) {
    report.why = "the loop has not begun";
    return report;
  }

  intent_ = intent;
  inWake_ = true;
  wakeReport_ = &report;
  wakeStartedAtMs_ = millis();
  transactionsAtWakeStart_ = jobs_->gate().transmissions();
  // The three bounds every call of this wake is measured against, the runner's with the loop's:
  // one ledger, one byte cap, one set of uplink seconds (task p1.6-21).
  meter_.beginWake(budget.wakeSeconds, budget.uplinkSeconds, budget.postBytes);
  const uint32_t keptBefore = jobs_->resultsKeptForBudget();

  if (halted_) {
    // A 401 is still a 401 after a sleep: nothing is sent until a person fixes the token.
    report.why = "halted: the server refused the token (http 401)";
  } else if (!server_->configured()) {
    report.why = "not configured: the server's url and a token are needed";
  } else if (!server_->link().up) {
    report.why = "the uplink is down";
  } else if (!elapsed(millis(), holdUntil_)) {
    // A backoff or a Retry-After this agent is still inside, carried across the sleep by the
    // state: the wake sends nothing and the caller is told how much of it is left.
    report.why = "a wait the server asked for is still running";
  } else {
    wakeConfig(report);
    if (!halted_) wakeHeartbeat(report);
    // A heartbeat that did not come back is a server that is away: there is no accepted interval,
    // no `jobsWaiting` to believe and nothing to be gained by putting the rest of the wake on a
    // link that just failed. The backoff is in `sleepAtLeastS` and the next wake honours it.
    if (!halted_ && report.heartbeat == StepOutcome::Done) {
      wakeResults(report);
      wakeJobs(report);
      wakeObservations(report);
    }
  }

  report.halted = halted_;
  report.acceptedIntervalS = pollIntervalS_;
  report.sleepAtLeastS = retrySeconds();
  // Every byte and every millisecond this wake spent at the server, whichever step spent it.
  report.bytesPosted = meter_.bytesPosted();
  report.uplinkMs = meter_.uplinkSpentMs();
  report.resultsKept = static_cast<int>(jobs_->resultsKeptForBudget() - keptBefore);
  report.transactionsSpent = static_cast<int>(jobs_->gate().transmissions() - transactionsAtWakeStart_);
  // What the sleep after this wake keeps: one image into the store, if there is one and it would
  // change what the store holds (task p1.6-22). Before `observationsLeft` is read, because what
  // the store cannot hold is dropped here and is not left in the ring either.
  keepAcrossSleep(report);
  report.observationsLeft = static_cast<int>(ring_.size());
  report.elapsedMs = millis() - wakeStartedAtMs_;
  report.counters = counters_;
  inWake_ = false;
  wakeReport_ = nullptr;
  meter_.endWake();

  LOG_I("wake: config %s, heartbeat %s, results %s, claim %s, jobs %s, observations %s", stepOutcomeName(report.config),
        stepOutcomeName(report.heartbeat), stepOutcomeName(report.results), stepOutcomeName(report.claim),
        stepOutcomeName(report.jobs), stepOutcomeName(report.observations));
  LOG_I("wake: %d job(s) run, %d not run, %d transaction(s), %d batch(es), %u byte(s) posted; %lu ms of %lu at "
        "the server%s; next in %d s%s%s%s",
        report.jobsRun, report.jobsNotRun, report.transactionsSpent, report.batchesPosted,
        static_cast<unsigned>(report.bytesPosted), static_cast<unsigned long>(report.uplinkMs),
        static_cast<unsigned long>(report.elapsedMs),
        report.resultsKept > 0 ? ", a result kept: budget" : "", report.acceptedIntervalS,
        report.sleepAtLeastS > 0 ? ", holding off first" : "",
        report.keptImageWritten ? ", an image kept for the sleep" : "",
        report.why[0] == '\0' ? "" : " - nothing was sent");
  if (report.why[0] != '\0') LOG_I("wake: %s", report.why);
  return report;
}

WakeState AgentLoop::state() const {
  WakeState saved;
  saved.version = WakeState::kVersion;
  saved.backoffMs = backoffMs_;
  saved.consecutiveFailures = counters_.consecutiveFailures;
  const uint32_t now = platform_ == nullptr ? 0 : millis();
  // Durations, not deadlines: millis() starts again from zero after a deep sleep.
  const int32_t left = static_cast<int32_t>(holdUntil_ - now);
  saved.holdForMs = left > 0 ? static_cast<uint32_t>(left) : 0;
  saved.failingForMs = counters_.consecutiveFailures == 0 ? 0 : now - firstFailureAtMillis_;
  saved.pollIntervalS = pollIntervalS_;
  saved.budget = budget_;
  saved.halted = halted_;
  saved.everHeartbeated = everHeartbeated_;
  saved.lastHeartbeatAtEpoch = lastHeartbeatAtEpoch_;
  saved.counters = counters_;
  return saved;
}

bool AgentLoop::restore(const WakeState& saved) {
  if (platform_ == nullptr) return false;
  if (saved.version != WakeState::kVersion) {
    LOG_W("wake: a state of version %lu was handed back and this firmware writes %lu; it is ignored",
          static_cast<unsigned long>(saved.version), static_cast<unsigned long>(WakeState::kVersion));
    return false;
  }
  const uint32_t now = millis();
  counters_ = saved.counters;
  counters_.consecutiveFailures = saved.consecutiveFailures;
  backoffMs_ = saved.backoffMs;
  holdUntil_ = now + saved.holdForMs;
  firstFailureAtMillis_ = now - saved.failingForMs;
  pollIntervalS_ = saved.pollIntervalS > 0 ? saved.pollIntervalS : 30;
  budget_ = saved.budget;
  jobs_->setBudget(budget_);
  halted_ = saved.halted;
  everHeartbeated_ = saved.everHeartbeated;
  lastHeartbeatAtEpoch_ = saved.lastHeartbeatAtEpoch;
  if (saved.holdForMs > 0) {
    LOG_I("wake: %lu s of a wait the server asked for is still owed from before the sleep",
          static_cast<unsigned long>(saved.holdForMs / 1000u));
  }
  return true;
}

// ---- what a sleep keeps (task p1.6-22) -----------------------------------------------------------
//
// The offline buffer and the observation ring are RAM, and a deep sleep is the end of RAM: a
// result a wake kept - for its budget, or because the server would not take it - was lost at the
// sleep, and its job stayed leased until the server's lease lapsed. The WakeState cannot carry
// twelve kilobytes of results through an RTC memory that is nearly full, so the consumer lends a
// store and a scratch, the end of every wake writes one image of what is kept (agent/KeptImage.h:
// results first, oldest first, then the adverts, whole or not at all, checksummed), and the boot
// after the sleep reads it back. The image is what survives, so the buffer and the ring are made to
// agree with it: what it cannot hold is dropped here and counted in the two figures the heartbeat
// already carries, and a loss is a number on the exit node's page and not an accident.

void AgentLoop::setRetained(IRetained* store, uint8_t* scratch, size_t scratchLen) {
  retained_ = nullptr;
  imageScratch_ = nullptr;
  imageScratchLen_ = 0;
  imageKnown_ = false;
  keptRestored_ = false;
  if (store == nullptr) return;
  if (scratch == nullptr || scratchLen < kKeptImageHeaderLen || store->capacity() < kKeptImageHeaderLen) {
    LOG_E("kept: a store of %u bytes and a scratch of %u cannot hold an image's header (%u); nothing is kept",
          static_cast<unsigned>(store->capacity()), static_cast<unsigned>(scratch == nullptr ? 0 : scratchLen),
          static_cast<unsigned>(kKeptImageHeaderLen));
    return;
  }
  retained_ = store;
  imageScratch_ = scratch;
  imageScratchLen_ = scratchLen;
}

size_t AgentLoop::imageCap() const {
  if (retained_ == nullptr) return 0;
  const size_t cap = retained_->capacity();
  return cap < imageScratchLen_ ? cap : imageScratchLen_;
}

uint32_t AgentLoop::emptyImageCrc() {
  KeptImageWriter empty(imageScratch_, imageCap());
  empty.finish(jobs_->results().dropped(), ring_.dropped(), imagesRefused_);
  return empty.crc();
}

bool AgentLoop::restoreKept() {
  if (retained_ == nullptr || platform_ == nullptr || jobs_ == nullptr) return false;
  if (keptRestored_) {
    LOG_W("kept: the image was already read this boot; it is not read twice");
    return false;
  }
  keptRestored_ = true;
  const size_t cap = imageCap();
  const size_t got = retained_->read(imageScratch_, cap);
  if (got == 0) {
    // Nothing there - a first boot, an erased region. What the store "holds" is an image of
    // nothing, so a wake that keeps nothing will not write one.
    imageKnown_ = true;
    lastImageCrc_ = emptyImageCrc();
    LOG_I("kept: the store holds no image");
    return false;
  }
  KeptImageReader image;
  const char* refused = image.check(imageScratch_, got);
  if (refused != nullptr) {
    // Whole or not at all: nothing of a bad image reaches the buffer or the ring. The store is
    // unknown from here, so the next wake writes a fresh image over it whatever it would hold.
    imagesRefused_++;
    imageKnown_ = false;
    LOG_W("kept: the image in the store is refused whole: %s (%u byte(s) read; %lu image(s) refused in all)", refused,
          static_cast<unsigned>(got), static_cast<unsigned long>(imagesRefused_));
    return false;
  }
  ResultBuffer& results = jobs_->results();
  if (!results.empty() || !ring_.empty()) {
    LOG_W("kept: the buffer holds %u result(s) and the ring %u advert(s) already; what the image holds goes behind them",
          static_cast<unsigned>(results.count()), static_cast<unsigned>(ring_.size()));
  }
  for (size_t i = 0; i < image.resultCount(); i++) {
    long long jobId = 0;
    size_t len = 0;
    const char* body = image.result(i, jobId, len);
    if (body != nullptr) results.push(jobId, body, len);
  }
  const uint32_t nowMs = millis();
  for (size_t i = 0; i < image.advertCount(); i++) {
    HeardAdvert a;
    if (!image.advert(i, a)) break;
    // The moment it was heard travels as `heardAtEpoch`; the uptime clock it was heard on is
    // gone, so on this boot's clock it was heard "now" - which only the batch-due timer reads.
    a.heardAtMillis = nowMs;
    ring_.push(a);
  }
  results.addDropped(image.resultsDropped());
  ring_.addDropped(image.observationsDropped());
  imagesRefused_ += image.imagesRefused();
  imageKnown_ = true;
  lastImageCrc_ = image.crc();
  LOG_I("kept: %u result(s) and %u advert(s) read back from the store (%u bytes); %lu result(s) and %lu advert(s) "
        "were dropped before the sleep",
        static_cast<unsigned>(image.resultCount()), static_cast<unsigned>(image.advertCount()),
        static_cast<unsigned>(image.length()), static_cast<unsigned long>(image.resultsDropped()),
        static_cast<unsigned long>(image.observationsDropped()));
  return true;
}

void AgentLoop::keepAcrossSleep(WakeReport& report) {
  if (retained_ == nullptr) return;
  const size_t cap = imageCap();
  KeptImageWriter image(imageScratch_, cap);

  // The results first, oldest first, whole ones only, stopping at the first that does not fit:
  // a lost result leaves a job leased, a lost advert is heard again.
  ResultBuffer& results = jobs_->results();
  size_t keptResults = 0;
  for (; keptResults < results.count(); keptResults++) {
    long long jobId = 0;
    size_t len = 0;
    const char* body = results.at(keptResults, jobId, len);
    if (body == nullptr || !image.addResult(jobId, body, len)) break;
  }
  const size_t resultsOut = results.count() - keptResults;

  // Then the adverts, with the moment each was heard: the one it came back from a sleep with, or
  // the clock's reading of the uptime it was heard at. No clock is 0, which the batch after the
  // next boot stamps as "now" - the same as it would have without a store.
  const uint32_t nowMs = millis();
  size_t keptAdverts = 0;
  for (; keptAdverts < ring_.size(); keptAdverts++) {
    const HeardAdvert& a = ring_.at(keptAdverts);
    uint32_t epoch = a.heardAtEpoch;
    if (epoch == 0 && clock_->hasTime()) epoch = epochOf(a.heardAtMillis, nowMs);
    if (!image.addAdvert(a, epoch)) break;
  }
  const size_t advertsOut = ring_.size() - keptAdverts;

  // What the store cannot hold is lost at the sleep, so it is dropped now and counted once, in
  // the heartbeat's own figures, and said once.
  if (resultsOut > 0 || advertsOut > 0) {
    results.dropNewest(resultsOut);
    ring_.dropNewest(advertsOut);
    report.resultsNotKept = static_cast<int>(resultsOut);
    report.observationsNotKept = static_cast<int>(advertsOut);
    LOG_W("kept: %u result(s) and %u advert(s) do not fit the %u bytes of the store and are dropped "
          "(%lu result(s), %lu advert(s) dropped in all)",
          static_cast<unsigned>(resultsOut), static_cast<unsigned>(advertsOut), static_cast<unsigned>(cap),
          static_cast<unsigned long>(results.dropped()), static_cast<unsigned long>(ring_.dropped()));
  }

  const size_t len = image.finish(results.dropped(), ring_.dropped(), imagesRefused_);
  if (imageKnown_ && image.crc() == lastImageCrc_) {
    // The store already holds this - the same results, the same adverts, the same counters - so
    // nothing is written: a wake that posted everything and keeps nothing costs no wear at all.
    LOG_D("kept: the store already holds this image; nothing written");
    return;
  }
  if (!retained_->write(imageScratch_, len)) {
    // What the store holds now is anybody's guess, so the next wake writes again regardless.
    imageKnown_ = false;
    LOG_E("kept: the store would not take an image of %u bytes; what the next boot finds is unknown",
          static_cast<unsigned>(len));
    return;
  }
  imageKnown_ = true;
  lastImageCrc_ = image.crc();
  report.keptImageWritten = true;
  LOG_I("kept: %u result(s) and %u advert(s) written to the store, %u bytes of %u",
        static_cast<unsigned>(image.resultCount()), static_cast<unsigned>(image.advertCount()),
        static_cast<unsigned>(len), static_cast<unsigned>(cap));
}

void AgentLoop::report(TextOut& out) const {
  const uint32_t now = millis();
  char when[24];

  if (halted_) {
    out.println("state     halted: the server refused the token (http 401). Set meshmaster.token again.");
  } else if (!server_->configured()) {
    out.println("state     not configured: meshmaster.token is needed");
  } else if (!server_->link().up) {
    out.println("state     waiting for wifi");
  } else if (!elapsed(now, holdUntil_)) {
    out.printf("state     holding off for %lu s\r\n", static_cast<unsigned long>((holdUntil_ - now) / 1000));
  } else {
    out.println("state     running");
  }

  if (everHeartbeated_) {
    formatClock(lastHeartbeatAtEpoch_, when, sizeof when);
    out.printf("heartbeat last %s (%lu s ago), %lu answered; jobs waiting %s\r\n", when,
               static_cast<unsigned long>((now - lastHeartbeatAtMillis_) / 1000),
               static_cast<unsigned long>(counters_.heartbeats), lastJobsWaiting_ ? "yes" : "no");
  } else {
    out.println("heartbeat none answered yet");
  }
  if (heartbeatScheduled_) {
    const int32_t due = static_cast<int32_t>(nextHeartbeatAt_ - now);
    out.printf("next      in %ld s (poll %d s, budget %d/min gap %d s)\r\n",
               static_cast<long>(due > 0 ? due / 1000 : 0), pollIntervalS_, budget_.maxTransactionsPerMinute,
               budget_.minGapSeconds);
  } else {
    out.println("next      not scheduled");
  }

  if (counters_.batches > 0) {
    formatClock(lastBatchAtEpoch_, when, sizeof when);
    out.printf("batch     last %s, %d item(s); %lu batch(es), %lu observation(s) posted\r\n", when, lastBatchCount_,
               static_cast<unsigned long>(counters_.batches), static_cast<unsigned long>(counters_.observationsPosted));
  } else {
    out.println("batch     none posted yet");
  }
  out.printf("pending   %u of %u in the ring, %lu dropped, %lu heard in all\r\n",
             static_cast<unsigned>(ring_.size()), static_cast<unsigned>(ObservationRing::kCapacity),
             static_cast<unsigned long>(ring_.dropped()), static_cast<unsigned long>(ring_.heard()));
  out.printf("jobs      %lu claim(s), %lu job(s) run, %lu claim(s) unanswered; `jobs` prints the last ten\r\n",
             static_cast<unsigned long>(jobs_->claims()), static_cast<unsigned long>(jobs_->ran()),
             static_cast<unsigned long>(counters_.claimFailures));
  out.printf("errors    overheard %lu, wifiReconnects %lu, httpFailures %lu, observationsDropped %lu, "
             "resultsDropped %lu, reboots %lu; `errors` has the rest\r\n",
             static_cast<unsigned long>(mesh_->overheard()), static_cast<unsigned long>(counters_.wifiReconnects),
             static_cast<unsigned long>(counters_.httpFailures), static_cast<unsigned long>(ring_.dropped()),
             static_cast<unsigned long>(jobs_->resultsDropped()), static_cast<unsigned long>(platform_->selfReboots()));
  out.printf("requests  %u in the last minute of the 120 the policy allows, %lu held back; %lu batch(es) refused\r\n",
             static_cast<unsigned>(server_->limiter().inWindow(now)), static_cast<unsigned long>(server_->limiter().held()),
             static_cast<unsigned long>(counters_.observationsRefused));
}

void AgentLoop::reportErrors(TextOut& out) const {
  // The six the heartbeat carries, first and by their wire names, so that a figure on the exit
  // node's page can be found here without a translation.
  const IUplink::LinkState link = server_->link();
  out.println("heartbeat the six counters the heartbeat reports, by the names the server shows");
  out.printf("  overheard            %lu  (mesh messages counted and dropped unread - a busy channel, not a fault)\r\n",
             static_cast<unsigned long>(mesh_->overheard()));
  out.printf("  wifiReconnects       %lu  (%u link(s) in all, %u drop(s))\r\n",
             static_cast<unsigned long>(counters_.wifiReconnects), static_cast<unsigned>(link.connects),
             static_cast<unsigned>(link.drops));
  out.printf("  httpFailures         %lu  (%lu in a row now, failing for %lu min; it restarts itself at %lu in "
             "a row and %lu min)\r\n",
             static_cast<unsigned long>(counters_.httpFailures),
             static_cast<unsigned long>(counters_.consecutiveFailures),
             static_cast<unsigned long>(
                 counters_.consecutiveFailures == 0 ? 0 : (millis() - firstFailureAtMillis_) / 60000),
             static_cast<unsigned long>(kRebootAfterFailures),
             static_cast<unsigned long>(kRebootAfterFailingMs / 60000));
  out.printf("  observationsDropped  %lu  (adverts the ring overwrote; %u of %u waiting, %lu heard in all)\r\n",
             static_cast<unsigned long>(ring_.dropped()), static_cast<unsigned>(ring_.size()),
             static_cast<unsigned>(ObservationRing::kCapacity), static_cast<unsigned long>(ring_.heard()));
  out.printf("  resultsDropped       %lu  (job results the buffer overwrote; %d waiting, %lu posted late)\r\n",
             static_cast<unsigned long>(jobs_->resultsDropped()), jobs_->queueDepth(),
             static_cast<unsigned long>(counters_.resultsPosted));
  out.printf("  reboots              %lu  (restarts this firmware asked for; `reboots` says why)\r\n",
             static_cast<unsigned long>(platform_->selfReboots()));
  out.println("beside    counters the server does not get, for a person at the port");
  out.printf("  claims unanswered    %lu\r\n", static_cast<unsigned long>(counters_.claimFailures));
  out.printf("  results not taken    %lu  (at the first attempt)\r\n",
             static_cast<unsigned long>(jobs_->postFailures()));
  out.printf("  batches refused      %lu  (the server would not have them, ever)\r\n",
             static_cast<unsigned long>(counters_.observationsRefused));
  out.printf("  observations posted  %lu in %lu batch(es)\r\n",
             static_cast<unsigned long>(counters_.observationsPosted), static_cast<unsigned long>(counters_.batches));
  out.printf("  heartbeats answered  %lu\r\n", static_cast<unsigned long>(counters_.heartbeats));
  out.printf("  undecodable adverts  %lu\r\n", static_cast<unsigned long>(mesh_->counters().advertsDropped));
}

void AgentLoop::describe(char* out, size_t cap) const {
  if (halted_) {
    std::snprintf(out, cap, "halted: http 401, the token was refused");
    return;
  }
  if (!everHeartbeated_) {
    std::snprintf(out, cap, "no heartbeat answered yet; %u advert(s) waiting", static_cast<unsigned>(ring_.size()));
    return;
  }
  char when[24];
  formatClock(lastHeartbeatAtEpoch_, when, sizeof when);
  std::snprintf(out, cap, "last at %s, %lu answered, poll %d s; %lu observation(s) posted, %u waiting, %lu dropped",
                when, static_cast<unsigned long>(counters_.heartbeats), pollIntervalS_,
                static_cast<unsigned long>(counters_.observationsPosted), static_cast<unsigned>(ring_.size()),
                static_cast<unsigned long>(ring_.dropped()));
}

bool AgentLoop::online() const {
  if (!everHeartbeated_ || counters_.consecutiveFailures != 0 || halted_) return false;
  // `holdUntil_` is set to `millis()` by every success, so `elapsed` is true the moment a call
  // is answered and this is only ever false while a wait is actually running.
  return elapsed(millis(), holdUntil_);
}

uint32_t AgentLoop::retrySeconds() const {
  const int32_t left = static_cast<int32_t>(holdUntil_ - millis());
  return left > 0 ? static_cast<uint32_t>(left) / 1000 : 0;
}

}  // namespace mm
