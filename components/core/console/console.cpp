#include "core/console/console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/link/client/words.h"
#include "core/swd/dp.h"
#include "core/swd/wire.h"
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
                 interfaces::Clock& clock, interfaces::Restart& restart,
                 link::client::Client& radio)
    : out_(out), pins_(pins), health_(health), clock_(clock), restart_(restart), radio_(radio) {}

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
    case Verb::kRakHello: rakHello(); return;
    case Verb::kRakPing: rakPing(); return;
    case Verb::kRakStatus: rakStatus(); return;
    case Verb::kRakId: rakId(); return;
    case Verb::kRakTime: rakTime(); return;
    case Verb::kRakTimeSet: rakTimeSet(p.command.epoch); return;
    case Verb::kRakPower: rakPower(); return;
    case Verb::kRakEnv: rakEnv(); return;
    case Verb::kRakCli: rakCli(p.command.text, p.command.text_size); return;
    case Verb::kRakEvents: rakEvents(); return;
    case Verb::kRakText: rakText(); return;
    case Verb::kRakSwdConnect: rakSwdConnect(); return;
  }
}

void Console::help() {
  print("commands:\n");
  print("  help                the commands\n");
  print("  status              version, uptime, why it booted, the pins, the radio's power\n");
  print("  reboots             why this board last restarted, and the four before that\n");
  print("  pins                the seven radio-facing pins: net, D-sub, GPIO, mode, level\n");
  print("  rak hello           say hello to the radio: version, role, slot, capabilities\n");
  print("  rak ping            ping it, with the round trip\n");
  print("  rak status          its uptime, why it booted, its clock, its flags and counters\n");
  print("  rak id              its public key and advert name\n");
  print("  rak time            read its clock; rak time set <epoch> sets it from here\n");
  print("  rak power           the three current-sense channels\n");
  print("  rak env             temperature, humidity, pressure\n");
  print("  rak cli <line>      run one line on the radio's own console\n");
  print("  rak events          the events the radio has sent\n");
  print("  rak text            the lines a scanner has sent instead of frames\n");
  print("  rak swd connect     connect to the radio's debug port over SWD: DPIDR and the power-up\n");
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
  const link::client::Counters& counted = radio_.counters();
  printf("link: %s; %s; retries %lu, timeouts %lu, drops %lu, events %lu\n",
         link::client::describe(radio_.mode()),
         radio_.session().up ? "a session is up" : "no session",
         static_cast<unsigned long>(counted.retries), static_cast<unsigned long>(counted.timeouts),
         static_cast<unsigned long>(radio_.receiver().dropped()),
         static_cast<unsigned long>(counted.events));
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
    // The radio is starting over: the session, the boot_id and - §9 - whatever it was running.
    radio_.radioWasReset();
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
    radio_.radioWasReset();  // the rail went down: a new boot over there, and the UART was taken
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

// ---------------------------------------------------------------------------- the link (p1-02)
//
// One command per request of spec/LINK-PROTOCOL.md §5, each printing the decoded answer in words
// or the one line that says why there is none. The decoded answer's variable-length fields are
// views into the client's buffer and are printed here, at once, which is what they are for.

namespace lc = link::client;

void Console::linkError(const char* what, lc::Fault fault, std::uint8_t status) {
  if (fault == lc::Fault::kRefused) {
    printf("error: %s: refused: %s\n", what, lc::statusName(status));
    return;
  }
  printf("error: %s: %s\n", what, lc::describe(fault));
}

void Console::printText(link::Text text) {
  if (text.data != nullptr && text.size > 0) {
    out_.write(text.data, text.size);
  }
  print("\n");
}

void Console::printBytes(const char* label, link::Bytes bytes) {
  const char* digits = "0123456789abcdef";
  print(label);
  char hex[65];  // 32 bytes a line: a 32-byte public key is one line, a longer field wraps
  std::size_t at = 0;
  for (std::size_t i = 0; i < bytes.size; ++i) {
    hex[at++] = digits[bytes.data[i] >> 4];
    hex[at++] = digits[bytes.data[i] & 0x0F];
    if (at + 2 > sizeof hex) {
      hex[at] = 0;
      print(hex);
      at = 0;
    }
  }
  hex[at] = 0;
  print(hex);
  print("\n");
}

void Console::rakHello() {
  const lc::Answer<link::HelloResponse> answer = radio_.hello();
  if (!answer.ok()) {
    linkError("rak hello", answer.fault, answer.status);
    // §9: three silences and NDJSON lines mean the other firmware is over there.
    if (radio_.mode() == lc::Mode::kText) {
      printf("the radio is running the scanner: %s (%lu lines seen; try 'rak text')\n",
             lc::describe(radio_.mode()),
             static_cast<unsigned long>(radio_.counters().text_lines));
    }
    return;
  }
  const lc::Session& s = radio_.session();
  char caps[lc::kWordsMax];
  lc::capabilityWords(s.caps, caps, sizeof caps);
  printf("hello: protocol %u.%u, role %u (%s), slot %u, max payload %u, boot_id 0x%08lx\n",
         static_cast<unsigned>(s.proto_major), static_cast<unsigned>(s.proto_minor),
         static_cast<unsigned>(s.role), lc::roleName(s.role), static_cast<unsigned>(s.slot),
         static_cast<unsigned>(s.max_payload), static_cast<unsigned long>(s.boot_id));
  printf("  firmware %s, board %s, upstream %s\n", s.fw_version, s.board, s.upstream);
  printf("  capabilities 0x%lx: %s\n", static_cast<unsigned long>(s.caps), caps);
  if (radio_.timePending()) {
    print("  its clock has not been set since it booted: the next request sets it\n");
  }
}

void Console::rakPing() {
  // The nonce is this board's clock: a number that differs every time, which is all §5.2 wants.
  const std::uint64_t at = clock_.nowMs();
  const std::uint32_t nonce = static_cast<std::uint32_t>(at);
  const lc::Answer<link::PingResponse> answer = radio_.ping(nonce);
  if (!answer.ok()) {
    linkError("rak ping", answer.fault, answer.status);
    return;
  }
  const std::uint64_t took = clock_.nowMs() - at;
  printf("ping: %s, uptime %lu s, %lu ms round trip, %u attempt%s\n",
         answer.message.nonce == nonce ? "the nonce came back" : "a different nonce came back",
         static_cast<unsigned long>(answer.message.uptime_s), static_cast<unsigned long>(took),
         static_cast<unsigned>(answer.attempts), answer.attempts == 1 ? "" : "s");
}

void Console::rakStatus() {
  const lc::Answer<link::StatusResponse> answer = radio_.status();
  if (!answer.ok()) {
    linkError("rak status", answer.fault, answer.status);
    return;
  }
  const link::StatusResponse& r = answer.message;
  char flags[lc::kWordsMax];
  lc::statusFlagWords(r.flags, flags, sizeof flags);
  printf("radio status: uptime %lu s, booted: %u (%s)\n", static_cast<unsigned long>(r.uptime_s),
         static_cast<unsigned>(r.reset_reason), lc::resetReasonName(r.reset_reason));
  printf("  clock %lu, time state %u (%s)\n", static_cast<unsigned long>(r.epoch),
         static_cast<unsigned>(r.time_state), lc::timeStateName(r.time_state));
  printf("  flags 0x%lx: %s\n", static_cast<unsigned long>(r.flags), flags);
  printf("  observations %u pending, %lu dropped; link errors %lu; attention %u (%s)\n",
         static_cast<unsigned>(r.obs_pending), static_cast<unsigned long>(r.obs_dropped),
         static_cast<unsigned long>(r.link_errors), static_cast<unsigned>(r.attention),
         lc::attentionName(r.attention));
}

void Console::rakId() {
  const lc::Answer<link::IdentityResponse> answer = radio_.identity();
  if (!answer.ok()) {
    linkError("rak id", answer.fault, answer.status);
    return;
  }
  printBytes("identity: public key ", answer.message.pubkey);
  print("  name ");
  printText(answer.message.name);
}

void Console::rakTime() {
  const lc::Answer<link::TimeGetResponse> answer = radio_.timeGet();
  if (!answer.ok()) {
    linkError("rak time", answer.fault, answer.status);
    return;
  }
  printf("radio clock: %lu, time state %u (%s)\n",
         static_cast<unsigned long>(answer.message.epoch),
         static_cast<unsigned>(answer.message.time_state),
         lc::timeStateName(answer.message.time_state));
}

void Console::rakTimeSet(std::uint32_t epoch) {
  // §5.4's source 3, a person at a console - and never *force*: force moves a MeshCore node's
  // clock backwards, which makes its own past look like a replay to everyone who heard it. The
  // radio refuses a backwards step and says so, which is the right answer to a typed mistake.
  const lc::Answer<link::TimeSetResponse> answer =
      radio_.timeSet(epoch, link::time_source::kConsole);
  if (!answer.ok()) {
    linkError("rak time set", answer.fault, answer.status);
    if (answer.fault == lc::Fault::kRefused && answer.status == link::status::kRange) {
      print("  a clock that goes backwards makes this node's own traffic look replayed; "
            "the radio refuses it\n");
    }
    return;
  }
  printf("radio clock: was %lu, now %lu, time state %u (%s)\n",
         static_cast<unsigned long>(answer.message.prev_epoch),
         static_cast<unsigned long>(answer.message.epoch),
         static_cast<unsigned>(answer.message.time_state),
         lc::timeStateName(answer.message.time_state));
}

void Console::rakPower() {
  const lc::Answer<link::PowerGetResponse> answer = radio_.powerGet();
  if (!answer.ok()) {
    linkError("rak power", answer.fault, answer.status);
    return;
  }
  // §5.8: channel 1 panel (positive = into the charger), 2 battery (positive = charging),
  // 3 total load (positive = drawn). The shunts are already applied by the radio.
  const char* names[3] = {"panel", "battery", "load"};
  printf("power (valid 0x%x):\n", static_cast<unsigned>(answer.message.valid));
  for (std::size_t i = 0; i < 3; ++i) {
    const link::PowerChannel& c = answer.message.channels[i];
    const bool valid = (answer.message.valid & (1u << i)) != 0;
    printf("  %-7s %5u mV, %6d mA%s\n", names[i], static_cast<unsigned>(c.bus_mv),
           static_cast<int>(c.current_ma), valid ? "" : "   (not read)");
  }
}

void Console::rakEnv() {
  const lc::Answer<link::EnvGetResponse> answer = radio_.envGet();
  if (!answer.ok()) {
    linkError("rak env", answer.fault, answer.status);
    return;
  }
  const link::EnvGetResponse& r = answer.message;
  // Hundredths, printed as hundredths: no floating point on the way to a console line. The sign
  // is the whole part's, so -0.25 °C prints as -0.25 and not as 0.-25.
  const int temp = r.temp_centi;
  const int whole = temp / 100;
  const unsigned frac = static_cast<unsigned>(temp < 0 ? -(temp % 100) : temp % 100);
  printf("env (valid 0x%x): %s%d.%02u C, %u.%02u %%rh, %lu Pa\n",
         static_cast<unsigned>(r.valid), temp < 0 && whole == 0 ? "-" : "", whole, frac,
         static_cast<unsigned>(r.rh_centi / 100), static_cast<unsigned>(r.rh_centi % 100),
         static_cast<unsigned long>(r.pressure_pa));
}

void Console::rakCli(const char* line, std::size_t size) {
  const lc::Answer<link::CliResponse> answer = radio_.cli(link::text(line, size));
  if (!answer.ok()) {
    linkError("rak cli", answer.fault, answer.status);
    if (answer.fault == lc::Fault::kLinkDown) {
      // §5.7: some lines restart the node and never answer. Silence after one of those is the
      // expected outcome, not a dead link.
      print("  some lines (reboot) restart the radio and never answer; that is expected\n");
    }
    return;
  }
  print("reply: ");
  printText(answer.message.text);
}

void Console::printEvent(const lc::QueuedEvent& event, std::size_t number) {
  printf("  %lu. %s (seq %u)", static_cast<unsigned long>(number), lc::eventName(event.type),
         static_cast<unsigned>(event.seq));
  switch (event.type) {
    case link::type::kEvBoot: {
      link::EvBoot decoded;
      if (!link::decode(event.view(), &decoded)) break;
      printf(": reset %u (%s), role %u (%s), boot_id 0x%08lx, firmware ",
             static_cast<unsigned>(decoded.reset_reason),
             lc::resetReasonName(decoded.reset_reason),
             static_cast<unsigned>(decoded.body.role), lc::roleName(decoded.body.role),
             static_cast<unsigned long>(decoded.body.boot_id));
      printText(decoded.body.fw_version);
      return;
    }
    case link::type::kEvMeshReply: {
      link::EvMeshReply decoded;
      if (!link::decode(event.view(), &decoded)) break;
      printf(": %s, tag 0x%08lx, snr %d/4 dB, rssi %d dBm, %lu ms\n",
             lc::replyStateName(decoded.reply.state),
             static_cast<unsigned long>(decoded.reply.tag),
             static_cast<int>(decoded.reply.snr_q4), static_cast<int>(decoded.reply.rssi),
             static_cast<unsigned long>(decoded.reply.elapsed_ms));
      return;
    }
    case link::type::kEvAttention: {
      link::EvAttention decoded;
      if (!link::decode(event.view(), &decoded)) break;
      printf(": %u (%s), detail %u, repeat %u\n", static_cast<unsigned>(decoded.reason),
             lc::attentionName(decoded.reason), static_cast<unsigned>(decoded.detail),
             static_cast<unsigned>(decoded.repeat));
      return;
    }
    default:
      break;
  }
  printf(": type 0x%02x, %u bytes\n", static_cast<unsigned>(event.type),
         static_cast<unsigned>(event.size));
}

void Console::rakEvents() {
  // Whatever has arrived since the last look: the main loop polls the port every time round, so
  // an event that came while nobody was asking is already here.
  radio_.poll();
  const lc::Counters& counted = radio_.counters();
  if (radio_.eventsQueued() == 0) {
    printf("no events queued (%lu seen, %lu dropped, %lu gaps)\n",
           static_cast<unsigned long>(counted.events),
           static_cast<unsigned long>(counted.events_dropped),
           static_cast<unsigned long>(counted.event_gaps));
    return;
  }
  printf("%lu event%s queued (%lu seen, %lu dropped, %lu gaps):\n",
         static_cast<unsigned long>(radio_.eventsQueued()),
         radio_.eventsQueued() == 1 ? "" : "s", static_cast<unsigned long>(counted.events),
         static_cast<unsigned long>(counted.events_dropped),
         static_cast<unsigned long>(counted.event_gaps));
  lc::QueuedEvent event;
  for (std::size_t number = 1; radio_.takeEvent(&event); ++number) {
    printEvent(event, number);
  }
}

void Console::rakText() {
  radio_.poll();
  const lc::Counters& counted = radio_.counters();
  if (radio_.textLines() == 0) {
    printf("no scanner lines (%lu seen, %lu dropped; the link is %s)\n",
           static_cast<unsigned long>(counted.text_lines),
           static_cast<unsigned long>(counted.text_dropped), lc::describe(radio_.mode()));
    return;
  }
  printf("%lu line%s kept of %lu seen (%lu dropped, %lu truncated):\n",
         static_cast<unsigned long>(radio_.textLines()), radio_.textLines() == 1 ? "" : "s",
         static_cast<unsigned long>(counted.text_lines),
         static_cast<unsigned long>(counted.text_dropped),
         static_cast<unsigned long>(counted.text_truncated));
  for (std::size_t i = 0; i < radio_.textLines(); ++i) {
    print("  ");
    printText(radio_.textLine(i));
  }
  radio_.clearText();
}

// ------------------------------------------------------------------------ the debug port (p1-03)
//
// Acquire the two lines, connect (line reset, the switch, line reset, DPIDR), ask for the debug
// and system power-up and wait for the acknowledges, withdraw the request - on the nRF52840 that
// is what ends debug interface mode (core/swd/dp.cpp, Dp::powerDown) - and release the lines.
// Every exit path releases them: the wire and the port live on this stack frame and nothing
// holds the pins after it returns.
void Console::rakSwdConnect() {
  const board::Outcome acquired = pins_.swdAcquire();
  if (acquired != board::Outcome::kOk) {
    printf("error: rak swd connect: %s\n", board::describe(acquired));
    return;
  }
  swd::Wire wire(pins_.swd(), clock_);
  swd::Dp dp(wire);
  printf("swd: %lu kHz (half-period %lu us): line reset, switch, line reset, DPIDR\n",
         static_cast<unsigned long>(wire.frequencyHz() / 1000),
         static_cast<unsigned long>(wire.halfPeriodUs()));
  const swd::Result connected = dp.connect();
  if (!connected.ok()) {
    printf("error: rak swd connect: %s\n", swd::describe(connected.status));
    pins_.swdRelease();
    print("  pins released\n");
    return;
  }
  const swd::DpidrFields f = swd::decodeDpidr(connected.value);
  printf("  DPIDR 0x%08lx: revision %u, part 0x%02x, DPv%u, designer 0x%03x%s%s\n",
         static_cast<unsigned long>(connected.value), static_cast<unsigned>(f.revision),
         static_cast<unsigned>(f.partno), static_cast<unsigned>(f.version),
         static_cast<unsigned>(f.designer), f.designer == swd::kDesignerArm ? " (Arm)" : "",
         connected.value == swd::kNrf52840Dpidr ? " - the nRF52840's" : " - not the nRF52840's 0x2ba01477");
  const swd::Result up = dp.powerUp();
  if (up.ok()) {
    printf("  power-up: debug and system domains acknowledged (CTRL/STAT 0x%08lx)\n",
           static_cast<unsigned long>(up.ctrl_stat));
  } else {
    printf("  power-up: %s (CTRL/STAT 0x%08lx)\n", swd::describe(up.status),
           static_cast<unsigned long>(up.ctrl_stat));
  }
  const swd::Result down = dp.powerDown();
  if (down.ok()) {
    print("  power-up request withdrawn: the radio leaves debug interface mode\n");
  } else {
    printf("  power-up request withdrawn: %s\n", swd::describe(down.status));
  }
  pins_.swdRelease();
  const swd::Dp::Counters& c = dp.counters();
  printf("  pins released; %lu transfers, %lu WAIT retries, %lu faults, %lu parity errors\n",
         static_cast<unsigned long>(c.transfers), static_cast<unsigned long>(c.wait_retries),
         static_cast<unsigned long>(c.faults), static_cast<unsigned long>(c.parity_errors));
}

}  // namespace console
}  // namespace core
