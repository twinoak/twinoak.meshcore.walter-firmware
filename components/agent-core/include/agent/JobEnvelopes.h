// The two envelopes of the job protocol this firmware speaks: the claim's answer in, the result
// out (API §3, EXIT-NODES §6).
//
//   ClaimedJob(long Id, string Type, RepeaterRef Repeater, CredentialClass CredentialClass,
//              DateTime LeaseUntil, JobTimeouts Timeouts, JsonElement Payload,
//              JobCredential? Credential, PathInfo? Path, string? IdempotencyKey,
//              IReadOnlyList<long>? GroupWith)
//   ClaimResponse(DateTime ServerTime, IReadOnlyList<ClaimedJob> Jobs)
//   JobResultReport(string ClientId, Outcome Outcome, DateTime StartedAt, DateTime FinishedAt,
//                   IReadOnlyList<string>? PathUsed, int? Hops, int? RttMs, int? Transactions,
//                   JsonElement? Data, string? Log, string? FailedAt)
//   ExtendRequest(int Seconds) / ExtendReply(long JobId, DateTime LeaseUntil)
//   ProgressReport(JsonElement? Data, string? Note) / ProgressReply(long JobId, DateTime RecordedAt)
//
// (src/MeshMaster.Contracts/Envelopes.cs in twinoak/meshmaster.dk; the bytes are held against
// tests/fixtures/contracts/ClaimedJob.json, JobResultReport.json, JobResultReportFailed.json,
// ExtendRequest.json, ExtendReply.json, ProgressReport.json and ProgressReply.json.)
//
// Host-portable, like the envelopes beside it: the claim is read with ArduinoJson and the result
// is written by JsonWriter.h, and both are what test/test_claim exercises without a board.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/ConfigApply.h>
#include <agent/ConfigRead.h>
#include <agent/Credentials.h>
#include <agent/ReplyCodec.h>

namespace mm {

// How a job ended, as the server spells it (spec/API.md §3). The names on the wire are the
// camelCase enum names ContractsJson uses.
enum class Outcome : uint8_t { Ok, Failed, Unreachable, Unauthorized, Unsupported, Timeout };
const char* outcomeName(Outcome o);

// The thirteen types this firmware has a handler for - every type of API §3 - in the order
// JobTypes.All lists them: the four reads of task 03, the three of task 04 that carry a password
// (discover, acl.set, config.read), and since task 05 the six that change a repeater
// (config.apply, cli, advert.request, clock.sync, password.set, reboot). The heartbeat sends
// exactly this list, and routing sends nothing else (task p3-53).
constexpr int kJobTypeCount = 13;
extern const char* const kJobTypes[kJobTypeCount];
bool isKnownJobType(const char* type);

// A claim takes two at a time: one transaction is on the air at any moment anyway, and two is
// the status+telemetry pair the server schedules in one slot and claims together.
constexpr int kClaimMax = 2;

constexpr size_t kMaxJobTypeLen = 16;
constexpr size_t kMaxRepeaterLabelLen = 48;
constexpr size_t kMaxIdempotencyKeyLen = 80;
constexpr int kMaxClaimPathHops = 8;
constexpr size_t kMaxHopHexLen = 6;  // three bytes of hash
constexpr int kMaxGroupWith = 4;
constexpr size_t kPublicKeyHexLen = 64;
constexpr size_t kMaxCliCommandLen = 120;  // CliPayload.MaxLength: one line, and no command anybody types is near it
constexpr size_t kMaxCredentialCandidateLen = 8;

// PasswordSetPayload.Field: which password a password.set changes.
enum class PasswordField : uint8_t { None, Admin, Guest };
const char* passwordFieldName(PasswordField f);

struct ClaimedJob {
  long long id = 0;
  char type[kMaxJobTypeLen + 1] = {0};

  long long repeaterId = 0;
  char repeaterLabel[kMaxRepeaterLabelLen + 1] = {0};
  char repeaterKey[kPublicKeyHexLen + 1] = {0};  // 64 lowercase hex, or empty

  CredentialClass credentialClass = CredentialClass::None;
  uint32_t leaseUntil = 0;  // UNIX seconds

  int perHopMs = 4000;
  int loginMs = 8000;

  // `acl` is the level the repeater's ACL grants this exit node's own key: the material of an
  // acl-class job, and beside the admin password of an admin-class one the level the session's
  // last command puts the entry back to (task p4.1-60).
  bool hasAclLevel = false;
  int aclLevel = 0;

