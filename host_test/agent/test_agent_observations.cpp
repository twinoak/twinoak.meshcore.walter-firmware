// What the radio heard, into the library's ring (task p4-04): `agent::Observations` draining
// §7's `OBS_PULL` from a simulated RAK and mapping each record into `mm::HeardAdvert`, field for
// field.
//
// The two records every case here uses are **the specification's own fixture** (`observations:
// two adverts` of spec/fixtures/link/link-v1.json): a repeater with a position and a name, and a
// chat node two hops away with no RSSI. Their `app_data` is MeshCore's advert payload, so the
// decode is held against the bytes a real advert carries and not against a transcription.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/wire.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/quiet_repeater.h"
#include "sim/sim_rak.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/observations.h"

namespace link = core::link;

namespace {

std::vector<std::uint8_t> hex(const std::string& text) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
    const auto digit = [](char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return 0;
    };
    out.push_back(static_cast<std::uint8_t>((digit(text[i]) << 4) | digit(text[i + 1])));
  }
  return out;
}

// The fixture's two records.
sim::SimRak::Advert theRepeater() {
  sim::SimRak::Advert a;
  a.pubkey = hex("a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90");
  a.adv_ts = 1791719000;
  a.age_s = 41;
  a.snr_q4 = 29;
  a.rssi = -84;
  a.path_len = 0;  // a direct neighbour: no path, no hops
  a.app_data = hex("9248cb5c0330fca5004772656e616120233031");
  return a;
}

sim::SimRak::Advert theChatNode() {
  sim::SimRak::Advert a;
  a.pubkey = hex("7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c");
  a.adv_ts = 1791718000;
  a.age_s = 300;
  a.snr_q4 = -10;
  a.rssi = 0x7FFF;  // §7: no RSSI
  a.path_len = 66;  // bits 7-6 = 1 -> two bytes a hop; bits 5-0 = 2 -> two hops
  a.path = hex("a1b27f3a");
  a.app_data = hex("8157696e646f77");
  return a;
}

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  sim::QuietRepeater repeater;
  sim::SimRak rak{clock, repeater};
  fakes::FakeWallClock wall;
  core::board::RadioPins radio{pins, clock, retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  link::client::LinkPort port{radio, rak};
  link::client::Client client{port, clock, wall, slots};
  agent::LinkMesh mesh{client, clock};
  mm::ObservationRing ring;
  agent::Observations observations{client, mesh};

  Rig() {
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    REQUIRE(client.hello().ok());
    mesh.setRing(&ring);
  }

  std::size_t pullsOnTheWire() const {
    std::size_t n = 0;
    for (const sim::ScriptedRadio::Seen& one : rak.radio.requests) {
      n += one.type == link::type::kObsPull;
    }
    return n;
  }
};

}  // namespace

TEST_CASE("MeshCore's advert payload into the three fields the library's struct has for it") {
  mm::HeardAdvert out;

  SUBCASE("a repeater with a position and a name") {
    const std::vector<std::uint8_t> bytes = hex("9248cb5c0330fca5004772656e616120233031");
    REQUIRE(agent::parseAdvertData(bytes.data(), bytes.size(), &out));
    CHECK_MESSAGE(out.advType == 2, "ADV_TYPE_REPEATER, the low nibble of the flags byte");
    CHECK(std::string(mm::roleName(out.advType)) == "repeater");
    CHECK(out.hasPosition);
    CHECK(out.latE6 == 56413000);
    CHECK(out.lonE6 == 10878000);
    CHECK(std::string(out.name) == "Grenaa #01");
  }

  SUBCASE("a chat node with a name and no position") {
    const std::vector<std::uint8_t> bytes = hex("8157696e646f77");
    REQUIRE(agent::parseAdvertData(bytes.data(), bytes.size(), &out));
    CHECK(out.advType == 1);
    CHECK(std::string(mm::roleName(out.advType)) == "companion");
    CHECK_FALSE(out.hasPosition);
    CHECK(std::string(out.name) == "Window");
  }

  SUBCASE("the two future feature fields are stepped over, so the name begins where it begins") {
    // ADV_FEAT1_MASK | ADV_FEAT2_MASK | ADV_NAME_MASK | ADV_TYPE_SENSOR, with two u16 before the
    // name (AdvertDataBuilder::encodeTo at d9296435). Neither has a field in the library's
    // struct, and the report names that as a gap - but the name must not be read out of them.
    const std::vector<std::uint8_t> bytes = hex("e4" "1234" "5678" "4e6f7265");
    REQUIRE(agent::parseAdvertData(bytes.data(), bytes.size(), &out));
    CHECK(out.advType == 4);
    CHECK(std::string(out.name) == "Nore");
  }

  SUBCASE("an advert that said nothing at all is not an error") {
    REQUIRE(agent::parseAdvertData(nullptr, 0, &out));
    CHECK(out.advType == mm::kAdvTypeUnknown);
    CHECK(out.name[0] == 0);
  }

  SUBCASE("a payload shorter than its own flags say is refused, and nothing is guessed") {
    const std::vector<std::uint8_t> bytes = hex("9248cb5c");  // latlon, four bytes of eight
    CHECK_FALSE(agent::parseAdvertData(bytes.data(), bytes.size(), &out));
  }

  SUBCASE("a name that is not UTF-8 is cut where it stops being UTF-8") {
    // 0x80 is a continuation byte with nothing to continue: the library's own rule bounds a name
    // before it reaches a JSON string, because a name is whatever a stranger typed.
    const std::vector<std::uint8_t> bytes = hex("81" "4f6b80" "4e6f");
    REQUIRE(agent::parseAdvertData(bytes.data(), bytes.size(), &out));
    CHECK(std::string(out.name) == "Ok");
  }

  SUBCASE("a name longer than the library's field is cut at the field") {
    std::vector<std::uint8_t> bytes = hex("81");
    for (std::size_t i = 0; i < 40; ++i) bytes.push_back('x');
    REQUIRE(agent::parseAdvertData(bytes.data(), bytes.size(), &out));
    CHECK(std::string(out.name).size() == mm::kMaxObservationNameLen);
  }
}

