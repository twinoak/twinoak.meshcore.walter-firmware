// `trace`, `trace modem on` and `trace modem off` (task p10-06).
//
// Three things are held here, and they are the three the task asks for: **the command** (what it
// prints, what a refusal says, what a build without a trace says), **the end at the next boot**
// (nothing of it is written to the settings store, and a boot asks for no trace), and **the two
// secrets reaching the hook without reaching the port** - the console reads `server.token` and
// `lte.pin` out of the store where they already are and hands them over, and prints neither.
//
// The redaction itself is core's and is tested against a real request head in
// host_test/core/trace/test_redact.cpp; the hook that installs it is
// components/modem/modem_trace.cpp, which needs esp_log and cannot be built on the host
// (host_test/fakes/fake_modem_trace.h says what stands in for it).
#include <doctest.h>

#include <string>

#include "core/console/modem_trace.h"
#include "settings_console_rig.h"

using bench::hasText;
using bench::SettingsConsole;

namespace {

constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";
constexpr const char* kPin = "86421357";

}  // namespace

TEST_CASE("trace reads, and says what the trace is for before anybody switches it on") {
  SettingsConsole c;
  const std::string out = c.run("trace");
  CHECK(hasText(out, "trace: the modem library's TX and RX lines are off"));
  CHECK(hasText(out, "'trace modem on' switches it on"));
  // The three things a person must know before they use it.
  CHECK(hasText(out, "for the bench"));
  CHECK(hasText(out, "not a setting"));
  CHECK(hasText(out, "no 'dump'"));
  CHECK(hasText(out, "0 line(s) printed in this boot"));
  // Reading it switches nothing.
  CHECK(c.trace.turns == 0);
  CHECK_FALSE(c.trace.on());
}

TEST_CASE("trace modem on tells the hook the two secrets and prints neither") {
  SettingsConsole c;
  c.run((std::string("set server.token ") + kToken).c_str());
  c.run((std::string("set lte.pin ") + kPin).c_str());
  const std::string out = c.run("trace modem on");

  CHECK(hasText(out, "trace modem on:"));
  CHECK(hasText(out, "until the next boot"));
  CHECK(hasText(out, "taken out of every line"));
  CHECK(c.trace.on());
  CHECK(c.trace.turns == 1);
  // The hook got them, exactly as the store has them.
  CHECK(c.trace.last_token == kToken);
  CHECK(c.trace.last_pin == kPin);
  // And no line of this console has ever carried either: the reply, the echo as it was typed, the
  // boot's lines, `status`, `trace` and the ring `dump` prints are all in `everything`.
  c.run("status");
  c.run("trace");
  c.run("dump");
  CHECK_FALSE(hasText(c.everything, kToken));
  CHECK_FALSE(hasText(c.everything, kPin));
  CHECK_FALSE(hasText(c.everything, "zQ7x4LpR"));
}

TEST_CASE("an unset secret is an empty value and not a refusal") {
  SettingsConsole c;
  const std::string out = c.run("trace modem on");
  CHECK(hasText(out, "trace modem on:"));
  CHECK(c.trace.on());
  CHECK(c.trace.last_token.empty());
  CHECK(c.trace.last_pin.empty());
}

TEST_CASE("status says so only while the trace is on") {
  SettingsConsole c;
  CHECK_FALSE(hasText(c.run("status"), "trace:"));
  c.run("trace modem on");
  const std::string on = c.run("status");
  CHECK(hasText(on, "trace: the modem library's TX and RX lines are ON"));
  CHECK(hasText(on, "the bench only"));
  const std::string out = c.run("trace modem off");
  CHECK(hasText(out, "trace modem off:"));
  CHECK_FALSE(c.trace.on());
  CHECK_FALSE(hasText(c.run("status"), "trace:"));
  // `trace` still reads, and still says what it is for.
  CHECK(hasText(c.run("trace"), "are off"));
}