  // The passwords, when the claim carried one for the job's class (Credentials.h), and the new
  // one a password.set sets: a fixed buffer, wiped when the job ends.
  JobCredential credential;

  // `credential.candidate` (task p4-55): `new` or `old` when the material is one of the two
  // candidates of an unverified credential. The job then logs in afresh whatever the cache
  // holds, because the server reads its outcome as the verdict on exactly this material. A
  // word, not a value.
  char credentialCandidate[kMaxCredentialCandidateLen + 1] = {0};

  // The payload, for the types whose payload says something: a config.read's `keys` (none
  // means the default list), an acl.set's `publicKey` and `level`, a config.apply's plan, a
  // cli's `command`, an advert.request's `flood`, a status's `version` and a password.set's
  // `field`.
  bool hasConfigKeys = false;
  int configKeyCount = 0;
  char configKeys[kMaxConfigKeys][kMaxConfigKeyLen + 1] = {};
  bool hasAclSet = false;
  char aclSetKey[kPublicKeyHexLen + 1] = {0};
  int aclSetLevel = 0;
  ConfigApplyPlan apply;
  bool hasCliCommand = false;
  bool cliCommandTooLong = false;
  char cliCommand[kMaxCliCommandLen + 1] = {0};
  bool advertFlood = true;  // AdvertRequestPayload.Flood defaults to true: the flood is what refreshes paths

  // StatusPayload.Version (meshmaster.dk's task p4.3-68): whether this status read also makes
  // the owner-info request. **Absent is "read it"** - what every claim said before that task -
  // and `false` is the claim saying the server's record is fresh enough to spend nothing on,
  // which at the default schedules is twenty-three of a repeater's twenty-four daily reads.
  // readsOwnerInfo() below is the rule; this is only what the payload said.
  //
  // **The field is still called `version` on the wire** (meshmaster.dk's task p4.30-181): what it
  // suppresses grew from one line of the reply to three - the version, the node name and the
  // owner info - but a server that renamed the field would have gone silent to every exit node
  // built before that task. The name is the history; the meaning is readsOwnerInfo().
  bool statusVersion = true;
  PasswordField passwordField = PasswordField::None;

  // What reachability knows for this (agent, repeater): the hops to set as the contact's
  // out-path before sending. Absent while nothing has been learned, and then the mesh floods.
  bool hasPath = false;
  int pathCount = 0;
  char pathHops[kMaxClaimPathHops][kMaxHopHexLen + 1] = {};

  char idempotencyKey[kMaxIdempotencyKeyLen + 1] = {0};

