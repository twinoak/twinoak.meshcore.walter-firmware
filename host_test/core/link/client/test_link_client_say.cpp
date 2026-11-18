// §9, the other direction (task p16-01): a line said to a scanner and its `ack` read, and a
// scanner's lines kept whole in a buffer the caller lends.
//
// spec/LINK-PROTOCOL.md §9 is the rule - "NoiseScope 0.1 speaks NDJSON lines on this same UART,
// not frames ... Text is never mixed with frames in one session" - and the shapes are the
// repeater firmware's own docs/noisescope.md §5: every command is answered with an `ack` line
// (`{"t":"ack","cmd":"sweep","ok":true,"msg":"started"}`), a finite activity ends with a `done`
// line, a `sweep` line of the band is thousands of bytes, and any line not beginning with `{` is
// chatter a machine parser ignores.
#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "core/link/client/client.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;

namespace {

// The scanner's own ack, as its document writes it.
std::string ackLine(const std::string& command, bool ok, const std::string& message) {
  return "{\"t\":\"ack\",\"cmd\":\"" + command + "\",\"ok\":" + (ok ? "true" : "false") +
         ",\"msg\":\"" + message + "\"}";
}

std::string doneLine(const std::string& command) {
  return "{\"t\":\"done\",\"cmd\":\"" + command + "\",\"seq\":18,\"ts\":1754820100,\"ms\":112000}";
}

// A `sweep` line of exactly `size` bytes, shaped like the real one: 281 points of the band, with
// the array padded to the length wanted. The longest line this scanner can send is about 7.6 kB.
std::string sweepLineOf(std::size_t size) {
  const std::string head =
      "{\"t\":\"sweep\",\"seq\":17,\"f0\":863.000,\"f1\":870.000,\"n\":281,\"avg\":[";
  const std::string tail = "]}";
  REQUIRE(size > head.size() + tail.size());
  return head + std::string(size - head.size() - tail.size(), '7') + tail;
}

// A bench whose radio is a scanner and has been found to be one (§9: three HELLOs unanswered and
// a line in answer to the probe).
struct ScannerBench : sim::LinkBench {
  ScannerBench() {
    radio.scanner = true;
    REQUIRE(client.hello().fault == lc::Fault::kLinkDown);
    REQUIRE(client.mode() == lc::Mode::kText);
  }

  // What this scanner answers each command with.
  void answers(std::function<bool(const std::string&)> hook) { radio.on_text_command = hook; }
};

}  // namespace

// ------------------------------------------------------------------------------- a line said

TEST_CASE("sweep said and acked ok: answered, with the scanner's own message") {
  ScannerBench bench;
  bench.answers([&bench](const std::string& line) {
    if (line != "sweep") return false;
    bench.radio.sendLine(ackLine("sweep", true, "started"));
    return true;
  });

  const lc::Said said = bench.client.say(link::text("sweep"));
  CHECK(said.fault == lc::Fault::kNone);
  CHECK(said.answered());
  CHECK(said.ok);
  CHECK(std::string(said.msg) == "started");
  CHECK_FALSE(said.msg_cut);
  REQUIRE_FALSE(bench.radio.commands.empty());
  CHECK(bench.radio.commands.back() == "sweep");
  CHECK(bench.client.counters().text_said == 1);
  CHECK(bench.client.counters().text_not_ok == 0);
  CHECK(bench.client.counters().text_unanswered == 0);
}

TEST_CASE("an ack that is not ok: answered, and the reason is the scanner's") {
  ScannerBench bench;
  // A refusal in the scanner's own words (noisescope.md §5: a sweep is refused when the ends are
  // crossed, either end is outside 150-960 MHz, the step is out of range...).
  bench.answers([&bench](const std::string& line) {
    if (line.rfind("sweep", 0) != 0) return false;
    bench.radio.sendLine(ackLine("sweep", false, "both ends of a range are read together"));
    return true;
  });

  const lc::Said said = bench.client.say(link::text("sweep 865"));
  CHECK(said.fault == lc::Fault::kNone);
  CHECK_FALSE(said.ok);
  CHECK(std::string(said.msg) == "both ends of a range are read together");
  CHECK(bench.client.counters().text_not_ok == 1);
}

