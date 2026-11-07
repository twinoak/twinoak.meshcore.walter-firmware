#include "core/link/client/client.h"

#include "core/version.h"

namespace core {
namespace link {
namespace client {

namespace {

// The codec needs nothing of <cstring> and neither does this: a bounded copy, written out.
void copyBytes(const void* from, void* to, std::size_t size) {
  const auto* in = static_cast<const std::uint8_t*>(from);
  auto* out = static_cast<std::uint8_t*>(to);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = in[i];
  }
}

void zeroBytes(void* at, std::size_t size) {
  auto* out = static_cast<volatile std::uint8_t*>(at);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = 0;
  }
}

// A str8 of the HELLO body into the session's own storage (§5.1). Longer than the room there is:
// as much as fits, terminated, and the caller counts it - a version string is not a protocol error.
bool copyText(Text from, char* to, std::size_t room) {
  if (room == 0) {
    return false;
  }
  const std::size_t n = from.size < room - 1 ? from.size : room - 1;
  for (std::size_t i = 0; i < n; ++i) {
    to[i] = from.data[i];
  }
  to[n] = 0;
  return n == from.size;
}

std::uint32_t saturate(std::uint32_t value) {
  return value == 0xFFFFFFFFu ? value : value + 1;
}

}  // namespace

const char* describe(Fault fault) {
  switch (fault) {
    case Fault::kNone: return "answered";
    case Fault::kClosed: return "the UART is not ours (the rail is off, or a kill took the pins)";
    case Fault::kTextMode: return "the radio is running the scanner: no frames are sent";
    case Fault::kNoSession: return "no session: say hello first";
    case Fault::kUnsupported: return "the radio does not advertise the capability";
    case Fault::kEncode: return "the request is longer than the protocol allows";
    case Fault::kLinkDown: return "no answer to three attempts: the link is down";
    case Fault::kRefused: return "refused";
    case Fault::kMalformed: return "the answer did not decode";
    case Fault::kSlotBusy: return "refused: that slot is not there, or another slot is in the middle of something";
  }
  return "?";
}

const char* describe(Mode mode) {
  switch (mode) {
    case Mode::kUnknown: return "unknown (nothing has answered yet)";
    case Mode::kFrames: return "frames (protocol 1.0)";
    case Mode::kText: return "scanner (text)";
  }
  return "?";
}

Client::Client(LinkPort& port, interfaces::Clock& clock, interfaces::WallClock& wall,
               board::SlotLock& slots)
    : port_(port),
      clock_(clock),
      wall_(wall),
      writer_(clock),
      slots_lock_(slots),
      fw_version_(text(core::version())) {}

// ---------------------------------------------------------------------------- the slot (p1-06)

// Refused rather than queued: a slot that is not there is a caller's mistake, and switching while
// an exchange holds the selection would steer the UART away from a frame that is on the wire.
bool Client::useSlot(board::Slot slot) {
  if (!slots_lock_.exists(slot)) return false;
  if (slot == slot_) return true;
  if (slots_lock_.held()) return false;
  slot_ = slot;
  return true;
}

// ---------------------------------------------------------------------------- the state

bool Client::hasCapability(std::uint32_t bits) const {
  return here().session.up && (here().session.caps & bits) == bits;
}

// The radio of the *current* slot (task p1-06): a reset pulse reaches the radio the wires are
// steered to, and on a dual adapter nRESET is routed like the debug lines
// (DUAL-RAK4630-FEASIBILITY.md §2.2), so one slot's reset is not the other's news. A kill is the
// exception - it cuts the whole rail (§5) - and the caller tells each slot in turn.
void Client::radioWasReset() {
  // §5.1: everything per-boot is gone. §9: a reset is also what ends a text session - the next
  // thing to try is HELLO, whatever the radio was running before.
  here().session = Session{};
  here().mode = Mode::kUnknown;
  here().boot_id_known = false;
  event_seq_known_ = false;
  receiver_.discardPartial();
  writer_.forgetIdle();
  line_size_ = 0;
  line_overflow_ = false;
  text_mark_ = counters_.text_lines;  // §9 is about the lines of *this* session
  // Whoever reset the radio may have taken the pins with it (a kill releases everything,
  // ARCHITECTURE §2 rule 3). Asking the port is how it finds out, and it then lets the stream go
  // too rather than keeping a peripheral on a pad it no longer owns.
  (void)port_.isOpen();
  // The queued events and the scanner lines are history and stay readable; they are what the
  // console shows after a reset. here().time_pending is not set here: §5.4 hangs the clock on an
  // EV_BOOT or a HELLO with an unseen boot_id, and the next HELLO brings one.
}

Fault Client::serviceTime() {
  if (!here().time_pending || in_time_) {
    return Fault::kNoSession;
  }
  interfaces::WallTime when;
  if (!wall_.now(&when)) {
    return Fault::kNoSession;  // nothing to give: phase 3 fills the interface
  }
  in_time_ = true;
  const Answer<TimeSetResponse> answer = timeSet(when.epoch, when.source);
  in_time_ = false;
  if (answer.ok()) {
    here().time_pending = false;
    counters_.time_sets = saturate(counters_.time_sets);
  } else if (answer.fault == Fault::kRefused) {
    // §5.4: the radio refuses an epoch earlier than its own clock without *force*. It said so;
    // asking again with the same time would get the same answer.
    here().time_pending = false;
    counters_.time_sets = saturate(counters_.time_sets);
  }
  return answer.fault;
}

// Listening is radio-facing too: on a dual adapter the bytes that arrive are the selected
// radio's, so poll() takes the slot before it reads - and does nothing at all when another slot's
// operation holds it, because then the UART is not this slot's to listen to. It never *opens* the
// port (task p1-02's decision) and it never waits.
void Client::poll() {
  const board::SlotHold hold(slots_lock_, slot_);
  if (!hold.ok()) return;
  pump(false, 0, 0);
}

// ---------------------------------------------------------------------------- §3, the exchange

std::uint8_t Client::nextSeq() {
  // §3: any value different from the previous request's.
  seq_ = static_cast<std::uint8_t>(seq_ + 1);
  return seq_;
}

void Client::wipeBuffers() {
  // §6: a password is in RAM for the length of the send and nowhere else. The exchange is over,
  // so the payload and the frame it was built into go now - both of them, whole.
  zeroBytes(request_, sizeof request_);
  zeroBytes(frame_, sizeof frame_);
  request_size_ = 0;
}


Client::Exchange Client::exchange(std::uint8_t type, Bytes payload, bool sensitive) {
  Exchange out;
  // The slot first, for the whole exchange: the wires are steered to this radio before a frame
  // leaves, and cannot be steered elsewhere until the response (or the third timeout) has come.
  // Nested inside hello()'s hold when that is the caller (task p1-06).
  const board::SlotHold hold(slots_lock_, slot_);
  if (!hold.ok()) {
    out.fault = Fault::kSlotBusy;
    return out;
  }
  if (here().mode == Mode::kText) {
    out.fault = Fault::kTextMode;
    return out;
  }
  if (!port_.isOpen() && port_.open() != board::Outcome::kOk) {
    out.fault = Fault::kClosed;
    return out;
  }
  const std::uint8_t seq = nextSeq();
  const std::uint8_t wire_type = responseTypeOf(type);
  zeroBytes(response_, sizeof response_);  // the last answer is nobody's any more
  response_size_ = 0;
  counters_.requests = saturate(counters_.requests);

  for (std::uint8_t attempt = 1; attempt <= kRequestAttempts; ++attempt) {
    // §3: the same frame, the same seq. The frame is rebuilt from the same bytes, so it is the
    // same frame; the one 0x00 in front of it is the sender's and belongs to the line's idleness,
    // not to the frame (§2) - after a second of waiting the line is idle, so a retry carries it.
    const std::size_t size = writer_.write(type, seq, payload, frame_, sizeof frame_);
    if (size == 0) {
      out.fault = Fault::kEncode;
      break;
    }
    if (attempt > 1) {
      counters_.retries = saturate(counters_.retries);
    }
    if (!port_.send(frame_, size)) {
      out.fault = Fault::kClosed;
      break;
    }
    counters_.frames_sent = saturate(counters_.frames_sent);
    out.attempts = attempt;

    // One response timeout, in slices of kPollMs: the longest this blocks is a tick.
    const std::uint64_t deadline = clock_.nowMs() + kResponseWaitMs;
    bool found = false;
    for (;;) {
      found = pump(true, wire_type, seq);
      if (found || !port_.isOpen()) {
        break;
      }
      if (clock_.nowMs() >= deadline) {
        break;
      }
      clock_.delayMs(kPollMs);
    }
    if (found) {
      counters_.responses = saturate(counters_.responses);
      out.fault = Fault::kNone;
      out.payload = Bytes{response_, response_size_};
      break;
    }
    if (!port_.isOpen()) {
      out.fault = Fault::kClosed;
      break;
    }
    counters_.timeouts = saturate(counters_.timeouts);
    out.fault = Fault::kLinkDown;
  }

  if (out.fault == Fault::kLinkDown) {
    counters_.link_downs = saturate(counters_.link_downs);
  }
  if (sensitive) {
    wipeBuffers();
  }
  return out;
}

Fault Client::gate(std::uint8_t type) const {
  if (here().mode == Mode::kText) {
    return Fault::kTextMode;  // §9: frames and text are never mixed in one session
  }
  const std::uint32_t needed = capabilityOf(type);
  if (needed == 0) {
    return Fault::kNone;  // §4: always
  }
  if (!here().session.up) {
    return Fault::kNoSession;  // the capabilities are what HELLO brings
  }
  return (here().session.caps & needed) == needed ? Fault::kNone : Fault::kUnsupported;
}

template <typename Request, typename Response>
Answer<Response> Client::call(const Request& request, bool sensitive) {
  Answer<Response> answer;
  constexpr std::uint8_t kType = Request::kType;
  answer.fault = gate(kType);
  if (answer.fault != Fault::kNone) {
    return answer;  // §5.1: nothing on the wire for a capability the radio does not have
  }
  // §5.4: the clock first, after a restart, before anything else this end asks for. Not before a
  // HELLO - which is how a restart is noticed in the first place - and not before the TIME_SET
  // that serviceTime() itself sends.
  if (kType != type::kHello && kType != type::kTimeSet) {
    serviceTime();
  }
  const Encoded encoded = encode(request, request_, sizeof request_);
  if (!encoded.ok) {
    answer.fault = Fault::kEncode;
    return answer;
  }
  request_size_ = encoded.size;
  const Exchange ex = exchange(kType, Bytes{request_, request_size_}, sensitive);
  answer.attempts = ex.attempts;
  answer.fault = ex.fault;
  if (ex.fault != Fault::kNone) {
    return answer;
  }
  if (!decode(ex.payload, &answer.message)) {
    counters_.malformed = saturate(counters_.malformed);
    answer.fault = Fault::kMalformed;
    return answer;
  }
  answer.status = answer.message.status;
  if (answer.status != status::kOk) {
    answer.fault = Fault::kRefused;
  }
  return answer;
}

// ---------------------------------------------------------------------------- reading the port

bool Client::pump(bool expecting, std::uint8_t wire_type, std::uint8_t seq) {
  bool found = false;
  for (;;) {
    const std::size_t have = port_.read(chunk_, sizeof chunk_);
    if (have == 0) {
      break;
    }
    for (std::size_t i = 0; i < have; ++i) {
      const std::uint8_t byte = chunk_[i];
      // §9: in a text session nothing is fed to the frame receiver, and in a frame session nothing
      // is read as text. Until a HELLO or a line has decided which it is, both are watched - that
      // is the only moment the two can be in the same buffer.
      if (here().mode != Mode::kFrames) {
        onTextByte(byte);
      }
      if (here().mode != Mode::kText && receiver_.push(byte) == FrameReceiver::Fed::kPacket) {
        onPacket(receiver_.packet(), expecting, wire_type, seq, &found);
        // The receive side (task p4-02): a reply the radio relayed can echo what was sent - a
        // repeater's `password now: ...` - and S-12 says a password is in RAM for the length of
        // the exchange and nowhere else. The packet is copied out (response_, or the queue), so
        // the receiver's buffers are zeroed; the chunk below the same once it has been fed; and
        // an event taken out of the queue leaves a zeroed slot behind.
        receiver_.wipe();
      }
    }
    zeroBytes(chunk_, sizeof chunk_);
    if (found) {
      break;  // the rest of what the port holds is read by the next pump
    }
  }
  return found;
}

void Client::onPacket(const Packet& packet, bool expecting, std::uint8_t wire_type,
                      std::uint8_t seq, bool* found) {
  // A frame whose CRC passed is proof enough of what the radio is running (§9).
  here().mode = Mode::kFrames;

  if (isEvent(packet.type)) {
    queueEvent(packet);
    return;
  }
  if (!isResponse(packet.type)) {
    // §3: in 1.0 only the Walter sends requests. Anything else is not ours to answer.
    counters_.mismatched = saturate(counters_.mismatched);
    return;
  }
  if (!expecting || *found) {
    // A response with nothing in flight: the late answer to an attempt that had already been
    // resent, or the remembered response to the retry (§3). Counted and ignored.
    counters_.stray = saturate(counters_.stray);
    return;
  }
  if (packet.type != wire_type || packet.seq != seq) {
    counters_.mismatched = saturate(counters_.mismatched);
    return;
  }
  // Copied out of the receiver's buffer at once: the next frame of this same chunk would
  // overwrite it, and the caller decodes after the chunk is done.
  response_size_ = packet.payload.size < sizeof response_ ? packet.payload.size : sizeof response_;
  copyBytes(packet.payload.data, response_, response_size_);
  *found = true;
}

void Client::queueEvent(const Packet& packet) {
  // §3: an event carries its own seq, counted by the radio from 0 and wrapping, so a gap is
  // visible. EV_BOOT is where the counter starts again, so it is never a gap.
  const bool is_boot = packet.type == type::kEvBoot;
  if (!is_boot && event_seq_known_ &&
      packet.seq != static_cast<std::uint8_t>(event_seq_ + 1)) {
    counters_.event_gaps = saturate(counters_.event_gaps);
  }
  event_seq_ = packet.seq;
  event_seq_known_ = true;

  // Decoded here to know it is well-formed, and - for EV_BOOT - to learn what it says. A payload
  // that does not decode is counted and dropped: nothing downstream could read it either.
  switch (packet.type) {
    case type::kEvBoot: {
      EvBoot event;
      if (!decode(packet.payload, &event)) {
        counters_.malformed = saturate(counters_.malformed);
        return;
      }
      // §5.1: EV_BOOT is the HELLO body with a reset reason in front of it, so the session can be
      // learned from it - and it always means a restart, even the first one this Walter sees.
      sessionFromBody(event.body);
      noticeRestart(event.body.boot_id, event.reset_reason, true);
      break;
    }
    case type::kEvMeshReply: {
      EvMeshReply event;
      if (!decode(packet.payload, &event)) {
        counters_.malformed = saturate(counters_.malformed);
        return;
      }
      break;
    }
    case type::kEvAttention: {
      EvAttention event;
      if (!decode(packet.payload, &event)) {
        counters_.malformed = saturate(counters_.malformed);
        return;
      }
      break;
    }
    default:
      // An event type of a later minor version (§5.1: a minor adds types). Queued as it came;
      // whoever knows it can decode it.
      break;
  }

  if (queued_ >= kEventQueue) {
    counters_.events_dropped = saturate(counters_.events_dropped);
    return;
  }
  QueuedEvent& slot = events_[queued_++];
  slot.type = packet.type;
  slot.seq = packet.seq;
  slot.size = static_cast<std::uint8_t>(packet.payload.size < kMaxPayload ? packet.payload.size
                                                                          : kMaxPayload);
  copyBytes(packet.payload.data, slot.payload, slot.size);
  counters_.events = saturate(counters_.events);
}

bool Client::takeEvent(QueuedEvent* out) {
  if (queued_ == 0 || out == nullptr) {
    return false;
  }
  *out = events_[0];
  for (std::size_t i = 1; i < queued_; ++i) {
    events_[i - 1] = events_[i];
  }
  --queued_;
  events_[queued_] = QueuedEvent{};  // the vacated slot holds nothing
  return true;
}

bool Client::takeEvent(std::uint8_t type, QueuedEvent* out) {
  if (out == nullptr) {
    return false;
  }
  for (std::size_t at = 0; at < queued_; ++at) {
    if (events_[at].type != type) {
      continue;
    }
    *out = events_[at];
    for (std::size_t i = at + 1; i < queued_; ++i) {
      events_[i - 1] = events_[i];
    }
    --queued_;
    events_[queued_] = QueuedEvent{};  // the vacated slot holds nothing
    return true;
  }
  return false;
}

bool Client::lastEventSeq(std::uint8_t* out) const {
  if (!event_seq_known_ || out == nullptr) {
    return false;
  }
  *out = event_seq_;
  return true;
}

void Client::noticeRestart(std::uint32_t boot_id, std::uint8_t reset_reason, bool announced) {
  const bool first = !here().boot_id_known;
  const bool changed = !first && boot_id != here().session.boot_id;
  here().session.boot_id = boot_id;
  here().boot_id_known = true;
  if (!first && !changed) {
    // The boot_id this end already has: the same boot. A second EV_BOOT of it - §8 repeats one
    // for a Walter that may have been asleep - is the same news, and news once is enough.
    return;
  }
  // §5.4: the clock is set after every EV_BOOT and every HELLO whose boot_id this end has not
  // seen - the first one included, because a radio this Walter has not spoken to has no clock.
  here().time_pending = true;
  if (!changed && !announced) {
    return;  // the first HELLO of a session is not news of a restart
  }
  counters_.restarts = saturate(counters_.restarts);
  event_seq_known_ = false;  // the radio's event counter starts again at 0
  for (std::size_t i = 0; i < restart_watcher_count_; ++i) {
    restart_watchers_[i]->onRadioRestart(boot_id, reset_reason);
  }
}

void Client::sessionFromBody(const HelloBody& body) {
  here().session.up = true;
  here().session.proto_major = body.proto_major;
  here().session.proto_minor = body.proto_minor;
  here().session.role = body.role;
  here().session.slot = body.slot;
  here().session.max_payload = body.max_payload;
  here().session.caps = body.caps;
  bool whole = copyText(body.fw_version, here().session.fw_version, sizeof here().session.fw_version);
  whole = copyText(body.board, here().session.board, sizeof here().session.board) && whole;
  whole = copyText(body.upstream, here().session.upstream, sizeof here().session.upstream) && whole;
  // §5.1, 1.1: the band, or - from a 1.0 body, whose decoded `band` is empty - nothing, which
  // empties the session's. A session that kept a band across a restart into a 1.0 image would
  // report a band the radio has stopped saying it has.
  whole = copyText(body.band, here().session.band, sizeof here().session.band) && whole;
  if (!whole) {
    counters_.long_versions = saturate(counters_.long_versions);
  }
}

// ---------------------------------------------------------------------------- §9, the text side

void Client::onTextByte(std::uint8_t byte) {
  if (byte == '\n' || byte == '\r') {
    if (line_size_ > 0 || line_overflow_) {
      keepTextLine();
    }
    line_size_ = 0;
    line_overflow_ = false;
    return;
  }
  if (line_size_ < kTextLineMax) {
    line_[line_size_++] = static_cast<char>(byte);
    return;
  }
  line_overflow_ = true;  // kept truncated, counted when the line ends
}

void Client::keepTextLine() {
  line_[line_size_] = 0;
  // NoiseScope's PROTOCOL.md: a line that does not begin with `{` is human-readable chatter and is
  // ignored by machine parsers. §9 makes the same byte the evidence of what the radio is running.
  if (line_size_ == 0 || line_[0] != '{') {
    return;
  }
  counters_.text_lines = saturate(counters_.text_lines);
  if (line_overflow_) {
    counters_.text_truncated = saturate(counters_.text_truncated);
  }
  const Text line{line_, line_size_};
  for (std::size_t i = 0; i < text_watcher_count_; ++i) {
    text_watchers_[i]->onTextLine(line);
  }
  if (text_count_ == kTextRing) {
    // The newest kTextRing lines: the oldest goes, and is counted.
    for (std::size_t i = 1; i < kTextRing; ++i) {
      copyBytes(text_[i], text_[i - 1], text_size_[i] + 1);
      text_size_[i - 1] = text_size_[i];
    }
    --text_count_;
    counters_.text_dropped = saturate(counters_.text_dropped);
  }
  copyBytes(line_, text_[text_count_], line_size_ + 1);
  text_size_[text_count_] = line_size_;
  ++text_count_;
}

Text Client::textLine(std::size_t index) const {
  if (index >= text_count_) {
    return Text{};
  }
  return Text{text_[index], text_size_[index]};
}

void Client::clearText() {
  text_count_ = 0;
  line_size_ = 0;
  line_overflow_ = false;
}

bool Client::probeForScanner() {
  // §9: "or gets one in answer to `info\n`". One command, one line, the same second a response
  // gets - NoiseScope answers every command with an `ack` line at once (its PROTOCOL.md).
  if (!port_.isOpen() && port_.open() != board::Outcome::kOk) {
    return false;
  }
  const std::uint8_t probe[] = {'i', 'n', 'f', 'o', '\n'};
  const std::uint32_t before = counters_.text_lines;
  receiver_.discardPartial();
  if (!port_.send(probe, sizeof probe)) {
    return false;
  }
  const std::uint64_t deadline = clock_.nowMs() + kProbeWaitMs;
  for (;;) {
    pump(false, 0, 0);
    if (counters_.text_lines != before) {
      return true;
    }
    if (clock_.nowMs() >= deadline) {
      return false;
    }
    clock_.delayMs(kPollMs);
  }
}

void Client::subscribeRestart(RestartWatcher* watcher) {
  if (watcher != nullptr && restart_watcher_count_ < kMaxWatchers) {
    restart_watchers_[restart_watcher_count_++] = watcher;
  }
}

void Client::subscribeText(TextWatcher* watcher) {
  if (watcher != nullptr && text_watcher_count_ < kMaxWatchers) {
    text_watchers_[text_watcher_count_++] = watcher;
  }
}

// ---------------------------------------------------------------------------- §5.1, HELLO

Answer<HelloResponse> Client::hello() {
  // The whole of HELLO is one operation on one radio: the three attempts and, after them, §9's
  // text-mode probe, which puts bytes on the same UART. One hold around all of it (task p1-06),
  // nested inside by the exchange - so the selection cannot change between the frames and the
  // probe that decides what is answering them.
  const board::SlotHold hold(slots_lock_, slot_);
  if (!hold.ok()) {
    Answer<HelloResponse> busy;
    busy.fault = Fault::kSlotBusy;
    return busy;
  }
  HelloRequest request;
  request.fw_version = fw_version_;
  Answer<HelloResponse> answer = call<HelloRequest, HelloResponse>(request);
  if (answer.ok()) {
    sessionFromBody(answer.message.body);
    noticeRestart(answer.message.body.boot_id, reset_reason::kUnknown, false);
    return answer;
  }
  here().session.up = false;
  if (answer.fault != Fault::kLinkDown) {
    return answer;  // refused, malformed, no port: the radio is there and speaks frames
  }
  // §9. Three HELLOs unanswered. Lines beginning with `{` already seen, or one in answer to the
  // probe, mean the radio is running NoiseScope: say so, stop sending frames, pass the lines on.
  if (counters_.text_lines > text_mark_ || probeForScanner()) {
    here().mode = Mode::kText;
    here().session = Session{};
    here().session.role = role::kScanner;  // §5.1's own number for it, learned without a HELLO
    receiver_.discardPartial();
  }
  return answer;
}

// ---------------------------------------------------------------------------- §5, the local calls

Answer<PingResponse> Client::ping(std::uint32_t nonce) {
  PingRequest request;
  request.nonce = nonce;
  return call<PingRequest, PingResponse>(request);
}

Answer<StatusResponse> Client::status() {
  return call<StatusRequest, StatusResponse>(StatusRequest{});
}

Answer<TimeSetResponse> Client::timeSet(std::uint32_t epoch, std::uint8_t source, bool force) {
  TimeSetRequest request;
  request.epoch = epoch;
  request.source = source;
  request.flags = force ? time_flag::kForce : 0;
  return call<TimeSetRequest, TimeSetResponse>(request);
}

Answer<TimeGetResponse> Client::timeGet() {
  return call<TimeGetRequest, TimeGetResponse>(TimeGetRequest{});
}

Answer<StatusOnly> Client::reboot(std::uint8_t mode, std::uint16_t delay_ms) {
  RebootRequest request;
  request.mode = mode;
  request.delay_ms = delay_ms;
  return call<RebootRequest, RebootResponse>(request);
}

Answer<IdentityResponse> Client::identity() {
  return call<IdentityRequest, IdentityResponse>(IdentityRequest{});
}

Answer<StatusOnly> Client::sleepNotice(std::uint32_t expect_back_s) {
  SleepNoticeRequest request;
  request.expect_back_s = expect_back_s;
  return call<SleepNoticeRequest, SleepNoticeResponse>(request);
}

Answer<CliResponse> Client::cli(Text line, bool sensitive) {
  CliRequest request;
  request.text = line;
  // A line made from a password (task p4-02): wiped from the request and frame buffers after the
  // exchange, exactly as SEND_LOGIN is. The line is never printed by this class either way.
  return call<CliRequest, CliResponse>(request, sensitive);
}

Answer<PowerGetResponse> Client::powerGet() {
  return call<PowerGetRequest, PowerGetResponse>(PowerGetRequest{});
}

Answer<EnvGetResponse> Client::envGet() {
  return call<EnvGetRequest, EnvGetResponse>(EnvGetRequest{});
}

Answer<LppGetResponse> Client::lppGet(std::uint8_t perm_mask) {
  LppGetRequest request;
  request.perm_mask = perm_mask;
  return call<LppGetRequest, LppGetResponse>(request);
}

// ---------------------------------------------------------------------------- §6 and §7

Answer<StatusOnly> Client::jobBegin(Bytes pubkey, const Path& path) {
  JobBeginRequest request;
  request.pubkey = pubkey;
  request.path = path;
  return call<JobBeginRequest, JobBeginResponse>(request);
}

Answer<StatusOnly> Client::jobEnd() {
  return call<JobEndRequest, JobEndResponse>(JobEndRequest{});
}

Answer<StatusOnly> Client::pathSet(const Path& path) {
  PathSetRequest request;
  request.path = path;
  return call<PathSetRequest, PathSetResponse>(request);
}

Answer<PathGetResponse> Client::pathGet() {
  return call<PathGetRequest, PathGetResponse>(PathGetRequest{});
}

Answer<SendResponse> Client::sendLogin(std::uint32_t timeout_ms, Bytes password) {
  SendLoginRequest request;
  request.timeout_ms = timeout_ms;
  request.password = password;
  // §6: the password is wiped from every buffer this end copied it to as soon as the exchange is
  // over. It is never stored and never printed.
  return call<SendLoginRequest, SendResponse>(request, true);
}

Answer<SendResponse> Client::sendRequest(std::uint32_t timeout_ms, std::uint8_t req_type,
                                         Bytes data) {
  SendRequestRequest request;
  request.timeout_ms = timeout_ms;
  request.req_type = req_type;
  request.data = data;
  return call<SendRequestRequest, SendResponse>(request);
}

Answer<SendResponse> Client::sendCli(std::uint32_t timeout_ms, std::uint8_t flags, Text line) {
  SendCliRequest request;
  request.timeout_ms = timeout_ms;
  request.flags = flags;
  request.text = line;
  const bool sensitive = (flags & send_cli_flag::kSensitive) != 0;
  return call<SendCliRequest, SendResponse>(request, sensitive);
}

Answer<SendResponse> Client::sendDiscovery(std::uint32_t timeout_ms) {
  SendDiscoveryRequest request;
  request.timeout_ms = timeout_ms;
  return call<SendDiscoveryRequest, SendResponse>(request);
}

Answer<SendResponse> Client::sendTrace(std::uint32_t timeout_ms, std::uint8_t hash_size,
                                       Bytes hashes) {
  SendTraceRequest request;
  request.timeout_ms = timeout_ms;
  request.hash_size = hash_size;
  request.hashes = hashes;
  return call<SendTraceRequest, SendResponse>(request);
}

Answer<ReplyGetResponse> Client::replyGet() {
  return call<ReplyGetRequest, ReplyGetResponse>(ReplyGetRequest{});
}

Answer<ObsPullResponse> Client::obsPull(std::uint8_t max) {
  ObsPullRequest request;
  request.max = max;
  return call<ObsPullRequest, ObsPullResponse>(request);
}

Answer<MeshCountersResponse> Client::meshCounters() {
  return call<MeshCountersRequest, MeshCountersResponse>(MeshCountersRequest{});
}

}  // namespace client
}  // namespace link
}  // namespace core
