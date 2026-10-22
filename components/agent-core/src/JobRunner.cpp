#include <agent/JobRunner.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <agent/CliReplies.h>
#include <agent/Iso8601.h>
#include <agent/JobResults.h>
#include <agent/Log.h>

namespace mm {

namespace {

// The scratch every job shares is the runner's own since task p1.5-15 (JobRunner::Scratch): one
// instance, statically allocated by the board as part of `jobs`, and no longer a translation
// unit's statics. The names below are the ones the buffers had as file statics - the host tests
// hold this file's text, and these names are in it - so each is the scratch's member under its
// old name, and nothing else in this file is a macro.
#define g_jobs (scratch_.jobs)
#define g_data (scratch_.data)
#define g_body (scratch_.body)
#define g_log (scratch_.log)
#define g_neighbours (scratch_.neighbours)
#define g_readings (scratch_.readings)
#define g_hops (scratch_.hops)
#define g_pathHops (scratch_.pathHops)
#define g_pathPtrs (scratch_.pathPtrs)
#define g_pathBytes (scratch_.pathBytes)
#define g_roundTrip (scratch_.roundTrip)
#define g_snapshot (scratch_.snapshot)
#define g_acl (scratch_.acl)
#define g_cliReply (scratch_.cliReply)
#define g_regionListing (scratch_.regionListing)
#define g_regionValue (scratch_.regionValue)
#define g_command (scratch_.command)
#define g_applyOutcome (scratch_.applyOutcome)
#define g_secretLine (scratch_.secretLine)
constexpr size_t kDataCap = JobRunner::kDataCap;
constexpr size_t kBodyCap = JobRunner::kBodyCap;
// How long a line that expects no reply is given to leave the radio before the next is queued:
// the region loader resolves a line's parent by indent against a stack, so order matters, and
// MeshCore's outbound queue is pumped by the loop this waits in.
constexpr uint32_t kLineSettleMs = 250;

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The claim's path hops as bytes, and the hash size they are written at (the hop hashes are hex,
// so their length says it). Returns the hop count, or 0 when the path cannot be read - which is
// also a known path of zero hops, a direct neighbour; `job.hasPath` tells the two apart.
uint8_t pathToBytes(const ClaimedJob& job, uint8_t* out, size_t cap, uint8_t& hashSize) {
  hashSize = 0;
  if (!job.hasPath || job.pathCount <= 0) return 0;
  const size_t width = std::strlen(job.pathHops[0]);
  if (width == 0 || width % 2 != 0 || width > 6) return 0;
  hashSize = static_cast<uint8_t>(width / 2);
  size_t n = 0;
  for (int i = 0; i < job.pathCount; i++) {
    if (std::strlen(job.pathHops[i]) != width || n + hashSize > cap) return 0;
    for (size_t b = 0; b < hashSize; b++) {
      const int hi = hexValue(job.pathHops[i][b * 2]);
      const int lo = hexValue(job.pathHops[i][b * 2 + 1]);
      if (hi < 0 || lo < 0) return 0;
      out[n++] = static_cast<uint8_t>((hi << 4) | lo);
    }
  }
  return static_cast<uint8_t>(job.pathCount);
}

bool startsWithOk(const char* text) { return text != nullptr && text[0] == 'O' && text[1] == 'K'; }

bool sameKey(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  for (;; a++, b++) {
    const char x = (*a >= 'A' && *a <= 'F') ? static_cast<char>(*a - 'A' + 'a') : *a;
    const char y = (*b >= 'A' && *b <= 'F') ? static_cast<char>(*b - 'A' + 'a') : *b;
    if (x != y) return false;
    if (x == '\0') return true;
  }
}

}  // namespace

void JobRunner::begin(IMesh& mesh, IUplink& server, IPlatform& platform, void (*pump)()) {
  mesh_ = &mesh;
  server_ = &server;
  platform_ = &platform;
  clock_ = &platform.clock();
  pump_ = pump;
}

void JobRunner::setIdentity(const char* publicKeyHex) { publicKey_ = publicKeyHex; }

void JobRunner::randomBytes(uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i += 4) {
    const uint32_t r = platform_->random();
    for (size_t b = 0; b < 4 && i + b < len; b++) out[i + b] = static_cast<uint8_t>(r >> (8 * b));
  }
}

void JobRunner::idle(uint32_t ms) {
  const uint32_t deadline = millis() + ms;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    mesh_->loop();
    if (pump_ != nullptr) pump_();
    platform_->delayMs(1);
  }
}

bool JobRunner::leaseRanOut(const ClaimedJob& job) const {
  return leaseLapsed(job.leaseUntil, clock_->hasTime(), clock_->now());
}

uint32_t JobRunner::requestWaitMs(const ClaimedJob& job) const {
  // The per-hop timeout scaled by the hops (EXIT-NODES §7); a flood waits as a three-hop
  // request would, because nobody knows how far it went.
  const int hops = mesh_->hasPath() ? mesh_->pathHopCount() : 3;
  return static_cast<uint32_t>(job.perHopMs) * static_cast<uint32_t>(hops + 1);
}

uint32_t JobRunner::loginWaitMs(const ClaimedJob& job) const {
  const uint32_t request = requestWaitMs(job);
  const uint32_t login = static_cast<uint32_t>(job.loginMs);
  return login > request ? login : request;
}

bool JobRunner::spend(const ClaimedJob& job, Context& ctx, bool escape) {
  if (!escape && leaseRanOut(job)) return false;
  uint32_t wait = gate_.waitMs(millis());
  if (wait > 0) {
    // A budget of two a minute means most of a job is waiting. Never wait past the lease: the
    // server has moved the job on by then, and the air is better spent on the next one. The one
    // exception is the line that ends a region load, which goes out whatever the lease says.
    if (!escape && clock_->hasTime() && job.leaseUntil != 0 && clock_->now() + (wait + 999) / 1000 > job.leaseUntil) {
      g_log.add("the budget's next slot is past the lease");
      return false;
    }
    g_log.add("waiting %lu s for the airtime budget", static_cast<unsigned long>((wait + 999) / 1000));
    gate_.addWait(wait);
    idle(wait);
  }
  if (!escape && leaseRanOut(job)) return false;
  gate_.took(millis());
  ctx.transactions++;
  return true;
}

// ------------------------------------------------------------------ the login

const char* JobRunner::loginMaterial(const ClaimedJob& job) {
  // Which material a class logs in with - the password, the blank login of `acl`, `none` and the
  // guest door, or nothing at all at `admin` - is agent/LoginSequence.h's decision, on a host, so
  // that every branch of it is walked natively (test/test_rotation_probe) rather than held on
  // this file's text. This puts its one line into the job's log, and no log line ever takes the
  // value as an argument.
  char note[kLoginNoteCap] = {0};
  const char* material = loginMaterialFor(job, note, sizeof note);
  if (note[0] != '\0') g_log.add("%s", note);
  return material;
}

JobRunner::LoginResult JobRunner::loginOnce(const ClaimedJob& job, Context& ctx, const char* material,
                                            const char* what) {
  if (!spend(job, ctx)) return LoginResult::Timeout;
  const uint32_t started = millis();
  uint32_t tag = 0;
  const SendResult sent = mesh_->sendLogin(material, tag);
  if (sent == SendResult::Failed) {
    g_log.add("%s: the packet could not be built", what);
    return LoginResult::Failed;
  }
  if (!mesh_->awaitReply(tag, loginWaitMs(job), pump_)) {
    // Stock firmware answers a login it will not take with **silence** (`handleLoginReq`
    // returns 0): a key its ACL does not hold on a blank login, a password that is neither of
    // its two on a password login. It cannot be told from a repeater that is off the air.
    g_log.add("%s: no reply (%s)", what, sent == SendResult::Direct ? "direct" : "flood");
    return LoginResult::NoReply;
  }
  if (!ctx.hasRtt) {
    ctx.hasRtt = true;
    ctx.rttMs = static_cast<int>(millis() - started);
  }
  const MeshReply& reply = mesh_->reply();
  LoginReply login;
  if (reply.kind != MeshReply::Kind::Response || !parseLoginReply(reply.data, reply.len, login)) {
    g_log.add("%s: refused", what);
    sessions_.invalidate(job.repeaterKey);
    return LoginResult::Refused;
  }
  const uint8_t permissions = login.hasAclPermissions ? login.aclPermissions : login.role();
  const RepeaterSession* stored = sessions_.put(job.repeaterKey, job.credentialClass, permissions, millis());
  const uint8_t granted = stored != nullptr ? stored->role() : login.role();
  char level[24] = {0};
  if (login.hasFirmwareLevel) {
    std::snprintf(level, sizeof level, ", firmware level %u", static_cast<unsigned>(login.firmwareLevel));
  }
  if (stored == nullptr || !stored->satisfies(job.credentialClass)) {
    // A login judged against the class it was for, the moment it comes back (task p3.5-41):
    // answered below what the job needs is the refusal it is, and the level travels in the
    // result so the server can tell a demoted entry from one that is gone (task p4.1-60).
    ctx.grantedBelowClass = true;
    ctx.grantedRole = granted;
    const char* with = job.credentialClass == CredentialClass::Admin   ? "admin password"
                       : job.credentialClass == CredentialClass::Guest ? "guest password"
                                                                       : "login from this exit node's own key";
    g_log.add("logged in as %s, but this job needs %s: the repeater accepted the %s at a lower level than %s%s",
              permissionName(granted), permissionName(roleNeededFor(job.credentialClass)), with,
              permissionName(roleNeededFor(job.credentialClass)), level);
    return LoginResult::Refused;
  }
  ctx.hasSession = true;
  ctx.sessionRole = granted;
  ctx.sessionWasFresh = true;
  g_log.add("%s: logged in as %s%s%s (%s)", what, permissionName(granted), level, login.legacy ? " (a legacy reply)" : "",
            sent == SendResult::Direct ? "direct" : "flood");
  return LoginResult::Ok;
}

// The login sequence's exchange over this runner (agent/LoginSequence.h): a login with the job's
// material is loginOnce(), the blank login between the tries is probeReachable(), and the log is
// the job's. The sequence decides what a silence means; this puts each login on the air once.
class JobRunner::LoginOnTheAir : public LoginExchange {
 public:
  LoginOnTheAir(JobRunner& runner, const ClaimedJob& job, Context& ctx, const char* material)
      : runner_(runner), job_(job), ctx_(ctx), material_(material) {}

  LoginTry login(int attempt) override {
    char what[40];
    std::snprintf(what, sizeof what, "login (%s) try %d", credentialClassName(job_.credentialClass), attempt);
    return runner_.loginOnce(job_, ctx_, material_, what);
  }