TEST_CASE("an ack for another command, then silence: not answered") {
  ScannerBench bench;
  // §9's `stat` heartbeat falls in the middle of everything, and an `ack` of the command before
  // can still be in flight: neither is this line's answer. The one that counts is the `ack`
  // whose `cmd` is the line's first word.
  bench.answers([&bench](const std::string& line) {
    if (line != "sweep") return false;
    bench.radio.sendLine("{\"t\":\"stat\",\"ts\":1754820010,\"ms\":12345,\"up\":12}");
    bench.radio.sendLine(ackLine("stop", true, "stopped"));
    return true;
  });

  const std::uint64_t before = bench.clock.now_ms;
  const lc::Said said = bench.client.say(link::text("sweep"));
  CHECK(said.fault == lc::Fault::kLinkDown);
  CHECK_FALSE(said.answered());
  CHECK_FALSE(said.ok);
  CHECK(std::string(said.msg).empty());
  CHECK(bench.client.counters().text_unanswered == 1);
  // It waited the whole of §9's window before it said so.
  const bool waited = bench.clock.now_ms - before >= lc::kProbeWaitMs;
  CHECK(waited);
}

TEST_CASE("nothing is said in frame mode, and the fault names the mode") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  REQUIRE(bench.client.mode() == lc::Mode::kFrames);
  const std::size_t writes = bench.radio.writes.size();

  const lc::Said said = bench.client.say(link::text("sweep"));
  CHECK(said.fault == lc::Fault::kFrameMode);
  CHECK(std::string(lc::describe(said.fault)) ==
        "the radio answers frames, not text: nothing was said");
  CHECK(bench.radio.writes.size() == writes);  // not a byte went out
  CHECK(bench.radio.commands.empty());
  CHECK(bench.client.counters().text_said == 0);
}

TEST_CASE("nothing is said before anything has answered, and the fault names that too") {
  sim::LinkBench bench;
  REQUIRE(bench.client.mode() == lc::Mode::kUnknown);

  const lc::Said said = bench.client.say(link::text("sweep"));
  CHECK(said.fault == lc::Fault::kModeUnknown);
  CHECK(std::string(lc::describe(said.fault)) ==
        "what the radio is running is not known yet: say hello first");
  CHECK(bench.radio.writes.empty());
  CHECK(bench.radio.commands.empty());
  CHECK(bench.client.counters().text_said == 0);
}

TEST_CASE("a line this console could not have typed is refused before the port is touched") {
  ScannerBench bench;
  const std::size_t writes = bench.radio.writes.size();
  CHECK(bench.client.say(link::Text{}).fault == lc::Fault::kEncode);
  CHECK(bench.client.say(link::text("")).fault == lc::Fault::kEncode);
  CHECK(bench.client.say(link::text(std::string(lc::kSaidMax + 1, 'x').c_str())).fault ==
        lc::Fault::kEncode);
  // A newline of its own would be a second command nobody asked for.
  CHECK(bench.client.say(link::text("sweep\nreboot")).fault == lc::Fault::kEncode);
  CHECK(bench.radio.writes.size() == writes);
}

// -------------------------------------------------------------------------- the lent room

TEST_CASE("a 7 600-byte sweep line, in reads of 64, is in the lent room byte for byte") {
  ScannerBench bench;
  // The client reads the port 64 bytes at a time (Client::chunk_), so a line this long arrives
  // in a hundred and nineteen of them - which is the case the ring of kTextLineMax cannot hold
  // and the room is for.
  const std::string line = sweepLineOf(7600);
  REQUIRE(line.size() == 7600);

  std::vector<char> room(8192);
  REQUIRE(bench.client.lendText(room.data(), room.size()));
  CHECK(bench.client.textLent());
  bench.radio.sendLine(line);
  bench.client.poll();

  const lc::TextHeld held = bench.client.takeText();
  CHECK_FALSE(bench.client.textLent());
  CHECK(held.lines == 1);
  CHECK(held.size == line.size() + 1);
  CHECK_FALSE(held.full);
  REQUIRE(held.data != nullptr);
  const bool same = std::memcmp(held.data, line.data(), line.size()) == 0;
  CHECK(same);
  CHECK(held.data[line.size()] == '\n');
  // And the ring above is untouched by any of it: it keeps its 320 bytes of the same line.
  REQUIRE(bench.client.textLines() >= 1);
  CHECK(bench.client.textLine(bench.client.textLines() - 1).size == lc::kTextLineMax);
}

