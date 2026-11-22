// A stock repeater behind the mesh seam, on a host (task p1.5-17).
//
// `FakeMesh` is `mm::IMesh` (agent/IMesh.h) with one MeshCore repeater behind it, at the pin
// `platformio.ini`'s `[meshcore]` names. It is a **second reading of the firmware source** - what
// the host harness of `tasks/phase-1.1/08-the-host-harness.md` asked for and never got - written
// from `examples/simple_repeater/MyMesh.cpp` and `src/helpers/CommonCLI.cpp` in
// `.pio/libdeps/heltec_v3/MeshCore`, so that this fake and meshmaster.dk's `FakeCompanion.cs`
// can disagree with each other and one of them be wrong.
//
// It holds what a repeater holds: a name, an ACL of keys at levels (which on stock firmware *is*
// the login table), an admin password, a guest password or none, the configuration
// `CONFIGURATION.md` lists, a neighbour list, a region map and a clock. It answers `sendLogin`,
// `sendRequest`, `sendCli`, `sendDiscovery` and `sendTrace` the way that firmware does - **from
// the bytes of a companion fixture wherever one exists** (`tests/fixtures/companion/*.hex`, each
// already cited to the function that writes it) and from a reading of the pinned source where
// none does, with the citation beside the answer. The manifest below names every answer with its
// citation, and `Citations.EveryAnswerOfTheFakeCitesAFixtureOrAFirmwareFunction` walks this file
// against it the way `test_contract_fixtures` walks its manifest.
//
// Since task p1.21-71 it also holds what a repeater holds about the way **home**: an `out_path`
// per key of its access list, written by a way back from such a key (`tellTheWayBack`) and by
// nothing else, forgotten when a login from that key arrives as a flood - and a reply sent along
// it when it holds one, flooded when it does not. So a test can read, out of `MeshReply::route`
// and out of the job's log, the thing the field could only be read out of counters before.
//
// It is a repeater and not a radio: nothing here models airtime, collisions or the real region
// loader. A reply arrives after `directLatencyMs` or `floodLatencyMs` **on the fake clock**
// (support/FakePlatform.h), so a wait is asserted rather than slept, and every send is recorded
// with that clock's time - which is what *one send per command*, the minimum gap and the
// per-minute cap are asserted against. One reply can be **late** rather than lost
// (`deliverTheNextReplyLate`, task p1.10-47, and `deliverTheNextLineRepliesLate` for a named
// console line, task p1.10-49): it misses its own request's wait and turns up in the next one,
// which is the only timing of the air this fake models beyond a latency.
//
// ---- answers (the manifest) ------------------------------------------------------------------
//
// answer: login-ok              cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:135-144 MyMesh::handleLoginReq -- the reply is timestamp(4), RESP_SERVER_LOGIN_OK, 0, is_admin, permissions, a random blob(4), FIRMWARE_VER_LEVEL
// answer: login-ok              cite: fixture login-success.hex
// answer: login-ok              cite: fixture login-success-readonly.hex
// answer: login-acl-entry       cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:91-99 MyMesh::handleLoginReq -- a blank password is the ACL alone: the entry's own level, and no password check at all
// answer: login-password        cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:100-128 MyMesh::handleLoginReq -- the admin password is admin, the guest password is guest, and the password rewrites the entry's role
// answer: login-silent-password cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:106-111 MyMesh::handleLoginReq -- a password that is neither is `return 0`, which is silence and not a refusal
// answer: login-silent-replay   cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:113-117 MyMesh::handleLoginReq -- a sender timestamp at or below the last one is silence
// answer: login-another-reply   cite: fw companion-v1.18.0 81dbae59 examples/companion_radio/MyMesh.cpp:716-721 MyMesh::onContactResponse -- a response that is neither "OK" nor RESP_SERVER_LOGIN_OK is what the companion turns into PUSH_CODE_LOGIN_FAIL: no firmware at the pin sends a refusal to a login, so this is a reply to something else and the agent must not read it as one (task p1.10-47)
// answer: status                cite: fixture status-response.hex
// answer: status                cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:216-240 MyMesh::handleRequest -- REQ_TYPE_GET_STATUS answers every login, guests included
// answer: telemetry             cite: fixture telemetry-response.hex
// answer: telemetry             cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:241-263 MyMesh::handleRequest -- REQ_TYPE_GET_TELEMETRY_DATA answers Cayenne LPP after the tag
// answer: neighbours            cite: fixture binary-response-neighbours.hex
// answer: neighbours            cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:277-371 MyMesh::handleRequest -- REQ_TYPE_GET_NEIGHBOURS pages by the count asked for: total(i16), returned(i16), then prefix, heard_seconds_ago(u32), snr(i8)
// answer: owner-info            cite: fixture binary-response-owner-info.hex
// answer: owner-info            cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:372-375 MyMesh::handleRequest -- REQ_TYPE_GET_OWNER_INFO is "FIRMWARE_VERSION\nnode_name\nowner_info"
// answer: access-list           cite: fixture access-list-response.hex
// answer: access-list           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:264-276 MyMesh::handleRequest -- REQ_TYPE_GET_ACCESS_LIST is {pub_key[6], permissions} per entry, entries at permissions 0 skipped, admin only
// answer: request-silent        cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:376-377 MyMesh::handleRequest -- an unknown request type is `return 0`, which is silence
// answer: forgotten-session     cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:663-671 MyMesh::onPeerDataRecv -- a sender the login table does not hold is answered nothing at all
// answer: request-silent-replay cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:676-700 MyMesh::onPeerDataRecv -- a REQ whose timestamp is not above the client's last is silence
// answer: datagram-too-long     cite: fw companion-v1.18.0 81dbae59 src/Mesh.cpp:488-493 Mesh::createDatagram -- a plaintext past MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1) = 167 bytes is never sent at all
// answer: reply-padding         cite: fw companion-v1.18.0 81dbae59 src/Utils.cpp:85-126 Utils::encrypt -- the plaintext is zero-padded to the cipher block, so a reply arrives a multiple of sixteen long
// answer: reply-padding         cite: fixture binary-response-telemetry-padded.hex
// answer: path-return           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:596-608 MyMesh::onAnonDataRecv -- a flooded request is answered with a path return, which is how a login teaches the way home
// answer: way-back-stored        cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:766-782 MyMesh::onPeerPathRecv -- a PATH packet from a client in the access list is stored as that client's out_path by Packet::copyPath, and nothing else ever writes one
// answer: way-back-dropped       cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:626-634 MyMesh::searchPeersByHash -- a packet whose sender is not in the access list matches no peer, so a way back from a key this repeater does not hold is dropped before it is decrypted and no out_path is written
// answer: way-back-forgotten     cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:131-133 MyMesh::handleLoginReq -- a login that arrived as a flood sets out_path_len to OUT_PATH_UNKNOWN, so the way back is forgotten and the next reply is flooded
// answer: reply-direct           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:596-619 MyMesh::onAnonDataRecv -- a DIRECT login is answered along the stored out_path through mesh::chooseReplyRoute, and flooded when there is none
// answer: reply-direct           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:684-697 MyMesh::onPeerDataRecv -- a DIRECT request is answered along the stored out_path, and flooded when out_path_len is OUT_PATH_UNKNOWN
// answer: reply-direct           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:744-756 MyMesh::onPeerDataRecv -- a console reply takes the same two routes as a request's
// answer: cli-not-admin         cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:701-702 MyMesh::onPeerDataRecv -- a text message from a sender below admin is dropped without a word
// answer: cli-retry             cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:709-740 MyMesh::onPeerDataRecv -- a sender timestamp equal to the last is a retry: nothing is run and the empty reply is not sent, and one below it is a replay
// answer: cli-reply             cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:741-758 MyMesh::onPeerDataRecv -- a non-empty reply is a TXT_MSG of the repeater's own clock, TXT_TYPE_CLI_DATA in the flags byte, then the text
// answer: cli-setperm           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:1241-1262 MyMesh::handleCommand -- `setperm <pubkey-hex> <level>` answers OK, or Err - bad params / Err - bad pubkey / Err - invalid params
// answer: cli-setperm-zero      cite: fw companion-v1.18.0 81dbae59 src/helpers/ClientACL.cpp:122-134 ClientACL::applyPermissions -- a setperm at the guest role **deletes** the entry and moves the later ones down; it is not an entry held at guest, and a second setperm for the same key is then `Err - invalid params`
// answer: cli-setperm-zero      cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:670-751 MyMesh::onPeerDataRecv -- the reply is addressed to `client`, a pointer into the slot the sender held **before** handleCommand ran, so a deletion at or below that slot sends the "OK" to whichever client now sits in it and it never reaches us (task p1.10-48)
// answer: cli-region-load       cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:1199-1231 MyMesh::handleCommand -- in load mode a region line is answered with nothing and the blank line ends the load with `OK - loaded N regions`
// answer: cli-reboot            cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:196-197 CommonCLI::handleCommand -- `reboot` is _board->reboot(), which does not return: the silence is the answer
// answer: cli-advert            cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:202-209 CommonCLI::handleCommand -- `advert` answers `OK - Advert sent` and `advert.zerohop` answers `OK - zerohop advert sent`
// answer: cli-time              cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:228-239 CommonCLI::handleCommand -- `time <unix>` above the clock answers `OK - clock set: ...` and at or below it `(ERR: clock cannot go backwards)`
// answer: cli-password          cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:267-272 CommonCLI::handleCommand -- `password <new>` stores at most fifteen bytes and echoes `password now: <what it stored>`
// answer: cli-ver               cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:283-284 CommonCLI::handleCommand -- `ver` is the firmware version and the build date
// answer: cli-board             cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:285-286 CommonCLI::handleCommand -- `board` is the board's manufacturer name
// answer: cli-unknown           cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:481-482 CommonCLI::handleCommand -- a command it does not know is `Unknown command`
// answer: cli-get               cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:701-834 CommonCLI::handleGetCmd -- a key it knows is `> <value>` and one it does not is `??: <key>`
// answer: cli-set               cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:486-699 CommonCLI::handleSetCmd -- a value it took is `OK` (`OK - repeat is now ON`/`OFF` for repeat), a key it does not know is `unknown config: <rest>`
// answer: cli-guest-password    cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:513-516 CommonCLI::handleSetCmd -- `set guest.password <new>` answers OK
// answer: cli-prefix            cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:1233-1239 MyMesh::handleCommand -- a line longer than four characters whose third character is `|` has its first three characters reflected at the head of the reply and the rest is run; the region-load branch returns before this, so a line sent in load mode keeps its prefix (task p1.10-49)
// answer: cli-region            cite: fw companion-v1.18.0 81dbae59 src/helpers/CommonCLI.cpp:889-988 CommonCLI::handleRegionCmd -- `region` exports the map into 160 characters, `region default` answers ` default scope is <name>`, `region load` answers nothing and `region save` answers OK
// answer: trace                 cite: fixture trace-data.hex
// answer: trace                 cite: fw companion-v1.18.0 81dbae59 src/Mesh.cpp:42-67 Mesh::onRecvPacket -- a TRACE states its hash size as an exponent in the flags byte and carries one SNR a hop, the final one measured on the packet home
// answer: discovery             cite: fw companion-v1.18.0 81dbae59 examples/companion_radio/MyMesh.cpp:1651-1679 MyMesh::handleCmdFrame -- path discovery is a flooded base-telemetry request, so what answers it is the telemetry reply and a path return
// answer: flood-scope           cite: fw repeater-v1.18.0 81dbae59 examples/simple_repeater/MyMesh.cpp:556-567 MyMesh::onRecvPacket -- a scoped flood is matched against the region map and an unscoped one against the wildcard, and a flood with no region behind it is forwarded by nobody (allowPacketForward, same file 434-443)
//
// ---- end of the manifest ---------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <agent/CliReplies.h>
#include <agent/IMesh.h>
#include <agent/Observations.h>
#include <agent/ReplyCodec.h>

