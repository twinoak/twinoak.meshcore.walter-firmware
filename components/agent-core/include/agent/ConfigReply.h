// The server's answers, as structs that mirror MeshMaster.Contracts field for field.
//
//   HeartbeatReply(DateTime ServerTime, AgentBudget Budget, int PollIntervalS, bool JobsWaiting)
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

struct HeartbeatReply {
  char serverTime[40] = {0};   // as sent
  uint32_t serverTimeUnix = 0; // as parsed (Iso8601.h)
  AgentBudget budget;
  int pollIntervalS = 0;
  bool jobsWaiting = false;
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
