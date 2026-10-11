// Every request case of spec/fixtures/link/link-v1.json, sent through the client to the scripted
// radio and answered with the fixture's response for that type (task p1-02's first test).
//
// What it holds the client to, case by case:
//
//   - the typed call built from the fixture's `fields` puts the fixture's `payload` on the wire,
//     under the fixture's `type` - so the call's signature and the encoder agree with the
//     specification, and not only with each other;
//   - the answer it gives back is the fixture response's fields, decoded: the same numbers the
//     radio firmware's own codec was tested against;
//   - a response the fixtures give as a refusal (SEND_TRACE's ERR_RANGE) comes back as a refusal
//     with that status, not as an answer and not as a malformed one.
//
// Two cases cannot be byte-for-byte the fixture's and say so where they happen: HELLO carries this
// firmware's own version string rather than the fixture's, and PATH_SET has no response case in
// the fixtures at all (§6.1: "Response: status only"), so the radio answers a synthesised one.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/link/client/client.h"
#include "core/version.h"
#include "fixtures/link_fields.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;
using fixtures::asHex;
using fixtures::asString;
using fixtures::hexOf;
using fixtures::i16;
using fixtures::i8;
using fixtures::textOf;
using fixtures::u16;
using fixtures::u32;
using fixtures::u8;
using fixtures::viewOf;
using jsonfix::Value;

namespace {

link::Path pathFrom(const Value& fields, std::vector<std::uint8_t>* hashes) {
  link::Path path;
  path.mode = u8(fields, "path_mode");
  if (path.mode == link::path_mode::kHops) {
    path.hash_size = u8(fields, "hash_size");
    path.hop_count = u8(fields, "hop_count");
    *hashes = hexOf(fields, "hashes");
    path.hashes = viewOf(*hashes);
  }
  return path;
}

void checkPath(const link::Path& path, const Value& fields) {
  CHECK_MESSAGE(path.mode == u8(fields, "path_mode"), "path_mode");
  if (path.mode != link::path_mode::kHops) {
    return;
  }
  CHECK_MESSAGE(path.hash_size == u8(fields, "hash_size"), "hash_size");
  CHECK_MESSAGE(path.hop_count == u8(fields, "hop_count"), "hop_count");
  CHECK_MESSAGE(asHex(path.hashes) == fixtures::toHex(hexOf(fields, "hashes")), "hashes");
}

// The status every answer below is held to: the fixture response's own. `want` is null for a type
// the fixtures have no response for, and then OK is what the radio synthesised.
std::uint8_t wantedStatus(const Value* want) {
  return want == nullptr ? static_cast<std::uint8_t>(link::status::kOk) : u8((*want)["fields"], "status");
}

// Every answer is checked the same way first: it is there when the status was OK, and it is a
// refusal with that status when it was not.
template <typename Response>
bool answered(const lc::Answer<Response>& answer, const Value* want) {
  const std::uint8_t status = wantedStatus(want);
  if (status == link::status::kOk) {
    CHECK_MESSAGE(answer.fault == lc::Fault::kNone, lc::describe(answer.fault));
    CHECK(answer.attempts == 1);
    return answer.ok();
  }
  CHECK(answer.fault == lc::Fault::kRefused);
  CHECK(answer.status == status);
  return false;
}

}  // namespace

