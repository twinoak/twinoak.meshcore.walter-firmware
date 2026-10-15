// Rails R8 and R9 (task p3-06), which are spec/SYSTEM.md S-12's two halves:
//
//   * **no secret, token, PIN, IMEI or ICCID in the log, the journal, `dump`, `status`, or the
//     wake record**;
//   * **nothing the mesh or a scanner said is logged as text**.
//
// One test, and it is the one the task asks for: two secrets provisioned, **a simulated day of
// wakes** on the wake bench - a radio that goes quiet and comes back, a spell as a scanner with a
// line of its own, an attention, readings taken - then **every command of the console's table**
// (fixtures/command_lines.h), and then everything this unit wrote down searched for all of it:
//
//   the console's whole transcript   what a person at the cable sees
//   `dump`                           the 4 kB ring, which is every line the console printed
//   `status`, `wake`, `journal`      the three summaries a person reads first
//   the journal partition's bytes    read straight out of the simulated flash, not through a printer
//   the retained records' bytes      RTC memory: the wake records, the telemetry ring, the caps
//
// What is deliberately **not** searched is the settings store itself: a secret is in NVS, that is
// where it is kept, NVS is not encrypted and the boot says so in a line of its own
// (`docs/bench.md` has the bench item that reads it back out with esptool). The rail is that
// nothing *else* carries it.
//
// The last case is the control: `lte id` does print the subscription's numbers and `rak text`
// does print the scanner's lines, because a person asked for them - and a grep that has stopped
// matching anything cannot pass this file. Those two commands are the only ones excused, and the
// ring they do not reach (`dump`) is searched all the same.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/cycle/wake_bench.h"
#include "fixtures/command_lines.h"

using bench::WakeBench;

namespace {

// The two secrets, long enough that a stray substring would be found, and nothing else in any
// line looks like either (the same pair host_test/core/console/test_console_secrets.cpp uses).
constexpr const char* kPin = "86421357";
constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";
constexpr const char* kTokenBody = "zQ7x4LpR";

// What the fake modem answers AT+CGSN, +CCID and +CIMI with (host_test/sim/fake_modem.cpp,
// makeIdentity): a subscription's three identifiers.
const char* const kIdentifying[] = {"357999001112223", "8945020000000777666", "238010000012345"};

// What the radio said, in its own words. A scanner's line is JSON a person could read; a mesh
// reply's payload is bytes. Neither may be written down as text (S-12).
constexpr const char* kScannerSaid = "what the mesh said at Grenaa";

struct Needle {
  const char* what;
  const char* text;
};

// What must be nowhere: the two secrets and the three numbers that identify a subscription. The
// one command that prints the three is `lte id`, because a person asked for them.
const Needle kSecrets[] = {
    {"the SIM's PIN", kPin},
    {"the server's token", kToken},
    {"the token's body", kTokenBody},
    {"the IMEI", kIdentifying[0]},
    {"the ICCID", kIdentifying[1]},
    {"the IMSI", kIdentifying[2]},
};

// And what the radio said, which may not be *written down* - not in the ring, the journal, the
// retained records or any summary. The one command that shows it is `rak text`, which is the
// subscription a person makes when they type it (docs/ARCHITECTURE.md §3: the client "offers the
// lines to whoever subscribed" and keeps none of them).
const Needle kRadioSaid[] = {
    {"what the scanner said", kScannerSaid},
};

void searchOne(const std::string& text, const char* where, const Needle& needle) {
  const bool clean = text.find(needle.text) == std::string::npos;
  CHECK_MESSAGE(clean, std::string(where) << " carries " << needle.what << ": "
                                          << std::string(needle.text));
}

// Both sets: for everything this unit wrote down.
void searchFor(const std::string& text, const char* where) {
  for (const Needle& needle : kSecrets) searchOne(text, where, needle);
  for (const Needle& needle : kRadioSaid) searchOne(text, where, needle);
}

// The secrets alone: for a command that a person may ask to show what the radio said.
void searchForSecrets(const std::string& text, const char* where) {
  for (const Needle& needle : kSecrets) searchOne(text, where, needle);
}

// The journal partition, byte for byte, as a string to search.
std::string journalBytes(sim::SimFlash& flash) {
  std::string all;
  for (std::uint32_t at = 0; at < flash.size(); at += 64) {
    std::uint8_t buf[64] = {};
    REQUIRE(flash.read(at, buf, sizeof buf));
    all.append(reinterpret_cast<const char*>(buf), sizeof buf);
  }
  return all;
}

}  // namespace