#include <support/FakePlatform.h>
#include <support/Fixtures.h>

namespace fake {

// MeshCore's own constants at the pin, written here because this is a host build and includes
// nothing of MeshCore. Each names the line it came from.
constexpr uint8_t kRespServerLoginOk = 0;      // src/helpers/BaseChatMesh.h:21
constexpr uint8_t kFirmwareVerLevel = 2;       // examples/simple_repeater/MyMesh.cpp:44
constexpr size_t kDatagramPlaintextMax = 167;  // src/Mesh.cpp:490 — 184 - 2 - 15
constexpr size_t kCipherBlock = 16;            // src/MeshCore.h:14
constexpr size_t kReplyDataRoom = 180;         // MyMesh.cpp:269 — sizeof(reply_data) - 4
constexpr size_t kNeighbourResults = 130;      // MyMesh.cpp:343 — its results_buffer

// One entry of the repeater's login table, which on stock firmware *is* its ACL.
struct AclEntry {
  std::string key;          // 64 lowercase hex
  uint8_t permissions = 0;  // the low two bits are the role
  uint32_t lastTimestamp = 0;
  // **The way home this repeater holds for that key** (task p1.21-71): `ClientInfo::out_path`
  // and `out_path_len`, where OUT_PATH_UNKNOWN is *none*. Written only by a way back from this
  // key and forgotten by a login of this key that arrived as a flood.
  bool hasOutPath = false;
  std::vector<uint8_t> outPath;
  uint8_t outPathLen = 0;  // the encoded path_len byte: (hashSize - 1) << 6 | hopCount
};

struct NeighbourEntry {
  std::string key;  // 64 lowercase hex; the reply carries the prefix the request asked for
  uint32_t heardSecondsAgo = 0;
  int8_t snrQ4 = 0;
};

// What the mesh was asked to send, with the fake clock's time: the record *one send per command*,
// the minimum gap and the per-minute cap are asserted against.
// A way back this mesh told, with the fake clock's time (task p1.21-71). It is **not** a `Send`
// below: `sends()` is the record of this mesh's *commands*, which is what *one send per command*,
// the minimum gap and the per-minute cap are asserted against, and a way back is none of those -
// it is one packet inside the login's own slot and no transaction at all. `frames()` is the two
// records merged, in order, for a test that wants every packet that went on the air.
struct WayBackTold {
  uint32_t atMs = 0;
  std::vector<uint8_t> path;   // the hops as they went out, which is the way out reversed
  uint8_t hashSize = 1;
  size_t beforeSend = 0;       // the index in `sends()` of the command whose slot this shared
  bool reached = false;        // the packet got to the repeater
  bool stored = false;         // and the repeater held this key, so it is the way home now
};

struct Send {
  enum class Kind : uint8_t { Login, Request, Cli, Discovery, Trace };
  Kind kind = Kind::Login;
  uint32_t atMs = 0;
  bool direct = false;  // else flooded
  uint32_t tag = 0;     // the tag, or for a CLI line the sender timestamp the mesh drew
  uint8_t reqType = 0;  // Request only
  // Cli only: the line **as the repeater ran it**, which is what went out with its reflected
  // prefix taken off again - so `timesSent` and the fault knobs are about the command, as they
  // were before there were prefixes. `prefix` is the three characters the line went out with
  // (`a7|`) and is empty for a line that went bare; the bytes on the air are the two joined.
  std::string text;
  std::string prefix;
  bool sensitive = false;
  // Login only: the login carried a password, rather than the empty material of the ACL login,
  // the guest door or a job the mesh answers locally (task p1.10-41). The **value** is never
  // kept here - what a test asserts is that a login carried one or did not.
  bool withMaterial = false;
  bool answered = false;  // a reply was delivered for this send
  // Which shape a flood went out in (IMesh::setFloodMode, task p1.8-32); false for a direct send
  // and for a mesh whose scope is `*`.
  bool scoped = false;

  const char* kindName() const {
    switch (kind) {
      case Kind::Login: return "login";
      case Kind::Request: return "request";
      case Kind::Cli: return "cli";
      case Kind::Discovery: return "discovery";
      case Kind::Trace: return "trace";
    }
    return "?";
  }
};

class FakeMesh : public mm::IMesh {
 public:
  explicit FakeMesh(FakePlatform& platform) : platform_(platform) {
    stats_ = afterPrefix("status-response");
    telemetry_ = afterPrefix("telemetry-response");
    ownerInfo_ = afterTag("binary-response-owner-info");
    neighbours_ = fixtureNeighbours();
    config_ = defaultConfig();
  }

  // ------------------------------------------------------------------ the repeater, as set up

  // The repeater's own key, which a job addresses; a job begun for any other key reaches nobody.
  void setKey(const std::string& hex64) { key_ = hex64; }
  const std::string& key() const { return key_; }
  // The exit node's key: what the login table is looked up by, and what `setperm` on it changes.
  void setSelfKey(const std::string& hex64) { selfKey_ = hex64; }
  const std::string& selfKey() const { return selfKey_; }

  // answer: cli-password — `NodePrefs` holds `char password[16]` and `StrHelper::strncpy` keeps
  // fifteen bytes of it, so a repeater's password is **never** longer than that and neither is
  // the one a login carries (`BaseChatMesh::sendLogin`). Both ends cut, so both ends cut here.
  void setAdminPassword(const std::string& p) { adminPassword_ = p.substr(0, mm::kMaxPasswordLen); }
  const std::string& adminPassword() const { return adminPassword_; }
  // Empty is a repeater whose owner never set one, which answers a blank login at guest
  // (`NodePrefs()` leaves `guest_password` empty).
  void setGuestPassword(const std::string& p) { guestPassword_ = p.substr(0, mm::kMaxPasswordLen); }
  const std::string& guestPassword() const { return guestPassword_; }

  void grant(const std::string& keyHex, uint8_t permissions) {
    for (AclEntry& e : acl_) {
      if (e.key == keyHex) {
        e.permissions = permissions;
        return;
      }
    }
    AclEntry entry;
    entry.key = keyHex;
    entry.permissions = permissions;
    acl_.push_back(entry);
  }
  const std::vector<AclEntry>& acl() const { return acl_; }
  uint8_t levelOf(const std::string& keyHex) const {
    const AclEntry* e = find(keyHex);
    return e == nullptr ? 0 : static_cast<uint8_t>(e->permissions & mm::kPermRoleMask);
  }

