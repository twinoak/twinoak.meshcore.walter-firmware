// What a repeater answers, as bytes, read into structs.
//
// The board **is** the companion, so these are one layer thinner than the .NET agent's frames:
// there the reply travels on to an app over a cable and arrives wrapped in a push code, a
// reserved byte and a prefix or a tag (`MyMesh::onContactResponse`, `companion-v1.17.1` at
// d9296435); here the same bytes arrive in `Mesh::onPeerDataRecv`, whose `data` is the
// repeater's reply with a four-byte tag in front of it. So what a handler parses is `data[4..]`,
// which is exactly the part of the transcribed fixture that follows the wrapper -
// tests/fixtures/companion/README.md is the table, and test/test_reply_codec strips the wrapper
// and feeds the rest to these functions.
//
// Host-portable on purpose: no Arduino, no MeshCore, no allocation. Everything is bounded, and a
// reply that is shorter than it should be is refused rather than read past.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// ClientACL.h at d9296435: the role is the low two bits of the permissions byte.
constexpr uint8_t kPermRoleMask = 0x03;
constexpr uint8_t kPermGuest = 0;
constexpr uint8_t kPermReadOnly = 1;
constexpr uint8_t kPermReadWrite = 2;
constexpr uint8_t kPermAdmin = 3;

// "guest" | "read-only" | "read-write" | "admin", the names the .NET agent's
// OpCodes.PermissionName gives the same byte. The role is masked out first.
const char* permissionName(uint8_t permissions);

// The credential classes of EXIT-NODES §7, least privileged first, as the claim spells them.
enum class CredentialClass : uint8_t { None = 0, Guest = 1, Acl = 2, Admin = 3 };
const char* credentialClassName(CredentialClass c);
bool parseCredentialClass(const char* text, CredentialClass& out);

// The role a class needs, as the firmware grades it (SessionStore.Needs in the .NET agent):
// admin needs admin, acl at least read-only, guest and none take whatever a login was answered
// with.
uint8_t roleNeededFor(CredentialClass c);

// ---------------------------------------------------------------------------------------------
// The login reply

// What `MyMesh::handleLoginReq` (repeater-v1.17.1, examples/simple_repeater/MyMesh.cpp:90-145)
// answers, after the four-byte tag: RESP_SERVER_LOGIN_OK(1), keep_alive/16(1), is_admin(1),
// permissions(1), a random blob(4), FIRMWARE_VER_LEVEL(1) - thirteen bytes in all, of which a
// legacy repeater sends only the literal "OK".
struct LoginReply {
  bool legacy = false;          // the two-byte "OK" of a pre-v7 repeater
  uint8_t isAdminFlag = 0;      // the repeater's `is_admin ? 1 : 0`, and not a role byte
  bool hasAclPermissions = false;
  uint8_t aclPermissions = 0;   // the client entry's permission byte; & 3 is the role
  bool hasFirmwareLevel = false;
  uint8_t firmwareLevel = 0;

  // The role granted: the ACL byte's role when the reply carries one, else what the flag alone
  // can say - admin or guest. Reading the flag as the role is what task p3.5-41 found and fixed
  // in the .NET agent, and this is the corrected reading.
  uint8_t role() const;
  bool isAdmin() const { return role() == kPermAdmin; }
};

// `data` is the reply payload **after** the four-byte tag. False when it is not a login reply
// this firmware knows (a refusal, or too short to read).
bool parseLoginReply(const uint8_t* data, size_t len, LoginReply& out);

// ---------------------------------------------------------------------------------------------
// The status reply

// `RepeaterStats` of examples/simple_repeater/MyMesh.h:44-60 at d9296435, 56 bytes on current
// firmware and 48 on one from before the last two counters were added - which is why those two
// are optional here rather than zero.
struct StatusReading {
  int batteryMv = 0;
  int txQueueLen = 0;
  int noiseFloorDbm = 0;
  int lastRssi = 0;
  uint32_t packetsRecv = 0;
  uint32_t packetsSent = 0;
  uint32_t airtimeS = 0;
  uint32_t uptimeS = 0;
  uint32_t sentFlood = 0;
  uint32_t sentDirect = 0;
  uint32_t recvFlood = 0;
  uint32_t recvDirect = 0;
  int errEvents = 0;
  double lastSnr = 0;  // the firmware stores it x4
  int directDups = 0;
  int floodDups = 0;
  bool hasRxAirtime = false;
  uint32_t rxAirtimeS = 0;
  bool hasRecvErrors = false;
  uint32_t recvErrors = 0;
};