TEST_CASE("one wake's drain: every field of §7's record in the library's struct") {
  Rig rig;
  rig.rak.observations.push_back(theRepeater());
  rig.rak.observations.push_back(theChatNode());
  rig.clock.advance(600000);  // ten minutes of uptime, so a back-dated advert has room

  const std::uint32_t now = static_cast<std::uint32_t>(rig.clock.nowMs());
  CHECK(rig.observations.drain(now) == 2u);
  REQUIRE(rig.ring.size() == 2u);

  const mm::HeardAdvert& first = rig.ring.at(0);  // 0 is the oldest, as the RAK handed them out
  CHECK(first.publicKey[0] == 0xa1);
  CHECK_MESSAGE(first.heardAtMillis == now - 41u * 1000u,
                "§7's age_s is measured by the radio, so this is when it was heard");
  CHECK_MESSAGE(first.heardAtEpoch == 0u,
                "the library stamps a pushed advert from heardAtMillis: the mesh never sets this");
  CHECK(first.snrQ4 == 29);
  CHECK(first.rssi == -84);
  CHECK(first.hops == 0);
  CHECK(first.pathByteLen == 0);
  CHECK(first.hasPosition);
  CHECK(std::string(first.name) == "Grenaa #01");

  const mm::HeardAdvert& second = rig.ring.at(1);
  CHECK(second.publicKey[0] == 0x7f);
  CHECK(second.heardAtMillis == now - 300u * 1000u);
  CHECK(second.snrQ4 == -10);
  CHECK_MESSAGE(second.rssi == mm::kNoRssi, "§7's 0x7FFF is no RSSI, and absent is not zero");
  CHECK_MESSAGE(second.hops == 2, "path_len's low six bits");
  CHECK_MESSAGE(second.pathHashSize == 2, "path_len's top two bits, plus one");
  CHECK(second.pathByteLen == 4);
  CHECK(second.pathBytes[0] == 0xa1);
  CHECK(std::string(second.name) == "Window");

  // One request, and the RAK's ring is empty afterwards: records handed out are gone from it.
  CHECK(rig.pullsOnTheWire() == 1u);
  CHECK(rig.rak.observations.empty());
  CHECK(rig.observations.counters().records == 2u);
  CHECK(rig.observations.counters().kept == 2u);
}

TEST_CASE("a ring of more than one payload is drained in pages, and the drain ends when it is empty") {
  Rig rig;
  for (int i = 0; i < 12; ++i) {
    sim::SimRak::Advert a = theChatNode();
    a.pubkey[0] = static_cast<std::uint8_t>(i);
    a.age_s = static_cast<std::uint32_t>(i);
    rig.rak.observations.push_back(a);
  }
  rig.clock.advance(60000);
  CHECK(rig.observations.drain(static_cast<std::uint32_t>(rig.clock.nowMs())) == 12u);
  // §7 answers at most five of the smallest record in one payload (link::kMaxObservations), so
  // twelve is three pages - and the third says `remaining` 0, which is what ends the loop.
  CHECK(rig.pullsOnTheWire() == 3u);
  CHECK(rig.ring.size() == 12u);
  CHECK(rig.observations.counters().remaining == 0u);
}

TEST_CASE("a record about this unit's own repeater is dropped before it is kept") {
  Rig rig;
  sim::SimRak::Advert own = theRepeater();
  rig.rak.observations.push_back(own);
  rig.rak.observations.push_back(theChatNode());
  rig.observations.useOwnKey(own.pubkey.data());

  CHECK(rig.observations.drain(0) == 1u);
  CHECK(rig.ring.size() == 1u);
  CHECK_MESSAGE(rig.ring.at(0).publicKey[0] == 0x7f, "the other one");
  CHECK(rig.observations.counters().own_key == 1u);
  CHECK(rig.observations.counters().records == 2u);
}

TEST_CASE("without this unit's key nothing is dropped as its own: a radio with no identity yet") {
  Rig rig;
  rig.rak.observations.push_back(theRepeater());
  rig.observations.useOwnKey(nullptr);
  CHECK(rig.observations.drain(0) == 1u);
  CHECK(rig.observations.counters().own_key == 0u);
}

