// The link client: spec/LINK-PROTOCOL.md as calls instead of bytes (task p1-02). The codec
// (core/link/codec/) is the protocol as bytes and knows nothing of a UART; this knows about the
// UART, the clock and one request at a time, and nothing about what a payload looks like.
//
// What it is, in the order §3, §5 and §9 ask for it:
//
//   - **one request at a time.** A `seq` different from the last one, the response awaited
//     kResponseWaitMs, the same frame and the same `seq` sent again at most twice, then *link
//     down* - as a result, not an action (acting is the supervisor's, phase 2). A response with
//     another type or `seq` is counted and ignored, and so is one that arrives with no request in
//     flight (a late answer to an attempt that had already been resent).
//   - **typed calls.** One per request type of §4, each returning the decoded response or the
//     reason there is none (Fault). A call whose capability HELLO did not advertise fails without
//     a byte on the wire.
//   - **the session.** What HELLO learned: the version agreed, the role, the slot, the
//     capabilities, the versions and `boot_id`. A changed `boot_id` or an EV_BOOT means the radio
//     restarted - every piece of per-boot state is gone (§5.1) - and the client's first act after
//     it is TIME_SET, when interfaces::WallClock has a time to give (§5.4).
//   - **events.** EV_BOOT, EV_MESH_REPLY and EV_ATTENTION, collected whenever the client reads
//     the port - inside a response wait, or from poll() - into a small bounded queue whose
//     overflow is counted, with their own sequence numbers watched for gaps (§3).
//   - **which firmware is it** (§9). No answer to three HELLOs, and lines beginning with `{`, mean
//     the radio is running NoiseScope: the client says *scanner (text)*, sends no more frames, and
//     offers the lines to whoever subscribed. A reset of the radio puts it back to "unknown, try
//     HELLO". Frames and text are never mixed in one session.
//
// **A decoded answer's variable-length fields are views** into this object's own response buffer
// (codec/view.h), valid until the next call on this client. Whoever keeps one copies it; the
// session copies the three strings of HELLO itself, which is why it has lengths of its own.
//
// Nothing here blocks longer than one response timeout, and it listens in slices of kPollMs so
// that the task it runs in is never away for longer than a tick. It is logic: no ESP-IDF include,
// no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/client/port.h"
#include "core/link/codec/frame.h"
#include "core/link/codec/messages.h"
#include "core/link/codec/view.h"
#include "core/link/codec/wire.h"
#include "interfaces/clock.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace link {
namespace client {

// How long the client waits inside one read of the port before looking at the clock again. Small
// enough that nothing is blocked for longer than a FreeRTOS tick, large enough that a 1000 ms wait
// is a hundred reads and not a hundred thousand.
constexpr std::uint32_t kPollMs = 10;

// §9: how long the client listens for a line beginning with `{` after the `info\n` probe. The
// scanner answers a command with one line at once (the hardware repository's
// software/firmware/noisescope-rak4630/PROTOCOL.md, "Commands": every command is answered with an
// `ack` line), so this is the same second a response gets.
constexpr std::uint32_t kProbeWaitMs = kResponseWaitMs;

// The event queue. Small on purpose: §3 says an event is never the only way to learn something -
// every event's content can also be fetched with a request - so a queue that overflows costs a
// request, and a Walter that sleeps through fifty attentions wants the first four, not the last.
constexpr std::size_t kEventQueue = 4;

// A scanner line that is kept whole (§9 and NoiseScope's PROTOCOL.md: a `stat` line is about
// 300 bytes, a `sweep` line about 2.5 kB). A longer line is offered and kept truncated, and
// counted - forwarding them verbatim is the uplink's business, not this task's.
constexpr std::size_t kTextLineMax = 320;
constexpr std::size_t kTextRing = 3;

// A str8 of the HELLO body, copied into the session. §5.1's examples are 13, 14 and 25 bytes
// (`0.3.0+abc1234`, `rak4630-single`, `repeater-v1.17.1+d9296435`); a longer one is kept
// truncated and counted rather than refused, because a version string is not a protocol error.
constexpr std::size_t kVersionMax = 48;

// Why a call has no decoded answer. kNone is the only one that means there is one.
enum class Fault : std::uint8_t {
  kNone,         // the response is there and decoded
  kClosed,       // the UART is not ours: the rail is off, or a kill took the pins
  kTextMode,     // the radio is running the scanner (§9): no frame is sent any more
  kNoSession,    // the call needs a capability and no HELLO has been answered yet
  kUnsupported,  // HELLO did not advertise the capability: nothing was sent (§4, §5.1)
  kEncode,       // this end could not build the request (a field longer than §5 allows)
  kLinkDown,     // three attempts, no valid response (§3, §9)
  kRefused,      // the radio answered with a status that is not OK; `status` says which
  kMalformed,    // a response arrived whose CRC passed and whose payload did not decode
};

// What this client believes it is talking to (§9). Never two of these at once.
enum class Mode : std::uint8_t {
  kUnknown,  // nothing has answered yet, or the radio was reset: try HELLO
  kFrames,   // a HELLO was answered: protocol 1.0, frames
  kText,     // NoiseScope: NDJSON lines, no frames in either direction
};

const char* describe(Fault fault);
const char* describe(Mode mode);

// What HELLO learned, copied out of the response so that it outlives the receiver's buffer.
struct Session {
  bool up = false;  // a HELLO has been answered with status OK
  std::uint8_t proto_major = 0;
  std::uint8_t proto_minor = 0;  // the version the radio will speak in this session (§5.1)
  std::uint8_t role = role::kUnknown;
  std::uint8_t slot = 0;        // 0 on a single-radio unit; the position on a dual adapter (S-11)
  std::uint8_t max_payload = 0; // the largest payload the radio accepts
  std::uint32_t caps = 0;       // cap::* bits; unknown ones are kept and ignored
  std::uint32_t boot_id = 0;    // a change means the radio restarted
  char fw_version[kVersionMax] = {};
  char board[kVersionMax] = {};
  char upstream[kVersionMax] = {};
};

// The call's answer: the decoded message, or why there is none. `status` carries the radio's own
// byte when the fault is kRefused (§3's ERR_*), so a caller can tell "no job begun" from "this
// role refuses it" without a second call.
template <typename Response>
struct Answer {
  Fault fault = Fault::kClosed;
  std::uint8_t status = status::kOk;
  std::uint8_t attempts = 0;  // frames put on the wire for this call, 1 to kRequestAttempts
  Response message{};

