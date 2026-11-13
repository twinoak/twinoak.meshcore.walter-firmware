// Who this exit node is (task p4-04): `agent::Identity` filled from the scripted radio of task
// p1-02 answering §5.1, §5.6 and §5.7 out of the specification's own fixtures - whose `cli
// request` *is* `get radio` and whose `cli response` *is* `> 869.618,62.5,8,8`, the same four
// numbers meshmaster.dk's `RadioParameters.json` carries.
//
// What is held here: every field's source, the radio block's parse, a `boot_id` change, a radio
// with no identity, **a radio that refuses to have one** (task p10-03: repeater v0.1.2's lost
// mode, and the heartbeat counter that tells meshmaster.dk which of the two it is), and the
// per-band intent.
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/wire.h"
#include "core/link/radio_identity.h"
#include "core/power/interval_policy.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/scripted_radio.h"
#include "walter_agent/identity.h"

namespace link = core::link;
namespace power = core::power;

namespace {

// The fixtures' own: `identity response`'s key and name, `hello response`'s three strings.
constexpr const char* kKeyHex =
    "7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c";
constexpr const char* kName = "TwinOak Risskov";

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  core::board::RadioPins radio{pins, clock, retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  link::client::LinkPort port{radio, rak};
  link::client::Client client{port, clock, wall, slots};
  agent::Identity identity{client, "0.4.0+p4test"};

  Rig() {
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
  }

  void hello() { REQUIRE(client.hello().ok()); }

  // The text of the first §5.7 line this radio was sent.
  std::string cliLine() const {
    for (const sim::ScriptedRadio::Seen& one : rak.requests) {
      if (one.type != link::type::kCli) continue;
      return std::string(one.payload.begin(), one.payload.end());
    }
    return std::string();
  }

  std::size_t requestsOf(std::uint8_t type) const {
    std::size_t n = 0;
    for (const sim::ScriptedRadio::Seen& one : rak.requests) n += one.type == type;
    return n;
  }
};

}  // namespace

TEST_CASE("the radio block is one CLI line, and only a reply with all four numbers is one") {
  mm::RadioParameters out;

  SUBCASE("the fixtures' own reply, which is the repeater's console's") {
    REQUIRE(agent::parseRadioLine("> 869.618,62.5,8,8", 18, &out));
    // Byte for byte what meshmaster.dk's RadioParameters carries: the doubles are the nearest to
    // those decimals, which is what a `double` of 869.618 is on both ends.
    CHECK(out.freqMHz == 869.618);
    CHECK(out.bwKHz == 62.5);
    CHECK(out.sf == 8);
    CHECK(out.cr == 8);
  }

  SUBCASE("the `> ` prefix is the console's manner and not the value") {
    REQUIRE(agent::parseRadioLine("869.618,62.5,8,8", 16, &out));
    CHECK(out.sf == 8);
  }

  SUBCASE("a reply this end half understood is no radio block at all") {
    const char* const refused[] = {
        "> 869.618,62.5,8",          // three numbers
        "> 869.618,62.5,8,8,9",      // four and then something - the fourth still parses
        "> unknown config: radio",   // what the repeater says for a key it does not know
        "> ",                        // nothing
        "> 869.618;62.5;8;8",        // not commas
        "> 100.0,62.5,8,8",          // below the band the repeater would take
        "> 869.618,62.5,4,8",        // SF 4: not one a radio is programmed with
        "> 869.618,62.5,8,9",        // CR 9: the same
        "> 869.618,600,8,8",         // 600 kHz: wider than the radio has
    };
    for (const char* line : refused) {
      mm::RadioParameters before = out;
      const bool took = agent::parseRadioLine(line, std::string(line).size(), &out);
      if (std::string(line) == "> 869.618,62.5,8,8,9") {
        CHECK_MESSAGE(took, "the four are there; what follows them is not this end's business");
        continue;
      }
      CHECK_MESSAGE(!took, line);
      CHECK(out.freqMHz == before.freqMHz);
    }
  }
}

