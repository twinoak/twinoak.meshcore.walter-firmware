// The platform as the agent's loop and runner see it (task p4-04): `mm::IPlatform` and the
// `mm::IClock` behind it (components/agent-core, agent/IPlatform.h), over this firmware's own
// interfaces - the millisecond clock, the hardware's random bits, the heap's two figures, phase
// 3's network clock and the console's log ring. Seven primitives and a wall clock, and the report
// of this task has the table.
//
// What is here and nowhere else:
//
//   - **`delayMs()` is a yield, never a busy spin.** The library idles through it inside a job's
//     reply wait, which is where all of a wake's time is, so the delay pumps what must not stop
//     while the agent waits: the link client's receive side (an EV_MESH_REPLY or an EV_BOOT that
//     arrives while the runner is waiting is one this end heard) and the console's one byte. The
//     pump is a plain function pointer the owner sets, because that is what the library's own
//     `JobRunner::begin` takes and the two should be the same function
//     (docs/library.md §6 step 3).
//
//     This sentence was a wish until task p9-01. The library idles with `delayMs(1)`
//     (`JobRunner::idle`), and `EspClock::delayMs(1)` was `esp_rom_delay_us(1000)` below the
//     10 ms tick of `CONFIG_FREERTOS_HZ=100` - so a job that waited out the airtime budget, up to
//     about 40 s of it, held the CPU for all of it and the 30 s task watchdog reset the first unit
//     on 7 November 2026 inside a `config.read`. Nothing is different here: the tick is a
//     millisecond now (`sdkconfig.defaults`, held by a `static_assert` in `platform/clock.cpp`),
//     so what this class promises is what the clock under it does. The pump still runs **before**
//     the delay, so a wait of one millisecond pumps once.
//   - **`rebootForHealth(why)` does not restart this board.** The supervisor and the wake cycle
//     own restarts (spec/SYSTEM.md §9, core/cycle/wake_guard.h), so the request is journalled,
//     the Walter's own attention is raised for the wake record and `status`
//     (core/cycle/agent_attention.h), and then - because the method is `[[noreturn]]` and may
//     not return - the board is handed to the one thing that does own restarts: this yields for
//     ever and the bound on the wake ends the wake. **In one-wake mode the library never calls
//     it** (agent/AgentLoop.cpp: "the guard is told and not taken"; the report says
//     `rebootWanted` and the reason), so this is a guard on a door the Walter keeps shut, and
//     `selfReboots()` is 0 for the same reason: the heartbeat's `reboots` counts restarts made
//     *that way*, and this firmware makes none.
//   - **the clock.** `hasTime()`/`now()` are phase 3's network clock
//     (core/cycle/network_clock.h), which is this boot's only idea of what day it is; a deep
//     sleep takes it, and that is the honest behaviour (§5.4: a Walter has no RTC it can trust
//     across a sleep). `takeServerTime()` offers the server's time **to that same object**, so
//     that the radio's `TIME_SET` of the next wake carries source 2 and a real epoch rather than
//     nothing - and a time the cellular network has already given wins over it.
//   - **the log.** The library formats its own lines (agent/Log.h) and hands them to the one
//     sink a platform sets; this sets the console's, so an agent line is on the port and in the
//     four kilobytes `dump` prints, like every other line. The library's redaction is the
//     library's and is preserved by doing nothing to it: no line of ours is added to its text
//     and no payload, reply body or password is ever one of its arguments (its own rail), and a
//     host test greps the whole capture of a job that logs in for the fake's passwords.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IPlatform.h>
#include <agent/Log.h>

#include "core/cycle/agent_attention.h"
#include "core/cycle/network_clock.h"
#include "interfaces/clock.h"
#include "interfaces/entropy.h"
#include "interfaces/heap.h"

namespace agent {

// How long `rebootForHealth` yields for, in the loop it never leaves. A second: the wake's bound
// is minutes (core/cycle/wake_guard.h) and nothing is gained by looking at the clock more often.
constexpr std::uint32_t kRefusedRestartYieldMs = 1000;

// Where the library's lines go. A plain function pointer is what `mm::logSetSink` takes, so the
// sink is a free function and the object it writes through is this one pointer - there is one
// platform on a board and one in a test, and a second construction takes the pointer over, which
// is what a test wants (the capture follows the object under test).
class WalterPlatform;
WalterPlatform* logOwner();

// The wall clock half of `mm::IPlatform`, over phase 3's network clock.
class WalterClock final : public mm::IClock {
 public:
  explicit WalterClock(core::cycle::NetworkClock& wall) : wall_(wall) {}