  bool ok() const { return fault == Fault::kNone; }
};

// An event as it arrived: the payload's bytes, copied. Not the decoded struct, because a decoded
// EV_MESH_REPLY points into the buffer the next frame overwrites - and because the decoders of
// codec/messages.h are then the only place a payload becomes fields, queued or not.
struct QueuedEvent {
  std::uint8_t type = 0;  // type::kEvBoot, kEvMeshReply or kEvAttention
  std::uint8_t seq = 0;   // the radio's own event counter (§3), counted from 0 and wrapping
  std::uint8_t size = 0;
  std::uint8_t payload[kMaxPayload] = {};

  Bytes view() const { return Bytes{payload, size}; }
};

// Told when the radio restarted - a changed `boot_id` or an EV_BOOT (§5.1). Called from inside
// whatever call noticed, so an implementation records and returns: it must not talk on the link.
class RestartWatcher {
 public:
  virtual ~RestartWatcher() = default;
  // `reset_reason` is the radio's own code (§5.3); reset_reason::kUnknown when the restart was
  // noticed through a HELLO, which does not carry one.
  virtual void onRadioRestart(std::uint32_t boot_id, std::uint8_t reset_reason) = 0;
};

// Offered every line the scanner sends, as it arrives (§9). The line is a view, valid for the
// length of the call.
class TextWatcher {
 public:
  virtual ~TextWatcher() = default;
  virtual void onTextLine(Text line) = 0;
};

// Four of each: the supervisor, the cycle, the console and one spare. A fixed array, because
// core/ has no heap.
constexpr std::size_t kMaxWatchers = 4;

// What the client has seen. All saturating: a count that has stopped being exact must not start
// being wrong.
struct Counters {
  std::uint32_t requests = 0;        // exchanges begun: a call that got past the gate (§4)
  std::uint32_t frames_sent = 0;     // frames, retries included
  std::uint32_t retries = 0;         // frames that were a resend of the one before
  std::uint32_t timeouts = 0;        // attempts that got nothing within kResponseWaitMs
  std::uint32_t link_downs = 0;      // calls that ran out of attempts
  std::uint32_t responses = 0;       // responses taken as the answer to the request in flight
  std::uint32_t mismatched = 0;      // a response of another type or seq while waiting (§3)
  std::uint32_t stray = 0;           // a response with no request in flight (a late answer)
  std::uint32_t malformed = 0;       // a payload that would not decode
  std::uint32_t events = 0;          // events queued
  std::uint32_t events_dropped = 0;  // events the queue had no room for
  std::uint32_t event_gaps = 0;      // gaps in the radio's event seq (§3)
  std::uint32_t text_lines = 0;      // NDJSON lines seen (§9)
  std::uint32_t text_truncated = 0;  // of those, lines longer than kTextLineMax
  std::uint32_t text_dropped = 0;    // lines the ring had no room for
  std::uint32_t long_versions = 0;    // HELLO strings longer than kVersionMax, kept truncated
  std::uint32_t restarts = 0;        // restarts of the radio noticed
  std::uint32_t time_sets = 0;       // TIME_SETs the client sent of its own accord
};

class Client {
 public:
  Client(LinkPort& port, interfaces::Clock& clock, interfaces::WallClock& wall);

