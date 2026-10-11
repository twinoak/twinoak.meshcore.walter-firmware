// §9, telling the firmware apart: NoiseScope 0.1 speaks NDJSON lines on this same UART, not
// frames. A Walter that gets no HELLO response but sees lines beginning with `{` - or gets one in
// answer to `info\n` - is talking to a scanner and treats the UART as text until the next reset of
// the radio. Text is never mixed with frames in one session (task p1-02).
//
// The scanner's own side is the hardware repository's
// software/firmware/noisescope-rak4630/PROTOCOL.md at b798aab: one JSON object per line, and any
// line not beginning with `{` is human-readable chatter a machine parser ignores.
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

class Listener final : public lc::TextWatcher {
 public:
  std::vector<std::string> lines;
  void onTextLine(link::Text line) override { lines.push_back(fixtures::asString(line)); }
};

}  // namespace

TEST_CASE("no hello, and a line in answer to info: the radio is running the scanner") {
  sim::LinkBench bench;
  Listener listener;
  bench.client.subscribeText(&listener);
  bench.radio.scanner = true;

  const lc::Answer<link::HelloResponse> answer = bench.client.hello();
  CHECK(answer.fault == lc::Fault::kLinkDown);
  CHECK(answer.attempts == link::kRequestAttempts);
  CHECK(bench.client.mode() == lc::Mode::kText);
  CHECK(bench.client.session().role == link::role::kScanner);
  CHECK_FALSE(bench.client.session().up);
  CHECK(std::string(lc::describe(bench.client.mode())) == "scanner (text)");

  // §9's probe, and the line that answered it.
  REQUIRE(bench.radio.commands.size() == 1);
  CHECK(bench.radio.commands[0] == "info");
  CHECK(bench.client.counters().text_lines == 1);
  REQUIRE(listener.lines.size() == 1);
  CHECK(listener.lines[0].rfind("{\"t\":\"info\"", 0) == 0);
  REQUIRE(bench.client.textLines() == 1);
  CHECK(fixtures::asString(bench.client.textLine(0)) == listener.lines[0]);
}

TEST_CASE("lines already seen are evidence enough: no probe is needed") {
  sim::LinkBench bench;
  bench.radio.scanner = true;
  bench.radio.sendLine("{\"t\":\"stat\",\"up\":3}");
  // The client reads the port while it waits for the HELLO it will never get.
  CHECK(bench.client.hello().fault == lc::Fault::kLinkDown);
  CHECK(bench.client.mode() == lc::Mode::kText);
  CHECK(bench.radio.commands.empty());  // §9: "if lines beginning with { were seen"
  CHECK(bench.client.counters().text_lines == 1);
}

TEST_CASE("chatter is not evidence: a silent radio that says nothing in JSON stays unknown") {
  sim::LinkBench bench;
  bench.radio.deaf = true;  // frames, but nothing answers them
  bench.radio.sendLine("# commands: help info stat");
  bench.radio.sendLine("not json either");
  CHECK(bench.client.hello().fault == lc::Fault::kLinkDown);
  CHECK(bench.client.mode() == lc::Mode::kUnknown);
  CHECK(bench.client.counters().text_lines == 0);
  CHECK(bench.client.textLines() == 0);
  // The probe went out and nothing came back: §9's second half, answered no.
  CHECK(bench.client.session().role == link::role::kUnknown);
}

TEST_CASE("in a text session no frame is ever sent again") {
  sim::LinkBench bench;
  bench.radio.scanner = true;
  REQUIRE(bench.client.hello().fault == lc::Fault::kLinkDown);
  REQUIRE(bench.client.mode() == lc::Mode::kText);
  const std::size_t frames = bench.radio.requests.size();

  CHECK(bench.client.ping(1).fault == lc::Fault::kTextMode);
  CHECK(bench.client.status().fault == lc::Fault::kTextMode);
  CHECK(bench.client.hello().fault == lc::Fault::kTextMode);
  CHECK(bench.client.timeSet(1791720000, link::time_source::kConsole).fault == lc::Fault::kTextMode);
  CHECK(bench.client.sleepNotice(600).fault == lc::Fault::kTextMode);
  CHECK(bench.radio.requests.size() == frames);
  // And nothing of a frame is read either: the scanner's lines are not fed to the receiver.
  bench.radio.sendLine("{\"t\":\"stat\",\"up\":9}");
  bench.client.poll();
  CHECK(bench.client.receiver().dropped() == 0);
  CHECK(bench.client.eventsQueued() == 0);
}