TEST_CASE("the radio's own dropped count reaches the heartbeat's figure, and only the increase") {
  Rig rig;
  // §6.4's `obs_dropped` is since the radio booted, and the Walter woke to a radio that has been
  // up for a week: the first reading is a baseline and not this wake's loss.
  rig.rak.obs_dropped = 40;
  REQUIRE(rig.mesh.refreshCounters());
  rig.observations.reconcileDrops();
  CHECK_MESSAGE(rig.ring.dropped() == 0u, "nobody could have collected what was lost before we woke");

  // Then seven more are lost while we are awake.
  rig.rak.obs_dropped = 47;
  REQUIRE(rig.mesh.refreshCounters());
  rig.observations.reconcileDrops();
  CHECK(rig.ring.dropped() == 7u);
  CHECK(rig.observations.counters().radio_dropped == 47u);

  // Nothing new: nothing is added twice.
  REQUIRE(rig.mesh.refreshCounters());
  rig.observations.reconcileDrops();
  CHECK(rig.ring.dropped() == 7u);

  // The radio restarted and counts again from a smaller number: what it says now is the whole of
  // what has been lost since.
  rig.rak.obs_dropped = 3;
  REQUIRE(rig.mesh.refreshCounters());
  rig.observations.reconcileDrops();
  CHECK(rig.ring.dropped() == 10u);
}

TEST_CASE("§7's per-pull `dropped` is recorded for the console and not added: one loss, one count") {
  Rig rig;
  rig.rak.obs_dropped_since_pull = 5;
  rig.rak.obs_dropped = 5;
  rig.rak.observations.push_back(theChatNode());
  CHECK(rig.observations.drain(0) == 1u);
  CHECK(rig.observations.counters().pull_dropped == 5u);
  CHECK_MESSAGE(rig.ring.dropped() == 0u,
                "the first reading of §6.4's count is a baseline; adding §7's as well would count it twice");
  // And the pull cleared the radio's per-pull figure, as §7 says.
  CHECK(rig.rak.obs_dropped_since_pull == 0u);
}

TEST_CASE("a radio with no OBSERVATIONS capability is asked nothing, and the drain says so") {
  Rig rig;
  rig.rak.radio.caps = link::cap::kCli;
  REQUIRE(rig.client.hello().ok());
  rig.rak.observations.push_back(theChatNode());
  CHECK(rig.observations.drain(0) == 0u);
  CHECK(rig.observations.counters().faults == 1u);
  CHECK_MESSAGE(rig.pullsOnTheWire() == 0u, "a call whose capability HELLO did not advertise sends no byte");
}

TEST_CASE("a wake's drain is bounded: a radio that keeps answering `remaining` cannot hold it open") {
  Rig rig;
  // Four of these records fit a payload (45 bytes each plus a four-byte path and seven of
  // application data, against §2's 240), so kMaxPullsPerWake is what ends this and not the ring.
  for (int i = 0; i < 200; ++i) {
    sim::SimRak::Advert a = theChatNode();
    a.pubkey[0] = static_cast<std::uint8_t>(i);
    rig.rak.observations.push_back(a);
  }
  const std::size_t kept = rig.observations.drain(0);
  CHECK(rig.pullsOnTheWire() == agent::kMaxPullsPerWake);
  CHECK_MESSAGE(kept == 4u * agent::kMaxPullsPerWake,
                "four of this record fit one payload, and sixteen pulls is the wake's bound");
  CHECK_MESSAGE(rig.observations.counters().remaining != 0u,
                "the radio still has records: the next wake's drain takes them");
}

TEST_CASE("a drain with no ring - the loop has not handed one over yet - loses nothing on the wire") {
  Rig rig;
  rig.mesh.setRing(nullptr);
  rig.rak.observations.push_back(theChatNode());
  // The records are read and counted; with nowhere to put them they are not kept. That is the
  // state between this task and task p4-05, and `agent obs` says it in one line.
  CHECK(rig.observations.drain(0) == 1u);
  CHECK(rig.ring.size() == 0u);
  CHECK(rig.observations.counters().kept == 1u);
}

TEST_CASE("the library's ring drops its oldest when full, and that is the figure the heartbeat carries") {
  Rig rig;
  for (std::size_t i = 0; i < mm::ObservationRing::kCapacity + 3; ++i) {
    // The smallest record there is - no path, no application data - so that what bounds this
    // drain is the library's ring and not the pulls or the payload.
    sim::SimRak::Advert a;
    a.pubkey = theChatNode().pubkey;
    a.pubkey[0] = static_cast<std::uint8_t>(i);
    rig.rak.observations.push_back(a);
  }
  CHECK(rig.observations.drain(0) == mm::ObservationRing::kCapacity + 3);
  CHECK(rig.ring.size() == mm::ObservationRing::kCapacity);
  CHECK(rig.ring.dropped() == 3u);
  CHECK(rig.ring.heard() == mm::ObservationRing::kCapacity + 3);
}
