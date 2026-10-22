// A `mm::IMesh` that does nothing, for a simulated RAK whose test is not about the mesh client
// (task p4-04): `sim::SimRak` needs a repeater behind it because §6's sends are answered by one,
// and the §7 and §5 requests this task is about - `IDENTITY`, `CLI`, `OBS_PULL`,
// `MESH_COUNTERS` - need none.
//
// The library's own `FakeMesh` is the real simulated stock repeater and is what the runner's
// scenarios run against (host_test/agent-link/); it asserts with GoogleTest, so it belongs in
// the GoogleTest binaries and not in `core_tests`. This is the quiet alternative: every send
// answers `Failed`, every await is a silence, and the counters are zeros a test may set.
#pragma once

#include <cstdint>

#include <agent/IMesh.h>

namespace sim {

class QuietRepeater final : public mm::IMesh {
 public:
  // What MESH_COUNTERS reports through the RAK: a test may set them.
  mm::MeshCounters counters_out;

  void setRing(mm::ObservationRing* ring) override { ring_ = ring; }
  const mm::MeshCounters& counters() const override { return counters_out; }
  std::uint32_t overheard() const override { return 0; }
  void loop() override {}

  bool beginJob(const char*, const std::uint8_t*, std::uint8_t, std::uint8_t) override {
    return false;
  }
  void endJob() override {}
  void setDirectPath() override {}
  void forgetPath() override {}
  bool hasPath() const override { return false; }
  std::uint8_t pathHopCount() const override { return 0; }
  int describePath(char[][8], int) const override { return 0; }

  mm::SendResult sendLogin(const char*, std::uint32_t& tag) override {
    tag = 0;
    return mm::SendResult::Failed;
  }
  mm::SendResult sendRequest(std::uint8_t, const std::uint8_t*, std::uint8_t,
                             std::uint32_t& tag) override {
    tag = 0;
    return mm::SendResult::Failed;
  }
  mm::SendResult sendCli(const char*, bool, std::uint32_t& timestamp) override {
    timestamp = 0;
    return mm::SendResult::Failed;
  }
  mm::SendResult sendDiscovery(std::uint32_t& tag) override {
    tag = 0;
    return mm::SendResult::Failed;
  }
  mm::SendResult sendTrace(const std::uint8_t*, std::uint8_t, std::uint8_t,
                           std::uint32_t& tag) override {
    tag = 0;
    return mm::SendResult::Failed;
  }

  bool awaitReply(std::uint32_t, std::uint32_t, void (*)()) override { return false; }
  bool awaitText(std::uint32_t, void (*)()) override { return false; }
  const mm::MeshReply& reply() const override { return reply_; }

  mm::ObservationRing* ring() const { return ring_; }

 private:
  mm::MeshReply reply_;
  mm::ObservationRing* ring_ = nullptr;
};

}  // namespace sim
