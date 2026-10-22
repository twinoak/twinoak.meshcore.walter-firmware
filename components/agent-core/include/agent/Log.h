// The serial log: one entry point, a level, a clock.
//
// What it will and will not print is decided here for every task after this one:
//   - a line is "[HH:MM:SS] L text" once the clock is known, "[+123s] L text" before that;
//   - nothing the mesh says is ever an argument (the rails in README.md): frames are counted,
//     and `log.frames on` prints a header line per frame - type, route, lengths, SNR - never
//     a payload byte;
//   - no secret is ever an argument: the token and the passphrase are read from NVS into the
//     call that needs them and into nothing that is printed. There is no redactor here because
//     there is nothing to redact - the .NET agent redacts its log against secrets it holds as
//     strings; this firmware never holds one as a printable thing;
//   - every line also goes into the four-kilobyte ring `dump` prints (LogRing.h), which is a
//     board's answer to the Pi agent's journal: the same bytes the port carried, and no others.
//
// Where a formatted line goes is the platform's (agent/IPlatform.h, task p1.5-15): the library
// formats a line and hands it to the sink the platform set (logSetSink), and the Heltec's sink is
// the serial port and the ring (src/Log.cpp, src/SerialLog.h). Since task p1.5-16 the formatting
// is the library's and the port is nobody's until a platform says so: a line formatted with no
// sink set is dropped, because a library that owns no port can do nothing else with it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

enum class LogLevel : uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// `clock` returns UNIX seconds, or 0 while there is no time yet; `uptimeMs` is the monotonic
// millisecond count the "[+123s]" stamp before the first time is made of (IPlatform::millis() on
// any platform, Arduino's millis() on the Heltec). A null one stamps "+0s".
void logBegin(uint32_t (*clock)(), uint32_t (*uptimeMs)());
void logSetLevel(LogLevel level);
bool logSetLevelByName(const char* name);  // "error" | "warn" | "info" | "debug"
LogLevel logLevel();
const char* logLevelName(LogLevel level);

void logf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// The sink one formatted line goes to - the stamped line, CR LF and all, and its length. Unset,
// the line is formatted and dropped; set, every line goes there and nowhere else, and the sink
// owns the ring if it wants one. The Heltec sets `mm::serialLogSink` (src/SerialLog.h), which is
// the serial port and the four-kilobyte ring `dump` prints.
using LogSink = void (*)(const char* line, size_t len);
void logSetSink(LogSink sink);

}  // namespace mm

#define LOG_E(...) ::mm::logf(::mm::LogLevel::Error, __VA_ARGS__)
#define LOG_W(...) ::mm::logf(::mm::LogLevel::Warn, __VA_ARGS__)
#define LOG_I(...) ::mm::logf(::mm::LogLevel::Info, __VA_ARGS__)
#define LOG_D(...) ::mm::logf(::mm::LogLevel::Debug, __VA_ARGS__)