TEST_CASE("every field of the identity comes from the radio or from this firmware's own table") {
  Rig rig;
  rig.hello();
  REQUIRE(rig.identity.refresh());

  // The repeater's own key, which is this exit node's: §5.6.
  CHECK(std::string(rig.identity.publicKeyHex()) == kKeyHex);
  CHECK(rig.identity.publicKey() != nullptr);
  CHECK(std::string(rig.identity.name()) == kName);
  // §5.1: the release version, the board, and what it is built on - three strings of one HELLO,
  // and no request of their own.
  CHECK(std::string(rig.identity.companionFirmware()) == "0.1.0+abc1234");
  CHECK(std::string(rig.identity.board()) == "rak4630-single");
  CHECK(std::string(rig.identity.upstream()) == "repeater-v1.17.1+d9296435");
  // §5.7: one `get radio` line.
  CHECK(rig.identity.radio().known);
  CHECK(rig.identity.radio().parameters.sf == 8);
  CHECK(rig.cliLine() == "get radio");

  // This firmware's own three.
  const mm::HeartbeatIdentity& id = rig.identity.heartbeatIdentity();
  CHECK(std::string(id.agentVersion) == "0.4.0+p4test");
  CHECK(std::string(id.board) == "walter");
  CHECK_MESSAGE(std::string(id.transport) == "native",
                "meshmaster.dk's word for an exit node that *is* its repeater (EXIT-NODES §2)");
  CHECK(std::string(id.companionFirmware) == "0.1.0+abc1234");

  // Two requests in all, and a second refresh makes none: the key is read once a boot and the
  // radio block once after it.
  CHECK(rig.requestsOf(link::type::kIdentity) == 1u);
  CHECK(rig.requestsOf(link::type::kCli) == 1u);
  REQUIRE(rig.identity.refresh());
  CHECK(rig.requestsOf(link::type::kIdentity) == 1u);
  CHECK(rig.requestsOf(link::type::kCli) == 1u);
  CHECK(rig.identity.counters().refreshes == 1u);
}

TEST_CASE("with no session nothing is asked and nothing is known: a HELLO is somebody else's") {
  Rig rig;
  CHECK_FALSE(rig.identity.refresh());
  CHECK(rig.identity.publicKeyHex() == nullptr);
  CHECK_FALSE(rig.identity.sessionUp());
  CHECK(rig.rak.requests.empty());
}

TEST_CASE("a boot_id change reads the key and the radio block again: §5.1") {
  Rig rig;
  rig.hello();
  REQUIRE(rig.identity.refresh());
  REQUIRE(rig.requestsOf(link::type::kIdentity) == 1u);

  // The radio restarted. The client notices at the next HELLO, and the identity with it.
  rig.rak.restart();
  rig.hello();
  REQUIRE(rig.identity.refresh());
  CHECK(rig.identity.counters().boot_changes == 1u);
  CHECK(rig.requestsOf(link::type::kIdentity) == 2u);
  CHECK(rig.requestsOf(link::type::kCli) == 2u);
  CHECK(std::string(rig.identity.publicKeyHex()) == kKeyHex);
}

TEST_CASE("after a local `set` line the radio block is read again, and the key is not") {
  Rig rig;
  rig.hello();
  REQUIRE(rig.identity.refresh());
  REQUIRE(rig.requestsOf(link::type::kCli) == 1u);

  // What a `config.apply` job for the unit's own repeater does: `set` lines over §5.7's CLI. The
  // settings it changed may be the four the heartbeat carries, so the block is stale.
  rig.identity.radioSettingsStale();
  REQUIRE(rig.identity.refresh());
  CHECK(rig.requestsOf(link::type::kCli) == 2u);
  CHECK_MESSAGE(rig.requestsOf(link::type::kIdentity) == 1u, "the key did not change");
  CHECK(rig.identity.radio().known);
}

TEST_CASE("a radio with no identity: the heartbeat carries no key, and the key is asked for again") {
  Rig rig;
  // A RAK with no application yet answers §5.6 with a key of all zeros, which is not a key.
  bool zero_key = true;
  rig.rak.on_request = [&zero_key](const link::Packet& packet,
                                   std::vector<std::uint8_t>* payload) {
    if (packet.type != link::type::kIdentity || !zero_key) return false;
    payload->assign(1 + link::kPubkeySize + 1, 0);
    (*payload)[0] = link::status::kOk;
    return true;
  };
  rig.hello();
  CHECK_FALSE(rig.identity.refresh());
  CHECK(rig.identity.publicKeyHex() == nullptr);
  CHECK(rig.identity.publicKey() == nullptr);
  CHECK(rig.identity.counters().no_identity == 1u);
  CHECK_MESSAGE(rig.requestsOf(link::type::kCli) == 0u,
                "no key, no radio block: there is nothing to say about a radio we have not identified");

  // It is asked for again at the next wake, because that is where a key appears: a unit is
  // registered before its repeater, and the first heartbeat after the radio has one carries it.
  CHECK_FALSE(rig.identity.refresh());
  CHECK(rig.requestsOf(link::type::kIdentity) == 2u);

  // And then it does.
  zero_key = false;
  CHECK(rig.identity.refresh());
  CHECK(std::string(rig.identity.publicKeyHex()) == kKeyHex);
  CHECK(rig.requestsOf(link::type::kCli) == 1u);
}

