// The parts of the codec the fixtures do not reach: the rules of §2 a sender and a receiver keep
// between frames (the idle 0x00, the overflow, the counters), the refusals a decoder has to make
// that no valid fixture can show, and what an encoder refuses to send.
//
// The fixtures prove the codec agrees with the radio's. This file proves it behaves when nothing
// agrees with anything - which is what the unit is for.
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/link/codec/cobs.h"
#include "core/link/codec/crc16.h"
#include "core/link/codec/frame.h"
#include "core/link/codec/messages.h"
#include "core/link/codec/packet.h"
#include "core/link/codec/reader.h"
#include "core/link/codec/wire.h"
#include "core/link/codec/writer.h"
#include "fakes/fake_clock.h"

namespace link = core::link;

namespace {

link::Bytes viewOf(const std::vector<std::uint8_t>& bytes) {
  return link::Bytes{bytes.data(), bytes.size()};
}

std::vector<std::uint8_t> frameOf(std::uint8_t type, std::uint8_t seq,
                                  const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> out(link::kMaxFrame);
  const std::size_t size = link::buildFrame(type, seq, viewOf(payload), out.data(), out.size());
  REQUIRE(size != 0);
  out.resize(size);
  return out;
}

// A frame our own encoder will not make: COBS of a run of bytes with no zero in it is one code
// byte and the bytes, and §2's encoder refuses anything longer than a packet - which is exactly
// what a test of the receiver's length check needs.
std::vector<std::uint8_t> oversizeFrameByHand() {
  for (unsigned fill = 1; fill <= 0xFF; ++fill) {
    std::vector<std::uint8_t> packet = {link::type::kPing, 1};
    packet.insert(packet.end(), link::kMaxPayload + 1, static_cast<std::uint8_t>(fill));
    const std::uint16_t crc = link::crc16(packet.data(), packet.size());
    packet.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    packet.push_back(static_cast<std::uint8_t>(crc >> 8));
    bool clean = true;
    for (const std::uint8_t byte : packet) {
      clean = clean && byte != 0;
    }
    if (!clean) {
      continue;  // a zero would need a second COBS block, and this helper does one
    }
    std::vector<std::uint8_t> frame = {static_cast<std::uint8_t>(packet.size() + 1)};
    frame.insert(frame.end(), packet.begin(), packet.end());
    frame.push_back(0x00);
    return frame;
  }
  return {};
}

// Everything a receiver makes of a run of bytes, for a test that only cares about the outcome.
struct Received {
  std::size_t packets = 0;
  std::uint32_t dropped = 0;
  std::uint32_t empty = 0;
  link::FrameReceiver::Drop last = link::FrameReceiver::Drop::kNone;
};

Received receive(const std::vector<std::uint8_t>& stream) {
  link::FrameReceiver receiver;
  Received seen;
  seen.packets = receiver.feed(viewOf(stream), [](const link::Packet&) {});
  seen.dropped = receiver.dropped();
  seen.empty = receiver.emptyFrames();
  seen.last = receiver.lastDrop();
  return seen;
}

}  // namespace

TEST_CASE("the numbers of the specification's own tables") {
  // §2 and §3, the shapes: 4 to 244 bytes of packet, 246 of receive buffer, the response bit.
  CHECK(link::kMaxPacket == 244);
  CHECK(link::kMaxEncoded == 245);
  CHECK(link::kMaxFrame == 246);
  CHECK(link::kReceiveBuffer == 246);
  CHECK(link::responseTypeOf(link::type::kHello) == 0x81);
  CHECK(link::requestTypeOf(0x81) == link::type::kHello);
  CHECK(link::isRequest(link::type::kHello));
  CHECK(link::isEvent(link::type::kEvBoot));
  CHECK(link::isResponse(link::responseTypeOf(link::type::kMeshCounters)));
  CHECK_FALSE(link::isRequest(link::type::kEvBoot));
  CHECK_FALSE(link::isResponse(link::type::kEvAttention));
  CHECK_FALSE(link::isEvent(0x80));  // §3: 0x80 is in no range; 0x81 is the first response
  // §7's encoded path length: bits 7-6 the hash size minus one, bits 5-0 the hop count.
  CHECK(link::pathHashSize(0x42) == 2);
  CHECK(link::pathHopCount(0x42) == 2);
  CHECK(link::pathHashSize(0x00) == 1);
  CHECK(link::pathHopCount(0x00) == 0);
  // §6.2: a trace cannot express three-byte hashes.
  CHECK(link::isTraceHashSize(1));
  CHECK(link::isTraceHashSize(2));
  CHECK_FALSE(link::isTraceHashSize(3));
  CHECK(link::isTraceHashSize(4));
  CHECK(link::isTraceHashSize(8));
  CHECK_FALSE(link::isTraceHashSize(16));
}

