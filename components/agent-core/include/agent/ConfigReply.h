// The server's answers, as structs that mirror MeshMaster.Contracts field for field.
//
//   HeartbeatReply(DateTime ServerTime, AgentBudget Budget, int PollIntervalS, bool JobsWaiting,
//                  UpdateAsk? Update)
//   UpdateAsk(long Id, long ReleaseId, string Product, string Version, string Base,
//             string ExpectedSha256, long Size)
//   AgentConfig(long AgentId, string Label, string Transport, AgentBudget Budget, int PollIntervalS,
//               DateTime ServerTime)
//   AgentBudget(int MaxTransactionsPerMinute, int MinGapSeconds)
//   ObservationsReply(int Accepted, bool Duplicate)
//
// (src/MeshMaster.Contracts/Envelopes.cs in twinoak/meshmaster.dk.) The JSON is camelCase, nulls
// are left out and unknown members are skipped on both ends, so a field this firmware does not
// know is not an error and a field it needs but does not get is. Parsed with ArduinoJson;
// host-portable, and test/test_config_reply feeds it the .NET fixture
// (tests/fixtures/contracts/HeartbeatReply.json) so the two ends are held to the same bytes.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

struct AgentBudget {
  int maxTransactionsPerMinute = 0;
  int minGapSeconds = 0;
};

// AgentBudget.Default of Envelopes.cs: the conservative two transactions a minute, twenty
// seconds apart, which is what is in force until the server has answered.
constexpr int kDefaultMaxTransactionsPerMinute = 2;
constexpr int kDefaultMinGapSeconds = 20;

// The ask in a heartbeat's reply: a person on meshmaster.dk pressed *Update* for this exit node
// (UpdateAsk; task p1.15-61 and meshmaster.dk's task p4.42-244). It is not a job - a job belongs
// to a repeater and this is about the exit node itself - so it arrives in the reply to every
// heartbeat from the moment it was asked for until the result is recorded, or it is withdrawn or
// runs out, and the same `id` twice is the same ask.
//
// `base` is the release's route prefix on the configured server, on the agent's own token: the
// manifest is `{base}/manifest.json`, its signature `{base}/manifest.json.sig` and a file
// `{base}/files/{nameOrSha256}`. `expectedSha256` and `size` are the file the server expects the
// board to choose - information, as in an offer: the manifest names the files and the manifest is
// what is checked.
//
// **This library does nothing with any of it** but hand it to the consumer's provider
// (AgentLoop.h, `IUpdateProvider`): what a slot, a release or a signature is lives on the other
// side of that seam.
struct UpdateAsk {
  long long id = 0;
  long long releaseId = 0;
  char product[64] = {0};
  char version[33] = {0};
  char base[65] = {0};
  char expectedSha256[2 * 32 + 1] = {0};
  uint32_t size = 0;
};

struct HeartbeatReply {
  char serverTime[40] = {0};   // as sent
  uint32_t serverTimeUnix = 0; // as parsed (Iso8601.h)
  AgentBudget budget;
  int pollIntervalS = 0;
  bool jobsWaiting = false;

  // The ask, last, and optional: absent for every reply a server sends an exit node nobody has
  // pressed *Update* for, which is nearly all of them.
  //
  // **An `update` object that is not of the shape above is dropped and the rest of the reply is
  // still taken** - `hasUpdate` false and `updateRefused` true, which the loop logs. A reply the
  // parser refused whole would be a heartbeat that failed, a backoff, and in the end an exit node
  // the server reads as offline because of a member it is free not to send at all; the ask comes
  // again in the next reply, so the cost of dropping one is thirty seconds.
  bool hasUpdate = false;
  bool updateRefused = false;
  UpdateAsk update;
};

struct AgentConfig {
  long long agentId = 0;
  char label[64] = {0};        // the server bounds it; a longer one is cut here, not refused
  char transport[16] = {0};    // serial | tcp | native | onboard - a hint, the board is onboard
  AgentBudget budget;
  int pollIntervalS = 0;
  char serverTime[40] = {0};
  uint32_t serverTimeUnix = 0;
};

// What POST /agent/v1/observations answers: how many of the batch were taken, and whether the
// server had this clientId already (then `accepted` is 0 and nothing was stored twice).
struct ObservationsReply {
  int accepted = 0;
  bool duplicate = false;
};

// `json` need not be NUL-terminated; `len` bounds it. On refusal, `err` says which field.
bool parseHeartbeatReply(const char* json, size_t len, HeartbeatReply& out, char* err, size_t errCap);
bool parseAgentConfig(const char* json, size_t len, AgentConfig& out, char* err, size_t errCap);
bool parseObservationsReply(const char* json, size_t len, ObservationsReply& out, char* err, size_t errCap);

}  // namespace mm
