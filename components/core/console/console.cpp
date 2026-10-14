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
    case Verb::kSelfStatus:
    case Verb::kSelfReceive:
    case Verb::kSelfSwitch:
    case Verb::kSelfConfirm:
    case Verb::kSelfReject:
    // The uplink (task p3-01): a modem is not a radio and has no slot.
    case Verb::kLte:
    case Verb::kLteUp:
    case Verb::kLteDown:
    case Verb::kLteTime:
    case Verb::kLteId:
    // The HTTPS client (task p3-02): no slot either.
    case Verb::kHttpGet:
    case Verb::kFetch:
    case Verb::kTrustTls:
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
    case Verb::kRakWatch:
    case Verb::kRakWatchResetCaps:
    case Verb::kRakLadder:
      return true;
  }
  return false;
}

}  // namespace

Console::Console(interfaces::TextOut& out, board::RadioPins& pins, health::Health& health,
                 interfaces::Clock& clock, interfaces::Restart& restart,
                 link::client::Client& radio, board::SlotLock& slots, image::Store* images,
                 image::Receiver* receiver, update::RadioUpdate* update, update::Journal* journal,
                 update::SelfUpdate* self, supervisor::Supervisor* supervisor,
                 uplink::UplinkPolicy* uplink, net::Client* http, update::Fetch* fetch,
                 net::TrustLoader* trust)
    : out_(out), pins_(pins), health_(health), clock_(clock), restart_(restart), radio_(radio),
      slots_(slots), images_(images), receiver_(receiver), update_(update), journal_(journal),
      self_(self), supervisor_(supervisor), uplink_(uplink), http_(http), fetch_(fetch),
      trust_(trust), flasher_(pins, clock, slots) {}

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
  // The Walter's own update (task p2-04): is this boot a trial of a new image, or the old image
  // back after one? tick() runs a trial from here, a step per turn of the main loop.
  if (self_ != nullptr) printSelfBoot(self_->begin());
  // The supervisor (task p2-05): its retained record, or the journal when a power cut cleared it.
  // Said out loud only when there is something to say - a unit found quiet should announce a stop
  // and a day that has already spent some of its caps, and keep silent otherwise.
  if (supervisor_ != nullptr) {
    supervisor_->begin();
    const supervisor::Status& w = supervisor_->status();
    if (w.stopped) {
      printf("supervisor: STOPPED: %s; %lu s of the quiet period left ('rak watch')\n",
             supervisor::describe(w.stop_reason), static_cast<unsigned long>(w.quiet_left_ms / 1000));
    } else if (w.counts.resets != 0 || w.counts.power_cycles != 0 || w.counts.flashes != 0) {
      printf("supervisor: today already: %lu resets, %lu power cycles, %lu flashes%s ('rak watch')\n",
             static_cast<unsigned long>(w.counts.resets),
             static_cast<unsigned long>(w.counts.power_cycles),
             static_cast<unsigned long>(w.counts.flashes),
             w.counts_from_journal ? ", read back from the journal" : "");
    }
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
  // The main loop is turning: the trial's first condition (core/update/self_update.h), met once
  // and for good. Then the trial itself, a step a turn, each change printed.
  if (self_ != nullptr) {
    if (!ticked_) {
      ticked_ = true;
      self_->mainLoopReached();
    }
    if (self_->onTrial()) {
      const update::SelfStep s = self_->step();
      printSelfStep(s);
      if (s.changed) prompt();
    }
  }
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
    case Verb::kRakWatch: rakWatch(); return;
    case Verb::kRakWatchResetCaps: rakWatchResetCaps(); return;
    case Verb::kRakLadder: rakLadder(); return;
    case Verb::kSelfStatus: selfStatus(); return;
    case Verb::kSelfReceive: selfReceive(); return;
    case Verb::kSelfSwitch: selfSwitch(p.command.radio_down); return;
    case Verb::kSelfConfirm: selfConfirm(); return;
    case Verb::kSelfReject: selfReject(); return;
    case Verb::kLte: lte(); return;
    case Verb::kLteUp: lteUp(); return;
    case Verb::kLteDown: lteDown(); return;
    case Verb::kLteTime: lteTime(); return;
    case Verb::kLteId: lteId(); return;
    case Verb::kHttpGet: httpGet(p.command.url); return;
    case Verb::kFetch: fetch(p.command.url, static_cast<image::Which>(p.command.which)); return;
    case Verb::kTrustTls: trustTls(); return;
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
  print("  self status         this Walter's two application slots, the trial if one is running,\n");
  print("                      and the last self-update's story from the journal\n");
  print("  self receive        take a release of this firmware over this port into the other slot\n");
  print("                      (tools/walterctl.py push --as self); the six checks run here\n");
  print("  self switch [radio-down]\n");
  print("                      make the received image the next to boot, tell the radio, restart;\n");
  print("                      it then has one boot to prove itself (docs/update.md)\n");
  print("  self confirm        end the running image's trial early: valid (the bench)\n");
  print("  self reject         end it early: rejected, restart into the old image (the bench)\n");
  print("  rak watch           the supervisor's view: healthy since when, the last incidents,\n");
  print("                      today's counts against the daily caps (docs/supervisor.md)\n");
  print("  rak watch reset-caps\n");
  print("                      empty the day's counts and lift a stop (the bench)\n");
  print("  rak ladder          climb the supervisor's ladder now - ask, reset, power-cycle, look at\n");
  print("                      the chip, stop - each rung printed as it is tried (minutes)\n");
  print("  lte                 the uplink: state, operator, band, signal, what the last attempt\n");
  print("                      did and when the next one is allowed (it only reads)\n");
  print("  lte up              bring the uplink up now (minutes: an attach from cold is slow)\n");
  print("  lte down            the modem's radio off\n");
  print("  lte time            the network's time, and whether it really is the network's\n");
  print("  lte id              the modem's IMEI and the SIM's ICCID and IMSI, for the label on\n");
  print("                      the box: printed here, and written nowhere else\n");
  print("  http get <url>      one GET over verified TLS (https only): status, headers, the first\n");
  print("                      bytes as hex, the size, the time. 'lte up' first\n");
  print("  fetch <base-url> [candidate|scanner]\n");
  print("                      a release from <base-url>/manifest.json into the store: the three\n");
  print("                      steps, each of the six checks printed as it passes (minutes)\n");
  print("  trust tls           the TLS roots this build trusts, their fingerprints and expiry, and\n");
  print("                      whether the modem's certificate slots hold them\n");
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
  const bool self = receiver_->target().use() == image::Use::kOta;
  printf("%s receive: %s%s%s\n", self ? "self" : "image", receiver_->reason(),
         receiver_->reasonMore()[0] != 0 ? ": " : "", receiver_->reasonMore());
  printf("  %lu chunks, %lu sent twice, %lu out of order, %lu frames dropped\n",
         static_cast<unsigned long>(receiver_->chunks()),
         static_cast<unsigned long>(receiver_->duplicates()),
         static_cast<unsigned long>(receiver_->outOfOrder()),
         static_cast<unsigned long>(receiver_->droppedFrames()));
  if (receiver_->stored()) {
    print(self ? "  'self status' shows it; 'self switch' makes it the next to boot and restarts\n"
               : "  'image list' shows it; 'image verify' reads it back\n");
  }
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

// ------------------------------------------------------------ the Walter's own update (p2-04)
//
// docs/update.md, "The Walter's half". `self receive` hands the port to the same transfer as
// `image receive`, with the other application slot as its target; `self switch` restarts the
// board and does not come back; the trial is run by tick() and printed as it goes.

bool Console::selfOk() {
  if (self_ != nullptr && receiver_ != nullptr) return true;
  print("error: there is no self-update in this build (the journal or the rakfw partition of\n"
        "  partitions.csv was not found at boot)\n");
  return false;
}

void Console::printSelfBoot(update::SelfBoot boot) {
  const update::SelfPayload& p = self_->bootEntry();
  switch (boot) {
    case update::SelfBoot::kNothing: return;
    case update::SelfBoot::kTrialBegun:
      printf("self-update: this image (%s, slot %u) is on trial: %lu s to reach the main loop, HELLO the radio\n",
             p.to[0] != 0 ? p.to : "?", static_cast<unsigned>(p.slot),
             static_cast<unsigned long>(update::kTrialWindowMs / 1000));
      print("  and reach the backend, or it is rolled back ('self status' watches, 'self confirm' / 'self reject' end it)\n");
      if ((p.flags & update::kFlagRadioKnownDown) != 0) print("  (the radio was known down before the switch: HELLO is not asked of it)\n");
      if ((p.flags & update::kFlagUnjournalled) != 0) print("  (no switch was journalled for it: nothing is known of the image but its slot)\n");
      return;
    case update::SelfBoot::kRollbackRecorded:
      printf("self-update: rolled back: %s\n", update::describe(p.reason));
      printf("  the image %s in slot %u is %s; this is %s again\n", p.to[0] != 0 ? p.to : "?",
             static_cast<unsigned>(p.slot),
             (p.flags & update::kFlagNotOnBadList) != 0 ? "not on the bad list (its hash is not known)" : "on the bad list",
             p.from[0] != 0 ? p.from : core::version());
      return;
    case update::SelfBoot::kNoSlots:
      print("self-update: the platform cannot say which application slot is running; no self-update\n");
      return;
    case update::SelfBoot::kJournalFailed:
      print("self-update: the journal could not be written; the trial runs without its entry\n");
      return;
  }
}

void Console::printSelfStep(const update::SelfStep& s) {
  if (!s.changed) {
    if (s.reason == update::SelfReason::kJournal || s.reason == update::SelfReason::kPlatform) {
      printf("self-update: %s\n", s.note);
    }
    return;
  }
  printf("self-update: on trial -> %s: %s\n", update::describe(s.after), s.note);
  if (s.after == update::SelfState::kRolledBack && s.reason != update::SelfReason::kPlatform) {
    print("  restarting into the previous image\n");
  }
}

void Console::printSlot(const char* label, const interfaces::OwnSlot& slot) {
  printf("%s slot %u at 0x%06lx (%lu KB): %s, version %s\n", label, static_cast<unsigned>(slot.index),
         static_cast<unsigned long>(slot.address), static_cast<unsigned long>(slot.size / 1024),
         interfaces::describe(slot.state), slot.version[0] != 0 ? slot.version : "(no image)");
}

void Console::printSelfEntry(std::size_t number, const update::Entry& entry, const update::SelfPayload& p) {
  char hash[2 * update::kHash16 + 1];
  hashHex(p.hash16, 8, hash);
  printf("  %2lu. seq %lu: %s", static_cast<unsigned long>(number), static_cast<unsigned long>(entry.seq),
         update::describe(p.state));
  switch (p.state) {
    case update::SelfState::kReceived:
      printf(" %s into slot %u (%lu bytes, sha256 %s...)", p.to, static_cast<unsigned>(p.slot),
             static_cast<unsigned long>(p.size), hash);
      break;
    case update::SelfState::kSwitching:
      printf(" to %s, from %s%s", p.to, p.from,
             (p.flags & update::kFlagRadioUp) != 0 ? " (the radio answered)"
             : (p.flags & update::kFlagRadioKnownDown) != 0 ? " (the radio known down)" : "");
      break;
    case update::SelfState::kOnTrial:
      printf(": %s in slot %u%s", p.to, static_cast<unsigned>(p.slot),
             (p.flags & update::kFlagUnjournalled) != 0 ? " (no switch was journalled)" : "");
      break;
    case update::SelfState::kConfirmed:
      printf(": %s%s%s", (p.flags & update::kFlagByHand) != 0 ? "on the console" : "all three conditions",
             (p.flags & update::kFlagRadioNotApplicable) != 0 ? "; the radio was known down" : "",
             (p.flags & update::kFlagUplinkNotApplicable) != 0 ? "; no server configured" : "");
      break;
    case update::SelfState::kRolledBack:
      printf(": %s%s", update::describe(p.reason),
             (p.flags & update::kFlagNotOnBadList) != 0 ? " (not on the bad list)" : "");
      break;
    case update::SelfState::kNone: break;
  }
  print("\n");
}

void Console::selfStatus() {
  if (!selfOk()) return;
  interfaces::OwnSlot running;
  interfaces::OwnSlot other;
  if (!self_->runningSlot(&running) || !self_->otherSlot(&other)) {
    print("self status: the platform cannot say which application slot is running\n");
    return;
  }
  printSlot("running:", running);
  printSlot("other:  ", other);
  const update::SelfStatus& st = self_->status();
  if (st.on_trial) {
    printf("on trial for %lu s of %lu: main loop %s; radio %s (%lu HELLO%s); uplink %s (%lu attempt%s)\n",
           static_cast<unsigned long>(st.trial_elapsed_ms / 1000),
           static_cast<unsigned long>(update::kTrialWindowMs / 1000), update::describe(st.main_loop),
           update::describe(st.radio), static_cast<unsigned long>(st.hello_tries), st.hello_tries == 1 ? "" : "s",
           update::describe(st.uplink), static_cast<unsigned long>(st.uplink_tries), st.uplink_tries == 1 ? "" : "s");
  } else {
    printf("the running image is %s%s\n",
           running.state == interfaces::OwnImageState::kValid ? "confirmed"
           : running.state == interfaces::OwnImageState::kUndefined ? "not on trial (flashed over USB, not by an update; rollback does not apply)"
                                                                   : interfaces::describe(running.state),
           self_->bootReason() != update::SelfReason::kNone && st.state == update::SelfState::kRolledBack
               ? "; the last self-update was rolled back"
               : "");
  }
  update::SelfPayload got;
  if (self_->received(&got)) {
    printf("received: %s in slot %u (%lu bytes), waiting for 'self switch'\n", got.to,
           static_cast<unsigned>(got.slot), static_cast<unsigned long>(got.size));
  }
  // The story: the last self-update's entries, oldest first, back to its *received* entry or
  // sixteen entries - the same shape as rak update status.
  constexpr std::size_t kStoryMax = 16;
  static update::Entry entries[kStoryMax];
  static update::SelfPayload payloads[kStoryMax];
  std::size_t count = 0;
  update::Cursor cursor;
  update::Entry e;
  bool more = journal_ != nullptr && journal_->first(&cursor, &e);
  while (more && count < kStoryMax) {
    update::SelfPayload p;
    if (update::decode(e, &p)) {
      entries[count] = e;
      payloads[count] = p;
      ++count;
      if (p.state == update::SelfState::kReceived) break;
    }
    more = journal_->previous(&cursor, &e);
  }
  if (count == 0) {
    print("self status: no self-update has been journalled on this unit\n");
    return;
  }
  printf("the last self-update's story (%lu entries):\n", static_cast<unsigned long>(count));
  for (std::size_t i = count; i > 0; --i) printSelfEntry(count - i + 1, entries[i - 1], payloads[i - 1]);
}

void Console::selfReceive() {
  if (!selfOk()) return;
  const update::SelfReason refused = self_->canReceive();
  if (refused != update::SelfReason::kNone) {
    printf("error: self receive: %s\n", update::describe(refused));
    return;
  }
  if (!receiver_->begin(*self_)) {
    printf("error: self receive: %s\n", receiver_->reason());
    return;
  }
  const image::SlotNeeds& needs = self_->needs();
  interfaces::OwnSlot other;
  (void)self_->otherSlot(&other);
  printf("self receive: this Walter's other application slot (%u); this port is now a framed binary transfer\n",
         static_cast<unsigned>(other.index));
  print("  (COBS and CRC-16, the box link's own framing). Send manifest.json, then its .sig,\n");
  print("  then the ota file: tools/walterctl.py push --as self\n");
  printf("  what will be accepted: role %s, board %s, band %s, an ota file that fits the slot,\n",
         needs.role, needs.board, needs.band);
  print("  and all six checks of FIRMWARE-MANIFEST section 5 run here - the laptop is not trusted\n");
}

void Console::selfSwitch(bool radio_down) {
  if (!selfOk()) return;
  update::SelfPayload got;
  const bool have = self_->received(&got);
  if (have) {
    printf("self switch: making %s (slot %u) the next to boot, from %s; the radio is told and the Walter restarts\n",
           got.to, static_cast<unsigned>(got.slot), core::version());
  }
  pins_.safe();
  const update::SelfReason refused = self_->switchNow(update::SwitchRequest{radio_down});
  if (refused != update::SelfReason::kNone) {
    printf("error: self switch: %s\n", update::describe(refused));
    return;
  }
  // On a board the restart does not return. On the host it does, and the bench rebuilds.
  print("self switch: restarting\n");
}

void Console::selfConfirm() {
  if (!selfOk()) return;
  const update::SelfReason refused = self_->confirmNow();
  if (refused != update::SelfReason::kNone) {
    printf("error: self confirm: %s\n", update::describe(refused));
    return;
  }
  print("self confirm: the running image is confirmed (valid); the trial is over\n");
}

void Console::selfReject() {
  if (!selfOk()) return;
  pins_.safe();
  const update::SelfReason refused = self_->rejectNow();
  if (refused == update::SelfReason::kPlatform) {
    print("self reject: rejected and journalled, but the platform would not roll back: the other slot holds nothing bootable\n");
    return;
  }
  if (refused != update::SelfReason::kNone) {
    printf("error: self reject: %s\n", update::describe(refused));
    return;
  }
  print("self reject: the running image is rejected; restarting into the previous image\n");
}

// ------------------------------------------------------------------ the supervisor (p2-05)
//
// docs/supervisor.md. `rak watch` only reads - it never steps the machine, so looking at the
// supervisor can never cause a reset - and `rak ladder` is the one command that climbs, printing
// each rung as it is tried. When the supervisor runs by itself is phase 3's (the wake cycle);
// nothing here drives it from the main loop.

bool Console::watchOk() {
  if (supervisor_ != nullptr) return true;
  print("error: there is no supervisor in this build (main/ constructs one on a board; a console\n"
        "  built without one - the host's walter_console - answers in one line instead)\n");
  return false;
}

void Console::printRungsTried(std::uint8_t rungs) {
  if (rungs == 0) {
    print("none");
    return;
  }
  bool first = true;
  for (std::size_t i = 1; i <= supervisor::kRungCount; ++i) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (i - 1));
    if ((rungs & bit) == 0) continue;
    printf("%s%lu", first ? "" : ", ", static_cast<unsigned long>(i));
    first = false;
  }
}

