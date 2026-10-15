// The platform behind the seam, on a host (task p1.5-17): a clock that only moves when the test
// says so, a seeded random, a log that is captured rather than printed, and a reboot that is
// recorded rather than taken.
//
// **The clock is the whole point.** `AgentLoop` and `JobRunner` wait by calling
// `IPlatform::delayMs` in a loop (`JobRunner::idle`, `AgentMesh::await`'s host twin), and here
// that call *is* the passage of time: nothing sleeps, so a config.read that is eleven minutes of
// airtime budget on the board is a few thousand iterations of arithmetic here. Every assertion
// about a wait - the budget's minimum gap, the per-minute cap, a lease that ran out, the
// backoff, ninety minutes of failures - is an assertion about this counter.
//
// The wall clock moves with the millisecond counter, as a board's does: `advance()` carries both,
// so a lease in UNIX seconds and a gap in milliseconds cannot drift apart inside one test.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <agent/IPlatform.h>
#include <agent/Log.h>

namespace fake {

class FakeClock : public mm::IClock {
 public:
  bool hasTime() const override { return has_; }
  uint32_t now() const override { return has_ ? epoch_ : 0; }
  bool fromNetwork() const override { return has_ && fromNetwork_; }
  const char* sourceName() const override { return has_ ? (fromNetwork_ ? "sntp" : "rtc") : "none"; }

  // AgentClock's rule: a time that came off the network already wins over the server's.
  bool takeServerTime(uint32_t serverEpoch, int32_t& difference) override {
    difference = 0;
    if (serverEpoch == 0) return false;
    difference = static_cast<int32_t>(serverEpoch - now());
    if (has_ && fromNetwork_) return false;
    epoch_ = serverEpoch;
    has_ = true;
    fromNetwork_ = true;
    serverTimes_++;
    return true;
  }

  // What a test sets up: a clock off the network, a clock off an RTC, or none at all.
  void setFromNetwork(uint32_t epoch) {
    epoch_ = epoch;
    has_ = true;
    fromNetwork_ = true;
  }
  void setFromRtc(uint32_t epoch) {
    epoch_ = epoch;
    has_ = true;
    fromNetwork_ = false;
  }
  void unset() {
    has_ = false;
    fromNetwork_ = false;
    epoch_ = 0;
  }
  void advanceSeconds(uint32_t s) {
    if (has_) epoch_ += s;
  }
  int serverTimesTaken() const { return serverTimes_; }

 private:
  uint32_t epoch_ = 0;
  bool has_ = false;
  bool fromNetwork_ = false;
  int serverTimes_ = 0;
};

// Every line `mm::logf` formats, captured. The sink is a function pointer (agent/Log.h), so the
// capture is one per process - which is what a suite of one test binary needs.
class LogCapture {
 public:
  static LogCapture& the() {
    static LogCapture capture;
    return capture;
  }

  void begin(mm::LogLevel level = mm::LogLevel::Debug) {
    lines_.clear();
    mm::logBegin(nullptr, nullptr);
    mm::logSetLevel(level);
    mm::logSetSink(&sink);
  }

  void clear() { lines_.clear(); }
  const std::vector<std::string>& lines() const { return lines_; }

  std::string text() const {
    std::string all;
    for (const std::string& line : lines_) all += line;
    return all;
  }

  bool holds(const std::string& needle) const { return text().find(needle) != std::string::npos; }

 private:
  static void sink(const char* line, size_t len) { the().lines_.emplace_back(line, len); }

  std::vector<std::string> lines_;
};

class FakePlatform : public mm::IPlatform {
 public:
  // What `rebootForHealth` does instead of restarting: the reason is recorded and this is
  // thrown, because the method is [[noreturn]] and a test that wants to see the guard fire must
  // not fall through into the rest of the loop. Nothing restarts.
  struct Reboot {
    std::string why;
  };

  explicit FakePlatform(uint32_t seed = 0x5eed1234u) : random_(seed) {}

  uint32_t millis() override { return ms_; }

  // The waits of the library are this call in a loop, and this is where time passes. A
  // zero-millisecond delay still moves the clock by one: on a board it is a yield and some
  // microseconds, and a loop that waited on a clock that never moved would not end.
  void delayMs(uint32_t ms) override {
    advance(ms == 0 ? 1 : ms);
    delays_++;
  }

  // xorshift32, seeded: a client id and a jitter that are the same on every run, so a failure is
  // the same failure twice.
  uint32_t random() override {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
  }

  mm::IClock& clock() override { return clock_; }
  FakeClock& fakeClock() { return clock_; }

  uint32_t freeHeap() override { return freeHeap_; }
  uint32_t minFreeHeap() override { return minFreeHeap_; }

  [[noreturn]] void rebootForHealth(const char* why) override {
    reboots_.emplace_back(why == nullptr ? "" : why);
    selfReboots_++;
    throw Reboot{reboots_.back()};
  }
  uint32_t selfReboots() override { return selfReboots_; }

  // ---------------------------------------------------------------- what a test drives

  void advance(uint32_t ms) {
    const uint32_t before = ms_;
    ms_ += ms;
    // The wall clock moves with the millisecond counter, to the second.
    const uint32_t seconds = (ms_ / 1000u) - (before / 1000u);
    if (seconds > 0) clock_.advanceSeconds(seconds);
  }
  void advanceSeconds(uint32_t s) { advance(s * 1000u); }

  void setFreeHeap(uint32_t free, uint32_t minEver) {
    freeHeap_ = free;
    minFreeHeap_ = minEver;
  }
  void setSelfReboots(uint32_t n) { selfReboots_ = n; }

  const std::vector<std::string>& reboots() const { return reboots_; }
  bool rebooted() const { return !reboots_.empty(); }
  uint32_t delays() const { return delays_; }

 private:
  FakeClock clock_;
  uint32_t ms_ = 0;
  uint32_t random_;
  uint32_t freeHeap_ = 180000;
  uint32_t minFreeHeap_ = 160000;
  uint32_t selfReboots_ = 0;
  uint32_t delays_ = 0;
  std::vector<std::string> reboots_;
};

}  // namespace fake