TEST_CASE("cobs refuses what section 2 says a decoder refuses") {
  std::uint8_t out[link::kMaxPacket] = {};
  SUBCASE("a zero inside the frame") {
    const std::vector<std::uint8_t> encoded = {0x03, 0x11, 0x00};
    const link::CobsDecoded decoded = link::cobsDecode(viewOf(encoded), out, sizeof(out));
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == link::CobsError::kZeroInside);
  }
  SUBCASE("a code byte of zero") {
    const std::vector<std::uint8_t> encoded = {0x00, 0x11};
    const link::CobsDecoded decoded = link::cobsDecode(viewOf(encoded), out, sizeof(out));
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == link::CobsError::kZeroInside);
  }
  SUBCASE("a block that runs past the end") {
    const std::vector<std::uint8_t> encoded = {0x09, 0x02, 0x03};
    const link::CobsDecoded decoded = link::cobsDecode(viewOf(encoded), out, sizeof(out));
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == link::CobsError::kRunsPastEnd);
  }
  SUBCASE("the 0xFF code of general COBS, which no packet of this link produces") {
    std::vector<std::uint8_t> encoded(1, 0xFF);
    encoded.insert(encoded.end(), 254, 0x11);
    const link::CobsDecoded decoded = link::cobsDecode(viewOf(encoded), out, sizeof(out));
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == link::CobsError::kCodeFf);
  }
  SUBCASE("nothing at all") {
    const link::CobsDecoded decoded = link::cobsDecode(link::Bytes{}, out, sizeof(out));
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == link::CobsError::kEmpty);
  }
  SUBCASE("a decoding that does not fit the caller's buffer") {
    const std::vector<std::uint8_t> encoded = {0x05, 0x11, 0x22, 0x33, 0x44};
    std::uint8_t small[2] = {};
    const link::CobsDecoded decoded = link::cobsDecode(viewOf(encoded), small, sizeof(small));
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == link::CobsError::kNoRoom);
  }
  SUBCASE("an encoding longer than a packet is not one this link sends") {
    const std::vector<std::uint8_t> raw(link::kMaxPacket + 1, 0x11);
    std::uint8_t big[link::kMaxPacket + 8] = {};
    CHECK(link::cobsEncode(viewOf(raw), big, sizeof(big)) == 0);
  }
  SUBCASE("an output buffer one byte too small") {
    const std::vector<std::uint8_t> raw = {0x11, 0x22, 0x33};
    std::uint8_t small[3] = {};
    CHECK(link::cobsEncode(viewOf(raw), small, sizeof(small)) == 0);
  }
}

TEST_CASE("a packet is 4 to 244 bytes and its crc is over type, seq and payload") {
  const std::vector<std::uint8_t> payload = {0xDE, 0xAD};
  std::uint8_t out[link::kMaxPacket] = {};
  const std::size_t size =
      link::buildPacket(link::type::kPing, 7, viewOf(payload), out, sizeof(out));
  REQUIRE(size == 6);
  const std::uint16_t crc = link::crc16(out, 4);
  CHECK(out[4] == static_cast<std::uint8_t>(crc & 0xFF));  // little-endian (§2)
  CHECK(out[5] == static_cast<std::uint8_t>(crc >> 8));

  link::Packet parsed;
  link::PacketError error = link::PacketError::kCrc;
  REQUIRE(link::parsePacket(link::Bytes{out, size}, &parsed, &error));
  CHECK(error == link::PacketError::kNone);
  CHECK(parsed.type == link::type::kPing);
  CHECK(parsed.seq == 7);
  CHECK(parsed.payload.size == 2);

  SUBCASE("a payload over 240 bytes is not a packet") {
    const std::vector<std::uint8_t> big(link::kMaxPayload + 1, 0x11);
    CHECK(link::buildPacket(link::type::kPing, 1, viewOf(big), out, sizeof(out)) == 0);
  }
  SUBCASE("three bytes leave no room for a crc") {
    const std::uint8_t three[3] = {0x02, 0x03, 0x04};
    error = link::PacketError::kNone;
    CHECK_FALSE(link::parsePacket(link::Bytes{three, sizeof(three)}, &parsed, &error));
    CHECK(error == link::PacketError::kLength);
  }
  SUBCASE("one flipped bit is a failed crc") {
    out[3] ^= 0x01;
    error = link::PacketError::kNone;
    CHECK_FALSE(link::parsePacket(link::Bytes{out, size}, &parsed, &error));
    CHECK(error == link::PacketError::kCrc);
  }
  SUBCASE("nothing is interpreted before the crc has passed") {
    // §2's last rule, as a property: the fields of `parsed` are untouched by a failed parse.
    out[2] = 0xFF;  // break the payload, and with it the crc
    link::Packet untouched;
    CHECK_FALSE(link::parsePacket(link::Bytes{out, size}, &untouched, nullptr));
    CHECK(untouched.type == 0);
    CHECK(untouched.seq == 0);
    CHECK(untouched.payload.data == nullptr);
  }
}