void Console::printIncident(std::size_t number, const supervisor::Incident& in) {
  const std::uint64_t now = clock_.nowMs();
  const unsigned long ago_s =
      now >= in.began_ms ? static_cast<unsigned long>((now - in.began_ms) / 1000u) : 0;
  printf("  %lu. %lu s ago", static_cast<unsigned long>(number), ago_s);
  if (in.began_epoch != 0) printf(" (epoch %lu)", static_cast<unsigned long>(in.began_epoch));
  printf(": %s; %s", supervisor::describe(in.cause), supervisor::describe(in.outcome));
  if (in.outcome == supervisor::Outcome::kCured && in.cured_at != supervisor::Rung::kNone) {
    printf(" by %s", supervisor::describe(in.cured_at));
  }
  if (in.stop != supervisor::StopReason::kNone) printf(": %s", supervisor::describe(in.stop));
  print("\n");
  printf("     lasted %lu s; rungs tried: ", static_cast<unsigned long>(in.lasted_ms / 1000u));
  printRungsTried(in.rungs_tried);
  printf("; battery %lu mV; the radio's last reset reason %u (%s)\n",
         static_cast<unsigned long>(in.battery_mv), static_cast<unsigned>(in.reset_reason),
         lc::resetReasonName(in.reset_reason));
  if (in.restarts != 0 || in.cause == supervisor::Cause::kBootLoop) {
    printf("     %u restarts of the radio, its last uptime %lu s\n",
           static_cast<unsigned>(in.restarts), static_cast<unsigned long>(in.last_uptime_s));
  }
  for (std::size_t i = 0; i < supervisor::kRungCount; ++i) {
    const supervisor::RungOutcome o = in.rung[i];
    if (o == supervisor::RungOutcome::kNotTried) continue;
    printf("     %s: %s\n", supervisor::describe(static_cast<supervisor::Rung>(i + 1)),
           supervisor::describe(o));
  }
}

