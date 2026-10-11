// §3's one request at a time, and what the client does when an answer does not come (task p1-02).
//
//   - silence once and twice: the same frame again, the same seq, byte for byte - and the answer
//     when it comes;
//   - silence three times: *link down*, as a result and not an action;
//   - a late answer to an attempt that had already been resent: taken when it matches the seq
//     that is still in flight, counted as stray when nothing is;
//   - a response with another seq, and one whose CRC does not survive the wire;
//   - §3's remembered response: the radio does not execute a repeated (type, seq) twice, which is
//     what makes a retry safe for the sends that transmit on the mesh;
//   - the timing: one attempt waits one response timeout, and nothing inside the client blocks
//     for longer than kPollMs at a time.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/link/client/client.h"
#include "fixtures/link_fields.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;

namespace {

// The frames of one call, as the radio decoded them: hello's are stripped off first.
std::vector<sim::ScriptedRadio::Seen> after(const sim::LinkBench& bench, std::size_t from) {
  return std::vector<sim::ScriptedRadio::Seen>(bench.radio.requests.begin() + static_cast<long>(from),
                                               bench.radio.requests.end());
}

}  // namespace

TEST_CASE("silence once and twice: the same frame again, byte for byte, and then the answer") {
  for (const int silences : {1, 2}) {
    sim::LinkBench bench;
    REQUIRE(bench.client.hello().ok());
    const std::size_t from = bench.radio.requests.size();
    const int executed = bench.radio.executions;

    bench.radio.silence = silences;
    const lc::Answer<link::StatusResponse> answer = bench.client.status();
    INFO("silences: " << silences);
    REQUIRE(answer.ok());
    CHECK(answer.attempts == silences + 1);

    const std::vector<sim::ScriptedRadio::Seen> seen = after(bench, from);
    REQUIRE(seen.size() == static_cast<std::size_t>(silences) + 1);
    for (const sim::ScriptedRadio::Seen& one : seen) {
      CHECK(one.type == link::type::kStatus);
      CHECK(one.seq == seen.front().seq);                       // §3: the same seq
      CHECK(fixtures::toHex(one.frame) == fixtures::toHex(seen.front().frame));  // the same frame
    }
    // The frames were written whole, and each write is the frame with §2's idle 0x00 in front of
    // it: a second has passed since the last one, so the line was idle every time.
    const std::vector<std::vector<std::uint8_t>> writes = bench.frames();
    REQUIRE(writes.size() >= seen.size());
    for (std::size_t i = writes.size() - seen.size(); i < writes.size(); ++i) {
      CHECK(fixtures::toHex(writes[i]) == fixtures::toHex(seen.front().frame));
    }
    // The radio ignored the silenced attempts whole, so it executed the request once.
    CHECK(bench.radio.executions == executed + 1);
    CHECK(bench.client.counters().retries == static_cast<std::uint32_t>(silences));
    CHECK(bench.client.counters().timeouts == static_cast<std::uint32_t>(silences));
  }
}

TEST_CASE("silence three times is link down, and the link stays usable afterwards") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const std::size_t from = bench.radio.requests.size();

  bench.radio.deaf = true;
  const std::uint64_t at = bench.clock.nowMs();
  const lc::Answer<link::PingResponse> answer = bench.client.ping(0xdeadbeef);
  CHECK(answer.fault == lc::Fault::kLinkDown);
  CHECK(answer.attempts == link::kRequestAttempts);
  CHECK(bench.client.counters().link_downs == 1);

  const std::vector<sim::ScriptedRadio::Seen> seen = after(bench, from);
  REQUIRE(seen.size() == static_cast<std::size_t>(link::kRequestAttempts));
  for (const sim::ScriptedRadio::Seen& one : seen) {
    CHECK(one.seq == seen.front().seq);
    CHECK(fixtures::toHex(one.frame) == fixtures::toHex(seen.front().frame));
  }

  // §10: three attempts of a second each, and not a millisecond more than the slices it listens
  // in. "Link down" is a result: the session is untouched and the next call goes out as usual.
  const std::uint64_t took = bench.clock.nowMs() - at;
  CHECK(took >= link::kRequestAttempts * link::kResponseWaitMs);
  CHECK(took <= link::kRequestAttempts * (link::kResponseWaitMs + lc::kPollMs));
  CHECK(bench.client.session().up);
  bench.radio.deaf = false;
  CHECK(bench.client.ping(1).ok());
}