TEST_CASE("a receiver drops and counts, and goes on") {
  const std::vector<std::uint8_t> good = frameOf(link::type::kPing, 3, {1, 2, 3, 4});

  SUBCASE("two terminators in a row are empty frames, not errors") {
    std::vector<std::uint8_t> stream = {0x00, 0x00};
    stream.insert(stream.end(), good.begin(), good.end());
    const Received seen = receive(stream);
    CHECK(seen.packets == 1);
    CHECK(seen.dropped == 0);
    CHECK(seen.empty == 2);
  }

  SUBCASE("noise between two frames costs one drop, not one per byte") {
    std::vector<std::uint8_t> stream = good;
    for (int i = 0; i < 40; ++i) {
      stream.push_back(0x42);
    }
    stream.push_back(0x00);
    stream.insert(stream.end(), good.begin(), good.end());
    const Received seen = receive(stream);
    CHECK(seen.packets == 2);
    CHECK(seen.dropped == 1);
  }

  SUBCASE("a frame that overflows the buffer is dropped when its terminator arrives") {
    std::vector<std::uint8_t> stream(link::kReceiveBuffer + 50, 0x11);
    stream.push_back(0x00);
    stream.insert(stream.end(), good.begin(), good.end());
    const Received seen = receive(stream);
    CHECK(seen.packets == 1);  // the frame after it is read, so the receiver recovered
    CHECK(seen.dropped == 1);
    CHECK(seen.last == link::FrameReceiver::Drop::kOverflow);
  }

  SUBCASE("a frame whose packet would be over 244 bytes is a length, not a cobs, complaint") {
    // 241 bytes of payload: one byte more than §2 allows. Its frame is 247 bytes - the 246 the
    // buffer holds and the terminator - so the buffer does not overflow and it is the decoded
    // length that refuses it.
    std::vector<std::uint8_t> frame = oversizeFrameByHand();
    REQUIRE(frame.size() == link::kReceiveBuffer + 1);
    const Received seen = receive(frame);
    CHECK(seen.packets == 0);
    CHECK(seen.dropped == 1);
    CHECK(seen.last == link::FrameReceiver::Drop::kLength);
  }

  SUBCASE("a frame cut short by a stray terminator") {
    std::vector<std::uint8_t> stream(good.begin(), good.begin() + 4);
    stream.push_back(0x00);
    stream.insert(stream.end(), good.begin(), good.end());
    const Received seen = receive(stream);
    CHECK(seen.packets == 1);
    CHECK(seen.dropped == 1);
    CHECK(seen.last == link::FrameReceiver::Drop::kCobs);
  }

  SUBCASE("a half-collected frame can be thrown away without losing the counters") {
    link::FrameReceiver receiver;
    for (std::size_t i = 0; i + 1 < good.size(); ++i) {
      receiver.push(good[i]);
    }
    receiver.discardPartial();
    CHECK(receiver.push(0x00) == link::FrameReceiver::Fed::kEmptyFrame);
    std::size_t packets = 0;
    receiver.feed(viewOf(good), [&](const link::Packet&) { ++packets; });
    CHECK(packets == 1);
    CHECK(receiver.dropped() == 0);
  }

  SUBCASE("a 240-byte payload, the largest there is, goes through whole") {
    const std::vector<std::uint8_t> payload(link::kMaxPayload, 0x5A);
    const std::vector<std::uint8_t> frame = frameOf(0xCC, 0xFF, payload);
    CHECK(frame.size() == link::kMaxFrame);
    link::FrameReceiver receiver;
    std::size_t packets = 0;
    receiver.feed(viewOf(frame), [&](const link::Packet& packet) {
      ++packets;
      CHECK(packet.type == 0xCC);
      CHECK(packet.seq == 0xFF);
      CHECK(packet.payload.size == link::kMaxPayload);
    });
    CHECK(packets == 1);
    CHECK(receiver.dropped() == 0);
  }
}