void Console::rakWatch() {
  if (!watchOk()) return;
  const supervisor::Status& w = supervisor_->status();
  const std::uint64_t now = clock_.nowMs();
  printf("rak watch: slot %u: the radio is %s; the supervisor is %s\n",
         static_cast<unsigned>(slot_.index), supervisor::describe(w.health),
         supervisor::describe(w.state));
  if (w.ever_answered) {
    printf("  healthy since %lu s ago; last answer %lu s ago; %lu pings; its uptime %lu s; boot_id 0x%08lx\n",
           static_cast<unsigned long>(now >= w.healthy_since_ms ? (now - w.healthy_since_ms) / 1000u : 0),
           static_cast<unsigned long>(now >= w.last_answer_ms ? (now - w.last_answer_ms) / 1000u : 0),
           static_cast<unsigned long>(w.pings), static_cast<unsigned long>(w.uptime_s),
           static_cast<unsigned long>(w.boot_id));
    printf("  its last reset reason %u (%s); the clock %s\n", static_cast<unsigned>(w.reset_reason),
           lc::resetReasonName(w.reset_reason),
           w.time_set ? "was set"
           : w.time_skipped ? "could not be set: this Walter has no time to give yet"
                            : "has not been set since the last restart");
  } else {
    print("  the radio has not answered this supervisor yet ('rak ladder' asks now)\n");
  }
  printf("today: resets %lu/%lu, power cycles %lu/%lu, golden-image flashes %lu/%lu\n",
         static_cast<unsigned long>(w.counts.resets), static_cast<unsigned long>(w.caps.resets),
         static_cast<unsigned long>(w.counts.power_cycles),
         static_cast<unsigned long>(w.caps.power_cycles),
         static_cast<unsigned long>(w.counts.flashes), static_cast<unsigned long>(w.caps.flashes));
  if (w.counts.resets != 0 || w.counts.power_cycles != 0 || w.counts.flashes != 0) {
    printf("  the day began %lu s ago of %lu%s%s\n",
           static_cast<unsigned long>(now >= w.window_began_ms ? (now - w.window_began_ms) / 1000u : 0),
           static_cast<unsigned long>(supervisor::kCapWindowS),
           w.window_began_epoch != 0 ? " (dated: a whole day can be shown to have passed)"
                                     : " (no time of day: the monotonic clock is all there is)",
           w.window_reanchored ? "; re-anchored after a restart, the counts kept" : "");
  }
  if (w.counts_from_journal) {
    print("  the counts were read back from the journal: a power cut cleared the retained record\n");
  }
  if (w.battery_known) {
    printf("battery: %lu mV, read %lu s ago (a power cycle needs %lu mV, a flash %lu mV)\n",
           static_cast<unsigned long>(w.battery_mv),
           static_cast<unsigned long>(w.battery_age_ms / 1000u),
           static_cast<unsigned long>(supervisor::kPowerCycleMinMv),
           static_cast<unsigned long>(supervisor::kFlashMinMv));
  } else {
    print("battery: never read (POWER_GET has not answered)\n");
  }
  printf("the radio restarted %lu times in the last %lu s (%lu in all); a boot loop is %lu of them\n",
         static_cast<unsigned long>(w.restarts),
         static_cast<unsigned long>(supervisor::kBootLoopWindowMs / 1000u),
         static_cast<unsigned long>(w.restarts_total),
         static_cast<unsigned long>(supervisor::kBootLoopBoots));
  if (w.boot_loop) {
    printf("  this is a boot loop: the ladder would start at %s\n",
           supervisor::describe(supervisor::Rung::kPowerCycle));
  }
  if (w.stopped) {
    printf("STOPPED: %s\n", supervisor::describe(w.stop_reason));
    print("  rungs tried: ");
    printRungsTried(w.stopped_after);
    printf("; nothing more for %lu s, or until 'rak watch reset-caps', 'rak ladder' or the server\n",
           static_cast<unsigned long>(w.quiet_left_ms / 1000u));
  }
  if (w.attentions != 0) {
    printf("attention from the radio: %lu kept\n", static_cast<unsigned long>(w.attentions));
    for (std::size_t i = 0; i < supervisor_->attentionCount(); ++i) {
      const supervisor::Attention& a = supervisor_->attentionAt(i);
      printf("  %u (%s), detail %u, repeat %u, %lu s ago%s\n", static_cast<unsigned>(a.reason),
             lc::attentionName(a.reason), static_cast<unsigned>(a.detail),
             static_cast<unsigned>(a.repeat),
             static_cast<unsigned long>(now >= a.at_ms ? (now - a.at_ms) / 1000u : 0),
             a.acknowledged ? ", acknowledged by a request of ours" : "");
    }
  }
  if (supervisor_->incidentCount() == 0) {
    print("no incident has been recorded on this unit\n");
    return;
  }
  printf("the last %lu incidents, newest first:\n",
         static_cast<unsigned long>(supervisor_->incidentCount()));
  for (std::size_t i = 0; i < supervisor_->incidentCount(); ++i) {
    printIncident(i + 1, supervisor_->incidentAt(i));
  }
}

