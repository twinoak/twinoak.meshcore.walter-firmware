// One struct per payload of protocol 1.1, and the functions the Walter's end needs: an encoder
// for every request it sends, a decoder for every response and every event it receives
// (spec/LINK-PROTOCOL.md §5-§8). The other two directions - decoding a request, encoding a
// response - are the radio firmware's and are not here; the fixtures are the only thing the two
// codecs share, and that is the point.
//
// How to read this file: it follows the specification's sections in order, and every struct's
// fields are the rows of that section's table, in that order, with the wire's type. The numbers
// are codec/wire.h.
//
// Three rules hold everywhere:
//
//   - **A variable-length field is a view** (codec/view.h) into the payload it was decoded from,
//     which is the receiver's buffer: valid until the next frame is read. Whoever keeps one copies
//     it (task 02's client does). core/ has no heap, so the alternative would be a fixed array of
//     the largest legal size in every struct - 238 bytes for a str8 - and a legal message would
//     be refused the day the radio's version string got long.
//   - **A decoder checks every length and every legal value and says malformed rather than
//     reading past the end** (§3's ERR_MALFORMED is the radio's word for the same thing). "Legal
//     value" means a value that selects the layout of what follows it: `path_mode` and a reply
//     record's `state`. A value that is only data - a status, a role, a reset reason, a
//     capability bit, an attention reason - is handed over as it arrived, because §5.1 says a
//     minor version adds numbers to these and that unknown ones are ignored, and a Walter that
//     refused the whole response would learn less than one that passed the number on.
//   - **Trailing bytes are ignored** (§5.1): a decoder reads the fields it knows and stops. A
//     response whose status is not OK is the one byte and nothing more (§3).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/view.h"
#include "core/link/codec/wire.h"

