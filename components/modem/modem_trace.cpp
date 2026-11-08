#include "modem/modem_trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "core/trace/redact.h"

namespace modem {

namespace {

// The one set (see the header): the hook is `esp_log_set_vprintf`'s, which is the image's.
char* g_line = nullptr;
SemaphoreHandle_t g_lock = nullptr;
core::trace::Redactor g_redactor;
bool g_hooked = false;
bool g_on = false;
std::uint32_t g_lines = 0;
std::uint32_t g_redacted = 0;

// Every log line of this image once the trace has been switched on once. `esp_log_write` composes
// the whole line - level letter, timestamp, tag and the caller's format - and calls this once with
// it, so one call is one line. What the line becomes is core's and is tested on the host
// (core/trace/redact.h, `formatAndRedact`); what is here is the lock, the counters and the write.
int traceVprintf(const char* format, std::va_list args) {
  if (g_line == nullptr || g_lock == nullptr) return std::vprintf(format, args);
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(kTraceLockWaitMs)) != pdTRUE) {
    return 0;  // dropped rather than printed with the rail skipped
  }
  const core::trace::Formatted line =
      core::trace::formatAndRedact(g_redactor, g_line, kTraceLineBytes, format, args);
  if (line.length > 0) {
    ++g_lines;
    if (line.taken > 0) ++g_redacted;
    std::fwrite(g_line, 1, line.length, stdout);
  }
  xSemaphoreGive(g_lock);
  return static_cast<int>(line.length);
}

// The buffer, the lock and the hook, made once. False with one sentence in `why` when the board
// would not give them.
bool install(char* why, std::size_t cap) {
  const bool say = why != nullptr && cap > 0;
  if (g_line == nullptr) {
    g_line = static_cast<char*>(std::malloc(kTraceLineBytes));
    if (g_line == nullptr) {
      if (say) {
        std::snprintf(why, cap,
                      "the trace needs %u bytes for one line and the heap would not give them",
                      static_cast<unsigned>(kTraceLineBytes));
      }
      return false;
    }
  }
  if (g_lock == nullptr) {
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) {
      if (say) {
        std::snprintf(why, cap, "the trace needs a lock for its line buffer and there was none");
      }
      return false;
    }
  }
  if (!g_hooked) {
    esp_log_set_vprintf(&traceVprintf);
    g_hooked = true;
  }
  return true;
}

}  // namespace

bool WalterModemTrace::on() const { return g_on; }

std::uint32_t WalterModemTrace::lines() const { return g_lines; }

std::uint32_t WalterModemTrace::redacted() const { return g_redacted; }

bool WalterModemTrace::turn(bool on, const char* token, const char* pin, char* why,
                            std::size_t cap) {
  if (why != nullptr && cap > 0) why[0] = 0;
  // Switching off something that was never on costs nothing: no buffer, no hook, no line.
  if (!on && !g_hooked) {
    g_on = false;
    return true;
  }
  if (!install(why, cap)) return false;
  // The secrets before the level: a line must not be able to go out between the two.
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(kTraceLockWaitMs)) == pdTRUE) {
    g_redactor.keep(0, token);
    g_redactor.keep(1, pin);
    xSemaphoreGive(g_lock);
  } else {
    if (why != nullptr && cap > 0) {
      std::snprintf(why, cap, "the trace's line buffer was busy; nothing was changed");
    }
    return false;
  }
  // The library's own tag, and only it: the rest of the image stays at the default level, so a
  // trace somebody switched on does not turn every component of ESP-IDF loud.
  esp_log_level_set(kModemLogTag, on ? ESP_LOG_DEBUG
                                     : static_cast<esp_log_level_t>(CONFIG_LOG_DEFAULT_LEVEL));
  g_on = on;
  return true;
}

}  // namespace modem