  // ------------------------------------------------------------------ the session and the state

  // §5.1. The first request of a session and repeatable at any time. On three silences it decides
  // what the radio is running (§9): the scanner when lines beginning with `{` were seen, or when
  // one answers the `info\n` probe; otherwise the link is down and the mode stays what it was.
  Answer<HelloResponse> hello();

  const Session& session() const { return session_; }
  Mode mode() const { return mode_; }
  // Whether the session advertised all of these bits (§5.1). False without a session.
  bool hasCapability(std::uint32_t bits) const;

  // §5.1 and §9: the radio was reset by somebody (a reset pulse, a kill, the console). Everything
  // the client believed about it is gone and the mode goes back to unknown - a reset is also what
  // ends a text session. It sends nothing.
  void radioWasReset();

  // §5.4: a restart was noticed and the radio's clock has not been set since. The next call sends
  // TIME_SET first, if interfaces::WallClock has a time by then.
  bool timePending() const { return time_pending_; }
  // Sends that TIME_SET now, if one is pending and there is a time to give. kNone when it was
  // sent and accepted; kNoSession when there is nothing to do or no time to give.
  Fault serviceTime();

  // Reads whatever is on the port without blocking: events are queued, scanner lines offered.
  // The firmware's main task calls it every time round (main/main.cpp); a response wait does the
  // same work inside the call. It never *opens* the port - taking a pin is not a side effect of
  // listening - so a Walter that wants to hear an EV_BOOT or an attention has the port open
  // already, which main/ does at start-up.
  void poll();

  // ------------------------------------------------------------------ §5, the local calls

  Answer<PingResponse> ping(std::uint32_t nonce);
  Answer<StatusResponse> status();
  Answer<TimeSetResponse> timeSet(std::uint32_t epoch, std::uint8_t source, bool force = false);
  Answer<TimeGetResponse> timeGet();
  Answer<StatusOnly> reboot(std::uint8_t mode, std::uint16_t delay_ms);
  Answer<IdentityResponse> identity();
  Answer<StatusOnly> sleepNotice(std::uint32_t expect_back_s);
  Answer<CliResponse> cli(Text line);
  Answer<PowerGetResponse> powerGet();
  Answer<EnvGetResponse> envGet();
  Answer<LppGetResponse> lppGet(std::uint8_t perm_mask);

  // ------------------------------------------------------------------ §6 and §7, the mesh client
  //
  // Thin: the codec does the work, and nothing in this firmware calls them until the agent exists
  // (spec/SYSTEM.md §8). They are here so that the whole protocol is exercised from this end.

  Answer<StatusOnly> jobBegin(Bytes pubkey, const Path& path);
  Answer<StatusOnly> jobEnd();
  Answer<StatusOnly> pathSet(const Path& path);
  Answer<PathGetResponse> pathGet();
  Answer<SendResponse> sendLogin(std::uint32_t timeout_ms, Bytes password);
  Answer<SendResponse> sendRequest(std::uint32_t timeout_ms, std::uint8_t req_type, Bytes data);
  Answer<SendResponse> sendCli(std::uint32_t timeout_ms, std::uint8_t flags, Text text);
  Answer<SendResponse> sendDiscovery(std::uint32_t timeout_ms);
  Answer<SendResponse> sendTrace(std::uint32_t timeout_ms, std::uint8_t hash_size, Bytes hashes);
  Answer<ReplyGetResponse> replyGet();
  Answer<ObsPullResponse> obsPull(std::uint8_t max);
  Answer<MeshCountersResponse> meshCounters();

  // ------------------------------------------------------------------ events

