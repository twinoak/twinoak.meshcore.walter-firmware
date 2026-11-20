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
// are the protocol's sizes rather than a header's. And one thing the link needed and the
// in-process mesh never will (task p1.6-23): supports(), the mesh's own answer to *can you carry
// a job of this type at all* - a UART link that cannot carry a trace says so once, here, and the
// unit neither advertises `trace` nor is sent one.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/FloodScope.h>
#include <agent/Observations.h>
#include <agent/ReplyCodec.h>

namespace mm {

// The sizes a reply is held in. These are MeshCore's at the pin (MeshCore.h at 81dbae59:
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
//
// Two fields that were here until task p1.6-23 are gone, because every implementation filled
// them and nothing read them: `viaPath` (the reply came back as a returned path) is what
// `hasPath()` answers after a flooded send, which is how the discover job reads it; and `snr`
// (what this board measured on the reply) had no field of any result to go into - a status's
// `lastSnr` and a neighbour's `snr` are the repeater's own figures out of its reply bytes, and
// the one SNR of ours a result carries is a trace's, which is `traceFinalSnr` below. A reply's
// SNR is yours to print; the library has nowhere to send it.
struct MeshReply {
  enum class Kind : uint8_t { None, Response, Trace, Text };

  Kind kind = Kind::None;
  uint32_t tag = 0;

  // A RESPONSE, after the four-byte tag: the repeater's own reply bytes (mesh/ReplyCodec.h).
  uint8_t data[kMeshReplyDataCap] = {};
  uint8_t len = 0;

  // A TXT_MSG: the CLI reply's text, as CommonCLI wrote it (at most 160 characters), untrimmed.
  char text[kMaxCliReplyLen + 1] = {0};

  // A TRACE, as Mesh::onTraceRecv delivers it. `traceFinalSnr` is what this board measured on
  // the trace coming home, and is the SNR of the result's last hop - this board's own.
  uint8_t traceFlags = 0;
  uint8_t traceHashes[kMeshPathCap] = {};
  uint8_t traceHashBytes = 0;
  uint8_t traceSnrs[kMeshPathCap] = {};
  uint8_t traceSnrCount = 0;
  float traceFinalSnr = 0;
};

// What a send came to. The words are MeshCore's MSG_SEND_* and mean the same thing.
enum class SendResult : uint8_t { Failed, Flood, Direct };

// The request types of examples/simple_repeater/MyMesh.cpp at 81dbae59 that the runner sends.
// kReqTypeGetAccessList (0x05) is beside its parser in mesh/ReplyCodec.h.
constexpr uint8_t kReqTypeGetStatus = 0x01;
constexpr uint8_t kReqTypeGetTelemetry = 0x03;
constexpr uint8_t kReqTypeGetNeighbours = 0x06;
constexpr uint8_t kReqTypeGetOwnerInfo = 0x07;

// The FIRMWARE_VER_LEVEL the owner-info request arrived at: `#define REQ_TYPE_GET_OWNER_INFO
// 0x07 // FIRMWARE_VER_LEVEL >= 2` of the same file, whose `FIRMWARE_VER_LEVEL` is 2. A login
// reply carries the repeater's own level (ReplyCodec.h's LoginReply), and a repeater below this
// one answers the request with silence rather than with a refusal, so asking buys a timeout and
// nothing else (meshmaster.dk's task p4.30-181, OpCodes.OwnerInfoVerLevel there).
constexpr uint8_t kOwnerInfoVerLevel = 2;

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

  // What this mesh cannot do (task p1.6-23). Asked for each of the thirteen core job types
  // (JobEnvelopes.h's kJobTypes) when the heartbeat is built, and once more before a job of one
  // of them is run: a type answered false is **not advertised**, so the server routes none of it
  // here (EXIT-NODES §9 - routing requires the type advertised), and a job of it that arrives all
  // the same is reported `unsupported` at no transaction with nothing sent, before `beginJob`.
  // The extension's types (IJobExtension.h) are not asked: those are the consumer's own.
  //
  // The default is *everything*, which is what every mesh was until now and what the in-process
  // mesh is: MeshCore on this board can send every packet the thirteen need. A link client
  // answers from what its link carries - the Walter's UART link cannot carry a trace, so it
  // answers false for "trace" and the unit never advertises it. Cheap, const, never blocks, and
  // `jobType` is one of the static strings of kJobTypes, so a `strcmp` is the whole of it.
  virtual bool supports(const char* jobType) const {
    (void)jobType;
    return true;
  }

  // ------------------------------------------------------------------ the flood scope

  // The region every flood of this mesh goes out in (meshmaster.dk's D-47; task p1.8-32): a
  // region's name, `*` for *no transport code at all*, or **nullptr for a mesh that says nothing
  // about scope** - whatever its radio holds decides, which is every exit node built before the
  // setting existed and what the heartbeat leaves the `flood` object out for. The string outlives
  // the call; nothing copies it.
  virtual const char* floodScope() const { return nullptr; }

