// The box link's numbers, protocol 1.0: spec/LINK-PROTOCOL.md §2-§8 as one header. Nothing here
// is a decision - every constant is a cell of one of that document's tables, with the section it
// comes from beside it, so that a reader can hold the two side by side. spec/tools/linkref.py is
// the same numbers as Python and the fixtures it generates are what the tests check them against.
//
// Raw constants in small namespaces rather than enum classes: these are bytes on a wire that are
// compared, masked and passed through. A `status` this firmware does not know still has to be
// reported, a `caps` bit it does not know still has to be ignored (§5.1), and an enum class with
// a value outside its enumerators is not something C++ promises anything about.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace link {

// The version this firmware speaks (§5.1). A RAK that does not speak this major answers
// ERR_UNSUPPORTED; within a major, a minor only adds things.
constexpr std::uint8_t kProtoMajor = 1;
constexpr std::uint8_t kProtoMinor = 0;

// §2, the shape of a packet and of a frame.
//
//   type : u8 | seq : u8 | payload : 0..240 | crc : u16      4 to 244 bytes
//
// COBS removes every zero from the packet and adds exactly one byte (no run inside a packet
// reaches 254 bytes), and one 0x00 ends the frame. A receive buffer needs 246 bytes.
constexpr std::size_t kMaxPayload = 240;
constexpr std::size_t kMinPacket = 4;                      // type, seq, crc16, no payload
constexpr std::size_t kMaxPacket = kMaxPayload + 4;         // 244
constexpr std::size_t kMaxEncoded = kMaxPacket + 1;         // 245: COBS' one byte of overhead
constexpr std::size_t kMaxFrame = kMaxEncoded + 1;          // 246: with the terminating 0x00
constexpr std::size_t kReceiveBuffer = 246;                 // §2, "A receive buffer needs 246 bytes"
constexpr std::size_t kMaxWrite = kMaxFrame + 1;            // 247: with the idle 0x00 before it

// §2: the sender prefixes one 0x00 when the line has been idle this long. An empty frame is not
// an error, and it ends whatever garbage the receiver may hold.
constexpr std::uint32_t kIdleMs = 100;

// §10, the timing summary. Not used by the codec - the client (task 02) and the supervisor own
// these - but they belong beside the numbers they come from.
constexpr std::uint32_t kAnswerWithinMs = 250;    // the RAK answers a request within this
constexpr std::uint32_t kResponseWaitMs = 1000;   // the Walter waits this long, then resends
constexpr std::uint8_t kRequestAttempts = 3;      // and gives up after three silences
constexpr std::uint32_t kJobIdleEndMs = 120000;   // the RAK ends a job after this much link silence
constexpr std::uint32_t kAttentionRepeatMs = 1000;
constexpr std::uint8_t kAttentionRepeats = 10;

// §3, the type ranges and the response bit. A response is the request's type with bit 7 set and
// the request's seq.
constexpr std::uint8_t kResponseBit = 0x80;
constexpr std::uint8_t kFirstRequest = 0x01, kLastRequest = 0x5F;
constexpr std::uint8_t kFirstEvent = 0x60, kLastEvent = 0x7F;
constexpr std::uint8_t kFirstResponse = 0x81, kLastResponse = 0xDF;

constexpr bool isRequest(std::uint8_t wire_type) {
  return wire_type >= kFirstRequest && wire_type <= kLastRequest;
}
constexpr bool isEvent(std::uint8_t wire_type) {
  return wire_type >= kFirstEvent && wire_type <= kLastEvent;
}
constexpr bool isResponse(std::uint8_t wire_type) {
  return wire_type >= kFirstResponse && wire_type <= kLastResponse;
}
// The type that answers a request, and the request a response answers.
constexpr std::uint8_t responseTypeOf(std::uint8_t request_type) {
  return static_cast<std::uint8_t>(request_type | kResponseBit);
}
constexpr std::uint8_t requestTypeOf(std::uint8_t response_type) {
  return static_cast<std::uint8_t>(response_type & ~kResponseBit);
}

