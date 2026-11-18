// `rak say <words>` (task p16-01): one line to a scanner, its `ack`, and then every line of the
// next five seconds printed whole out of room borrowed from the heap and given back.
//
// The two things this holds that nothing else can:
//
//   * **printed and not logged** (spec/SYSTEM.md S-12, "Nothing the mesh says is logged as
//     text"). A scanner's bytes go to the port through printText() and nowhere else: not into
//     the ring `dump` prints, not into the journal, not into a section of `twinoak.report`.
//   * **the room is borrowed and handed back**. 8 192 bytes of the heap for the length of the
//     command and not a byte of this firmware's own storage, because a buffer that size would
//     move the start-up margin rail R22 holds (tools/heap_at_start.py).
#include <doctest.h>

#include <string>
#include <vector>

#include "core/link/client/client.h"
#include "fakes/capture.h"
#include "settings_console_rig.h"

namespace lc = core::link::client;

namespace {

using bench::SettingsConsole;

// Words only this scanner would ever say, so that finding them anywhere is proof of a leak.
const char* const kSecretOfTheAir = "863014000";
const char* const kAckMessage = "started-281-points";

std::string ackLine(const std::string& command, bool ok, const std::string& message) {
  return "{\"t\":\"ack\",\"cmd\":\"" + command + "\",\"ok\":" + (ok ? "true" : "false") +
         ",\"msg\":\"" + message + "\"}";
}

// A `sweep` line longer than the 320 bytes the client keeps of one, with the marker in it.
std::string longSweepLine() {
  return std::string("{\"t\":\"sweep\",\"seq\":17,\"f0\":") + kSecretOfTheAir +
         ",\"n\":281,\"avg\":[" + std::string(900, '7') + "]}";
}

// The console over a radio that is running the scanner, found to be one as §9 finds it.
void makeItAScanner(SettingsConsole& sim) {
  sim.rak.scanner = true;
  sim.run("rak hello");
  REQUIRE(sim.console != nullptr);
  REQUIRE(sim.link.mode() == lc::Mode::kText);
}

}  // namespace

TEST_CASE("rak say: the ack and every line of the window, whole") {
  SettingsConsole sim;
  makeItAScanner(sim);
  const std::string sweep = longSweepLine();
  sim.rak.on_text_command = [&sim, &sweep](const std::string& line) {
    if (line != "sweep") return false;
    sim.rak.sendLine(ackLine("sweep", true, kAckMessage));
    sim.rak.sendLine(sweep, 50);
    sim.rak.sendLine("{\"t\":\"done\",\"cmd\":\"sweep\",\"seq\":18}", 60);
    return true;
  };

  const std::string said = sim.run("rak say sweep");
  CHECK(said.find("ack: ok") != std::string::npos);
  CHECK(said.find(kAckMessage) != std::string::npos);
  // The long line is there whole: the ring would have cut it at 320 bytes.
  CHECK(said.find(sweep) != std::string::npos);
  CHECK(said.find("\"t\":\"done\"") != std::string::npos);
  CHECK(said.find("2 lines") != std::string::npos);  // the ack is printed above, not listed
  REQUIRE_FALSE(sim.rak.commands.empty());
  CHECK(sim.rak.commands.back() == "sweep");
  // The room went back before the prompt did.
  CHECK(sim.scratch.out == 0);
  CHECK(sim.scratch.asks == 1);
  CHECK(sim.scratch.last_asked == 8192);
}

TEST_CASE("rak say: the done ends the wait, so the prompt comes back before the five seconds") {
  SettingsConsole sim;
  makeItAScanner(sim);
  sim.rak.on_text_command = [&sim](const std::string& line) {
    if (line != "stop") return false;
    sim.rak.sendLine(ackLine("stop", true, "stopped"));
    sim.rak.sendLine("{\"t\":\"done\",\"cmd\":\"stop\",\"seq\":19}", 20);
    return true;
  };

  const std::uint64_t before = sim.clock.now_ms;
  const std::string said = sim.run("rak say stop");
  CHECK(said.find("ack: ok") != std::string::npos);
  const bool quick = sim.clock.now_ms - before < 5000;
  CHECK(quick);
}

TEST_CASE("rak say: a scanner that says nothing after its ack") {
  SettingsConsole sim;
  makeItAScanner(sim);
  sim.rak.on_text_command = [&sim](const std::string& line) {
    if (line != "cad") return false;
    sim.rak.sendLine(ackLine("cad", true, "running"));
    return true;
  };

  const std::string said = sim.run("rak say cad");
  CHECK(said.find("ack: ok") != std::string::npos);
  CHECK(said.find("no lines in the 5 seconds after it") != std::string::npos);
  CHECK(sim.scratch.out == 0);
}