  std::size_t eventsQueued() const { return queued_; }
  // The oldest queued event, removed from the queue. False when there is none.
  bool takeEvent(QueuedEvent* out);
  void clearEvents() { queued_ = 0; }
  // The last event seq the radio sent, when one has been seen (§3: counted from 0, wrapping).
  bool lastEventSeq(std::uint8_t* out) const;

  // ------------------------------------------------------------------ §9, the scanner's text

  std::size_t textLines() const { return text_count_; }
  // One of the kept lines, oldest first. A view into this object, valid until the next call.
  Text textLine(std::size_t index) const;
  void clearText();

  // ------------------------------------------------------------------ the rest

  void subscribeRestart(RestartWatcher* watcher);
  void subscribeText(TextWatcher* watcher);
  const Counters& counters() const { return counters_; }
  const FrameReceiver& receiver() const { return receiver_; }
  LinkPort& port() { return port_; }

  // The version this end puts in its HELLO (§5.1, `fw_version`). core::version() at construction.
  Text firmwareVersion() const { return fw_version_; }

 private:
  // One request on the wire and the response to it, with §3's retries. The payload of the answer
  // is in response_ and is valid until the next exchange.
  struct Exchange {
    Fault fault = Fault::kClosed;
    std::uint8_t attempts = 0;
    Bytes payload;
  };

  Exchange exchange(std::uint8_t type, Bytes payload, bool sensitive);

  // encode -> exchange -> decode, which is every typed call. Declared here and defined in
  // client.cpp, where every instantiation of it is.
  template <typename Request, typename Response>
  Answer<Response> call(const Request& request, bool sensitive = false);

  // The capability gate (§4, §5.1): what to answer when the radio cannot do this, or when nothing
  // is known yet. kNone when the call may go out.
  Fault gate(std::uint8_t type) const;

  // Reads the port once and deals with what came: our response (copied into response_), an event
  // (copied into the queue), a scanner line (offered and kept), anything else counted. True when
  // the response to (type, seq) was among it.
  bool pump(bool expecting, std::uint8_t wire_type, std::uint8_t seq);
  void onPacket(const Packet& packet, bool expecting, std::uint8_t wire_type, std::uint8_t seq,
                bool* found);
  void onTextByte(std::uint8_t byte);
  void keepTextLine();
  void queueEvent(const Packet& packet);
  // `announced` is an EV_BOOT, which is news of a restart whether or not a boot_id was known;
  // a HELLO is news only when the boot_id changed.
  void noticeRestart(std::uint32_t boot_id, std::uint8_t reset_reason, bool announced);
  void sessionFromBody(const HelloBody& body);
  // §9's probe: `info\n` and one second of listening. True when a line beginning with `{` came.
  bool probeForScanner();
  std::uint8_t nextSeq();
  void wipeBuffers();

  LinkPort& port_;
  interfaces::Clock& clock_;
  interfaces::WallClock& wall_;

  FrameReceiver receiver_;
  FrameWriter writer_;

  Session session_;
  Mode mode_ = Mode::kUnknown;
  bool boot_id_known_ = false;
  bool time_pending_ = false;
  bool in_time_ = false;  // a TIME_SET of our own is on the wire: do not start another

  std::uint8_t seq_ = 0;

  std::uint8_t request_[kMaxPayload] = {};
  std::size_t request_size_ = 0;
  std::uint8_t frame_[kMaxWrite] = {};
  std::uint8_t response_[kMaxPayload] = {};
  std::size_t response_size_ = 0;
  std::uint8_t chunk_[64] = {};

  QueuedEvent events_[kEventQueue] = {};
  std::size_t queued_ = 0;
  bool event_seq_known_ = false;
  std::uint8_t event_seq_ = 0;

  char text_[kTextRing][kTextLineMax + 1] = {};
  std::size_t text_size_[kTextRing] = {};
  std::size_t text_count_ = 0;
  char line_[kTextLineMax + 1] = {};
  std::size_t line_size_ = 0;
  bool line_overflow_ = false;
  // The line count when this session began. §9 asks whether lines beginning with `{` were seen
  // *in this session*: a radio that was running the scanner, was reset, and now says nothing must
  // not be called a scanner on the strength of what the last session saw.
  std::uint32_t text_mark_ = 0;

  RestartWatcher* restart_watchers_[kMaxWatchers] = {};
  std::size_t restart_watcher_count_ = 0;
  TextWatcher* text_watchers_[kMaxWatchers] = {};
  std::size_t text_watcher_count_ = 0;

  Counters counters_;
  Text fw_version_;
};

}  // namespace client
}  // namespace link
}  // namespace core
