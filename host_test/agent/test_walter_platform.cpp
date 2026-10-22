// The platform as the agent's loop and runner see it (task p4-04): `agent::WalterPlatform` and
// the `mm::IClock` behind it, over the fakes - the clock, the entropy, the heap, phase 3's
// network clock and a capture of every log line.
//
// The two that are *refusals* rather than implementations are what most of this file is about:
// `rebootForHealth` must not restart this Walter, and `delayMs` must pump rather than spin.
#include <doctest.h>

#include <string>

#include <agent/Log.h>

#include "core/cycle/agent_attention.h"
#include "core/cycle/network_clock.h"
#include "core/link/codec/wire.h"
#include "core/update/journal.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_entropy.h"
#include "fakes/fake_heap.h"
#include "fakes/fake_retained.h"
#include "sim/sim_flash.h"
#include "walter_agent/walter_platform.h"

namespace link = core::link;
namespace cycle = core::cycle;

namespace {

// Where the library's lines go in this file: one string, as the console's ring would be.
std::string g_log;
void captureLine(const char* line) { g_log += line; }

struct Rig {
  fakes::FakeClock clock;
  fakes::FakeEntropy entropy;
  fakes::FakeHeap heap;
  fakes::FakeRetained retained;
  cycle::NetworkClock wall{clock};
  cycle::WakeReason reason{retained, clock};
  // A journal over a simulated flash, so that the one thing `rebootForHealth` *does* can be read
  // back as bytes.
  sim::SimFlash flash{64 * 1024, 4096};
  core::update::Journal journal{flash};
  cycle::AgentAttention attention{&reason, &journal};
  agent::WalterPlatform platform{clock, wall, entropy, heap, &attention};