  bool hasTime() const override;
  std::uint32_t now() const override;
  // The library's "a packet of ours left this board and something answered it": true for the
  // cellular network's time and for the server's, false for no time at all. This firmware has
  // no RTC and keeps no time across a sleep, so there is no third case.
  bool fromNetwork() const override;
  // One word for a job's log. **Not one of the library's four** ("sntp", "server", "rtc",
  // "none"): the Walter's time comes off the LTE-M network's clock and not from SNTP, so it says
  // `network`. The library compares this against nothing - it is one argument of one log line
  // (agent/JobRunner.cpp) - and the report says so.
  const char* sourceName() const override;
  bool takeServerTime(std::uint32_t serverEpoch, std::int32_t& difference) override;

  // How many server times were offered, taken and refused: what the console prints.
  std::uint32_t offered() const { return offered_; }
  std::uint32_t taken() const { return taken_; }

 private:
  core::cycle::NetworkClock& wall_;
  std::uint32_t offered_ = 0;
  std::uint32_t taken_ = 0;
};

class WalterPlatform final : public mm::IPlatform {
 public:
  // `attention` and `pump` may be left out: a build with no duty cycle raises the attention
  // nowhere (core/cycle/agent_attention.h says what that means) and a platform with no pump
  // delays without pumping, which is what a test that drives the clock itself wants.
  WalterPlatform(interfaces::Clock& clock, core::cycle::NetworkClock& wall,
                 interfaces::Entropy& entropy, interfaces::Heap& heap,
                 core::cycle::AgentAttention* attention = nullptr);
  // The library's sink is a plain function pointer through one static owner, so a platform that
  // goes away takes it with it: on a board this never happens (there is one, with static
  // storage), and in a test a second one must not leave a dangling pointer behind.
  ~WalterPlatform() override;

  // Where the library's log lines go, and at what level: both once, before anything of the
  // library runs (docs/library.md §5). `sink` is this firmware's - the console's `agentLine` -
  // and a null one leaves the library formatting lines and dropping them, which is what a build
  // with no console does.
  void beginLog(void (*sink)(const char* line), mm::LogLevel level);
  // What `delayMs()` pumps. The same function pointer the library's `JobRunner::begin` is given.
  void pumpWith(void (*pump)()) { pump_ = pump; }

  // ------------------------------------------------------------------ mm::IPlatform

  std::uint32_t millis() override;
  void delayMs(std::uint32_t ms) override;
  std::uint32_t random() override;
  mm::IClock& clock() override { return clock_out_; }
  std::uint32_t freeHeap() override { return heap_.freeBytes(); }
  std::uint32_t minFreeHeap() override { return heap_.minFreeBytes(); }
  [[noreturn]] void rebootForHealth(const char* why) override;
  std::uint32_t selfReboots() override { return 0; }

  // ------------------------------------------------------------------ what the console reads

  WalterClock& wallClock() { return clock_out_; }
  std::uint32_t delays() const { return delays_; }
  std::uint32_t pumps() const { return pumps_; }
  std::uint32_t randoms() const { return randoms_; }
  std::uint32_t logLines() const { return log_lines_; }
  // How many times the library asked for a restart it did not get.
  std::uint32_t restartsRefused() const { return restarts_refused_; }

  // The sink `mm::logSetSink` is given, and the two stamps `mm::logBegin` is given: free
  // functions, through logOwner().
  static void logSink(const char* line, std::size_t len);
  static std::uint32_t logEpoch();
  static std::uint32_t logUptimeMs();

 private:
  interfaces::Clock& clock_;
  interfaces::Entropy& entropy_;
  interfaces::Heap& heap_;
  core::cycle::AgentAttention* attention_ = nullptr;
  WalterClock clock_out_;

  void (*sink_)(const char* line) = nullptr;
  void (*pump_)() = nullptr;
  std::uint32_t delays_ = 0;
  std::uint32_t pumps_ = 0;
  std::uint32_t randoms_ = 0;
  std::uint32_t log_lines_ = 0;
  std::uint32_t restarts_refused_ = 0;
};

}  // namespace agent
