// The log, formatted: everything of agent/Log.h except where a line goes.
//
// Until task p1.5-16 this file was src/Log.cpp and ended in `Serial.print`. The formatting is
// portable - a level, a stamp, one `vsnprintf` into a static line - and the port is not, so the
// port is the sink a platform sets (logSetSink, agent/Log.h) and src/Log.cpp is the Heltec's:
// the serial port and the four-kilobyte ring. A platform that sets no sink gets a formatted line
// and drops it, which is the only honest default for a library that owns no port.

#include <agent/Log.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <agent/Iso8601.h>

namespace mm {

namespace {

uint32_t (*s_clock)() = nullptr;
uint32_t (*s_uptimeMs)() = nullptr;
LogLevel s_level = LogLevel::Info;
LogSink s_sink = nullptr;

char levelLetter(LogLevel l) {
  switch (l) {
    case LogLevel::Error: return 'E';
    case LogLevel::Warn: return 'W';
    case LogLevel::Info: return 'I';
    case LogLevel::Debug: return 'D';
  }
  return '?';
}

}  // namespace

void logBegin(uint32_t (*clock)(), uint32_t (*uptimeMs)()) {
  s_clock = clock;
  s_uptimeMs = uptimeMs;
}

void logSetSink(LogSink sink) { s_sink = sink; }

void logSetLevel(LogLevel level) { s_level = level; }

bool logSetLevelByName(const char* name) {
  if (name == nullptr) return false;
  if (std::strcmp(name, "error") == 0) { s_level = LogLevel::Error; return true; }
  if (std::strcmp(name, "warn") == 0) { s_level = LogLevel::Warn; return true; }
  if (std::strcmp(name, "info") == 0) { s_level = LogLevel::Info; return true; }
  if (std::strcmp(name, "debug") == 0) { s_level = LogLevel::Debug; return true; }
  return false;
}

LogLevel logLevel() { return s_level; }

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
  }
  return "?";
}

void logf(LogLevel level, const char* fmt, ...) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(s_level)) return;

  char stamp[16];
  const uint32_t now = s_clock ? s_clock() : 0;
  if (now != 0) {
    formatClock(now, stamp, sizeof stamp);
  } else {
    const uint32_t uptime = s_uptimeMs ? s_uptimeMs() : 0;
    std::snprintf(stamp, sizeof stamp, "+%lus", static_cast<unsigned long>(uptime / 1000));
  }

  // Static, not automatic: this is called from inside a job's exchange and the Arduino loop task
  // has 8 KB of stack. There is one task on this board - the loop - so there is one caller.
  static char line[240];
  static char full[288];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof line, fmt, args);
  va_end(args);

  const int n = std::snprintf(full, sizeof full, "[%s] %c %s\r\n", stamp, levelLetter(level), line);
  const size_t len = n < 0 ? 0 : (static_cast<size_t>(n) < sizeof full ? static_cast<size_t>(n) : sizeof full - 1);
  if (s_sink != nullptr) s_sink(full, len);
}

}  // namespace mm