TEST_CASE("two lines in one read, each whole and in order") {
  ScannerBench bench;
  std::vector<char> room(1024);
  REQUIRE(bench.client.lendText(room.data(), room.size()));

  const std::string first = "{\"t\":\"stat\",\"up\":12}";
  const std::string second = "{\"t\":\"stat\",\"up\":13}";
  bench.radio.sendLine(first);   // both due now: they are in the port together
  bench.radio.sendLine(second);
  bench.client.poll();

  const lc::TextHeld held = bench.client.takeText();
  REQUIRE(held.lines == 2);
  CHECK(std::string(held.data, held.size) == first + "\n" + second + "\n");
}

TEST_CASE("one line over three reads, whole") {
  ScannerBench bench;
  std::vector<char> room(1024);
  REQUIRE(bench.client.lendText(room.data(), room.size()));

  // Three pieces of one line, arriving at three different moments: what a UART does with a line
  // longer than whatever was in the receiver when the client last looked.
  const std::string whole = "{\"t\":\"sweep\",\"seq\":17,\"n\":281,\"avg\":[-115.2,-114.8]}";
  const std::string a = whole.substr(0, 10);
  const std::string b = whole.substr(10, 20);
  const std::string c = whole.substr(30);
  bench.radio.sendBytes(std::vector<std::uint8_t>(a.begin(), a.end()));
  bench.client.poll();
  CHECK(bench.client.textHeld().lines == 0);  // half a line is not a line
  bench.radio.sendBytes(std::vector<std::uint8_t>(b.begin(), b.end()));
  bench.client.poll();
  std::vector<std::uint8_t> last(c.begin(), c.end());
  last.push_back('\n');
  bench.radio.sendBytes(last);
  bench.client.poll();

  const lc::TextHeld held = bench.client.takeText();
  REQUIRE(held.lines == 1);
  CHECK(std::string(held.data, held.size) == whole + "\n");
}

TEST_CASE("a chatter line between two lines is not in the room") {
  ScannerBench bench;
  std::vector<char> room(1024);
  REQUIRE(bench.client.lendText(room.data(), room.size()));

  const std::string first = "{\"t\":\"stat\",\"up\":12}";
  const std::string second = "{\"t\":\"stat\",\"up\":13}";
  bench.radio.sendLine(first);
  bench.radio.sendLine("# commands: help info stat sweep stop");
  bench.radio.sendLine(second);
  bench.client.poll();

  const lc::TextHeld held = bench.client.takeText();
  REQUIRE(held.lines == 2);
  const std::string text(held.data, held.size);
  CHECK(text == first + "\n" + second + "\n");
  CHECK(text.find('#') == std::string::npos);
}

TEST_CASE("a line that would end past the end of the room ends the lending as full") {
  ScannerBench bench;
  const std::string fits = "{\"t\":\"stat\",\"up\":12}";
  std::vector<char> room(fits.size() + 1 + 8);  // the first line and its ending, and no second
  REQUIRE(bench.client.lendText(room.data(), room.size()));

  bench.radio.sendLine(fits);
  bench.radio.sendLine("{\"t\":\"sweep\",\"n\":281,\"avg\":[-115.2,-114.8,-113.0]}");
  bench.radio.sendLine("{\"t\":\"stat\",\"up\":13}");  // after the full: not kept either
  bench.client.poll();

  const lc::TextHeld held = bench.client.takeText();
  CHECK(held.full);
  REQUIRE(held.lines == 1);
  CHECK(std::string(held.data, held.size) == fits + "\n");
  CHECK(bench.client.counters().text_full == 1);
  // Nothing of it was lost to the client itself: the ring and the counters saw all three.
  CHECK(bench.client.counters().text_lines >= 3);
}