TEST_CASE("rak say: an ack that is not ok prints the scanner's own reason") {
  SettingsConsole sim;
  makeItAScanner(sim);
  sim.rak.on_text_command = [&sim](const std::string& line) {
    if (line.rfind("sweep", 0) != 0) return false;
    sim.rak.sendLine(ackLine("sweep", false, "both ends of a range are read together"));
    return true;
  };

  const std::string said = sim.run("rak say sweep 865");
  CHECK(said.find("ack: not ok") != std::string::npos);
  CHECK(said.find("both ends of a range are read together") != std::string::npos);
}

TEST_CASE("rak say: with no scanner there it says so and sends nothing") {
  SettingsConsole sim;
  sim.run("rak hello");  // a repeater: frames
  REQUIRE(sim.link.mode() == lc::Mode::kFrames);
  const std::size_t commands = sim.rak.commands.size();

  const std::string said = sim.run("rak say sweep");
  CHECK(said.find("error: rak say:") != std::string::npos);
  CHECK(said.find("the radio answers frames, not text") != std::string::npos);
  CHECK(sim.rak.commands.size() == commands);
  CHECK(sim.scratch.out == 0);

  // And before anything has answered at all, the same refusal with the other reason.
  SettingsConsole fresh;
  const std::string unknown = fresh.run("rak say sweep");
  CHECK(unknown.find("not known yet") != std::string::npos);
  CHECK(fresh.rak.commands.empty());
}

TEST_CASE("rak say: when the heap has no block it says so and sends nothing") {
  SettingsConsole sim;
  makeItAScanner(sim);
  sim.scratch.onlyHas(4096);
  const std::size_t commands = sim.rak.commands.size();

  const std::string said = sim.run("rak say sweep");
  CHECK(said.find("the heap has no 8192-byte block") != std::string::npos);
  CHECK(said.find("the largest is 4096") != std::string::npos);
  CHECK(sim.rak.commands.size() == commands);  // nothing was said
  CHECK(sim.scratch.out == 0);
  CHECK(sim.scratch.refusals == 1);
}

TEST_CASE("rak say: no byte of a scanner's line is in the log, the journal or a report") {
  SettingsConsole sim;
  makeItAScanner(sim);
  const std::string sweep = longSweepLine();
  sim.rak.on_text_command = [&sim, &sweep](const std::string& line) {
    if (line != "sweep") return false;
    sim.rak.sendLine(ackLine("sweep", true, kAckMessage));
    sim.rak.sendLine(sweep, 50);
    sim.rak.sendLine("{\"t\":\"done\",\"cmd\":\"sweep\",\"seq\":18}", 60);
    return true;
  };
  const std::string said = sim.run("rak say sweep");
  REQUIRE(said.find(kSecretOfTheAir) != std::string::npos);  // it did reach the port

  // The ring `dump` prints.
  const std::string dumped = sim.run("dump");
  CHECK(dumped.find(kSecretOfTheAir) == std::string::npos);
  CHECK(dumped.find(kAckMessage) == std::string::npos);
  CHECK(dumped.find("\"t\":\"sweep\"") == std::string::npos);

  // The flash journal.
  const std::string journal = sim.run("journal");
  CHECK(journal.find(kSecretOfTheAir) == std::string::npos);
  CHECK(journal.find(kAckMessage) == std::string::npos);

  // Every section of `twinoak.report`.
  for (int i = 0; i < sim.console->sectionCount(); ++i) {
    const char* name = sim.console->sectionName(i);
    REQUIRE(name != nullptr);
    fakes::CaptureOut section;
    (void)sim.console->writeSection(name, section);
    CHECK_MESSAGE(section.text.find(kSecretOfTheAir) == std::string::npos, std::string(name));
    CHECK_MESSAGE(section.text.find(kAckMessage) == std::string::npos, std::string(name));
  }
}

TEST_CASE("rak say: the parser's own refusals") {
  SettingsConsole sim;
  CHECK(sim.run("rak say").find("rak say needs a line to say") != std::string::npos);
  const std::string overlong = std::string("rak say ") + std::string(161, 'x');
  CHECK(sim.run(overlong.c_str()).find("the line is 1 to 160 bytes") != std::string::npos);
}