TEST_CASE("nothing in the client blocks for longer than one slice") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  std::uint32_t longest = 0;
  bench.clock.on_delay = [&](std::uint32_t ms) {
    if (ms > longest) {
      longest = ms;
    }
  };
  bench.radio.deaf = true;
  CHECK(bench.client.status().fault == lc::Fault::kLinkDown);
  CHECK(longest == lc::kPollMs);
  bench.clock.on_delay = nullptr;
}

TEST_CASE("a late answer: the one still in flight is taken, a second one is stray") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const std::size_t from = bench.radio.requests.size();

  // The answer comes 1200 ms after the request: after the first attempt has timed out and the
  // second has gone out. It carries the seq both attempts share, so it answers the call - and the
  // radio's remembered response to the retry arrives behind it with nothing in flight.
  bench.radio.answer_delay_ms = 1200;
  const lc::Answer<link::TimeGetResponse> answer = bench.client.timeGet();
  REQUIRE(answer.ok());
  CHECK(answer.attempts == 2);

  const std::vector<sim::ScriptedRadio::Seen> seen = after(bench, from);
  REQUIRE(seen.size() == 2);
  CHECK(seen[0].executed);
  CHECK_FALSE(seen[1].executed);  // §3: the remembered response, not a second execution
  CHECK(bench.radio.executions == 1 + 1);  // the hello and this one

  // The remembered response is still on its way; the next poll finds it with nothing in flight.
  bench.clock.advance(1300);
  bench.client.poll();
  CHECK(bench.client.counters().stray == 1);
  CHECK(bench.client.counters().mismatched == 0);
}

TEST_CASE("a response with another seq is counted and ignored") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  bench.radio.wrong_seq = 1;
  const lc::Answer<link::StatusResponse> answer = bench.client.status();
  REQUIRE(answer.ok());  // the retry's answer carries the right one
  CHECK(answer.attempts == 2);
  CHECK(bench.client.counters().mismatched == 1);
  CHECK(bench.client.counters().stray == 0);
  CHECK(bench.client.receiver().dropped() == 0);  // the frame itself was perfectly good
}

TEST_CASE("a corrupted response is dropped by the receiver and the retry recovers") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  bench.radio.corrupt = 1;
  const lc::Answer<link::TimeGetResponse> answer = bench.client.timeGet();
  REQUIRE(answer.ok());
  CHECK(answer.attempts == 2);
  CHECK(bench.client.receiver().dropped() == 1);
  CHECK(bench.client.counters().mismatched == 0);  // nothing was interpreted: the CRC never passed
  CHECK(bench.client.counters().malformed == 0);
}

TEST_CASE("a refused request is an answer, not a retry") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const std::size_t from = bench.radio.requests.size();
  bench.radio.refuseNext(link::status::kBusy);
  const lc::Answer<link::SendResponse> answer = bench.client.sendDiscovery(5000);
  CHECK(answer.fault == lc::Fault::kRefused);
  CHECK(answer.status == link::status::kBusy);
  CHECK(answer.attempts == 1);
  CHECK(after(bench, from).size() == 1);
  CHECK(bench.client.counters().retries == 0);
}

TEST_CASE("a password is in RAM for the length of the send and nowhere after it") {
  // §6 and S-12: SEND_LOGIN's password, and a SEND_CLI line marked sensitive, are wiped from
  // every buffer this end copied them to as soon as the exchange is over. The buffers are the
  // client's own, so the test looks for the bytes in the whole object.
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const std::string secret = "hunter2-ish";
  const std::vector<std::uint8_t> password(secret.begin(), secret.end());

  const auto holds = [&bench](const std::string& needle) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&bench.client);
    const std::string image(reinterpret_cast<const char*>(bytes), sizeof(bench.client));
    return image.find(needle) != std::string::npos;
  };

  REQUIRE(bench.client.sendLogin(5000, fixtures::viewOf(password)).ok());
  CHECK_FALSE(holds(secret));
  // The same line through SEND_CLI: wiped when it is flagged sensitive, which is the flag a line
  // made from a password carries (§6.2).
  REQUIRE(bench.client.sendCli(5000, link::send_cli_flag::kSensitive,
                               link::text(secret.data(), secret.size()))
              .ok());
  CHECK_FALSE(holds(secret));
  // And the radio did get it: it is the wire that carries a password, not a buffer afterwards.
  bool on_the_wire = false;
  for (const sim::ScriptedRadio::Seen& one : bench.radio.requests) {
    const std::string payload(one.payload.begin(), one.payload.end());
    on_the_wire = on_the_wire || payload.find(secret) != std::string::npos;
  }
  CHECK(on_the_wire);
}