void Console::rakWatchResetCaps() {
  if (!watchOk()) return;
  const bool was_stopped = supervisor_->status().stopped;
  supervisor_->resetCaps();
  printf("rak watch reset-caps: the day's counts are empty again%s; journalled\n",
         was_stopped ? " and the stop is lifted" : "");
}

void Console::printLadderStep(const supervisor::Step& s) {
  if (s.rung != supervisor::Rung::kNone && s.rung_outcome != supervisor::RungOutcome::kNotTried) {
    printf("supervisor: %s: %s%s%s\n", supervisor::describe(s.rung),
           supervisor::describe(s.rung_outcome), s.note[0] != 0 ? " - " : "", s.note);
    return;
  }
  if (s.changed) {
    printf("supervisor: %s -> %s%s%s\n", supervisor::describe(s.before),
           supervisor::describe(s.after), s.note[0] != 0 ? ": " : "", s.note);
  }
}

void Console::driveLadder() {
  for (;;) {
    const supervisor::Step s = supervisor_->step();
    printLadderStep(s);
    if (!supervisor_->climbing()) break;
    if (s.waited) clock_.delayMs(supervisor::kStepIdleMs);
  }
}

void Console::rakLadder() {
  if (!watchOk()) return;
  if (!supervisor_->runLadder()) {
    printf("error: rak ladder: the supervisor is already busy (%s)\n",
           supervisor::describe(supervisor_->state()));
    return;
  }
  printf("rak ladder: slot %u: asking the radio, then climbing if it is silent; each rung is\n"
         "  printed as it is tried (this takes minutes: the waits are %lu, %lu and %lu s)\n",
         static_cast<unsigned>(slot_.index),
         static_cast<unsigned long>(supervisor::kAskAgainPauseMs / 1000u),
         static_cast<unsigned long>(supervisor::kAfterResetMs / 1000u),
         static_cast<unsigned long>(supervisor::kAfterPowerCycleMs / 1000u));
  PrintProgress progress(out_);
  if (update_ != nullptr) update_->setProgress(&progress);
  driveLadder();
  if (update_ != nullptr) update_->setProgress(nullptr);
  const supervisor::Status& w = supervisor_->status();
  if (w.stopped) {
    printf("rak ladder: STOPPED: %s\n", supervisor::describe(w.stop_reason));
    print("  rungs tried: ");
    printRungsTried(w.stopped_after);
    print("; 'rak watch' has the incident\n");
    return;
  }
  if (w.health == supervisor::Health::kScanner) {
    print("rak ladder: the radio is running the scanner (NoiseScope): not a fault, left alone ('rak text')\n");
    return;
  }
  if (supervisor_->incidentCount() != 0 &&
      supervisor_->incidentAt(0).outcome == supervisor::Outcome::kCured) {
    printf("rak ladder: the radio is back after %s ('rak watch' has the incident)\n",
           supervisor::describe(supervisor_->incidentAt(0).cured_at));
    return;
  }
  printf("rak ladder: the radio is %s: nothing to climb\n", supervisor::describe(w.health));
}