  void setConfig(const std::string& key, const std::string& value) { config_[key] = value; }
  const std::map<std::string, std::string>& config() const { return config_; }
  void setFirmware(const std::string& ver) { firmware_ = ver; }
  void setBoardName(const std::string& board) { board_ = board; }
  void setRegionListing(const std::string& listing) { regionListing_ = listing; }
  // The flood scope this mesh reports (IMesh::floodScope): a region's name, `*`, or the empty
  // string for a mesh that says **nothing** about scope, which answers nullptr as the interface's
  // own default does - and which is this fake's own default, so that every suite written before
  // task p1.8-32 sends the heartbeat it always sent. A test of the scope says `setFloodScope("dk")`
  // and gets what the board's key table falls back to.
  void setFloodScope(const std::string& scope) { floodScope_ = scope; }
  // The path-hash width this mesh floods at (IMesh::floodPathHashSize), two bytes by default.
  void setFloodPathHashSize(uint8_t bytes) { floodPathHashSize_ = bytes; }
  // This mesh answers for the repeater without the air (IMesh::answersLocally, task p1.10-40):
  // the Walter's *local door*, where the radio a job is for is the one wired to the board. The
  // repeater behind the fake answers exactly the same bytes either way - it is a repeater and not
  // a radio - so what a test of the door asserts is the **pacing** and, since task p1.10-41, the
  // **material** the login carries (`loginsWithMaterial`). Those two are the library's whole side
  // of the door. The real door answers a login *at admin whatever it carried*
  // (`LinkMesh::localLogin`); this fake is a stock repeater and answers an empty login at the
  // level its ACL holds this board's key at, so a test that wants a local **admin** session says
  // `grant(self(), kPermAdmin)` and gets the same answer by the stock route. False by default, as
  // the interface is and as this board's mesh is.
  void setAnswersLocally(bool local) { answersLocally_ = local; }
  // The door of **one** repeater, by its key: the Walter's own radio, which is what the real door
  // is keyed to (`LinkMesh::setOwnKey`, and `beginJob` there computes *local* as exactly this
  // comparison). A job for this key is answered off the air and a job for any other is on it,
  // which is what `setAnswersLocally(true)` above is not - that one holds the door open for
  // whatever repeater a job names, which was all tasks p1.10-40 and p1.10-41 needed to see.
  // Setting it is how a test asks the two sides of the door the **same** question: whether a key
  // is served locally (`answersLocallyFor`, task p1.10-43) and whether the job in progress is
  // (`answersLocally`), from one piece of state and one comparison, as the Walter does.
  void setLocalDoorKey(std::string hex64) { doorKey_ = lowerHex(std::move(hex64)); }
  // answer: flood-scope - which shape of flood reaches this repeater at all. A repeater with no
  // region map drops a `dk` flood it would have forwarded unscoped, and a mesh of repeaters
  // running `flood.max.unscoped=0` never carries an unscoped one. Both reach it by default.
  void setFloodsReaching(bool scoped, bool unscoped) {
    scopedReaches_ = scoped;
    unscopedReaches_ = unscoped;
  }
  void setRegionDefault(const std::string& scope) { regionDefault_ = scope; }
  void setNeighbours(std::vector<NeighbourEntry> list) { neighbours_ = std::move(list); }
  void setRepeaterClock(uint32_t epoch) { repeaterNow_ = epoch; }
  uint32_t repeaterClock() const { return repeaterNow_; }
  void setStatusBytes(std::vector<uint8_t> bytes) { stats_ = std::move(bytes); }
  void setTelemetryBytes(std::vector<uint8_t> bytes) { telemetry_ = std::move(bytes); }
  // FIRMWARE_VER_LEVEL, which the login reply carries and which the owner-info request needs 2
  // of (meshmaster.dk's task p4.30-181 gave the fake companion the same knob). A repeater below
  // that level does not know the request **at all**, so this fake answers it with the silence
  // `handleRequest`'s `return 0` is - which is what makes a test of the skip worth writing: the
  // library must not spend a timeout finding out.
  void setFirmwareVerLevel(uint8_t level) { firmwareVerLevel_ = level; }
  // The owner-info reply, for a repeater whose three lines are not this fixture's three: an
  // owner info of two lines, an empty one, a reply with the version alone.
  void setOwnerInfoReply(const std::string& text) { ownerInfo_.assign(text.begin(), text.end()); }
  bool repeaterRebooted() const { return rebooted_; }
  int adverts() const { return adverts_; }
  int floodAdverts() const { return floodAdverts_; }
  int regionsLoaded() const { return loadedRegions_; }

  // The path a flooded reply teaches (the path return). Empty: a flood teaches nothing, which is
  // the repeater that answers and whose reply came home another way.
  void setReturnPath(const std::vector<uint8_t>& hashes, uint8_t hashSize) {
    returnPath_ = hashes;
    returnPathSize_ = hashSize;
  }

  // A line the test answers itself, whatever the firmware would have said; and a line the
  // repeater is silent to, whatever it is.
  void answerLine(const std::string& line, const std::string& reply) { scripted_[line] = reply; }
  // A line this repeater never answers: it is a **silence on the air**, which with a prefix on
  // the line is no longer the same as a handler that writes an empty reply (task p1.10-49).
  void silenceLine(const std::string& line) { silentLines_.insert(line); }

  // ------------------------------------------------------------------ the faults, on cue

  void beSilent(int sends) { silentSends_ = sends; }  // the next n sends get no reply
  void beSilentAlways() { silentAlways_ = true; }     // off the air
  void answerAgain() {
    silentSends_ = 0;
    silentAlways_ = false;
  }
  void replyWithTheWrongTag(int sends) { wrongTagSends_ = sends; }
  void replySlowly(int sends) { slowSends_ = sends; }  // the reply arrives past the wait
  void forgetTheSession() { forgotten_ = true; }       // the login table lost us
  // **The next n logins are answered with a response that is not a login reply.** Stock firmware
  // sends no such thing to a login - `handleLoginReq` answers RESP_SERVER_LOGIN_OK or nothing -
  // so what this stands for is the reply to something *else* arriving while a login is awaited,
  // which is the whole of task p1.10-47. Consumed before `beSilentToTheNextLogins` below, so a
  // test can script "login one answers this, and the rest are silent".
  void answerTheNextLoginWithSomethingElse(int logins) { somethingElseLogins_ = logins; }
  // The next n logins get no reply at all, whatever this repeater's ACL and passwords say: the
  // mesh ate the reply. `beSilent` above cannot say it, because it counts every send.
  void beSilentToTheNextLogins(int logins) { silentLogins_ = logins; }
  // **The next n replies arrive after the wait for their own request has ended** (task
  // p1.10-47): the reply is not lost, as `replySlowly` loses it, but held and delivered during
  // the *next* wait of the job - which is what a repeater slower than this board's patience
  // really does, and what parked ten ACL associations on meshmaster.dk on 12 November 2026. A
  // late reply survives the sends that follow it, so the login after a timed-out request finds
  // it waiting.
  void deliverTheNextReplyLate(int sends) { lateSends_ = sends; }
  // **The next `times` sends of this line never reach the repeater**: not run, not answered -
  // the line lost on the way out. `silenceLine` above is a line the repeater has and will not
  // answer, and `beSilent` counts every send, which a line checked by a login between two lines
  // cannot use (task p1.10-48).
  void loseTheNextLines(const std::string& line, int times) { lostLines_[line] = times; }
  // **The next `times` sends of this line run and their replies never come home**: the lost "OK"
  // of a line that cannot be asked twice, which is 20 of meshmaster.dk's 52 failed grants of 12
  // November 2026. Not `silenceLine` (the line never ran) and not `replySlowly` (which counts
  // every reply, a login's included).
  void loseTheNextLineReplies(const std::string& line, int times) { lostLineReplies_[line] = times; }
  // **The neighbours page at `page` and every page after it goes unanswered** (task p1.15-56):
  // the list stops where a page was lost, which is what a repeater with more neighbours than one
  // page does on a mesh that drops one. 1 is the first page. `beSilent` above cannot say it,
  // because it counts every send and the login and the pages before this one were answered.
  void silenceTheNeighboursPageFrom(int page) { silentNeighboursPage_ = page; }
  // **The neighbours page at `page` comes home too short to be a page**: fewer than the four
  // bytes of `total(i16)` and `returned(i16)`, which is what `parseNeighboursPage` refuses. A
  // reply is only ever that short with the padding off (`setPadding`), which is how a test that
  // wants a page the reader cannot make sense of asks for one.
  void truncateTheNeighboursPage(int page) { shortNeighboursPage_ = page; }
  // **What this repeater says its neighbour count is, from `page` on**: every page carries the
  // count, and a repeater that heard a new neighbour between two pages says a larger one in the
  // second. Without this the count is the length of the list the test set.
  void sayTheNeighbourTotalFromPage(int page, int total) { pageTotals_[page] = total; }
  // **The next `times` replies to this line arrive after their own wait has ended** (task
  // p1.10-49): held, not lost, and delivered `waits` waits later - 1 is the wait after its own,
  // and 2 is the one after that, which for a line the library **retries** is the next line's
  // wait, where the reply to `get name` can be stored as the radio. The line is named as the
  // repeater **ran** it, without the prefix the line went out with.
  void deliverTheNextLineRepliesLate(const std::string& line, int times, int waits = 1) {
    lateLineReplies_[line] = {times, waits};
  }

  // **The next `n` ways back never reach the repeater**: the packet lost on the air, so no
  // out_path is written and the replies that follow are flooded as they were before this task.
  void loseTheNextWaysBack(int n) { lostWaysBack_ = n; }
  // **The way back is dead**: the repeater stores it and every reply it then sends along it is
  // lost. The link is good out and poor back, which is the one thing this task makes less
  // forgiving - a flood might have got through where this does not.
  void theWayBackIsDead(bool dead = true) { wayBackDead_ = dead; }
  // How long this mesh tells the library a way back needs to clear **each** hop. On the board it
  // is arithmetic over the radio's own figures (`AgentMesh::clearMsFor`: the packet's airtime,
  // and the longest a hop may hold it before it retransmits it); this fake has no radio, so it is
  // a figure like its latencies above.
  void setWayBackClearMs(uint32_t perHopMs) { wayBackClearMs_ = perHopMs; }
  // **This mesh tells no way back at all**: `tellTheWayBack` answers exactly what the default on
  // `mm::IMesh` answers, which is what a host's mesh and the repeater-v2 unit's Walter are. The
  // mesh every test but one runs against *does* tell one, because the board does.
  void tellsNoWayBack(bool none = true) { tellsNoWayBack_ = none; }

