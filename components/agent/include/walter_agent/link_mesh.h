// The mesh as the agent's runner sees it, over the box link (task p4-02): `mm::IMesh`
// (components/agent-core, agent/IMesh.h) with each call one request of spec/LINK-PROTOCOL.md §6
// (JOB_BEGIN, JOB_END, PATH_SET, PATH_GET, the five sends, REPLY_GET, MESH_COUNTERS) and, for the
// unit's own repeater, §5.7's CLI and §5.8's LPP_GET - the link was designed against this list
// (spec/SYSTEM.md S-4), which is why the mapping is flat. The report of this task has the table.
//
// What is here and nowhere else:
//
//   - **the reply.** A send puts one packet on the air (or none) and is answered at once; the
//     reply arrives later as EV_MESH_REPLY (§6.3). awaitReply()/awaitText() pump the link client
//     until that event comes, for the runner's timeout plus a margin, and then ask with REPLY_GET:
//     a lost event costs one request, never a lost reply. The record is mapped into the library's
//     MeshReply: state 3 -> Response, 4 -> Text, 5 -> Trace, 2 -> none and a false return.
//   - **the timeout's two ends.** §6.2 takes the timeout at the send and the library gives it at
//     the await, so the send carries a ceiling (kSendTimeoutCeilingMs) and the Walter's own timer
//     over the event is the authority. When this end gives up first the RAK would wait on and
//     answer the next send ERR_BUSY, so the wait is ended with JOB_BEGIN again - the same peer
//     and the path this end holds - which §6.1 says forgets any awaited reply.
//   - **the password's path.** sendLogin() copies at most 15 bytes into one buffer of its own,
//     hands it to the client, and wipes the copy before returning; the client wipes its request
//     and frame buffers after the exchange (core/link/client, `sensitive`). A sendCli() flagged
//     sensitive takes the same path with §6.2's flag bit. Nothing here prints either.
//   - **the path, cached.** hasPath()/pathHopCount()/describePath() read what this end last sent
//     or last read with PATH_GET, refreshed after every reply to a flooded send, because a
//     returned path changes it (§6.3) and that is how the route a flood discovered is learned.
//   - **the local door.** A job begun for the unit's own repeater (setOwnKey(); task p4-04
//     supplies it from IDENTITY) sends nothing on the mesh: the login is answered by this end
//     with a synthetic admin reply (the link has console privileges, §5.7), a CLI line is §5.7's
//     CLI request, a telemetry request is §5.8's LPP_GET at an admin's permissions, the access
//     list is answered with silence (the library reads that as *no ACL, no evidence either way*),
//     and status, neighbours, owner-info, discovery and trace have no request on the link that
//     yields the same bytes and are Failed. client_tx does not move for a local job.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IMesh.h>

#include "core/link/client/client.h"
#include "core/link/codec/messages.h"
#include "core/link/codec/view.h"
#include "core/link/codec/wire.h"
#include "interfaces/clock.h"