// -- the uplink (task p3-01) --------------------------------------------------------------------

bool Console::lteOk() {
  if (uplink_ != nullptr) return true;
  print("error: this build has no uplink: nothing was given to the console (main/main.cpp)\n");
  return false;
}

void Console::printSignal(const interfaces::Signal& s) {
  print("  signal:");
  if (s.rsrp_known) printf(" RSRP %d dBm,", static_cast<int>(s.rsrp_dbm));
  if (s.rsrq_known) {
    printf(" RSRQ %d.%d dB,", static_cast<int>(s.rsrq_tenth_db / 10),
           static_cast<int>(s.rsrq_tenth_db < 0 ? -(s.rsrq_tenth_db % 10) : s.rsrq_tenth_db % 10));
  }
  if (s.rssi_known) printf(" RSSI %d dBm,", static_cast<int>(s.rssi_dbm));
  if (!s.rsrp_known && !s.rsrq_known && !s.rssi_known) print(" nothing the modem would say,");
  printf(" operator %s,", s.operator_name[0] != 0 ? s.operator_name : "unknown");
  if (s.band != 0) {
    printf(" band %u,", static_cast<unsigned>(s.band));
  } else {
    print(" band unknown,");
  }
  printf(" %s\n", interfaces::describe(s.rat));
}

