// One job at a time: claim, run, report (EXIT-NODES §5, §6, §7).
//
// The .NET agent's JobRunner has a companion on a cable and a handler per type; here the board
// **is** the companion, so each handler is a direct call into the mesh - the same primitives the
// companion firmware runs when a phone asks, behind IMesh (IMesh.h) since task p1.5-15 - and the
// reply arrives in the callbacks src/mesh/AgentMesh.h has. Simpler on the wire, and the same
// result shapes on the server: what is posted is held to the .NET agent's bytes by
// test/test_job_results and test/test_config_read.
//
// Thirteen types. Four of them log in as this board and nothing else (task 03):
//
//   status      REQ_TYPE_GET_STATUS, and the owner-info request beside it when the claim asks
//               for the reply - at any class since meshmaster.dk's task p4.30-181, for the
//               firmware version the association's verify step records and for the node name and
//               the owner info the repeater answers to anybody who logs in
//   telemetry   REQ_TYPE_GET_TELEMETRY_DATA, Cayenne LPP decoded with the channel-0 rule
//   neighbours  REQ_TYPE_GET_NEIGHBOURS, nine a page, eight-byte prefixes, paged to the count
//   trace       a trace packet along the round trip; `unsupported` at no transaction when the
//               path is built of three-byte hashes, which a trace packet cannot express
//
// and three carry the repeater's admin password for the length of the job (task 04; where it
// lives and what may see it is agent/Credentials.h):
//
//   discover    an admin login - direct along a known path, which verifies it; flooded when
//               none is known, and the path its reply came back along - and the path-discovery
//               request last, only when neither login was answered (tasks p3.5-39, 40, 41)
//   acl.set     `setperm <key> <level>` on the admin session, the reply captured; on our own
//               key the command is the session's level (task p4.1-60), it is sent **once** and
//               its silence is read back with a blank login (task p1.10-48), and a confirmed
//               grant keeps no session, so the verify logs in by key
//   config.read the `get` list in order, `region` and `region default`, `ver` and `board`, and
//               the access-list request last (admin only, 23 entries at most, its silence after
//               keys were read tolerated: tasks p3.5-43, p4-49, p4-57)
//
// and six change a repeater (task 05; tasks p4-53, 54, 55, 57), every one of them one send per
// command - a `set` sent twice is a change made twice - on an admin session, with the sender
// timestamps strictly increasing (the RTC's unique second, which every tag and every CLI line of
// this board come from):
//
//   config.apply  the plan's commands in order, each exactly once, every reply kept, stopped at
//                 the first reply that is not what the line expects and at the first silence
//                 (`not-sent` for the rest); a regions entry as its lines, the ones that expect
//                 no reply sent without waiting and the terminator sent even when the sequence
//                 dies; then the re-read of every touched key and `ver`. The policy is
//                 agent/ConfigApply.h, which runs on a host; this file puts its lines on the air
//   cli           one line as the server handed it, one reply, both recorded; the blocked list
//                 is the server's, and this firmware refuses nothing but a malformed line
//   advert.request  `advert` (the flood) or `advert.zerohop`, answered `OK - Advert sent`
//   clock.sync    `time <unix>` from this board's clock, the reply recorded, a refusal
//                 (`(ERR: clock cannot go backwards)`) recorded as the refusal it is
//   password.set  `password <new>` or `set guest.password <new>` with the value the claim
//                 carried, once; the admin echo compared with what was sent and recorded as
//                 `<set>`, never the value; the restore **after** the password line, and the
//                 session dropped whatever came back
//   reboot        `reboot`, which the firmware never answers: `ok` as sent on silence, no
//                 restore (there is no session after it, and before it a restore would make the
//                 command unanswerable), the session dropped
//
// A fourteenth and after: a type that is none of the thirteen is offered to the extension the
// board set, if any (IJobExtension.h), before it is answered `unsupported`. The runner still owns
// the pacing, the lease, the envelope and the log; the extension writes the data and the outcome.
//
// And one of the thirteen the mesh cannot carry (IMesh::supports, task p1.6-23) - a trace over a
// link that has no request for one - is `unsupported` at no transaction too, with nothing sent
// and the job never begun on the mesh; the heartbeat did not advertise it either.
//
// **Every admin session on a repeater where this exit node holds a grant ends with the restore**
// (task p4.1-60): `setperm <own key> <granted level>` as the session's last command, because on
// stock firmware the login table *is* the ACL and the admin login has just rewritten our entry
// to admin. The level travels in the claim's `acl`; after the command the cache holds the
// session at class `acl`, reusable by the next acl-class job and invisible to an admin-class
// one. A blank login the repeater answered *above* the grant restores the same way, at once. An
// `acl.set` on our own key sends no second `setperm`: the command is the level. Two types cannot
// end that way (task 60 as built): `password.set` restores after its password line and only then
// drops the session, and `reboot` restores not at all.
//
// **And that line cannot be asked twice** (task p1.10-48): it is the session's own level, so a
// repeater that ran it drops the retry without a word. The restore and the grant therefore send
// it once and read its silence back with a blank login - two rounds at most - which is what
// `writeOwnEntry` below is, and what took meshmaster.dk's *elevated* and its 20 failed grants of
// 12 November 2026 off the air.
//
// The pacing is the budget the heartbeat last carried (agent/Airtime.h): one transaction at a
// time, the gap between two, the ceiling in a minute. A board has one thread, so a job's waiting
// is the loop's waiting - `pump` is what keeps the console and the OLED alive through it.
//
// Host-portable since task p1.5-15: the mesh, the server and the platform are the three seams
// (IMesh.h, IUplink.h, IPlatform.h), and the scratch every job shares is this object's own
// (Scratch, below) rather than a translation unit's statics.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <agent/Airtime.h>
#include <agent/CliReplies.h>
#include <agent/ConfigApply.h>
#include <agent/ConfigRead.h>
#include <agent/FloodScope.h>
#include <agent/IJobExtension.h>
#include <agent/IMesh.h>
#include <agent/IPlatform.h>
#include <agent/IUplink.h>
#include <agent/JobEnvelopes.h>
#include <agent/LoginSequence.h>
#include <agent/ResultBuffer.h>
#include <agent/Sessions.h>
#include <agent/TextOut.h>
#include <agent/UplinkMeter.h>