  int groupCount = 0;
  long long groupWith[kMaxGroupWith] = {};
};

// Whether this firmware can run the job at all, and one sentence saying why not when it cannot:
// a type without a handler, a class the type cannot use (JobTypes.CredentialClasses on the
// server: status, telemetry and neighbours at guest, acl or admin - neighbours since task
// p1.2-10, which is the rule the pinned firmware has (REQ_TYPE_GET_NEIGHBOURS checks no
// permission at all); trace at none; discover at guest or admin - guest since task p1.4-14, the
// guest door's pairing step, which the server sends at that class; acl.set, config.read and the
// six writes at admin), a repeater with no key -
// and, belt beside
// the claim's braces (task p4.1-60), the **guest password on a pair that holds a grant**: a
// guest login from this key would rewrite the ACL entry to guest, so a claim that carries both
// is refused here whatever the server decided. Every refusal is `unsupported` rather than
// `failed`: "this firmware does not have what the job asked for" is what that outcome means,
// and it moves nothing in reachability, which is right for a transaction that never happened
// (EXIT-NODES §3).
bool canRun(const ClaimedJob& job, char* why, size_t whyCap);

// Jobs of one claim that share a login (EXIT-NODES §8): `groupWith` names them, and the repeater
// they are for is the session cache's own answer to the same question.
bool sharesLogin(const ClaimedJob& a, const ClaimedJob& b);

// Whether a status job also makes the owner-info request, which is one transaction more
// (EXIT-NODES §7). One rule, in one place, as StatusPayload.ReadsOwnerInfo is on the server:
// **every class a status read goes at**, unless the claim's payload says the server does not
// want the reply this time. A job of another type is false.
//
// Class `acl` alone fetched it until meshmaster.dk's task p4.30-181, because only the
// association's verify step needed the firmware version. The reply's other two lines - the node
// name and the owner info - are wanted for every repeater a person can see, somebody else's
// included, and `MyMesh::handleRequest` checks no permission at all for this request, so a guest
// login is answered it. The class is therefore no longer part of the question.
//
// It is not the whole of *when*: the firmware level the login reply carried is the other half,
// and a repeater below kOwnerInfoVerLevel is not asked (JobRunner's runStatus).
bool readsOwnerInfo(const ClaimedJob& job);

// **What a guest login is answered whole** (meshmaster.dk's task p4.30-180,
// `JobTypes.ServedWholeAtGuest`), read off `MyMesh::handleRequest` at the pin rather than off a
// table: `REQ_TYPE_GET_STATUS` checks no permission at all - its line is commented *guests can
// also access this now* - and neither does `REQ_TYPE_GET_NEIGHBOURS`. So `status` and
// `neighbours` are the two types a guest login serves **whole**.
//
// `telemetry` is **not** one of them although a guest login is answered it: `perm_mask` is forced
// to `0x00` for a guest sender, so the reply is the base channels and none of the sensors, and a
// read that quietly dropped the sensors would look like the sensors had gone. The access list is
// `sender->isAdmin()`, and every CLI line is admin's (`onPeerDataRecv` drops a text message from
// anybody below admin).
bool servedWholeAtGuest(const char* type);

// **Whether a login answered at `grantedRole` is enough to run this job anyway** -
// `GuestIsEnough.Covers` on the server and in the .NET agent, so that what one runs and the other
// reads cannot drift (meshmaster.dk's task p4.30-180, EXIT-NODES §7 *And guest is enough to read
// with*). Three things at once: class **`acl`**, a type served whole at guest, and a role
// **below read-only**, which on stock firmware is `guest` and nothing else.
//
// Such a login is still the evidence it was - an `acl` login answered at guest says the ACL entry
// our grant wrote is gone or demoted - and the result says so on `JobReport::loggedInAs`. What
// changes is that the reading is not thrown away with the login: AAR_LH, 10 November 2026, lost
// two *Read the neighbours* to a request this firmware would have answered.
//
// Every other class and every other type is judged as before, with `writeLoginBelowClassResult`.
bool guestIsEnough(const char* type, CredentialClass credentialClass, uint8_t grantedRole);

// Whether the lease this job was claimed under has run out (task 06). A claim's jobs are run one
// at a time and a job is minutes of airtime, so a server that went away under a claim - or a
// board that spent an hour on the one before - can reach a job the server has long since given
// back to the queue. Such a job is **not run**: nothing goes on the air for a lease somebody
// else may now hold, and the result is reported as `timeout` at no transaction. The result is
// still posted, late if it must be: the server takes a late result and de-duplicates on the
// client id (EXIT-NODES §5).
//
// `hasTime` is false while the board's clock is unset, and then no lease has run out: a board
// with no clock cannot tell, and refusing every job would be worse than running one late.
// `leaseUntil` of 0 is a claim that named no lease.
bool leaseLapsed(uint32_t leaseUntil, bool hasTime, uint32_t nowEpoch);

// POST /agent/v1/jobs/claim's body: `{"max":N}`.
size_t writeClaimRequest(int max, char* out, size_t cap);

// Reads the claim's answer. At most `max` jobs are kept - the server sends at most what was
// asked for, and a reply that says otherwise is not a reason to overrun a buffer. `count` is how
// many were read; `serverTime` is the claim's own clock. On refusal, `err` says why.
//
// `json` is **mutable and is parsed in place** (ArduinoJson's zero-copy mode): the document
// points into the caller's buffer and keeps no copy of any string, so a password in the claim
// exists in that buffer and in the job's credential and nowhere else - and the caller wipes the
// buffer the moment this returns (agent/AgentClient.cpp). The buffer is unreadable afterwards.
bool parseClaimResponse(char* json, size_t len, ClaimedJob* jobs, int max, int& count, uint32_t& serverTime, char* err,
                        size_t errCap);

// The bound API §3 puts on a result's free text.
constexpr size_t kMaxJobLogChars = 4000;

struct JobReport {
  const char* clientId = "";
  Outcome outcome = Outcome::Failed;
  uint32_t startedAt = 0;
  uint32_t finishedAt = 0;