void Console::printPowerSaving(const interfaces::PowerSavingGrant& g) {
  printf("  power saving: asked for %s", interfaces::describe(g.requested));
  if (!g.asked) {
    print("; the modem has not been asked yet\n");
    return;
  }
  if (!g.granted) {
    print("; the network has granted nothing (it reports PSM timers when it feels like it)\n");
    return;
  }
  printf("; granted: sleep up to %lu s, reachable %lu s after a transmission\n",
         static_cast<unsigned long>(uplink::tauSeconds(g.tau_octet)),
         static_cast<unsigned long>(uplink::activeTimeSeconds(g.active_octet)));
}

void Console::printNextTry() {
  const uplink::Status& u = uplink_->status();
  if (u.retry == uplink::Retry::kNever) {
    print("  try again: never automatically - a person has to act. 'lte up' still tries\n");
    return;
  }
  printf("  try again: %s\n", uplink::describe(u.retry));
  printf("    %lu failures in a row; every %lu wake(s); %lu wake(s) until the next is allowed\n",
         static_cast<unsigned long>(u.failures), static_cast<unsigned long>(u.interval_wakes),
         static_cast<unsigned long>(u.wakes_until_next_try));
}

void Console::lte() {
  if (!lteOk()) return;
  const uplink::Status& u = uplink_->status();
  printf("lte: %s%s\n", interfaces::describe(u.state),
         u.ever_up ? "" : "; it has not been up in this boot");
  if (u.tried_this_boot) {
    printf("  last attempt: %s - %s\n", uplink::describe(u.last_failure),
           uplink::means(u.last_failure));
    printf("    reached '%s' in %lu ms%s%s%s\n", uplink::describe(u.reached),
           static_cast<unsigned long>(u.last_spent_ms),
           u.last_abandoned ? "; abandoned: the wake's budget ran out" : "",
           u.last_overran ? "; a modem call came back after its deadline" : "",
           u.modem_reset_this_wake ? "; the modem was reset" : "");
  } else {
    print("  last attempt: none in this boot\n");
  }
  printNextTry();
  if (!u.pin_set) {
    print("  SIM PIN: none is set, so a card that wants one is 'sim-locked' and left alone\n");
  } else if (u.pin_refused) {
    print("  SIM PIN: set, and refused once: it is never sent again until the setting changes\n");
  } else {
    print("  SIM PIN: set, never refused\n");
  }
  if (u.signal_known) {
    printSignal(u.signal);
  } else {
    print("  signal: not read in this boot\n");
  }
  if (u.time.origin == interfaces::TimeOrigin::kNetwork) {
    printf("  time: %lu unix seconds, from the network\n",
           static_cast<unsigned long>(u.time.epoch));
  } else {
    printf("  time: %s\n", interfaces::describe(u.time.origin));
  }
  printPowerSaving(u.power_saving);
  printf("  radio access technology %s; %lu wakes counted, %lu attempts made\n",
         interfaces::describe(u.rat), static_cast<unsigned long>(u.wakes),
         static_cast<unsigned long>(u.tries));
  if (u.record_lost) {
    print("  the backoff's record was gone at this boot (a first boot, or a power cut: RTC\n");
    print("  memory does not survive one), so the count began again from zero\n");
  }
}

void Console::lteUp() {
  if (!lteOk()) return;
  // The wake is begun *first*, and the backoff asked afterwards: "would the backoff have tried
  // in this wake" is only a true sentence about a wake that has been counted (core/uplink/
  // policy.h, beginWake). A person asking goes ahead either way.
  uplink_->beginWake();
  const bool allowed = uplink_->mayTry();
  const std::uint32_t wait = uplink_->wakesUntilNextTry();
  printf("lte up: bringing the uplink up; the budget is %lu s and an attach from cold can take\n",
         static_cast<unsigned long>(uplink::kWakeBudgetMs / 1000u));
  printf("  most of it. The backoff would %s\n",
         allowed ? "have allowed this wake"
                 : (uplink_->status().retry == uplink::Retry::kNever
                        ? "never have tried by itself; a person asking is the way past that"
                        : "have waited"));
  if (!allowed && wait != 0) {
    printf("  (%lu more wake(s)); a person asking goes ahead anyway\n",
           static_cast<unsigned long>(wait));
  }
  const uplink::Result r = uplink_->bringUp();
  if (r.up) {
    printf("lte up: up in %lu ms (%s)\n", static_cast<unsigned long>(r.spent_ms),
           interfaces::describe(uplink_->state()));
    interfaces::Signal s;
    if (uplink_->signal(&s)) printSignal(s);
    if (r.time.origin == interfaces::TimeOrigin::kNetwork) {
      printf("  time: %lu unix seconds, from the network\n",
             static_cast<unsigned long>(r.time.epoch));
    } else {
      printf("  time: %s - the uplink is up all the same\n",
             interfaces::describe(r.time.origin));
    }
    printPowerSaving(uplink_->status().power_saving);
    return;
  }
  printf("lte up: %s - %s\n", uplink::describe(r.failure), uplink::means(r.failure));
  printf("  reached '%s' in %lu ms; the modem is down again%s%s%s\n", uplink::describe(r.reached),
         static_cast<unsigned long>(r.spent_ms),
         r.modem_reset ? "; the modem was reset (the one reset a wake is allowed)" : "",
         r.abandoned ? "; abandoned: the wake's budget ran out" : "",
         r.overran ? "; a modem call came back after its deadline" : "");
  printNextTry();
}