  ProbeAnswer probe(uint8_t& role) override {
    if (!runner_.probeReachable(job_, ctx_)) return ProbeAnswer::NoReply;
    if (ctx_.probeRefused) return ProbeAnswer::Refused;
    role = ctx_.probeAnsweredRole;
    return ProbeAnswer::Answered;
  }

  void log(const char* line) override { runner_.scratch_.log.add("%s", line); }

 private:
  JobRunner& runner_;
  const ClaimedJob& job_;
  Context& ctx_;
  const char* material_;
};

Outcome JobRunner::ensureSession(const ClaimedJob& job, Context& ctx) {
  if (job.credentialClass == CredentialClass::None) return Outcome::Ok;  // a trace asks nobody

  const uint32_t now = millis();
  if (const RepeaterSession* live = sessions_.get(job.repeaterKey, job.credentialClass, now)) {
    ctx.hasSession = true;
    ctx.sessionRole = live->role();
    ctx.sessionWasFresh = false;
    g_log.add("session reused (%s, %s, %lu s old)", credentialClassName(live->credentialClass), live->tier(),
              static_cast<unsigned long>((now - live->loggedInAtMs) / 1000));
    return Outcome::Ok;
  }

  const char* material = loginMaterial(job);
  if (material == nullptr) return Outcome::Unauthorized;

  // The two tries, and on the one path that needs it the blank login between them (task
  // p1.3-13): a job carrying the `new` candidate of an unverified rotation. The policy is
  // agent/LoginSequence.h's, on a host; this puts its logins on the air.
  const bool probes = probesBetweenTries(job.credentialCandidate, material);
  LoginOnTheAir exchange(*this, job, ctx, material);
  const LoginVerdict verdict = loginSequence(exchange, probes);
  if (verdict.silentAfterReachable) {
    ctx.silentAfterReachable = true;
    LOG_W("jobs: job %lld: the repeater answered a blank login at %s and then said nothing to the new candidate; "
          "reported unauthorized (silentAfterReachable)",
          job.id, probeAnsweredAs(verdict));
  }
  return verdict.outcome;
}

bool JobRunner::probeReachable(const ClaimedJob& job, Context& ctx) {
  ctx.probeRefused = false;
  ctx.probeAnsweredRole = 0;
  if (!spend(job, ctx)) {
    g_log.add("reachability probe (blank login): not sent, the lease ran out");
    return false;
  }
  uint32_t tag = 0;
  const SendResult sent = mesh_->sendLogin("", tag);
  if (sent == SendResult::Failed) {
    g_log.add("reachability probe (blank login): the packet could not be built");
    return false;
  }
  const char* how = sent == SendResult::Direct ? "direct" : "flood";
  if (!mesh_->awaitReply(tag, loginWaitMs(job), pump_)) {
    g_log.add("reachability probe (blank login): no reply (%s)", how);
    g_log.add("the blank login was not answered either: nothing is known about the password - the repeater is off "
              "the air, or it has a guest password set and no entry for this exit node's key (task p4.7-95)");
    LOG_I("jobs: job %lld: reachability probe (blank login): no reply; nothing is known about the password", job.id);
    return false;
  }
  // No session is stored from this reply, whatever it says: the next job is a candidate job and
  // logs in afresh, and a session another login opened would make its verdict a lie.
  const MeshReply& reply = mesh_->reply();
  LoginReply login;
  if (reply.kind != MeshReply::Kind::Response || !parseLoginReply(reply.data, reply.len, login)) {
    ctx.probeRefused = true;
    g_log.add("reachability probe (blank login): refused (%s)", how);
    g_log.add("the repeater is on the air: it refused the blank login, so a silence to the new candidate right after "
              "it is a refusal (task p4.7-95)");
    LOG_I("jobs: job %lld: reachability probe (blank login): refused; the repeater is on the air", job.id);
    return true;
  }
  const uint8_t permissions = login.hasAclPermissions ? login.aclPermissions : login.role();
  ctx.probeAnsweredRole = static_cast<uint8_t>(permissions & kPermRoleMask);
  const char* tier = permissionName(ctx.probeAnsweredRole);
  g_log.add("reachability probe (blank login): answered at %s%s (%s)", tier, login.legacy ? " (a legacy reply)" : "",
            how);
  g_log.add("the repeater is on the air: it answered the blank login at %s, so a silence to the new candidate right "
            "after it is a refusal (task p4.7-95)",
            tier);
  LOG_I("jobs: job %lld: reachability probe (blank login): answered at %s; the repeater is on the air", job.id, tier);
  return true;
}

// ------------------------------------------------------------------ one exchange

template <typename Send>
Outcome JobRunner::exchange(const ClaimedJob& job, Context& ctx, const char* what, bool retry, bool text, Send send,
                            bool escape) {
  const int attempts = retry ? 2 : 1;
  for (int attempt = 1; attempt <= attempts; attempt++) {
    if (!spend(job, ctx, escape)) return Outcome::Timeout;
    const uint32_t started = millis();
    uint32_t tag = 0;
    const SendResult sent = send(tag);
    if (sent == SendResult::Failed) {
      g_log.add("%s try %d: the packet could not be built", what, attempt);
      return Outcome::Failed;
    }
    const bool answered = text ? mesh_->awaitText(requestWaitMs(job), pump_) : mesh_->awaitReply(tag, requestWaitMs(job), pump_);
    if (answered) {
      ctx.hasRtt = true;
      ctx.rttMs = static_cast<int>(millis() - started);
      g_log.add("%s try %d: ok (%s, %lu ms)", what, attempt, sent == SendResult::Direct ? "direct" : "flood",
                static_cast<unsigned long>(ctx.rttMs));
      if (text) std::memcpy(g_cliReply, mesh_->reply().text, sizeof g_cliReply);
      return Outcome::Ok;
    }
    g_log.add("%s try %d: no reply", what, attempt);
    if (attempt == 1 && retry && !ctx.sessionWasFresh && ctx.hasSession) {
      // An old session and no answer: the repeater may have rebooted and forgotten us.
      sessions_.invalidate(job.repeaterKey);
      ctx.hasSession = false;
      g_log.add("no reply on a reused session: logging in again once");
      const Outcome again = ensureSession(job, ctx);
      if (again != Outcome::Ok) return again;
    }
  }
  return Outcome::Unreachable;
}

Outcome JobRunner::request(const ClaimedJob& job, Context& ctx, const char* what, uint8_t reqType,
                           const uint8_t* data, uint8_t len) {
  return exchange(job, ctx, what, true, false,
                  [&](uint32_t& tag) { return mesh_->sendRequest(reqType, data, len, tag); });
}

Outcome JobRunner::cliLine(const ClaimedJob& job, Context& ctx, const char* what, const char* command, bool retry,
                           uint32_t* timestampUsed, bool sensitive, bool escape) {
  g_cliReply[0] = '\0';
  // Each attempt is its own sender timestamp, strictly above the last: the mesh draws it from the
  // one series every tag and every line of this board come from and hands it back (IMesh::sendCli,
  // task p1.5-15), because the repeater answers an equal one with nothing and runs nothing, and
  // drops a lower one. A CLI reply carries no tag, so the timestamp stands in for one here.
  return exchange(
      job, ctx, what, retry, true,
      [&](uint32_t& tag) {
        const SendResult sent = mesh_->sendCli(command, sensitive, tag);
        if (timestampUsed != nullptr) *timestampUsed = tag;
        return sent;
      },
      escape);
}

Outcome JobRunner::adminSession(const ClaimedJob& job, Context& ctx) {
  const Outcome session = ensureSession(job, ctx);
  if (session != Outcome::Ok) return session;
  if (ctx.sessionRole != kPermAdmin) {
    g_log.add("logged in as %s: this firmware answers CLI commands to admin only, so nothing is sent",
              permissionName(ctx.sessionRole));
    return Outcome::Unauthorized;
  }
  return Outcome::Ok;
}

// ------------------------------------------------------------------ the restore (task p4.1-60)

int JobRunner::restoreAcl(const ClaimedJob& job, Context& ctx, Outcome outcome) {
  if (ctx.aclRestoreDone) return -1;
  // Nothing to restore unless the claim carried a grant below admin and the session in force is
  // admin - by the admin password, which rewrote our entry to admin on stock firmware, or a
  // blank login answered admin because an earlier session died before its restore.
  if (!job.hasAclLevel || job.aclLevel >= kPermAdmin || job.aclLevel < kPermReadOnly || !ctx.hasSession ||
      ctx.sessionRole != kPermAdmin) {
    return -1;
  }
  ctx.aclRestoreDone = true;
  const char* granted = permissionName(static_cast<uint8_t>(job.aclLevel));
  if (outcome == Outcome::Unreachable || outcome == Outcome::Timeout) {
    // The repeater stopped answering, or the lease is spent: a restore sent now would go into
    // the same silence. The session is dropped, and the next login from this key - a blank
    // login answered admin - puts the entry back on that job's account (the third rule of §7).
    g_log.add("the ACL entry of this exit node's key may still be at admin: the job ended %s, so the restore is not "
              "sent; the session is dropped, and the next login from this key will put it right (task 60)",
              outcomeName(outcome));
    sessions_.invalidate(job.repeaterKey);
    ctx.hasSession = false;
    return 0;
  }
  if (publicKey_ == nullptr || std::strlen(publicKey_) != kPublicKeyHexLen) {
    g_log.add("the ACL entry cannot be put back: this board's own public key is unknown");
    sessions_.invalidate(job.repeaterKey);
    ctx.hasSession = false;
    return 0;
  }
  std::snprintf(g_command, sizeof g_command, "setperm %s %d", publicKey_, job.aclLevel);
  char what[48];
  std::snprintf(what, sizeof what, "restore: cli 'setperm <own key> %d'", job.aclLevel);
  const Outcome sent = cliLine(job, ctx, what, g_command, false);
  trimInPlace(g_cliReply);
  if (sent == Outcome::Ok && startsWithOk(g_cliReply)) {
    // On stock firmware the entry *is* the session (ClientACL::applyPermissions finds the same
    // ClientInfo the login filled): the repeater now holds us at the granted level and the cache
    // says so - reusable by the next acl-class job, invisible to an admin-class one.
    sessions_.put(job.repeaterKey, CredentialClass::Acl, static_cast<uint8_t>(job.aclLevel), millis());
    ctx.sessionRole = static_cast<uint8_t>(job.aclLevel);
    g_log.add("the ACL entry of this exit node's key is back at %s, as granted; the session is now %s (task 60)",
              granted, granted);
    return 1;
  }
  sessions_.invalidate(job.repeaterKey);
  ctx.hasSession = false;
  if (sent == Outcome::Ok) {
    g_log.add("the ACL entry of this exit node's key may still be at admin: the repeater answered '%s'; the session "
              "is dropped, and the next login from this key will put it right (task 60)",
              g_cliReply);
  } else {
    g_log.add("the ACL entry of this exit node's key may still be at admin: the restore got %s; the session is "
              "dropped, and the next login from this key will put it right (task 60)",
              outcomeName(sent));
  }
  return 0;
}

