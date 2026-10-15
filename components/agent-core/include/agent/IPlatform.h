// The platform as the agent loop and the job runner see it (task p1.5-15): a millisecond clock, a
// wall clock, a delay, a random number, the heap figures and the one drastic thing a board can do
// to itself. Everything the portable files took from Arduino and the ESP-IDF directly before
// this, and nothing more.
//
// `Esp32Platform` (src/platform/Esp32Platform.h) is the Arduino-on-ESP32 implementation: the
// calls the files made before. The Walter of the repeater-v2 design implements it over ESP-IDF.
//
// The log is the third seam of this header and is not a method here: `mm::logf` (Log.h) is the
// one entry point every file already uses, and the sink it writes to is a pointer the platform
// sets with logSetSink() - the Heltec leaves it on its serial port and the 4 kB ring.

#pragma once

#include <cstdint>

namespace mm {

// The wall clock half: what AgentClock (src/net/Clock.h) provides, as the runner's lease check,
// the heartbeat's `at` and every result's timestamps read it.
class IClock {
 public:
  virtual ~IClock() = default;
  // False while nothing has set the clock; then now() is 0 and no lease has run out.
  virtual bool hasTime() const = 0;
  // UNIX seconds, 0 while !hasTime().
  virtual uint32_t now() const = 0;
  // The time came off the network - SNTP, or the server's own - rather than being kept across a
  // reset: with the link up, that is what proves the link (AgentLoop's backoff ceiling).
  virtual bool fromNetwork() const = 0;
  // One word for where the time came from, for a job's log: "sntp", "server", "rtc", "none".
  virtual const char* sourceName() const = 0;
  // Offer the server's time, which every config reply, heartbeat reply and claim carries. True
  // when the clock was set from it; `difference` is server minus local before any change, for
  // the log. A time that came off the network already wins over it (AgentClock's rule).
  virtual bool takeServerTime(uint32_t serverEpoch, int32_t& difference) = 0;
};

class IPlatform {
 public:
  virtual ~IPlatform() = default;

  // Milliseconds since boot, wrapping every 49.7 days: every comparison in the library is a
  // signed difference, which is right across the wrap.
  virtual uint32_t millis() = 0;
  // Yield for this long; the runner's idle loop calls it between pumps.
  virtual void delayMs(uint32_t ms) = 0;
  // Thirty-two random bits: client ids, the backoff's jitter, a request's nonce.
  virtual uint32_t random() = 0;

  virtual IClock& clock() = 0;

  // The heap, for the heartbeat and the log: what is free now, and the least that was.
  virtual uint32_t freeHeap() = 0;
  virtual uint32_t minFreeHeap() = 0;

  // Restart the board on purpose, having noted why (what Health.h's healthRebootNow does):
  // the loop's answer to a TLS stack that has failed for an hour and a half against a live link.
  // Does not return.
  [[noreturn]] virtual void rebootForHealth(const char* why) = 0;
  // How many times this firmware has restarted itself that way since the last power cut: the
  // heartbeat's `reboots` counter.
  virtual uint32_t selfReboots() = 0;
};

}  // namespace mm