// §4, the message table. The capability a type needs is in the comment: a RAK without it answers
// ERR_UNSUPPORTED, which is how a newer Walter discovers what an older RAK cannot do (§3).
namespace type {

constexpr std::uint8_t kHello = 0x01;         // always        §5.1
constexpr std::uint8_t kPing = 0x02;          // always        §5.2
constexpr std::uint8_t kStatus = 0x03;        // always        §5.3
constexpr std::uint8_t kTimeSet = 0x04;       // always        §5.4
constexpr std::uint8_t kTimeGet = 0x05;       // always        §5.4
constexpr std::uint8_t kReboot = 0x06;        // always        §5.5
constexpr std::uint8_t kIdentity = 0x07;      // always        §5.6
constexpr std::uint8_t kSleepNotice = 0x08;   // always        §8
constexpr std::uint8_t kCli = 0x09;           // CLI           §5.7
constexpr std::uint8_t kPowerGet = 0x0A;      // POWER         §5.8
constexpr std::uint8_t kEnvGet = 0x0B;        // ENV           §5.8
constexpr std::uint8_t kLppGet = 0x0C;        // CLI           §5.8
// 0x10-0x1F reserved: provisioning
constexpr std::uint8_t kJobBegin = 0x20;      // MESH_CLIENT   §6.1
constexpr std::uint8_t kJobEnd = 0x21;        // MESH_CLIENT   §6.1
constexpr std::uint8_t kPathSet = 0x22;       // MESH_CLIENT   §6.1
constexpr std::uint8_t kPathGet = 0x23;       // MESH_CLIENT   §6.1
constexpr std::uint8_t kSendLogin = 0x24;     // MESH_CLIENT   §6.2
constexpr std::uint8_t kSendRequest = 0x25;   // MESH_CLIENT   §6.2
constexpr std::uint8_t kSendCli = 0x26;       // MESH_CLIENT   §6.2
constexpr std::uint8_t kSendDiscovery = 0x27; // MESH_CLIENT   §6.2
constexpr std::uint8_t kSendTrace = 0x28;     // MESH_CLIENT   §6.2
constexpr std::uint8_t kReplyGet = 0x29;      // MESH_CLIENT   §6.3
constexpr std::uint8_t kObsPull = 0x2A;       // OBSERVATIONS  §7
constexpr std::uint8_t kMeshCounters = 0x2B;  // MESH_CLIENT   §6.4
// 0x30-0x3F reserved: the bridge role
constexpr std::uint8_t kEvBoot = 0x60;        // always        §5.1
constexpr std::uint8_t kEvMeshReply = 0x61;   // MESH_CLIENT   §6.3
constexpr std::uint8_t kEvAttention = 0x62;   // ATTENTION     §8

}  // namespace type

// §3, the first byte of every response. When it is not kOk the payload is that one byte and
// nothing else.
namespace status {

constexpr std::uint8_t kOk = 0;              // done; the body follows
constexpr std::uint8_t kUnsupported = 1;     // the type is unknown, or the capability is missing
constexpr std::uint8_t kMalformed = 2;       // too short, or a field is not a legal value
constexpr std::uint8_t kBusy = 3;            // a mesh exchange is still waiting for its reply
constexpr std::uint8_t kState = 4;           // not possible now (no job begun, radio not up)
constexpr std::uint8_t kRange = 5;           // a value is outside what is accepted
constexpr std::uint8_t kFailed = 6;          // tried and failed (no packet buffer, I2C error)
constexpr std::uint8_t kDenied = 7;          // this role refuses it

}  // namespace status

// §5.1, the HELLO response's `role`.
namespace role {
constexpr std::uint8_t kUnknown = 0, kRepeater = 1, kBridge = 2, kScanner = 3;
}

// §5.1, the HELLO request's `peer_kind`.
namespace peer_kind {
constexpr std::uint8_t kWalter = 1, kBenchTool = 2;
}

// §5.1, `caps`. Unknown bits are ignored.
namespace cap {
constexpr std::uint32_t kCli = 0x01;
constexpr std::uint32_t kPower = 0x02;
constexpr std::uint32_t kEnv = 0x04;
constexpr std::uint32_t kMeshClient = 0x08;
constexpr std::uint32_t kObservations = 0x10;
constexpr std::uint32_t kAttention = 0x20;
constexpr std::uint32_t kTimePersist = 0x40;
}  // namespace cap

// §5.3, the RAK's `reset_reason` - also the first byte of EV_BOOT (§5.1) and the `detail` of an
// attention of reason 1 (§8). These are the radio's codes, not the Walter's own
// (core/health/health.h).
namespace reset_reason {
constexpr std::uint8_t kUnknown = 0, kPowerOn = 1, kResetPin = 2, kWatchdog = 3, kSoftware = 4,
                       kLockup = 5, kWakeFromSystemOff = 6, kLeftBootloader = 7;
}

// §5.3, the STATUS response's `flags`.
namespace status_flag {
constexpr std::uint16_t kJobBegun = 1u << 0;
constexpr std::uint16_t kReplyAwaited = 1u << 1;
constexpr std::uint16_t kWalterAsleep = 1u << 2;
constexpr std::uint16_t kAdvertsHeld = 1u << 3;  // held back until the clock is set
constexpr std::uint16_t kRadioUp = 1u << 4;
}  // namespace status_flag

// §5.4, `time_state`.
namespace time_state {
constexpr std::uint8_t kUnset = 0;     // the firmware's built-in default; never right
constexpr std::uint8_t kRestored = 1;  // carried over a reset: never earlier than what it has sent
constexpr std::uint8_t kSet = 2;       // set over this link since boot
constexpr std::uint8_t kRtc = 3;       // read from a hardware RTC
}  // namespace time_state