  // Scoped or unscoped for the **next** flood, told per send and never per job (the companion's
  // own `send_unscoped`, examples/companion_radio/MyMesh.cpp at 81dbae59). A direct send ignores
  // it, and so does a mesh whose scope is `*` or nullptr - there is nothing to drop. The rule
  // that decides which send gets which is agent/FloodScope.h's and lives in the job runner.
  virtual void setFloodMode(FloodMode mode) { (void)mode; }

  // ------------------------------------------------------------------ the path-hash width

  // The path-hash width every flood of this mesh originates, in bytes (task p1.10-39). The
  // library chooses a width in exactly one place - a trace to a **direct neighbour**, where there
  // is no learned hop to take a width from and the only hash is one it computes itself from the
  // repeater's key - and this is the number it chooses. Everywhere else it passes through the
  // width the claim's path carried, because the extra byte of a narrow hop belongs to no node.
  //
  // The default is **2**, which is the rail of this repository and meshmaster.dk's D-33: path
  // hashes are two bytes or more, never one, because two repeaters that share a first byte both
  // repeat a packet addressed to that byte - the first bench showed exactly that, and so did job
  // 20005 of 5 November 2026, a trace whose `86` could have been any of three repeaters. A mesh
  // that overrides nothing floods no narrower than the rail allows. Override it only to say
  // *wider*: 1, 2, 4 and 8 are what a trace packet can state (`isTraceableHashSize`), and a mesh
  // answering anything else makes a trace to a direct neighbour `unsupported` rather than a frame
  // nothing on the path can follow.
  virtual uint8_t floodPathHashSize() const { return 2; }

  // ------------------------------------------------------------------ the local door

  // **This job is answered without the air** (task p1.10-40): true from a beginJob() for a
  // repeater this mesh reaches by something other than the mesh, until the endJob() that closes
  // that job - and false between jobs. The one such mesh is the repeater-v2 unit's Walter, whose
  // own radio is wired to it over a UART: a job for *that* radio is served over the link (the
  // *local door* of `components/agent/include/walter_agent/link_mesh.h` in
  // twinoak.meshcore.walter-firmware) and nothing leaves an antenna.
  //
  // Such a job **neither waits for the airtime budget nor spends it**, and its exchanges count
  // no transaction. The budget exists to share one radio channel (EXIT-NODES §8), and a job that
  // never touches the channel has nothing to share: before this, a 34-line `config.read` of a
  // unit's own repeater was paced at the default *2 a minute, gap 20 s* and spent eleven minutes
  // waiting for air it was not going to use - on 7 November 2026 the first of those waits, about
  // 40 s, outlived the Walter's task watchdog and reset it mid-job.
  //
  // Such a job also **logs in with no material at all** (task p1.10-41): the door answers the
  // login itself, at admin, whatever it carried, so `loginMaterialFor` answers the empty string
  // at every class - `admin` included, where a claim with no password is otherwise *nothing to
  // log in with* and the job is refused unsent. A unit therefore configures its own repeater with
  // no admin password stored anywhere, and a password the claim did carry is not sent to a door
  // that would ignore it. If your mesh answers true, the login you are handed may be the blank
  // one at any class.
  //
  // Two things do **not** change for a local job. The **lease** still ends it: a lease is the
  // server's claim on the repeater and says nothing about the air. And the result's
  // `transactions` stays what EXIT-NODES §7 defines it as - "one request/response exchange with
  // one repeater over the mesh" - so a local job reports **0** of them, which is the reading that
  // keeps meshmaster.dk's planned load a figure about the air.
  //
  // The default is **false**: a mesh that overrides nothing is on the air for every job, which is
  // what every mesh was until now and what this board's `AgentMesh` is - the Heltec's radio is
  // its own and it has no local door. Cheap, const, never blocks.
  virtual bool answersLocally() const { return false; }

  // **Would a job for this repeater be answered without the air?** - the same door, asked for a
  // key **before** any job has begun (task p1.10-43). `answersLocally()` above cannot answer it:
  // it is true only from a `beginJob` to its `endJob`, and the caller that needs to know is the
  // one which **prices** a claimed job before it runs it (`AgentLoop::wakeJobs`). The two must
  // agree: this is true for a key exactly when a job begun for that key would make
  // `answersLocally()` true. The key is 64 lowercase hex characters, as a claim carries it and as
  // `beginJob` takes it; a key this mesh cannot parse is false, not an error.
  //
  // A job answered true here is priced at **its reply waits**: no airtime gaps and no
  // transactions, because neither is spent on a job that never touches the channel. Priced as air
  // instead, a 34-line `config.read` of a unit's own repeater looked like 34 x (20 s + 8 s), 952
  // s, to a wake that would really have finished it in seconds - and a wake slice shorter than
  // that refused, at no transaction and in no time at all, the very job `answersLocally()` made
  // fast.
  //
  // The default is **false**, as above: a mesh that overrides nothing is priced as air for every
  // job, which is what this board's `AgentMesh` is. Cheap, const, never blocks.
  virtual bool answersLocallyFor(const char* publicKeyHex) const {
    (void)publicKeyHex;
    return false;
  }

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
  // the last one it saw from this key (MyMesh::onPeerDataRecv at 81dbae59 answers an equal one
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