// ------------------------------------------------------------------ the four reads

Outcome JobRunner::runStatus(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  const Outcome session = ensureSession(job, ctx);
  if (session != Outcome::Ok) return session;

  const Outcome got = request(job, ctx, "status request", kReqTypeGetStatus, nullptr, 0);
  if (got != Outcome::Ok) return got;

  StatusReading reading;
  const MeshReply& reply = mesh_->reply();
  if (!parseRepeaterStats(reply.data, reply.len, reading)) {
    g_log.add("the status reply was %u bytes; a RepeaterStats is at least %u", static_cast<unsigned>(reply.len),
              static_cast<unsigned>(kRepeaterStatsMinLen));
    return Outcome::Failed;
  }

  char firmware[kMaxFirmwareVersionLen + 1] = {0};
  if (readsFirmwareVersion(job)) {
    // The verify step of an association records the firmware it proved the ACL login on
    // (EXIT-NODES §4.1 step 5); the version string is only readable through the owner-info
    // request, one more transaction, so it is fetched for acl-class reads alone - and since
    // meshmaster.dk's task p4.3-68 only when the claim asks for it, which is when the server's
    // record is stale or this pair has recorded none. Forty-seven of a repeater's forty-eight
    // daily reads are one transaction instead of two.
    if (request(job, ctx, "owner info (firmware)", kReqTypeGetOwnerInfo, nullptr, 0) == Outcome::Ok) {
      const MeshReply& owner = mesh_->reply();
      parseOwnerInfoFirmware(owner.data, owner.len, firmware, sizeof firmware);
    } else {
      g_log.add("the firmware version was not read; the status itself stands");
    }
  }

  ErrorCounter errors[kMaxStatusErrors];
  const int errorCount = statusErrors(reading, errors, kMaxStatusErrors);
  dataLen = writeStatusResult(reading, errors, errorCount, firmware[0] != '\0' ? firmware : nullptr, g_data, kDataCap);
  return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
}

Outcome JobRunner::runTelemetry(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  const Outcome session = ensureSession(job, ctx);
  if (session != Outcome::Ok) return session;

  // The request data of a telemetry read: the inverse permission mask (0 is everything the
  // sender may have) and four random bytes, for a packet hash nothing repeats.
  uint8_t data[8] = {0};
  randomBytes(&data[4], 4);
  const Outcome got = request(job, ctx, "telemetry request", kReqTypeGetTelemetry, data, sizeof data);
  if (got != Outcome::Ok) return got;

  const MeshReply& reply = mesh_->reply();
  size_t undecodedFrom = 0;
  const int count = decodeLpp(reply.data, reply.len, g_readings, kMaxLppReadings, undecodedFrom);
  if (undecodedFrom < reply.len) {
    g_log.add("telemetry: %u byte(s) not decoded", static_cast<unsigned>(reply.len - undecodedFrom));
  }
  dataLen = writeTelemetryResult(g_readings, count, g_data, kDataCap);
  return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
}

Outcome JobRunner::runNeighbours(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  const Outcome session = ensureSession(job, ctx);
  if (session != Outcome::Ok) return session;

  int collected = 0;
  int offset = 0;
  int total = 0;
  for (int page = 0; page < 8; page++) {
    // version 0, count, offset (u16), order_by, prefix length, four random bytes.
    uint8_t data[10] = {0};
    data[0] = 0;
    data[1] = kNeighboursPerPage;
    data[2] = static_cast<uint8_t>(offset & 0xFF);
    data[3] = static_cast<uint8_t>((offset >> 8) & 0xFF);
    data[4] = 0;  // newest to oldest
    data[5] = kNeighbourPrefixLen;
    randomBytes(&data[6], 4);

    char what[40];
    std::snprintf(what, sizeof what, "neighbours page at %d", offset);
    const Outcome got = request(job, ctx, what, kReqTypeGetNeighbours, data, sizeof data);
    if (got != Outcome::Ok) {
      // A page that never came back after some already did is still a reading: the server gets
      // what was heard rather than nothing at all.
      if (collected == 0) return got;
      g_log.add("the list stops at %d of %d", collected, total);
      break;
    }

    const MeshReply& reply = mesh_->reply();
    NeighboursPage p;
    if (!parseNeighboursPage(reply.data, reply.len, kNeighbourPrefixLen, p)) {
      g_log.add("a neighbours page did not parse (%u bytes)", static_cast<unsigned>(reply.len));
      if (collected == 0) return Outcome::Failed;
      break;
    }
    total = p.total;
    if (p.count == 0) break;
    for (int i = 0; i < p.count && collected < kMaxNeighbours; i++) g_neighbours[collected++] = p.entries[i];
    offset += p.count;
    if (offset >= total || collected >= kMaxNeighbours) break;
  }
  if (collected < total) g_log.add("%d of the repeater's %d neighbour(s) reported", collected, total);

  dataLen = writeNeighboursResult(g_neighbours, collected, g_data, kDataCap);
  return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
}

Outcome JobRunner::runTrace(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  uint8_t hashSize = 0;
  uint8_t hops = pathToBytes(job, g_pathBytes, sizeof g_pathBytes, hashSize);
  if (hops == 0 && job.hasPath && job.pathCount == 0) {
    // A direct neighbour: a known path of zero hops, and the trace's hashes are this board's own
    // size, one byte (PATH_HASH_SIZE).
    hashSize = 1;
  } else if (hops == 0) {
    g_log.add("no path is known to this repeater: a trace needs one; run discover first");
    return Outcome::Unreachable;
  }
  if (!isTraceableHashSize(hashSize)) {
    // A trace packet states its hash size as an exponent, 1 << (flags & 3), which has no value
    // three: a path of three-byte hashes cannot be traced at all, by protocol and not by
    // omission (task p3.5-42). Say so instead of sending a frame nothing on the path can follow.
    g_log.add("this path is built of %u-byte hashes, and a trace packet can carry only 1, 2, 4 or 8",
              static_cast<unsigned>(hashSize));
    return Outcome::Unsupported;
  }

  // The round trip: out along the path, the repeater itself, and back the way it came.
  size_t n = 0;
  for (int i = 0; i < hops; i++) {
    std::memcpy(&g_roundTrip[n], &g_pathBytes[i * hashSize], hashSize);
    n += hashSize;
  }
  for (int i = 0; i < hashSize; i++) {
    const int hi = hexValue(job.repeaterKey[i * 2]);
    const int lo = hexValue(job.repeaterKey[i * 2 + 1]);
    if (hi < 0 || lo < 0) return Outcome::Failed;
    g_roundTrip[n++] = static_cast<uint8_t>((hi << 4) | lo);
  }
  for (int i = hops - 1; i >= 0; i--) {
    std::memcpy(&g_roundTrip[n], &g_pathBytes[i * hashSize], hashSize);
    n += hashSize;
  }
  if (n > kMeshPathCap) {
    g_log.add("the round trip is %u bytes, more than a trace packet's path", static_cast<unsigned>(n));
    return Outcome::Unsupported;
  }

  for (int attempt = 1; attempt <= 2; attempt++) {
    if (!spend(job, ctx)) return Outcome::Timeout;
    const uint32_t started = millis();
    uint32_t tag = 0;
    if (mesh_->sendTrace(g_roundTrip, static_cast<uint8_t>(n), hashSize, tag) == SendResult::Failed) {
      g_log.add("trace try %d: the packet could not be built", attempt);
      return Outcome::Failed;
    }
    if (!mesh_->awaitReply(tag, requestWaitMs(job), pump_)) {
      g_log.add("trace try %d: no reply", attempt);
      continue;
    }
    const MeshReply& reply = mesh_->reply();
    if (reply.kind != MeshReply::Kind::Trace) {
      g_log.add("trace try %d: the reply was not a trace", attempt);
      return Outcome::Failed;
    }
    ctx.hasRtt = true;
    ctx.rttMs = static_cast<int>(millis() - started);

    TraceReading trace;
    parseTrace(reply.traceFlags, reply.traceHashes, reply.traceHashBytes, reply.traceSnrs, reply.traceSnrCount,
               trace);
    int count = trace.count;
    if (count > kMaxTraceHops) count = kMaxTraceHops;
    for (int i = 0; i < count; i++) g_hops[i] = trace.hops[i];
    // The whole round trip, and this exit node's own hash last with the SNR it measured on the
    // packet coming home: the server takes the outward half of it (task 24).
    if (publicKey_ != nullptr && count < kMaxTraceHops + 1) {
      TraceHop& self = g_hops[count];
      self = TraceHop();
      std::snprintf(self.prefix, sizeof self.prefix, "%.*s", trace.hashSize * 2, publicKey_);
      self.hasSnr = true;
      self.snr = reply.traceFinalSnr;
      count++;
    }
    g_log.add("trace: %d hop(s) of the round trip, %lu ms", count, static_cast<unsigned long>(ctx.rttMs));
    dataLen = writeTraceResult(g_hops, count, g_data, kDataCap);
    return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
  }
  return Outcome::Unreachable;
}

// ------------------------------------------------------------------ discover (tasks p3.5-39, 40, 41)

Outcome JobRunner::learned(const ClaimedJob& job, Context& ctx, LoginResult from, const char* source,
                           size_t& dataLen) {
  (void)job;
  const int hopCount = mesh_->describePath(g_pathHops, kMaxClaimPathHops);
  for (int h = 0; h < hopCount; h++) g_pathPtrs[h] = g_pathHops[h];
  g_log.add("path of %d hop(s), learned from the %s", hopCount, source);
  const uint32_t learnedAt = clock_->now();
  if (ctx.grantedBelowClass) {
    // The route is real and is reported; the job is unauthorized, because the grant that would
    // follow would fail at this same login (task p3.5-41).
    g_log.add("the path is reported; the job is unauthorized, because an admin job that follows would fail at this "
              "same login");
    dataLen = writeDiscoverResult(true, g_pathPtrs, hopCount, source, learnedAt, permissionName(ctx.grantedRole),
                                  g_data, kDataCap);
    return dataLen > 0 ? Outcome::Unauthorized : Outcome::Failed;
  }
  if (ctx.hasSession) {
    // The session's own tier, not the word "admin": a discover at the guest door opens a guest
    // one (task p1.4-14), and the association step reads the level the reply granted.
    g_log.add("the %s session is open for %lu s: the next job for this repeater logs in again only if it has gone",
              permissionName(ctx.sessionRole), static_cast<unsigned long>(SessionStore::kLifetimeMs / 1000));
    dataLen = writeDiscoverResult(true, g_pathPtrs, hopCount, source, learnedAt, permissionName(ctx.sessionRole),
                                  g_data, kDataCap);
    return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
  }
  if (from == LoginResult::Refused) {
    g_log.add("the login was refused, which still taught the path - but an admin job through this exit node needs a "
              "password this repeater accepts");
  }
  dataLen = writeDiscoverResult(true, g_pathPtrs, hopCount, source, learnedAt, nullptr, g_data, kDataCap);
  return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
}