void Console::lteDown() {
  if (!lteOk()) return;
  uplink_->takeDown();
  printf("lte down: the modem's radio is off (%s)\n", interfaces::describe(uplink_->state()));
}

void Console::lteTime() {
  if (!lteOk()) return;
  // One command's worth of budget, so that a `lte time` at a console cannot sit on the modem.
  uplink_->openBudget(uplink::kTimeDeadlineMs + 1000);
  interfaces::NetworkTime when;
  if (!uplink_->networkTime(&when)) {
    if (when.origin == interfaces::TimeOrigin::kDefault) {
      print("error: lte time: the modem answered with its own default, not a time the network\n");
      print("  gave it. This firmware refuses it: a wrong clock is worse than none\n");
      print("  (spec/LINK-PROTOCOL.md section 5.4)\n");
      return;
    }
    printf("error: lte time: no answer (%s)\n", interfaces::describe(uplink_->state()));
    return;
  }
  printf("lte time: %lu unix seconds, from the network\n", static_cast<unsigned long>(when.epoch));
}

void Console::lteId() {
  if (!lteOk()) return;
  uplink_->openBudget(uplink::kIdentityDeadlineMs + 1000);
  interfaces::Identity id;
  if (!uplink_->identity(&id)) {
    printf("error: lte id: the modem would not say (%s; the card's numbers need the modem\n",
           interfaces::describe(uplink_->state()));
    print("  awake - 'lte up' first, or at least a modem that answers)\n");
    return;
  }
  // The one place this firmware prints these. They go nowhere else - not the log, not the
  // journal, not anything it sends by itself (core/uplink/policy.h, *Privacy*).
  printf("lte id: IMEI %s\n", id.imei[0] != 0 ? id.imei : "unknown");
  printf("        ICCID %s\n", id.iccid[0] != 0 ? id.iccid : "unknown");
  printf("        IMSI %s\n", id.imsi[0] != 0 ? id.imsi : "unknown");
  print("  these identify a subscription: they are printed because a person asked, and are\n");
  print("  written to no log and sent nowhere by this firmware\n");
}

// -- the HTTPS client (task p3-02) --------------------------------------------------------------

bool Console::httpOk() {
  if (http_ != nullptr) return true;
  print("error: this build has no HTTPS client: nothing was given to the console (main/main.cpp)\n");
  return false;
}

bool Console::uplinkUp() {
  // A rig without an uplink (the host tests') is not held to it; a build with one is.
  if (uplink_ == nullptr) return true;
  if (uplink_->state() == interfaces::UplinkState::kUp) return true;
  printf("error: the uplink is %s: 'lte up' first\n", interfaces::describe(uplink_->state()));
  return false;
}

namespace {

// The body of `http get`: the first bytes kept for a hex line, the rest counted. The client's
// bound is what cuts a long body; this keeps 32 bytes whatever comes.
class PeekSink final : public interfaces::HttpBodySink {
 public:
  bool piece(const std::uint8_t* data, std::size_t size) override {
    for (std::size_t i = 0; i < size && kept_ < sizeof first_; ++i) first_[kept_++] = data[i];
    total_ += size;
    return true;
  }
  const std::uint8_t* first() const { return first_; }
  std::size_t kept() const { return kept_; }
  std::uint64_t total() const { return total_; }

 private:
  std::uint8_t first_[32] = {};
  std::size_t kept_ = 0;
  std::uint64_t total_ = 0;
};

// How much of a response `http get` reads before cutting it: a console line is for looking, not
// for downloading; a kilobyte shows the shape of an answer.
constexpr std::uint64_t kHttpGetBound = 1024;

}  // namespace