namespace agent {

namespace link = core::link;
namespace lc = core::link::client;

// §6.2's `timeout_ms` at the send: how long the RAK waits. The library decides the real wait at
// the await (docs/library.md §3.3: perHopMs x (hops + 1), a flood as a three-hop request, a login
// the longer of that and loginMs - 16 s at the claim's defaults), so this is a ceiling above every
// wait the library makes at any timeouts a claim could reasonably carry, and the timer that
// decides is this end's.
constexpr std::uint32_t kSendTimeoutCeilingMs = 60000;

// §6.3: "its own timer of timeout_ms plus a margin". The RAK answers within 250 ms (§10), so a
// reply that reached the RAK at the last moment is on this UART inside that.
constexpr std::uint32_t kReplyMarginMs = link::kAnswerWithinMs;

// The slice this end listens in while a reply is awaited: the client's own.
constexpr std::uint32_t kPollMs = lc::kPollMs;

// §5.8: LPP_GET's permissions for the unit's own repeater - what a mesh client logged in as admin
// would be told, which is every bit (MeshCore's telemetry permission bits, perm::*).
constexpr std::uint8_t kLocalLppPermissions = link::perm::kBase | link::perm::kLocation | link::perm::kEnvironment;

constexpr std::size_t kKeyHexLen = 64;
constexpr std::size_t kKeyLen = link::kPubkeySize;

// What this object did on the link, for the console and the tests. Saturating.
struct LinkMeshCounters {
  std::uint32_t requests = 0;       // §6 requests that went out (every typed call that reached the link)
  std::uint32_t refused = 0;        // answered with a status other than OK
  std::uint32_t faults = 0;         // no answer at all: link down, port closed, no session, malformed
  std::uint32_t sends = 0;          // SEND_* answered OK with a packet sent (send 1 or 2)
  std::uint32_t nothing_sent = 0;   // SEND_* answered OK with `send` 0
  std::uint32_t events = 0;         // EV_MESH_REPLY taken out of the client's queue
  std::uint32_t malformed = 0;      // of those, ones that would not decode
  std::uint32_t late_replies = 0;   // EV_MESH_REPLY with no wait in progress (this end had given up)
  std::uint32_t mismatched = 0;     // a reply of another kind or tag than the one awaited
  std::uint32_t reply_gets = 0;     // REPLY_GET sent because the event did not come in time
  std::uint32_t lost_events = 0;    // of those, the record was already final: the event was lost
  std::uint32_t cancelled = 0;      // JOB_BEGIN again, to end a wait the RAK was still in
  std::uint32_t path_reads = 0;     // PATH_GET after a reply to a flooded send
  std::uint32_t counter_reads = 0;  // MESH_COUNTERS
  std::uint32_t restarts = 0;       // the radio restarted under a mesh job: everything forgotten
  std::uint32_t local_jobs = 0;     // jobs begun for the unit's own repeater
  std::uint32_t local_requests = 0; // CLI and LPP_GET requests made for one
  std::uint32_t local_silent = 0;   // local sends answered with silence on purpose (the access list)
  std::uint32_t local_unserved = 0; // local sends the link has no request for (Failed)
};

// The link's own fields of the last reply record (§6.3), which the library's MeshReply has no
// field for since v0.3.0: yours to print, the console does.
struct LastRecord {
  std::uint8_t state = link::reply_state::kNone;
  bool via_path = false;
  std::int8_t snr_q4 = 0;
  std::int16_t rssi = link::kRssiUnknown;
  std::uint32_t elapsed_ms = 0;
  bool flood = false;  // the send this record answers went as a flood
};

class LinkMesh final : public mm::IMesh, public lc::RestartWatcher {
 public:
  LinkMesh(lc::Client& client, interfaces::Clock& clock);

  // ------------------------------------------------------------------ the unit's own repeater

  // The key IDENTITY answers (§5.6), 32 bytes: a job begun for it goes through the local door.
  // Until it is set, every job is a mesh job. Task p4-04 sets it.
  void setOwnKey(const std::uint8_t* key);
  void clearOwnKey();
  bool hasOwnKey() const { return own_key_known_; }

  // ------------------------------------------------------------------ what the console reads

  bool jobBegun() const { return job_.begun; }
  bool local() const { return job_.begun && job_.local; }
  bool awaiting() const { return await_ != Await::kNone; }
  // MESH_COUNTERS now, into counters(). False when the link did not answer; counters() then
  // keeps what it had.
  bool refreshCounters();
  // The radio's own `tx` (§6.4: every packet it transmitted, forwards included), which counters()
  // deliberately does not carry: an exit node on a repeater must not report forwards as its own.
  std::uint32_t radioTx() const { return radio_tx_; }
  // §6.4's `obs_dropped`: what the RAK's observation ring has lost since it booted. Not one of
  // the library's `MeshCounters` - its `observationsDropped` is its own ring's - and task
  // p4-04's `Observations` is what adds the increase to that figure, once.
  std::uint32_t radioObsDropped() const { return radio_obs_dropped_; }
  lc::Fault lastFault() const { return last_fault_; }
  std::uint8_t lastStatus() const { return last_status_; }
  const LinkMeshCounters& linkCounters() const { return link_counters_; }
  const LastRecord& lastRecord() const { return last_record_; }
  // The path this end holds, as the link describes it. The hashes point into this object.
  link::Path path() const;

  // ------------------------------------------------------------------ mm::IMesh

  void setRing(mm::ObservationRing* ring) override { ring_ = ring; }
  mm::ObservationRing* ring() const { return ring_; }
  const mm::MeshCounters& counters() const override { return counters_; }
  // §6.4 has no `overheard` and no count by type: nothing on this link carries it, and 0 says
  // *this transport cannot see chat* (docs/library.md §3.2). A specification gap, in the report.
  std::uint32_t overheard() const override { return 0; }
  void loop() override;
  bool supports(const char* jobType) const override;

