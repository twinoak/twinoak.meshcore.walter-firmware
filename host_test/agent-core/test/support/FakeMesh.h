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
// It is a repeater and not a radio: nothing here models airtime, collisions or the real region
// loader. A reply arrives after `directLatencyMs` or `floodLatencyMs` **on the fake clock**
// (support/FakePlatform.h), so a wait is asserted rather than slept, and every send is recorded
// with that clock's time - which is what *one send per command*, the minimum gap and the
// per-minute cap are asserted against.
//
// ---- answers (the manifest) ------------------------------------------------------------------
//
// answer: login-ok              cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:135-144 MyMesh::handleLoginReq -- the reply is timestamp(4), RESP_SERVER_LOGIN_OK, 0, is_admin, permissions, a random blob(4), FIRMWARE_VER_LEVEL
// answer: login-ok              cite: fixture login-success.hex
// answer: login-ok              cite: fixture login-success-readonly.hex
// answer: login-acl-entry       cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:91-99 MyMesh::handleLoginReq -- a blank password is the ACL alone: the entry's own level, and no password check at all
// answer: login-password        cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:100-128 MyMesh::handleLoginReq -- the admin password is admin, the guest password is guest, and the password rewrites the entry's role
// answer: login-silent-password cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:106-111 MyMesh::handleLoginReq -- a password that is neither is `return 0`, which is silence and not a refusal
// answer: login-silent-replay   cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:113-117 MyMesh::handleLoginReq -- a sender timestamp at or below the last one is silence
// answer: login-refused         cite: fixture login-failed.hex
// answer: status                cite: fixture status-response.hex
// answer: status                cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:216-240 MyMesh::handleRequest -- REQ_TYPE_GET_STATUS answers every login, guests included
// answer: telemetry             cite: fixture telemetry-response.hex
// answer: telemetry             cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:241-263 MyMesh::handleRequest -- REQ_TYPE_GET_TELEMETRY_DATA answers Cayenne LPP after the tag
// answer: neighbours            cite: fixture binary-response-neighbours.hex
// answer: neighbours            cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:277-371 MyMesh::handleRequest -- REQ_TYPE_GET_NEIGHBOURS pages by the count asked for: total(i16), returned(i16), then prefix, heard_seconds_ago(u32), snr(i8)
// answer: owner-info            cite: fixture binary-response-owner-info.hex
// answer: owner-info            cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:372-375 MyMesh::handleRequest -- REQ_TYPE_GET_OWNER_INFO is "FIRMWARE_VERSION\nnode_name\nowner_info"
// answer: access-list           cite: fixture access-list-response.hex
// answer: access-list           cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:264-276 MyMesh::handleRequest -- REQ_TYPE_GET_ACCESS_LIST is {pub_key[6], permissions} per entry, entries at permissions 0 skipped, admin only
// answer: request-silent        cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:376-377 MyMesh::handleRequest -- an unknown request type is `return 0`, which is silence
// answer: forgotten-session     cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:663-671 MyMesh::onPeerDataRecv -- a sender the login table does not hold is answered nothing at all
// answer: request-silent-replay cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:676-700 MyMesh::onPeerDataRecv -- a REQ whose timestamp is not above the client's last is silence
// answer: datagram-too-long     cite: fw companion-v1.17.1 d9296435 src/Mesh.cpp:488-493 Mesh::createDatagram -- a plaintext past MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1) = 167 bytes is never sent at all
// answer: reply-padding         cite: fw companion-v1.17.1 d9296435 src/Utils.cpp:85-126 Utils::encrypt -- the plaintext is zero-padded to the cipher block, so a reply arrives a multiple of sixteen long
// answer: reply-padding         cite: fixture binary-response-telemetry-padded.hex
// answer: path-return           cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:596-608 MyMesh::onAnonDataRecv -- a flooded request is answered with a path return, which is how a login teaches the way home
// answer: cli-not-admin         cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:701-702 MyMesh::onPeerDataRecv -- a text message from a sender below admin is dropped without a word
// answer: cli-retry             cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:709-740 MyMesh::onPeerDataRecv -- a sender timestamp equal to the last is a retry: nothing is run and the empty reply is not sent, and one below it is a replay
// answer: cli-reply             cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:741-758 MyMesh::onPeerDataRecv -- a non-empty reply is a TXT_MSG of the repeater's own clock, TXT_TYPE_CLI_DATA in the flags byte, then the text
// answer: cli-setperm           cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:1239-1260 MyMesh::handleCommand -- `setperm <pubkey-hex> <level>` answers OK, or Err - bad params / Err - bad pubkey / Err - invalid params
// answer: cli-region-load       cite: fw repeater-v1.17.1 d9296435 examples/simple_repeater/MyMesh.cpp:1197-1229 MyMesh::handleCommand -- in load mode a region line is answered with nothing and the blank line ends the load with `OK - loaded N regions`
// answer: cli-reboot            cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:185-186 CommonCLI::handleCommand -- `reboot` is _board->reboot(), which does not return: the silence is the answer
// answer: cli-advert            cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:191-198 CommonCLI::handleCommand -- `advert` answers `OK - Advert sent` and `advert.zerohop` answers `OK - zerohop advert sent`
// answer: cli-time              cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:217-228 CommonCLI::handleCommand -- `time <unix>` above the clock answers `OK - clock set: ...` and at or below it `(ERR: clock cannot go backwards)`
// answer: cli-password          cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:256-261 CommonCLI::handleCommand -- `password <new>` stores at most fifteen bytes and echoes `password now: <what it stored>`
// answer: cli-ver               cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:272-273 CommonCLI::handleCommand -- `ver` is the firmware version and the build date
// answer: cli-board             cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:274-275 CommonCLI::handleCommand -- `board` is the board's manufacturer name
// answer: cli-unknown           cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:443-444 CommonCLI::handleCommand -- a command it does not know is `Unknown command`
// answer: cli-get               cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:807-986 CommonCLI::handleGetCmd -- a key it knows is `> <value>` and one it does not is `??: <key>`
// answer: cli-set               cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:448-804 CommonCLI::handleSetCmd -- a value it took is `OK` (`OK - repeat is now ON`/`OFF` for repeat), a key it does not know is `unknown config: <rest>`
// answer: cli-guest-password    cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:506-509 CommonCLI::handleSetCmd -- `set guest.password <new>` answers OK
// answer: cli-region            cite: fw companion-v1.17.1 d9296435 src/helpers/CommonCLI.cpp:1041-1140 CommonCLI::handleRegionCmd -- `region` exports the map into 160 characters, `region default` answers ` default scope is <name>`, `region load` answers nothing and `region save` answers OK
// answer: trace                 cite: fixture trace-data.hex
// answer: trace                 cite: fw companion-v1.17.1 d9296435 src/Mesh.cpp:42-67 Mesh::onRecvPacket -- a TRACE states its hash size as an exponent in the flags byte and carries one SNR a hop, the final one measured on the packet home
// answer: discovery             cite: fw companion-v1.17.1 d9296435 examples/companion_radio/MyMesh.cpp:1602-1630 MyMesh::handleCmdFrame -- path discovery is a flooded base-telemetry request, so what answers it is the telemetry reply and a path return
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
};

