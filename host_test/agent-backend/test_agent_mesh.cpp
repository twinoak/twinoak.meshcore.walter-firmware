// A job for a neighbour and jobs for the unit's own repeater, through the whole wake (task
// p4-05): a `status` on a neighbour goes over the mesh - LinkMesh, the UART framing, the RAK,
// the library's simulated stock repeater - and its result is posted in the fixture's shape;
// a `telemetry` and a `config.read` on the own repeater go through the local door - no packet
// on the air - and their results are posted too. GoogleTest, because the repeater is.
#include <gtest/gtest.h>

#include <string>

#include "agent_mesh_bench.h"

namespace {

using bench::MeshBench;

std::string data(MeshBench& bench, long long id) { return bench.agent.lastData(id); }
std::string member(const std::string& json, const std::string& key) { return bench::member(json, key); }

TEST(AgentMesh, AStatusOnANeighbourGoesOverTheMeshAndItsResultIsPosted) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(bench::neighbourStatus(50).json());
  interfaces::BackendAnswer out;
  const std::uint32_t tx_before = bench.agent.half().mesh.counters().tx;
  const interfaces::Exchanged how = bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);
  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  // It went over the air: client_tx moved, and the RAK saw the sends.
  EXPECT_GT(bench.agent.half().mesh.counters().tx, tx_before);
  EXPECT_GT(bench.rak().client_tx(), 0u);
  // The result reached the server in the StatusResult shape (the repeater's fixture's bytes).
  const std::string result = bench.agent.lastResult(50);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(member(result, "outcome"), "ok");
  const std::string d = data(bench, 50);
  EXPECT_EQ(member(d, "batteryMv"), "4012");
  EXPECT_NE(d.find("\"packets\""), std::string::npos);
  // client_tx equals the runner's transactions for the job, exactly - the one-send-per-command
  // rail made checkable (the library's own MESH_COUNTERS read).
  EXPECT_EQ(bench.agent.half().mesh.counters().tx, bench.rak().client_tx());
}

TEST(AgentMesh, TelemetryAndConfigReadOnTheOwnRepeaterGoThroughTheLocalDoorAndPostResults) {
  MeshBench bench;
  bench.agent.modemWorks();
  bench.agent.server().queue(std::vector<std::string>{bench::ownTelemetry(60).json(), bench::ownConfigRead(61).json()});
  interfaces::BackendAnswer out;
  const std::uint32_t tx_before = bench.agent.half().mesh.counters().tx;
  const interfaces::Exchanged how = bench.agent.exchange(bench::knowledgeFor(core::power::Band::kPlenty), 600, &out);
  EXPECT_EQ(how, interfaces::Exchanged::kAsked);
  // The local door: nothing on the air for either job.
  EXPECT_EQ(bench.agent.half().mesh.counters().tx, tx_before);
  EXPECT_EQ(bench.rak().client_tx(), 0u);
  // A telemetry: served, the LPP channels as the fixture decodes them.
  const std::string telemetry = bench.agent.lastResult(60);
  ASSERT_FALSE(telemetry.empty());
  EXPECT_EQ(member(telemetry, "outcome"), "ok");
  EXPECT_NE(data(bench, 60).find("\"channels\""), std::string::npos);
  // A config.read: served without the ACL, the keys it asked for read back.
  const std::string config = bench.agent.lastResult(61);
  ASSERT_FALSE(config.empty());
  EXPECT_EQ(member(config, "outcome"), "ok");
  EXPECT_NE(data(bench, 61).find("TwinOak Risskov"), std::string::npos);
}

}  // namespace