TEST_CASE("the writer puts one 0x00 in front after 100 ms of idle") {
  fakes::FakeClock clock;
  link::FrameWriter writer(clock);
  std::uint8_t out[link::kMaxWrite] = {};
  const std::vector<std::uint8_t> payload = {0x01};

  // Nothing has gone out yet: the line counts as idle, which ends whatever the radio may hold.
  std::size_t size = writer.write(link::type::kPing, 1, viewOf(payload), out, sizeof(out));
  REQUIRE(size != 0);
  CHECK(writer.prefixed());
  CHECK(out[0] == 0x00);
  CHECK(out[1] != 0x00);

  clock.advance(link::kIdleMs - 1);
  size = writer.write(link::type::kPing, 2, viewOf(payload), out, sizeof(out));
  REQUIRE(size != 0);
  CHECK_FALSE(writer.prefixed());
  CHECK(out[0] != 0x00);

  clock.advance(link::kIdleMs);
  size = writer.write(link::type::kPing, 3, viewOf(payload), out, sizeof(out));
  REQUIRE(size != 0);
  CHECK(writer.prefixed());

  SUBCASE("a clock that restarted under us counts as idle") {
    // esp_timer restarts at zero at every wake from deep sleep (interfaces/clock.h), and a
    // Walter that slept has certainly left the line idle.
    clock.now_ms = 0;
    size = writer.write(link::type::kPing, 4, viewOf(payload), out, sizeof(out));
    REQUIRE(size != 0);
    CHECK(writer.prefixed());
  }

  SUBCASE("forgetting the idle time asks for the 0x00 whatever the clock says") {
    writer.forgetIdle();
    size = writer.write(link::type::kPing, 4, viewOf(payload), out, sizeof(out));
    REQUIRE(size != 0);
    CHECK(writer.prefixed());
  }

  SUBCASE("a frame a receiver reads back, prefix and all") {
    clock.advance(link::kIdleMs);
    const std::vector<std::uint8_t> big(link::kMaxPayload, 0x5A);
    size = writer.write(0x81, 9, viewOf(big), out, sizeof(out));
    REQUIRE(size == link::kMaxWrite);  // 247: the idle 0x00, 245 of COBS and the terminator
    link::FrameReceiver receiver;
    std::size_t packets = 0;
    receiver.feed(link::Bytes{out, size}, [&](const link::Packet& packet) {
      ++packets;
      CHECK(packet.type == 0x81);
      CHECK(packet.seq == 9);
      CHECK(packet.payload.size == link::kMaxPayload);
    });
    CHECK(packets == 1);
    CHECK(receiver.emptyFrames() == 1);
    CHECK(receiver.dropped() == 0);
  }

  SUBCASE("a buffer that cannot hold the frame is refused, and nothing is stamped") {
    std::uint8_t small[4] = {};
    CHECK(writer.write(link::type::kPing, 5, viewOf(payload), small, sizeof(small)) == 0);
    const std::vector<std::uint8_t> too_big(link::kMaxPayload + 1, 0x11);
    CHECK(writer.write(link::type::kPing, 5, viewOf(too_big), out, sizeof(out)) == 0);
  }
}