TEST_CASE("every request case: the call puts the fixture's payload on the wire") {
  const Value& cases = fixtures::linkFixture()["cases"];
  std::size_t checked = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const Value& one = cases.at(i);
    if (one["kind"].string() != "request") {
      continue;
    }
    const std::string name = one["name"].string();
    const std::uint8_t type = u8(one, "type");
    const Value& fields = one["fields"];
    const std::vector<std::uint8_t> payload = hexOf(one, "payload");
    INFO("case: " << name << " (" << one["type_name"].string() << ")");

    sim::LinkBench bench;
    // Everything but HELLO itself needs a session: §4's capability column is what HELLO brings,
    // and a call whose capability is unknown is refused without a byte on the wire.
    if (type != link::type::kHello) {
      REQUIRE(bench.client.hello().ok());
    }
    const std::size_t before = bench.radio.requests.size();
    const Value* want = fixtures::responseCaseFor(type);

    switch (type) {
      case link::type::kHello: {
        const lc::Answer<link::HelloResponse> answer = bench.client.hello();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.body.proto_major == u8(wf, "proto_major"));
          CHECK(answer.message.body.proto_minor == u8(wf, "proto_minor"));
          CHECK(answer.message.body.slot == u8(wf, "slot"));
          CHECK(answer.message.body.max_payload == u8(wf, "max_payload"));
          CHECK(asString(answer.message.body.board) == textOf(wf, "board"));
          CHECK(asString(answer.message.body.upstream) == textOf(wf, "upstream"));
          CHECK(asString(answer.message.body.fw_version) == textOf(wf, "fw_version"));
          // The radio's own role, caps and boot_id are the scripted radio's, patched into the
          // fixture's body: a test can take a capability away or make it restart.
          CHECK(answer.message.body.role == bench.radio.role);
          CHECK(answer.message.body.caps == bench.radio.caps);
          CHECK(answer.message.body.boot_id == bench.radio.boot_id);
        }
        break;
      }
      case link::type::kPing: {
        const lc::Answer<link::PingResponse> answer = bench.client.ping(u32(fields, "nonce"));
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.nonce == u32(wf, "nonce"));
          CHECK(answer.message.uptime_s == u32(wf, "uptime_s"));
        }
        break;
      }
      case link::type::kStatus: {
        const lc::Answer<link::StatusResponse> answer = bench.client.status();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.uptime_s == u32(wf, "uptime_s"));
          CHECK(answer.message.reset_reason == u8(wf, "reset_reason"));
          CHECK(answer.message.time_state == u8(wf, "time_state"));
          CHECK(answer.message.epoch == u32(wf, "epoch"));
          CHECK(answer.message.flags == u16(wf, "flags"));
          CHECK(answer.message.obs_pending == u8(wf, "obs_pending"));
          CHECK(answer.message.obs_dropped == u16(wf, "obs_dropped"));
          CHECK(answer.message.link_errors == u16(wf, "link_errors"));
          CHECK(answer.message.attention == u8(wf, "attention"));
        }
        break;
      }
      case link::type::kTimeSet: {
        const lc::Answer<link::TimeSetResponse> answer =
            bench.client.timeSet(u32(fields, "epoch"), u8(fields, "source"),
                                 (u8(fields, "flags") & link::time_flag::kForce) != 0);
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.prev_epoch == u32(wf, "prev_epoch"));
          CHECK(answer.message.epoch == u32(wf, "epoch"));
          CHECK(answer.message.time_state == u8(wf, "time_state"));
        }
        break;
      }
      case link::type::kTimeGet: {
        const lc::Answer<link::TimeGetResponse> answer = bench.client.timeGet();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.epoch == u32(wf, "epoch"));
          CHECK(answer.message.time_state == u8(wf, "time_state"));
        }
        break;
      }
      case link::type::kReboot: {
        const lc::Answer<link::StatusOnly> answer =
            bench.client.reboot(u8(fields, "mode"), u16(fields, "delay_ms"));
        answered(answer, want);
        break;
      }
      case link::type::kIdentity: {
        const lc::Answer<link::IdentityResponse> answer = bench.client.identity();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(asHex(answer.message.pubkey) == fixtures::toHex(hexOf(wf, "pubkey")));
          CHECK(asString(answer.message.name) == textOf(wf, "name"));
        }
        break;
      }
      case link::type::kSleepNotice: {
        const lc::Answer<link::StatusOnly> answer =
            bench.client.sleepNotice(u32(fields, "expect_back_s"));
        answered(answer, want);
        break;
      }
      case link::type::kCli: {
        const std::string line = textOf(fields, "text");
        const lc::Answer<link::CliResponse> answer = bench.client.cli(viewOf(line));
        if (answered(answer, want)) {
          CHECK(asString(answer.message.text) == textOf((*want)["fields"], "text"));
        }
        break;
      }
      case link::type::kPowerGet: {
        const lc::Answer<link::PowerGetResponse> answer = bench.client.powerGet();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.valid == u8(wf, "valid"));
          // §5.8: channel 1 panel, 2 battery (positive = charging), 3 total load.
          const Value& channels = wf["channels"];
          REQUIRE(channels.size() == 3);
          for (std::size_t c = 0; c < 3; ++c) {
            CHECK(answer.message.channels[c].bus_mv == u16(channels.at(c), "bus_mv"));
            CHECK(answer.message.channels[c].current_ma == i16(channels.at(c), "current_ma"));
          }
        }
        break;
      }
      case link::type::kEnvGet: {
        const lc::Answer<link::EnvGetResponse> answer = bench.client.envGet();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.valid == u8(wf, "valid"));
          CHECK(answer.message.temp_centi == i16(wf, "temp_centi"));
          CHECK(answer.message.rh_centi == u16(wf, "rh_centi"));
          CHECK(answer.message.pressure_pa == u32(wf, "pressure_pa"));
        }
        break;
      }
      case link::type::kLppGet: {
        const lc::Answer<link::LppGetResponse> answer =
            bench.client.lppGet(u8(fields, "perm_mask"));
        if (answered(answer, want)) {
          CHECK(asHex(answer.message.lpp) == fixtures::toHex(hexOf((*want)["fields"], "lpp")));
        }
        break;
      }
      case link::type::kJobBegin: {
        const std::vector<std::uint8_t> pubkey = hexOf(fields, "pubkey");
        std::vector<std::uint8_t> hashes;
        const link::Path path = pathFrom(fields, &hashes);
        const lc::Answer<link::StatusOnly> answer = bench.client.jobBegin(viewOf(pubkey), path);
        answered(answer, want);
        break;
      }
      case link::type::kJobEnd: {
        const lc::Answer<link::StatusOnly> answer = bench.client.jobEnd();
        answered(answer, want);
        break;
      }
      case link::type::kPathSet: {
        std::vector<std::uint8_t> hashes;
        const link::Path path = pathFrom(fields, &hashes);
        const lc::Answer<link::StatusOnly> answer = bench.client.pathSet(path);
        // §6.1 says the response is a status and nothing else, and the fixtures have no case for
        // it: the radio synthesised one, which is what `want == nullptr` means here.
        CHECK(want == nullptr);
        CHECK(answered(answer, want));
        CHECK(answer.message.status == link::status::kOk);
        break;
      }
      case link::type::kPathGet: {
        const lc::Answer<link::PathGetResponse> answer = bench.client.pathGet();
        if (answered(answer, want)) {
          checkPath(answer.message.path, (*want)["fields"]);
        }
        break;
      }
      case link::type::kSendLogin: {
        const std::vector<std::uint8_t> password = hexOf(fields, "password");
        const lc::Answer<link::SendResponse> answer =
            bench.client.sendLogin(u32(fields, "timeout_ms"), viewOf(password));
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.send == u8(wf, "send"));
          CHECK(answer.message.tag == u32(wf, "tag"));
          CHECK(answer.message.sender_ts == u32(wf, "sender_ts"));
        }
        break;
      }
      case link::type::kSendRequest: {
        const std::vector<std::uint8_t> data = hexOf(fields, "data");
        const lc::Answer<link::SendResponse> answer = bench.client.sendRequest(
            u32(fields, "timeout_ms"), u8(fields, "req_type"), viewOf(data));
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.send == u8(wf, "send"));
          CHECK(answer.message.tag == u32(wf, "tag"));
          CHECK(answer.message.sender_ts == u32(wf, "sender_ts"));
        }
        break;
      }
      case link::type::kSendCli: {
        const std::string line = textOf(fields, "text");
        const lc::Answer<link::SendResponse> answer =
            bench.client.sendCli(u32(fields, "timeout_ms"), u8(fields, "flags"), viewOf(line));
        if (answered(answer, want)) {
          CHECK(answer.message.send == u8((*want)["fields"], "send"));
        }
        break;
      }
      case link::type::kSendDiscovery: {
        const lc::Answer<link::SendResponse> answer =
            bench.client.sendDiscovery(u32(fields, "timeout_ms"));
        if (answered(answer, want)) {
          CHECK(answer.message.send == u8((*want)["fields"], "send"));
        }
        break;
      }
      case link::type::kSendTrace: {
        const std::vector<std::uint8_t> hashes = hexOf(fields, "hashes");
        const lc::Answer<link::SendResponse> answer = bench.client.sendTrace(
            u32(fields, "timeout_ms"), u8(fields, "hash_size"), viewOf(hashes));
        // §6.2: a trace cannot express three-byte hashes, and the fixtures' only SEND_TRACE
        // response is that refusal - so this case proves the refusal path, not the happy one.
        answered(answer, want);
        break;
      }
      case link::type::kReplyGet: {
        const lc::Answer<link::ReplyGetResponse> answer = bench.client.replyGet();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.reply.state == u8(wf, "state"));
          CHECK(answer.message.reply.tag == u32(wf, "tag"));
          CHECK(answer.message.reply.via_path == u8(wf, "via_path"));
          CHECK(answer.message.reply.snr_q4 == i8(wf, "snr_q4"));
          CHECK(answer.message.reply.rssi == i16(wf, "rssi"));
          CHECK(answer.message.reply.elapsed_ms == u32(wf, "elapsed_ms"));
        }
        break;
      }
      case link::type::kObsPull: {
        const lc::Answer<link::ObsPullResponse> answer = bench.client.obsPull(u8(fields, "max"));
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.remaining == u8(wf, "remaining"));
          CHECK(answer.message.dropped == u16(wf, "dropped"));
          // The fixtures give the records and not a count: the count is how many there are.
          const Value& records = wf["records"];
          CHECK(static_cast<std::size_t>(answer.message.count) == records.size());
          REQUIRE(static_cast<std::size_t>(answer.message.count) == records.size());
          for (std::size_t r = 0; r < records.size(); ++r) {
            const Value& rec = records.at(r);
            const link::Observation& got = answer.message.records[r];
            CHECK(asHex(got.pubkey) == fixtures::toHex(hexOf(rec, "pubkey")));
            CHECK(got.adv_ts == u32(rec, "adv_ts"));
            CHECK(got.age_s == u32(rec, "age_s"));
            CHECK(got.snr_q4 == i8(rec, "snr_q4"));
            CHECK(got.rssi == i16(rec, "rssi"));
            CHECK(got.path_len == u8(rec, "path_len"));
            CHECK(asHex(got.path) == fixtures::toHex(hexOf(rec, "path")));
            CHECK(asHex(got.app_data) == fixtures::toHex(hexOf(rec, "app_data")));
          }
        }
        break;
      }
      case link::type::kMeshCounters: {
        const lc::Answer<link::MeshCountersResponse> answer = bench.client.meshCounters();
        if (answered(answer, want)) {
          const Value& wf = (*want)["fields"];
          CHECK(answer.message.rx == u32(wf, "rx"));
          CHECK(answer.message.tx == u32(wf, "tx"));
          CHECK(answer.message.client_tx == u32(wf, "client_tx"));
          CHECK(answer.message.adverts == u32(wf, "adverts"));
          CHECK(answer.message.adverts_dropped == u32(wf, "adverts_dropped"));
          CHECK(answer.message.obs_dropped == u32(wf, "obs_dropped"));
          CHECK(answer.message.noise_floor == i16(wf, "noise_floor"));
          CHECK(answer.message.last_rssi == i16(wf, "last_rssi"));
          CHECK(answer.message.last_snr_q4 == i8(wf, "last_snr_q4"));
        }
        break;
      }
      default:
        FAIL("no call for this request type: " << name);
        break;
    }

    // What reached the radio: one more request, of this type, with this payload.
    REQUIRE(bench.radio.requests.size() == before + 1);
    const sim::ScriptedRadio::Seen& seen = bench.radio.requests.back();
    CHECK(seen.type == type);
    if (type == link::type::kHello) {
      // §5.1's `fw_version` is this firmware's own, not the fixture's: the first four bytes are
      // the fixture's and the str8 that follows is core::version().
      REQUIRE(seen.payload.size() >= 5);
      CHECK(fixtures::toHex(seen.payload.data(), 4) == fixtures::toHex(payload.data(), 4));
      const std::string version(seen.payload.begin() + 5, seen.payload.end());
      CHECK(static_cast<std::size_t>(seen.payload[4]) == version.size());
      CHECK(version == std::string(core::version()));
    } else {
      CHECK(fixtures::toHex(seen.payload) == fixtures::toHex(payload));
    }
    CHECK(seen.executed);
    ++checked;
  }
  CHECK(checked == 30);  // the 30 request cases of the fixtures
}

TEST_CASE("the answer's views point into the client and survive until the next call") {
  // The decoded message's variable-length fields are views into the client's response buffer
  // (codec/view.h), not into the receiver's: a second frame read off the port - an event, say -
  // must not change what the last answer said.
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const lc::Answer<link::IdentityResponse> answer = bench.client.identity();
  REQUIRE(answer.ok());
  const std::string name = asString(answer.message.name);
  const std::string key = asHex(answer.message.pubkey);
  CHECK_FALSE(name.empty());

  bench.radio.sendEvent("attention: the battery is low");
  bench.client.poll();
  CHECK(bench.client.eventsQueued() == 1);
  CHECK(asString(answer.message.name) == name);
  CHECK(asHex(answer.message.pubkey) == key);
}