// §5.4, TIME_SET's `source` and `flags`.
namespace time_source {
constexpr std::uint8_t kCellular = 1, kServer = 2, kConsole = 3;
}
namespace time_flag {
// Force moves the clock backwards, which makes a MeshCore node's own past look like a replay to
// everyone who heard it. The Walter sets it only on a person's instruction (§5.4).
constexpr std::uint8_t kForce = 1u << 0;
}

// §5.5, REBOOT's `mode`. Modes 1-3 hand the radio to a bootloader this link cannot talk to; they
// exist for a person at the field USB port. The Walter's own way to replace firmware is SWD.
namespace reboot_mode {
constexpr std::uint8_t kApplication = 0, kUf2Bootloader = 1, kSerialDfu = 2, kOtaDfu = 3;
}

// §5.8, LPP_GET's `perm_mask`: MeshCore's telemetry permission bits.
namespace perm {
constexpr std::uint8_t kBase = 0x01, kLocation = 0x02, kEnvironment = 0x04;
}

// §6.1, a path's `path_mode`. This one selects the layout of what follows, so a decoder refuses a
// value it does not know rather than guessing (codec/messages.h).
namespace path_mode {
constexpr std::uint8_t kNone = 0;    // the next send floods
constexpr std::uint8_t kDirect = 1;  // zero hops, sent direct with an empty path
constexpr std::uint8_t kHops = 2;    // hash_size x hop_count bytes follow
}  // namespace path_mode

// §6.1: 1 to 3 bytes per hop, 1 to 63 hops, at most 64 bytes of hashes.
constexpr std::uint8_t kMinHashSize = 1, kMaxHashSize = 3;
constexpr std::uint8_t kMinHopCount = 1, kMaxHopCount = 63;
constexpr std::size_t kMaxPathBytes = 64;

// §6.2, a send response's `send`.
namespace send_result {
constexpr std::uint8_t kNothingSent = 0, kFlood = 1, kDirect = 2;
}

// §6.3, a reply record's `state`. Like path_mode this selects the layout of the rest.
namespace reply_state {
constexpr std::uint8_t kNone = 0;      // nothing is held
constexpr std::uint8_t kPending = 1;   // still waiting
constexpr std::uint8_t kTimeout = 2;   // the time was up
constexpr std::uint8_t kResponse = 3;  // `data`: the response after its four tag bytes
constexpr std::uint8_t kText = 4;      // `text`: the CLI reply
constexpr std::uint8_t kTrace = 5;     // a completed trace
}  // namespace reply_state

// §6.3 and §7: the RSSI a node did not measure.
constexpr std::int16_t kRssiUnknown = 0x7FFF;

// §6.2, SEND_TRACE's `hash_size`: a trace cannot express three-byte hashes (ERR_RANGE).
constexpr bool isTraceHashSize(std::uint8_t hash_size) {
  return hash_size == 1 || hash_size == 2 || hash_size == 4 || hash_size == 8;
}

// §5.7 and §6.2, the lengths of the two text fields that have one.
constexpr std::size_t kMinCliLine = 1, kMaxCliLine = 160;    // CLI request
constexpr std::size_t kMaxCliReply = 160;                     // CLI response
constexpr std::size_t kMaxMeshCliText = 148;                  // SEND_CLI's text
constexpr std::size_t kMaxPassword = 15;                       // SEND_LOGIN's password

// §6.2, SEND_CLI's `flags`.
namespace send_cli_flag {
// Sensitive: the line was made from a password. It is wiped from every buffer as soon as the
// packet is built, never stored and never printed (§6).
constexpr std::uint8_t kSensitive = 1u << 0;
}

// §7, an observation record's `path_len`: MeshCore's encoded path length.
constexpr std::uint8_t pathHashSize(std::uint8_t path_len) {
  return static_cast<std::uint8_t>((path_len >> 6) + 1);
}
constexpr std::uint8_t pathHopCount(std::uint8_t path_len) {
  return static_cast<std::uint8_t>(path_len & 0x3F);
}
constexpr std::size_t kPubkeySize = 32;
constexpr std::size_t kMaxAppData = 32;  // §7, `app_len` is 0..32

// §8, EV_ATTENTION's `reason`, with what `detail` then carries.
namespace attention {
constexpr std::uint8_t kNone = 0;              // STATUS: no attention is pending
constexpr std::uint8_t kRestarted = 1;         // detail: the reset reason
constexpr std::uint8_t kBatteryLow = 2;        // detail: battery millivolts
constexpr std::uint8_t kBatteryCritical = 3;   // detail: battery millivolts
constexpr std::uint8_t kPersonAsked = 4;       // detail: 0
constexpr std::uint8_t kObsRingNearlyFull = 5; // detail: records waiting
constexpr std::uint8_t kFault = 6;             // detail: a code of the radio firmware's own
}  // namespace attention

}  // namespace link
}  // namespace core
