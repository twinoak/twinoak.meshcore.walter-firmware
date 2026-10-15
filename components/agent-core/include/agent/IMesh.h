// The mesh as the job runner sees it (task p1.5-15): one repeater at a time, one packet per
// send, one reply awaited - AgentMesh's public surface as JobRunner and LoginSequence use it, and
// nothing more.
//
// Two implementations are meant to stand behind it. `AgentMesh` (src/mesh/AgentMesh.h) is the
// in-process one, on MeshCore's own primitives, which is what the Heltec runs. The Walter of the
// repeater-v2 design implements it as a link client: each call below is one request of its
// `LINK-PROTOCOL.md` §6 (`JOB_BEGIN`, `PATH_SET`, the five `SEND_*`, `REPLY_GET`), and awaitReply()
// is the `EV_MESH_REPLY` that follows a send. The link was written against this list
// (`SYSTEM.md` S-4), and proposal 03 of that repository is why the list is an interface now.
//
// Nothing of MeshCore is included here, on purpose: the reply the runner reads is a struct of the
// library's own, sized by the two constants below, and AgentMesh.h holds those against MeshCore's
// MAX_PACKET_PAYLOAD and MAX_PATH_SIZE at compile time so that a MeshCore bump that moves either is
// a build error there and not a silent overrun here.
//
// Two things the in-process mesh did not need and the link does, made here so that the in-process
// end loses nothing: sendCli() *returns* the sender timestamp it drew rather than taking one (on
// the link the RAK draws it at the moment it builds the packet, §6.2), and the reply's buffers
// are the protocol's sizes rather than a header's.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/Observations.h>
#include <agent/ReplyCodec.h>

namespace mm {

// The sizes a reply is held in. These are MeshCore's at the pin (MeshCore.h at d9296435:
// MAX_PACKET_PAYLOAD 184, MAX_PATH_SIZE 64) written as the library's own numbers, and the link
// protocol's too - LINK-PROTOCOL §6.1 caps a path at 64 bytes. AgentMesh.h asserts that
// MeshCore's fit inside them.
constexpr size_t kMeshReplyDataCap = 184;  // a RESPONSE after its four-byte tag is never longer than a payload
constexpr size_t kMeshPathCap = 64;        // a path's hashes, a trace's hashes and its per-hop SNRs

struct MeshCounters {
  uint32_t rx = 0;             // every frame that parsed
  uint32_t rxByType[16] = {};  // by payload type (0x00..0x0F, MeshCore's PAYLOAD_TYPE_*)
  uint32_t tx = 0;             // what left the radio: one per transaction, and nothing else
  uint32_t adverts = 0;        // adverts that verified and became observations
  uint32_t advertsDropped = 0; // adverts whose app_data did not decode
};

// What came back from a repeater during a job. One instance, reused: a board runs one
// transaction at a time, by the budget of EXIT-NODES §8.
struct MeshReply {
  enum class Kind : uint8_t { None, Response, Trace, Text };

  Kind kind = Kind::None;
  uint32_t tag = 0;
  bool viaPath = false;  // it came back as a returned path, which also taught us the way out

  // A RESPONSE, after the four-byte tag: the repeater's own reply bytes (mesh/ReplyCodec.h).
  uint8_t data[kMeshReplyDataCap] = {};
  uint8_t len = 0;

  // A TXT_MSG: the CLI reply's text, as CommonCLI wrote it (at most 160 characters), untrimmed.
  char text[kMaxCliReplyLen + 1] = {0};

  float snr = 0;  // what this board measured on the reply

  // A TRACE, as Mesh::onTraceRecv delivers it.
  uint8_t traceFlags = 0;
  uint8_t traceHashes[kMeshPathCap] = {};
  uint8_t traceHashBytes = 0;
  uint8_t traceSnrs[kMeshPathCap] = {};
  uint8_t traceSnrCount = 0;
  float traceFinalSnr = 0;
};

// What a send came to. The words are MeshCore's MSG_SEND_* and mean the same thing.
enum class SendResult : uint8_t { Failed, Flood, Direct };

// The request types of examples/simple_repeater/MyMesh.cpp at d9296435 that the runner sends.
// kReqTypeGetAccessList (0x05) is beside its parser in mesh/ReplyCodec.h.
constexpr uint8_t kReqTypeGetStatus = 0x01;
constexpr uint8_t kReqTypeGetTelemetry = 0x03;
constexpr uint8_t kReqTypeGetNeighbours = 0x06;
constexpr uint8_t kReqTypeGetOwnerInfo = 0x07;

class IMesh {
 public:
  virtual ~IMesh() = default;

  // Where a heard advert goes. Until this is set, adverts are counted and dropped. (On the link
  // this is LINK-PROTOCOL §7, OBS_PULL, not §6: the Walter pulls the RAK's ring into this one.)
  virtual void setRing(ObservationRing* ring) = 0;

  virtual const MeshCounters& counters() const = 0;

