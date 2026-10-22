// The library's FakeMesh behind the link (task p4-02): the runner's scenarios, again, through the
// UART framing.
//
// The agent library's test_runner suite drives `fake::FakeMesh` - its simulated stock repeater,
// which answers a login, a request, a CLI line, a discovery and a trace the way MyMesh.cpp and
// CommonCLI.cpp at the MeshCore pin do - and asserts on what it recorded. This header shadows
// that class, the way host_test/shim/support/FakePlatform.h shadows the platform, with one that keeps every
// knob and every record of the original (`grant`, `answerLine`, `beSilent`, `sends()`,
// `timesSent()`, ...) and puts a simulated RAK (sim/sim_rak.h) and the agent's LinkMesh
// (components/agent/) between the runner and the repeater:
//
//     JobRunner -> mm::IMesh = LinkMesh -> link client -> UART -> SimRak -> IMesh = FakeMeshRepeater
//
// So a scenario's `mesh.sendLogin(...)` is a SEND_LOGIN frame, its reply an EV_MESH_REPLY event,
// and `mesh.sends()` what the repeater on the far side was asked for - the same assertions, now
// through the framing. The 46 cases of test_runner.cpp are compiled a second time against this
// directory (host_test/CMakeLists.txt, `agent_core_runner_link_tests`), unchanged.
//
// Two things this class adds to every scenario, because the task asks for them in every one: at
// the end of every job `client_tx` - MESH_COUNTERS as LinkMesh read it - must equal the number of
// sends the repeater was asked for, exactly; and the RAK's own `tx` must not be what the mesh
// reports, because that one counts forwards.
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include <support/FakePlatform.h>  // host_test/shim's: fake::FakePlatform carries the link's clock

#define FakeMesh FakeMeshRepeater
#include_next <support/FakeMesh.h>
#undef FakeMesh

#include "sim/mesh_world.h"

namespace fake {

class FakeMesh : public FakeMeshRepeater {
 public:
  explicit FakeMesh(FakePlatform& platform)
      : FakeMeshRepeater(platform), inner_(*this), world_(platform.linkClock, inner_) {}

  ~FakeMesh() override {
    // Whatever the test did, nothing was left awaited on the RAK and the counters agree.
    EXPECT_FALSE(world_.rak.awaiting()) << "a reply was still awaited on the RAK when the test ended";
    holdCountersEqual("at the end of the test");
  }

  // The link under the runner, for the tests of components/agent/ that want its knobs.
  sim::MeshWorld& world() { return world_; }
  agent::LinkMesh& link() { return world_.mesh; }
  // The repeater on the far side, as itself: what a test sets up and reads back.
  FakeMeshRepeater& repeater() { return *this; }

  // ------------------------------------------------------------------ IMesh, over the link

  void setRing(mm::ObservationRing* ring) override { world_.mesh.setRing(ring); }
  const mm::MeshCounters& counters() const override { return world_.mesh.counters(); }
  uint32_t overheard() const override { return world_.mesh.overheard(); }
  // Two of the repeater's methods call its own virtuals - await() calls loop(), beginJob()
  // calls endJob() - and those land here, on the most-derived class. While the RAK is driving
  // the repeater (`rak.busy()`) they are the repeater's own and go no further; from the runner
  // they are the link's.
  void loop() override {
    FakeMeshRepeater::loop();  // the repeater's own count of pumps
    if (!world_.rak.busy()) world_.mesh.loop();
  }
  bool supports(const char* jobType) const override {
    return FakeMeshRepeater::supports(jobType) && world_.mesh.supports(jobType);
  }

  bool beginJob(const char* publicKeyHex, const uint8_t* pathHashes, uint8_t hopCount, uint8_t hashSize) override {
    return world_.mesh.beginJob(publicKeyHex, pathHashes, hopCount, hashSize);
  }
  void endJob() override {
    if (world_.rak.busy()) {
      FakeMeshRepeater::endJob();
      return;
    }
    world_.mesh.endJob();
    holdCountersEqual("after endJob()");
  }
  void setDirectPath() override { world_.mesh.setDirectPath(); }
  void forgetPath() override { world_.mesh.forgetPath(); }
  bool hasPath() const override { return world_.mesh.hasPath(); }
  uint8_t pathHopCount() const override { return world_.mesh.pathHopCount(); }
  int describePath(char hops[][8], int max) const override { return world_.mesh.describePath(hops, max); }

