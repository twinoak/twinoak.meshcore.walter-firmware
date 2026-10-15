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
//                   JsonElement? Data, string? Log)
//   ExtendRequest(int Seconds) / ExtendReply(long JobId, DateTime LeaseUntil)
//   ProgressReport(JsonElement? Data, string? Note) / ProgressReply(long JobId, DateTime RecordedAt)
//
// (src/MeshMaster.Contracts/Envelopes.cs in twinoak/meshmaster.dk; the bytes are held against
// tests/fixtures/contracts/ClaimedJob.json, JobResultReport.json, ExtendRequest.json,
// ExtendReply.json, ProgressReport.json and ProgressReply.json.)
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
  // the owner-info request that carries the firmware version. **Absent is "read it"** - what
  // every claim said before that task - and `false` is the claim saying the server's record is
  // fresh enough to spend nothing on, which at the default schedules is forty-seven of a
  // repeater's forty-eight daily reads. readsFirmwareVersion() below is the rule; this is only
  // what the payload said.
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

// Whether a status job also fetches the firmware version, which is one transaction more
// (EXIT-NODES §7). One rule, in one place, as StatusPayload.ReadsVersion is on the server:
// class `acl` - only the association's verify step needs the version at all - unless the
// claim's payload says the server does not want it this time. A job of another type is false.
bool readsFirmwareVersion(const ClaimedJob& job);

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
};

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
