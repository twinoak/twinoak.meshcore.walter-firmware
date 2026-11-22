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
// through the framing. The 123 cases of test_runner.cpp are compiled a second time against this
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
#include <map>
#include <string>

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

  // The exit node's own key. On a Walter the unit *is* its repeater (spec/SYSTEM.md S-1) and
  // this key comes from §5.6 IDENTITY, which every wake reads before it claims (task p8-04);
  // `LinkMesh::supports()` carries no mesh job until it has one, because without it a job for
  // the unit's own repeater cannot be told from a stranger's. The library's scenarios hand the
  // key over with this call, so the shim arms the link's own key from the same one - the
  // scenarios themselves are unchanged. A job for the *target* repeater stays a mesh job: the
  // two keys differ, which is what a Heltec's arrangement models on a Walter's wire.
  void setSelfKey(const std::string& hex64) {
    FakeMeshRepeater::setSelfKey(hex64);
    uint8_t key[32];
    if (hex64.size() != 64) return;
    for (size_t i = 0; i < 32; i++) {
      const int high = hexNibble(hex64[2 * i]);
      const int low = hexNibble(hex64[2 * i + 1]);
      if (high < 0 || low < 0) return;
      key[i] = static_cast<uint8_t>((high << 4) | low);
    }
    world_.mesh.setOwnKey(key);
  }

  // **A reply that comes home late** (the library's tasks p1.10-47 and p1.10-49). The original
  // holds it in a list of its own and delivers it in the job's next wait, counting waits as calls
  // of its own `await()`. Through the RAK a wait is a thousand of those - the radio polls the
  // repeater a millisecond at a time (sim/sim_rak.h) - so the original's count would be spent
  // inside the request's own wait and the reply would not be late at all. So the lateness is
  // modelled where a wait is a wait on this road: the **radio** holds the answer and §6.2 decides
  // which later exchange may read it (`rak.late_replies`), and the repeater answers as it always
  // does. What a Walter then does with it is this firmware's own, and differs from the Heltec's:
  // §6.3 reports one outcome per exchange, so an exchange that was handed a late reply is over
  // and the reply it was waiting for is lost - the job's own retry is what recovers it.
  void deliverTheNextReplyLate(int sends) {
    world_.rak.late_replies = sends;
    world_.rak.late_reply_waits = 1;
  }
  // The same for a named console line (task p1.10-49): armed at the send, because the radio knows
  // a line's bytes and not the key the library gave it, and the line is named here as the
  // repeater **ran** it - without the prefix the line went out with, which is the original's rule.
  void deliverTheNextLineRepliesLate(const std::string& line, int times, int waits = 1) {
    lateLines_[line] = {times, waits};
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
  // §6.5, link 1.3 (task p14-01): the scope the runner's rule is applied to, and the mode it asks
  // for, are the **link's** - as every other call here is - so the library's own scenarios run
  // over `FLOOD_SCOPE` and over §6.2's `scoped` and not over the repeater fake's idea of a scope.
  // Against the scripted radio these scenarios use - a radio of 1.2 - `LinkMesh::floodScope()` is
  // null and `setFloodMode` sends nothing, which is exactly what the repeater fake answered
  // before this, so every one of them is unchanged until a test turns the radio's own switch
  // (`radio.speakLink13`).
  const char* floodScope() const override { return world_.mesh.floodScope(); }
  void setFloodMode(mm::FloodMode mode) override { world_.mesh.setFloodMode(mode); }

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

  // **The way back** (the library's `v0.7.6`, upstream's task p1.21-71): the link's, which tells
  // none. §6 of the link protocol has no request for a PATH packet, so `LinkMesh` takes the
  // library's default - nothing goes on the air, the answer is *not told*, and every reply to this
  // unit is flooded exactly as it was before the bump.
  //
  // It is overridden here and not left inherited, because the repeater fake **does** tell one: a
  // way back told on it would be a packet it sent to itself, never across the link, and the
  // repeater's count of sends would stand one above `client_tx` at the end of every job - which is
  // the whole rail this shim exists to hold.
  mm::IMesh::WayBack tellTheWayBack() override { return world_.mesh.tellTheWayBack(); }

  mm::SendResult sendLogin(const char* password, uint32_t& tag) override {
    return world_.mesh.sendLogin(password, tag);
  }
  mm::SendResult sendRequest(uint8_t reqType, const uint8_t* data, uint8_t len, uint32_t& tag) override {
    return world_.mesh.sendRequest(reqType, data, len, tag);
  }
  mm::SendResult sendCli(const char* text, bool sensitive, uint32_t& timestamp) override {
    armALateLineReply(text);
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
  // `deliverTheNextLineRepliesLate`, at the send: this line's reply is to come home late, and the
  // radio is armed for the one answer that follows. The line the library sends carries its
  // two-character prefix and a `|` (agent/CliReplies.h); the repeater runs what is behind it, and
  // that is the name a test uses.
  void armALateLineReply(const char* text) {
    if (text == nullptr || lateLines_.empty()) return;
    const char* bar = std::strchr(text, '|');
    const std::string ran = bar == nullptr ? std::string(text) : std::string(bar + 1);
    const std::map<std::string, LateLine>::iterator it = lateLines_.find(ran);
    if (it == lateLines_.end() || it->second.times == 0) return;
    it->second.times--;
    world_.rak.late_replies = 1;
    world_.rak.late_reply_waits = it->second.waits;
  }

  static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

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
    mm::IMesh::WayBack tellTheWayBack() override { return o_.FakeMeshRepeater::tellTheWayBack(); }
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

  struct LateLine {
    int times = 0;
    int waits = 1;
  };

  Inner inner_;
  sim::MeshWorld world_;
  std::map<std::string, LateLine> lateLines_;
};

}  // namespace fake