  void setLatency(uint32_t directMs, uint32_t floodMs) {
    directLatencyMs_ = directMs;
    floodLatencyMs_ = floodMs;
  }
  // The zero padding a reply arrives with: on by default, as the air does it.
  void setPadding(bool on) { pad_ = on; }

  // ------------------------------------------------------------------ what a test reads back

  const std::vector<Send>& sends() const { return sends_; }
  int sendCount() const { return static_cast<int>(sends_.size()); }
  // The ways back this mesh told (task p1.21-71), and every frame that went on the air in order:
  // the commands of `sends()` with the ways back among them, by their kind names.
  const std::vector<WayBackTold>& waysBack() const { return waysBack_; }
  int waysBackCount() const { return static_cast<int>(waysBack_.size()); }
  std::vector<std::string> frames() const { return frames_; }
  int timesSent(const std::string& line) const {
    int n = 0;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Cli && s.text == line) n++;
    }
    return n;
  }
  int logins() const { return count(Send::Kind::Login); }
  // How many of those logins carried a password at all (task p1.10-41): a job the mesh answers
  // locally must log in with none, at every class, and this is how a test sees that the claim's
  // password stayed where it was.
  int loginsWithMaterial() const {
    int n = 0;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Login && s.withMaterial) n++;
    }
    return n;
  }
  // The shape of every flood that went out, in order: true scoped, false unscoped. A direct send
  // contributes nothing.
  std::vector<bool> floodShapes() const {
    std::vector<bool> out;
    for (const Send& s : sends_) {
      if (!s.direct) out.push_back(s.scoped);
    }
    return out;
  }
  int requests() const { return count(Send::Kind::Request); }
  int cliLines() const { return count(Send::Kind::Cli); }
  int requestsOf(uint8_t reqType) const {
    int n = 0;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Request && s.reqType == reqType) n++;
    }
    return n;
  }
  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Cli) out.push_back(s.text);
    }
    return out;
  }
  // The prefix every console line went out with, in order (task p1.10-49): `a7|`, or the empty
  // string for a line that went bare. One entry per CLI send, so it lines up with `lines()`.
  std::vector<std::string> linePrefixes() const {
    std::vector<std::string> out;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Cli) out.push_back(s.prefix);
    }
    return out;
  }
  // The prefix a named line went out with on its nth send (0-based), or "" if there was none.
  std::string prefixOf(const std::string& line, int nth = 0) const {
    for (const Send& s : sends_) {
      if (s.kind != Send::Kind::Cli || s.text != line) continue;
      if (nth-- == 0) return s.prefix;
    }
    return std::string();
  }
  // The sender timestamps of the CLI lines, in the order they went: the series the repeater's
  // replay check reads, which must be strictly increasing.
  std::vector<uint32_t> cliTimestamps() const {
    std::vector<uint32_t> out;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Cli) out.push_back(s.tag);
    }
    return out;
  }
  // **When each transaction started**, in order. A way back is not a transaction of its own
  // (task p1.21-71): it shares the slot of the command it precedes, so a command that one went
  // before started when the way back did and the command's own time is not a start.
  std::vector<uint32_t> transactionStartsMs() const {
    std::vector<uint32_t> starts;
    for (size_t i = 0; i < sends_.size(); i++) {
      uint32_t at = sends_[i].atMs;
      for (const WayBackTold& w : waysBack_) {
        if (w.beforeSend == i && w.atMs < at) at = w.atMs;
      }
      starts.push_back(at);
    }
    return starts;
  }
  // The gap in milliseconds between the starts of two transactions, in order.
  std::vector<uint32_t> gapsMs() const {
    const std::vector<uint32_t> starts = transactionStartsMs();
    std::vector<uint32_t> gaps;
    for (size_t i = 1; i < starts.size(); i++) gaps.push_back(starts[i] - starts[i - 1]);
    return gaps;
  }
  // The most transactions that started inside any sixty seconds of the record.
  int mostInAnyMinute() const {
    const std::vector<uint32_t> starts = transactionStartsMs();
    int worst = 0;
    for (size_t i = 0; i < starts.size(); i++) {
      int n = 0;
      for (size_t j = i; j < starts.size(); j++) {
        if (starts[j] - starts[i] < 60000u) n++;
      }
      if (n > worst) worst = n;
    }
    return worst;
  }
  void forgetSends() {
    sends_.clear();
    waysBack_.clear();
    frames_.clear();
  }

  // ------------------------------------------------------------------ IMesh

  const char* floodScope() const override { return floodScope_.empty() ? nullptr : floodScope_.c_str(); }
  void setFloodMode(mm::FloodMode mode) override { nextMode_ = mode; }
  // The width this mesh's floods originate at (IMesh::floodPathHashSize, task p1.10-39): two
  // bytes, as the Heltec's `AgentMesh::kFloodPathHashSize` is and as the rail asks. A test that
  // wants to see the library *ask* rather than assume says `setFloodPathHashSize(4)`.
  uint8_t floodPathHashSize() const override { return floodPathHashSize_; }
  // Whether the job **in progress** is served off the air (IMesh::answersLocally, task p1.10-40):
  // the flat flag, or a job begun for the door's own key - and then only while that job is begun,
  // which is the interface's contract (true from `beginJob` to `endJob`, false between jobs).
  bool answersLocally() const override { return answersLocally_ || (peerSet_ && servesLocally(peer_)); }
  // Whether a job **for this key** would be (IMesh::answersLocallyFor, task p1.10-43): the same
  // flag and the same comparison, with no job needed. The two agree by construction here, as they
  // must on the real door.
  bool answersLocallyFor(const char* publicKeyHex) const override {
    if (answersLocally_) return true;
    if (publicKeyHex == nullptr) return false;
    return servesLocally(lowerHex(publicKeyHex));
  }

  void setRing(mm::ObservationRing* ring) override { ring_ = ring; }
  mm::ObservationRing* ring() const { return ring_; }
  const mm::MeshCounters& counters() const override { return counters_; }
  uint32_t overheard() const override { return overheard_; }
  void setOverheard(uint32_t n) { overheard_ = n; }
  void setAdvertsDropped(uint32_t n) { counters_.advertsDropped = n; }
  // What this mesh cannot do (IMesh::supports, task p1.6-23): a type named here is answered
  // false, so the loop does not advertise it and the runner answers a job of it `unsupported`
  // with nothing sent. The Walter's UART link cannot carry a trace; this is that, on the host.
  // Nothing named is the default, which is the mesh every existing test runs against.
  void cannotServe(const std::string& jobType) { unserved_.insert(jobType); }
  bool supports(const char* jobType) const override { return unserved_.count(jobType) == 0; }
  // Nothing to pump: a host has no radio queue. The count is how a test says the library pumped.
  void loop() override { pumps_++; }
  uint32_t pumps() const { return pumps_; }

  bool beginJob(const char* publicKeyHex, const uint8_t* pathHashes, uint8_t hopCount, uint8_t hashSize) override {
    endJob();
    if (publicKeyHex == nullptr) return false;
    // AgentMesh's own rule: 64 hex characters, or there is no identity to address.
    for (size_t i = 0; i < 64; i++) {
      if (hexDigit(publicKeyHex[i]) < 0) return false;
    }
    peer_.assign(publicKeyHex, 64);
    for (char& c : peer_) {
      if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    peerSet_ = true;
    if (pathHashes != nullptr && hopCount > 0 && hashSize >= 1 && hashSize <= 3 &&
        static_cast<size_t>(hopCount) * hashSize <= mm::kMeshPathCap && hopCount <= 63) {
      outPath_.assign(pathHashes, pathHashes + static_cast<size_t>(hopCount) * hashSize);
      outPathLen_ = static_cast<uint8_t>(((hashSize - 1) << 6) | hopCount);
      havePath_ = true;
    }
    return true;
  }

  void endJob() override {
    peerSet_ = false;
    havePath_ = false;
    outPathLen_ = 0;
    outPath_.clear();
    pending_ = Pending();
    // No job in flight: a reply still on its way home belongs to a job that is over, and this
    // mesh releases a response from anybody between jobs (IMesh::endJob).
    late_.clear();
    reply_ = mm::MeshReply();
  }

  void setDirectPath() override {
    if (!peerSet_) return;
    outPath_.clear();
    outPathLen_ = 0;
    havePath_ = true;
  }
  void forgetPath() override {
    havePath_ = false;
    outPathLen_ = 0;
    outPath_.clear();
  }
  bool hasPath() const override { return havePath_; }
  uint8_t pathHopCount() const override { return havePath_ ? static_cast<uint8_t>(outPathLen_ & 63) : 0; }
  int describePath(char hops[][8], int max) const override {
    if (!havePath_) return 0;
    const int size = (outPathLen_ >> 6) + 1;
    int n = outPathLen_ & 63;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
      mm::toHexLower(&outPath_[static_cast<size_t>(i) * size], static_cast<size_t>(size), hops[i]);
    }
    return n;
  }

  // **The way back** (task p1.21-71), both halves of it: what this mesh sends, and what the
  // repeater behind it does with the packet.
  //
  // The exit node's half: the hops of the path this job holds, in the opposite order, at the
  // width the path carried - the empty path for a pair of no hops - and nothing at all when
  // there is no path to tell one along. It is told through no `Send`: `sends()` is the record of
  // commands and a way back is not one.
  //
  // The repeater's half, by citation: the packet is dropped unless the access list holds this
  // key (answer: way-back-dropped), and stored as that key's `out_path` when it does
  // (answer: way-back-stored).
  WayBack tellTheWayBack() override {
    WayBack out;
    if (tellsNoWayBack_) return out;  // the default member, and nothing goes on the air
    if (!peerSet_ || !havePath_) return out;
    const uint8_t size = static_cast<uint8_t>((outPathLen_ >> 6) + 1);
    const uint8_t hops = static_cast<uint8_t>(outPathLen_ & 63);
    WayBackTold told;
    told.atMs = platform_.millis();
    told.hashSize = size;
    told.beforeSend = sends_.size();
    for (uint8_t i = 0; i < hops; i++) {
      for (uint8_t b = 0; b < size; b++) {
        told.path.push_back(outPath_[static_cast<size_t>(hops - 1 - i) * size + b]);
      }
    }
    out.hops = hops;
    out.clearMs = static_cast<uint32_t>(hops) * wayBackClearMs_;
    out.how = WayBack::How::Told;
    counters_.tx++;
    frames_.push_back("wayback");
    if (lostWaysBack_ > 0) {
      lostWaysBack_--;  // the packet lost on the air: no out_path is written
    } else if (addressed()) {
      told.reached = true;
      AclEntry* client = find(selfKey_);
      if (client != nullptr && !forgotten_) {
        // answer: way-back-stored
        told.stored = true;
        client->hasOutPath = true;
        client->outPath = told.path;
        client->outPathLen = outPathLen_;
      }
      // answer: way-back-dropped - no entry for this key, so nothing was decrypted
    }
    waysBack_.push_back(told);
    return out;
  }

  mm::SendResult sendLogin(const char* password, uint32_t& tag) override {
    tag = 0;  // a login reply carries the repeater's own timestamp and no tag of ours
    if (!peerSet_) return mm::SendResult::Failed;
    const uint32_t senderTimestamp = unique();
    const bool direct = note(Send::Kind::Login, senderTimestamp);
    const std::string offered =
        password == nullptr ? std::string() : std::string(password).substr(0, mm::kMaxPasswordLen);
    sends_.back().withMaterial = !offered.empty();
    const mm::SendResult how = direct ? mm::SendResult::Direct : mm::SendResult::Flood;
    if (!reachable()) return how;
    // answer: way-back-forgotten - a login that arrived as a flood clears the way home, whatever
    // the login goes on to answer, so the reply to it and every reply after it is flooded until
    // another way back is told.
    if (!direct) {
      AclEntry* known = find(selfKey_);
      if (known != nullptr) {
        known->hasOutPath = false;
        known->outPath.clear();
        known->outPathLen = 0;
      }
    }

    if (somethingElseLogins_ > 0) {
      somethingElseLogins_--;
      // answer: login-another-reply — a response that is not a login-ok at all. Stock firmware
      // never sends one to a login (a wrong password is silence), so what arrives on the air in
      // this shape is the reply to something else; the runner drops it and waits on (task
      // p1.10-47), where it used to read it as `Refused`.
      arm(response({0x7F, 0x01}, senderTimestamp, /*repeaterStamps=*/true));
      return how;
    }
    if (silentLogins_ > 0) {
      silentLogins_--;
      return how;  // the mesh ate the login's reply: silence, which is all a login can be refused with
    }

    AclEntry* client = nullptr;
    if (offered.empty()) {
      // answer: login-acl-entry
      client = find(selfKey_);
    }
    if (client == nullptr) {
      uint8_t perms = 0;
      if (offered == adminPassword_) {
        // answer: login-password
        perms = mm::kPermAdmin;
      } else if (offered == guestPassword_) {
        perms = mm::kPermGuest;
      } else {
        // answer: login-silent-password
        return how;
      }
      grant(selfKey_, levelOrZero(selfKey_));
      client = find(selfKey_);
      if (senderTimestamp <= client->lastTimestamp) {
        // answer: login-silent-replay
        return how;
      }
      client->lastTimestamp = senderTimestamp;
      client->permissions = static_cast<uint8_t>((client->permissions & ~mm::kPermRoleMask) | perms);
    }
    forgotten_ = false;

    // answer: login-ok
    std::vector<uint8_t> body;
    body.push_back(kRespServerLoginOk);
    body.push_back(0);  // the legacy keep-alive field
    body.push_back((client->permissions & mm::kPermRoleMask) == mm::kPermAdmin ? 1 : 0);
    body.push_back(client->permissions);
    for (int i = 0; i < 4; i++) body.push_back(0xAB);  // the random blob that makes the hash unique
    body.push_back(firmwareVerLevel_);
    arm(response(body, senderTimestamp, /*repeaterStamps=*/true));
    return how;
  }

  mm::SendResult sendRequest(uint8_t reqType, const uint8_t* data, uint8_t len, uint32_t& tag) override {
    if (!peerSet_) return mm::SendResult::Failed;
    tag = unique();
    const bool direct = note(Send::Kind::Request, tag);
    sends_.back().reqType = reqType;
    const mm::SendResult how = direct ? mm::SendResult::Direct : mm::SendResult::Flood;
    if (!reachable()) return how;
    if (!knownSender()) {
      // answer: forgotten-session
      return how;
    }
    AclEntry* client = find(selfKey_);
    if (tag <= client->lastTimestamp) {
      // answer: request-silent-replay
      return how;
    }
    const std::vector<uint8_t> body = requestReply(reqType, data, len, *client);
    if (body.empty()) {
      // answer: request-silent
      return how;
    }
    client->lastTimestamp = tag;
    arm(response(body, tag, /*repeaterStamps=*/false));
    return how;
  }

  mm::SendResult sendCli(const char* text, bool sensitive, uint32_t& timestamp) override {
    timestamp = 0;
    if (!peerSet_ || text == nullptr || text[0] == '\0') return mm::SendResult::Failed;
    timestamp = unique();
    const bool direct = note(Send::Kind::Cli, timestamp);
    // answer: cli-prefix — the optional prefix, in handleCommand's own order: it is read
    // **after** the region-load branch has returned, so a line sent while the repeater is
    // loading regions keeps its prefix and is read as a region, and the condition is on the
    // whole line's length and the third character.
    std::string wire = text;
    std::string prefix;
    if (!regionLoadActive_ && wire.size() > 4 && wire[2] == '|') {
      prefix = wire.substr(0, mm::kCliPrefixLen);
      wire.erase(0, mm::kCliPrefixLen);
    }
    sends_.back().text = wire;
    sends_.back().prefix = prefix;
    sends_.back().sensitive = sensitive;
    const mm::SendResult how = direct ? mm::SendResult::Direct : mm::SendResult::Flood;
    if (!reachable()) return how;
    if (consume(lostLines_, wire)) return how;  // the line never arrived: nothing ran
    if (!knownSender()) {
      // answer: forgotten-session
      return how;
    }
    AclEntry* client = find(selfKey_);
    if ((client->permissions & mm::kPermRoleMask) != mm::kPermAdmin) {
      // answer: cli-not-admin
      return how;
    }
    if (timestamp <= client->lastTimestamp) {
      // answer: cli-retry — equal is a retry (the mark is rewritten to the same value, nothing is
      // run and the empty reply is not sent) and below it is a replay, dropped
      return how;
    }
    client->lastTimestamp = timestamp;
    // A line the test says this repeater is silent to: it ran, or did not, and **nothing comes
    // home** - which is not the same as a handler that writes an empty reply, because the
    // reflected prefix alone would make that one a packet.
    if (silentLines_.count(wire) > 0) return how;
    const bool wasRebooted = rebooted_;
    const std::string answer = cliReply(wire);
    // answer: cli-reboot — the board is gone before `strlen(reply)` is reached, so a rebooting
    // line is answered with nothing whether it carried a prefix or not.
    if (rebooted_ && !wasRebooted) return how;
    // answer: cli-setperm-zero — the "OK" was addressed to the slot our entry used to hold, so
    // it reaches another client and never us, prefix or not.
    if (replyWentElsewhere_) {
      replyWentElsewhere_ = false;
      return how;
    }
    // answer: cli-prefix — the reflected prefix is written into the reply buffer before the
    // command runs, so a command that answers nothing is still a reply of three characters and
    // `if (text_len > 0)` sends it. That is why no caller prefixes a line it awaits nothing for.
    if (answer.empty() && prefix.empty()) {
      // answer: cli-reply — an empty reply is not sent at all
      return how;
    }
    // The line **ran** and its reply is lost on the way home (task p1.10-48), which for a
    // `setperm` on our own key is the fault the whole task is about.
    if (consume(lostLineReplies_, wire)) return how;
    // **This line's reply misses its own wait** (task p1.10-49): held and delivered in the next
    // one, which for a console line is the wait of the *next* line - where only its prefix can
    // tell it from that line's answer. The same mechanism `deliverTheNextReplyLate` uses.
    const auto late = lateLineReplies_.find(wire);
    if (late != lateLineReplies_.end() && late->second.first > 0) {
      late->second.first--;
      lateSends_++;
      lateWaits_ = late->second.second;
    }
    arm(asText(prefix + answer));
    return how;
  }

  mm::SendResult sendDiscovery(uint32_t& tag) override {
    if (!peerSet_) return mm::SendResult::Failed;
    tag = unique();
    note(Send::Kind::Discovery, tag, /*alwaysFlood=*/true);
    if (!reachable()) return mm::SendResult::Flood;
    // answer: discovery — the flooded base-telemetry request, answered as a telemetry read is
    // and with the path return a flood earns. Some firmware never answers one at all, which is
    // what `beSilent` is for.
    arm(response(telemetry_, tag, /*repeaterStamps=*/false));
    return mm::SendResult::Flood;
  }

  mm::SendResult sendTrace(const uint8_t* roundTripHashes, uint8_t hashBytes, uint8_t hashSize,
                           uint32_t& tag) override {
    tag = 0;
    if (roundTripHashes == nullptr || hashBytes == 0) return mm::SendResult::Failed;
    if (!mm::isTraceableHashSize(hashSize)) return mm::SendResult::Failed;
    tag = unique();
    note(Send::Kind::Trace, tag, /*alwaysFlood=*/false, /*alwaysDirect=*/true);
    if (!reachable()) return mm::SendResult::Direct;
    // answer: trace — the round trip comes back as it went, with one SNR a hop and the final one
    // measured on the packet home. The series is trace-data.hex's.
    Pending p;
    p.reply.kind = mm::MeshReply::Kind::Trace;
    p.reply.tag = tag;
    p.reply.traceFlags = mm::traceFlagsFor(hashSize);
    p.reply.traceHashBytes = hashBytes;
    std::memcpy(p.reply.traceHashes, roundTripHashes, hashBytes);
    const uint8_t hops = static_cast<uint8_t>(hashBytes / hashSize);
    p.reply.traceSnrCount = hops;
    for (uint8_t i = 0; i < hops; i++) {
      p.reply.traceSnrs[i] = static_cast<uint8_t>(traceSnrsQ4_[i % traceSnrsQ4_.size()]);
    }
    p.reply.traceFinalSnr = traceFinalSnr_;
    p.has = true;
    arm(std::move(p));
    return mm::SendResult::Direct;
  }

  bool awaitReply(uint32_t tag, uint32_t timeoutMs, void (*pump)()) override {
    return await(Awaiting::Response, tag, timeoutMs, pump);
  }
  bool awaitText(uint32_t timeoutMs, void (*pump)()) override { return await(Awaiting::Text, 0, timeoutMs, pump); }
  const mm::MeshReply& reply() const override { return reply_; }

 private:
  enum class Awaiting : uint8_t { None, Response, Text };

  struct Pending {
    mm::MeshReply reply;
    bool has = false;
    uint32_t deliverAtMs = 0;
    bool neverArrives = false;
    bool teachesPath = false;  // it comes home as a returned path, which teaches the way out
    size_t sendIndex = 0;      // which send this answers, for `Send::answered`
    // How many more waits this reply is not delivered in (`deliverTheNextReplyLate`): 1 is its
    // own request's wait, so the one after it is where it turns up.
    int waitsToSkip = 0;
  };

  // ---------------------------------------------------------------- the clocks and the record

  // getCurrentTimeUnique(): the time, or one above the last one handed out, so the series a
  // repeater sees from this key is strictly increasing across REQs and CLI lines alike
  // (src/MeshCore.h:119-125 at 81dbae59). This is the **exit node's** series.
  uint32_t unique() {
    const uint32_t now = platform_.clock().hasTime() ? platform_.clock().now() : platform_.millis() / 1000u;
    if (now <= lastUnique_) return ++lastUnique_;
    return lastUnique_ = now;
  }
  // The same function on the repeater's own clock: what it stamps a reply with.
  uint32_t repeaterUnique() {
    if (repeaterNow_ <= lastRepeaterUnique_) return ++lastRepeaterUnique_;
    return lastRepeaterUnique_ = repeaterNow_;
  }

  bool note(Send::Kind kind, uint32_t tag, bool alwaysFlood = false, bool alwaysDirect = false) {
    Send s;
    s.kind = kind;
    s.atMs = platform_.millis();
    s.direct = alwaysDirect || (!alwaysFlood && havePath_);
    // What the mesh was last told, as this mesh's own scope allows it: `*` and "nothing at all"
    // have no scope to carry, so every flood of theirs is unscoped.
    s.scoped = !s.direct && floodScope_ != "*" && !floodScope_.empty() && nextMode_ == mm::FloodMode::Scoped;
    s.tag = tag;
    sends_.push_back(s);
    frames_.push_back(s.kindName());
    counters_.tx++;
    return s.direct;
  }

  bool reachable() {
    if (silentAlways_) return false;
    if (silentSends_ > 0) {
      silentSends_--;
      return false;
    }
    if (!floodReaches()) return false;
    return addressed();
  }
  // answer: flood-scope - a flood whose scope this repeater's region map does not hold with
  // flood allowed is never seen by it, and neither is an unscoped one on a mesh that stopped
  // forwarding those. From here that is a silence, exactly as being off the air is.
  bool floodReaches() const {
    if (sends_.empty() || sends_.back().direct) return true;  // direct routing is not scoped
    return sends_.back().scoped ? scopedReaches_ : unscopedReaches_;
  }
  // A job begun for another key reaches nobody: there is one repeater on this mesh.
  bool addressed() const { return peerSet_ && peer_ == key_; }

  // The sender the repeater knows: an entry in the login table, unless it has forgotten us.
  bool knownSender() { return !forgotten_ && find(selfKey_) != nullptr; }

  const AclEntry* find(const std::string& keyHex) const {
    for (const AclEntry& e : acl_) {
      if (e.key == keyHex) return &e;
    }
    return nullptr;
  }
  AclEntry* find(const std::string& keyHex) {
    for (AclEntry& e : acl_) {
      if (e.key == keyHex) return &e;
    }
    return nullptr;
  }
  // Which slot of the table a key sits in, or -1: what `ClientACL::getClient` answers as a
  // pointer, and what the deletion of a level-0 setperm has to move the later entries into.
  int indexOf(const std::string& keyHex) const {
    for (size_t i = 0; i < acl_.size(); i++) {
      if (acl_[i].key == keyHex) return static_cast<int>(i);
    }
    return -1;
  }
  // One of the counted, line-keyed faults above: true when this line is to be lost this time.
  static bool consume(std::map<std::string, int>& faults, const std::string& line) {
    const auto it = faults.find(line);
    if (it == faults.end() || it->second <= 0) return false;
    it->second--;
    return true;
  }
  uint8_t levelOrZero(const std::string& keyHex) const {
    const AclEntry* e = find(keyHex);
    return e == nullptr ? 0 : e->permissions;
  }

  // ---------------------------------------------------------------- the replies

  // A RESPONSE: the four-byte tag and then the repeater's own bytes, zero-padded to the cipher
  // block. `repeaterStamps` is a login, whose first four bytes are the repeater's own clock and
  // not the tag we sent (which is why a login reply is awaited with tag 0).
  Pending response(const std::vector<uint8_t>& body, uint32_t tag, bool repeaterStamps) {
    Pending p;
    std::vector<uint8_t> plaintext;
    uint32_t stamped = repeaterStamps ? repeaterUnique() : tag;
    if (wrongTagSends_ > 0) {
      wrongTagSends_--;
      stamped = tag + 0x1000u;  // a reply of another exchange, home by another route
    }
    for (int i = 0; i < 4; i++) plaintext.push_back(static_cast<uint8_t>((stamped >> (8 * i)) & 0xFF));
    plaintext.insert(plaintext.end(), body.begin(), body.end());
    // answer: datagram-too-long
    if (plaintext.size() > kDatagramPlaintextMax) return p;
    pad(plaintext);
    p.reply.kind = mm::MeshReply::Kind::Response;
    p.reply.tag = stamped;
    size_t n = plaintext.size() - 4;
    if (n > mm::kMeshReplyDataCap) n = mm::kMeshReplyDataCap;
    std::memcpy(p.reply.data, plaintext.data() + 4, n);
    p.reply.len = static_cast<uint8_t>(n);
    p.has = true;
    return p;
  }

  // A TXT_MSG of the repeater's own: its timestamp, TXT_TYPE_CLI_DATA in the flags byte, the
  // text, padded.
  Pending asText(const std::string& answer) {
    Pending p;
    std::vector<uint8_t> plaintext;
    const uint32_t stamp = repeaterUnique();
    for (int i = 0; i < 4; i++) plaintext.push_back(static_cast<uint8_t>((stamp >> (8 * i)) & 0xFF));
    plaintext.push_back(static_cast<uint8_t>(mm::kTxtTypeCliData << 2));
    for (char c : answer) plaintext.push_back(static_cast<uint8_t>(c));
    // answer: datagram-too-long
    if (plaintext.size() > kDatagramPlaintextMax) return p;
    pad(plaintext);
    char parsed[mm::kMaxCliReplyLen + 1] = {0};
    if (!mm::parseCliReply(plaintext.data(), plaintext.size(), parsed, sizeof parsed)) return p;
    p.reply.kind = mm::MeshReply::Kind::Text;
    std::memcpy(p.reply.text, parsed, std::strlen(parsed) + 1);
    p.has = true;
    return p;
  }

  void pad(std::vector<uint8_t>& plaintext) const {
    // answer: reply-padding
    if (!pad_) return;
    while (plaintext.size() % kCipherBlock != 0) plaintext.push_back(0);
  }

  // **Which way this repeater sends a reply** (answer: reply-direct): a reply to a request that
  // arrived as a **flood** is a path return, flooded back; a reply to a direct request goes along
  // the `out_path` this repeater holds for our key and is flooded when it holds none. A trace is
  // neither - it travels the round trip in its own payload and comes home direct.
  mm::ReplyRoute replyRouteNow(const mm::MeshReply& reply) const {
    if (reply.kind == mm::MeshReply::Kind::Trace) return mm::ReplyRoute::Direct;
    if (!sends_.empty() && !sends_.back().direct) return mm::ReplyRoute::Flood;
    const AclEntry* client = find(selfKey_);
    // answer: reply-direct
    return client != nullptr && client->hasOutPath ? mm::ReplyRoute::Direct : mm::ReplyRoute::Flood;
  }

  void arm(Pending p) {
    if (!p.has) {
      pending_ = Pending();
      return;
    }
    p.reply.route = replyRouteNow(p.reply);
    // A way back the test has declared dead: the repeater sends the reply along it and it never
    // arrives, which from here is the same silence being off the air is.
    if (wayBackDead_ && p.reply.route == mm::ReplyRoute::Direct && p.reply.kind != mm::MeshReply::Kind::Trace) {
      pending_ = Pending();
      return;
    }
    const bool flood = !sends_.empty() && !sends_.back().direct;
    p.deliverAtMs = platform_.millis() + (flood ? floodLatencyMs_ : directLatencyMs_);
    p.sendIndex = sends_.empty() ? 0 : sends_.size() - 1;
    if (slowSends_ > 0) {
      slowSends_--;
      p.neverArrives = true;  // past every wait, which from here is a silence
    }
    // A flooded request is answered with a path return, which teaches the way home.
    p.teachesPath = flood && !returnPath_.empty();
    if (lateSends_ > 0) {
      lateSends_--;
      // Late, not lost (task p1.10-47): it misses its own request's wait and waits for the next
      // one, in a list of its own so that the sends between cannot evict it the way one send's
      // reply evicts the last.
      p.waitsToSkip = lateWaits_;
      lateWaits_ = 1;
      late_.push_back(p);
      pending_ = Pending();
      return;
    }
    pending_ = p;
  }

  // The REQ answers, by type, in handleRequest's own order.
  std::vector<uint8_t> requestReply(uint8_t reqType, const uint8_t* data, uint8_t len, const AclEntry& client) {
    if (reqType == mm::kReqTypeGetStatus) {
      // answer: status
      return stats_;
    }
    if (reqType == mm::kReqTypeGetTelemetry) {
      // answer: telemetry
      return telemetry_;
    }
    if (reqType == mm::kReqTypeGetAccessList) {
      // answer: access-list
      if ((client.permissions & mm::kPermRoleMask) != mm::kPermAdmin) return {};
      if (len < 2 || data == nullptr || data[0] != 0 || data[1] != 0) return {};
      std::vector<uint8_t> body;
      for (const AclEntry& e : acl_) {
        if (e.permissions == 0) continue;  // the firmware skips a deleted entry
        if (4 + body.size() + 7 > kReplyDataRoom) break;  // its own loop bound
        for (int i = 0; i < mm::kAccessListPrefixBytes; i++) {
          body.push_back(static_cast<uint8_t>((hexDigit(e.key[i * 2]) << 4) | hexDigit(e.key[i * 2 + 1])));
        }
        body.push_back(e.permissions);
      }
      return body;
    }
    if (reqType == mm::kReqTypeGetNeighbours) {
      // answer: neighbours
      if (len < 6 || data == nullptr || data[0] != 0) return {};
      const int want = data[1];
      const int offset = data[2] | (data[3] << 8);
      // Which page this is, counting from one: the runner asks `want` at a time and moves the
      // offset on by what came back, so the offset says it. The three faults above are keyed to
      // it, and a page asked for a second time is the same page (task p1.15-56).
      const int page = want > 0 ? offset / want + 1 : 1;
      if (silentNeighboursPage_ > 0 && page >= silentNeighboursPage_) return {};
      if (shortNeighboursPage_ == page) return {0, 0, 0};
      int prefixLen = data[5];
      if (prefixLen > static_cast<int>(mm::kPublicKeyLen)) prefixLen = static_cast<int>(mm::kPublicKeyLen);
      std::vector<uint8_t> entries;
      int returned = 0;
      // What the page **says** the repeater has, and what it can hand over, are two numbers: a
      // scripted count is the repeater's own figure and never an index into this list.
      const int held = static_cast<int>(neighbours_.size());
      int total = held;
      for (const auto& said : pageTotals_) {
        if (page >= said.first) total = said.second;
      }
      for (int i = 0; i < want && i + offset < held; i++) {
        if (entries.size() + static_cast<size_t>(prefixLen) + 5 > kNeighbourResults) break;
        const NeighbourEntry& n = neighbours_[static_cast<size_t>(i + offset)];
        for (int b = 0; b < prefixLen; b++) {
          entries.push_back(static_cast<uint8_t>((hexDigit(n.key[b * 2]) << 4) | hexDigit(n.key[b * 2 + 1])));
        }
        for (int b = 0; b < 4; b++) entries.push_back(static_cast<uint8_t>((n.heardSecondsAgo >> (8 * b)) & 0xFF));
        entries.push_back(static_cast<uint8_t>(n.snrQ4));
        returned++;
      }
      std::vector<uint8_t> body;
      body.push_back(static_cast<uint8_t>(total & 0xFF));
      body.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
      body.push_back(static_cast<uint8_t>(returned & 0xFF));
      body.push_back(static_cast<uint8_t>((returned >> 8) & 0xFF));
      body.insert(body.end(), entries.begin(), entries.end());
      return body;
    }
    if (reqType == mm::kReqTypeGetOwnerInfo) {
      // answer: request-silent - REQ_TYPE_GET_OWNER_INFO is FIRMWARE_VER_LEVEL >= 2, and a
      // repeater that does not know a request type answers it with nothing at all.
      if (firmwareVerLevel_ < mm::kOwnerInfoVerLevel) return {};
      // answer: owner-info
      return ownerInfo_;
    }
    // answer: request-silent
    return {};
  }

  // The CLI, in handleCommand's own order. An empty string is "the firmware answers nothing",
  // which on the air is silence.
  std::string cliReply(const std::string& command) {
    const auto scripted = scripted_.find(command);
    if (scripted != scripted_.end()) return scripted->second;

    // answer: cli-region-load
    if (regionLoadActive_) {
      if (command.find_first_not_of(' ') == std::string::npos) {
        regionLoadActive_ = false;
        char reply[64];
        std::snprintf(reply, sizeof reply, "OK - loaded %d regions", loadedRegions_);
        return reply;
      }
      loadedRegions_++;
      return "";
    }

    std::string c = command;
    while (!c.empty() && c.front() == ' ') c.erase(c.begin());

    // answer: cli-setperm
    if (starts(c, "setperm ")) {
      const std::string rest = c.substr(8);
      const size_t sp = rest.find(' ');
      if (sp == std::string::npos) return "Err - bad params";
      const std::string hex = rest.substr(0, sp);
      const int level = std::atoi(rest.c_str() + sp + 1);
      if (hex.empty() || hex.size() % 2 != 0 ||
          hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return "Err - bad pubkey";
      }
      // `ClientACL::applyPermissions` finds the same ClientInfo the login filled, so a setperm on
      // our own key moves the session with it (task p4.1-60).
      const std::string target = resolve(hex);
      if (target.empty()) return "Err - invalid params";
      if (level != 0) {
        grant(target, static_cast<uint8_t>(level));
        return "OK";
      }
      // answer: cli-setperm-zero - the guest role is not an entry: `applyPermissions` **deletes**
      // the entry and moves the later ones down, so a key taken off at level 0 is not in the
      // table at all afterwards (which is why the blank login that follows falls through to the
      // password compare) and a second level-0 line for the same key finds nothing.
      const int gone = indexOf(target);
      if (gone < 0) return "Err - invalid params";  // getClient found no such entry
      const int ours = indexOf(selfKey_);
      const int held = static_cast<int>(acl_.size());
      acl_.erase(acl_.begin() + gone);
      // And the reply is addressed to the slot the sender held before the line ran: unless our
      // own entry was the last one in the table, a deletion at or below it sends the "OK" to
      // whichever client sits in that slot now, and we never hear it (task p1.10-48). From here
      // that is a silence, exactly as a lost reply is.
      if (gone <= ours && ours != held - 1) {
        replyWentElsewhere_ = true;
        return "";
      }
      return "OK";
    }

    // answer: cli-reboot — _board->reboot() does not return, so nothing is ever answered
    if (starts(c, "reboot")) {
      rebooted_ = true;
      return "";
    }
    // answer: cli-advert
    if (starts(c, "advert.zerohop")) {
      adverts_++;
      return "OK - zerohop advert sent";
    }
    if (starts(c, "advert")) {
      adverts_++;
      floodAdverts_++;
      return "OK - Advert sent";
    }
    // answer: cli-time
    if (starts(c, "time ")) {
      const uint32_t secs = static_cast<uint32_t>(std::strtoul(c.c_str() + 5, nullptr, 10));
      if (secs > repeaterNow_) {
        repeaterNow_ = secs;
        return "OK - clock set: 14:00 - 23/9/2026 UTC";
      }
      return "(ERR: clock cannot go backwards)";
    }
    // answer: cli-password
    if (starts(c, "password ")) {
      adminPassword_ = c.substr(9).substr(0, mm::kMaxPasswordLen);
      return "password now: " + adminPassword_;
    }
    // answer: cli-get
    if (starts(c, "get ")) {
      const std::string key = c.substr(4);
      const auto it = config_.find(key);
      if (it == config_.end()) return "??: " + key;
      return "> " + it->second;
    }
    // answer: cli-set
    if (starts(c, "set ")) {
      const std::string rest = c.substr(4);
      const size_t sp = rest.find(' ');
      if (sp == std::string::npos) return "unknown config: " + rest;
      const std::string key = rest.substr(0, sp);
      const std::string value = rest.substr(sp + 1);
      // answer: cli-guest-password
      if (key == "guest.password") {
        guestPassword_ = value.substr(0, mm::kMaxPasswordLen);
        return "OK";
      }
      if (config_.find(key) == config_.end()) return "unknown config: " + rest;
      config_[key] = value;
      if (key == "repeat") return value == "off" ? "OK - repeat is now OFF" : "OK - repeat is now ON";
      return "OK";
    }
    // answer: cli-ver
    if (starts(c, "ver")) return firmware_;
    // answer: cli-board
    if (starts(c, "board")) return board_;
    // answer: cli-region
    if (starts(c, "region")) return regionReply(c);

    // answer: cli-unknown
    return "Unknown command";
  }

  std::string regionReply(const std::string& c) {
    if (c == "region") return regionListing_;
    if (c == "region default") return " default scope is " + (regionDefault_.empty() ? "<null>" : regionDefault_);
    if (c == "region home") return " home is *";
    if (c == "region load") {
      regionLoadActive_ = true;
      loadedRegions_ = 0;
      return "";
    }
    if (c == "region save") return "OK";
    if (starts(c, "region default ")) {
      regionDefault_ = c.substr(15);
      return " default scope is now " + regionDefault_;
    }
    if (starts(c, "region put ") || starts(c, "region allowf ") || starts(c, "region denyf ")) return "OK";
    return "Err - unknown region";
  }

  // `setperm` takes hex of any even length; the firmware matches it against the entries it has
  // (and `putClient` creates one for a whole key it does not know).
  std::string resolve(const std::string& hex) const {
    for (const AclEntry& e : acl_) {
      if (e.key.compare(0, hex.size(), hex) == 0) return e.key;
    }
    return hex.size() == 64 ? hex : std::string();
  }

  static bool starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

  // ---------------------------------------------------------------- the wait

  bool await(Awaiting what, uint32_t tag, uint32_t timeoutMs, void (*pump)()) {
    reply_ = mm::MeshReply();
    const uint32_t deadline = platform_.millis() + timeoutMs;
    bool got = false;
    while (!got && static_cast<int32_t>(platform_.millis() - deadline) < 0) {
      loop();
      if (pump != nullptr) pump();
      // A reply held over from an earlier request comes first: it has been due the longer.
      for (size_t i = 0; i < late_.size(); i++) {
        Pending& p = late_[i];
        if (p.waitsToSkip > 0 || p.neverArrives || static_cast<int32_t>(platform_.millis() - p.deliverAtMs) < 0) {
          continue;
        }
        const bool taken = deliver(p, what, tag);
        const size_t which = p.sendIndex;
        late_.erase(late_.begin() + static_cast<long>(i));
        if (taken) {
          if (which < sends_.size()) sends_[which].answered = true;
          counters_.rx++;
          got = true;
        }
        break;
      }
      if (got) break;
      if (pending_.has && !pending_.neverArrives &&
          static_cast<int32_t>(platform_.millis() - pending_.deliverAtMs) >= 0) {
        const bool taken = deliver(pending_, what, tag);
        const size_t which = pending_.sendIndex;
        pending_ = Pending();
        if (taken) {
          if (which < sends_.size()) sends_[which].answered = true;
          counters_.rx++;
          got = true;
          break;
        }
      }
      platform_.delayMs(1);
    }
    // One wait of this mesh is over, delivered or not: a late reply is a wait nearer its turn.
    for (Pending& p : late_) {
      if (p.waitsToSkip > 0) p.waitsToSkip--;
    }
    return got;
  }

  // AgentMesh's own reading: a reply of the wrong kind, or with a tag that is not the one being
  // waited for, is dropped and not read, and the wait goes on into silence. A login waits with
  // tag 0, so **every** response reaches the library there - which is where task p1.10-47's rule
  // lives, and the reason this fake hands one through rather than filtering it here.
  bool deliver(const Pending& held, Awaiting what, uint32_t tag) {
    const mm::MeshReply& p = held.reply;
    const bool isText = p.kind == mm::MeshReply::Kind::Text;
    if ((what == Awaiting::Text) != isText) return false;
    if (what == Awaiting::Response && tag != 0 && p.tag != tag) return false;
    reply_ = p;
    if (held.teachesPath && returnPathSize_ > 0) {
      // answer: path-return
      outPath_ = returnPath_;
      outPathLen_ =
          static_cast<uint8_t>(((returnPathSize_ - 1) << 6) | (returnPath_.size() / returnPathSize_));
      havePath_ = true;
    }
    return true;
  }

  int count(Send::Kind kind) const {
    int n = 0;
    for (const Send& s : sends_) {
      if (s.kind == kind) n++;
    }
    return n;
  }

  // The two entries `binary-response-neighbours.hex` carries, as a neighbour list: the same
  // prefixes, ages and SNRs test/test_reply_codec asserts for that file.
  static std::vector<NeighbourEntry> fixtureNeighbours() {
    return {
        NeighbourEntry{"7f8e9dacbbcad9e8" + std::string(48, '0'), 60, 30},
        NeighbourEntry{"c0ffee0011223344" + std::string(48, '0'), 3600, -9},
    };
  }

  // The default key list of CONFIGURATION §2 as a V3 at v1.17 answers it, so a config.read of
  // the server's own list is answered key for key.
  static std::map<std::string, std::string> defaultConfig() {
    return {
        {"name", "Grenaa Chimney #1"},
        {"lat", "56.4167"},
        {"lon", "10.8833"},
        {"advert.interval", "240"},
        {"flood.advert.interval", "12"},
        {"repeat", "on"},
        {"dutycycle", "10.0%"},
        {"flood.max", "64"},
        {"flood.max.unscoped", "3"},
        {"tx", "22"},
        {"path.hash.mode", "1"},
        {"multi.acks", "0"},
        {"allow.read.only", "off"},
        {"agc.reset.interval", "0"},
        {"freq", "869.618"},
        {"guest.password", ""},
    };
  }

  // A key as this mesh holds one: 64 lowercase hex, which is what `beginJob` normalises to.
  static std::string lowerHex(std::string hex) {
    for (char& c : hex) {
      if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return hex;
  }
  // The local door's one comparison, in one place: both sides of it read this.
  bool servesLocally(const std::string& keyHex) const { return !doorKey_.empty() && keyHex == doorKey_; }

  FakePlatform& platform_;

  // the repeater
  std::string key_ = std::string(64, 'a');
  std::string selfKey_ = std::string(64, 'b');
  std::string adminPassword_ = "adm1n-pass";
  std::string guestPassword_;
  std::vector<AclEntry> acl_;
  std::map<std::string, std::string> config_;
  std::vector<NeighbourEntry> neighbours_;
  std::vector<uint8_t> stats_;
  std::vector<uint8_t> telemetry_;
  std::vector<uint8_t> ownerInfo_;
  uint8_t firmwareVerLevel_ = kFirmwareVerLevel;
  std::string firmware_ = "v1.17.1 (Build: 27 Sep 2025)";
  std::string board_ = "Heltec";
  std::string regionListing_ = "* F\n dk F\n";
  std::string floodScope_;
  uint8_t floodPathHashSize_ = 2;
  bool answersLocally_ = false;
  std::string doorKey_;  // the one repeater this mesh reaches without the air, if any
  mm::FloodMode nextMode_ = mm::FloodMode::Scoped;
  bool scopedReaches_ = true;
  bool unscopedReaches_ = true;
  std::string regionDefault_ = "dk";
  uint32_t repeaterNow_ = 1790000000;
  uint32_t lastRepeaterUnique_ = 0;
  bool regionLoadActive_ = false;
  int loadedRegions_ = 0;
  bool rebooted_ = false;
  int adverts_ = 0;
  int floodAdverts_ = 0;
  std::map<std::string, std::string> scripted_;
  std::set<std::string> silentLines_;
  std::set<std::string> unserved_;
  std::vector<int8_t> traceSnrsQ4_ = {29, -10, 26};  // trace-data.hex: 7.25, -2.5, 6.5
  float traceFinalSnr_ = 7.5f;                       // trace-data.hex's final SNR

  // the faults
  int silentSends_ = 0;
  int silentNeighboursPage_ = 0;
  int shortNeighboursPage_ = 0;
  std::map<int, int> pageTotals_;  // from this page on, the repeater says this many neighbours
  bool silentAlways_ = false;
  int wrongTagSends_ = 0;
  int slowSends_ = 0;
  bool forgotten_ = false;
  int somethingElseLogins_ = 0;
  int silentLogins_ = 0;
  int lateSends_ = 0;
  int lateWaits_ = 1;  // how many waits the next late reply misses (task p1.10-49)
  std::map<std::string, int> lostLines_;
  std::map<std::string, int> lostLineReplies_;
  std::map<std::string, std::pair<int, int>> lateLineReplies_;  // times, and the waits it misses
  // The last line's reply was addressed to another client and never comes home (task p1.10-48's
  // reading of `applyPermissions` and `onPeerDataRecv`), which is a silence and not an empty
  // reply: with a prefix on the line the two are no longer the same thing.
  bool replyWentElsewhere_ = false;
  uint32_t directLatencyMs_ = 400;
  uint32_t floodLatencyMs_ = 1500;
  // the way back (task p1.21-71)
  int lostWaysBack_ = 0;
  bool wayBackDead_ = false;
  bool tellsNoWayBack_ = false;
  uint32_t wayBackClearMs_ = 150;
  bool pad_ = true;

  // the mesh's own state
  mm::ObservationRing* ring_ = nullptr;
  mm::MeshCounters counters_;
  uint32_t overheard_ = 0;
  uint32_t pumps_ = 0;
  std::string peer_;
  bool peerSet_ = false;
  std::vector<uint8_t> outPath_;
  uint8_t outPathLen_ = 0;
  bool havePath_ = false;
  std::vector<uint8_t> returnPath_;
  uint8_t returnPathSize_ = 2;
  mm::MeshReply reply_;
  Pending pending_;
  // The replies that missed their own request's wait and are still on their way home.
  std::vector<Pending> late_;
  uint32_t lastUnique_ = 0;
  std::vector<Send> sends_;
  std::vector<WayBackTold> waysBack_;
  std::vector<std::string> frames_;
};

}  // namespace fake