TEST_CASE("a done for the command is found among the lines, and one for another is not") {
  ScannerBench bench;
  std::vector<char> room(1024);
  REQUIRE(bench.client.lendText(room.data(), room.size()));

  bench.radio.sendLine("{\"t\":\"sweep\",\"seq\":17,\"n\":281}");
  bench.radio.sendLine(doneLine("sweep"));
  bench.client.poll();

  const lc::TextHeld held = bench.client.takeText();
  REQUIRE(held.lines == 2);
  CHECK(held.hasDone(link::text("sweep")));
  CHECK_FALSE(held.hasDone(link::text("hist")));
  CHECK_FALSE(held.hasDone(link::Text{}));
}

TEST_CASE("the room is lent once, and a second lending is refused") {
  ScannerBench bench;
  std::vector<char> room(1024);
  std::vector<char> other(1024);
  REQUIRE(bench.client.lendText(room.data(), room.size()));
  CHECK_FALSE(bench.client.lendText(other.data(), other.size()));
  CHECK_FALSE(bench.client.lendText(nullptr, 1024));
  (void)bench.client.takeText();
  CHECK_FALSE(bench.client.lendText(room.data(), 1));  // no room for any line at all
  CHECK(bench.client.lendText(other.data(), other.size()));
}

TEST_CASE("nothing is laid down when no room is lent") {
  ScannerBench bench;
  bench.radio.sendLine("{\"t\":\"stat\",\"up\":12}");
  bench.client.poll();
  const lc::TextHeld held = bench.client.textHeld();
  CHECK(held.data == nullptr);
  CHECK(held.size == 0);
  CHECK(held.lines == 0);
  CHECK_FALSE(held.hasDone(link::text("sweep")));
}

TEST_CASE("half a line is dropped when the radio restarts under a lending") {
  ScannerBench bench;
  std::vector<char> room(1024);
  REQUIRE(bench.client.lendText(room.data(), room.size()));
  bench.radio.sendLine("{\"t\":\"stat\",\"up\":12}");
  bench.client.poll();
  const std::string half = "{\"t\":\"swe";
  bench.radio.sendBytes(std::vector<std::uint8_t>(half.begin(), half.end()));
  bench.client.poll();

  bench.client.radioWasReset();
  const lc::TextHeld held = bench.client.takeText();
  REQUIRE(held.lines == 1);
  CHECK(std::string(held.data, held.size) == "{\"t\":\"stat\",\"up\":12}\n");
}

// ------------------------------------------------------------- the ring is what it was (3.)

TEST_CASE("with a room lent, the three kept lines and the counts are what they are without one") {
  const std::vector<std::string> lines = {
      "{\"t\":\"stat\",\"up\":1}",  "{\"t\":\"stat\",\"up\":2}", "# chatter",
      "{\"t\":\"stat\",\"up\":3}",  "{\"t\":\"stat\",\"up\":4}",
      "{\"t\":\"sweep\",\"avg\":[" + std::string(3000, '1') + "]}",
  };

  std::string without_lines;
  lc::Counters without{};
  {
    ScannerBench bench;
    for (const std::string& line : lines) bench.radio.sendLine(line);
    bench.client.poll();
    for (std::size_t i = 0; i < bench.client.textLines(); ++i) {
      const link::Text kept = bench.client.textLine(i);
      without_lines.append(kept.data, kept.size);
      without_lines.push_back('|');
    }
    without = bench.client.counters();
  }

  ScannerBench bench;
  std::vector<char> room(16384);
  REQUIRE(bench.client.lendText(room.data(), room.size()));
  for (const std::string& line : lines) bench.radio.sendLine(line);
  bench.client.poll();

  std::string with_lines;
  for (std::size_t i = 0; i < bench.client.textLines(); ++i) {
    const link::Text kept = bench.client.textLine(i);
    with_lines.append(kept.data, kept.size);
    with_lines.push_back('|');
  }
  CHECK(bench.client.textLines() == lc::kTextRing);
  CHECK(with_lines == without_lines);
  const lc::Counters& with = bench.client.counters();
  CHECK(with.text_lines == without.text_lines);
  CHECK(with.text_dropped == without.text_dropped);
  CHECK(with.text_truncated == without.text_truncated);

  // And the room has the long line whole, which is the whole difference between the two.
  const lc::TextHeld held = bench.client.takeText();
  CHECK(held.lines == 5);
  CHECK(std::string(held.data, held.size).find(std::string(3000, '1')) != std::string::npos);
}