Outcome JobRunner::runDiscover(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  // Why this order (25 September 2026, task p3.5-39): a login that is answered - accepted or
  // refused - proves the path in both directions and costs the transaction the association flow
  // needs next anyway; a flooded login's reply comes back along a path and teaches it; and the
  // path-discovery request is what a v1.17.1 repeater ten metres away forwarded twice and never
  // answered, so it is the last resort. Which login (task p3.5-40): the admin password, because
  // a login with none is the ACL login, and a repeater that has not granted our key an entry
  // answers that with nothing at all. At class `guest` - the guest door's pairing step, which
  // the server sends at that class since its task p4.7-94 (task p1.4-14) - it is the guest
  // password the claim carried, or, for a pair the door made at its empty form, the blank login
  // of the empty door itself, which a repeater with no guest password answers at guest.
  const char* material = loginMaterial(job);
  if (material != nullptr) {
    if (mesh_->hasPath()) {
      char what[80];
      std::snprintf(what, sizeof what, "login along the path the server holds (%u hop(s)), which verifies it",
                    static_cast<unsigned>(mesh_->pathHopCount()));
      const LoginResult direct = loginOnce(job, ctx, material, what);
      if (direct == LoginResult::Ok || direct == LoginResult::Refused) {
        return learned(job, ctx, direct, kPathSourceLogin, dataLen);
      }
      if (direct == LoginResult::Failed || direct == LoginResult::Timeout) {
        dataLen = writeDiscoverResult(false, nullptr, 0, nullptr, 0, nullptr, g_data, kDataCap);
        return direct == LoginResult::Failed ? Outcome::Failed : Outcome::Timeout;
      }
      // Silence: the path may be stale, so it goes and the next login floods. A flood that is
      // answered writes a fresh one; the next claim brings the server's path back anyway.
      mesh_->forgetPath();
      g_log.add("nothing came back along it: the path is cleared, so the next login floods");
    }

    const LoginResult flooded = loginOnce(job, ctx, material, "flooded login, for the path its reply comes back along");
    if (flooded == LoginResult::Failed || flooded == LoginResult::Timeout) {
      dataLen = writeDiscoverResult(false, nullptr, 0, nullptr, 0, nullptr, g_data, kDataCap);
      return flooded == LoginResult::Failed ? Outcome::Failed : Outcome::Timeout;
    }
    if (flooded == LoginResult::Ok || flooded == LoginResult::Refused) {
      if (mesh_->hasPath()) return learned(job, ctx, flooded, kPathSourceLogin, dataLen);
      g_log.add("the login was answered but no path came back with the reply");
    }
  }

  // The last resort, and the reason this handler exists: what the field found a v1.17.1
  // repeater forwarding and never answering. One try only - a login has already gone unanswered
  // by now (or the claim carried nothing to log in with), so a second flood of a different shape
  // is not worth two.
  if (!spend(job, ctx)) {
    dataLen = writeDiscoverResult(false, nullptr, 0, nullptr, 0, nullptr, g_data, kDataCap);
    return Outcome::Timeout;
  }
  uint32_t tag = 0;
  if (mesh_->sendDiscovery(tag) == SendResult::Failed) {
    g_log.add("path discovery request: the packet could not be built");
    dataLen = writeDiscoverResult(false, nullptr, 0, nullptr, 0, nullptr, g_data, kDataCap);
    return Outcome::Failed;
  }
  const uint32_t started = millis();
  if (mesh_->awaitReply(tag, requestWaitMs(job), pump_) && mesh_->hasPath()) {
    ctx.hasRtt = true;
    ctx.rttMs = static_cast<int>(millis() - started);
    g_log.add("path discovery request, the last resort (some firmware never answers it): ok, %lu ms",
              static_cast<unsigned long>(ctx.rttMs));
    return learned(job, ctx, LoginResult::NoReply, kPathSourceDiscovery, dataLen);
  }
  g_log.add("path discovery request, the last resort (some firmware never answers it): no reply");
  dataLen = writeDiscoverResult(false, nullptr, 0, nullptr, 0, nullptr, g_data, kDataCap);
  return Outcome::Unreachable;
}

// ------------------------------------------------------------------ acl.set (CONFIGURATION §8, task p4.1-60)

Outcome JobRunner::runAclSet(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  if (!job.hasAclSet || job.aclSetKey[0] == '\0' || job.aclSetLevel < 0 || job.aclSetLevel > 3) {
    g_log.add("acl.set needs a public key and a level 0..3");
    return Outcome::Failed;
  }
  const Outcome session = ensureSession(job, ctx);
  if (session != Outcome::Ok) return session;
  if (ctx.sessionRole != kPermAdmin) {
    g_log.add("logged in as %s: setperm needs admin", permissionName(ctx.sessionRole));
    return Outcome::Unauthorized;
  }

  std::snprintf(g_command, sizeof g_command, "setperm %s %d", job.aclSetKey, job.aclSetLevel);
  char what[48];
  std::snprintf(what, sizeof what, "cli 'setperm %.12s... %d'", job.aclSetKey, job.aclSetLevel);
  const Outcome sent = cliLine(job, ctx, what, g_command, true);
  if (sent != Outcome::Ok) {
    dataLen = writeAclSetResult(job.aclSetKey, job.aclSetLevel, false, nullptr, g_data, kDataCap);
    return sent;
  }
  trimInPlace(g_cliReply);
  g_log.add("the repeater answered '%s'", g_cliReply);
  const bool confirmed = startsWithOk(g_cliReply);
  if (confirmed && sameKey(job.aclSetKey, publicKey_)) {
    // The entry that was just written is our own session: on stock firmware `applyPermissions`
    // finds the same ClientInfo the login filled, so from here the repeater holds this exit node
    // at the level sent - read-only after a grant, nothing after a revoke - and the cache must
    // say the same, or the next admin-class job would reuse a session the repeater no longer
    // honours at admin. No second setperm follows: the command is the level.
    if (job.aclSetLevel == 0) {
      sessions_.invalidate(job.repeaterKey);
      ctx.hasSession = false;
    } else {
      sessions_.put(job.repeaterKey, CredentialClass::Acl, static_cast<uint8_t>(job.aclSetLevel), millis());
      ctx.sessionRole = static_cast<uint8_t>(job.aclSetLevel);
    }
    ctx.aclRestoreDone = true;
    g_log.add("the entry set is this exit node's own: the session is now %s",
              job.aclSetLevel == 0 ? "gone" : permissionName(static_cast<uint8_t>(job.aclSetLevel)));
  }
  dataLen = writeAclSetResult(job.aclSetKey, job.aclSetLevel, confirmed, g_cliReply, g_data, kDataCap);
  if (dataLen == 0) return Outcome::Failed;
  return confirmed ? Outcome::Ok : Outcome::Failed;
}

// ------------------------------------------------------------------ config.read (CONFIGURATION §3, §8)

Outcome JobRunner::readRegions(const ClaimedJob& job, Context& ctx, ConfigSnapshot& snapshot) {
  // Two commands, because the firmware prints the map in one and the default scope in another
  // (task p4-57): `region` is RegionMap::exportTo into 160 characters, and `region default` is
  // the scope a `region load` clears, which is why it is part of the value.
  Outcome got = cliLine(job, ctx, "cli 'region'", kRegionListCommand, true);
  if (got != Outcome::Ok) return got;
  std::memcpy(g_regionListing, g_cliReply, sizeof g_regionListing);

  const char* start = g_regionListing;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
  char line[kMaxConfigRawLen + 1];
  if (*start != '*') {
    // RegionMap::exportTo always starts at the wildcard root; anything else is a firmware that
    // does not know the command (`Unknown command` is a reply, not silence) or a reply this
    // version cannot read. No evidence, and the snapshot says so.
    trimInPlace(g_regionListing);
    std::snprintf(line, sizeof line, "%s: %.160s", kRegionListCommand, g_regionListing);
    snapshot.addRaw(line);
    return Outcome::Ok;
  }
  if (regionListingIsTruncated(g_regionListing)) {
    g_log.add("the region listing filled the firmware's 160-character buffer: no hierarchy is stored from this read");
    snapshot.addRaw(kRegionTruncatedLine);
    return Outcome::Ok;
  }

  got = cliLine(job, ctx, "cli 'region default'", kRegionDefaultCommand, true);
  if (got != Outcome::Ok) return got;
  char scope[kMaxConfigKeyLen + 1];
  if (!regionDefaultOf(g_cliReply, scope, sizeof scope)) {
    trimInPlace(g_cliReply);
    std::snprintf(line, sizeof line, "%s: %.160s", kRegionDefaultCommand, g_cliReply);
    snapshot.addRaw(line);
  }
  if (regionValue(g_regionListing, g_cliReply, g_regionValue, sizeof g_regionValue)) {
    snapshot.put(kRegionsKey, g_regionValue);
  }
  return Outcome::Ok;
}

Outcome JobRunner::readAccessList(const ClaimedJob& job, Context& ctx, ConfigSnapshot& snapshot) {
  // The ACL is a binary request and not a `get` (task p3.5-43): `get acl` is answered on the
  // repeater's local serial console alone. Admin only; both reserved bytes zero.
  const uint8_t data[2] = {0, 0};
  const Outcome got = request(job, ctx, "access-list request (the ACL)", kReqTypeGetAccessList, data,
                              sizeof data);
  if (got != Outcome::Ok) {
    if ((got == Outcome::Unreachable || got == Outcome::Timeout) && snapshot.valuesRead() > 0) {
      // No answer, and keys were read before it (task p4-49): the ACL is the one key whose
      // silence the firmware guarantees for a repeater that is perfectly reachable - a session
      // below admin, a firmware without the request, an ACL of 24 entries or more - so the read
      // stands, with no ACL, which is no evidence either way.
      g_log.add("no reply to the access-list request after %d keys were read: no ACL in this snapshot, and no "
                "evidence either way",
                snapshot.valuesRead());
      snapshot.addRaw(kAclSilentLine);
      return Outcome::Ok;
    }
    return got;
  }
  const MeshReply& reply = mesh_->reply();
  int count = 0;
  if (!parseAccessList(reply.data, reply.len, g_acl, kAccessListMaxEntries, count)) {
    // Something after the list that is not the padding it should be: these are not the bytes
    // this reader knows, and a short list is what revokes an association. Read no ACL.
    g_log.add("the access-list reply is not the layout this agent reads: no ACL was read from it");
    snapshot.addRaw(kAclUnreadableLine);
    return Outcome::Ok;
  }
  accessListTokens(g_acl, count, snapshot.acl, sizeof snapshot.acl);
  snapshot.hasAcl = true;
  g_log.add("the ACL has %d entr%s", count, count == 1 ? "y" : "ies");
  if (count >= kAccessListMaxEntries) {
    // A reply at the packet's ceiling: a key absent from it may never have fitted.
    snapshot.addRaw(kAclFullLine);
  }
  return Outcome::Ok;
}

