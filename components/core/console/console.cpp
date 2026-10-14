#include "core/console/console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/image/crypto/ed25519.h"
#include "core/image/trust.h"
#include "core/link/client/words.h"
#include "core/nrf52/nrf52.h"
#include "core/swd/dp.h"
#include "core/swd/mem_ap.h"
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

// Which lines are about a radio, and therefore about a slot (task p1-06). No `default`: a new
// verb has to be put on one side or the other, and -Wswitch - a warning, and in our own
// components an error - is what asks.
bool isRadioVerb(Verb verb) {
  switch (verb) {
    case Verb::kEmpty:
    case Verb::kHelp:
    case Verb::kStatus:
    case Verb::kReboots:
    case Verb::kPins:
    case Verb::kReboot:
    case Verb::kTrust:
    case Verb::kImageList:
    case Verb::kImageVerify:
    case Verb::kImageErase:
    case Verb::kImageBad:
    case Verb::kImageReceive:
      return false;
    case Verb::kRakReset:
    case Verb::kRakKill:
    case Verb::kRakHello:
    case Verb::kRakPing:
    case Verb::kRakStatus:
    case Verb::kRakId:
    case Verb::kRakTime:
    case Verb::kRakTimeSet:
    case Verb::kRakPower:
    case Verb::kRakEnv:
    case Verb::kRakCli:
    case Verb::kRakEvents:
    case Verb::kRakText:
    case Verb::kRakSwdConnect:
    case Verb::kRakSwdId:
    case Verb::kRakSwdMem:
    case Verb::kRakFlashTest:
    case Verb::kRakUpdate:
    case Verb::kRakUpdateFirstInstall:
    case Verb::kRakUpdateStatus:
    case Verb::kRakRoleScanner:
    case Verb::kRakRoleRepeater:
      return true;
  }
  return false;
}

}  // namespace

Console::Console(interfaces::TextOut& out, board::RadioPins& pins, health::Health& health,
                 interfaces::Clock& clock, interfaces::Restart& restart,
                 link::client::Client& radio, board::SlotLock& slots, image::Store* images,
                 image::Receiver* receiver, update::RadioUpdate* update, update::Journal* journal)
    : out_(out), pins_(pins), health_(health), clock_(clock), restart_(restart), radio_(radio),
      slots_(slots), images_(images), receiver_(receiver), update_(update), journal_(journal),
      flasher_(pins, clock, slots) {}

bool Console::slotOk(board::Slot slot) {
  if (slots_.exists(slot)) return true;
  printf("error: slot %u: this adapter has %u radio, so slot 0 is the only one\n",
         static_cast<unsigned>(slot.index), static_cast<unsigned>(slots_.slots()));
  return false;
}

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
  // An update the journal says was in progress is taken up where it was (spec/SYSTEM.md §6:
  // "a Walter that resets in the middle resumes or rolls back; it never guesses"). tick()
  // drives it from here, one step per turn of the main loop, printing each transition.
  if (update_ != nullptr && update_->resume()) {
    printf("update: the journal says a run was in progress (%s); resuming at %s\n",
           update::describe(update_->request().kind), update::describe(update_->state()));
  }
  print("type 'help' for the commands\n");
  prompt();
}