TEST_CASE("it is not a setting: nothing of it is written down, and a boot asks for no trace") {
  SettingsConsole c;
  const std::size_t keys = c.kv.size();
  const int writes = c.kv.writes;
  c.run("trace modem on");
  CHECK(c.kv.size() == keys);
  CHECK(c.kv.writes == writes);
  CHECK_FALSE(c.kv.has("trace"));
  CHECK_FALSE(c.kv.has("trace_modem"));
  // There is no key for it, so `get` and `set` have never heard of it.
  CHECK(hasText(c.run("get trace"), "unknown key"));
  CHECK(hasText(c.run("set trace on"), "unknown key"));
  // And a boot - the console's begin(), which is what puts every setting in force - switches
  // nothing on. On a board the hook's own state goes with the RAM; here the count says the
  // console never asked for it.
  c.trace.turns = 0;
  c.build();
  c.begin();
  CHECK(c.trace.turns == 0);
}

TEST_CASE("a board that cannot trace says so in one line and changes nothing") {
  SettingsConsole c;
  c.trace.refuse = true;
  const std::string out = c.run("trace modem on");
  CHECK(hasText(out, "error: trace modem on: "));
  CHECK(hasText(out, "would not give them"));
  CHECK_FALSE(c.trace.on());
  CHECK_FALSE(hasText(c.run("status"), "trace:"));
}

// Task p11-01: the release build compiles no debug log line, so there is nothing to switch on -
// and switching nothing on must not even reach the seam, which on a board would take a 4 KB line
// buffer and install a log hook for a trace that could never print.
TEST_CASE("a build whose debug lines were never compiled says so and touches nothing") {
  SettingsConsole c;
  c.trace.compiled_in = false;

  const std::string on = c.run("trace modem on");
  CHECK(hasText(on, "error: trace modem on: "));
  CHECK(hasText(on, "are not in this build"));
  CHECK(hasText(on, "docs/bench.md section 13"));
  CHECK(c.trace.turns == 0);
  CHECK_FALSE(c.trace.on());
  // Off says the same thing rather than pretending to have switched something off.
  CHECK(hasText(c.run("trace modem off"), "error: trace modem off: "));
  CHECK(c.trace.turns == 0);
  // `trace` still reads, says where the lines are, and does not offer the command that cannot work.
  const std::string read = c.run("trace");
  CHECK(hasText(read, "trace: the modem library's TX and RX lines are off"));
  CHECK(hasText(read, "the lines are not in this build"));
  CHECK(hasText(read, "docs/bench.md section 13"));
  CHECK_FALSE(hasText(read, "'trace modem on' switches it on"));
  // And `status` stays quiet about a trace, because there is none.
  CHECK_FALSE(hasText(c.run("status"), "trace:"));
}

TEST_CASE("a trace build offers the command") {
  SettingsConsole c;
  CHECK(c.trace.compiled_in);
  const std::string read = c.run("trace");
  CHECK(hasText(read, "'trace modem on' switches it on"));
  CHECK_FALSE(hasText(read, "not in this build"));
}

TEST_CASE("a build with no trace at all answers in one line") {
  SettingsConsole c;
  c.withoutSettings();  // the console main hands the fewest things to
  CHECK(hasText(c.run("trace"), "error: this build cannot trace the modem"));
  CHECK(hasText(c.run("trace modem on"), "error: this build cannot trace the modem"));
  CHECK(c.trace.turns == 0);
}

TEST_CASE("what the grammar will not take") {
  SettingsConsole c;
  CHECK(hasText(c.run("trace modem"), "error: trace modem needs 'on' or 'off'"));
  CHECK(hasText(c.run("trace modem yes"), "error: trace takes no argument, or 'modem on' or "
                                          "'modem off': modem yes"));
  CHECK(hasText(c.run("trace radio on"), "error: trace takes no argument"));
  CHECK(c.trace.turns == 0);
}