TEST_CASE("the reader stops at the end of the payload") {
  const std::uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
  link::Reader reader(payload, sizeof(payload));
  std::uint32_t value = 0;
  REQUIRE(reader.u32(&value));
  CHECK(value == 0x04030201u);  // little-endian (§2)
  CHECK(reader.remaining() == 0);
  std::uint8_t byte = 0xAA;
  CHECK_FALSE(reader.u8(&byte));
  CHECK(byte == 0xAA);  // untouched by a read that did not happen
  CHECK_FALSE(reader.ok());
  CHECK(reader.rest().size == 0);

  SUBCASE("a str8 whose length is longer than what is left") {
    const std::uint8_t short_str[3] = {0x08, 'h', 'i'};
    link::Reader str(short_str, sizeof(short_str));
    link::Text text;
    CHECK_FALSE(str.str8(&text));
    CHECK(text.data == nullptr);
  }
  SUBCASE("an empty payload reads nothing") {
    link::Reader empty(nullptr, 0);
    std::uint8_t first = 0;
    CHECK_FALSE(empty.u8(&first));
    CHECK(empty.rest().size == 0);
  }
  SUBCASE("the signed scalars are two's complement") {
    const std::uint8_t signed_bytes[3] = {0xFF, 0x18, 0xFC};
    link::Reader signed_reader(signed_bytes, sizeof(signed_bytes));
    std::int8_t small = 0;
    std::int16_t wide = 0;
    REQUIRE(signed_reader.i8(&small));
    REQUIRE(signed_reader.i16(&wide));
    CHECK(small == -1);
    CHECK(wide == -1000);
  }
}

TEST_CASE("the writer stops at the end of the buffer") {
  std::uint8_t out[4] = {};
  link::Writer writer(out, sizeof(out));
  CHECK(writer.u16(0x1234));
  CHECK(writer.u16(0x5678));
  CHECK_FALSE(writer.u8(0x9A));
  CHECK_FALSE(writer.ok());
  CHECK(writer.size() == 0);  // a failed writer has written nothing anybody may send
  CHECK(out[0] == 0x34);
  CHECK(out[3] == 0x56);

  SUBCASE("a str8 over 255 bytes has no length to write") {
    std::uint8_t big[300] = {};
    link::Writer wide(big, sizeof(big));
    const std::string long_text(256, 'x');
    CHECK_FALSE(wide.str8(link::text(long_text.data(), long_text.size())));
    CHECK_FALSE(wide.ok());
  }
}