void Console::feed(char c) {
  // `image receive` (task p2-02) hands the port to the transfer: the bytes are frames and not a
  // typed line, so there is no echo, no backspace and no prompt until it is over.
  if (receiver_ != nullptr && receiver_->active()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(c);
    receiver_->feed(&byte, 1);
    if (!receiver_->active()) transferEnded();
    return;
  }
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

void Console::tick() {
  // A resumed update, one step a turn (a `rak update` runs its own loop and is not here).
  if (update_ != nullptr && update_->active() && !driving_update_) {
    const update::StepResult s = update_->step();
    printUpdateStep(s);
    if (!update_->active()) {
      printUpdateOutcome();
      prompt();
    }
  }
  if (receiver_ == nullptr || !receiver_->active()) return;
  receiver_->tick();
  if (!receiver_->active()) transferEnded();
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
  // The slot, once, for the whole line (task p1-06): refused here when this adapter has no such
  // radio, and handed to the link client so that the session the `rak ...` commands read and
  // write is that radio's. Nothing is steered yet - the handlers below take the hold.
  slot_ = board::Slot{p.command.slot};
  if (isRadioVerb(p.command.verb)) {
    if (!slotOk(slot_)) return;
    if (!radio_.useSlot(slot_)) {
      printf("error: slot %u: the link is in the middle of something on slot %u\n",
             static_cast<unsigned>(slot_.index), static_cast<unsigned>(radio_.slot().index));
      return;
    }
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
    case Verb::kRakSwdId: rakSwdId(); return;
    case Verb::kRakSwdMem: rakSwdMem(p.command.address, p.command.words); return;
    case Verb::kRakFlashTest: rakFlashTest(); return;
    case Verb::kTrust: trust(); return;
    case Verb::kImageList: imageList(); return;
    case Verb::kImageVerify: imageVerify(static_cast<image::Which>(p.command.which)); return;
    case Verb::kImageErase: imageErase(static_cast<image::Which>(p.command.which)); return;
    case Verb::kImageBad: imageBad(); return;
    case Verb::kImageReceive: imageReceive(static_cast<image::Which>(p.command.which)); return;
    case Verb::kRakUpdate: rakUpdate(update::Kind::kUpdate); return;
    case Verb::kRakUpdateFirstInstall: rakUpdate(update::Kind::kFirstInstall); return;
    case Verb::kRakUpdateStatus: rakUpdateStatus(); return;
    case Verb::kRakRoleScanner: rakUpdate(update::Kind::kRoleScanner); return;
    case Verb::kRakRoleRepeater: rakUpdate(update::Kind::kRoleRepeater); return;
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
  print("  rak swd id          identify the radio's chip over SWD: part, variant, flash, protection, core\n");
  print("  rak swd mem <addr> [words]\n");
  print("                      read 1 to 32 words of the chip's memory (read-only; hex 0x... or decimal)\n");
  print("  rak flash-test      write, verify and erase a 2-page pattern at the top of the application\n");
  print("                      region, only if those pages are blank; the radio restarts\n");
  print("  trust               the release keys this build accepts an update signed with\n");
  print("  image list          the radio images this Walter holds, per slot\n");
  print("  image verify <which>\n");
  print("                      read one back out of flash and hold it to the hash in its record\n");
  print("  image erase <candidate|scanner>\n");
  print("                      forget one; the golden image is never erased\n");
  print("  image bad           the hashes of candidates that failed a health check\n");
  print("  image receive <which>\n");
  print("                      take manifest.json, its .sig and the image over this port\n");
  print("                      (tools/walterctl.py push); the six checks run here\n");
  print("  rak update          flash the candidate, prove it, make it golden - or roll back to the\n");
  print("                      golden image and never try that candidate again (minutes; docs/update.md)\n");
  print("  rak update --first-install\n");
  print("                      the same with no golden image to return to (a person's command)\n");
  print("  rak update status   the journal's story of the last update\n");
  print("  rak role scanner|repeater\n");
  print("                      flash the stored scanner image and prove a scanner is there; or\n");
  print("                      the golden image back, proved as an update would be\n");
  print("  rak reset           one 100 ms reset pulse to the radio (a second after the last)\n");
  printf("  rak kill <seconds>  the radio's rail off for that long (%lu..%lu), then back\n",
         static_cast<unsigned long>(pins_.timing().min_ms / 1000),
         static_cast<unsigned long>(pins_.timing().max_ms / 1000));
  print("  reboot              restart the Walter\n");
  printf("every 'rak ...' takes an optional slot after the word rak - rak @1 hello - and @0\n"
         "without one; this unit has %u radio and it is slot 0\n",
         static_cast<unsigned>(slots_.slots()));
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
  const board::SlotHold hold(slots_, slot_);
  if (!hold.ok()) {
    printf("error: rak reset: %s\n", board::describe(hold.result()));
    return;
  }
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
  // The kill line cuts the whole rail, both radios of a dual adapter together
  // (DUAL-RAK4630-FEASIBILITY.md §5: "You cannot power-cycle one module alone"). The hold is
  // taken all the same, because releasing every pin and bringing it back is one operation and
  // must not interleave with another slot's.
  const board::SlotHold hold(slots_, slot_);
  if (!hold.ok()) {
    printf("error: rak kill: %s\n", board::describe(hold.result()));
    return;
  }
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
  const board::SlotHold hold(slots_, slot_);
  if (!hold.ok()) {
    printf("error: rak swd connect: %s\n", board::describe(hold.result()));
    return;
  }
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

// --------------------------------------------------------------------------- the nRF52 (p1-04)
//
// A session: the lines acquired, Nrf52::open() - connect, power-up, the control access port's
// protection, the memory access port, FICR - then the command's own reads, then close() (the
// power-up request withdrawn) and the lines released. Nothing here halts the core or writes the
// chip: `id` and `mem` are read-only, and the flasher (task p1-05) is where writes begin.

namespace {

// One line for a result that is not ok: the nRF52 layer's word, the debug port's underneath it,
// and the address when there is one.
void describeFailure(char* buf, std::size_t size, const char* what, const core::nrf52::Result& r) {
  if (r.status == core::nrf52::Status::kSwd) {
    if (r.address != 0) {
      std::snprintf(buf, size, "error: %s: %s at 0x%08lx", what, swd::describe(r.swd),
                    static_cast<unsigned long>(r.address));
    } else {
      std::snprintf(buf, size, "error: %s: %s", what, swd::describe(r.swd));
    }
  } else if (r.address != 0) {
    std::snprintf(buf, size, "error: %s: %s (0x%08lx)", what, core::nrf52::describe(r.status),
                  static_cast<unsigned long>(r.address));
  } else {
    std::snprintf(buf, size, "error: %s: %s", what, core::nrf52::describe(r.status));
  }
}

}  // namespace

void Console::rakSwdId() {
  const board::SlotHold hold(slots_, slot_);
  if (!hold.ok()) {
    printf("error: rak swd id: %s\n", board::describe(hold.result()));
    return;
  }
  const board::Outcome acquired = pins_.swdAcquire();
  if (acquired != board::Outcome::kOk) {
    printf("error: rak swd id: %s\n", board::describe(acquired));
    return;
  }
  swd::Wire wire(pins_.swd(), clock_);
  swd::Dp dp(wire);
  nrf52::Nrf52 nrf(dp, clock_);
  printf("swd: %lu kHz: connect, power-up, control port, memory port, FICR\n",
         static_cast<unsigned long>(wire.frequencyHz() / 1000));
  // The slot the line asked for; the hold above is this command's (task p1-06).
  const nrf52::Result opened = nrf.open(slot_);
  char line[160];
  if (dp.connected()) {
    printf("  DPIDR 0x%08lx%s\n", static_cast<unsigned long>(dp.dpidr()),
           dp.dpidr() == swd::kNrf52840Dpidr ? " (the nRF52840's)" : " - not the nRF52840's 0x2ba01477");
  }
  if (nrf.ctrlApIdr() != 0) {
    const swd::ApIdrFields f = swd::decodeApIdr(nrf.ctrlApIdr());
    printf("  control access port: IDR 0x%08lx (%s)", static_cast<unsigned long>(nrf.ctrlApIdr()),
           f.designer == nrf52::ctrl_ap::kDesignerNordic ? "Nordic" : "not Nordic's");
    if (nrf.protection() == nrf52::Protection::kOpen) {
      print("; protection: open\n");
    } else if (nrf.protection() == nrf52::Protection::kProtected) {
      print("; protection: PROTECTED - memory access not attempted\n");
      print("  opening a protected module erases it whole (bootloader, SoftDevice, identity, UICR): a\n"
            "  bench procedure for a person (hardware repository, BENCH-BREADBOARD.md caveat 6), never\n"
            "  this firmware's\n");
    } else {
      print("\n");
    }
  }
  if (nrf.memAp().opened()) {
    const swd::ApIdrFields f = swd::decodeApIdr(nrf.memAp().idr());
    printf("  memory access port: IDR 0x%08lx (%s, %s%s)\n", static_cast<unsigned long>(nrf.memAp().idr()),
           f.designer == swd::kDesignerArm ? "Arm" : "not Arm", "class MEM-AP",
           nrf.memAp().idr() == swd::mem_ap::kCortexM4AhbApIdr ? "; the Cortex-M4's AHB-AP" : "");
  }
  const nrf52::Identity& id = nrf.identity();
  if (id.part != 0) {
    if (id.is_nrf52840) {
      printf("  chip: nRF52840, variant %s, flash %lu KB = %lu pages of %lu bytes (INFO.FLASH %lu KB)\n",
             id.variant, static_cast<unsigned long>(id.flash_bytes / 1024), static_cast<unsigned long>(id.pages),
             static_cast<unsigned long>(id.page_size), static_cast<unsigned long>(id.info_flash_kb));
    } else {
      printf("  chip: part 0x%lx, variant %s, %lu pages of %lu bytes - not the nRF52840 with 4096-byte pages this firmware knows: refused\n",
             static_cast<unsigned long>(id.part), id.variant, static_cast<unsigned long>(id.pages),
             static_cast<unsigned long>(id.page_size));
    }
    printf("  device id %08lx%08lx\n", static_cast<unsigned long>(id.device_id[1]),
           static_cast<unsigned long>(id.device_id[0]));
  }
  if (opened.ok()) {
    nrf52::CoreState state = nrf52::CoreState::kUnknown;
    const nrf52::Result core = nrf.coreState(&state);
    if (core.ok()) {
      printf("  core: %s\n", nrf52::describe(state));
    } else {
      describeFailure(line, sizeof line, "rak swd id: core", core);
      printf("%s\n", line);
    }
  } else {
    describeFailure(line, sizeof line, "rak swd id", opened);
    printf("%s\n", line);
  }
  const nrf52::Result closed = nrf.close();
  pins_.swdRelease();
  const swd::Dp::Counters& c = dp.counters();
  printf("  %s; pins released; %lu transfers, %lu WAIT retries, %lu faults, %lu parity errors\n",
         closed.ok() ? "power-up request withdrawn" : "power-up request withdrawn (not acknowledged)",
         static_cast<unsigned long>(c.transfers), static_cast<unsigned long>(c.wait_retries),
         static_cast<unsigned long>(c.faults), static_cast<unsigned long>(c.parity_errors));
}

void Console::rakSwdMem(std::uint32_t address, std::uint32_t words) {
  const board::SlotHold hold(slots_, slot_);
  if (!hold.ok()) {
    printf("error: rak swd mem: %s\n", board::describe(hold.result()));
    return;
  }
  const board::Outcome acquired = pins_.swdAcquire();
  if (acquired != board::Outcome::kOk) {
    printf("error: rak swd mem: %s\n", board::describe(acquired));
    return;
  }
  swd::Wire wire(pins_.swd(), clock_);
  swd::Dp dp(wire);
  nrf52::Nrf52 nrf(dp, clock_);
  char line[160];
  nrf52::Result r = nrf.open(slot_);
  if (!r.ok()) {
    describeFailure(line, sizeof line, "rak swd mem", r);
    printf("%s\n", line);
  } else {
    std::uint32_t data[kMaxMemWords];
    if (words > kMaxMemWords) words = kMaxMemWords;
    r = nrf.read(address, data, words);
    if (!r.ok()) {
      describeFailure(line, sizeof line, "rak swd mem", r);
      printf("%s\n", line);
    } else {
      for (std::uint32_t i = 0; i < words; ++i) {
        if (i % 4 == 0) printf("0x%08lx:", static_cast<unsigned long>(address + 4 * i));
        printf(" %08lx", static_cast<unsigned long>(data[i]));
        if (i % 4 == 3 || i + 1 == words) print("\n");
      }
    }
  }
  nrf.close();
  pins_.swdRelease();
}

// -------------------------------------------------------------------------- the flasher (p1-05)
//
// rak flash-test: the flasher's erase, write, verify and reset exercised on real silicon at the
// top of the application region, on two pages that must be blank, so that the firmware running
// on the radio is not at risk - it is restarted, not changed. The settings page is not touched.
// `rak flash <slot>` is phase 2's, when there is an image store to flash from.

namespace {

class PrintProgress final : public core::flasher::ProgressSink {
 public:
  explicit PrintProgress(interfaces::TextOut& out) : out_(out) {}
  void onProgress(const core::flasher::Progress& p) override {
    char buf[96];
    const int n = std::snprintf(buf, sizeof buf, "  %s: page %lu/%lu (0x%08lx)\n", core::flasher::describe(p.step),
                                static_cast<unsigned long>(p.pages_done), static_cast<unsigned long>(p.pages_total),
                                static_cast<unsigned long>(p.address));
    if (n > 0) out_.write(buf, static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n) : sizeof buf - 1);
  }

 private:
  interfaces::TextOut& out_;
};

}  // namespace

void Console::printFlashResult(const char* what, const flasher::Result& r) {
  const flasher::Counters& c = r.counters;
  if (r.ok()) {
    printf("  %s: done in %lu ms: %lu pages (%lu skipped, %lu written in place, %lu erased, %lu retries), %lu erases, %lu words written, %lu read\n",
           what, static_cast<unsigned long>(c.elapsed_ms), static_cast<unsigned long>(c.pages_total),
           static_cast<unsigned long>(c.pages_skipped), static_cast<unsigned long>(c.pages_written_in_place),
           static_cast<unsigned long>(c.pages_erased), static_cast<unsigned long>(c.retries),
           static_cast<unsigned long>(c.erases), static_cast<unsigned long>(c.words_written),
           static_cast<unsigned long>(c.words_read));
  } else if (r.outcome == flasher::Outcome::kRefused) {
    if (r.address != 0) {
      printf("error: %s: refused while %s: %s (0x%08lx)\n", what, flasher::describe(r.step), r.why,
             static_cast<unsigned long>(r.address));
    } else {
      printf("error: %s: refused while %s: %s\n", what, flasher::describe(r.step), r.why);
    }
  } else {
    char line[160];
    describeFailure(line, sizeof line, what, r.nrf);
    printf("%s - failed while %s%s%s\n", line, flasher::describe(r.step), r.why[0] != 0 ? ": " : "",
           r.why[0] != 0 ? r.why : "");
  }
  if (r.reset_issued) {
    print(r.port_closed_after_reset
              ? "  the radio was reset; its access port closed afterwards (a build code Fxx module); the next reset pulse waits out the spacing\n"
              : "  the radio was reset through the debug port; the next reset pulse waits out the spacing\n");
  }
  print("  pins released\n");
}

void Console::rakFlashTest() {
  // The board in that slot, from the table rather than from one name written here (task p1-06).
  const flasher::BoardProfile* found = flasher::profileForSlot(slot_);
  if (found == nullptr) {
    printf("error: rak flash-test: no board profile for slot %u\n",
           static_cast<unsigned>(slot_.index));
    return;
  }
  const flasher::BoardProfile& profile = *found;
  printf("flash-test: slot %u, %s, two pages at 0x%08lx..0x%08lx (the top of the application region), only if blank\n",
         static_cast<unsigned>(slot_.index), profile.name,
         static_cast<unsigned long>(profile.app.end - flasher::Flasher::kTestPages * nrf52::flash::kPageSize),
         static_cast<unsigned long>(profile.app.end));
  PrintProgress progress(out_);
  // The flasher takes the slot and holds it for the whole call.
  const flasher::Result r = flasher_.flashTest(profile, &progress, slot_);
  printFlashResult("rak flash-test", r);
}

// ----------------------------------------------------------------------- the trust table (p2-01)
//
// trust: the key ids this build accepts a release manifest signed with (spec/FIRMWARE-MANIFEST.md
// §3), from the table generated out of keys/release-keys.txt - or that there are none, which is
// the state until the release key exists and means no update is accepted (§5 check 1 fails for
// every manifest). A build that trusts the specification's public test key says so loudly.
void Console::trust() {
  const image::TrustTable& table = image::buildTrust();
  // The arithmetic itself, on this compiler: RFC 8032's first vector verified and a changed bit
  // refused. A few hundred milliseconds of TweetNaCl on the ESP32-S3; the bench writes it down.
  const std::uint64_t t0 = clock_.nowMs();
  const bool crypto_ok = image::ed25519SelfTest();
  printf("ed25519 self-test (RFC 8032 test 1, and one bit changed): %s, %lu ms\n",
         crypto_ok ? "passed" : "FAILED - this build cannot check a signature", static_cast<unsigned long>(clock_.nowMs() - t0));
  if (table.empty()) {
    print("trust: none: no update will be accepted (keys/release-keys.txt names no key;\n"
          "  the release key is made once, by a person: spec/SYSTEM.md section 10 item 1)\n");
    return;
  }
  printf("trust: %u key%s this build accepts a release signed with:", static_cast<unsigned>(table.count()),
         table.count() == 1 ? "" : "s");
  for (std::size_t i = 0; i < table.count(); ++i) {
    char id[image::kKeyIdHexSize + 1];
    image::keyIdHex(table.at(i).id, id);
    printf(" %s", id);
  }
  print("\n");
  if (image::trustsTestKey()) {
    print("  WARNING: this build trusts the specification's public TEST KEY: it is a test build\n"
          "  (version +TESTKEY) and accepts anybody's firmware; it must never be released\n");
  }
}

// --------------------------------------------------------------------- the image store (p2-02)
//
// docs/ARCHITECTURE.md section 6: per radio slot a golden image, a candidate and a scanner image,
// each whole or absent. These five commands are the whole of what a person at the port can do to
// it: look, re-hash, forget one that is not golden, see the bad list, and load one over USB.

namespace {

// A hash as hexadecimal, `bytes` of it, into a buffer of at least 2 * bytes + 1. The console has
// no printf of a byte array and core/ has no heap.
void hashHex(const std::uint8_t* hash, std::size_t bytes, char* out) {
  static const char* digits = "0123456789abcdef";
  for (std::size_t i = 0; i < bytes; ++i) {
    out[i * 2] = digits[hash[i] >> 4];
    out[i * 2 + 1] = digits[hash[i] & 0x0F];
  }
  out[bytes * 2] = 0;
}

}  // namespace

bool Console::imagesOk() {
  if (images_ != nullptr && receiver_ != nullptr) return true;
  print("error: there is no image store in this build (the rakfw partition of partitions.csv was\n"
        "  not found at boot; nothing can be stored or read)\n");
  return false;
}

void Console::imageOneLine(const image::Record& record) {
  printf("  %-9s  ", image::name(record.which));
  if (!record.present) {
    if (record.transfer) {
      char hash[9];
      hashHex(record.transfer_sha256, 4, hash);
      printf("empty; a transfer of sha256 %s stopped at %lu of %lu bytes (it can be resumed)\n",
             hash, static_cast<unsigned long>(record.transferred),
             static_cast<unsigned long>(record.transfer_size));
      return;
    }
    print("empty\n");
    return;
  }
  const char* role = image::role_name(record.descriptor.role);
  char hash[17];
  hashHex(record.sha256, 8, hash);
  printf("%s %s %s %s, %lu bytes at 0x%08lx, sha256 %s..., area %u\n",
         role != nullptr ? role : "?", record.descriptor.board, record.descriptor.band,
         record.descriptor.version, static_cast<unsigned long>(record.size),
         static_cast<unsigned long>(record.load_address), hash,
         static_cast<unsigned>(images_->areaOf(record.which, record.slot)));
  char key[image::kKeyIdHexSize + 1];
  image::keyIdHex(record.key_id, key);
  if (record.stored_at != 0) {
    printf("             stored at %lu (unix), from commit %s, signed by %s\n",
           static_cast<unsigned long>(record.stored_at), record.commit, key);
  } else {
    printf("             stored when the Walter had no clock, from commit %s, signed by %s\n",
           record.commit, key);
  }
}

void Console::imageList() {
  if (!imagesOk()) return;
  printf("image store: %u slots x %u images of %lu bytes (%lu sectors) and a header sector each,\n"
         "  and %lu index sectors: %lu bytes of a %lu-byte partition, %lu left; index sequence %lu\n",
         static_cast<unsigned>(image::kRadioSlots), static_cast<unsigned>(image::kWhichCount),
         static_cast<unsigned long>(image::kImageBytesMax),
         static_cast<unsigned long>(image::kDataSectors),
         static_cast<unsigned long>(image::kIndexCopies),
         static_cast<unsigned long>(image::kStoreBytes),
         static_cast<unsigned long>(image::kPartitionBytes),
         static_cast<unsigned long>(image::kPartitionBytes - image::kStoreBytes),
         static_cast<unsigned long>(images_->indexSequence()));
  for (std::uint8_t s = 0; s < image::kRadioSlots; ++s) {
    const board::Slot slot{s};
    const flasher::BoardProfile* profile = flasher::profileForSlot(slot);
    if (profile != nullptr) {
      printf("slot %u, %s\n", static_cast<unsigned>(s), profile->name);
    } else {
      printf("slot %u: no radio in this adapter (its areas are laid out all the same, so a dual\n"
             "  unit needs no new partition table)\n", static_cast<unsigned>(s));
    }
    for (std::uint8_t w = 0; w < image::kWhichCount; ++w) {
      image::Record record;
      const image::StoreError e = images_->read(static_cast<image::Which>(w), slot, &record);
      if (e != image::StoreError::kNone) {
        printf("  %-9s  error: %s\n", image::name(static_cast<image::Which>(w)),
               image::describe(e));
        continue;
      }
      imageOneLine(record);
    }
  }
}

void Console::imageVerify(image::Which which) {
  if (!imagesOk() || !slotOk(slot_)) return;
  image::Record record;
  const image::StoreError read = images_->read(which, slot_, &record);
  if (read != image::StoreError::kNone) {
    printf("error: %s\n", image::describe(read));
    return;
  }
  if (!record.present) {
    printf("error: slot %u's %s image: %s\n", static_cast<unsigned>(slot_.index),
           image::name(which), image::describe(image::StoreError::kEmpty));
    return;
  }
  const std::uint64_t t0 = clock_.nowMs();
  const image::StoreError e = images_->verify(which, slot_);
  const unsigned long ms = static_cast<unsigned long>(clock_.nowMs() - t0);
  if (e != image::StoreError::kNone) {
    printf("error: %s %s: %s\n", image::name(which), record.descriptor.version,
           image::describe(e));
    return;
  }
  printf("%s: %lu bytes read back and hashed in %lu ms: the SHA-256 is the one in its record\n",
         image::name(which), static_cast<unsigned long>(record.size), ms);
}

void Console::imageErase(image::Which which) {
  if (!imagesOk() || !slotOk(slot_)) return;
  const image::StoreError e = images_->erase(which, slot_);
  if (e != image::StoreError::kNone) {
    printf("error: %s\n", image::describe(e));
    return;
  }
  printf("%s: forgotten in slot %u (the bytes above the header are left; the next image erases\n"
         "  the sectors it needs)\n", image::name(which), static_cast<unsigned>(slot_.index));
}

void Console::imageBad() {
  if (!imagesOk()) return;
  if (images_->badCount() == 0) {
    print("image bad: no image has failed a health check on this unit\n");
    return;
  }
  printf("image bad: %u of at most %u hashes, refused at the door for ever:\n",
         static_cast<unsigned>(images_->badCount()), static_cast<unsigned>(image::kBadListMax));
  for (std::size_t i = 0; i < images_->badCount(); ++i) {
    char text[image::kSha256Size * 2 + 1];
    hashHex(images_->badAt(i), image::kSha256Size, text);
    printf("  %s\n", text);
  }
}

void Console::imageReceive(image::Which which) {
  if (!imagesOk() || !slotOk(slot_)) return;
  if (!receiver_->begin(which, slot_)) {
    printf("error: image receive: %s\n", receiver_->reason());
    return;
  }
  const image::SlotNeeds& needs = receiver_->needs();
  // One printf a line: Console::printf writes into a 200-byte buffer and cuts what does not fit.
  printf("image receive: slot %u's %s image; this port is now a framed binary transfer\n",
         static_cast<unsigned>(slot_.index), image::name(which));
  print("  (COBS and CRC-16, the box link's own framing). Send manifest.json, then its .sig,\n");
  printf("  then the file: tools/walterctl.py push --as %s\n", image::name(which));
  printf("  what will be accepted: role %s, board %s, band %s, and all six checks of\n",
         needs.role, needs.board, needs.band);
  print("  FIRMWARE-MANIFEST section 5 run here - the laptop is not trusted\n");
}

void Console::transferEnded() {
  printf("image receive: %s%s%s\n", receiver_->reason(),
         receiver_->reasonMore()[0] != 0 ? ": " : "", receiver_->reasonMore());
  printf("  %lu chunks, %lu sent twice, %lu out of order, %lu frames dropped\n",
         static_cast<unsigned long>(receiver_->chunks()),
         static_cast<unsigned long>(receiver_->duplicates()),
         static_cast<unsigned long>(receiver_->outOfOrder()),
         static_cast<unsigned long>(receiver_->droppedFrames()));
  if (receiver_->stored()) print("  'image list' shows it; 'image verify' reads it back\n");
  prompt();
}

// ------------------------------------------------------------------- the radio's update (p2-03)
//
// docs/update.md. `rak update` runs the machine (core/update/radio_update.h) to its end inside
// the command, printing each transition as the journal receives it: a flash is minutes and the
// proving period is minutes more, and the console is busy for all of it - which is what a person
// at the bench wants to watch. A resumed update (begin()) is driven by tick() instead.

bool Console::updateOk() {
  if (update_ != nullptr && journal_ != nullptr) return true;
  print("error: there is no update machine in this build (the journal or the rakfw partition of\n"
        "  partitions.csv was not found at boot)\n");
  return false;
}

void Console::printUpdateStep(const update::StepResult& s) {
  if (s.changed) {
    printf("update: %s -> %s%s%s\n", update::describe(s.before), update::describe(s.after),
           s.note[0] != 0 ? ": " : "", s.note);
    return;
  }
  if (s.reason == update::Reason::kJournal) {
    printf("update: stopped in %s: %s\n", update::describe(s.before), s.note);
  }
}

void Console::printUpdateOutcome() {
  const update::Status& st = update_->status();
  char hash[17];
  hashHex(st.sha256, 8, hash);
  switch (st.state) {
    case update::State::kDone:
      if (st.kind == update::Kind::kRoleScanner) {
        printf("rak role scanner: done: a scanner is running; golden and candidate untouched ('rak text')\n");
      } else if (st.kind == update::Kind::kRoleRepeater) {
        printf("rak role repeater: done: the golden image %s is back and proved\n", st.version);
      } else {
        printf("rak update: done: %s (sha256 %s...) is the golden image; 'image list' shows it, 'rak hello' its version\n",
               st.version, hash);
      }
      break;
    case update::State::kRestored:
      // Two printf calls: the two lines together are longer than printf's 200-byte buffer.
      printf("rak update: restored: the candidate failed its health check (%s%s%s) and is on the bad list;\n",
             update::describe(st.fail_reason), st.fail_step != update::ProveStep::kNone ? " at " : "",
             st.fail_step != update::ProveStep::kNone ? update::describe(st.fail_step) : "");
      printf("  the golden image %s is back and proved\n", st.version);
      break;
    case update::State::kStuck:
      printf("rak update: STUCK: %s - the radio has no proved application; a person's business now\n",
             update::describe(st.reason));
      break;
    case update::State::kRefused:
      printf("error: rak update: refused at check: %s (nothing was touched)\n", update::describe(st.reason));
      break;
    default:
      printf("rak update: %s\n", update::describe(st.state));
      break;
  }
  if (st.time_skipped) {
    print("  (the clock check was skipped: this Walter has no time to give until phase 3's modem)\n");
  }
}

void Console::driveUpdate() {
  driving_update_ = true;
  for (;;) {
    const update::StepResult s = update_->step();
    printUpdateStep(s);
    if (!update_->active()) break;
    if (!s.changed && s.reason == update::Reason::kJournal) break;  // it cannot go on, and said so
    if (s.waited) clock_.delayMs(update::kStepIdleMs);
  }
  driving_update_ = false;
  printUpdateOutcome();
}

void Console::rakUpdate(update::Kind kind) {
  if (!updateOk()) return;
  if (update_->active()) {
    printf("error: rak update: an update is already running (%s); 'rak update status' says where\n",
           update::describe(update_->state()));
    return;
  }
  update::Request request;
  request.kind = kind;
  request.slot = slot_;
  if (!update_->start(request)) {
    printf("error: rak update: %s\n", update::describe(update_->reason()));
    if (update_->reason() == update::Reason::kRadioUpdateInProgress) {
      print("  it is resumed at the next boot (or now: 'reboot'), never started over\n");
    }
    return;
  }
  printf("%s: slot %u; every transition is printed as the journal takes it (this takes minutes:\n"
         "  the flash, then a %lu s proving period)\n",
         kind == update::Kind::kRoleScanner ? "rak role scanner"
         : kind == update::Kind::kRoleRepeater ? "rak role repeater" : "rak update",
         static_cast<unsigned>(slot_.index), static_cast<unsigned long>(update::kProvingPeriodMs / 1000));
  PrintProgress progress(out_);
  update_->setProgress(&progress);
  driveUpdate();
  update_->setProgress(nullptr);
}

void Console::printUpdateEntry(std::size_t number, const update::Entry& entry, const update::Payload& p) {
  char hash[17];
  hashHex(p.sha256, 8, hash);
  printf("  %2lu. seq %lu: %s%s", static_cast<unsigned long>(number), static_cast<unsigned long>(entry.seq),
         update::describe(p.state), (p.flags & update::kFlagResumed) != 0 ? " (resumed after a restart)" : "");
  if (p.state == update::State::kCheck) {
    printf(" - %s, slot %u, image sha256 %s...", update::describe(p.kind), static_cast<unsigned>(p.slot), hash);
  }
  if (p.reason != update::Reason::kNone) printf(": %s", update::describe(p.reason));
  if (p.step != update::ProveStep::kNone && p.step != update::ProveStep::kPassed) {
    printf(" (at: %s)", update::describe(p.step));
  }
  if (p.step == update::ProveStep::kPassed && p.detail != 0) {
    printf(" (the health check passed; %lu ms of proving)", static_cast<unsigned long>(p.detail));
  }
  print("\n");
}

void Console::rakUpdateStatus() {
  if (!updateOk()) return;
  // The story: the entries of the last run, from its check to its last, oldest first. The
  // journal walks backwards, so they are gathered into a small array and printed in reverse;
  // a run is bounded (update::kEntriesPerRunMax), so the array is too.
  // Static, not on the stack: the main task's stack is 3584 bytes and sixteen entries are
  // 1.6 KB. One console, one story at a time.
  constexpr std::size_t kStoryMax = 16;
  static update::Entry entries[kStoryMax];
  static update::Payload payloads[kStoryMax];
  std::size_t count = 0;
  update::Cursor cursor;
  update::Entry e;
  bool more = journal_->first(&cursor, &e);
  while (more && count < kStoryMax) {
    update::Payload p;
    if (update::decode(e, &p)) {
      entries[count] = e;
      payloads[count] = p;
      ++count;
      if (p.state == update::State::kCheck && (p.flags & update::kFlagResumed) == 0) break;  // the run's start
    }
    more = journal_->previous(&cursor, &e);
  }
  printf("journal: %lu entries in %lu sectors, next seq %lu, head sector %lu slot %lu\n",
         static_cast<unsigned long>(journal_->validEntries()), static_cast<unsigned long>(journal_->sectors()),
         static_cast<unsigned long>(journal_->nextSeq()), static_cast<unsigned long>(journal_->headSector()),
         static_cast<unsigned long>(journal_->headSlot()));
  if (count == 0) {
    print("rak update status: no update has been journalled on this unit\n");
    return;
  }
  printf("the last update's story (%lu entries, newest seq %lu):\n", static_cast<unsigned long>(count),
         static_cast<unsigned long>(entries[0].seq));
  for (std::size_t i = count; i > 0; --i) printUpdateEntry(count - i + 1, entries[i - 1], payloads[i - 1]);
  const update::Payload& last = payloads[0];
  if (update_->active()) {
    const update::Status& st = update_->status();
    printf("now: %s%s, %s; %lu pings, %lu ms of proving; %lu entries this run\n", update::describe(st.state),
           st.resumed ? " (resumed)" : "", update::describe(st.step), static_cast<unsigned long>(st.pings),
           static_cast<unsigned long>(st.prove_elapsed_ms), static_cast<unsigned long>(st.entries));
  } else if (update::isTerminal(last.state)) {
    printf("the last update ended: %s%s%s\n", update::describe(last.state),
           last.reason != update::Reason::kNone ? ": " : "", update::describe(last.reason));
  } else {
    printf("the last update did not end: the journal says %s; it is resumed at the next boot\n",
           update::describe(last.state));
  }
}

}  // namespace console
}  // namespace core