  // The heartbeat's `overheard`: frames of the mesh's own chat, counted and never read.
  virtual uint32_t overheard() const = 0;

  // The pump: the runner calls it while it idles for the budget, so what the radio hears is
  // taken in while a job waits. The in-process mesh runs MeshCore's loop; a link client reads
  // its link.
  virtual void loop() = 0;

  // ------------------------------------------------------------------ a job's exchanges

  // The repeater this job is for, and the path the server knows to it (the hop hashes as the
  // claim carries them, `hashSize` bytes each). With no path the mesh floods, which is what
  // EXIT-NODES §3 asks for: the server never guesses, and a companion that knows no route finds
  // one. Returns false when the key is not 64 hex characters.
  virtual bool beginJob(const char* publicKeyHex, const uint8_t* pathHashes, uint8_t hopCount, uint8_t hashSize) = 0;

  // No job in flight: the peer is forgotten, the shared secret is wiped, and a RESPONSE from
  // anybody is released again before it is decrypted.
  virtual void endJob() = 0;

  // The server knows the repeater as a direct neighbour: a path of zero hops, which is sent
  // DIRECT with an empty path rather than flooded.
  virtual void setDirectPath() = 0;
  // The path is stale (a direct login along it got nothing back): forget it, so the next send
  // floods and the reply teaches a fresh one.
  virtual void forgetPath() = 0;

  virtual bool hasPath() const = 0;
  virtual uint8_t pathHopCount() const = 0;
  // The out-path as hex hops, for the result's `pathUsed`; returns how many were written.
  virtual int describePath(char hops[][8], int max) const = 0;

  // A login as ourselves (EXIT-NODES §7). With `password` empty or nullptr it is the ACL login,
  // which offers the repeater our own key and no secret at all; with one it is the guest or
  // admin login, and the plaintext that carried the password is wiped as soon as the packet is
  // encrypted (at most kMaxPasswordLen bytes go, as BaseChatMesh::sendLogin sends them). `tag`
  // is what the reply is matched by - 0 for a login, whose reply carries the peer's timestamp.
  virtual SendResult sendLogin(const char* password, uint32_t& tag) = 0;

  // REQ_TYPE_* with its request data, on the session the login opened.
  virtual SendResult sendRequest(uint8_t reqType, const uint8_t* data, uint8_t len, uint32_t& tag) = 0;

  // One CLI line as a TXT_MSG datagram of type CLI data, on the admin session. The mesh draws
  // the sender timestamp - the one the repeater judges replays by, which must be strictly above
  // the last one it saw from this key (MyMesh::onPeerDataRecv at d9296435 answers an equal one
  // with nothing and does not run the command) - from its unique-time series, puts it in the
  // packet and hands it back in `timestamp`; a retry calls again and gets a fresh one. The reply
  // is a TXT_MSG of the repeater's own and is awaited with awaitText(). `sensitive` is a line
  // made from a password: its frame, and the reply awaited for it, are printed with their bodies
  // as stars.
  virtual SendResult sendCli(const char* text, bool sensitive, uint32_t& timestamp) = 0;

  // The path-discovery request as the companion's CMD_SEND_PATH_DISCOVERY_REQ makes it: a
  // base-telemetry REQ, always flooded, whose reply comes back as a returned path. The last
  // resort of a discover.
  virtual SendResult sendDiscovery(uint32_t& tag) = 0;

  // A trace along the round-trip path, which is the way out, the repeater, and the way back.
  // `hashSize` is 1, 2, 4 or 8: a trace packet cannot express three-byte hashes, and asking for
  // them is Failed.
  virtual SendResult sendTrace(const uint8_t* roundTripHashes, uint8_t hashBytes, uint8_t hashSize, uint32_t& tag) = 0;

  // Wait for the RESPONSE (or TRACE) with this tag - 0 is "the next reply from the peer" -
  // pumping the mesh (and whatever `pump` is) meanwhile. Returns false on silence. The reply is
  // reply().
  virtual bool awaitReply(uint32_t tag, uint32_t timeoutMs, void (*pump)()) = 0;
  // Wait for a CLI reply: the first TXT_MSG of CLI data from the peer, which carries no tag.
  virtual bool awaitText(uint32_t timeoutMs, void (*pump)()) = 0;
  virtual const MeshReply& reply() const = 0;

  // **`uniqueTime()` is not here, and that is deliberate** (task p1.5-17, the follow-up task 15
  // left). The unique second every tag and every sender timestamp comes from is the *mesh's* to
  // draw: since task p1.5-15 every send draws its own and hands it back, nothing in the library
  // asks for one, and `LINK-PROTOCOL.md` §6.2 says the same of the link - "all timestamps the
  // client sends come from the RAK's clock, so the Walter never supplies one". Keeping it on the
  // interface would have made every implementation answer a question nobody asks, and the link
  // would have needed a request for it. The in-process mesh keeps the method as its own
  // (src/mesh/AgentMesh.h), because that is where the series is drawn.
};

}  // namespace mm