  bool beginJob(const char* publicKeyHex, const std::uint8_t* pathHashes, std::uint8_t hopCount,
                std::uint8_t hashSize) override;
  void endJob() override;
  void setDirectPath() override;
  void forgetPath() override;
  bool hasPath() const override;
  std::uint8_t pathHopCount() const override;
  int describePath(char hops[][8], int max) const override;

  mm::SendResult sendLogin(const char* password, std::uint32_t& tag) override;
  mm::SendResult sendRequest(std::uint8_t reqType, const std::uint8_t* data, std::uint8_t len,
                             std::uint32_t& tag) override;
  mm::SendResult sendCli(const char* text, bool sensitive, std::uint32_t& timestamp) override;
  mm::SendResult sendDiscovery(std::uint32_t& tag) override;
  mm::SendResult sendTrace(const std::uint8_t* roundTripHashes, std::uint8_t hashBytes,
                           std::uint8_t hashSize, std::uint32_t& tag) override;

  bool awaitReply(std::uint32_t tag, std::uint32_t timeoutMs, void (*pump)()) override;
  bool awaitText(std::uint32_t timeoutMs, void (*pump)()) override;
  const mm::MeshReply& reply() const override { return reply_; }

  // ------------------------------------------------------------------ lc::RestartWatcher

  // §5.1: a changed boot_id means the job, the path and any awaited reply are gone on the RAK's
  // side. This end forgets them too; the next send is refused by the RAK (ERR_STATE) and the
  // library ends the job.
  void onRadioRestart(std::uint32_t boot_id, std::uint8_t reset_reason) override;

 private:
  enum class Await : std::uint8_t { kNone, kResponse, kText, kTrace };

  struct Job {
    bool begun = false;
    bool local = false;
    std::uint8_t peer[kKeyLen] = {};
  };

  // §6.1's path as this end last sent it or last read it.
  struct PathCache {
    std::uint8_t mode = link::path_mode::kNone;
    std::uint8_t hash_size = 0;
    std::uint8_t hop_count = 0;
    std::uint8_t hashes[link::kMaxPathBytes] = {};
  };

  template <typename Response>
  bool note(const lc::Answer<Response>& answer);
  mm::SendResult afterSend(const lc::Answer<link::SendResponse>& answer, Await kind,
                           std::uint32_t* tag, std::uint32_t* sender_ts);
  bool await(bool text, std::uint32_t tag, std::uint32_t timeoutMs, void (*pump)());
  // Drains EV_MESH_REPLY out of the client's queue. True when one resolved the wait in progress.
  bool takeMeshReplies();
  // The record into reply_ and last_record_. True for a reply (state 3, 4, 5); false for a
  // timeout (state 2), which is a resolved wait with nothing to read.
  bool recordFrom(const link::ReplyRecord& record);
  void clearReply();
  void cancelWait();
  void refreshPath();
  void pathSet(std::uint8_t mode);
  void cachePath(const link::Path& path);

  mm::SendResult localLogin();
  mm::SendResult localRequest(std::uint8_t reqType);
  mm::SendResult localCli(const char* text, std::size_t size, bool sensitive);
  bool localAwait(bool text, std::uint32_t tag);

  static bool parseKey(const char* hex, std::uint8_t* out);
  static bool sameKey(const std::uint8_t* a, const std::uint8_t* b);

  lc::Client& client_;
  interfaces::Clock& clock_;

  bool own_key_known_ = false;
  std::uint8_t own_key_[kKeyLen] = {};

  Job job_;
  PathCache path_;

  Await await_ = Await::kNone;
  std::uint32_t await_tag_ = 0;
  bool await_flood_ = false;
  bool resolved_ = false;  // a final record was taken while a wait was in progress
  bool local_held_ = false;  // the local door holds a reply for the await that follows

  mm::MeshReply reply_;
  LastRecord last_record_;
  mm::MeshCounters counters_;
  std::uint32_t radio_tx_ = 0;
  std::uint32_t radio_obs_dropped_ = 0;
  mm::ObservationRing* ring_ = nullptr;

  // The one buffer of this object a password is copied into, wiped before sendLogin() returns.
  std::uint8_t secret_[link::kMaxPassword] = {};

  lc::Fault last_fault_ = lc::Fault::kNone;
  std::uint8_t last_status_ = link::status::kOk;
  LinkMeshCounters link_counters_;
};

}  // namespace agent