  Rig() {
    REQUIRE(journal.begin() == core::update::JournalError::kNone);
    g_log.clear();
    platform.beginLog(&captureLine, mm::LogLevel::Debug);
  }
};

// The sentence the library builds when its reboot guard fires (agent/AgentLoop.cpp): 45
// characters at two digits each, which is longer than a journal entry's field.
constexpr const char* kWhy = "12 server calls unanswered in 95 min, wifi up";

// What a bench reading looks like: the first epoch the clock window accepts
// (core/uplink/clock_window.h, kEarliestEpoch = 2026-10-14T00:00:00Z).
constexpr std::uint32_t kEpoch = 1791936000u;

bool holds(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("millis() is the hal clock's, truncated to the thirty-two bits the library wants") {
  Rig rig;
  CHECK(rig.platform.millis() == 0u);
  rig.clock.advance(1234);
  CHECK(rig.platform.millis() == 1234u);
  // The library's comparisons are signed differences, which is right across the wrap, so the low
  // thirty-two bits are all it is owed - and a 64-bit clock past the wrap must not be cut to 0.
  rig.clock.now_ms = 0x1'0000'04D2ull;
  CHECK(rig.platform.millis() == 0x04D2u);
}

TEST_CASE("delayMs is a yield that pumps, and never a busy spin") {
  Rig rig;
  static int pumps = 0;
  pumps = 0;
  rig.platform.pumpWith([] { ++pumps; });
  rig.platform.delayMs(1);
  CHECK_MESSAGE(pumps == 1, "the library idles with 1 ms, and that still pumps once");
  CHECK(rig.clock.delayed_ms == 1u);
  for (int i = 0; i < 9; ++i) rig.platform.delayMs(10);
  CHECK(pumps == 10);
  // The time passed through the hal's delay - the thing that yields to the other tasks - and not
  // through a loop reading the clock: the fake counts what it was asked to wait.
  CHECK(rig.clock.delayed_ms == 91u);
  CHECK(rig.platform.delays() == 10u);
  CHECK(rig.platform.pumps() == 10u);
}

TEST_CASE("a platform with no pump still delays, and the counters say nothing was pumped") {
  Rig rig;
  rig.platform.delayMs(5);
  CHECK(rig.clock.delayed_ms == 5u);
  CHECK(rig.platform.pumps() == 0u);
}

TEST_CASE("random() is the hal's entropy, and two draws differ") {
  Rig rig;
  const std::uint32_t first = rig.platform.random();
  const std::uint32_t second = rig.platform.random();
  CHECK(first != second);
  CHECK(rig.entropy.draws() == 2u);
  CHECK(rig.platform.randoms() == 2u);
}

TEST_CASE("the heap's two figures are the hal's, reported and not decided on") {
  Rig rig;
  rig.heap.say(123456, 98765);
  CHECK(rig.platform.freeHeap() == 123456u);
  CHECK(rig.platform.minFreeHeap() == 98765u);
}

TEST_CASE("the clock: no time, the network's time, and the server's") {
  Rig rig;
  mm::IClock& clock = rig.platform.clock();

  SUBCASE("a Walter that has not attached has no time, and says so in the library's words") {
    CHECK_FALSE(clock.hasTime());
    CHECK(clock.now() == 0u);
    CHECK_FALSE(clock.fromNetwork());
    CHECK(std::string(clock.sourceName()) == "none");
  }

  SUBCASE("the cellular network's time is the one the radio's TIME_SET then carries as source 1") {
    REQUIRE(rig.wall.give(kEpoch));
    CHECK(clock.hasTime());
    CHECK(clock.now() == kEpoch);
    CHECK(clock.fromNetwork());
    CHECK(std::string(clock.sourceName()) == "network");
    interfaces::WallTime time;
    REQUIRE(rig.wall.now(&time));
    CHECK(time.source == link::time_source::kCellular);
  }

  SUBCASE("the server's time is taken when the network has given none, and says source 2") {
    std::int32_t difference = -1;
    CHECK(clock.takeServerTime(kEpoch, difference));
    CHECK_MESSAGE(difference == 0, "server minus local, and there was no local");
    CHECK(clock.hasTime());
    CHECK(clock.now() == kEpoch);
    CHECK(std::string(clock.sourceName()) == "server");
    // §5.4's source byte: the radio is told where the time came from, and a server time is not
    // the network's.
    interfaces::WallTime time;
    REQUIRE(rig.wall.now(&time));
    CHECK(time.source == link::time_source::kServer);
  }

  SUBCASE("a time that came off the network wins over the server's, and the difference is still reported") {
    REQUIRE(rig.wall.give(kEpoch));
    std::int32_t difference = 0;
    CHECK_FALSE(clock.takeServerTime(kEpoch + 90u, difference));
    CHECK_MESSAGE(difference == 90, "server minus local, before any change, for the library's log");
    CHECK(clock.now() == kEpoch);
    CHECK(std::string(clock.sourceName()) == "network");
  }

  SUBCASE("a server time outside the window this firmware believes is refused, as the modem's is") {
    std::int32_t difference = 0;
    CHECK_FALSE(clock.takeServerTime(1000, difference));
    CHECK_FALSE(clock.hasTime());
  }

  SUBCASE("a second server time is taken: the server is the only source there is") {
    std::int32_t difference = 0;
    REQUIRE(clock.takeServerTime(kEpoch, difference));
    rig.clock.advance(60000);
    CHECK(clock.now() == kEpoch + 60u);
    CHECK(clock.takeServerTime(kEpoch + 120u, difference));
    CHECK_MESSAGE(difference == 60, "the server was a minute ahead of what this unit had drifted to");
    CHECK(clock.now() == kEpoch + 120u);
  }
}

TEST_CASE("rebootForHealth journals, raises the Walter's own attention, and does not restart") {
  Rig rig;
  rig.reason.begin(core::health::BootRecord{}, interfaces::WokeBy::kNotFromSleep, false, nullptr);
  // The method may not return (it is [[noreturn]]) and it may not restart this board, so what it
  // does is yield for ever and let the bound on the wake end the wake
  // (core/cycle/wake_guard.h). A test cannot wait for ever: the fake clock's delay hook throws
  // out of the first yield, which is the same shape the library's own FakePlatform uses.
  struct Yielded {};
  int yields = 0;
  rig.clock.on_delay = [&yields](std::uint32_t ms) {
    ++yields;
    CHECK_MESSAGE(ms == agent::kRefusedRestartYieldMs, "a yield, not a spin");
    throw Yielded{};
  };
  bool caught = false;
  try {
    rig.platform.rebootForHealth(kWhy);
  } catch (const Yielded&) {
    caught = true;
  }
  CHECK_MESSAGE(caught, "it did not return: the board was left to the bound on the wake");
  CHECK(yields == 1);

  // Nothing restarted. The only interface that could is interfaces::Restart, and this platform is
  // not given one - it cannot restart the board even by accident, which is the point.
  CHECK(rig.platform.restartsRefused() == 1u);

  // The attention is raised for the next wake record and for `status`.
  CHECK(rig.attention.raised() == cycle::agent_attention::kRebootWanted);
  CHECK(rig.reason.agentAttention() == cycle::agent_attention::kRebootWanted);
  CHECK(rig.attention.rebootsWanted() == 1u);
  CHECK_MESSAGE(std::string(rig.attention.lastWhy()) == kWhy,
                "the whole of the library's sentence, which `status` prints");

  // And it is journalled, with the library's own sentence, where a person reading `journal` after
  // a week on a mast will find it.
  core::update::Entry entry;
  REQUIRE(rig.journal.lastOf(core::update::kind::kAgentAttention, &entry));
  cycle::AgentPayload payload;
  REQUIRE(cycle::decode(entry, &payload));
  CHECK(payload.what == cycle::AgentAttentionKind::kRebootWanted);
  CHECK(payload.count == 1);
  // The journal's field is 44 bytes of a 48-byte payload, and this sentence is 45 characters, so
  // it loses its last word there - which is what the header of core/cycle/agent_attention.h says
  // and is why `lastWhy()` above exists.
  CHECK(std::string(payload.why) == std::string(kWhy).substr(0, cycle::kAgentWhyBytes - 1));

  // The line it logged says what happened and why, and carries no secret of any kind.
  CHECK(holds(g_log, "this firmware does not restart itself"));
  CHECK(holds(g_log, kWhy));
}

TEST_CASE("selfReboots is 0: the heartbeat's count is of restarts made that way, and none are") {
  Rig rig;
  CHECK(rig.platform.selfReboots() == 0u);
}

TEST_CASE("the library's log goes to the sink, stamped, and the level gates it") {
  Rig rig;
  mm::logf(mm::LogLevel::Info, "server: %s -> http %u", "/agent/v1/config", 200u);
  CHECK(holds(g_log, "server: /agent/v1/config -> http 200"));
  CHECK_MESSAGE(holds(g_log, "[+0s]"), "before this unit knows the time, the stamp is the uptime");
  CHECK(holds(g_log, "\r\n"));
  CHECK(rig.platform.logLines() == 1u);

  // Once the clock is known the stamp is the time of day: the library asks the platform for both.
  REQUIRE(rig.wall.give(kEpoch));
  g_log.clear();
  mm::logf(mm::LogLevel::Info, "server: a line with a time on it");
  CHECK(holds(g_log, "[00:00:00]"));

  // The level is the console's `log.level`, handed over at beginLog: a line below it is formatted
  // and dropped, and the sink never sees it.
  g_log.clear();
  rig.platform.beginLog(&captureLine, mm::LogLevel::Error);
  mm::logf(mm::LogLevel::Debug, "server: a line nobody asked for");
  CHECK(g_log.empty());
  mm::logf(mm::LogLevel::Error, "server: a line that matters");
  CHECK(holds(g_log, "a line that matters"));
}

TEST_CASE("a platform with no sink formats a line and drops it: a library owns no port") {
  Rig rig;
  g_log.clear();
  rig.platform.beginLog(nullptr, mm::LogLevel::Debug);
  mm::logf(mm::LogLevel::Error, "server: nowhere to put this");
  CHECK(g_log.empty());
  CHECK(rig.platform.logLines() == 0u);
  // And the next platform takes the sink back, which is what a second test in one process needs.
  rig.platform.beginLog(&captureLine, mm::LogLevel::Debug);
  mm::logf(mm::LogLevel::Error, "server: and now there is");
  CHECK(holds(g_log, "and now there is"));
}

TEST_CASE("a build with no duty cycle raises the attention nowhere, and still counts it") {
  fakes::FakeClock clock;
  fakes::FakeEntropy entropy;
  fakes::FakeHeap heap;
  fakes::FakeRetained retained;
  cycle::NetworkClock wall{clock};
  cycle::AgentAttention attention;  // no wake reason, no journal: a factory image
  agent::WalterPlatform platform{clock, wall, entropy, heap, &attention};
  platform.beginLog(nullptr, mm::LogLevel::Error);
  struct Yielded {};
  clock.on_delay = [](std::uint32_t) { throw Yielded{}; };
  try {
    platform.rebootForHealth("nothing to write it on");
  } catch (const Yielded&) {
  }
  CHECK(attention.raised() == cycle::agent_attention::kRebootWanted);
  CHECK(attention.rebootsWanted() == 1u);
  CHECK(platform.restartsRefused() == 1u);
}
