// meshmaster.dk's `tests/fixtures/contracts/AgentHeartbeatNoKey.json`, as one string.
//
// **Why it is here and not in the copied fixture set.** The contract fixtures this repository
// holds come across with the agent library (`host_test/agent-core/tests/fixtures/contracts/`,
// task p4-01), and that set is the exit-node firmware's copy of meshmaster.dk's at
// `79b1e9cbaf97ea2dc7b58f69ac06d5a116efb9b9` - which is *before* this fixture existed.
// meshmaster.dk's pile 4.16 added it in commit `d8f12ca` ("p4.16-132: the unit before its
// repeater"), on `master` at `def65a4`, and bumping the library's pin to bring the newer set
// across is task p4-01's and not this task's. So the bytes are here, with their source named, and
// `tasks/phase-4/FOLLOWUPS.md`'s `[04]` item is what asked for them.
//
// It is the heartbeat of a **unit registered before its repeater**: `native`, no
// `companionPublicKey`, no `companionFirmware`, no `radio` block, an empty `jobTypes` and
// `companionConnected` false - a unit that is alive and whose radio is not identified. The one
// field this unit adds to it is `board`, which it names whether or not its radio has a key.
//
// When task p4-01 next bumps the library and the newer fixture set arrives, this file goes and
// the test reads the fixture like every other.
#pragma once

namespace fixtures {

// source: twinoak/meshmaster.dk tests/fixtures/contracts/AgentHeartbeatNoKey.json at def65a4
inline constexpr const char* kAgentHeartbeatNoKey =
    "{\"agentVersion\":\"0.3.1\",\"transport\":\"native\",\"at\":\"2026-09-23T14:00:00Z\","
    "\"jobTypes\":[],\"budget\":{\"maxTransactionsPerMinute\":2,\"minGapSeconds\":20},"
    "\"companionConnected\":false,\"queueDepth\":0,\"errors\":{\"uplink\":0},"
    "\"nextCheckInS\":3600,\"maxTransactionsThisWake\":12}";

}  // namespace fixtures