TEST_CASE("an encoder refuses what the specification does not allow on the wire") {
  std::uint8_t out[link::kMaxPayload] = {};
  const std::vector<std::uint8_t> key(link::kPubkeySize, 0xAB);

  SUBCASE("a console line of 0 or over 160 bytes (§5.7)") {
    link::CliRequest request;
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);  // empty
    const std::string too_long(link::kMaxCliLine + 1, 'x');
    request.text = link::text(too_long.data(), too_long.size());
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
    const std::string longest(link::kMaxCliLine, 'x');
    request.text = link::text(longest.data(), longest.size());
    CHECK(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("a mesh cli line over 148 bytes (§6.2)") {
    link::SendCliRequest request;
    request.timeout_ms = 1000;
    const std::string too_long(link::kMaxMeshCliText + 1, 'x');
    request.text = link::text(too_long.data(), too_long.size());
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("a password over 15 bytes (§6.2)") {
    link::SendLoginRequest request;
    std::vector<std::uint8_t> password(link::kMaxPassword + 1, 'x');
    request.password = viewOf(password);
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
    password.pop_back();
    request.password = viewOf(password);
    CHECK(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("three-byte hashes in a trace (§6.2)") {
    const std::vector<std::uint8_t> hashes(6, 0x11);
    link::SendTraceRequest request;
    request.hash_size = 3;
    request.hashes = viewOf(hashes);
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
    request.hash_size = 2;
    CHECK(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("a public key that is not 32 bytes (§6.1)") {
    const std::vector<std::uint8_t> short_key(31, 0xAB);
    link::JobBeginRequest request;
    request.pubkey = viewOf(short_key);
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
    request.pubkey = viewOf(key);
    CHECK(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("a path that does not add up (§6.1)") {
    const std::vector<std::uint8_t> hashes(4, 0x11);
    link::PathSetRequest request;
    request.path.mode = link::path_mode::kHops;
    request.path.hash_size = 2;
    request.path.hop_count = 2;
    request.path.hashes = viewOf(hashes);
    REQUIRE(link::encode(request, out, sizeof(out)).ok);

    request.path.hop_count = 3;  // the hashes do not match the count
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
    request.path.hop_count = 2;
    request.path.hash_size = 4;  // §6.1: 1 to 3 bytes per hop
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
    request.path.hash_size = 2;
    request.path.mode = 3;  // a mode the radio could not read
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("a path of over 64 bytes of hashes (§6.1)") {
    const std::vector<std::uint8_t> hashes(3 * 23, 0x11);  // 69 bytes, 23 hops of three
    link::PathSetRequest request;
    request.path.mode = link::path_mode::kHops;
    request.path.hash_size = 3;
    request.path.hop_count = 23;
    request.path.hashes = viewOf(hashes);
    CHECK_FALSE(link::encode(request, out, sizeof(out)).ok);
  }

  SUBCASE("a buffer too small for the payload") {
    std::uint8_t small[2] = {};
    link::PingRequest request;
    request.nonce = 1;
    CHECK_FALSE(link::encode(request, small, sizeof(small)).ok);
  }
}

TEST_CASE("a decoder refuses a value that selects a layout it does not know") {
  SUBCASE("a path mode past the three of §6.1") {
    const std::uint8_t payload[4] = {link::status::kOk, 3, 0, 0};
    link::PathGetResponse response;
    CHECK_FALSE(link::decode(link::Bytes{payload, sizeof(payload)}, &response));
  }
  SUBCASE("a path whose hash size or hop count is out of range") {
    const std::uint8_t too_wide[7] = {link::status::kOk, link::path_mode::kHops, 4, 1, 1, 2, 3};
    link::PathGetResponse response;
    CHECK_FALSE(link::decode(link::Bytes{too_wide, sizeof(too_wide)}, &response));
    const std::uint8_t no_hops[4] = {link::status::kOk, link::path_mode::kHops, 2, 0};
    CHECK_FALSE(link::decode(link::Bytes{no_hops, sizeof(no_hops)}, &response));
  }
  SUBCASE("a path of over 64 bytes of hashes") {
    std::vector<std::uint8_t> payload = {link::status::kOk, link::path_mode::kHops, 3, 23};
    payload.insert(payload.end(), 69, 0x11);
    link::PathGetResponse response;
    CHECK_FALSE(link::decode(viewOf(payload), &response));
  }
  SUBCASE("a reply state past the six of §6.3") {
    std::vector<std::uint8_t> payload = {link::status::kOk, 6};
    payload.insert(payload.end(), 12, 0x00);
    link::ReplyGetResponse response;
    CHECK_FALSE(link::decode(viewOf(payload), &response));
  }
  SUBCASE("an observation with over 32 bytes of app data") {
    std::vector<std::uint8_t> payload = {link::status::kOk, 0, 0, 0, 1};
    payload.insert(payload.end(), link::kPubkeySize, 0xAB);
    payload.insert(payload.end(), 8, 0x00);          // adv_ts and age_s
    payload.push_back(0x00);                          // snr_q4
    payload.insert(payload.end(), 2, 0x00);           // rssi
    payload.push_back(0x00);                          // path_len: no path
    payload.push_back(static_cast<std::uint8_t>(link::kMaxAppData + 1));
    payload.insert(payload.end(), link::kMaxAppData + 1, 0x11);
    link::ObsPullResponse response;
    CHECK_FALSE(link::decode(viewOf(payload), &response));
  }
  SUBCASE("more observations than a payload can hold") {
    const std::uint8_t payload[5] = {link::status::kOk, 0, 0, 0,
                                     static_cast<std::uint8_t>(link::kMaxObservations + 1)};
    link::ObsPullResponse response;
    CHECK_FALSE(link::decode(link::Bytes{payload, sizeof(payload)}, &response));
  }
  SUBCASE("a status with no byte to hold it") {
    link::StatusOnly response;
    CHECK_FALSE(link::decode(link::Bytes{}, &response));
    link::PingResponse ping;
    CHECK_FALSE(link::decode(link::Bytes{}, &ping));
  }
  SUBCASE("a status this firmware does not know is passed on, not refused") {
    // §5.1: a minor version adds numbers to the tables and a receiver ignores what it does not
    // know. A status of 9 is still "not OK", which is all the client has to act on.
    const std::uint8_t payload[1] = {9};
    link::HelloResponse response;
    REQUIRE(link::decode(link::Bytes{payload, sizeof(payload)}, &response));
    CHECK(response.status == 9);
    CHECK(response.body.boot_id == 0);  // nothing was read: there was no body
  }
}
