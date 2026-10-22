// The `data` of a job result, one writer per job type.
//
// These are MeshMaster.Contracts' result records (src/MeshMaster.Contracts/Results.cs in
// twinoak/meshmaster.dk) written straight into a buffer, in the record's own field order with
// the null fields left out:
//
//   StatusResult(int? BatteryMv, int? NoiseFloorDbm, int? LastRssi, double? LastSnr, long? UptimeS,
//                int? TxQueueLen, PacketCounters? Packets, long? AirtimeS, DuplicateCounters? Dups,
//                IReadOnlyDictionary<string,long>? Errors, string? Firmware)
//   PacketCounters(long? Recv, long? Sent, long? RecvFlood, long? RecvDirect, long? SentFlood, long? SentDirect)
//   DuplicateCounters(long? Flood, long? Direct)
//   TelemetryResult(IReadOnlyList<LppChannel> Channels); LppChannel(int Channel, string Type, double Value, string? Unit)
//   NeighboursResult(IReadOnlyList<Neighbour> Neighbours); Neighbour(string Prefix, long? LastHeardS, double? Snr)
//   TraceResult(IReadOnlyList<TraceHop> Hops); TraceHop(string Prefix, double? Snr)
//   LoginBelowClassResult(string LoggedInAs, string Needed)
//
// The server parses nothing about radio frames: the agent returns fields (EXIT-NODES §6). Every
// one of these is held against tests/fixtures/contracts for its shape and against the values
// MeshMaster.Agent.Tests asserts for the transcribed bytes of tests/fixtures/companion.
//
// Host-portable: the writers take the structs agent/ReplyCodec.h fills.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/Envelopes.h>
#include <agent/ReplyCodec.h>

namespace mm {

// Each returns the length written, or 0 when it did not fit.

// `errors` is the open key space the record has on both ends: `eventsFull` always, and the two
// counters a firmware old enough not to send them leaves out rather than reporting as zero.
// statusErrors() is what a handler fills it with from a reading; the writer takes the array so a
// counter this firmware has never heard of still has a way through.
constexpr int kMaxStatusErrors = 3;
int statusErrors(const StatusReading& s, ErrorCounter* out, int max);

// `firmware` is nullptr for every class but `acl`, where the owner-info request fetched it.
size_t writeStatusResult(const StatusReading& s, const ErrorCounter* errors, int errorCount, const char* firmware,
                         char* out, size_t cap);

size_t writeTelemetryResult(const LppReading* readings, int count, char* out, size_t cap);

size_t writeNeighboursResult(const Neighbour* neighbours, int count, char* out, size_t cap);

// The hops of the outward trace, the repeater last; the SNR each hop measured goes with it.
size_t writeTraceResult(const TraceHop* hops, int count, char* out, size_t cap);

// The data of an `unauthorized` whose login was *answered* below the class the job asked for
// (task p3.5-41, task p4.1-60): the server can then tell "the repeater answered our key at
// guest" from "the entry is gone", which carries no data at all.
size_t writeLoginBelowClassResult(const char* loggedInAs, const char* needed, char* out, size_t cap);

//   LoginSilentResult(string Reason, string? ProbeAnsweredAs)
//
// The data of an `unauthorized` whose login was *silent* right after a blank login the repeater
// answered (task p1.3-13; meshmaster.dk's task p4.7-95 and CONFIGURATION §7 step 4): the one
// case where a silence is reported as the refusal it is, during an unverified credential
// rotation. `reason` is always `silentAfterReachable` - the one reason the record has
// (LoginSilentReasons) - and the server reads it and treats it exactly as a refusal, so the
// spelling is the rail. `probeAnsweredAs` is the tier the blank login was answered at (`guest`,
// `read-only`, `read-write`, `admin`) or `refused` for the legacy frame; nullptr leaves it out,
// as the record's null does.
extern const char kLoginSilentAfterReachable[];
size_t writeLoginSilentResult(const char* probeAnsweredAs, char* out, size_t cap);

//   DiscoverResult(PathInfo? Path, string? LoggedInAs); PathInfo(IReadOnlyList<string> Hops, string? Source,
//                  DateTime? LearnedAt)
//   AclSetResult(string PublicKey, int Level, bool Confirmed, string? Reply)
//
// (task 04). A discover with no path is `{}`: both fields null and left out. `source` is one of
// PathSources - "login" for a path a login's reply came back along, "discovery" for the
// path-discovery request - and `loggedInAs` is the tier the repeater granted, or nullptr when no
// login was answered.
extern const char kPathSourceLogin[];
extern const char kPathSourceDiscovery[];

size_t writeDiscoverResult(bool hasPath, const char* const* hops, int hopCount, const char* source, uint32_t learnedAt,
                           const char* loggedInAs, char* out, size_t cap);

size_t writeAclSetResult(const char* publicKey, int level, bool confirmed, const char* reply, char* out, size_t cap);

//   CliResult(string Command, string? Reply) { Answered, IsHardError }
//   AdvertRequestResult(bool Confirmed, string? Reply)
//   ClockSyncResult(DateTime SetTo, long? DriftS, bool Confirmed, string? Reply)
//   PasswordSetResult(string Field, bool Confirmed, bool Sent, string? Reply)
//   RebootResult(bool Sent, string? Reply)
//
// (task 05; tasks p4-54 and p4-55). `reply` nullptr is silence and is left out. A cli's
// `answered` and `isHardError` are the record's computed properties, written as System.Text.Json
// writes them. `driftS` is what a firmware that says how far the clock was off would report; this
// one does not, so a handler passes hasDriftS false and the field is left out. A password.set's
// reply never carries the value: `password now: <set>` is the admin echo with the value taken
// out (PasswordSetResult.AdminReplyRedacted).
extern const char kPasswordSetAdminReplyRedacted[];

size_t writeCliResult(const char* command, const char* reply, char* out, size_t cap);
size_t writeAdvertRequestResult(bool confirmed, const char* reply, char* out, size_t cap);
size_t writeClockSyncResult(uint32_t setTo, bool hasDriftS, long long driftS, bool confirmed, const char* reply, char* out,
                            size_t cap);
size_t writePasswordSetResult(const char* field, bool confirmed, bool sent, const char* reply, char* out, size_t cap);
size_t writeRebootResult(bool sent, const char* reply, char* out, size_t cap);

}  // namespace mm
