// The two things the link client gained for the mesh client (task p4-02): a CLI request flagged
// sensitive, wiped like a login, and an event taken out of the queue by type with the others
// left for whoever they are for.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "core/link/client/client.h"
#include "core/link/codec/wire.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;

TEST_CASE("a CLI line flagged sensitive is wiped from the client's buffers like a login") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const std::string secret = "password hunter2-ish";
  const auto holds = [&bench](const std::string& needle) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&bench.client);
    const std::string image(reinterpret_cast<const char*>(bytes), sizeof(bench.client));
    return image.find(needle) != std::string::npos;
  };
  // Not flagged: the line is an ordinary request and its bytes may still be in the request
  // buffer afterwards - that is what the flag is for.
  REQUIRE(bench.client.cli(link::text(secret.data(), secret.size())).ok());
  CHECK(holds(secret));
  // Flagged: gone, both the payload and the frame.
  REQUIRE(bench.client.cli(link::text(secret.data(), secret.size()), true).ok());
  CHECK_FALSE(holds(secret));
  // And the radio did get it both times.
  int carried = 0;
  for (const sim::ScriptedRadio::Seen& one : bench.radio.requests) {
    const std::string payload(one.payload.begin(), one.payload.end());
    carried += payload.find(secret) != std::string::npos;
  }
  CHECK(carried == 2);
}

TEST_CASE("an event is taken out of the queue by type, the others left in order") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  bench.radio.raiseAttention(4, 0, 1);
  bench.radio.sendEvent("mesh reply: a cli reply");
  bench.radio.raiseAttention(2, 3700, 1);
  bench.clock.advance(400);
  bench.client.poll();
  REQUIRE(bench.client.eventsQueued() == 3);

  lc::QueuedEvent event;
  CHECK(bench.client.takeEvent(link::type::kEvBoot, &event) == false);
  CHECK(bench.client.eventsQueued() == 3);
  REQUIRE(bench.client.takeEvent(link::type::kEvMeshReply, &event));
  CHECK(event.type == link::type::kEvMeshReply);
  CHECK(bench.client.eventsQueued() == 2);
  REQUIRE(bench.client.takeEvent(&event));
  CHECK(event.type == link::type::kEvAttention);
  link::EvAttention first;
  REQUIRE(link::decode(event.view(), &first));
  CHECK(first.reason == 4);
  REQUIRE(bench.client.takeEvent(&event));
  link::EvAttention second;
  REQUIRE(link::decode(event.view(), &second));
  CHECK(second.reason == 2);
  CHECK(bench.client.eventsQueued() == 0);
  CHECK(bench.client.takeEvent(link::type::kEvMeshReply, &event) == false);
}