Outcome JobRunner::runConfigRead(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  ConfigSnapshot& snapshot = g_snapshot;
  snapshot.clear();

  const Outcome session = ensureSession(job, ctx);
  if (session != Outcome::Ok) return session;
  if (ctx.sessionRole != kPermAdmin) {
    // The login *succeeded*, so this is not `unauthorized` at any class but admin: that outcome
    // means a refused login, and at class acl the server reads it as "the entry is gone". A `get`
    // is a CLI command the 1.17 firmware answers to admin alone.
    g_log.add("logged in as %s: this firmware answers CLI commands (get) to admin only", permissionName(ctx.sessionRole));
    return job.credentialClass == CredentialClass::Admin ? Outcome::Unauthorized : Outcome::Unsupported;
  }

  const char* const* keys = kDefaultConfigKeys;
  int keyCount = kDefaultConfigKeyCount;
  const char* asked[kMaxConfigKeys];
  if (job.hasConfigKeys) {
    for (int i = 0; i < job.configKeyCount; i++) asked[i] = job.configKeys[i];
    keys = asked;
    keyCount = job.configKeyCount;
  }
  bool wantsAcl = false;
  bool wantsRegions = false;
  for (int i = 0; i < keyCount; i++) {
    if (std::strcmp(keys[i], kAclKey) == 0) wantsAcl = true;
    if (std::strcmp(keys[i], kRegionsKey) == 0) wantsRegions = true;
  }
  g_log.add("%d key(s), %s list", keyCount, job.hasConfigKeys ? "the claim's" : "the default");

  // `regions` first, as the .NET handler reads it, so the two snapshots have the same order.
  if (wantsRegions) {
    const Outcome got = readRegions(job, ctx, snapshot);
    if (got != Outcome::Ok) {
      dataLen = writeConfigReadResult(snapshot, g_data, kDataCap);
      return got;
    }
  }

  char command[kMaxConfigKeyLen + 8];
  char what[kMaxConfigKeyLen + 16];
  char line[kMaxConfigRawLen + 1];
  for (int i = 0; i < keyCount + kConfigReadExtraCount; i++) {
    const bool extra = i >= keyCount;
    const char* key = extra ? kConfigReadExtras[i - keyCount] : keys[i];
    if (!extra && (std::strcmp(key, kAclKey) == 0 || std::strcmp(key, kRegionsKey) == 0)) continue;
    if (extra) {
      std::snprintf(command, sizeof command, "%s", key);
    } else {
      std::snprintf(command, sizeof command, "get %s", key);
    }
    std::snprintf(what, sizeof what, "cli '%s'", command);
    const Outcome got = cliLine(job, ctx, what, command, true);
    if (got != Outcome::Ok) {
      dataLen = writeConfigReadResult(snapshot, g_data, kDataCap);
      return got;
    }
    trimInPlace(g_cliReply);
    if (extra && std::strcmp(key, "ver") == 0) {
      // `ver` is "v1.17.1 (Build: 27 Sep 2025)"; the snapshot keeps the first 64 characters.
      std::snprintf(snapshot.firmware, sizeof snapshot.firmware, "%.*s", static_cast<int>(kMaxConfigTextLen), g_cliReply);
    } else if (extra) {
      std::snprintf(snapshot.board, sizeof snapshot.board, "%.*s", static_cast<int>(kMaxConfigTextLen), g_cliReply);
    } else if (const char* value = cliReplyValue(g_cliReply)) {
      snapshot.put(key, value);
    } else {
      // A refusal or an unknown key (`??: <key>`), kept verbatim so nothing the repeater said
      // is lost (CONFIGURATION §3). A raw line is the command, a colon and up to 160 characters.
      std::snprintf(line, sizeof line, "%.36s: %.160s", command, g_cliReply);
      snapshot.addRaw(line);
    }
  }

  if (wantsAcl) {
    const Outcome got = readAccessList(job, ctx, snapshot);
    if (got != Outcome::Ok) {
      dataLen = writeConfigReadResult(snapshot, g_data, kDataCap);
      return got;
    }
  }

  // The session's last command, and this result says what became of it (task p4.1-60): the ACL
  // just read lists our own key at admin, because this read's login put it there, and the server
  // must not read that as a session that died before its restore.
  const int restored = restoreAcl(job, ctx, Outcome::Ok);
  snapshot.hasAclRestored = restored >= 0;
  snapshot.aclRestored = restored == 1;
  dataLen = writeConfigReadResult(snapshot, g_data, kDataCap);
  return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
}

// ------------------------------------------------------------------ config.apply (CONFIGURATION §5, tasks p4-53, p4-57)

// The apply engine's exchange over this runner: one CLI line on the air per call, the sender
// timestamp from the RTC's unique second, the regions read the config read has, and the job's
// log. The engine decides what is sent and what a reply means; this puts it on the air once.
class JobRunner::ApplyOnTheAir : public ApplyExchange {
 public:
  ApplyOnTheAir(JobRunner& runner, const ClaimedJob& job, Context& ctx) : runner_(runner), job_(job), ctx_(ctx) {}

  Sent send(const ApplySend& s, char* reply, size_t cap) override {
    reply[0] = '\0';
    if (!s.awaitReply) {
      // A line the firmware answers with nothing: sent, and the mesh pumped so it leaves the
      // radio before the next one is queued behind it. One transaction, as the .NET agent
      // counts it.
      if (!runner_.spend(job_, ctx_, s.escape)) return Sent::LeaseOut;
      uint32_t timestamp = 0;
      const SendResult sent = runner_.mesh_->sendCli(s.line, false, timestamp);
      if (sent == SendResult::Failed) {
        runner_.scratch_.log.add("%s: the packet could not be built", s.what);
        return Sent::NotSent;
      }
      runner_.scratch_.log.add("%s: sent (%s)", s.what, sent == SendResult::Direct ? "direct" : "flood");
      runner_.idle(kLineSettleMs);
      return Sent::Answered;
    }
    switch (runner_.cliLine(job_, ctx_, s.what, s.line, s.retry, nullptr, false, s.escape)) {
      case Outcome::Ok:
        std::snprintf(reply, cap, "%s", runner_.scratch_.cliReply);
        return Sent::Answered;
      case Outcome::Unreachable: return Sent::NoReply;
      case Outcome::Timeout: return Sent::LeaseOut;
      case Outcome::Failed: return Sent::NotSent;
      default:
        // The session went and the re-login a retry makes was refused: nothing more will be
        // answered on it, which to the sequence is the silence it is.
        return Sent::NoReply;
    }
  }

  // 0: the mesh draws every line's sender timestamp at the send and hands it back (IMesh::sendCli,
  // task p1.5-15), so the engine's series is the host test's alone. Before this the engine drew
  // it here, from the same uniqueTime() the mesh draws from now - the frames are the same.
  uint32_t nextTimestamp() override { return 0; }

  bool readRegions(ConfigSnapshot& snapshot) override { return runner_.readRegions(job_, ctx_, snapshot) == Outcome::Ok; }

  void log(const char* line) override { runner_.scratch_.log.add("%s", line); }

 private:
  JobRunner& runner_;
  const ClaimedJob& job_;
  Context& ctx_;
};

Outcome JobRunner::runConfigApply(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  if (job.apply.refused) {
    g_log.add("%s", job.apply.refusedWhy);
    return Outcome::Failed;
  }
  if (job.apply.commandCount == 0) {
    g_log.add("config.apply carries no plan: nothing to send");
    return Outcome::Failed;
  }
  const Outcome session = adminSession(job, ctx);
  if (session != Outcome::Ok) return session;

  g_log.add("apply plan %s: %d command(s), single attempt, stop at the first refusal or silence",
            job.apply.planHash[0] != '\0' ? job.apply.planHash : "(no hash)", job.apply.commandCount);
  ApplyOnTheAir air(*this, job, ctx);
  const ApplyRun run = mm::runConfigApply(job.apply, air, g_applyOutcome, g_snapshot);
  dataLen = writeConfigApplyResult(job.apply, g_applyOutcome, g_snapshot, g_data, kDataCap);
  if (dataLen == 0) return Outcome::Failed;
  if (run == ApplyRun::Timeout) return Outcome::Timeout;
  return run == ApplyRun::Ok ? Outcome::Ok : Outcome::Failed;
}

// ------------------------------------------------------------------ cli, advert.request, clock.sync (task p4-54)

Outcome JobRunner::runCli(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  if (!job.hasCliCommand || job.cliCommandTooLong) {
    g_log.add("cli carries no command, more than one line, or one too long: nothing is sent");
    return Outcome::Failed;
  }
  const Outcome session = adminSession(job, ctx);
  if (session != Outcome::Ok) return session;

  // Sent once, as it was given: the blocked list is the server's, enforced on the payload
  // before the job exists, and an agent is not where a security rail lives.
  char what[kMaxCliCommandLen + 8];
  std::snprintf(what, sizeof what, "cli '%s'", job.cliCommand);
  const Outcome sent = cliLine(job, ctx, what, job.cliCommand, false);
  if (sent == Outcome::Ok) {
    trimInPlace(g_cliReply);
    const bool hardError = cliReplyIsHardError(g_cliReply);
    g_log.add("the repeater %s '%s': %s", hardError ? "refused" : "answered", job.cliCommand, g_cliReply);
    dataLen = writeCliResult(job.cliCommand, g_cliReply, g_data, kDataCap);
    if (dataLen == 0) return Outcome::Failed;
    return hardError ? Outcome::Failed : Outcome::Ok;
  }
  if (sent == Outcome::Unreachable) {
    // For a `set` this is §5a's lost reply, and the server settles it against the next read.
    g_log.add("no reply to '%s' - the command may or may not have run; not sent again", job.cliCommand);
  }
  dataLen = writeCliResult(job.cliCommand, nullptr, g_data, kDataCap);
  return sent;
}