constexpr size_t kRepeaterStatsMinLen = 48;

bool parseRepeaterStats(const uint8_t* stats, size_t len, StatusReading& out);

// The owner-info reply of REQ_TYPE_GET_OWNER_INFO: "FIRMWARE_VERSION\nnode_name\nowner_info",
// no NUL and no length. Only the first line is read - the version the association's verify step
// records - and the node's name and its owner's note are never kept.
constexpr size_t kMaxFirmwareVersionLen = 24;
bool parseOwnerInfoFirmware(const uint8_t* data, size_t len, char* out, size_t cap);

// ---------------------------------------------------------------------------------------------
// The neighbours reply

// The firmware answers a page at a time, and nine is a page (NeighboursPayload.PerPage): a reply
// carries 126 bytes and a row is 13 of them at the eight-byte prefix this firmware asks for.
constexpr int kNeighboursPerPage = 9;
constexpr int kNeighbourPrefixLen = 8;

// Six pages. A repeater with more zero-hop neighbours than this is not one an exit node's
// airtime budget should be paging through anyway, and the result says how many it has.
constexpr int kMaxNeighbours = 54;

struct Neighbour {
  char prefix[2 * kNeighbourPrefixLen + 1] = {0};
  long long lastHeardS = 0;
  double snr = 0;
};

struct NeighboursPage {
  int total = 0;  // how many the repeater knows, not how many this page carries
  int count = 0;
  Neighbour entries[kNeighboursPerPage];
};

// `data` is the reply payload after the four-byte tag: total(i16), returned(i16), then entries of
// prefix(prefixLen), heard_seconds_ago(u32), snr x4(i8).
bool parseNeighboursPage(const uint8_t* data, size_t len, int prefixLen, NeighboursPage& out);

// ---------------------------------------------------------------------------------------------
// The telemetry reply (Cayenne LPP)

constexpr int kMaxLppReadings = 24;
constexpr size_t kMaxLppTypeName = 20;

struct LppReading {
  int channel = 0;
  uint8_t typeCode = 0;
  char type[kMaxLppTypeName] = {0};
  double value = 0;
  const char* unit = nullptr;  // a static string, or nullptr for a type that has no unit
};

// Decodes as many readings as the buffer holds. `undecodedFrom` is where the bytes this table
// cannot read begin, and equals `len` when the whole reply was read.
//
// **The reply does not end where the readings do** (task p3.5-44): it is one AES datagram whose
// plaintext is zero-padded to a cipher block, so one to fifteen zero bytes follow the last
// reading and nothing in the frame says where they start. A channel byte of zero is the
// boundary - TELEM_CHANNEL_SELF is 1 and no repeater writes a channel 0 - and a run of zeros
// shorter than a block from there is the padding and is nothing.
int decodeLpp(const uint8_t* lpp, size_t len, LppReading* out, int max, size_t& undecodedFrom);

// ---------------------------------------------------------------------------------------------
// The trace reply

constexpr int kMaxTraceHops = 16;
constexpr size_t kMaxTraceHashSize = 8;

struct TraceHop {
  char prefix[2 * kMaxTraceHashSize + 1] = {0};
  bool hasSnr = false;
  double snr = 0;
};

struct TraceReading {
  int hashSize = 1;
  int count = 0;
  TraceHop hops[kMaxTraceHops];
};

// A trace packet states its path-hash size as an **exponent**: every repeater on the way matches
// its own hash over `1 << (flags & 3)` bytes (`Mesh::onRecvPacket`, PAYLOAD_TYPE_TRACE), so 0, 1,
// 2, 3 mean 1, 2, 4 and 8 bytes - a different encoding from a packet path's length byte, which is
// `(byte >> 6) + 1` and runs 1, 2, 3. The two agree on one and two bytes and nowhere else, which
// is why a path of three-byte hashes cannot be traced at all (task p3.5-42).
constexpr int traceHashSizeFor(uint8_t flags) { return 1 << (flags & 0x03); }
constexpr bool isTraceableHashSize(int hashSize) {
  return hashSize == 1 || hashSize == 2 || hashSize == 4 || hashSize == 8;
}
// The flags byte for a path of that hash size; only call it when isTraceableHashSize() says yes.
uint8_t traceFlagsFor(int hashSize);