TEST_CASE("a reset of the radio returns it to unknown, and a repeater answers again") {
  sim::LinkBench bench;
  bench.radio.scanner = true;
  REQUIRE(bench.client.hello().fault == lc::Fault::kLinkDown);
  REQUIRE(bench.client.mode() == lc::Mode::kText);

  // §9: "until the next reset of the RAK". The console's `rak reset` and the supervisor's reset
  // both tell the client, and the radio comes back with the repeater firmware on it.
  bench.client.radioWasReset();
  CHECK(bench.client.mode() == lc::Mode::kUnknown);
  bench.radio.scanner = false;
  REQUIRE(bench.client.hello().ok());
  CHECK(bench.client.mode() == lc::Mode::kFrames);
  CHECK(bench.client.session().role == link::role::kRepeater);
}

TEST_CASE("the lines of the session before are not evidence about this one") {
  // §9 asks whether lines beginning with `{` were seen - in this session. A radio that was running
  // the scanner, was reset, and now says nothing at all is a silent radio, not a scanner: calling
  // it one would send the supervisor down the wrong ladder (ARCHITECTURE §8).
  sim::LinkBench bench;
  bench.radio.scanner = true;
  REQUIRE(bench.client.hello().fault == lc::Fault::kLinkDown);
  REQUIRE(bench.client.mode() == lc::Mode::kText);
  const std::uint32_t seen = bench.client.counters().text_lines;
  CHECK(seen > 0);

  bench.client.radioWasReset();
  bench.radio.scanner = false;
  bench.radio.deaf = true;
  CHECK(bench.client.hello().fault == lc::Fault::kLinkDown);
  CHECK(bench.client.mode() == lc::Mode::kUnknown);
  CHECK(bench.client.counters().text_lines == seen);  // nothing new came
}

TEST_CASE("the lines are offered one by one, and the ring keeps the newest few") {
  sim::LinkBench bench;
  Listener listener;
  bench.client.subscribeText(&listener);
  bench.radio.scanner = true;
  REQUIRE(bench.client.hello().fault == lc::Fault::kLinkDown);
  const std::size_t after_probe = listener.lines.size();

  for (int i = 0; i < 5; ++i) {
    bench.radio.sendLine("{\"t\":\"sweep\",\"seq\":" + std::to_string(i) + "}");
  }
  bench.client.poll();
  CHECK(listener.lines.size() == after_probe + 5);  // every line is offered as it arrives
  REQUIRE(bench.client.textLines() == lc::kTextRing);
  for (std::size_t i = 0; i < lc::kTextRing; ++i) {
    const std::string want = "{\"t\":\"sweep\",\"seq\":" +
                             std::to_string(5 - lc::kTextRing + i) + "}";
    CHECK(fixtures::asString(bench.client.textLine(i)) == want);
  }
  CHECK(bench.client.counters().text_dropped == 5 + after_probe - lc::kTextRing);
  bench.client.clearText();
  CHECK(bench.client.textLines() == 0);
}

TEST_CASE("a line longer than the client keeps is offered truncated and counted") {
  sim::LinkBench bench;
  bench.radio.scanner = true;
  REQUIRE(bench.client.hello().fault == lc::Fault::kLinkDown);
  const std::uint32_t truncated = bench.client.counters().text_truncated;
  // A `sweep` line at NoiseScope's default settings is about 2.5 kB (its PROTOCOL.md): more than
  // this client keeps, and forwarding them verbatim is the uplink's business, not this task's.
  bench.radio.sendLine("{\"t\":\"sweep\",\"avg\":[" + std::string(3000, '1') + "]}");
  bench.client.poll();
  CHECK(bench.client.counters().text_truncated == truncated + 1);
  REQUIRE(bench.client.textLines() >= 1);
  const link::Text last = bench.client.textLine(bench.client.textLines() - 1);
  CHECK(last.size == lc::kTextLineMax);
  CHECK(bench.client.receiver().dropped() == 0);
}