Outcome JobRunner::runAdvertRequest(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  // `advert` floods (CommonCLI::handleCommand at d9296435, src/helpers/CommonCLI.cpp:195-198:
  // sendSelfAdvertisement(1500, true), answered `OK - Advert sent`), which is what refreshes
  // everyone's paths; `advert.zerohop` (191-194) reaches the direct neighbours only.
  const char* command = job.advertFlood ? "advert" : "advert.zerohop";
  const Outcome session = adminSession(job, ctx);
  if (session != Outcome::Ok) return session;

  char what[32];
  std::snprintf(what, sizeof what, "cli '%s'", command);
  const Outcome sent = cliLine(job, ctx, what, command, false);
  if (sent == Outcome::Ok) {
    trimInPlace(g_cliReply);
    const bool confirmed = cliReplyIsConfirmed(g_cliReply);
    g_log.add("'%s': %s", command, g_cliReply);
    dataLen = writeAdvertRequestResult(confirmed, g_cliReply, g_data, kDataCap);
    if (dataLen == 0) return Outcome::Failed;
    return confirmed ? Outcome::Ok : Outcome::Failed;
  }
  if (sent == Outcome::Unreachable) {
    g_log.add("no reply to '%s'; the advert may have gone out all the same, and the observations will say", command);
  }
  dataLen = writeAdvertRequestResult(false, nullptr, g_data, kDataCap);
  return sent;
}

Outcome JobRunner::runClockSync(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  // `time <unix>` (CommonCLI.cpp:217-227) rather than `clock sync` (199-208): both refuse to
  // go backwards, but with `time` the value set is in the command line and in the result, not
  // only in a frame field nobody stores. The value is this board's clock - SNTP, or the
  // server's time until SNTP answers - and a board with no time yet has nothing to set.
  if (!clock_->hasTime()) {
    g_log.add("this board's clock is not set yet: nothing to set the repeater's from");
    return Outcome::Failed;
  }
  const Outcome session = adminSession(job, ctx);
  if (session != Outcome::Ok) return session;

  const uint32_t setTo = clock_->now();
  char command[24];
  std::snprintf(command, sizeof command, "time %lu", static_cast<unsigned long>(setTo));
  char what[40];
  std::snprintf(what, sizeof what, "cli '%s'", command);
  const Outcome sent = cliLine(job, ctx, what, command, false);
  if (sent == Outcome::Ok) {
    trimInPlace(g_cliReply);
    const bool confirmed = cliReplyIsConfirmed(g_cliReply);
    g_log.add("'%s' (%s): %s", command, clock_->sourceName(), g_cliReply);
    dataLen = writeClockSyncResult(setTo, false, 0, confirmed, g_cliReply, g_data, kDataCap);
    if (dataLen == 0) return Outcome::Failed;
    return confirmed ? Outcome::Ok : Outcome::Failed;
  }
  if (sent == Outcome::Unreachable) g_log.add("no reply to '%s'", command);
  dataLen = writeClockSyncResult(setTo, false, 0, false, nullptr, g_data, kDataCap);
  return sent;
}

// ------------------------------------------------------------------ password.set (CONFIGURATION §7, task p4-55)

Outcome JobRunner::runPasswordSet(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  const char* field = passwordFieldName(job.passwordField);
  if (job.passwordField == PasswordField::None) {
    g_log.add("password.set names no credential field: nothing is sent");
    dataLen = writePasswordSetResult("", false, false, nullptr, g_data, kDataCap);
    return Outcome::Failed;
  }
  if (!job.credential.hasNewPassword) {
    g_log.add("the claim carried no new password: nothing is sent");
    dataLen = writePasswordSetResult(field, false, false, nullptr, g_data, kDataCap);
    return Outcome::Failed;
  }
  if (job.credential.newPasswordCut) {
    // The firmware keeps fifteen bytes (`char password[16]` in NodePrefs at d9296435): a value
    // it would cut is a lock-out, because the server would store what was sent and the repeater
    // what it kept.
    g_log.add("the new password is longer than %u bytes and the firmware keeps %u: nothing is sent, because a cut-off "
              "password is a lock-out",
              static_cast<unsigned>(kMaxPasswordLen), static_cast<unsigned>(kMaxPasswordLen));
    dataLen = writePasswordSetResult(field, false, false, nullptr, g_data, kDataCap);
    return Outcome::Failed;
  }
  const Outcome session = adminSession(job, ctx);
  if (session != Outcome::Ok) {
    dataLen = writePasswordSetResult(field, false, false, nullptr, g_data, kDataCap);
    return session;
  }

  // The command line as bytes - prefix and secret - built right before the send and wiped right
  // after it; the log names the command without its value, and the frame log stars the body
  // (`sensitive`). `password <new>` is CommonCLI.cpp:256-261 (admin), `set guest.password <new>`
  // is handleSetCmd at 506-509 (guest); the first echoes what it stored, `password now: …`.
  const bool admin = job.passwordField == PasswordField::Admin;
  const char* what = admin ? "cli 'password <new>'" : "cli 'set guest.password <new>'";
  std::snprintf(g_secretLine, sizeof g_secretLine, "%s%s", admin ? "password " : "set guest.password ",
                job.credential.newPassword);
  const Outcome sent = cliLine(job, ctx, what, g_secretLine, false, 0, true);
  wipeBytes(g_secretLine, sizeof g_secretLine);

  // What the reply means, decided while the reply is still here and before the restore's own
  // reply overwrites it. The echo is compared with what was sent and never recorded: the result
  // says `password now: <set>`, and the reply buffer is wiped once the comparison is made.
  bool confirmed = false;
  char recorded[kMaxCliReplyLen + 1] = {0};
  bool hasRecorded = false;
  if (sent == Outcome::Ok) {
    trimInPlace(g_cliReply);
    hasRecorded = true;
    if (admin) {
      const char* echo = "password now: ";
      const size_t echoLen = std::strlen(echo);
      if (std::strncmp(g_cliReply, echo, echoLen) == 0) {
        confirmed = std::strcmp(g_cliReply + echoLen, job.credential.newPassword) == 0;
        std::snprintf(recorded, sizeof recorded, "%s",
                      confirmed ? kPasswordSetAdminReplyRedacted : "password now: <a different value>");
        g_log.add(confirmed ? "the repeater confirmed the new admin password"
                            : "the repeater stored a different admin password from the one sent (it may have cut it): not "
                              "confirmed");
      } else {
        std::snprintf(recorded, sizeof recorded, "%s", g_cliReply);
        g_log.add("the repeater answered 'password <new>' with: %s", g_cliReply);
      }
    } else {
      confirmed = cliReplyIsConfirmed(g_cliReply);
      std::snprintf(recorded, sizeof recorded, "%s", g_cliReply);
      g_log.add("the repeater %s 'set guest.password <new>': %s", confirmed ? "confirmed" : "refused", g_cliReply);
    }
  } else if (sent == Outcome::Unreachable) {
    g_log.add("no reply to '%s' - the password may or may not have changed; not sent again", what);
  }
  wipeBytes(g_cliReply, sizeof g_cliReply);

  // The restore **after** the password line, on the same session (task p4.1-60 as built): the
  // login table is the ACL, which a new password does not touch, so the entry can still be put
  // back - and before the line it could not, because the entry *is* the session and a demoted
  // session is not answered a CLI line. Only then is the cached session dropped, whatever came
  // back: it was opened with a password that may no longer be valid, and a later job on it would
  // be a login the server did not decide.
  restoreAcl(job, ctx, sent);
  sessions_.invalidate(job.repeaterKey);
  ctx.hasSession = false;
  g_log.add("the session is dropped: it was opened with a password that may no longer be valid");

  const bool wasSent = sent == Outcome::Ok || sent == Outcome::Unreachable;
  dataLen = writePasswordSetResult(field, confirmed, wasSent, hasRecorded ? recorded : nullptr, g_data, kDataCap);
  if (dataLen == 0) return Outcome::Failed;
  if (sent == Outcome::Ok) return confirmed ? Outcome::Ok : Outcome::Failed;
  return sent;
}

// ------------------------------------------------------------------ reboot (task p4-54)

Outcome JobRunner::runReboot(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  const Outcome session = adminSession(job, ctx);
  if (session != Outcome::Ok) {
    dataLen = writeRebootResult(false, nullptr, g_data, kDataCap);
    return session;
  }

  // The one admin type that cannot end with the restore (task p4.1-60 as built): there is no
  // session after a reboot to send it on, and it cannot go before the reboot either - on stock
  // firmware the entry *is* the session, so `setperm <own key> 1` first would leave a read-only
  // session, and onPeerDataRecv drops a CLI line from anybody below admin without a word. The
  // entry stays at admin until the next login from this key puts it back.
  ctx.aclRestoreDone = true;
  g_log.add("no restore on this session: the repeater is asked to reboot on it, and a restore before the command would "
            "make the command unanswerable; the next login from this key puts the entry back (task 60)");

  // `reboot` is `_board->reboot()`, which does not return (CommonCLI.cpp:185-186): silence is
  // the expected answer, and the next status read confirms it by the uptime, on the server.
  const Outcome sent = cliLine(job, ctx, "cli 'reboot'", "reboot", false);
  if (sent == Outcome::Unreachable) {
    sessions_.invalidate(job.repeaterKey);
    ctx.hasSession = false;
    g_log.add("reboot sent; the firmware does not answer one, so the next status read confirms it by the uptime");
    dataLen = writeRebootResult(true, nullptr, g_data, kDataCap);
    return dataLen > 0 ? Outcome::Ok : Outcome::Failed;
  }
  if (sent == Outcome::Ok) {
    sessions_.invalidate(job.repeaterKey);
    ctx.hasSession = false;
    trimInPlace(g_cliReply);
    const bool hardError = cliReplyIsHardError(g_cliReply);
    g_log.add(hardError ? "the repeater refused 'reboot': %s" : "reboot sent, and the repeater said something before it went: %s",
              g_cliReply);
    dataLen = writeRebootResult(true, g_cliReply, g_data, kDataCap);
    if (dataLen == 0) return Outcome::Failed;
    return hardError ? Outcome::Failed : Outcome::Ok;
  }
  dataLen = writeRebootResult(false, nullptr, g_data, kDataCap);
  return sent;
}

// ------------------------------------------------------------------ the runner