// The arguments `Mesh::onTraceRecv` delivers, read into hops: `hashes` is `path_len` bytes of
// hop hashes and `snrsQ4` is one signed byte per hop, SNR x 4, in the same order.
bool parseTrace(uint8_t flags, const uint8_t* hashes, size_t hashBytes, const uint8_t* snrsQ4, size_t snrCount,
                TraceReading& out);

// Lowercase hex of `len` bytes into `out` (2*len + 1 bytes).
void toHexLower(const uint8_t* bytes, size_t len, char* out);

// ---------------------------------------------------------------------------------------------
// The access-list reply (task p3.5-43)

// REQ_TYPE_GET_ACCESS_LIST (0x05), answered to an **admin** session and to nobody else - a
// session below admin gets no reply at all (`MyMesh::handleRequest` at d9296435,
// examples/simple_repeater/MyMesh.cpp:262-275, guarded by `sender->isAdmin()` and returning 0).
// After the four-byte tag: {pub_key[6], permissions} per entry, entries with permissions 0
// skipped by the firmware, then the cipher block's zero padding.
constexpr uint8_t kReqTypeGetAccessList = 0x05;
constexpr int kAccessListPrefixBytes = 6;
constexpr int kAccessListEntrySize = kAccessListPrefixBytes + 1;

// What one reply can **carry**: the firmware's own loop would write 25 (4 + 25 x 7 fits its
// reply buffer), but a reply is one encrypted RESPONSE datagram and Mesh::createDatagram
// refuses one whose plaintext passes MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE
// - 1) = 167 bytes, so 4 + 23 x 7 = 165 fits and 24 entries (172) are never sent at all: an ACL
// of 24 or more is silence, not a short list. A list of exactly 23 is a list at the ceiling
// and no evidence either way (EXIT-NODES §4.3).
constexpr int kAccessListMaxEntries = (167 - 4) / kAccessListEntrySize;  // 23

struct AccessListEntry {
  char prefix[2 * kAccessListPrefixBytes + 1] = {0};  // twelve lowercase hex characters
  uint8_t permissions = 0;

  uint8_t role() const { return permissions & kPermRoleMask; }
};

// `data` is the reply payload after the four-byte tag. Entries are read until one whose
// permissions byte is 0 - the firmware never sends one, so that is where the padding starts -
// and everything after it must be zero: a non-zero tail means these are not the bytes this
// firmware thinks they are, and the function refuses rather than guesses (false, count 0). An
// empty body is an empty ACL, which is a real answer (true, count 0).
bool parseAccessList(const uint8_t* data, size_t len, AccessListEntry* out, int max, int& count);

// ---------------------------------------------------------------------------------------------
// A CLI reply (task p3-34, p4-54)

// A repeater answers a CLI command with a TXT_MSG datagram of its own: a timestamp (4), a flags
// byte whose upper six bits are the text type - TXT_TYPE_CLI_DATA (1) since v1.17, TXT_TYPE_PLAIN
// (0) from older firmware - and the reply text, zero-padded to the cipher block
// (`MyMesh::onPeerDataRecv`, examples/simple_repeater/MyMesh.cpp:739-758 at d9296435). The
// reply's own timestamp is the repeater's clock and is matched on nothing.
constexpr uint8_t kTxtTypePlain = 0;
constexpr uint8_t kTxtTypeCliData = 1;
// `CommonCLI::handleCommand` answers in a 160-byte buffer.
constexpr size_t kMaxCliReplyLen = 160;

// `data` is the datagram's plaintext, timestamp first. False when it is not a CLI reply this
// firmware reads (too short, or a text type that is neither of the two). The text is copied up
// to its first NUL or kMaxCliReplyLen bytes.
bool parseCliReply(const uint8_t* data, size_t len, char* text, size_t cap);

}  // namespace mm