void Console::httpGet(const char* url) {
  if (!httpOk() || !uplinkUp()) return;
  // The headers worth naming on a bench: the type and the length, the two validators the
  // download uses, and whether the server takes ranges.
  char content_type[64] = {};
  char etag[64] = {};
  char last_modified[48] = {};
  char accept_ranges[16] = {};
  interfaces::HttpWantedHeader wanted[4] = {};
  wanted[0].name = "Content-Type";
  wanted[0].value = content_type;
  wanted[0].capacity = sizeof content_type;
  wanted[1].name = "ETag";
  wanted[1].value = etag;
  wanted[1].capacity = sizeof etag;
  wanted[2].name = "Last-Modified";
  wanted[2].value = last_modified;
  wanted[2].capacity = sizeof last_modified;
  wanted[3].name = "Accept-Ranges";
  wanted[3].value = accept_ranges;
  wanted[3].capacity = sizeof accept_ranges;
  net::Options options;
  options.wanted = wanted;
  options.wanted_count = 4;
  options.bound = kHttpGetBound;
  PeekSink body;
  printf("http get: %s (the chain is verified against this build's roots)\n", url);
  http_->setTrace(&out_);
  const net::Result r = http_->get(url, options, body);
  http_->setTrace(nullptr);
  if (r.verdict == net::Verdict::kRefusedUrl) {
    printf("error: http get: %s\n", r.why);
    return;
  }
  if (r.status == 0) {
    printf("error: http get: no response: %s\n", r.why);
    return;
  }
  printf("http get: %u %s%s%s\n", static_cast<unsigned>(r.status), net::describe(r.verdict),
         r.redirects != 0 ? " after a redirect to " : "", r.redirects != 0 ? r.final_host : "");
  if (r.verdict != net::Verdict::kOk && r.verdict != net::Verdict::kTooLarge) printf("  %s\n", r.why);
  for (std::size_t i = 0; i < 4; ++i) {
    if (wanted[i].present) {
      printf("  %s: %s%s\n", wanted[i].name, wanted[i].value, wanted[i].truncated ? " (cut)" : "");
    }
  }
  if (r.has_content_length) {
    printf("  Content-Length: %lu\n", static_cast<unsigned long>(r.content_length));
  } else if (r.chunked) {
    print("  Transfer-Encoding: chunked\n");
  }
  if (body.kept() != 0) {
    char hex[2 * 32 + 1];
    hashHex(body.first(), body.kept(), hex);
    printf("  first %lu bytes: %s\n", static_cast<unsigned long>(body.kept()), hex);
  }
  printf("  %lu bytes%s in %lu ms\n", static_cast<unsigned long>(body.total()),
         r.verdict == net::Verdict::kTooLarge ? " (cut off: a console line reads 1 KB at most)" : "",
         static_cast<unsigned long>(r.elapsed_ms));
  if (r.authorization_dropped) print("  (an Authorization header was not carried past the first host)\n");
}

void Console::fetch(const char* base_url, image::Which which) {
  if (!httpOk() || !uplinkUp()) return;
  if (fetch_ == nullptr) {
    print("error: fetch: this build has no image store, so nothing can be fetched into it\n");
    return;
  }
  printf("fetch: %s into the %s image of slot %u: manifest.json, its .sig, the six checks, the image\n",
         base_url, image::name(which), static_cast<unsigned>(slot_.index));
  http_->setTrace(&out_);
  const update::FetchResult r = fetch_->run(base_url, which, slot_, net::Budget{});
  http_->setTrace(nullptr);
  switch (r.verdict) {
    case update::FetchVerdict::kStored:
      printf("fetch: stored: %s (%s, %lu bytes) is the %s image; 'image list' shows it\n", r.version,
             r.file_name, static_cast<unsigned long>(r.expected), image::name(which));
      return;
    case update::FetchVerdict::kRefused:
      printf("fetch: refused at %s: %s; nothing stored, the slot is empty\n",
             image::describe(r.check), r.why);
      return;
    case update::FetchVerdict::kBudget:
      printf("fetch: stopped on the budget with %lu of %lu bytes in flash: %s\n",
             static_cast<unsigned long>(r.held), static_cast<unsigned long>(r.expected), r.why);
      return;
    case update::FetchVerdict::kLater:
      printf("fetch: later: %s (Retry-After %lu s, capped); %lu of %lu bytes in flash\n", r.why,
             static_cast<unsigned long>(r.retry_after_s), static_cast<unsigned long>(r.held),
             static_cast<unsigned long>(r.expected));
      return;
    case update::FetchVerdict::kDenied:
    case update::FetchVerdict::kFailed:
    case update::FetchVerdict::kBadUrl:
    case update::FetchVerdict::kNoSlot:
    case update::FetchVerdict::kStoreRefused:
    case update::FetchVerdict::kWrongFile:
      printf("error: fetch: %s: %s%s\n", update::describe(r.verdict), r.why,
             r.held != 0 ? " (the bytes in flash stay for a resume)" : "");
      return;
  }
}

void Console::trustTls() {
  if (trust_ == nullptr) {
    // The roots are in every build; the loader (and its record) is what a rig may lack.
    print("trust tls: the roots this build carries (no loader in this build: the modem's slots are not tracked)\n");
  } else {
    printf("trust tls: %lu roots; the modem's certificate slots %s\n",
           static_cast<unsigned long>(net::tlsRootCount()),
           trust_->recordLost() && trust_->writesThisBoot() == 0
               ? "have no record (a first boot or a power cut): they are written at the first request"
               : "are tracked in retained storage");
  }
  for (std::uint8_t i = 0; i < net::tlsRootCount(); ++i) {
    const net::TlsRoot* r = net::tlsRoot(i);
    printf("  %u. %s\n", static_cast<unsigned>(i), r->name);
    printf("     subject %s\n", r->subject);
    printf("     sha256 %s\n", r->sha256_hex);
    printf("     expires %s; checked %s from %s\n", r->expires, r->checked, r->source);
    printf("     certificate slot %u, TLS profile %u: ", static_cast<unsigned>(net::certificateSlotOf(i)),
           static_cast<unsigned>(net::tlsProfileOf(i)));
    if (trust_ == nullptr) {
      print("not tracked\n");
    } else {
      const net::RootStatus st = trust_->statusOf(i);
      print(st.matches ? "written, matches this build's root\n"
                       : (st.recorded ? "written with another root or numbering: rewritten at the next request\n"
                                      : "not written yet: written at the next request\n"));
    }
  }
  std::size_t rows = 0;
  const net::HostRoot* hosts = net::hostRoots(&rows);
  print("  hosts: ");
  for (std::size_t i = 0; i < rows; ++i) {
    printf("%s -> root %u; ", hosts[i].suffix, static_cast<unsigned>(hosts[i].root));
  }
  printf("every other host -> root %u\n", static_cast<unsigned>(net::kRootIsrgX1));
  if (trust_ != nullptr) {
    printf("  %lu root(s) written in this boot\n", static_cast<unsigned long>(trust_->writesThisBoot()));
  }
}

}  // namespace console
}  // namespace core