TEST_CASE("R8, R9: a day of wakes and every console command, and nothing of it is written down") {
  WakeBench bench;
  bench.modemWorks();
  std::string everything;  // everything this unit printed, over the whole day

  // -- provisioned, as a person does it at the cable (docs/provisioning.md) --------------------
  everything += bench.v().runLine("set lte.pin 86421357");
  everything += bench.v().runLine("set server.token mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV");
  everything += bench.v().runLine("set server.url https://backend.example/unit");
  REQUIRE(bench.v().settings.isSet(core::settings::Key::kLtePin));
  REQUIRE(bench.v().settings.isSet(core::settings::Key::kServerToken));

  // -- a simulated day --------------------------------------------------------------------------
  // Ninety-six wakes, which is a day at the shortest interval the policy has (`plenty`, a quarter
  // of an hour). The radio is mostly there; for three wakes it is a scanner with a line to say,
  // for three more it is silent; the uplink is up throughout, so the modem knows the
  // subscription's numbers while all of this is being written down.
  int scanner_wakes = 0;
  int silent_wakes = 0;
  int attention_wakes = 0;
  for (int i = 0; i < 96; ++i) {
    bench.world.rak.battery_mv = static_cast<std::uint16_t>(4100 - (i % 8) * 25);
    bench.world.rak.battery_ma = (i % 3 == 0) ? 120 : -30;
    bench.world.rak.scanner = (i % 32 == 7);
    bench.world.rak.deaf = (i % 32 == 19);
    if (bench.world.rak.scanner) {
      // A scanner says what it heard, in text, on the same UART (LINK-PROTOCOL.md §9).
      bench.world.rak.sendLine("{\"noise\":-112,\"note\":\"what the mesh said at Grenaa\"}");
      ++scanner_wakes;
    }
    if (bench.world.rak.deaf) ++silent_wakes;
    bench.world.backend.forget();
    if (i % 4 == 0) bench.world.backend.takeReadings();
    const bool on_the_line = (i % 16 == 3);
    if (on_the_line) {
      bench.world.rak.raiseAttention(core::link::attention::kPersonAsked, 0, 10, 0);
      ++attention_wakes;
    }
    const core::cycle::Pass& pass =
        bench.oneWake(on_the_line ? interfaces::WokeBy::kRadioLine : interfaces::WokeBy::kTimer);
    REQUIRE(pass.ending == core::cycle::Ending::kSlept);
  }
  CHECK(scanner_wakes == 3);
  CHECK(silent_wakes == 3);
  CHECK(attention_wakes == 6);

  // The uplink up and the identity in the house: the numbers are not only unprinted, they are
  // unprinted while something knows them.
  bench.world.rak.scanner = false;
  bench.world.rak.deaf = false;
  everything += bench.v().runLine("lte up");
  interfaces::Identity id;
  REQUIRE(bench.v().uplink.identity(&id));
  REQUIRE(std::string(id.imei) == kIdentifying[0]);

  // One more scanner line, now, so that the ring and `rak text` have something fresh. What
  // `rak text` prints is not appended to the transcript searched below: it is the one command
  // that shows a person the lines, as `lte id` is the one that shows the numbers, and the two
  // control cases at the end hold both to printing them.
  bench.world.rak.scanner = true;
  bench.world.rak.sendLine("{\"noise\":-109,\"note\":\"what the mesh said at Grenaa\"}");
  bench.world.clock.advance(50);
  const std::string text_lines = bench.v().runLine("rak text");
  searchForSecrets(text_lines, "`rak text`");
  bench.world.rak.scanner = false;

  // -- every command of the table ---------------------------------------------------------------
  for (std::size_t i = 0; i < fixtures::kCommandLineCount; ++i) {
    const fixtures::CommandLine& line = fixtures::kCommandLines[i];
    const std::string word(line.word);
    if (word == "lte id") continue;   // the control cases, below: these two print on purpose
    const std::string text = bench.v().runLine(line.line);
    if (word == "rak text") {
      searchForSecrets(text, line.line);
      continue;  // and not into the transcript: see above
    }
    searchFor(text, line.line);
    everything += text;
  }

  // -- and the five places it could have been left ----------------------------------------------
  const std::string dumped = bench.v().runLine("dump");
  const std::string status = bench.v().runLine("status");
  const std::string wake = bench.v().runLine("wake");
  const std::string journal = bench.v().runLine("journal");
  searchFor(dumped, "`dump` (the console's 4 kB ring)");
  searchFor(status, "`status`");
  searchFor(wake, "`wake` (the last ten wakes)");
  searchFor(journal, "`journal`");
  everything += dumped + status + wake + journal;
  searchFor(everything, "the console's whole transcript");

  const std::string flash = journalBytes(bench.world.journal_flash);
  searchFor(flash, "the journal partition's bytes");
  const std::vector<std::uint8_t> retained = bench.world.retained.allBytes();
  searchFor(std::string(reinterpret_cast<const char*>(retained.data()), retained.size()),
            "the retained records' bytes (the wake records, the readings, the caps)");

  // This is not a test that passed by printing nothing.
  CHECK(everything.size() > 8000);
  CHECK(dumped.size() > 500);
  CHECK(bench.wakeEntries().size() > 0);
  CHECK(retained.size() > 100);
  MESSAGE("a simulated day: 96 wakes (" << scanner_wakes << " with a scanner, " << silent_wakes
          << " with a silent radio, " << attention_wakes << " on the radio's line), "
          << fixtures::kCommandLineCount << " console commands, "
          << everything.size() << " characters printed, " << dumped.size()
          << " in the ring, " << flash.size() << " bytes of journal and " << retained.size()
          << " bytes in " << bench.world.retained.recordCount()
          << " retained records - none of it carrying any of the "
          << (sizeof kSecrets / sizeof kSecrets[0]) << " secrets or the "
          << (sizeof kRadioSaid / sizeof kRadioSaid[0]) << " things the radio said");
}

