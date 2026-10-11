// Events (task p1-02): §3's own sequence numbers and the gaps in them, a bounded queue whose
// overflow is counted, an event that arrives between a request and its response, and §8's
// attention - the two zero bytes that wake a sleeping Walter, the 300 ms, and the repeats.
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/link/client/client.h"
#include "fixtures/link_fields.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;

namespace {

// Everything the client has queued, taken out in order.
std::vector<lc::QueuedEvent> drain(lc::Client& client) {
  std::vector<lc::QueuedEvent> out;
  lc::QueuedEvent one;
  while (client.takeEvent(&one)) {
    out.push_back(one);
  }
  return out;
}

// Reads the port for a while, as the firmware's main loop does (main/main.cpp).
void poll(sim::LinkBench& bench, std::uint32_t ms) {
  for (std::uint32_t at = 0; at < ms; at += 100) {
    bench.client.poll();
    bench.clock.advance(100);
  }
  bench.client.poll();
}

}  // namespace

TEST_CASE("an event between a request and its response: both arrive, neither spoils the other") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  bench.radio.answer_delay_ms = 400;
  bench.radio.sendEvent("attention: the battery is low", 200);

  const lc::Answer<link::StatusResponse> answer = bench.client.status();
  REQUIRE(answer.ok());
  CHECK(answer.attempts == 1);
  CHECK(answer.message.uptime_s == 3600);  // the fixture's STATUS response, undisturbed

  const std::vector<lc::QueuedEvent> events = drain(bench.client);
  REQUIRE(events.size() == 1);
  CHECK(events[0].type == link::type::kEvAttention);
  link::EvAttention decoded;
  REQUIRE(link::decode(events[0].view(), &decoded));
  CHECK(decoded.reason == link::attention::kBatteryLow);
  CHECK(decoded.detail == 3450);
  CHECK(bench.client.counters().events == 1);
  CHECK(bench.client.counters().events_dropped == 0);
}

TEST_CASE("the queue is bounded and its overflow is counted") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  for (std::size_t i = 0; i < lc::kEventQueue + 3; ++i) {
    bench.radio.sendEvent("attention: the battery is low");
  }
  bench.client.poll();
  CHECK(bench.client.eventsQueued() == lc::kEventQueue);
  CHECK(bench.client.counters().events == lc::kEventQueue);
  CHECK(bench.client.counters().events_dropped == 3);
  // §3: nothing is lost that cannot be asked for - the oldest are kept, in order.
  const std::vector<lc::QueuedEvent> events = drain(bench.client);
  REQUIRE(events.size() == lc::kEventQueue);
  for (std::size_t i = 0; i < events.size(); ++i) {
    CHECK(static_cast<std::size_t>(events[i].seq) == i);
  }
  CHECK(bench.client.eventsQueued() == 0);
}

TEST_CASE("a gap in the event numbers is counted, and a boot is not a gap") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  bench.radio.sendEvent("attention: the battery is low");  // seq 0
  bench.client.poll();
  CHECK(bench.client.counters().event_gaps == 0);
  std::uint8_t last = 0xFF;
  REQUIRE(bench.client.lastEventSeq(&last));
  CHECK(last == 0);

  // The radio sent one the Walter never heard (§8's repeats are each their own event).
  bench.radio.event_seq = 2;
  bench.radio.sendEvent("attention: the battery is low");  // seq 2
  bench.client.poll();
  CHECK(bench.client.counters().event_gaps == 1);
  REQUIRE(bench.client.lastEventSeq(&last));
  CHECK(last == 2);

  // §3: the radio counts from 0 at every boot, so the EV_BOOT that follows a restart is not a gap.
  bench.radio.restart(true);
  bench.client.poll();
  CHECK(bench.client.counters().event_gaps == 1);
  bench.radio.sendEvent("attention: the battery is low");  // seq 1 of the new boot
  bench.client.poll();
  CHECK(bench.client.counters().event_gaps == 1);
}