namespace mm {

// A result's free text, bounded here rather than at the envelope: 4 000 characters is what API §3
// allows and a kilobyte is more than any of these jobs has to say. When it fills, the oldest
// entries go - what a job did last is what a reader of its page wants. Nothing that could need
// redacting ever reaches it: a password is never an argument of any line the runner adds, and
// the .NET agent's redactor has nothing to do here because there is nothing to redact.
//
// **One entry per line** (task p1.10-38), as MeshMaster.Agent's own `JobLog.Append` writes it:
// *The agent's log* on a job's page is a `<pre>` with `whitespace-pre-wrap`, so it keeps the
// line breaks and reads line by line. A `"; "` between entries could not, because several
// entries have a `"; "` of their own inside them and the page cannot tell the two apart. The
// JSON writer escapes the `'\n'` as `\n`, so one raw byte is still two on the wire - the width
// `"; "` had - and API §3's 4 000-character bound is untouched.
class JobLog {
 public:
  void clear() {
    len_ = 0;
    buf_[0] = '\0';
  }

  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (len_ + 2 >= sizeof buf_) drop();
    if (len_ > 0) {
      buf_[len_++] = '\n';
      buf_[len_] = '\0';
    }
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(&buf_[len_], sizeof buf_ - len_, fmt, args);
    va_end(args);
    if (n > 0) {
      len_ += static_cast<size_t>(n) < sizeof buf_ - len_ ? static_cast<size_t>(n) : sizeof buf_ - len_ - 1;
    }
    buf_[len_] = '\0';
  }

  const char* text() const { return buf_; }

 private:
  // Whole entries go, not bytes: the cut is the first line break at or after the half, so what
  // is left begins where an entry begins and the first line a reader sees is a whole one. An
  // entry longer than half the buffer has no line break to cut at, and is cut at the half as it
  // always was - there is nothing better to do with it.
  void drop() {
    size_t cut = sizeof buf_ / 2;
    for (size_t i = cut; i < len_; i++) {
      if (buf_[i] == '\n') {
        cut = i + 1;
        break;
      }
    }
    std::memmove(buf_, buf_ + cut, len_ - cut + 1);
    len_ -= cut;
  }

  char buf_[1024] = {0};
  size_t len_ = 0;
};

class JobRunner {
 public:
  // What `jobs` prints: the last ten, newest first.
  struct Record {
    long long id = 0;
    char type[kMaxJobTypeLen + 1] = {0};
    Outcome outcome = Outcome::Failed;
    int transactions = 0;
    uint32_t atEpoch = 0;
    bool posted = false;
  };
  static constexpr int kRemembered = 10;

  // The result's data and the whole body. A config read is the largest result here, and since
  // meshmaster.dk's task p4.9-102 the read it is asked for is the Configuration tab's whole list
  // - about twenty-six keys on a V3 at v1.17, `owner.info`'s 119 characters among them, the
  // regions listing, the ACL's 23 tokens and up to twelve raw lines for the keys that firmware
  // does not know. That is some 4.2 KB of JSON, which 4 608 bytes held only just; 8 192 holds it
  // twice over.
  //
  // It is **not** sized for the worst case the constants allow (some 10.9 KB unescaped, and
  // System.Text.Json's encoder can sextuple a byte), because that is a repeater answering 208
  // characters to every one of 32 keys and not a repeater. `writeConfigReadResult` degrades
  // instead - raw lines first, then trailing values, each said in the JSON - so the bound above is
  // what a real read needs and not what a read is allowed to need (ConfigRead.h).
  static constexpr size_t kDataCap = 8192;
  static constexpr size_t kBodyCap = 10240;  // the data, the envelope and the log