namespace core {
namespace link {

// What an encoder gives back: the payload's length, and whether it could be built at all. It
// cannot when a field is longer than the specification allows or the buffer is too small - and
// then nothing of it is sent, rather than something wrong (§6 wipes a password; half a password
// in a buffer nobody sends is still a password in a buffer).
struct Encoded {
  bool ok = false;
  std::size_t size = 0;
};

// ---------------------------------------------------------------------------- the shared pieces

// §6.1, a path on this link. `hash_size` and `hop_count` are 0 unless `mode` is kHops, and
// `hashes` is then hash_size x hop_count bytes, at most 64.
struct Path {
  std::uint8_t mode = path_mode::kNone;
  std::uint8_t hash_size = 0;
  std::uint8_t hop_count = 0;
  Bytes hashes;
};

// §5.1, the HELLO response's body, which EV_BOOT carries as well.
struct HelloBody {
  std::uint8_t proto_major = 0;
  std::uint8_t proto_minor = 0;
  std::uint8_t role = role::kUnknown;
  std::uint8_t slot = 0;  // 0 on a single-radio unit; the radio's position on a dual adapter
  std::uint8_t max_payload = 0;
  std::uint32_t caps = 0;
  std::uint32_t boot_id = 0;  // a change means the radio restarted and its per-boot state is gone
  Text fw_version;
  Text board;
  Text upstream;
  // 1.1, and read only off a body whose own `proto_minor` is kBandMinor or more: the band the
  // running image was built for, as its descriptor says it (`868`). Empty from a 1.0 RAK, which
  // is what every radio is until the repeater firmware's 1.1 reaches it.
  Text band;
};

// §6.3, a reply record. Which of the last four groups is filled in depends on `state`.
struct ReplyRecord {
  std::uint8_t state = reply_state::kNone;
  std::uint32_t tag = 0;
  std::uint8_t via_path = 0;  // 1 when the response came inside a returned path
  std::int8_t snr_q4 = 0;     // quarter-decibels
  std::int16_t rssi = kRssiUnknown;
  std::uint32_t elapsed_ms = 0;
  Bytes data;  // kResponse: the response after its four tag bytes
  Text text;   // kText: the CLI reply's text
  // kTrace, as MeshCore delivers a completed trace:
  std::uint8_t trace_flags = 0;
  std::int8_t final_snr_q4 = 0;
  Bytes hashes;
  Bytes snrs;
};

// §7, one observation: an advert this node heard, after its signature verified.
struct Observation {
  Bytes pubkey;             // 32 bytes
  std::uint32_t adv_ts = 0; // the timestamp inside the advert
  std::uint32_t age_s = 0;  // seconds since this node heard it - measured, so the clock cannot spoil it
  std::int8_t snr_q4 = 0;
  std::int16_t rssi = kRssiUnknown;
  std::uint8_t path_len = 0;  // bits 7-6 the hash size minus one, bits 5-0 the hop count
  Bytes path;
  Bytes app_data;  // the advert's own bytes, untouched. The name in it is a stranger's: never logged
};

// The most observations one payload can hold: the smallest record is 45 bytes (a 32-byte key, two
// u32, an i8, an i16, `path_len` and `app_len`, with no path and no app data) and the response's
// own head is 5 (status, remaining, dropped, count).
constexpr std::size_t kObsPullHeadSize = 5;
constexpr std::size_t kMinObservationSize = 45;
constexpr std::size_t kMaxObservations = (kMaxPayload - kObsPullHeadSize) / kMinObservationSize;
static_assert(kMaxObservations == 5, "§7: five of the smallest records and no more fit a payload");

// ---------------------------------------------------------------------------- requests

// §4: the requests whose payload is empty - the type is the whole message. Distinct types so that
// a caller cannot send one where another was meant, and so that `kType` is always where the frame
// writer looks for it.
template <std::uint8_t Type>
struct NoPayload {
  static constexpr std::uint8_t kType = Type;
};

using StatusRequest = NoPayload<type::kStatus>;              // §5.3
using TimeGetRequest = NoPayload<type::kTimeGet>;            // §5.4
using IdentityRequest = NoPayload<type::kIdentity>;          // §5.6
using PowerGetRequest = NoPayload<type::kPowerGet>;          // §5.8
using EnvGetRequest = NoPayload<type::kEnvGet>;              // §5.8
using JobEndRequest = NoPayload<type::kJobEnd>;              // §6.1
using PathGetRequest = NoPayload<type::kPathGet>;            // §6.1
using ReplyGetRequest = NoPayload<type::kReplyGet>;          // §6.3
using MeshCountersRequest = NoPayload<type::kMeshCounters>;  // §6.4

template <std::uint8_t Type>
Encoded encode(const NoPayload<Type>&, std::uint8_t* out, std::size_t out_size) {
  (void)out;
  (void)out_size;
  return Encoded{true, 0};
}

// §5.1. The first request of a session, repeatable at any time.
struct HelloRequest {
  static constexpr std::uint8_t kType = type::kHello;
  std::uint8_t proto_major = kProtoMajor;
  std::uint8_t proto_minor = kProtoMinor;
  std::uint8_t peer_kind = peer_kind::kWalter;
  std::uint8_t max_payload = static_cast<std::uint8_t>(kMaxPayload);
  Text fw_version;  // the sender's own version string
};

// §5.2. The supervisor's heartbeat: it proves the main loop is turning, which a reset line cannot.
struct PingRequest {
  static constexpr std::uint8_t kType = type::kPing;
  std::uint32_t nonce = 0;
};

// §5.4. The Walter sets the clock after every restart it sees, before anything else.
struct TimeSetRequest {
  static constexpr std::uint8_t kType = type::kTimeSet;
  std::uint32_t epoch = 0;
  std::uint8_t source = time_source::kCellular;
  std::uint8_t flags = 0;  // time_flag::kForce, only on a person's instruction (§5.4)
};

// §5.5. The response is sent first; the restart follows after the delay.
struct RebootRequest {
  static constexpr std::uint8_t kType = type::kReboot;
  std::uint8_t mode = reboot_mode::kApplication;
  std::uint16_t delay_ms = 0;
};

// §8. The Walter's last request before it sleeps.
struct SleepNoticeRequest {
  static constexpr std::uint8_t kType = type::kSleepNotice;
  std::uint32_t expect_back_s = 0;  // 0 = unknown
};

// §5.7. One command line, run by the radio's own interpreter as if typed on its console. 1 to 160
// bytes; an encoder refuses anything else, because the radio would answer ERR_MALFORMED.
struct CliRequest {
  static constexpr std::uint8_t kType = type::kCli;
  Text text;
};

// §5.8.
struct LppGetRequest {
  static constexpr std::uint8_t kType = type::kLppGet;
  std::uint8_t perm_mask = 0;  // perm::kBase | perm::kLocation | perm::kEnvironment
};

// §6.1. Ends any job in progress first, then makes this key the peer.
struct JobBeginRequest {
  static constexpr std::uint8_t kType = type::kJobBegin;
  Bytes pubkey;  // 32 bytes
  Path path;
};

// §6.1. Replaces the job's path; mode kNone forgets it.
struct PathSetRequest {
  static constexpr std::uint8_t kType = type::kPathSet;
  Path path;
};

// §6.2, the five sends. Each begins with how long the radio waits for the reply; 0 awaits nothing.
struct SendLoginRequest {
  static constexpr std::uint8_t kType = type::kSendLogin;
  std::uint32_t timeout_ms = 0;
  // 0 to 15 bytes. With none it is the ACL login: the node's own key and nothing else. It is in
  // RAM for the length of the send and wiped from every buffer afterwards (§6) - never logged.
  Bytes password;
};

struct SendRequestRequest {
  static constexpr std::uint8_t kType = type::kSendRequest;
  std::uint32_t timeout_ms = 0;
  std::uint8_t req_type = 0;
  Bytes data;
};

struct SendCliRequest {
  static constexpr std::uint8_t kType = type::kSendCli;
  std::uint32_t timeout_ms = 0;
  std::uint8_t flags = 0;  // send_cli_flag::kSensitive when the line was made from a password
  Text text;               // 1 to 148 bytes
};

struct SendDiscoveryRequest {
  static constexpr std::uint8_t kType = type::kSendDiscovery;
  std::uint32_t timeout_ms = 0;
};

struct SendTraceRequest {
  static constexpr std::uint8_t kType = type::kSendTrace;
  std::uint32_t timeout_ms = 0;
  std::uint8_t hash_size = 0;  // 1, 2, 4 or 8; three-byte hashes a trace cannot express
  Bytes hashes;                // the round-trip path
};

// §7. The most records wanted; the radio returns as many as fit in one payload.
struct ObsPullRequest {
  static constexpr std::uint8_t kType = type::kObsPull;
  std::uint8_t max = 0;
};

Encoded encode(const HelloRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const PingRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const TimeSetRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const RebootRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const SleepNoticeRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const CliRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const LppGetRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const JobBeginRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const PathSetRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const SendLoginRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const SendRequestRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const SendCliRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const SendDiscoveryRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const SendTraceRequest& request, std::uint8_t* out, std::size_t out_size);
Encoded encode(const ObsPullRequest& request, std::uint8_t* out, std::size_t out_size);

// ---------------------------------------------------------------------------- responses

// §3. A response whose status is not OK is that one byte and nothing else - and for five request
// types it is the whole response whatever the status is. The same struct decodes the answer to a
// request of a type the radio does not know (ERR_UNSUPPORTED with that type's response code),
// which is how a newer Walter discovers what an older radio cannot do.
struct StatusOnly {
  std::uint8_t status = status::kOk;
};

using RebootResponse = StatusOnly;        // §5.5
using SleepNoticeResponse = StatusOnly;   // §8
using JobBeginResponse = StatusOnly;      // §6.1
using JobEndResponse = StatusOnly;        // §6.1
using PathSetResponse = StatusOnly;       // §6.1

struct HelloResponse {  // §5.1
  std::uint8_t status = status::kOk;
  HelloBody body;
};

struct PingResponse {  // §5.2
  std::uint8_t status = status::kOk;
  std::uint32_t nonce = 0;  // echoed
  std::uint32_t uptime_s = 0;
};

struct StatusResponse {  // §5.3
  std::uint8_t status = status::kOk;
  std::uint32_t uptime_s = 0;
  std::uint8_t reset_reason = reset_reason::kUnknown;
  std::uint8_t time_state = time_state::kUnset;
  std::uint32_t epoch = 0;
  std::uint16_t flags = 0;
  std::uint8_t obs_pending = 0;
  std::uint16_t obs_dropped = 0;  // since boot, saturating
  std::uint16_t link_errors = 0;  // frames the radio's receiver dropped since boot, saturating
  std::uint8_t attention = attention::kNone;
};

struct TimeSetResponse {  // §5.4
  std::uint8_t status = status::kOk;
  std::uint32_t prev_epoch = 0;
  std::uint32_t epoch = 0;
  std::uint8_t time_state = time_state::kUnset;
};

struct TimeGetResponse {  // §5.4
  std::uint8_t status = status::kOk;
  std::uint32_t epoch = 0;
  std::uint8_t time_state = time_state::kUnset;
};

struct IdentityResponse {  // §5.6
  std::uint8_t status = status::kOk;
  Bytes pubkey;  // 32 bytes: the node's MeshCore public key
  Text name;     // its advert name
};

struct CliResponse {  // §5.7
  std::uint8_t status = status::kOk;
  Text text;  // the reply, 0 to 160 bytes
};

struct PowerChannel {  // §5.8: bus voltage and current, the board's shunt already applied
  std::uint16_t bus_mv = 0;
  std::int16_t current_ma = 0;
};

struct PowerGetResponse {  // §5.8
  std::uint8_t status = status::kOk;
  std::uint8_t valid = 0;  // bit 0 channel 1, bit 1 channel 2, bit 2 channel 3
  // 1 = panel (positive into the charger), 2 = battery (positive charging), 3 = total load.
  // A channel whose bit is clear reads 0.
  PowerChannel channels[3] = {};
};

struct EnvGetResponse {  // §5.8
  std::uint8_t status = status::kOk;
  std::uint8_t valid = 0;  // bit 0 temperature, bit 1 humidity, bit 2 pressure
  std::int16_t temp_centi = 0;
  std::uint16_t rh_centi = 0;
  std::uint32_t pressure_pa = 0;
};

struct LppGetResponse {  // §5.8
  std::uint8_t status = status::kOk;
  Bytes lpp;  // the Cayenne-LPP bytes a mesh client would have been told at those permissions
};

struct PathGetResponse {  // §6.1: the path the job holds now, a learned one included
  std::uint8_t status = status::kOk;
  Path path;
};

struct SendResponse {  // §6.2, the answer to all five sends
  std::uint8_t status = status::kOk;
  std::uint8_t send = send_result::kNothingSent;
  std::uint32_t tag = 0;        // what the reply will be matched by; 0 = the next reply from the peer
  std::uint32_t sender_ts = 0;  // the timestamp the radio put in the packet
};

struct ReplyGetResponse {  // §6.3
  std::uint8_t status = status::kOk;
  ReplyRecord reply;
};

struct ObsPullResponse {  // §7
  std::uint8_t status = status::kOk;
  std::uint8_t remaining = 0;  // still in the ring after this answer
  std::uint16_t dropped = 0;   // lost since the last OBS_PULL
  std::uint8_t count = 0;
  Observation records[kMaxObservations] = {};  // oldest first
};

struct MeshCountersResponse {  // §6.4
  std::uint8_t status = status::kOk;
  std::uint32_t rx = 0;
  std::uint32_t tx = 0;         // every packet this node transmitted, forwards included
  std::uint32_t client_tx = 0;  // one per send of the mesh client, and nothing else
  std::uint32_t adverts = 0;    // adverts that verified
  std::uint32_t adverts_dropped = 0;
  std::uint32_t obs_dropped = 0;
  std::int16_t noise_floor = 0;  // dBm
  std::int16_t last_rssi = 0;
  std::int8_t last_snr_q4 = 0;
};

bool decode(Bytes payload, StatusOnly* out);
bool decode(Bytes payload, HelloResponse* out);
bool decode(Bytes payload, PingResponse* out);
bool decode(Bytes payload, StatusResponse* out);
bool decode(Bytes payload, TimeSetResponse* out);
bool decode(Bytes payload, TimeGetResponse* out);
bool decode(Bytes payload, IdentityResponse* out);
bool decode(Bytes payload, CliResponse* out);
bool decode(Bytes payload, PowerGetResponse* out);
bool decode(Bytes payload, EnvGetResponse* out);
bool decode(Bytes payload, LppGetResponse* out);
bool decode(Bytes payload, PathGetResponse* out);
bool decode(Bytes payload, SendResponse* out);
bool decode(Bytes payload, ReplyGetResponse* out);
bool decode(Bytes payload, ObsPullResponse* out);
bool decode(Bytes payload, MeshCountersResponse* out);

// ---------------------------------------------------------------------------- events

// §5.1. Sent once when the radio's link comes up after a reset: the reset reason and the HELLO
// response's body, without a status byte. It saves a round trip and tells a sleeping Walter that
// the radio restarted - but the Walter does not rely on it; it sends HELLO when it wants to know.
struct EvBoot {
  static constexpr std::uint8_t kType = type::kEvBoot;
  std::uint8_t reset_reason = reset_reason::kUnknown;
  HelloBody body;
};

// §6.3. One of these when an awaited reply arrives or times out. A lost one costs one REPLY_GET,
// never a lost reply.
struct EvMeshReply {
  static constexpr std::uint8_t kType = type::kEvMeshReply;
  ReplyRecord reply;
};

// §8. The radio wakes the Walter with 0x00 0x00, waits 300 ms, and sends this - then every second,
// `repeat` counting from 0, until a valid request arrives or it has sent it ten times.
struct EvAttention {
  static constexpr std::uint8_t kType = type::kEvAttention;
  std::uint8_t reason = attention::kNone;
  std::uint16_t detail = 0;
  std::uint8_t repeat = 0;
};

bool decode(Bytes payload, EvBoot* out);
bool decode(Bytes payload, EvMeshReply* out);
bool decode(Bytes payload, EvAttention* out);

}  // namespace link
}  // namespace core