  mm::SendResult sendLogin(const char* password, uint32_t& tag) override {
    return world_.mesh.sendLogin(password, tag);
  }
  mm::SendResult sendRequest(uint8_t reqType, const uint8_t* data, uint8_t len, uint32_t& tag) override {
    return world_.mesh.sendRequest(reqType, data, len, tag);
  }
  mm::SendResult sendCli(const char* text, bool sensitive, uint32_t& timestamp) override {
    return world_.mesh.sendCli(text, sensitive, timestamp);
  }
  mm::SendResult sendDiscovery(uint32_t& tag) override { return world_.mesh.sendDiscovery(tag); }
  mm::SendResult sendTrace(const uint8_t* roundTripHashes, uint8_t hashBytes, uint8_t hashSize,
                           uint32_t& tag) override {
    return world_.mesh.sendTrace(roundTripHashes, hashBytes, hashSize, tag);
  }
  bool awaitReply(uint32_t tag, uint32_t timeoutMs, void (*pump)()) override {
    return world_.mesh.awaitReply(tag, timeoutMs, pump);
  }
  bool awaitText(uint32_t timeoutMs, void (*pump)()) override { return world_.mesh.awaitText(timeoutMs, pump); }
  const mm::MeshReply& reply() const override { return world_.mesh.reply(); }

 private:
  // The repeater's own IMesh, reached without the overrides above: what the RAK drives.
  class Inner final : public mm::IMesh {
   public:
    explicit Inner(FakeMesh& outer) : o_(outer) {}
    void setRing(mm::ObservationRing* ring) override { o_.FakeMeshRepeater::setRing(ring); }
    const mm::MeshCounters& counters() const override { return o_.FakeMeshRepeater::counters(); }
    uint32_t overheard() const override { return o_.FakeMeshRepeater::overheard(); }
    void loop() override {}
    bool supports(const char* jobType) const override { return o_.FakeMeshRepeater::supports(jobType); }
    bool beginJob(const char* key, const uint8_t* hashes, uint8_t hops, uint8_t size) override {
      return o_.FakeMeshRepeater::beginJob(key, hashes, hops, size);
    }
    void endJob() override { o_.FakeMeshRepeater::endJob(); }
    void setDirectPath() override { o_.FakeMeshRepeater::setDirectPath(); }
    void forgetPath() override { o_.FakeMeshRepeater::forgetPath(); }
    bool hasPath() const override { return o_.FakeMeshRepeater::hasPath(); }
    uint8_t pathHopCount() const override { return o_.FakeMeshRepeater::pathHopCount(); }
    int describePath(char hops[][8], int max) const override { return o_.FakeMeshRepeater::describePath(hops, max); }
    mm::SendResult sendLogin(const char* password, uint32_t& tag) override {
      return o_.FakeMeshRepeater::sendLogin(password, tag);
    }
    mm::SendResult sendRequest(uint8_t reqType, const uint8_t* data, uint8_t len, uint32_t& tag) override {
      return o_.FakeMeshRepeater::sendRequest(reqType, data, len, tag);
    }
    mm::SendResult sendCli(const char* text, bool sensitive, uint32_t& timestamp) override {
      return o_.FakeMeshRepeater::sendCli(text, sensitive, timestamp);
    }
    mm::SendResult sendDiscovery(uint32_t& tag) override { return o_.FakeMeshRepeater::sendDiscovery(tag); }
    mm::SendResult sendTrace(const uint8_t* hashes, uint8_t bytes, uint8_t size, uint32_t& tag) override {
      return o_.FakeMeshRepeater::sendTrace(hashes, bytes, size, tag);
    }
    bool awaitReply(uint32_t tag, uint32_t timeoutMs, void (*pump)()) override {
      return o_.FakeMeshRepeater::awaitReply(tag, timeoutMs, pump);
    }
    bool awaitText(uint32_t timeoutMs, void (*pump)()) override {
      return o_.FakeMeshRepeater::awaitText(timeoutMs, pump);
    }
    const mm::MeshReply& reply() const override { return o_.FakeMeshRepeater::reply(); }

   private:
    FakeMesh& o_;
  };

  // The one-send-per-command rail, made checkable across the link (docs/library.md §11):
  // client_tx as the mesh read it, the RAK's own count of packets, and the repeater's count of
  // sends - all three equal, exactly - and the RAK's `tx` not among them.
  void holdCountersEqual(const char* when) const {
    if (world_.rak.counter_reads == 0) return;  // never read: nothing to hold (a test with no job)
    const uint32_t reported = world_.mesh.counters().tx;
    const uint32_t on_the_air = world_.rak.client_tx();
    const uint32_t asked_for = FakeMeshRepeater::counters().tx;
    EXPECT_EQ(reported, asked_for) << when << ": client_tx as LinkMesh read it is not the sends the repeater was asked for";
    EXPECT_EQ(on_the_air, asked_for) << when << ": the RAK's client_tx is not the sends the repeater was asked for";
    EXPECT_EQ(reported, world_.sendsExecuted()) << when << ": client_tx is not the SEND_* requests the RAK executed";
    EXPECT_NE(world_.mesh.radioTx(), reported) << when << ": the radio's tx (forwards included) must not be the mesh's";
  }

  Inner inner_;
  sim::MeshWorld world_;
};

}  // namespace fake