TEST_CASE("an attention's leading zeros and its repeats do not confuse the receiver") {
  // §8: to raise an attention the radio sends 0x00 0x00, waits 300 ms, sends the event, and then
  // repeats it every 1000 ms. The zeros are empty frames, which §2 says are not errors.
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  REQUIRE(bench.port.isOpen());
  const std::uint32_t repeats = 3;
  bench.radio.raiseAttention(link::attention::kPersonAsked, 0, static_cast<int>(repeats));

  poll(bench, 300 + repeats * link::kAttentionRepeatMs);
  CHECK(bench.client.receiver().dropped() == 0);
  CHECK(bench.client.receiver().emptyFrames() >= 2);
  CHECK(bench.client.eventsQueued() == repeats);
  const std::vector<lc::QueuedEvent> events = drain(bench.client);
  REQUIRE(events.size() == repeats);
  for (std::size_t i = 0; i < events.size(); ++i) {
    INFO("repeat " << i);
    CHECK(events[i].type == link::type::kEvAttention);
    link::EvAttention decoded;
    REQUIRE(link::decode(events[i].view(), &decoded));
    CHECK(decoded.reason == link::attention::kPersonAsked);
    CHECK(static_cast<std::size_t>(decoded.repeat) == i);  // §8: `repeat` counts from 0
    // Each repeat is its own event with its own seq, counted from 0 at this boot (§3).
    CHECK(static_cast<std::size_t>(events[i].seq) == i);
  }
}

TEST_CASE("a request stops the repeats, because the radio knows the Walter is awake") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  bench.radio.raiseAttention(link::attention::kBatteryCritical, 3300,
                             link::kAttentionRepeats);
  poll(bench, 400);  // the zeros, the 300 ms, and the first event
  CHECK(bench.client.eventsQueued() == 1);
  REQUIRE(bench.client.ping(3).ok());  // §8: "until any valid request arrives"
  poll(bench, 5 * link::kAttentionRepeatMs);
  CHECK(bench.client.eventsQueued() == 1);
}

TEST_CASE("an event whose payload does not decode is counted, not queued") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  // EV_ATTENTION is reason, detail and repeat: four bytes. One byte is malformed (§3's
  // ERR_MALFORMED is the radio's word for the same thing), and nothing downstream could read it.
  std::uint8_t frame[link::kMaxFrame] = {};
  const std::uint8_t payload[1] = {link::attention::kFault};
  const std::size_t size = link::buildFrame(link::type::kEvAttention, 9,
                                            link::Bytes{payload, sizeof payload}, frame,
                                            sizeof frame);
  REQUIRE(size != 0);
  bench.radio.sendBytes(std::vector<std::uint8_t>(frame, frame + size));
  bench.client.poll();
  CHECK(bench.client.eventsQueued() == 0);
  CHECK(bench.client.counters().malformed == 1);
  CHECK(bench.client.counters().events == 0);
  CHECK(bench.client.receiver().dropped() == 0);  // the frame was good; the payload was not
}

TEST_CASE("the events the fixtures have, decoded out of the queue") {
  // Every event case of the fixtures through the queue: what a consumer gets is the same fields
  // the codec's own test checks, after a copy and a decode on the other side of it.
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const jsonfix::Value& cases = fixtures::linkFixture()["cases"];
  std::size_t checked = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const jsonfix::Value& one = cases.at(i);
    if (one["kind"].string() != "event") {
      continue;
    }
    const std::string name = one["name"].string();
    INFO("case: " << name);
    bench.client.clearEvents();
    bench.radio.sendEvent(name);
    bench.client.poll();
    REQUIRE(bench.client.eventsQueued() == 1);
    lc::QueuedEvent event;
    REQUIRE(bench.client.takeEvent(&event));
    CHECK(event.type == fixtures::u8(one, "type"));
    const jsonfix::Value& fields = one["fields"];
    switch (event.type) {
      case link::type::kEvBoot: {
        link::EvBoot decoded;
        REQUIRE(link::decode(event.view(), &decoded));
        CHECK(decoded.reset_reason == fixtures::u8(fields, "reset_reason"));
        CHECK(decoded.body.proto_major == fixtures::u8(fields, "proto_major"));
        CHECK(fixtures::asString(decoded.body.board) == fixtures::textOf(fields, "board"));
        break;
      }
      case link::type::kEvMeshReply: {
        link::EvMeshReply decoded;
        REQUIRE(link::decode(event.view(), &decoded));
        CHECK(decoded.reply.state == fixtures::u8(fields, "state"));
        CHECK(decoded.reply.tag == fixtures::u32(fields, "tag"));
        CHECK(decoded.reply.elapsed_ms == fixtures::u32(fields, "elapsed_ms"));
        CHECK(decoded.reply.rssi == fixtures::i16(fields, "rssi"));
        break;
      }
      case link::type::kEvAttention: {
        link::EvAttention decoded;
        REQUIRE(link::decode(event.view(), &decoded));
        CHECK(decoded.reason == fixtures::u8(fields, "reason"));
        CHECK(decoded.detail == fixtures::u16(fields, "detail"));
        CHECK(decoded.repeat == fixtures::u8(fields, "repeat"));
        break;
      }
      default:
        FAIL("an event type this test does not know: " << name);
    }
    ++checked;
  }
  CHECK(checked == 7);  // the 7 event cases of the fixtures
}