Outcome JobRunner::runJob(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  dataLen = 0;
  // The lease first, before anything is decided about the job (task 06). A claim's jobs are run
  // one at a time and each is minutes of airtime, so the second of a pair - or every job of a
  // claim the server went away under - can be reached after the lease the server gave it has
  // run out. Nothing is put on the air for a lease somebody else may hold now: the job is
  // `timeout` at no transaction, and the result is posted, late if it must be.
  if (leaseLapsed(job.leaseUntil, clock_->hasTime(), clock_->now())) {
    g_log.add("the lease ran out before this job started, so nothing was sent: the server has given it back to the "
              "queue and this result is the record of that");
    LOG_W("jobs: job %lld %s: the lease had already run out; nothing was sent", job.id, job.type);
    return Outcome::Timeout;
  }
  // A type that is none of the thirteen is the extension's to run or decline (IJobExtension.h),
  // and the class rules of canRun() are the thirteen's; with no extension set it is `unsupported`
  // there, as before.
  const bool offered = extension_ != nullptr && !isKnownJobType(job.type);
  char why[200] = {0};
  if (!offered && !canRun(job, why, sizeof why)) {
    g_log.add("%s", why);
    return Outcome::Unsupported;
  }
  // The mesh's own answer for one of the thirteen (IMesh::supports, task p1.6-23). A type it
  // cannot carry is not advertised, so the server routes none of it here - but a job claimed
  // before a heartbeat said so, or one bound to this agent, can still arrive, and it is
  // `unsupported` at no transaction with nothing sent, as a type nobody handles is. Asked
  // before beginJob(), so the mesh is never given a job it has already declined.
  if (!offered && !mesh_->supports(job.type)) {
    g_log.add("this mesh cannot carry a '%s' job, so it is not advertised and nothing was sent", job.type);
    return Outcome::Unsupported;
  }

  uint8_t hashSize = 0;
  const uint8_t hops = pathToBytes(job, g_pathBytes, sizeof g_pathBytes, hashSize);
  if (!mesh_->beginJob(job.repeaterKey, hops > 0 ? g_pathBytes : nullptr, hops, hashSize)) {
    g_log.add("the repeater's public key is not 64 hex characters");
    return Outcome::Failed;
  }
  if (hops == 0 && job.hasPath && job.pathCount == 0) mesh_->setDirectPath();  // a direct neighbour
  g_log.add("path %s", mesh_->hasPath() ? "from the server" : "unknown: the mesh floods");

  if (std::strcmp(job.type, "status") == 0) return runStatus(job, ctx, dataLen);
  if (std::strcmp(job.type, "telemetry") == 0) return runTelemetry(job, ctx, dataLen);
  if (std::strcmp(job.type, "neighbours") == 0) return runNeighbours(job, ctx, dataLen);
  if (std::strcmp(job.type, "trace") == 0) return runTrace(job, ctx, dataLen);
  if (std::strcmp(job.type, "discover") == 0) return runDiscover(job, ctx, dataLen);
  if (std::strcmp(job.type, "acl.set") == 0) return runAclSet(job, ctx, dataLen);
  if (std::strcmp(job.type, "config.read") == 0) return runConfigRead(job, ctx, dataLen);
  if (std::strcmp(job.type, "config.apply") == 0) return runConfigApply(job, ctx, dataLen);
  if (std::strcmp(job.type, "cli") == 0) return runCli(job, ctx, dataLen);
  if (std::strcmp(job.type, "advert.request") == 0) return runAdvertRequest(job, ctx, dataLen);
  if (std::strcmp(job.type, "clock.sync") == 0) return runClockSync(job, ctx, dataLen);
  if (std::strcmp(job.type, "password.set") == 0) return runPasswordSet(job, ctx, dataLen);
  if (std::strcmp(job.type, "reboot") == 0) return runReboot(job, ctx, dataLen);
  if (offered) return runExtension(job, ctx, dataLen);
  return Outcome::Unsupported;
}

// ------------------------------------------------------------------ the extension (IJobExtension.h)

// What the runner lends an extension for one job, and where the extension's result lands: the
// pacing and the lease through spend(), the job's log, and the data buffer every handler writes
// into. One class for both halves, because they are one job's.
class JobRunner::ExtensionJob : public JobContext, public ResultWriter {
 public:
  ExtensionJob(JobRunner& runner, const ClaimedJob& job, Context& ctx, size_t& dataLen)
      : runner_(runner), job_(job), ctx_(ctx), dataLen_(dataLen) {}

  IMesh& mesh() override { return *runner_.mesh_; }
  IPlatform& platform() override { return *runner_.platform_; }
  bool spend() override { return runner_.spend(job_, ctx_); }
  uint32_t replyWaitMs() const override { return runner_.requestWaitMs(job_); }
  void log(const char* line) override { runner_.scratch_.log.add("%s", line); }

  bool data(const char* json, size_t len) override {
    if (json == nullptr || len == 0 || len >= kDataCap) {
      dataLen_ = 0;
      return false;
    }
    std::memcpy(runner_.scratch_.data, json, len);
    runner_.scratch_.data[len] = '\0';
    dataLen_ = len;
    return true;
  }
  void outcome(Outcome o) override { outcome_ = o; }
  Outcome result() const { return outcome_; }

 private:
  JobRunner& runner_;
  const ClaimedJob& job_;
  Context& ctx_;
  size_t& dataLen_;
  Outcome outcome_ = Outcome::Failed;
};

Outcome JobRunner::runExtension(const ClaimedJob& job, Context& ctx, size_t& dataLen) {
  ExtensionJob ext(*this, job, ctx, dataLen);
  if (!extension_->run(job, ext, ext)) {
    dataLen = 0;
    g_log.add("no handler for job type '%s' in this firmware or its extension", job.type);
    return Outcome::Unsupported;
  }
  return ext.result();
}

bool JobRunner::post(long long jobId, const char* body, size_t len) {
  // Anything already waiting goes first, so the server sees the results in the order they were
  // made; a failure there - or a wake's byte cap - leaves this one behind the queue rather than
  // in front of it. The flush is the bounded one and there is no other (task p1.6-21): a wake's
  // budget covers the job step's posts as it covers the rest.
  const uint32_t failuresBefore = postFailures_;
  flushResults();
  if (results_.empty() && mayPost(len)) {
    if (spend(len, [&] { return server_->postJobResult(jobId, body, len); })) return true;
    postFailures_++;
  } else if (postFailures_ == failuresBefore) {
    // Not the server: what stopped this was the wake's own bytes or its uplink seconds, here or
    // in the queue ahead of it. The result is **kept: budget** - behind the queue, in the order
    // it was made - and that is a bound of ours and not a failure of the server's, so nothing
    // backs off for it and the next wake posts it with the timestamps it was made with.
    resultsKeptForBudget_++;
    LOG_I("jobs: the result of job %lld is kept: budget (%u byte(s), %u left this wake)", jobId,
          static_cast<unsigned>(len), static_cast<unsigned>(meter_ == nullptr ? 0 : meter_->bytesLeft()));
  }
  // Kept with its client id and retried: the server de-duplicates on that id, and a late result
  // is still taken (API §3). The buffer is RAM and bounded, so a long outage drops the oldest
  // and counts it - a board has no journal (EXIT-NODES §5).
  const uint32_t droppedBefore = results_.dropped();
  if (results_.push(jobId, body, len)) {
    LOG_W("jobs: the result of job %lld was not taken; it is kept (%u waiting) and sent again", jobId,
          static_cast<unsigned>(results_.count()));
  } else {
    LOG_E("jobs: the result of job %lld is larger than the whole offline buffer and is lost", jobId);
  }
  if (results_.dropped() > droppedBefore) {
    LOG_W("jobs: the offline buffer is full; %lu result(s) dropped in all, oldest first",
          static_cast<unsigned long>(results_.dropped()));
  }
  return false;
}

void JobRunner::markPosted(long long jobId) {
  for (int i = 0; i < recentCount_; i++) {
    if (recent_[i].id == jobId) recent_[i].posted = true;
  }
}

int JobRunner::flushResults() {
  int posted = 0;
  while (!results_.empty()) {
    long long jobId = 0;
    size_t len = 0;
    const char* body = results_.oldest(jobId, len);
    if (body == nullptr) return posted;
    // A wake on a metered uplink says how many bytes it may send and how long its calls together
    // may take; what does not fit either stays in the buffer with the timestamps and the client
    // id it was made with, oldest first, and the next wake posts it (task p1.5-18, p1.6-21).
    if (!mayPost(len)) {
      if (meter_ != nullptr && !meter_->timeLeft()) {
        LOG_I("jobs: the kept result of job %lld waits: this wake's seconds at the server are spent", jobId);
      } else {
        LOG_I("jobs: the kept result of job %lld is %u bytes and this wake has %u left; it waits", jobId,
              static_cast<unsigned>(len),
              static_cast<unsigned>(meter_ == nullptr ? 0 : meter_->bytesLeft()));
      }
      return posted;
    }
    if (!spend(len, [&] { return server_->postJobResult(jobId, body, len); })) {
      postFailures_++;
      return posted;
    }
    posted++;
    LOG_I("jobs: the kept result of job %lld went through", jobId);
    results_.popOldest();
    markPosted(jobId);
  }
  return posted;
}

void JobRunner::remember(const ClaimedJob& job, Outcome outcome, int transactions, bool posted) {
  for (int i = kRemembered - 1; i > 0; i--) recent_[i] = recent_[i - 1];
  Record& r = recent_[0];
  r = Record();
  r.id = job.id;
  std::snprintf(r.type, sizeof r.type, "%s", job.type);
  r.outcome = outcome;
  r.transactions = transactions;
  r.atEpoch = clock_->now();
  r.posted = posted;
  if (recentCount_ < kRemembered) recentCount_++;
}

int JobRunner::claimAndRun() {
  flushResults();

  const int count = claim(kClaimMax);
  if (count <= 0) return count;
  for (int i = 0; i < count; i++) runClaimed(i);
  endClaim();
  return count;
}

int JobRunner::claim(int max) {
  if (max > kClaimMax) max = kClaimMax;
  if (max < 1) max = 1;

  // Whatever a previous claim left: no password outlives its job, and none waits for the next.
  for (int i = 0; i < kClaimMax; i++) g_jobs[i].credential.wipe();
  claimed_ = 0;

  int count = 0;
  if (!spend(0, [&] { return server_->claimJobs(g_jobs, max, count); })) {
    for (int i = 0; i < kClaimMax; i++) g_jobs[i].credential.wipe();
    return -1;
  }
  claims_++;
  claimed_ = count;
  if (count == 0) {
    LOG_D("jobs: the claim came back with nothing");
    return 0;
  }
  LOG_I("jobs: %d claimed", count);
  return count;
}

void JobRunner::endClaim() {
  for (int i = 0; i < kClaimMax; i++) g_jobs[i].credential.wipe();
  claimed_ = 0;
}