TEST_CASE("a radio that refuses: ERR_STATE is a lost identity and not a radio awaiting its first install") {
  Rig rig;
  // Repeater v0.1.2's lost mode, as that firmware answers in it (sim/scripted_radio.h, from its
  // tasks/phase-3.9/reports/01.md): HELLO as always with caps 0x21, IDENTITY answered ERR_STATE.
  rig.rak.loseIdentity();
  rig.hello();
  CHECK_FALSE(rig.identity.refresh());

  // The three states, where there used to be two. Before this task both this radio and a RAK
  // with no application answered `known() == false` and nothing told them apart.
  CHECK(rig.identity.identityState() == link::IdentityState::kLost);
  CHECK(rig.identity.identityLost());
  CHECK_MESSAGE(rig.identity.capsSayIdentityLost(),
                "the evidence beside the refusal: HELLO advertises CLI and ATTENTION and nothing else");
  CHECK(rig.client.session().caps == link::kCapsIdentityLost);

  // And what it costs the unit: no key, so no local door and no mesh job - exactly as for a
  // radio with no key at all, because that part of it is the same fact.
  CHECK(rig.identity.publicKeyHex() == nullptr);
  CHECK(rig.identity.publicKey() == nullptr);
  CHECK_FALSE(rig.identity.radio().known);
  CHECK(rig.requestsOf(link::type::kCli) == 0u);
  // Counted apart: `no_identity` is every answer that left this end with no key, and
  // `identity_lost` is the ones that were a refusal.
  CHECK(rig.identity.counters().no_identity == 1u);
  CHECK(rig.identity.counters().identity_lost == 1u);
  CHECK_MESSAGE(rig.identity.counters().identity_reads == 0u,
                "a refusal is not an answer with a key in it");

  // It is asked again at the next wake, as a radio with no key is: a person may have typed
  // `erase` and `reboot` between two wakes. Still a refusal, and counted again.
  CHECK_FALSE(rig.identity.refresh());
  CHECK(rig.requestsOf(link::type::kIdentity) == 2u);
  CHECK(rig.identity.counters().identity_lost == 2u);

  // The way out is a person's, and after it the radio is a new node with a key.
  rig.rak.keepIdentity();
  rig.rak.restart(false);
  rig.client.radioWasReset();
  rig.hello();
  CHECK(rig.identity.refresh());
  CHECK(rig.identity.identityState() == link::IdentityState::kKnown);
  CHECK_FALSE(rig.identity.identityLost());
  CHECK_FALSE(rig.identity.capsSayIdentityLost());
  CHECK(std::string(rig.identity.publicKeyHex()) == kKeyHex);
}

TEST_CASE("the heartbeat's counters say which of the three the radio is in") {
  // The library's own six are written at every heartbeat (agent/AgentLoop.cpp), and this board's
  // `radioIdentityLost` goes behind them **only while the radio refuses**: meshmaster.dk's
  // `errors` is an open key space, so the one thing its page could not show costs no new field.
  mm::ErrorCounter errors[agent::Identity::kErrorCount];
  mm::Heartbeat hb;
  const agent::HeartbeatExtras extras;
  const mm::WakeIntent intent;

  SUBCASE("a radio that refuses: the seventh counter says so") {
    Rig rig;
    rig.rak.loseIdentity();
    rig.hello();
    REQUIRE_FALSE(rig.identity.refresh());
    rig.identity.fillHeartbeat(&hb, nullptr, nullptr, extras, intent, errors,
                               agent::Identity::kErrorCount);
    // The library's six, this board's seventh, and the four of task p13-02 behind it.
    REQUIRE(hb.errorCount == agent::Identity::kErrorCount);
    CHECK(std::string(hb.errors[6].name) == "radioIdentityLost");
    CHECK(std::string(hb.errors[6].name) == std::string(agent::kRadioIdentityLostKey));
    CHECK(hb.errors[6].value == 1);
    // Nothing else about the heartbeat changes: a unit whose radio refuses still checks in.
    CHECK(hb.companionPublicKey == nullptr);
    CHECK_FALSE(hb.hasRadio);
    CHECK_FALSE(hb.companionConnected);
  }

  SUBCASE("a radio awaiting its first install: no seventh counter") {
    Rig rig;
    bool zero_key = true;
    rig.rak.on_request = [&zero_key](const link::Packet& packet,
                                     std::vector<std::uint8_t>* payload) {
      if (packet.type != link::type::kIdentity || !zero_key) return false;
      payload->assign(1 + link::kPubkeySize + 1, 0);
      (*payload)[0] = link::status::kOk;
      return true;
    };
    rig.hello();
    REQUIRE_FALSE(rig.identity.refresh());
    REQUIRE(rig.identity.identityState() == link::IdentityState::kNotMadeYet);
    rig.identity.fillHeartbeat(&hb, nullptr, nullptr, extras, intent, errors,
                               agent::Identity::kErrorCount);
    CHECK(hb.errorCount == agent::Identity::kLibraryErrorCount + core::cycle::kWakeCounterCount);
  }

  SUBCASE("a radio with its key: no seventh counter") {
    Rig rig;
    rig.hello();
    REQUIRE(rig.identity.refresh());
    rig.identity.fillHeartbeat(&hb, nullptr, nullptr, extras, intent, errors,
                               agent::Identity::kErrorCount);
    CHECK(hb.errorCount == agent::Identity::kLibraryErrorCount + core::cycle::kWakeCounterCount);
  }
}