  // The scratch every job shares. One instance, the runner's own, and not automatic: the
  // Arduino loop task has 8 KB of stack and a neighbour list alone is more than that. Until task
  // p1.5-15 these were file statics of JobRunner.cpp; as the runner's they are still allocated
  // once by the board (main.cpp's `jobs` is a namespace-scope object), a second runner in a host
  // test gets its own, and the library puts no globals in a translation unit it does not control.
  struct Scratch {
    ClaimedJob jobs[kClaimMax];
    char data[kDataCap];
    char body[kBodyCap];
    JobLog log;
    Neighbour neighbours[kMaxNeighbours];
    LppReading readings[kMaxLppReadings];
    TraceHop hops[kMaxTraceHops + 1];
    char pathHops[kMaxClaimPathHops][8];
    const char* pathPtrs[kMaxClaimPathHops];
    uint8_t pathBytes[kMaxClaimPathHops * 3];
    uint8_t roundTrip[kMeshPathCap];
    ConfigSnapshot snapshot;
    AccessListEntry acl[kAccessListMaxEntries];
    char cliReply[kMaxCliReplyLen + 1];       // the last CLI reply, its own prefix taken off
    // **The console line in flight** (task p1.10-49): the prefix the attempt went out with - two
    // characters and a `|`, or empty for a line that goes bare - and the bytes that went on the
    // air, which are that prefix and the command. `line` is the one buffer besides `secretLine`
    // that can hold a secret: `cliLine` builds it right before the send and wipes it right
    // after, for the same two lines and under the same rule.
    char cliPrefix[kCliPrefixLen + 1];
    char line[kMaxCliCommandLen + kCliPrefixLen + 1];
    char regionListing[kMaxCliReplyLen + 1];  // the `region` reply, kept across `region default`
    char regionValue[kMaxConfigValueLen + 1];
    char command[kPublicKeyHexLen + 32];
    ConfigApplyOutcome applyOutcome;
    // The one buffer of the runner's that carries a secret: `password <new>` or `set
    // guest.password <new>`, built from the claim's credential right before the send and wiped
    // right after it.
    char secretLine[kMaxCliCommandLen + 1];
  };
  static_assert(kDataCap >= kMinConfigReadResultCap, "the config read's result must always be writable");
  static_assert(kBodyCap > kDataCap + sizeof(JobLog), "the body carries the data, the envelope and the log");

  void begin(IMesh& mesh, IUplink& server, IPlatform& platform, void (*pump)());
  void setIdentity(const char* publicKeyHex);
  void setBudget(const AgentBudget& budget) { gate_.setBudget(budget); }

  // The ledger of what a wake spends at the server (agent/UplinkMeter.h), which `AgentLoop::begin`
  // lends this runner: every call this runner makes goes through it, so the claim and every
  // result a job posts are inside the wake's byte cap and its uplink seconds and not beside them
  // (task p1.6-21). A runner nobody lent one to - a host test of this class alone - makes its
  // calls as it always did, bounded by nothing.
  void setMeter(UplinkMeter* meter) { meter_ = meter; }

  // The handlers compiled in beside the thirteen (IJobExtension.h), or nullptr: the Heltec sets
  // none. The heartbeat advertises its types after the thirteen.
  void setExtension(IJobExtension* extension) { extension_ = extension; }
  const IJobExtension* extension() const { return extension_; }

  // One claim and everything it hands over, each result posted as the job ends. Returns how many
  // jobs ran, or -1 when the claim itself did not come back - the caller's backoff then applies.
  // Every job's credential is wiped when the job ends, whatever it came to.
  int claimAndRun();

  // The four steps `claimAndRun` is, for a caller that has to decide between them - a wake with
  // a bounded slice of time and energy (agent/AgentLoop.h's one-wake mode, task p1.5-18), which
  // prices each claimed job before it puts it on the air. The Heltec calls `claimAndRun` and
  // never these.
  //
  //   claim(max)       one claim of at most `max` jobs (never more than kClaimMax), into this
  //                    runner's own buffer. The count, or -1 when the call did not come back
  //   claimed(i)       the job at `i` of that claim, to read its type, class and payload
  //   runClaimed(i)    run it and post its result, exactly as `claimAndRun` would have
  //   reportNotRun(i)  report it `failed` with `why` in its log, at no transaction and with
  //                    nothing sent: a job this wake has no room for is **never left leased and
  //                    silent**, which is the whole point of the pair
  //   endClaim()       the claim is over: every credential wiped, whatever happened
  int claim(int max);
  int claimedCount() const { return claimed_; }
  const ClaimedJob& claimed(int i) const { return scratch_.jobs[i < 0 || i >= kClaimMax ? 0 : i]; }
  void runClaimed(int i);
  void reportNotRun(int i, const char* why);
  void endClaim();