TEST_CASE("R8, R9: `lte id` and `rak text` do print, so the search looks for the right things") {
  WakeBench bench;
  bench.modemWorks();
  bench.v().runLine("lte up");
  const std::string text = bench.v().runLine("lte id");
  for (const char* number : kIdentifying) {
    CHECK_MESSAGE(text.find(number) != std::string::npos,
                  "lte id did not print " << std::string(number));
  }
  // And the secrets are findable where they *are* kept, which is the settings store: the rail is
  // that nothing else carries them, not that they are nowhere (docs/provisioning.md, and the
  // bench item that reads the `nvs` partition back out with esptool).
  bench.v().runLine("set lte.pin 86421357");
  CHECK(bench.world.kv.raw(core::settings::def(core::settings::Key::kLtePin).nvs) == kPin);

  // And `rak text` shows a person the lines the scanner sent, which is what a subscription is.
  bench.world.rak.scanner = true;
  bench.world.rak.sendLine("{\"noise\":-112,\"note\":\"what the mesh said at Grenaa\"}");
  bench.world.clock.advance(50);
  const std::string lines = bench.v().runLine("rak text");
  CHECK(lines.find(kScannerSaid) != std::string::npos);
  // But not afterwards, in the ring: what `dump` prints has none of it.
  CHECK(bench.v().runLine("dump").find(kScannerSaid) == std::string::npos);
}