TEST_CASE("the counter is added in one place, and to the heartbeat the library built") {
  mm::ErrorCounter errors[8] = {};
  errors[0] = mm::ErrorCounter{"overheard", 41};
  CHECK(agent::addRadioIdentityLost(errors, 1, 8, false) == 1);
  CHECK(agent::addRadioIdentityLost(errors, 1, 8, true) == 2);
  CHECK(std::string(errors[1].name) == std::string(agent::kRadioIdentityLostKey));
  CHECK(errors[1].value == 1);
  CHECK_MESSAGE(agent::addRadioIdentityLost(errors, 8, 8, true) == 8,
                "no room: the counters the library wrote are kept and nothing is added");
  CHECK(agent::addRadioIdentityLost(nullptr, 0, 8, true) == 0);
}

TEST_CASE("a radio whose console does not know `get radio`: no radio block, and the key stands") {
  Rig rig;
  rig.rak.on_request = [](const link::Packet& packet, std::vector<std::uint8_t>* payload) {
    if (packet.type != link::type::kCli) return false;
    const char* text = "unknown config: radio";
    payload->push_back(link::status::kOk);
    for (const char* at = text; *at != 0; ++at) payload->push_back(static_cast<std::uint8_t>(*at));
    return true;
  };
  rig.hello();
  CHECK(rig.identity.refresh());
  CHECK_FALSE(rig.identity.radio().known);
  CHECK(rig.identity.counters().unreadable == 1u);
  CHECK(std::string(rig.identity.publicKeyHex()) == kKeyHex);
  // It is asked again at the next wake: a firmware that was updated between two wakes may answer.
  CHECK(rig.identity.refresh());
  CHECK(rig.requestsOf(link::type::kCli) == 2u);
}

TEST_CASE("a radio that was turned into a scanner: the identity is forgotten, not kept") {
  Rig rig;
  rig.hello();
  REQUIRE(rig.identity.refresh());
  REQUIRE(rig.identity.known());

  // §9: the radio is running NoiseScope now. The client's session is gone with the reset.
  rig.rak.scanner = true;
  rig.client.radioWasReset();
  CHECK_FALSE(rig.identity.refresh());
  CHECK(rig.identity.publicKeyHex() == nullptr);
  CHECK_FALSE(rig.identity.radio().known);
}

TEST_CASE("the intent follows the band, and the interval is the policy's") {
  // The cap, band by band: the table of walter_agent/identity.h, held to its own sentences.
  CHECK(agent::intent::forBand(power::Band::kPlenty) == mm::kClaimMax * agent::intent::kLargestJob);
  CHECK(agent::intent::forBand(power::Band::kNormal) == agent::intent::kPlenty / 2);
  CHECK(agent::intent::forBand(power::Band::kSaving) == agent::intent::kSaving);
  CHECK_MESSAGE(agent::intent::forBand(power::Band::kCritical) == 0,
                "the modem is not brought up at all in `critical`, so the intent is moot");
  CHECK_MESSAGE(agent::intent::forBand(power::Band::kUnknown) == agent::intent::kNormal,
                "no reading is its own case and takes the medium one, as kUnknownMinutes does");

  // And the interval is the duty cycle's decision, in seconds, which is what the server accepts
  // between 60 and 86 400 and answers as the poll interval.
  power::Decision decision;
  decision.band = power::Band::kPlenty;
  decision.interval_seconds = power::kPlentyMinutes * 60u;
  const mm::WakeIntent intent = agent::intentFor(decision);
  CHECK(intent.nextCheckInS == 900u);
  CHECK(intent.maxTransactionsThisWake == agent::intent::kPlenty);

  // A `critical` wake proposes nothing at all, which the library writes as *no field*: better
  // than promising a wake that will not use the modem.
  const mm::WakeIntent none = agent::intentFor(power::Band::kCritical, power::kCriticalMinutes * 60u);
  CHECK(none.maxTransactionsThisWake == 0);
}