  // Will this job be **declined** rather than run, because the mesh cannot carry its type
  // (IMesh::supports, task p1.6-23)? `runClaimed` asks this itself, before anything goes on the
  // air, and answers `unsupported` at no transaction with nothing sent. A caller that prices a
  // job before running it asks it **first** (task p1.7-25): a type this unit will never carry
  // costs no transaction, so pricing it is pricing a job that was never going to be run, and a
  // bound `trace` in a tight wake came back `failed`, *wake budget*, instead of `unsupported`.
  // The extension's own types are not asked; they are the extension's to decline.
  bool meshDeclines(const ClaimedJob& job) const;

  // Will this job be answered **without the air** (IMesh::answersLocallyFor, task p1.10-43)?
  // The same shape as meshDeclines() above, for the same reason: the mesh's own answer about a
  // claimed job, asked by a caller that **prices** the job before it runs it. A job answered true
  // here is the job `IMesh::answersLocally()` is true for once it has begun - and so the job
  // `spend` neither paces nor counts a transaction for (task p1.10-40) - so a caller that prices
  // one prices its reply waits and no gaps, and charges it no transaction. A runner with no mesh
  // answers false, as it declines nothing.
  bool meshAnswersLocally(const ClaimedJob& job) const;

  // The offline buffer (task 06): results the server did not take are kept and posted later,
  // oldest first, with the timestamps and the client ids they were made with. Called by the
  // agent loop after every heartbeat the server answered - "the offline buffer is posted first,
  // in order" (EXIT-NODES §5, docs/ops/agent.md §4). Safe to call with nothing waiting, and
  // returns how many went through.
  //
  // There is **one** flush and it is bounded by whatever the wake's ledger says (setMeter, task
  // p1.6-21): a wake on a metered plan posts the oldest results while the next one's body still
  // fits in the bytes and the uplink seconds it has left, and leaves the rest for the next wake
  // with the timestamps they were made with. Nothing is dropped and nothing goes out of order;
  // with no ledger, or a ledger with no bounds - `loop()`, so the Heltec - the bound is nothing
  // and this is what it always was.
  int flushResults();

  // How many results are waiting to be posted, which is the heartbeat's `queueDepth`.
  int queueDepth() const { return static_cast<int>(results_.count()); }

  // What the OLED's fourth line needs (src/ui/DisplayLines.h): the job that is open, or
  // nullptr between jobs. Only `claimAndRun`'s own pump ever sees one - the pointer is into the
  // claim's buffer and is good for the length of the job and no longer.
  const ClaimedJob* openJob() const { return open_; }

  // The newest of the last ten, or nullptr before any job has run.
  const Record* newest() const { return recentCount_ > 0 ? &recent_[0] : nullptr; }

  const AirtimeGate& gate() const { return gate_; }
  // The same gate, for a transaction that is not a job's (task p1.8-33: the observer's
  // neighbour sweep sends a regions request per neighbour, and each one waits for this gate
  // and is counted on it, so the board's airtime is one budget and not two).
  AirtimeGate& gate() { return gate_; }
  const ResultBuffer& results() const { return results_; }
  // The buffer itself, for the one thing outside this runner that writes it: the kept image the
  // loop writes at the end of a wake and reads back after a deep sleep (agent/IRetained.h, task
  // p1.6-22). Nothing else may push into it - a result reaches it through `post()` or not at all.
  ResultBuffer& results() { return results_; }
  uint32_t claims() const { return claims_; }
  uint32_t ran() const { return ran_; }
  uint32_t postFailures() const { return postFailures_; }
  uint32_t resultsDropped() const { return results_.dropped(); }
  // Results this runner could not post inside a wake's byte cap or its uplink seconds and kept
  // for the next wake: *kept: budget*, which is a bound of ours and not a failure of the
  // server's, so it backs nothing off (task p1.6-21). The WakeReport carries a wake's own count.
  uint32_t resultsKeptForBudget() const { return resultsKeptForBudget_; }

  // `jobs` on the console: the last ten with their outcome and their transactions.
  void report(TextOut& out) const;

  // `sessions` on the console: per repeater, the class, the level and the age. No secret is in
  // a session - a key, a class, a role byte and a time.
  void reportSessions(TextOut& out) const;

  // One line for `status`.
  void describe(char* out, size_t cap) const;

  // ------------------------------------------------------------------ the flood scope

  // `flood.unscoped` on the console: whether the second flood of a transaction drops the scope
  // (agent/FloodScope.h; meshmaster.dk's D-47). The scope itself is the mesh's (IMesh::floodScope)
  // and is set where the mesh is.
  void setFloodRetry(UnscopedRetry retry) { flood_.setRetry(retry); }
  // What the heartbeat's `flood` object carries: the three counts, per flooded transaction since
  // boot, and whether the unscoped retry is on **and** there is a scope to drop for one send.
  const FloodCounts& floodCounts() const { return flood_.counts(); }
  bool floodUnscopedRetry() const { return flood_.unscopedRetryFor(mesh_ == nullptr ? nullptr : mesh_->floodScope()); }
  // `status`'s `flood` line: the scope this mesh floods in and the counts since boot.
  void describeFlood(char* out, size_t cap) const;