struct NeighbourEntry {
  std::string key;  // 64 lowercase hex; the reply carries the prefix the request asked for
  uint32_t heardSecondsAgo = 0;
  int8_t snrQ4 = 0;
};

// What the mesh was asked to send, with the fake clock's time: the record *one send per command*,
// the minimum gap and the per-minute cap are asserted against.
struct Send {
  enum class Kind : uint8_t { Login, Request, Cli, Discovery, Trace };
  Kind kind = Kind::Login;
  uint32_t atMs = 0;
  bool direct = false;  // else flooded
  uint32_t tag = 0;     // the tag, or for a CLI line the sender timestamp the mesh drew
  uint8_t reqType = 0;  // Request only
  std::string text;     // Cli only
  bool sensitive = false;
  bool answered = false;  // a reply was delivered for this send

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
    acl_.push_back(AclEntry{keyHex, permissions, 0});
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
  void setRegionDefault(const std::string& scope) { regionDefault_ = scope; }
  void setNeighbours(std::vector<NeighbourEntry> list) { neighbours_ = std::move(list); }
  void setRepeaterClock(uint32_t epoch) { repeaterNow_ = epoch; }
  uint32_t repeaterClock() const { return repeaterNow_; }
  void setStatusBytes(std::vector<uint8_t> bytes) { stats_ = std::move(bytes); }
  void setTelemetryBytes(std::vector<uint8_t> bytes) { telemetry_ = std::move(bytes); }
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
  void refuseTheNextLogin(int logins) { refuseLogins_ = logins; }

