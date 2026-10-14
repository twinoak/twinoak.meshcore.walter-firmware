#include "sim/scripted_radio.h"

#include <doctest.h>

#include "fixtures/link_fixture.h"

namespace sim {

namespace link = core::link;
using jsonfix::Value;

namespace {

// The fixtures' cases, once: the responses and events this radio answers with.
const Value& cases() { return fixtures::linkFixture()["cases"]; }

const Value* findCase(const std::string& name) {
  const Value& all = cases();
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all.at(i)["name"].string() == name) {
      return &all.at(i);
    }
  }
  return nullptr;
}

void putU32(std::vector<std::uint8_t>& into, std::size_t at, std::uint32_t value) {
  REQUIRE(at + 4 <= into.size());
  into[at] = static_cast<std::uint8_t>(value & 0xFF);
  into[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  into[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  into[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

// NoiseScope 0.1's own lines, from the hardware repository's
// software/firmware/noisescope-rak4630/PROTOCOL.md at b798aab: NDJSON, one object per line, and
// anything not beginning with `{` is chatter a machine parser ignores.
const char* kInfoLine =
    "{\"t\":\"info\",\"fw\":\"noisescope\",\"ver\":\"0.1.0\",\"hw\":\"rak4630\",\"radio\":\"sx1262\"}";
const char* kStatLine =
    "{\"t\":\"stat\",\"ts\":1754820010,\"ms\":12345,\"up\":12,\"mode\":\"monitor\","
    "\"home\":869.618}";
const char* kAckLine = "{\"t\":\"ack\",\"cmd\":\"?\",\"ok\":false,\"msg\":\"unknown\"}";

}  // namespace

ScriptedRadio::ScriptedRadio(fakes::FakeClock& clock) : clock_(clock) {}

bool ScriptedRadio::open() {
  if (refuse_open) {
    return false;
  }
  ++opens;
  is_open = true;
  last_activity_ms_ = clock_.nowMs();
  return true;
}

void ScriptedRadio::close() {
  if (is_open) {
    ++closes;
  }
  is_open = false;
}

std::size_t ScriptedRadio::read(std::uint8_t* out, std::size_t max) {
  if (!is_open || max == 0) {
    return 0;
  }
  ripen();
  const std::size_t n = readable_.size() < max ? readable_.size() : max;
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = readable_[i];
  }
  readable_.erase(readable_.begin(), readable_.begin() + static_cast<long>(n));
  if (n > 0) {
    last_activity_ms_ = clock_.nowMs();
  }
  return n;
}

std::size_t ScriptedRadio::write(const std::uint8_t* data, std::size_t size) {
  if (!is_open || size == 0) {
    return 0;
  }
  writes.push_back(std::vector<std::uint8_t>(data, data + size));
  last_activity_ms_ = clock_.nowMs();
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t byte = data[i];
    if (scanner) {
      // §9: the scanner reads lines of text (its PROTOCOL.md: "plain-text commands, one per
      // line"). A frame's bytes are not a line: anything that is not printable ASCII is garbage
      // to a line parser, and it throws away what it had collected - which is also what separates
      // a frame from the command that follows it, since every frame ends in 0x00.
      if (byte == '\n' || byte == '\r') {
        if (!line_.empty()) {
          onCommand(line_);
          line_.clear();
        }
        continue;
      }
      if (byte < 0x20 || byte > 0x7e) {
        line_.clear();
        continue;
      }
      if (line_.size() < 200) {
        line_.push_back(static_cast<char>(byte));
      }
      continue;
    }
    collected_.push_back(byte);
    const link::FrameReceiver::Fed fed = receiver_.push(byte);
    if (fed == link::FrameReceiver::Fed::kPacket) {
      std::vector<std::uint8_t> frame = collected_;
      // The sender's idle 0x00 belongs to the line, not to the frame (§2): strip a leading one so
      // that a test comparing two attempts compares frames.
      while (!frame.empty() && frame.front() == 0) {
        frame.erase(frame.begin());
      }
      onFrame(receiver_.packet(), frame);
      collected_.clear();
    } else if (fed != link::FrameReceiver::Fed::kNothing) {
      if (fed == link::FrameReceiver::Fed::kDropped) {
        ++dropped;
      }
      collected_.clear();
    }
  }
  return size;
}

bool ScriptedRadio::flush(std::uint32_t) { return is_open; }

bool ScriptedRadio::idleFor(std::uint32_t ms) {
  if (!is_open) {
    return false;
  }
  const std::uint64_t now = clock_.nowMs();
  return now >= last_activity_ms_ && now - last_activity_ms_ >= ms;
}

void ScriptedRadio::onFrame(const link::Packet& packet, const std::vector<std::uint8_t>& frame) {
  Seen seen;
  seen.type = packet.type;
  seen.seq = packet.seq;
  seen.payload.assign(packet.payload.data, packet.payload.data + packet.payload.size);
  seen.frame = frame;

  // §8: a pending attention stops the moment a valid request arrives.
  for (auto at = pending_.begin(); at != pending_.end();) {
    at = at->attention ? pending_.erase(at) : at + 1;
  }

  if (deaf || silence > 0) {
    if (silence > 0) {
      --silence;
    }
    requests.push_back(seen);
    return;
  }
  if (ignore_type_times_ > 0 && packet.type == ignore_type_) {
    --ignore_type_times_;
    requests.push_back(seen);
    return;
  }

  // §3: the (type, seq) of the last request executed is not executed again - the remembered
  // response is sent again. This is what makes a retry of a SEND_* transmit nothing.
  if (remembered_ && remembered_type_ == packet.type && remembered_seq_ == packet.seq) {
    seen.executed = false;
    requests.push_back(seen);
    answer(packet.type, packet.seq, remembered_payload_);
    return;
  }

  std::string name;
  const std::vector<std::uint8_t> payload = responseFor(packet, &name);
  ++executions;
  seen.executed = true;
  requests.push_back(seen);
  remembered_ = true;
  remembered_type_ = packet.type;
  remembered_seq_ = packet.seq;
  remembered_payload_ = payload;
  last_response = payload;
  last_response_case = name;
  answer(packet.type, packet.seq, payload);
}

std::vector<std::uint8_t> ScriptedRadio::responseFor(const core::link::Packet& packet,
                                                      std::string* case_name) {
  const std::uint8_t type = packet.type;
  case_name->clear();
  if (refuse_) {
    refuse_ = false;
    return std::vector<std::uint8_t>{refuse_status_};
  }
  if (refuse_type_times_ > 0 && type == refuse_type_) {
    --refuse_type_times_;
    return std::vector<std::uint8_t>{refuse_type_status_};
  }
  if (answer_with_) {
    answer_with_ = false;
    const Value* one = findCase(answer_case_);
    REQUIRE_MESSAGE(one != nullptr, "the fixtures have no case called " << answer_case_);
    *case_name = answer_case_;
    return fixtures::fromHex((*one)["payload"].string());
  }

  // The fixtures' own response for this type: the one whose status is OK if there is one (§3's
  // happy answer), otherwise whatever the fixtures give - SEND_TRACE's only response case is its
  // ERR_RANGE refusal, and that is a true answer to a trace of three-byte hashes.
  const Value& all = cases();
  const Value* fallback = nullptr;
  for (std::size_t i = 0; i < all.size(); ++i) {
    const Value& one = all.at(i);
    if (one["kind"].string() != "response" ||
        static_cast<std::uint8_t>(one["type"].integer()) != type) {
      continue;
    }
    if (one["fields"]["status"].integer() == core::link::status::kOk) {
      *case_name = one["name"].string();
      std::vector<std::uint8_t> payload = fixtures::fromHex(one["payload"].string());
      if (type == link::type::kHello) {
        // §5.1's body, with this radio's own role, capabilities and boot_id in it. The body starts
        // after the status byte: proto_major, proto_minor, role, slot, max_payload, caps, boot_id.
        payload = helloBody(payload, 1);
      }
      if (type == link::type::kStatus && override_status_flags && payload.size() >= 13) {
        // §5.3's body after the status byte: uptime_s u32, reset_reason u8, time_state u8,
        // epoch u32, flags u16 - so flags is at offset 11.
        payload[11] = static_cast<std::uint8_t>(status_flags & 0xFF);
        payload[12] = static_cast<std::uint8_t>(status_flags >> 8);
      }
      if (type == link::type::kPing && packet.payload.size >= 4 && payload.size() >= 5) {
        // §5.2: the nonce is echoed. The fixture's response carries the fixture request's nonce;
        // a radio answering *this* request echoes this one, which is what makes the client's
        // "the nonce came back" worth printing.
        for (std::size_t b = 0; b < 4; ++b) {
          payload[1 + b] = packet.payload.data[b];
        }
      }
      return payload;
    }
    fallback = &one;
  }
  if (fallback != nullptr) {
    *case_name = (*fallback)["name"].string();
    return fixtures::fromHex((*fallback)["payload"].string());
  }
  // §6.1: PATH_SET's response is a status and nothing else, and the fixtures have no case for it.
  *case_name = "synthesised: status only";
  return std::vector<std::uint8_t>{core::link::status::kOk};
}

std::vector<std::uint8_t> ScriptedRadio::helloBody(const std::vector<std::uint8_t>& from,
                                                   std::size_t at) {
  // §5.1: proto_major, proto_minor, role, slot, max_payload, caps (u32), boot_id (u32), then the
  // three str8s. `at` is where the body begins: 1 in a HELLO response (after the status), 1 in an
  // EV_BOOT (after the reset reason).
  std::vector<std::uint8_t> out = from;
  REQUIRE(out.size() >= at + 14);
  out[at + 2] = role;
  putU32(out, at + 5, caps);
  putU32(out, at + 9, boot_id);
  if (fw_version.empty() && board.empty()) {
    return out;
  }
  // The three str8s again, with this radio's own strings where it has them. A str8 is a length and
  // that many bytes (§2), so the tail is rebuilt rather than patched in place.
  std::size_t read_at = at + 13;
  std::string strings[3];
  for (std::size_t i = 0; i < 3; ++i) {
    REQUIRE(read_at < out.size());
    const std::size_t length = out[read_at++];
    REQUIRE(read_at + length <= out.size());
    strings[i].assign(reinterpret_cast<const char*>(out.data()) + read_at, length);
    read_at += length;
  }
  if (!fw_version.empty()) {
    strings[0] = fw_version;
  }
  if (!board.empty()) {
    strings[1] = board;
  }
  std::vector<std::uint8_t> rebuilt(out.begin(), out.begin() + static_cast<long>(at) + 13);
  for (const std::string& one : strings) {
    REQUIRE(one.size() <= 0xFF);
    rebuilt.push_back(static_cast<std::uint8_t>(one.size()));
    rebuilt.insert(rebuilt.end(), one.begin(), one.end());
  }
  REQUIRE(rebuilt.size() <= core::link::kMaxPayload);
  return rebuilt;
}

void ScriptedRadio::answer(std::uint8_t type, std::uint8_t seq,
                           const std::vector<std::uint8_t>& payload) {
  std::uint8_t frame[link::kMaxFrame] = {};
  std::uint8_t out_seq = seq;
  if (wrong_seq > 0) {
    --wrong_seq;
    out_seq = static_cast<std::uint8_t>(seq + 1);
  }
  const std::size_t size =
      link::buildFrame(link::responseTypeOf(type), out_seq,
                       link::Bytes{payload.data(), payload.size()}, frame, sizeof frame);
  REQUIRE(size != 0);
  std::vector<std::uint8_t> bytes(frame, frame + size);
  if (corrupt > 0) {
    --corrupt;
    // A byte in the middle, so that COBS still finds the terminator and the CRC does not match.
    bytes[bytes.size() / 2] = static_cast<std::uint8_t>(bytes[bytes.size() / 2] ^ 0x5A);
    if (bytes[bytes.size() / 2] == 0) {
      bytes[bytes.size() / 2] = 0x5B;  // never a second terminator
    }
  }
  queue(bytes, answer_delay_ms);
}

void ScriptedRadio::restart(bool announce) {
  boot_id = boot_id * 2654435761u + 1u;  // any new number will do
  event_seq = 0;
  remembered_ = false;
  receiver_.discardPartial();
  collected_.clear();
  if (announce) {
    sendEvent("boot event");
  }
}

void ScriptedRadio::sendEvent(const std::string& case_name, std::uint32_t after_ms) {
  const Value* one = findCase(case_name);
  REQUIRE_MESSAGE(one != nullptr, "the fixtures have no case called " << case_name);
  REQUIRE((*one)["kind"].string() == "event");
  const std::uint8_t type = static_cast<std::uint8_t>((*one)["type"].integer());
  std::vector<std::uint8_t> payload = fixtures::fromHex((*one)["payload"].string());
  if (type == link::type::kEvBoot) {
    payload = helloBody(payload, 1);  // after the reset reason
  }
  std::uint8_t frame[link::kMaxFrame] = {};
  const std::size_t size = link::buildFrame(
      type, event_seq, link::Bytes{payload.data(), payload.size()}, frame, sizeof frame);
  REQUIRE(size != 0);
  event_seq = static_cast<std::uint8_t>(event_seq + 1);
  queue(std::vector<std::uint8_t>(frame, frame + size), after_ms);
}

void ScriptedRadio::raiseAttention(std::uint8_t reason, std::uint16_t detail, int repeats) {
  // §8: 0x00 0x00 wakes the Walter, 300 ms later the event, then every 1000 ms with `repeat`
  // counting from 0, at most ten times.
  REQUIRE(repeats >= 1);
  REQUIRE(repeats <= core::link::kAttentionRepeats);
  queue(std::vector<std::uint8_t>{0x00, 0x00}, 0, true);
  for (int i = 0; i < repeats; ++i) {
    const std::vector<std::uint8_t> payload{
        reason, static_cast<std::uint8_t>(detail & 0xFF),
        static_cast<std::uint8_t>((detail >> 8) & 0xFF), static_cast<std::uint8_t>(i)};
    std::uint8_t frame[link::kMaxFrame] = {};
    const std::size_t size =
        link::buildFrame(link::type::kEvAttention, event_seq,
                         link::Bytes{payload.data(), payload.size()}, frame, sizeof frame);
    REQUIRE(size != 0);
    event_seq = static_cast<std::uint8_t>(event_seq + 1);
    queue(std::vector<std::uint8_t>(frame, frame + size),
          300 + static_cast<std::uint32_t>(i) * core::link::kAttentionRepeatMs, true);
  }
}

void ScriptedRadio::sendLine(const std::string& line, std::uint32_t after_ms) {
  std::vector<std::uint8_t> bytes(line.begin(), line.end());
  bytes.push_back('\n');
  queue(bytes, after_ms);
}

void ScriptedRadio::sendBytes(const std::vector<std::uint8_t>& bytes, std::uint32_t after_ms) {
  queue(bytes, after_ms);
}

void ScriptedRadio::answerWith(const std::string& case_name) {
  answer_with_ = true;
  answer_case_ = case_name;
}

void ScriptedRadio::refuseNext(std::uint8_t status) {
  refuse_ = true;
  refuse_status_ = status;
}

void ScriptedRadio::refuseType(std::uint8_t type, std::uint8_t status, int times) {
  refuse_type_ = type;
  refuse_type_status_ = status;
  refuse_type_times_ = times;
}

void ScriptedRadio::ignoreType(std::uint8_t type, int times) {
  ignore_type_ = type;
  ignore_type_times_ = times;
}

void ScriptedRadio::onCommand(const std::string& line) {
  commands.push_back(line);
  if (line == "info") {
    sendLine(kInfoLine);
    return;
  }
  if (line == "stat") {
    sendLine(kStatLine);
    return;
  }
  if (line == "help") {
    sendLine("# commands: help info stat sweep stop");  // chatter: not a `{` line
    return;
  }
  sendLine(kAckLine);
}

void ScriptedRadio::queue(const std::vector<std::uint8_t>& bytes, std::uint32_t after_ms,
                          bool attention) {
  Pending entry;
  entry.due_ms = clock_.nowMs() + after_ms;
  entry.bytes = bytes;
  entry.attention = attention;
  // In the order it will go out, not the order it was asked for: a test that schedules an event
  // for the middle of a response delay wants the event first on the wire.
  auto at = pending_.begin();
  while (at != pending_.end() && at->due_ms <= entry.due_ms) {
    ++at;
  }
  pending_.insert(at, entry);
}

void ScriptedRadio::ripen() {
  const std::uint64_t now = clock_.nowMs();
  while (!pending_.empty() && pending_.front().due_ms <= now) {
    const Pending& entry = pending_.front();
    readable_.insert(readable_.end(), entry.bytes.begin(), entry.bytes.end());
    pending_.pop_front();
  }
}

}  // namespace sim