 private:
  // What one job is while it runs.
  struct Context {
    const ClaimedJob* job = nullptr;
    int transactions = 0;
    bool hasRtt = false;
    int rttMs = 0;
    bool sessionWasFresh = false;
    bool hasSession = false;
    uint8_t sessionRole = 0;
    bool grantedBelowClass = false;
    uint8_t grantedRole = 0;
    // **This job ran below the class it asked for** (meshmaster.dk's task p4.30-180,
    // `JobContext.RanBelowClassAs` there): an `acl`-class `status` or `neighbours` whose login
    // was answered at guest, which `guestIsEnough` says is enough to read with because this
    // firmware serves both requests whole to a guest login. The session is kept at the tier
    // granted, the job runs, and `ranBelowClassRole` goes on the result's `loggedInAs` so the
    // server reads the login as the same evidence about the ACL entry that an `unauthorized`
    // with the same tier carries. False on every other job, which is every job that ran at the
    // class it asked for.
    bool ranBelowClass = false;
    uint8_t ranBelowClassRole = 0;
    // FIRMWARE_VER_LEVEL as **this job's own login** was answered with (meshmaster.dk's task
    // p4.30-181, `JobContext.FirmwareVerLevel` there). `hasFirmwareLevel` is false while no login
    // of this job has come back - a session reused from an earlier job, or a legacy reply that
    // carries no level - and **not knowing is never a reason to refuse**: the owner-info request
    // is skipped only on a level this login actually named and that is below
    // kOwnerInfoVerLevel. In practice a status read is the first job of its group and makes the
    // login itself, so a repeater too old for the request costs at most one timeout a day in the
    // arrangement where it does not.
    bool hasFirmwareLevel = false;
    uint8_t firmwareLevel = 0;
    // The rotation probe (task p1.3-13; agent/LoginSequence.h): a blank login between the two
    // tries of the `new` candidate was answered, and the second try was then silent - the
    // password's silence, reported `unauthorized` with LoginSilentResult as the data. The tier
    // the probe was answered at is `probeAnsweredRole`, set by probeReachable() and meaning
    // nothing until it ran. There is no *refused* beside it since task p1.10-47: a response that
    // is not a login reply does not end the probe's wait, so the probe is answered or silent.
    bool silentAfterReachable = false;
    uint8_t probeAnsweredRole = 0;
    // **The clock the repeater showed at the last login of this job that was answered**
    // (meshmaster.dk's task p4.37-210), and this board's own time at that same moment. The four
    // bytes a login reply begins with are `MeshReply::tag`, which this firmware has always read
    // and thrown away; `noteRepeaterClock` is the one place they are kept, and it is called from
    // `awaitLoginReply` - so **whichever login it was** contributes: the job's own, the blank
    // reachability probe between its two tries, the one after *no reply on a reused session*,
    // and the discover's.
    //
    // **The last answered login wins**, which is the freshest reading and, where a job made more
    // than one, the login the job went on to run on: a probe answered and then a second try
    // answered reports the second. `hasRepeaterClock` is false for a job that made no login of
    // its own at all - a reused session, a `trace`, a type nothing was sent for - for a job the
    // mesh answered without the air, and for a board whose own clock is not set, which has no
    // second number to put beside the first.
    bool hasRepeaterClock = false;
    uint32_t repeaterClockS = 0;
    uint32_t agentClockS = 0;
    // **The tags of the requests this job has sent** (task p1.10-47), most recent first to fall
    // off the end. A login reply carries the repeater's own clock and no tag of ours, so a login
    // is awaited with tag 0 and whatever the peer sends next reaches `awaitLoginReply` - and a
    // response carrying one of these is the reply to a request of *ours*, however late, and
    // therefore not a login reply whatever it parses as. That is the half a parse cannot catch:
    // a neighbour page of a repeater with no neighbours is zero bytes after its tag, which
    // `parseLoginReply` reads as *login ok, permissions 0*. Eight is more requests than a job
    // has in flight at once by a wide margin - one, by the budget of EXIT-NODES §8 - and the
    // ones that matter are the most recent.
    static constexpr int kSentTagCap = 8;
    uint32_t sentTags[kSentTagCap] = {};
    uint8_t sentTagCount = 0;
    uint8_t sentTagNext = 0;
    void rememberTag(uint32_t tag);
    bool sentTag(uint32_t tag) const;
    // **The tag a console line carries instead**, which is the prefix the firmware reflects
    // (task p1.10-49; agent/CliReplies.h). A CLI reply is a TXT_MSG with no tag of ours in it,
    // so the sequence below is counted out one per line and per try: a late reply to the line
    // before, or to try 1 of a line that is not safe to run twice, carries another prefix and is
    // not this line's answer. The series is seeded from the platform's random on the job's first
    // console line and dies with the job, so two jobs for the same repeater do not begin at the
    // same two characters - the reply to the last line of one of them can still be in the air
    // when the next begins. `cliPrefixNoted` keeps the job's log to one sentence about it.
    bool hasCliPrefixSeq = false;
    uint8_t cliPrefixSeq = 0;
    bool cliPrefixNoted = false;
    // The restore (task p4.1-60): sent once per job, by the handler that needs its verdict in
    // the result (config.read) or by the runner after the handler.
    bool aclRestoreDone = false;
    // **How far this job got**, for the result's `failedAt` (task p1.7-28; meshmaster.dk's task
    // p4.17-136). `None` is a job that asked the repeater for nothing - a `trace`, a type this
    // mesh declines, a lease that had already run out - and such a result carries no `failedAt`
    // at all. `Login` is a login on the air or a session reused to stand in for one; `Request`
    // is a request or a CLI line after one, set at the top of every attempt, so a request that
    // had to log in again mid-way and failed at *that* reads `Login` again. Where a job stopped
    // is where it failed.
    enum class Stage : uint8_t { None, Login, Request };
    Stage stage = Stage::None;
  };

