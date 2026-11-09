// The console's one seam into the modem library's own log (task p10-06): `trace modem on` and
// `trace modem off`, and the line `status` prints while it is on.
//
// Why a seam. The trace is the library's `ESP_LOGD` lines - `TX:` for every AT command and every
// payload it hands the modem, `RX:` for everything the modem says back - and switching them on
// is `esp_log_level_set` on the library's own tag, which core/ may not name (rail R12: no
// ESP-IDF include and no WalterModem symbol in `components/core/`). So the console takes this
// interface and `main/` hands it `modem::WalterModemTrace`; a build without one - every host test
// but the one that fakes it - says so in one line.
//
// **It is not a setting.** Nothing persists it: the implementation holds it in RAM, so the next
// boot has it off, and `core/settings/keys.h` has no key for it. A unit nobody switches it on for
// prints exactly what it printed before this task existed.
//
// What crosses the seam is a bool and two secrets. The secrets cross **into** it and never back:
// they are what the trace must not carry out (`core/trace/redact.h`), the console reads them from
// the settings when the trace is switched on, and nothing of this interface can be asked what
// they were.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace console {

class ModemTrace {
 public:
  virtual ~ModemTrace() = default;

  // Whether the library's TX and RX lines are going to the port now. False at every boot.
  virtual bool on() const = 0;

  // Whether those lines are in this image at all (task p11-01). They are `ESP_LOGD` calls, and a
  // build whose CONFIG_LOG_MAXIMUM_LEVEL stops at info has none of them in its bytes - the
  // release build is such a build, because the debug strings cost 3 080 bytes of `.bss` that the
  // main task's stack needs (`docs/ARCHITECTURE.md` §14). Nothing can switch on what was not
  // compiled, so the console says so in one sentence rather than reporting a trace that prints
  // nothing; `docs/bench.md` §13 says how to make a build that has them.
  virtual bool compiledIn() const = 0;

  // Switch it on or off. `token` and `pin` are the two values no trace line may carry - the
  // `server.token` and `lte.pin` settings as they stand, either of them "" when unset - and are
  // copied before a single line can be printed. Returns false, with one sentence in `why`, when
  // this build or this board cannot: the hook needs a line buffer, and a board that cannot spare
  // one says so rather than tracing without the rail.
  virtual bool turn(bool on, const char* token, const char* pin, char* why, std::size_t cap) = 0;

  // The trace's own counters for this boot: lines the hook has printed, and how many of them had
  // a stretch taken out of them. What `trace` prints, so that the bench can see the rail work.
  virtual std::uint32_t lines() const = 0;
  virtual std::uint32_t redacted() const = 0;
};

}  // namespace console
}  // namespace core
