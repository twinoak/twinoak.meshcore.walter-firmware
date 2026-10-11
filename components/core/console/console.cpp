#include "core/console/console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/version.h"

namespace core {
namespace console {

namespace {

const char* modeName(interfaces::PinMode mode) {
  switch (mode) {
    case interfaces::PinMode::kInput: return "input";
    case interfaces::PinMode::kInputPullUp: return "input pull-up";
    case interfaces::PinMode::kOutput: return "output";
    case interfaces::PinMode::kOpenDrain: return "open-drain";
  }
  return "?";
}

}  // namespace

Console::Console(interfaces::TextOut& out, board::RadioPins& pins, health::Health& health,
                 interfaces::Clock& clock, interfaces::Restart& restart)
    : out_(out), pins_(pins), health_(health), clock_(clock), restart_(restart) {}

void Console::print(const char* text) { out_.write(text, std::strlen(text)); }

// Into a stack buffer: no heap. A line that does not fit is cut, never overrun.
void Console::printf(const char* format, ...) {
  char buf[200];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  if (n <= 0) return;
  out_.write(buf, static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n)
                                                           : sizeof buf - 1);
}

void Console::prompt() { print("> "); }

// The banner itself is main's (the bench reads it first, before the slot and the descriptor);
// this is the boot's reason and the first prompt.
void Console::begin() {
  const health::BootRecord& boot = health_.thisBoot();
  if (boot.reason == health::ResetReason::kDeepSleepWake) {
    printf("boot %lu: %s (%s)\n", static_cast<unsigned long>(health_.boots()),
           health::name(boot.reason), health::name(boot.wake));
  } else if (boot.reason == health::ResetReason::kSoftware) {
    printf("boot %lu: %s reset, %s\n", static_cast<unsigned long>(health_.boots()),
           health::name(boot.reason), health::name(boot.requested));
  } else {
    printf("boot %lu: %s reset\n", static_cast<unsigned long>(health_.boots()),
           health::name(boot.reason));
  }
  if (!health_.recordSurvived()) print("the boot record starts here (a power cut, or the first boot)\n");
  print("type 'help' for the commands\n");
  prompt();
}

void Console::feed(char c) {
  if (c == '\n' && last_was_cr_) {  // the LF of a CR LF
    last_was_cr_ = false;
    return;
  }
  last_was_cr_ = c == '\r';

  if (c == '\r' || c == '\n') {
    print("\n");
    line_[length_] = 0;
    runLine(line_);
    length_ = 0;
    prompt();
    return;
  }
  if (c == '\b' || c == 0x7f) {
    if (length_ > 0) {
      length_--;
      print("\b \b");
    }
    return;
  }
  if (length_ >= kMaxLineLength + 2) {
    // Keep taking characters so the line ends where the terminal's does, but drop them: the
    // parser refuses the line as too long.
    length_ = kMaxLineLength + 3;
    return;
  }
  line_[length_++] = c;
  out_.write(&c, 1);
}

void Console::runLine(const char* line) {
  if (std::strlen(line) > kMaxLineLength + 2) {
    print("error: line too long (200 characters at most)\n");
    return;
  }
  const Parsed p = parseLine(line);
  if (!p.ok) {
    printf("error: %s\n", p.error);
    return;
  }
  switch (p.command.verb) {
    case Verb::kEmpty: return;
    case Verb::kHelp: help(); return;
    case Verb::kStatus: status(); return;
    case Verb::kReboots: reboots(); return;
    case Verb::kPins: pins(); return;
    case Verb::kRakReset: rakReset(); return;
    case Verb::kRakKill: rakKill(p.command.seconds); return;
    case Verb::kReboot: reboot(); return;
  }
}

void Console::help() {
  print("commands:\n");
  print("  help                the commands\n");
  print("  status              version, uptime, why it booted, the pins, the radio's power\n");
  print("  reboots             why this board last restarted, and the four before that\n");
  print("  pins                the seven radio-facing pins: net, D-sub, GPIO, mode, level\n");
  print("  rak reset           one 100 ms reset pulse to the radio (a second after the last)\n");
  printf("  rak kill <seconds>  the radio's rail off for that long (%lu..%lu), then back\n",
         static_cast<unsigned long>(pins_.timing().min_ms / 1000),
         static_cast<unsigned long>(pins_.timing().max_ms / 1000));
  print("  reboot              restart the Walter\n");
}

void Console::status() {
  const health::BootRecord& boot = health_.thisBoot();
  printf("%s\n", core::banner());
  printf("uptime %lu s, boot %lu since the record began\n",
         static_cast<unsigned long>(health_.uptimeS()),
         static_cast<unsigned long>(health_.boots()));
  if (boot.reason == health::ResetReason::kDeepSleepWake) {
    printf("booted: %s, %s\n", health::name(boot.reason), health::name(boot.wake));
  } else if (boot.reason == health::ResetReason::kSoftware) {
    printf("booted: %s reset, %s\n", health::name(boot.reason), health::name(boot.requested));
  } else {
    printf("booted: %s reset\n", health::name(boot.reason));
  }
  printf("radio power: %s\n", board::describe(pins_.power()));
  printf("uart: %s; swd: %s; nRESET: %s; kill line: %s\n",
         pins_.uartEnabled() ? "enabled" : "released", pins_.swdHeld() ? "acquired" : "released",
         pins_.resetAsserted() ? "asserted" : "released",
         pins_.killDriven() ? "driven high" : "not driven");
  const std::uint32_t wait = pins_.msUntilResetAllowed();
  if (wait == 0) {
    print("next reset: allowed now\n");
  } else {
    printf("next reset: in %lu ms (two resets are a second apart)\n",
           static_cast<unsigned long>(wait));
  }
  print("pins:");
  for (std::size_t i = 0; i < board::RadioPins::kPinCount; ++i) {
    const board::PinState s = pins_.state(i);
    printf(" %s=%s/%s", s.net, modeName(s.mode), s.level ? "high" : "low");
  }
  print("\n");
}

void Console::reboots() {
  printf("boots since the record began: %lu%s\n", static_cast<unsigned long>(health_.boots()),
         health_.recordSurvived() ? "" : " (the record started at this boot: a power cut, or the first boot)");
  for (std::size_t i = 0; i < health_.count(); ++i) {
    const health::BootRecord& r = health_.at(i);
    printf("  %lu. %s", static_cast<unsigned long>(i + 1), health::name(r.reason));
    if (r.reason == health::ResetReason::kDeepSleepWake) printf(" (%s)", health::name(r.wake));
    if (r.reason == health::ResetReason::kSoftware) printf(" (%s)", health::name(r.requested));
    if (i == 0) {
      print(" - this boot");
    }
    if (r.previous_uptime_s > 0 || i + 1 < health_.count()) {
      printf("; the run before it lasted %lu s", static_cast<unsigned long>(r.previous_uptime_s));
    }
    print("\n");
  }
}

void Console::pins() {
  print("net            d-sub  gpio  mode           level  use\n");
  for (std::size_t i = 0; i < board::RadioPins::kPinCount; ++i) {
    const board::PinState s = pins_.state(i);
    printf("%-14s %5u  %4u  %-14s %-6s %s\n", s.net, static_cast<unsigned>(s.dsub),
           static_cast<unsigned>(s.gpio), modeName(s.mode), s.level ? "high" : "low", s.use);
  }
  printf("radio power: %s\n", board::describe(pins_.power()));
}

void Console::rakReset() {
  const std::uint32_t wait = pins_.msUntilResetAllowed();
  if (wait > 0) printf("waiting %lu ms: two resets are a second apart\n", static_cast<unsigned long>(wait));
  const board::Outcome r = pins_.resetPulse();
  if (r == board::Outcome::kOk) {
    printf("ok: nRESET low for %lu ms, then released\n", static_cast<unsigned long>(board::kResetPulseMs));
  } else {
    printf("error: rak reset: %s\n", board::describe(r));
  }
}

void Console::rakKill(std::uint32_t seconds) {
  // Bounded by the parser to nine digits; the product below cannot overflow a 64-bit value, and
  // RadioPins refuses anything over its maximum before touching a pin.
  const std::uint64_t ms = static_cast<std::uint64_t>(seconds) * 1000u;
  const std::uint32_t off_ms = ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ms);
  const board::Outcome r = pins_.killCycle(off_ms);
  if (r == board::Outcome::kOk) {
    printf("ok: rail off %lu s, released, settled %lu ms; radio power %s\n",
           static_cast<unsigned long>(seconds), static_cast<unsigned long>(pins_.timing().settle_ms),
           board::describe(pins_.power()));
  } else if (r == board::Outcome::kKillTooShort || r == board::Outcome::kKillTooLong) {
    printf("error: rak kill %lu: %s (%lu..%lu seconds)\n", static_cast<unsigned long>(seconds),
           board::describe(r), static_cast<unsigned long>(pins_.timing().min_ms / 1000),
           static_cast<unsigned long>(pins_.timing().max_ms / 1000));
  } else {
    printf("error: rak kill: %s\n", board::describe(r));
  }
}

void Console::reboot() {
  print("rebooting\n");
  pins_.safe();
  health_.noteRestart(health::Restart::kConsole);
  restart_.restart();
}

}  // namespace console
}  // namespace core