  void setLatency(uint32_t directMs, uint32_t floodMs) {
    directLatencyMs_ = directMs;
    floodLatencyMs_ = floodMs;
  }
  // The zero padding a reply arrives with: on by default, as the air does it.
  void setPadding(bool on) { pad_ = on; }

  // ------------------------------------------------------------------ what a test reads back

  const std::vector<Send>& sends() const { return sends_; }
  int sendCount() const { return static_cast<int>(sends_.size()); }
  int timesSent(const std::string& line) const {
    int n = 0;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Cli && s.text == line) n++;
    }
    return n;
  }
  int logins() const { return count(Send::Kind::Login); }
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
  // The sender timestamps of the CLI lines, in the order they went: the series the repeater's
  // replay check reads, which must be strictly increasing.
  std::vector<uint32_t> cliTimestamps() const {
    std::vector<uint32_t> out;
    for (const Send& s : sends_) {
      if (s.kind == Send::Kind::Cli) out.push_back(s.tag);
    }
    return out;
  }
  // The gap in milliseconds between the starts of two sends, in order.
  std::vector<uint32_t> gapsMs() const {
    std::vector<uint32_t> gaps;
    for (size_t i = 1; i < sends_.size(); i++) gaps.push_back(sends_[i].atMs - sends_[i - 1].atMs);
    return gaps;
  }
  // The most sends that started inside any sixty seconds of the record.
  int mostInAnyMinute() const {
    int worst = 0;
    for (size_t i = 0; i < sends_.size(); i++) {
      int n = 0;
      for (size_t j = i; j < sends_.size(); j++) {
        if (sends_[j].atMs - sends_[i].atMs < 60000u) n++;
      }
      if (n > worst) worst = n;
    }
    return worst;
  }
  void forgetSends() { sends_.clear(); }

  // ------------------------------------------------------------------ IMesh

  void setRing(mm::ObservationRing* ring) override { ring_ = ring; }
  mm::ObservationRing* ring() const { return ring_; }
  const mm::MeshCounters& counters() const override { return counters_; }
  uint32_t overheard() const override { return overheard_; }
  void setOverheard(uint32_t n) { overheard_ = n; }
  void setAdvertsDropped(uint32_t n) { counters_.advertsDropped = n; }
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

  mm::SendResult sendLogin(const char* password, uint32_t& tag) override {
    tag = 0;  // a login reply carries the repeater's own timestamp and no tag of ours
    if (!peerSet_) return mm::SendResult::Failed;
    const uint32_t senderTimestamp = unique();
    const bool direct = note(Send::Kind::Login, senderTimestamp);
    const std::string offered =
        password == nullptr ? std::string() : std::string(password).substr(0, mm::kMaxPasswordLen);
    const mm::SendResult how = direct ? mm::SendResult::Direct : mm::SendResult::Flood;
    if (!reachable()) return how;

    if (refuseLogins_ > 0) {
      refuseLogins_--;
      // answer: login-refused — a reply that is not a login-ok at all. Stock firmware never
      // sends one (a wrong password is silence); the legacy frame and any firmware that does
      // send one land here, and the runner reads it as `Refused`.
      arm(response({0x7F, 0x01}, senderTimestamp, /*repeaterStamps=*/true));
      return how;
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
    body.push_back(kFirmwareVerLevel);
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
    sends_.back().text = text;
    sends_.back().sensitive = sensitive;
    const mm::SendResult how = direct ? mm::SendResult::Direct : mm::SendResult::Flood;
    if (!reachable()) return how;
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
    const std::string answer = cliReply(text);
    if (answer.empty()) {
      // answer: cli-reply — an empty reply is not sent at all
      return how;
    }
    arm(asText(answer));
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
    p.reply.snr = traceFinalSnr_;
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
    bool viaPath = false;
  };

  // ---------------------------------------------------------------- the clocks and the record

  // getCurrentTimeUnique(): the time, or one above the last one handed out, so the series a
  // repeater sees from this key is strictly increasing across REQs and CLI lines alike
  // (src/MeshCore.h:108-114 at d9296435). This is the **exit node's** series.
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
    s.tag = tag;
    sends_.push_back(s);
    counters_.tx++;
    return s.direct;
  }

  bool reachable() {
    if (silentAlways_) return false;
    if (silentSends_ > 0) {
      silentSends_--;
      return false;
    }
    return addressed();
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
    p.reply.snr = replySnr_;
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
    p.reply.snr = replySnr_;
    p.has = true;
    return p;
  }

  void pad(std::vector<uint8_t>& plaintext) const {
    // answer: reply-padding
    if (!pad_) return;
    while (plaintext.size() % kCipherBlock != 0) plaintext.push_back(0);
  }

  void arm(Pending p) {
    if (!p.has) {
      pending_ = Pending();
      return;
    }
    const bool flood = !sends_.empty() && !sends_.back().direct;
    p.deliverAtMs = platform_.millis() + (flood ? floodLatencyMs_ : directLatencyMs_);
    if (slowSends_ > 0) {
      slowSends_--;
      p.neverArrives = true;  // past every wait, which from here is a silence
    }
    // A flooded request is answered with a path return, which teaches the way home.
    p.viaPath = flood && !returnPath_.empty();
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
      int prefixLen = data[5];
      if (prefixLen > static_cast<int>(mm::kPublicKeyLen)) prefixLen = static_cast<int>(mm::kPublicKeyLen);
      std::vector<uint8_t> entries;
      int returned = 0;
      const int total = static_cast<int>(neighbours_.size());
      for (int i = 0; i < want && i + offset < total; i++) {
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
      // answer: owner-info
      return ownerInfo_;
    }
    // answer: request-silent
    return {};
  }

  // The CLI, in handleCommand's own order. An empty string is "the firmware answers nothing",
  // which on the air is silence.
  std::string cliReply(const std::string& command) {
    if (silentLines_.count(command) > 0) return "";
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
      if (level == 0) {
        for (AclEntry& e : acl_) {
          if (e.key == target) e.permissions = 0;
        }
      } else {
        grant(target, static_cast<uint8_t>(level));
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
    while (static_cast<int32_t>(platform_.millis() - deadline) < 0) {
      loop();
      if (pump != nullptr) pump();
      if (pending_.has && !pending_.neverArrives &&
          static_cast<int32_t>(platform_.millis() - pending_.deliverAtMs) >= 0) {
        if (deliver(what, tag)) {
          if (!sends_.empty()) sends_.back().answered = true;
          counters_.rx++;
          return true;
        }
      }
      platform_.delayMs(1);
    }
    return false;
  }

  // AgentMesh's own reading: a reply of the wrong kind, or with a tag that is not the one being
  // waited for, is dropped and not read, and the wait goes on into silence.
  bool deliver(Awaiting what, uint32_t tag) {
    const mm::MeshReply& p = pending_.reply;
    const bool isText = p.kind == mm::MeshReply::Kind::Text;
    if ((what == Awaiting::Text) != isText) {
      pending_ = Pending();
      return false;
    }
    if (what == Awaiting::Response && tag != 0 && p.tag != tag) {
      pending_ = Pending();
      return false;
    }
    reply_ = p;
    reply_.viaPath = pending_.viaPath;
    if (pending_.viaPath && returnPathSize_ > 0) {
      // answer: path-return
      outPath_ = returnPath_;
      outPathLen_ =
          static_cast<uint8_t>(((returnPathSize_ - 1) << 6) | (returnPath_.size() / returnPathSize_));
      havePath_ = true;
    }
    pending_ = Pending();
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
  std::string firmware_ = "v1.17.1 (Build: 27 Sep 2025)";
  std::string board_ = "Heltec";
  std::string regionListing_ = "* F\n dk F\n";
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
  std::vector<int8_t> traceSnrsQ4_ = {29, -10, 26};  // trace-data.hex: 7.25, -2.5, 6.5
  float traceFinalSnr_ = 7.5f;                       // trace-data.hex's final SNR
  float replySnr_ = 6.25f;                           // status-response.hex's last_snr

  // the faults
  int silentSends_ = 0;
  bool silentAlways_ = false;
  int wrongTagSends_ = 0;
  int slowSends_ = 0;
  bool forgotten_ = false;
  int refuseLogins_ = 0;
  uint32_t directLatencyMs_ = 400;
  uint32_t floodLatencyMs_ = 1500;
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
  uint32_t lastUnique_ = 0;
  std::vector<Send> sends_;
};

}  // namespace fake