  // The word a result carries for where it failed, or nullptr: the rule above, with success and
  // a job that logs in to nobody (CredentialClass::None - a trace) carrying none.
  static const char* failedAtOf(const ClaimedJob& job, const Context& ctx, Outcome outcome);

  // What one login came to: agent/LoginSequence.h's reading, which is this runner's. `Refused`
  // is a login answered **below the class it was for** (task p3.5-41) and nothing else since
  // task p1.10-47; `NoReply` is what stock firmware answers a key and password it does not
  // accept, and what a wait that only dropped late replies comes to.
  using LoginResult = LoginTry;

  Outcome runJob(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runStatus(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runTelemetry(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runNeighbours(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runTrace(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runDiscover(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runAclSet(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runConfigRead(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  // The writes (task 05).
  Outcome runConfigApply(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runCli(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runAdvertRequest(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runClockSync(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runPasswordSet(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  Outcome runReboot(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  // A type that is none of the thirteen, offered to the extension (IJobExtension.h).
  Outcome runExtension(const ClaimedJob& job, Context& ctx, size_t& dataLen);
  class ExtensionJob;
  friend class ExtensionJob;
  // The admin session every write needs: a login answered below admin is `unauthorized` at the
  // login, and nothing is sent (a CLI line is answered to admin only on this firmware).
  Outcome adminSession(const ClaimedJob& job, Context& ctx);
  // The apply engine's lines on the air: agent/ConfigApply.h's exchange over this runner.
  class ApplyOnTheAir;
  friend class ApplyOnTheAir;

  // The pieces of a config read.
  Outcome readRegions(const ClaimedJob& job, Context& ctx, ConfigSnapshot& snapshot);
  Outcome readAccessList(const ClaimedJob& job, Context& ctx, ConfigSnapshot& snapshot);
  // The discover's result, from the path the mesh holds now.
  Outcome learned(const ClaimedJob& job, Context& ctx, LoginResult from, const char* source, size_t& dataLen);

  // The login material of the job's class: the password the claim carried, "" for the ACL login
  // (class acl and none) and for a guest-class job the claim carried no password for - the guest
  // door's blank login, which a repeater with no guest password answers at guest (task p1.4-14) -
  // and nullptr only at class admin with no admin password, because there is no admin login
  // without one.
  const char* loginMaterial(const ClaimedJob& job);
  // One login: counted, logged, the session bookkeeping a reply implies, and no policy beyond
  // that. ensureSession() is this twice; a discover is this once, for the path.
  LoginResult loginOnce(const ClaimedJob& job, Context& ctx, const char* material, const char* what);
  // **The wait a login reply ends, and nothing else does** (task p1.10-47). The login's whole
  // wait, in which a response that is not a login reply - another kind, a tag this job sent, or
  // bytes `parseLoginReply` does not know - is dropped with a line in the job's log and the wait
  // goes on to its end. True with `out` filled from a login reply; false is *no reply*, which is
  // what stock firmware answers a login it will not take anyway. Both logins of this file use
  // it, so neither can call a late reply a refusal.
  bool awaitLoginReply(const ClaimedJob& job, Context& ctx, const char* what, LoginReply& out);
  // **The four bytes a login reply begins with, kept** (meshmaster.dk's task p4.37-210): the
  // repeater's own clock as the reply gave it, and this board's own time read at that moment,
  // onto the context for the result to carry. Called from `awaitLoginReply` and from nowhere
  // else, so every answered login of the job contributes and the last of them wins. It refuses
  // two cases, each for want of a number rather than by policy: a job the mesh **answers
  // without the air**, whose login was answered by the door at this end and whose four bytes
  // are no repeater's clock; and a board whose own clock is **not set**, which has nothing to
  // put beside the repeater's. No arithmetic: the two numbers travel and the server subtracts.
  void noteRepeaterClock(const ClaimedJob& job, Context& ctx, const char* what, uint32_t fromReply);
  // One blank login between the two tries of the `new` candidate of an unverified rotation (task
  // p1.3-13; meshmaster.dk's task p4.7-95): one transaction through the gate as every send is
  // (refused by the gate: no probe, false), the ACL login the status probes already make, and
  // **no session stored** whatever came back - `sessions_` is untouched, and the next job logs
  // in afresh. True when the repeater answered with a login reply, at whatever level, which
  // lands on ctx.probeAnsweredRole; false when it was silent - which since task p1.10-47 is also
  // a wait in which nothing but a late reply to something else arrived.
  bool probeReachable(const ClaimedJob& job, Context& ctx);
  // A login at the job's class if none is live: agent/LoginSequence.h's two tries, with the
  // probe between them on the one path that needs it. Returns Ok when there is a session to use.
  Outcome ensureSession(const ClaimedJob& job, Context& ctx);
  // The sequence's logins on the air: agent/LoginSequence.h's exchange over this runner.
  class LoginOnTheAir;
  friend class LoginOnTheAir;
  // One request, with the bounded retry of EXIT-NODES §5: no reply once is retried once, and on
  // a reused session the retry logs in again first. Silence twice is `unreachable`.
  Outcome request(const ClaimedJob& job, Context& ctx, const char* what, uint8_t reqType, const uint8_t* data,
                  uint8_t len);
  // One CLI line, the same way; the reply's text lands in the runner's own buffer. With `retry`
  // false it is sent once whatever comes back, which is what a write needs. The mesh draws the
  // sender timestamp of every attempt (IMesh::sendCli) and hands it back; `timestampUsed`, when
  // given, receives the one of the last attempt, for a caller that records it. `sensitive` stars
  // the frame's body in the frame log (a `password` line); `escape` is the one line that goes out
  // past the lease, the region loader's terminator.
  Outcome cliLine(const ClaimedJob& job, Context& ctx, const char* what, const char* command, bool retry,
                  uint32_t* timestampUsed = nullptr, bool sensitive = false, bool escape = false);
  // The common shape of the two: the send, the wait, the retry and the re-login.
  template <typename Send>
  Outcome exchange(const ClaimedJob& job, Context& ctx, const char* what, bool retry, bool text, Send send,
                   bool escape = false);
  // **The wait only this line's own console reply ends** (task p1.10-49). A CLI reply carries no
  // tag, so while the line in flight carries a prefix (`Scratch::cliPrefix`) a text that does
  // not start with it is a **late** reply - to the line before, or to try 1 of this one - and is
  // dropped with a line in the job's log while the wait goes on to its end. The reply that is
  // this line's lands in `Scratch::cliReply` with its prefix taken off, so nothing downstream
  // sees it. With no prefix on the line this is the one `awaitText` it always was.
  bool awaitCliReply(const ClaimedJob& job, const char* what, int attempt);
  // The prefix the next console line of this job goes out with, into `Scratch::cliPrefix`:
  // counted out one per line and per try from the job's own seeded series (Context above). The
  // empty string for a line that must go bare - `cliLineTakesPrefix` says which, and a repeater
  // whose firmware level this job does not know takes none at all.
  void nextCliPrefix(const ClaimedJob& job, Context& ctx, const char* command, bool escape);
  // The session's last command on a pair with a grant (task p4.1-60). Returns 1 when the
  // repeater confirmed the restore, 0 when it was sent and not confirmed (or could not be), -1
  // when there was nothing to restore.
  int restoreAcl(const ClaimedJob& job, Context& ctx, Outcome outcome);
  // **A line that cannot be asked twice** (task p1.10-48; meshmaster.dk's tasks p4.32-188 and
  // p4.32-189 are the server's half). `setperm <our own key> <level>` runs on a session whose
  // key is, at that moment, admin - and on stock firmware the session *is* the entry, so once
  // the line has run the key is at `<level>` and `onPeerDataRecv` drops every further console
  // line from it without a word. So the line is sent **once** and its silence read back with a
  // blank login, whose reply carries the entry's own permission byte; a check that finds the key
  // still at admin means the line did not arrive, and the line is sent once more. Two rounds at
  // most. The two callers are the two places this board sends that line: `runAclSet` for our own
  // key, and `restoreAcl`.
  struct OwnEntryWrite {
    bool wentOut = false;        // a line left this board: the entry may be at the level now
    bool confirmed = false;      // the entry is at the level the line asked for
    bool byLogin = false;        // confirmed by the blank login and not by an "OK"
    Outcome sent = Outcome::Ok;  // what the last line's own exchange came to
  };
  OwnEntryWrite writeOwnEntry(const ClaimedJob& job, Context& ctx, const char* command, int level, const char* what);
  // The blank login that check is: one transaction through the gate, this board's own key and no
  // secret at all, and a wait only a login reply ends (`awaitLoginReply`). True with `role` set
  // to the level the repeater holds this exit node's key at; false is a silence, which proves
  // nothing. **No session is stored**, as the rotation probe stores none - what becomes of the
  // session depends on what the line was for, and that is the caller's to say (a grant does not
  // keep it, a restore does).
  bool readOwnEntryLevel(const ClaimedJob& job, Context& ctx, const char* what, uint8_t& role);
  // The budget's turn to speak, and the lease's. False when the lease ran out first - unless
  // `escape`: the region loader's terminator waits for the budget and ignores the lease, because
  // a repeater left in load mode reads every later CLI command as a region.
  bool spend(const ClaimedJob& job, Context& ctx, bool escape = false);
  void idle(uint32_t ms);
  uint32_t requestWaitMs(const ClaimedJob& job) const;
  uint32_t loginWaitMs(const ClaimedJob& job) const;
  bool leaseRanOut(const ClaimedJob& job) const;
  void randomBytes(uint8_t* out, size_t len);
  void remember(const ClaimedJob& job, Outcome outcome, int transactions, bool posted);
  // One result on its way to the server: the queue ahead of it first, then this one if the
  // budget has room for it, and otherwise **kept behind the queue** - in the order it was made,
  // which is the buffer's rule (ResultBuffer.h). True when the server took it.
  bool post(long long jobId, const char* body, size_t len);
  // Whether a body of this length may go out at all, which is the wake's ledger's answer and
  // "yes" for a runner that was lent none.
  bool mayPost(size_t len) const { return meter_ == nullptr || meter_->mayPost(len); }
  // Every call this runner makes to the server goes through here, and that is the whole of the
  // accounting: the ledger times the call and weighs the body (agent/UplinkMeter.h).
  template <typename Call>
  bool spend(size_t bodyBytes, Call call) {
    return meter_ == nullptr ? call() : meter_->spend(bodyBytes, call);
  }
  // Every remembered job with this id is marked posted, once its result has gone through.
  void markPosted(long long jobId);
  // The platform's millisecond clock, under the name every line of JobRunner.cpp already used
  // when it was Arduino's.
  uint32_t millis() const { return platform_->millis(); }

  // ---- the flood scope, around every send that may flood (agent/FloodScope.h) ----
  //
  // One flooded transaction is one retry loop, and the three calls below are the whole of it:
  // `FloodTransaction` is the loop's own lifetime (nestable - a re-login inside a request's retry
  // floods inside the transaction already running), `scopeTheNextSend` tells the mesh which shape
  // to use and `noteSend` records what the send came to. A reply is `flood_.answered()`.
  struct FloodTransaction {
    explicit FloodTransaction(FloodPolicy& policy) : policy_(policy) { policy_.begin(); }
    ~FloodTransaction() { policy_.end(); }
    FloodTransaction(const FloodTransaction&) = delete;
    FloodTransaction& operator=(const FloodTransaction&) = delete;
    FloodPolicy& policy_;
  };
  FloodMode scopeTheNextSend();
  void noteSend(SendResult sent, FloodMode mode);
  // The words a job's log line prints for a send: `direct`, `flood, scope dk`, `flood, unscoped`.
  void describeSend(SendResult sent, FloodMode mode, char* out, size_t cap) const;

  IMesh* mesh_ = nullptr;
  IUplink* server_ = nullptr;
  IPlatform* platform_ = nullptr;
  const IClock* clock_ = nullptr;  // platform_->clock(), for the lease and the timestamps
  void (*pump_)() = nullptr;
  const char* publicKey_ = nullptr;
  IJobExtension* extension_ = nullptr;

  AirtimeGate gate_;
  SessionStore sessions_;
  // The flood scope's rule and its counts (agent/FloodScope.h, task p1.8-32).
  FloodPolicy flood_;
  // The wake's ledger, or null: AgentLoop::begin lends it (setMeter).
  UplinkMeter* meter_ = nullptr;

  Record recent_[kRemembered];
  int recentCount_ = 0;

  // The job `claimAndRun` is running, for the display (task p1.2-09). Null between jobs.
  const ClaimedJob* open_ = nullptr;

  uint32_t claims_ = 0;
  uint32_t ran_ = 0;
  uint32_t postFailures_ = 0;
  uint32_t resultsKeptForBudget_ = 0;
  // How many jobs the last claim handed over, which is how far claimed(i) and runClaimed(i) go.
  int claimed_ = 0;

  // The results whose POST has not got through yet, oldest first. Each keeps the client id it
  // was written with, so the server de-duplicates it however many times it arrives (API §3), and
  // the buffer is bounded and drops from the oldest end (ResultBuffer.h).
  ResultBuffer results_;

  Scratch scratch_;
};

}  // namespace mm
