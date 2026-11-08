// The modem library's own TX and RX lines, in the release build, switched on from the console
// (task p10-06): `core::console::ModemTrace` over `esp_log`.
//
// -- what the library prints, read at 1.5.0 -----------------------------------------------------
//
// Three lines, all `ESP_LOGD` under the tag `WalterModem`:
//
//   `TX: <command>`   `WalterDefines.h`, the `_transmitCmd` macro: every AT command, element by
//                     element, **unescaped**. `AT+SQNSRECV=<id>,<n>` - the command the stall of
//                     8 November left unanswered for the library's whole 30 s timeout - is one of
//                     these, and so is `AT+CPIN=<pin>` (`unlockSIM`).
//   `RX: <escaped>`   `WalterModem.cpp`, `_processModemRSP`: every buffer the modem sent back,
//                     escaped into `_logEscapeBuffer`. `+SQNSRING`, `+SQNSRECV: <id>,<n>` with
//                     its length, `+SQNSH`, `+CME ERROR: 4`, and the bytes of an answer.
//   `TX: <escaped>`   `WalterModem.cpp`, at the modem's data prompt: the payload a `socketSend`
//                     hands over - which for this firmware is **the HTTP request as it is**,
//                     `Authorization: Bearer <token>` included, because TLS is inside the modem
//                     and the UART carries the head in plaintext.
//
// The last two are compiled in only at `CONFIG_LOG_MAXIMUM_LEVEL >= ESP_LOG_DEBUG`, which is why
// `sdkconfig.defaults` now sets it (and why it is the whole-image switch and not a per-file level:
// that guard is a Kconfig macro no `LOG_LOCAL_LEVEL` can raise). The default level is unchanged,
// so nothing of it prints until this class raises the tag.
//
// -- the rail -----------------------------------------------------------------------------------
//
// Every line that goes out while the hook is installed passes through `core::trace::Redactor`
// first, which takes out the value of `Authorization:`, the value of `AT+CPIN=` and every
// occurrence of the two secrets the console gave it. The hook is **installed once and never
// taken off again** for the rest of the boot: a hook that could be removed while another task
// was inside it would be a race with a line of trace in it, and leaving it in place makes the
// rail hold for every line the image logs, whoever logs it. Switching the trace off sets the tag
// back to the default level, which is what stops the lines.
//
// -- not a setting ------------------------------------------------------------------------------
//
// The state is this boot's. Nothing writes it to NVS and no key of `core/settings/keys.h` names
// it; a reset is what ends a trace somebody left on.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/console/modem_trace.h"

namespace modem {

// The library's log tag: every line of it is printed under this one name (`WalterModem.cpp` and
// `proto/*.cpp` pass the same string literal to every `ESP_LOGx`).
constexpr const char* kModemLogTag = "WalterModem";

// The buffer one line is formatted into. The longest the library can print is an escaped payload:
// `WALTER_MODEM_RSP_BUF_SIZE * 2` = 3080 bytes (`_logEscapeBuffer`), after `TX: ` and ESP-IDF's
// own `D (<ms>) WalterModem: ` prefix. 4096 holds that with room; a longer line is cut at the
// buffer rather than dropped or printed around the rail.
constexpr std::size_t kTraceLineBytes = 4096;

// How long a line waits for the buffer when another task is formatting one. One second is far
// more than a format and a write need; a line that cannot get the buffer is **dropped**, never
// printed unredacted.
constexpr std::uint32_t kTraceLockWaitMs = 1000;

// The hook, the buffer, the lock and the counters are **one set for the whole image**, because
// `esp_log_set_vprintf` is one hook for the whole image: two of these objects would be two names
// for the same thing, so this class carries no state of its own and `main/` constructs one.
class WalterModemTrace final : public core::console::ModemTrace {
 public:
  bool on() const override;
  bool turn(bool on, const char* token, const char* pin, char* why, std::size_t cap) override;
  std::uint32_t lines() const override;
  std::uint32_t redacted() const override;
};

}  // namespace modem