  bool hasPathUsed = false;
  const char* const* pathUsed = nullptr;
  int pathCount = 0;

  bool hasHops = false;
  int hops = 0;
  bool hasRttMs = false;
  int rttMs = 0;

  int transactions = 0;

  // The job type's result record, already written (JobResults.h), or nullptr.
  const char* data = nullptr;

  const char* log = nullptr;

  // **Where a failed job failed** (meshmaster.dk's task p4.17-136): one of the two words below,
  // or nullptr to leave the field out. `unreachable` is two different things - a login nobody
  // answered, which on stock firmware is also a wrong password, and a request lost twice after
  // a login that *was* answered - and only the first of them is a reason to tell an owner that
  // the stored password is probably wrong. The server reads a result without the field exactly
  // as it did before, so an older exit node is unaffected.
  const char* failedAt = nullptr;

  // **The tier the repeater answered the login at, when the job ran below the class it asked
  // for** (meshmaster.dk's task p4.30-180; API §3): one of `guest`, `read-only`, `read-write`,
  // `admin`, or nullptr to leave the field out. It is written for one case and no other - an
  // `acl`-class `status` or `neighbours` whose login was answered at `guest` and which ran all
  // the same (`guestIsEnough` above) - so the outcome is `ok`, `data` is the reading, and this is
  // what tells the server the login was not the one the job asked for: it moves the association
  // exactly as an `unauthorized` carrying the same tier does. **Absent means the login did what
  // the job asked for**, which is every other result and every result from an exit node built
  // before this.
  const char* loggedInAs = nullptr;

  // **What time the repeater thought it was, and what time this board thought it was at that
  // same moment** (meshmaster.dk's task p4.37-210; API §3, EXIT-NODES §7). A login reply begins
  // with four bytes of the repeater's own clock - `getRTCClock()->getCurrentTimeUnique()` in
  // `handleLoginReq` - which this firmware has always read as `MeshReply::tag` and thrown away.
  // They cost no request and no airtime, and they are the cheapest reading there is of a
  // repeater whose battery-backed clock started again from an old date.
  //
  // **Two numbers and no arithmetic here**: the server stores the difference. They go
  // **together or not at all** - one of them alone is a 400 on `repeaterClockS` - which is what
  // the single `hasClock` is for, and each is a `uint32_t`, which is exactly the range four
  // bytes hold and the range the server accepts. Absent from a job that reused a session and so
  // made no login of its own, from a job the mesh answered without the air, and from a board
  // whose own clock is not set; absent is also every result of every exit node built before
  // this, which the server reads exactly as it read them.
  bool hasClock = false;
  uint32_t repeaterClockS = 0;
  uint32_t agentClockS = 0;
};

// The two words `JobReport::failedAt` may carry (the server's `JobFailedAt`). Static strings, so
// a report can carry one without a buffer of its own.
extern const char kFailedAtLogin[];    // no login was answered: silence to the password, or a refusal
extern const char kFailedAtRequest[];  // a login was answered and the request after it was not

size_t writeJobResult(const JobReport& r, char* out, size_t cap);

// ---------------------------------------------------------------------------------------------
// The two calls the protocol has for a job that runs past its lease (API §3; task p1.5-15). The
// reference agent never makes them and nothing on the Heltec does yet; the uplink seam
// (IUplink.h) carries them so that another firmware can.

// POST /agent/v1/jobs/{id}/extend's body: `{"seconds":N}`, 1-1800 (ExtendRequest).
size_t writeExtendRequest(int seconds, char* out, size_t cap);

struct ExtendReply {
  long long jobId = 0;
  char leaseUntil[40] = {0};   // as sent
  uint32_t leaseUntilUnix = 0; // as parsed (Iso8601.h)
};
bool parseExtendReply(const char* json, size_t len, ExtendReply& out, char* err, size_t errCap);

// POST /agent/v1/jobs/{id}/progress's body: `{"data":<object>,"note":"..."}` (ProgressReport).
// `dataJson` is a JSON value already written, or nullptr to leave the field out; `note` likewise.
size_t writeProgressReport(const char* dataJson, const char* note, char* out, size_t cap);

struct ProgressReply {
  long long jobId = 0;
  char recordedAt[40] = {0};
  uint32_t recordedAtUnix = 0;
};
bool parseProgressReply(const char* json, size_t len, ProgressReply& out, char* err, size_t errCap);

}  // namespace mm