void JobRunner::runClaimed(int i) {
  if (i < 0 || i >= claimed_) return;
  ClaimedJob& job = g_jobs[i];
  const uint32_t startedAt = clock_->now();
  g_log.clear();
  g_log.add("job %lld %s for repeater %lld \"%s\" (%s)", job.id, job.type, job.repeaterId, job.repeaterLabel,
            credentialClassName(job.credentialClass));
  for (int g = 0; g < job.groupCount; g++) {
    // The other jobs of this claim for the same repeater: they share one login (EXIT-NODES §8),
    // which on this board is the session cache doing it rather than anything this loop does.
    if (i + 1 < claimed_ && sharesLogin(job, g_jobs[i + 1])) {
      g_log.add("grouped with job %lld: one login for the pair", job.groupWith[g]);
      break;
    }
  }
  LOG_I("jobs: job %lld %s for repeater %lld \"%s\" (%s)", job.id, job.type, job.repeaterId, job.repeaterLabel,
        credentialClassName(job.credentialClass));

  Context ctx;
  ctx.job = &job;
  // The display's fourth line, for as long as this job lasts: the pump refreshes the panel
  // through every wait, so "Job 1234 acl.set" is live (src/ui/DisplayLines.h).
  open_ = &job;
  if (job.credentialCandidate[0] != '\0') {
    // The material is one of the two candidates of an unverified credential (task p4-55), and
    // the server reads this job's outcome as the verdict on it: a session another job opened
    // with another password would make that verdict a lie, so this job logs in afresh
    // whatever the cache holds.
    sessions_.invalidate(job.repeaterKey);
    g_log.add("the claim carries the %s candidate of an unverified credential: logging in afresh with it",
              job.credentialCandidate);
  }
  size_t dataLen = 0;
  Outcome outcome = runJob(job, ctx, dataLen);

  // The session's last command (task p4.1-60): an admin session on a repeater where this exit
  // node holds a grant puts its own entry back to the granted level before the job is over,
  // whatever the job came to. A config.read has sent it already, for its result's sake; a
  // password.set has sent it after its password line and dropped the session; a reboot has
  // said why it sends none; an acl.set on our own key *is* the level and needs no second one.
  if (std::strcmp(job.type, "acl.set") != 0) restoreAcl(job, ctx, outcome);

  // The password's job is over. Whatever the outcome, and before anything is posted.
  job.credential.wipe();

  // A login answered below the class carries the tier it was answered at, for a type that has
  // no result of its own to say it (task p4.1-60).
  if (dataLen == 0 && outcome == Outcome::Unauthorized && ctx.grantedBelowClass) {
    dataLen = writeLoginBelowClassResult(permissionName(ctx.grantedRole),
                                         permissionName(roleNeededFor(job.credentialClass)), g_data, kDataCap);
  } else if (dataLen == 0 && outcome == Outcome::Unauthorized && ctx.silentAfterReachable) {
    // The candidate's silence right after a blank login the repeater answered (task p1.3-13):
    // the server reads `reason` and treats it exactly as a refusal, notes the candidate
    // refused, and hands the next attempt the old one (CONFIGURATION §7 step 4).
    dataLen = writeLoginSilentResult(
        ctx.probeRefused ? kProbeAnsweredRefused : permissionName(ctx.probeAnsweredRole), g_data, kDataCap);
  }

  // The path the mesh holds *after* the exchange, which is the claim's own unless a flooded
  // reply taught a better one on the way (EXIT-NODES §3: every result reports what path was
  // used). Read before the job is closed, because closing it forgets the peer.
  const bool hadPath = mesh_->hasPath();
  const int pathCount = mesh_->describePath(g_pathHops, kMaxClaimPathHops);
  for (int h = 0; h < pathCount; h++) g_pathPtrs[h] = g_pathHops[h];
  mesh_->endJob();

  char clientId[48];
  std::snprintf(clientId, sizeof clientId, "%lld-%08" PRIx32 "%08" PRIx32 "%08" PRIx32, job.id,
                platform_->random(), platform_->random(), platform_->random());

  JobReport report;
  report.clientId = clientId;
  report.outcome = outcome;
  report.startedAt = startedAt;
  report.finishedAt = clock_->now();
  // No path known is not the same as a direct neighbour: the first leaves both fields out, as
  // the .NET runner does for a contact the companion has no path for; the second sends an
  // empty array and a zero.
  report.hasPathUsed = hadPath;
  report.pathUsed = g_pathPtrs;
  report.pathCount = pathCount;
  report.hasHops = hadPath;
  report.hops = pathCount;
  report.hasRttMs = ctx.hasRtt;
  report.rttMs = ctx.rttMs;
  report.transactions = ctx.transactions;
  report.data = dataLen > 0 ? g_data : nullptr;
  report.log = g_log.text();

  LOG_I("jobs: job %lld: %s after %d transaction(s)", job.id, outcomeName(outcome), ctx.transactions);
  const size_t len = writeJobResult(report, g_body, kBodyCap);
  bool posted = false;
  if (len == 0) {
    LOG_E("jobs: the result of job %lld did not fit in %u bytes", job.id, static_cast<unsigned>(kBodyCap));
  } else {
    posted = post(job.id, g_body, len);
  }
  ran_++;
  remember(job, outcome, ctx.transactions, posted);
  open_ = nullptr;
}

// A job this wake has no room for: `failed`, with `why` in its log, at no transaction and with
// nothing sent. The lease the claim handed over is answered - a job claimed is never left leased
// and silent (task p1.5-18) - and the result is posted, or buffered like any other the server
// would not take.
void JobRunner::reportNotRun(int i, const char* why) {
  if (i < 0 || i >= claimed_) return;
  ClaimedJob& job = g_jobs[i];
  const uint32_t at = clock_->now();
  g_log.clear();
  g_log.add("job %lld %s for repeater %lld \"%s\" (%s)", job.id, job.type, job.repeaterId, job.repeaterLabel,
            credentialClassName(job.credentialClass));
  g_log.add("%s", why == nullptr ? "not run" : why);
  // Nothing goes on the air for this job, so the password it carried is done with here.
  job.credential.wipe();

  char clientId[48];
  std::snprintf(clientId, sizeof clientId, "%lld-%08" PRIx32 "%08" PRIx32 "%08" PRIx32, job.id, platform_->random(),
                platform_->random(), platform_->random());
  JobReport report;
  report.clientId = clientId;
  report.outcome = Outcome::Failed;
  report.startedAt = at;
  report.finishedAt = at;
  report.transactions = 0;
  report.log = g_log.text();

  LOG_W("jobs: job %lld %s was not run: %s", job.id, job.type, why == nullptr ? "not run" : why);
  const size_t len = writeJobResult(report, g_body, kBodyCap);
  bool posted = false;
  if (len == 0) {
    LOG_E("jobs: the result of job %lld did not fit in %u bytes", job.id, static_cast<unsigned>(kBodyCap));
  } else {
    posted = post(job.id, g_body, len);
  }
  remember(job, Outcome::Failed, 0, posted);
}

void JobRunner::report(TextOut& out) const {
  out.printf("budget    %d/min, gap %d s; %lu transaction(s) sent, %lu s waited for the budget\r\n",
             gate_.budget().maxTransactionsPerMinute, gate_.budget().minGapSeconds,
             static_cast<unsigned long>(gate_.transmissions()), static_cast<unsigned long>(gate_.totalWaitMs() / 1000));
  out.printf("claims    %lu made, %lu job(s) run, %lu result(s) not taken first time\r\n",
             static_cast<unsigned long>(claims_), static_cast<unsigned long>(ran_),
             static_cast<unsigned long>(postFailures_));
  out.printf("buffer    %u result(s) waiting of %u, %u of %u bytes; %lu dropped, %lu held in all\r\n",
             static_cast<unsigned>(results_.count()), static_cast<unsigned>(ResultBuffer::kMaxResults),
             static_cast<unsigned>(results_.bytes()), static_cast<unsigned>(ResultBuffer::kCapacityBytes),
             static_cast<unsigned long>(results_.dropped()), static_cast<unsigned long>(results_.held()));
  out.print("types     ");
  for (int i = 0; i < kJobTypeCount; i++) out.printf("%s%s", i > 0 ? ", " : "", kJobTypes[i]);
  out.print("\r\n");
  if (!results_.empty()) {
    long long jobId = 0;
    size_t len = 0;
    results_.oldest(jobId, len);
    out.printf("pending   the result of job %lld is the next to go, %u byte(s)\r\n", jobId,
               static_cast<unsigned>(len));
  }
  if (recentCount_ == 0) {
    out.println("last ten  none yet");
    return;
  }
  out.println("last ten  newest first: id, type, outcome, transactions, when");
  char when[24];
  for (int i = 0; i < recentCount_; i++) {
    const Record& r = recent_[i];
    formatClock(r.atEpoch, when, sizeof when);
    out.printf("  %-8lld %-11s %-13s %d tx  %s%s\r\n", r.id, r.type, outcomeName(r.outcome), r.transactions, when,
               r.posted ? "" : "  (not posted)");
  }
}

void JobRunner::reportSessions(TextOut& out) const {
  const uint32_t now = millis();
  out.printf("sessions  %u live of %u slots, lifetime %lu s; a repeater's session is a key, a class, a level and an "
             "age - no secret is in one\r\n",
             static_cast<unsigned>(sessions_.live(now)), static_cast<unsigned>(SessionStore::kMaxSessions),
             static_cast<unsigned long>(SessionStore::kLifetimeMs / 1000));
  bool any = false;
  for (size_t i = 0; i < SessionStore::kMaxSessions; i++) {
    const RepeaterSession* s = sessions_.slot(i, now);
    if (s == nullptr) continue;
    if (!any) out.println("live      repeater key (first 12), class, level, age");
    any = true;
    out.printf("  %.12s...  %-6s %-10s %lu s old\r\n", s->key, credentialClassName(s->credentialClass), s->tier(),
               static_cast<unsigned long>((now - s->loggedInAtMs) / 1000));
  }
  if (!any) out.println("live      none");
}

void JobRunner::describe(char* out, size_t cap) const {
  if (ran_ == 0) {
    std::snprintf(out, cap, "none run yet; budget %d/min gap %d s", gate_.budget().maxTransactionsPerMinute,
                  gate_.budget().minGapSeconds);
    return;
  }
  const Record& r = recent_[0];
  std::snprintf(out, cap, "%lu run, %lu transaction(s); last job %lld %s %s after %d tx", static_cast<unsigned long>(ran_),
                static_cast<unsigned long>(gate_.transmissions()), r.id, r.type, outcomeName(r.outcome),
                r.transactions);
}

#undef g_jobs
#undef g_data
#undef g_body
#undef g_log
#undef g_neighbours
#undef g_readings
#undef g_hops
#undef g_pathHops
#undef g_pathPtrs
#undef g_pathBytes
#undef g_roundTrip
#undef g_snapshot
#undef g_acl
#undef g_cliReply
#undef g_regionListing
#undef g_regionValue
#undef g_command
#undef g_applyOutcome
#undef g_secretLine

}  // namespace mm
