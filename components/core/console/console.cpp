#include "core/console/console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/image/crypto/ed25519.h"
#include "core/image/this_image.h"
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
    // The agent's uplink (task p4-03) and its identity (task p4-04): a server and this unit's
    // own name for itself, not a radio - `agent id` does ask the radio who it is, but a unit has
    // one (spec/SYSTEM.md S-11) and the line takes no slot.
    case Verb::kAgentUrl:
    case Verb::kAgentProbe:
    case Verb::kAgentId:
    case Verb::kAgentHeartbeat:
    case Verb::kAgentObs:
    case Verb::kAgentObsPull:
    case Verb::kAgentWake:
    case Verb::kAgentJobs:
    case Verb::kAgentState:
    // The settings and the rest of the console (task p3-03): about the unit, not a radio.
    case Verb::kGet:
    case Verb::kGetAll:
    case Verb::kSet:
    case Verb::kErase:
    case Verb::kDump:
    case Verb::kJournal:
    // Sleep and the battery (task p3-04): about the whole unit. `sleep now` does talk to the
    // radio - SLEEP_NOTICE is its last request (§8) - but it is not *about* a radio, and the
    // link client takes the hold around that exchange as it does around every other.
    case Verb::kWake:
    case Verb::kPower:
    case Verb::kSleepNow:
    case Verb::kSleepFor:
    case Verb::kStay:
    // One whole wake (task p3-05): the whole unit's, not a radio's.
    case Verb::kCycleNow:
    case Verb::kCycleDry:
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
    case Verb::kRakRequest:
    case Verb::kRakEvents:
    case Verb::kRakText:
    case Verb::kRakSwdConnect:
    case Verb::kRakSwdId:
    case Verb::kRakSwdMem:
    case Verb::kRakFlashTest:
    case Verb::kRakProvisionCheck:
    case Verb::kRakProvision:
    case Verb::kRakUpdate:
    case Verb::kRakUpdateFirstInstall:
    case Verb::kRakUpdateStatus:
    case Verb::kRakRoleScanner:
    case Verb::kRakRoleRepeater:
    case Verb::kRakWatch:
    case Verb::kRakWatchResetCaps:
    case Verb::kRakLadder:
    case Verb::kRakMeshStatus:
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
                 net::TrustLoader* trust, settings::Settings* settings,
                 update::UplinkCheck* server, cycle::DutyCycle* duty, cycle::WakeCycle* cycle,
                 update::BatterySource* battery, MeshProbe* mesh, UplinkProbe* agent,
                 AgentProbe* identity, AgentLoopProbe* loop, const net::SocketRotation* sockets,
                 cycle::NetworkClock* wall)
    : out_(out), pins_(pins), health_(health), clock_(clock), restart_(restart), radio_(radio),
      slots_(slots), images_(images), receiver_(receiver), update_(update), journal_(journal),
      self_(self), supervisor_(supervisor), uplink_(uplink), http_(http), fetch_(fetch),
      trust_(trust), settings_(settings), server_(server), duty_(duty), cycle_(cycle),
      mesh_(mesh), agent_(agent), identity_(identity), loop_(loop), sockets_(sockets), wall_(wall),
      flasher_(pins, clock, slots),
      provision_(pins, slots, clock, radio, images, battery, update, supervisor) {}

bool Console::slotOk(board::Slot slot) {
  if (slots_.exists(slot)) return true;
  printf("error: slot %u: this adapter has %u radio, so slot 0 is the only one\n",
         static_cast<unsigned>(slot.index), static_cast<unsigned>(slots_.slots()));
  return false;
}

void Console::print(const char* text) {
  const std::size_t n = std::strlen(text);
  out_.write(text, n);
  log_.append(text, n);
}

// Into a stack buffer: no heap. A line that does not fit is cut, never overrun.
void Console::printf(const char* format, ...) {
  char buf[200];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  if (n <= 0) return;
  const std::size_t length =
      static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n) : sizeof buf - 1;
  out_.write(buf, length);
  log_.append(buf, length);
}

// Is a line nobody asked for worth printing. `log.level` and below; `info` without a settings
// store, which is what every build before task p3-03 did.
bool Console::says(settings::Level level) const {
  const settings::Level in_force =
      settings_ != nullptr ? settings_->level() : settings::Level::kInfo;
  return static_cast<std::uint8_t>(level) <= static_cast<std::uint8_t>(in_force);
}

void Console::prompt() { print("> "); }

// The banner itself is main's (the bench reads it first, before the slot and the descriptor);
// this is the settings, the boot's reason and the first prompt. Everything here is a line nobody
// asked for, so `log.level error` leaves only what has to be said - a supervisor's stop, the
// invitation and the prompt.
void Console::begin() {
  // The settings first: `log.level` decides what the rest of this function prints, and
  // `swd.khz`, the APN, the PIN and the server have to be in force before any command can run.
  applySettings();
  const health::BootRecord& boot = health_.thisBoot();
  if (says(settings::Level::kInfo)) {
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
  }
  // The settings, in one line (task p3-03). A store that would not open and a stored value this
  // firmware will not have are said at `warn`: both mean the unit is running on fallbacks and
  // a person reading a console has to know it before they trust what `get` says.
  if (settings_ != nullptr) {
    if (!settings_->storeReady()) {
      if (says(settings::Level::kWarn)) {
        print("settings: the store would not open: every key falls back and a 'set' cannot be "
              "kept (see the boot lines above)\n");
      }
    } else {
      if (settings_->rejected() != 0 && says(settings::Level::kWarn)) {
        printf("settings: %lu stored value(s) this firmware will not have were ignored (the "
               "first is %s); those keys fall back\n",
               static_cast<unsigned long>(settings_->rejected()), settings_->firstRejected());
      }
      if (says(settings::Level::kInfo)) {
        printf("settings: %lu of %lu keys set; log.level %s, swd %ld kHz%s%s ('get')\n",
               static_cast<unsigned long>(settings_->setCount()),
               static_cast<unsigned long>(settings::kKeyCount),
               settings::name(settings_->level()), static_cast<long>(settings_->swdKhz()),
               settings_->unitName()[0] != 0 ? "; " : "", settings_->unitName());
      }
    }
  }
  // An update the journal says was in progress is taken up where it was (spec/SYSTEM.md §6:
  // "a Walter that resets in the middle resumes or rolls back; it never guesses"). tick()
  // drives it from here, one step per turn of the main loop, printing each transition.
  if (update_ != nullptr && update_->resume() && says(settings::Level::kWarn)) {
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
    // Unless main/ has already done it: the duty cycle's wake reason asks the supervisor to
    // listen for §8's attention before anything else happens in a wake (task p3-04), and a
    // supervisor that has not read its retained record cannot record one properly.
    if (!supervisor_->begun()) supervisor_->begin();
    const supervisor::Status& w = supervisor_->status();
    if (w.stopped) {
      // Said at every level, `error` included: a unit that has given up on its radio is the one
      // thing a person at a console has to be told whatever else they asked to be spared.
      printf("supervisor: STOPPED: %s; %lu s of the quiet period left ('rak watch')\n",
             supervisor::describe(w.stop_reason), static_cast<unsigned long>(w.quiet_left_ms / 1000));
    } else if (says(settings::Level::kInfo) &&
               (w.counts.resets != 0 || w.counts.power_cycles != 0 || w.counts.flashes != 0)) {
      printf("supervisor: today already: %lu resets, %lu power cycles, %lu flashes%s ('rak watch')\n",
             static_cast<unsigned long>(w.counts.resets),
             static_cast<unsigned long>(w.counts.power_cycles),
             static_cast<unsigned long>(w.counts.flashes),
             w.counts_from_journal ? ", read back from the journal" : "");
    }
  }
  // The duty cycle (task p3-04): what this wake means, worked out before anything is done about
  // it, and - because this board cannot wake on a cable - whether a person is here. Said at
  // `info`, except a line that was disarmed and a fault, which are at `warn`: both are a unit
  // behaving differently from the way it was provisioned to.
  if (duty_ != nullptr) {
    const cycle::WakeInfo& w = duty_->wake();
    if (says(settings::Level::kInfo)) {
      printf("wake: %s ('wake', 'power')\n", cycle::describe(w.wake));
    }
    if (w.line_disarmed && says(settings::Level::kWarn)) {
      printf("wake: the radio's line is NOT armed as a wake source: %lu spurious wake(s) reached "
             "the limit of %u; the next timer wake arms it again\n",
             static_cast<unsigned long>(w.spurious_in_window),
             static_cast<unsigned>(cycle::kSpuriousPerWindow));
    }
    if (w.faults_in_a_row != 0 && says(settings::Level::kWarn)) {
      printf("wake: %lu crash or watchdog wake(s) in a row: the next sleep is lengthened, not "
             "shortened ('wake')\n", static_cast<unsigned long>(w.faults_in_a_row));
    }
    // The USB host. Not in the prompt - `docs/provisioning.md` and every tool that waits for
    // this port read `> ` and nothing else (tasks/phase-3/reports/04.md, *Deviations*) - but
    // said here, by `wake`, by `status` and whenever `stay` changes.
    if (w.usb_host && says(settings::Level::kInfo)) {
      print("wake: a USB host is on the port, so this Walter stays awake while you are here "
            "(and for a minute after the cable goes). 'stay' keeps it awake without one\n");
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
  // The echo. A secret's value echoes as asterisks (task p3-03, the agent firmware's
  // `secretValueIsBeingTyped`): what is typed after `set lte.pin ` never appears on the port, so
  // a shoulder, a screen recording and a scrollback hold stars. The echo does not go through
  // print(): a typed character is not a line and has no place in the ring `dump` prints.
  const char echo = secretBeingTyped() ? '*' : c;
  out_.write(&echo, 1);
}

// The line so far is `set <key> ` for a key the table calls a secret. Decided from the line and
// not from a flag, so that a backspace back into the key - `set lte.pinX` corrected to
// `set lte.apn` - echoes correctly from there on.
bool Console::secretBeingTyped() const {
  static const char kSet[] = "set ";
  constexpr std::size_t kSetLen = sizeof kSet - 1;
  if (length_ <= kSetLen) return false;
  if (std::strncmp(line_, kSet, kSetLen) != 0) return false;
  for (const settings::KeyDef& key : settings::kKeys) {
    if (!key.secret) continue;
    const std::size_t n = std::strlen(key.name);
    // `set <key> ` and at least one character after the space: the key is not a secret and
    // echoes as it is typed, the space after it echoes, and the value is stars.
    if (length_ <= kSetLen + n + 1) continue;
    if (std::strncmp(line_ + kSetLen, key.name, n) != 0) continue;
    if (line_[kSetLen + n] == ' ') return true;
  }
  return false;
}

void Console::tick() {
  // The duty cycle's bound on a wake (task p3-04): the sleeper is asked whether a host is on the
  // port, and a wake that has overrun is ended by a reset. `main/` arms it once a boot (task
  // p3-05) and the wake cycle is what reaches sleep; a USB host and `stay` hold it off.
  if (duty_ != nullptr && duty_->tick()) {
    print("the wake did not reach sleep inside its bound: restarting\n");
    return;
  }
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

// A command is on the stack for as long as one of these lives: `pump()` reads it.
class Console::LineInProgress {
 public:
  explicit LineInProgress(bool& flag) : flag_(flag) { flag_ = true; }
  ~LineInProgress() { flag_ = false; }
  LineInProgress(const LineInProgress&) = delete;
  LineInProgress& operator=(const LineInProgress&) = delete;

 private:
  bool& flag_;
};

void Console::pump() {
  // Whatever the radio sent while the caller was waiting: events queued, scanner lines kept. It
  // reads what is there and returns - nothing here blocks on the radio.
  radio_.poll();
  if (!running_line_) tick();
}

void Console::agentLine(const char* line) {
  if (line == nullptr) return;
  print(line);
}

void Console::runLine(const char* line) {
  const LineInProgress running(running_line_);
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
    case Verb::kRakRequest:
      rakRequest(p.command.req_type, p.command.bytes, p.command.bytes_size);
      return;
    case Verb::kRakEvents: rakEvents(); return;
    case Verb::kRakText: rakText(); return;
    case Verb::kRakSwdConnect: rakSwdConnect(); return;
    case Verb::kRakSwdId: rakSwdId(); return;
    case Verb::kRakSwdMem: rakSwdMem(p.command.address, p.command.words); return;
    case Verb::kRakFlashTest: rakFlashTest(); return;
    case Verb::kRakProvisionCheck: rakProvisionCheck(); return;
    case Verb::kRakProvision: rakProvision(p.command.text, p.command.on); return;
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
    case Verb::kRakMeshStatus: rakMeshStatus(p.command.text); return;
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
    case Verb::kAgentUrl: agentUrl(); return;
    case Verb::kAgentProbe: agentProbe(); return;
    case Verb::kAgentId: agentId(); return;
    case Verb::kAgentHeartbeat: agentHeartbeat(); return;
    case Verb::kAgentObs: agentObs(false); return;
    case Verb::kAgentObsPull: agentObs(true); return;
    case Verb::kAgentWake: agentWake(); return;
    case Verb::kAgentJobs: agentJobs(); return;
    case Verb::kAgentState: agentState(); return;
    case Verb::kGet: get(*p.command.key); return;
    case Verb::kGetAll: getAll(); return;
    case Verb::kSet: set(*p.command.key, p.command.text); return;
    case Verb::kErase: erase(); return;
    case Verb::kDump: dump(); return;
    case Verb::kJournal: journal(); return;
    case Verb::kWake: wake(); return;
    case Verb::kPower: power(); return;
    case Verb::kSleepNow: sleepNow(); return;
    case Verb::kSleepFor: sleepFor(p.command.seconds); return;
    case Verb::kStay: stay(p.command.on); return;
    case Verb::kCycleNow: cycleNow(); return;
    case Verb::kCycleDry: cycleDry(); return;
  }
}
// One line of help, the usage in a column and the text wrapped after it. The table
// (commands.h) holds one sentence per command; this is what makes it fit a terminal.
void Console::helpLine(const char* usage, const char* text) {
  constexpr std::size_t kColumn = 22;   // where the text begins
  constexpr std::size_t kWidth = 96;    // the whole line
  const std::size_t used = std::strlen(usage);
  // Two spaces of indent, the usage, and at least one space before the text: a usage that would
  // leave none gets a line of its own and the text begins under it.
  const bool inline_text = used + 3 <= kColumn;
  if (inline_text) {
    printf("  %-*s", static_cast<int>(kColumn - 2), usage);
  } else {
    printf("  %s\n", usage);
  }
  const char* p = text;
  bool first = inline_text;
  while (*p != 0) {
    if (!first) printf("%*s", static_cast<int>(kColumn), "");
    first = false;
    const std::size_t room = kWidth - kColumn;
    std::size_t take = std::strlen(p);
    if (take > room) {
      // Break at the last space that fits; a word longer than the room is cut at the room.
      take = room;
      while (take > 0 && p[take] != ' ') --take;
      if (take == 0) take = room;
    }
    char piece[128];
    const std::size_t n = take < sizeof piece - 1 ? take : sizeof piece - 1;
    std::memcpy(piece, p, n);
    piece[n] = 0;
    printf("%s\n", piece);
    p += take;
    while (*p == ' ') ++p;
  }
}

// Every command of the table, grouped, with its one line. The table is the only list there is
// (commands.h): host_test/core/console/test_console_grammar.cpp holds this output, the parser and
// docs/console.md to it.
void Console::help() {
  for (std::uint8_t g = 0; g < static_cast<std::uint8_t>(Group::kCount); ++g) {
    const Group group = static_cast<Group>(g);
    printf("%s:\n", describe(group));
    for (const CommandDef& command : kCommands) {
      if (command.group != group) continue;
      helpLine(command.usage, command.help);
    }
  }
  printf("every 'rak ...' takes an optional slot after the word rak - rak @1 hello - and @0\n"
         "without one; this unit has %u radio and it is slot 0\n",
         static_cast<unsigned>(slots_.slots()));
  printf("the radio's rail may be taken away for %lu to %lu whole seconds, and over %lu of them\n"
         "'rak kill' wants the word '%s' after the number\n",
         static_cast<unsigned long>(pins_.timing().min_ms / 1000),
         static_cast<unsigned long>(pins_.timing().max_ms / 1000),
         static_cast<unsigned long>(kKillSecondsWithoutWord), kConfirmWord);
  // The keys, from the same table `get` and `set` use (core/settings/keys.h).
  print("settings ('get', 'set <key> <value>'):\n");
  for (const settings::KeyDef& key : settings::kKeys) {
    helpLine(key.name, key.help);
  }
  print("a value takes effect as the key's row of docs/console.md says; a secret is never printed\n");
}

// One screen (task p3-03): this firmware and what it is, why it booted, the radio and its clock,
// the battery as the radio last reported it, the images, the uplink's last attempt, the next
// wake. **No secret and no IMEI** - the identity of a subscription is `lte id` and nothing else
// (core/uplink/policy.h, *Privacy*); host_test/core/console/test_console_secrets.cpp greps this
// output for both.
void Console::status() {
  const health::BootRecord& boot = health_.thisBoot();
  const image::Fields self = image::this_image();
  const char* role = image::role_name(self.role);
  printf("%s\n", core::banner());
  if (settings_ != nullptr && settings_->unitName()[0] != 0) {
    printf("unit: %s; role %s, board %s, band %s, link %u.%u\n", settings_->unitName(),
           role != nullptr ? role : "?", self.board, self.band,
           static_cast<unsigned>(self.link_major), static_cast<unsigned>(self.link_minor));
  } else {
    printf("unit: no name (set unit.name); role %s, board %s, band %s, link %u.%u\n",
           role != nullptr ? role : "?", self.board, self.band,
           static_cast<unsigned>(self.link_major), static_cast<unsigned>(self.link_minor));
  }
  if (boot.reason == health::ResetReason::kDeepSleepWake) {
    printf("booted: %s, %s; uptime %lu s, boot %lu since the record began\n",
           health::name(boot.reason), health::name(boot.wake),
           static_cast<unsigned long>(health_.uptimeS()),
           static_cast<unsigned long>(health_.boots()));
  } else if (boot.reason == health::ResetReason::kSoftware) {
    printf("booted: %s reset, %s; uptime %lu s, boot %lu since the record began\n",
           health::name(boot.reason), health::name(boot.requested),
           static_cast<unsigned long>(health_.uptimeS()),
           static_cast<unsigned long>(health_.boots()));
  } else {
    printf("booted: %s reset; uptime %lu s, boot %lu since the record began\n",
           health::name(boot.reason), static_cast<unsigned long>(health_.uptimeS()),
           static_cast<unsigned long>(health_.boots()));
  }

  // The radio: is it there, what is it, what does it think the time is, and the link's counters.
  const link::client::Session& session = radio_.session();
  if (session.up) {
    // The band where the `unit:` line above shows this firmware's own: what the radio's image was
    // built for (§5.1, 1.1), or *not said* from a 1.0 radio.
    printf("radio: %s %s, slot %u, band %s, link %u.%u\n", link::client::roleName(session.role),
           session.fw_version, static_cast<unsigned>(session.slot),
           session.band[0] != 0 ? session.band : "not said",
           static_cast<unsigned>(session.proto_major), static_cast<unsigned>(session.proto_minor));
  } else {
    print("radio: no session (nothing has been asked of it since this boot: try 'rak hello')\n");
  }
  if (supervisor_ != nullptr) {
    const supervisor::Status& w = supervisor_->status();
    printf("radio health: %s%s; clock %s\n", supervisor::describe(w.health),
           w.stopped ? ", and the supervisor has STOPPED ('rak watch')" : "",
           w.time_set ? "set from here" : w.time_skipped ? "not set (this unit has no time to give)"
                                                         : "not set yet");
    if (w.battery_known) {
      printf("battery: %lu mV, as the radio reported it %lu s ago\n",
             static_cast<unsigned long>(w.battery_mv),
             static_cast<unsigned long>(w.battery_age_ms / 1000u));
    } else {
      print("battery: never read (POWER_GET has not answered)\n");
    }
  }
  const link::client::Counters& counted = radio_.counters();
  printf("link: %s; retries %lu, timeouts %lu, drops %lu, events %lu\n",
         link::client::describe(radio_.mode()), static_cast<unsigned long>(counted.retries),
         static_cast<unsigned long>(counted.timeouts),
         static_cast<unsigned long>(radio_.receiver().dropped()),
         static_cast<unsigned long>(counted.events));
  // Two lines, so that neither is over a hundred characters: `status` is read in a terminal
  // window (docs/bench.md §14), and `pins` is the command that says it pin by pin.
  printf("pins: power %s; next reset %s\n", board::describe(pins_.power()),
         pins_.msUntilResetAllowed() == 0 ? "allowed now" : "not yet (two are a second apart)");
  printf("  uart %s, swd %s, nRESET %s, kill line %s\n",
         pins_.uartEnabled() ? "enabled" : "released",
         pins_.swdHeld() ? "acquired" : "released",
         pins_.resetAsserted() ? "asserted" : "released",
         pins_.killDriven() ? "driven high" : "not driven");

  // The images the store holds for this radio, in one line, and this firmware's own two slots.
  if (images_ != nullptr) {
    print("images:");
    for (std::uint8_t w = 0; w < image::kWhichCount; ++w) {
      const image::Which which = static_cast<image::Which>(w);
      image::Record record;
      const image::StoreError e = images_->read(which, board::Slot{0}, &record);
      if (e != image::StoreError::kNone) {
        printf(" %s=%s", image::name(which), image::describe(e));
      } else if (record.present && which == image::Which::kBase) {
        // A base has no version of ours to print (§4a): the bootloader's address says which it is.
        printf(" base=0x%08lx", static_cast<unsigned long>(record.base.bootloader));
      } else if (record.present) {
        printf(" %s=%s", image::name(which), record.descriptor.version);
      } else {
        printf(" %s=empty", image::name(which));
      }
    }
    print("\n");
  } else {
    print("images: no store in this build ('image list' says why)\n");
  }
  if (self_ != nullptr) {
    const update::SelfStatus& st = self_->status();
    interfaces::OwnSlot running;
    if (self_->runningSlot(&running)) {
      printf("this firmware: slot %u, %s, version %s; %s\n", static_cast<unsigned>(running.index),
             interfaces::describe(running.state),
             running.version[0] != 0 ? running.version : "(no version)",
             st.on_trial ? "ON TRIAL ('self status')" : "no trial ('self status')");
    } else {
      printf("this firmware: the running slot could not be read; %s\n",
             st.on_trial ? "ON TRIAL ('self status')" : "no trial ('self status')");
    }
  }

  // The uplink's last attempt, and when the next one is allowed. Nothing is touched here.
  if (uplink_ != nullptr) {
    const uplink::Status& u = uplink_->status();
    printf("uplink: %s; last %s at %s; %lu wake(s), %lu attempt(s), next in %lu\n",
           interfaces::describe(uplink_->state()), uplink::describe(u.last_failure),
           uplink::describe(u.reached), static_cast<unsigned long>(u.wakes),
           static_cast<unsigned long>(u.tries),
           static_cast<unsigned long>(u.wakes_until_next_try));
  }
  // The Walter's **own** attention (task p4-04): the agent's 401 and a restart it asked for and
  // did not get. Not §8's, which is the radio asking for us and is the `radio health` line
  // above; nothing of this touches the radio (core/cycle/agent_attention.h).
  if (duty_ != nullptr) {
    const std::uint8_t raised = duty_->reason().agentAttention();
    if (raised != 0) {
      char letters[3] = {};
      cycle::agentAttentionLetters(raised, letters);
      printf("agent: ATTENTION %s - %s\n", letters, cycle::describeAgentAttention(raised));
    }
  }
  if (server_ != nullptr) {
    if (server_->serverHost()[0] == 0) {
      print("backend: no server (set server.url); a self-update's third condition does not apply\n");
    } else {
      printf("backend: %s:%u, the agent's server and a self-update's third condition\n",
             server_->serverHost(), static_cast<unsigned>(server_->serverPort()));
    }
  }

  // The next wake (task p3-04). The band from the readings this wake has - `power` is the
  // command that takes a new one, and this line does not spend a POWER_GET - and what woke us.
  if (duty_ != nullptr) {
    const cycle::WakeInfo& w = duty_->wake();
    const power::Decision& d = duty_->decision();
    printf("this wake: %s; %lu wake(s) counted%s\n", cycle::name(w.wake),
           static_cast<unsigned long>(w.wakes),
           w.line_disarmed ? "; the radio's line is disarmed ('wake')" : "");
    if (duty_->decided()) {
      printf("next wake: %lu min, band %s%s; line %s ('power', 'wake')\n",
             static_cast<unsigned long>(d.interval_minutes), power::name(d.band),
             d.overridden ? " (wake.interval)" : "",
             duty_->lineWillArm() ? "armed" : "not armed");
    } else {
      // No reading has been taken this wake, and `status` does not spend a POWER_GET to take
      // one: `power` is the command that decides. So this says what is known - the band the
      // last wake ended in - and when the next one will be chosen.
      char every[16] = "auto";
      const std::uint32_t asked = duty_->intervalSetting();
      if (asked != 0) {
        std::snprintf(every, sizeof every, "%lu min", static_cast<unsigned long>(asked));
      }
      printf("next wake: at the end of this wake; last band %s, wake.interval %s; line %s\n",
             power::name(duty_->reason().band()), every,
             duty_->lineWillArm() ? "armed" : "not armed");
    }
  } else if (settings_ != nullptr) {
    const std::uint32_t interval = settings_->wakeIntervalMinutes();
    char every[16] = "auto";
    if (interval != 0) {
      std::snprintf(every, sizeof every, "%lu min", static_cast<unsigned long>(interval));
    }
    printf("next wake: this build has no duty cycle; wake.interval %s, radio may wake it: %s\n",
           every, settings_->wakeOnRadio() ? "yes" : "no");
  }
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

// The one place text the radio sent reaches the port: a scanner's NDJSON line (`rak text`), a
// CLI reply, an advert name. **It is printed and not kept** (spec/SYSTEM.md S-12: "Nothing the
// mesh says is logged as text"): the bytes go straight to the port, outside print(), and the
// Quiet holds the ring shut for the newline and for anything a caller prints inside this.
void Console::printText(link::Text text) {
  const LogRing::Quiet quiet(log_);
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
  // §5.1's `band`, which 1.1 added: with the role and the board it is what an image is chosen by
  // (spec/FIRMWARE-MANIFEST.md §5 check 3), and a 1.0 radio does not say it. Its own line, so
  // that the sentence for a radio that says none has room to be a sentence.
  if (s.band[0] != 0) {
    printf("  band %s\n", s.band);
  } else {
    printf("  band: not said (a %u.%u radio)\n", static_cast<unsigned>(s.proto_major),
           static_cast<unsigned>(s.proto_minor));
  }
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

// §5.9's LOCAL_REQUEST (link 1.2, task p9-06): one question of the radio's own request handler,
// answered as it answers an admin over the mesh and with nothing on the air. The answer is the
// handler's own bytes, so they are printed as hex - and for a status, which is the one a person
// on the bench reads for meaning, field by field in the layout the fixtures were written from
// (`link::decodeRepeaterStats`; spec/fixtures/link/link-v1.2.json).
void Console::rakRequest(std::uint8_t req_type, const std::uint8_t* data, std::size_t size) {
  const lc::Answer<link::LocalRequestResponse> answer =
      radio_.localRequest(req_type, link::Bytes{size == 0 ? nullptr : data, size});
  printf("local request 0x%02x (%s)", static_cast<unsigned>(req_type),
         lc::localRequestName(req_type));
  if (size > 0) {
    printBytes(", data ", link::Bytes{data, size});
  }
  print("\n");
  if (!answer.ok()) {
    linkError("rak request", answer.fault, answer.status);
    if (answer.fault == lc::Fault::kUnsupported) {
      print("  this radio does not carry LOCAL_REQUEST (link 1.2): a status, an access list, a "
            "neighbour list and the owner info can only be read over the mesh from it\n");
    } else if (answer.fault == lc::Fault::kRefused && answer.status == link::status::kRange) {
      // §5.9: a request the handler gives no answer to. Over the mesh it would have been silence.
      print("  the handler answered nothing, which over the mesh would have been silence: this "
            "request type, or this `data`, is not one it answers\n");
    }
    return;
  }
  const link::Bytes body = answer.message.data;
  printf("  %s: %u byte(s)", lc::statusName(answer.status), static_cast<unsigned>(body.size));
  if (body.size > 0) {
    printBytes("\n  ", body);
  }
  print("\n");
  if (req_type != link::local_req::kStatus) {
    return;
  }
  link::RepeaterStats s;
  if (!link::decodeRepeaterStats(body, &s)) {
    printf("  a RepeaterStats is at least %u bytes; this answer is not one\n",
           static_cast<unsigned>(link::kRepeaterStatsMinLen));
    return;
  }
  // The SNR is quarter-decibels on the wire (§6.3's own unit) and printed as a tenth of a dB
  // without floating point, as `rak env` prints its hundredths.
  const int snr_tenths = (static_cast<int>(s.last_snr_q4) * 10) / 4;
  printf("  battery %u mV, uptime %lu s, tx queue %u\n", static_cast<unsigned>(s.battery_mv),
         static_cast<unsigned long>(s.uptime_s), static_cast<unsigned>(s.tx_queue_len));
  printf("  noise floor %d dBm, last rssi %d dBm, last snr %s%d.%d dB\n",
         static_cast<int>(s.noise_floor_dbm), static_cast<int>(s.last_rssi),
         snr_tenths < 0 && snr_tenths > -10 ? "-" : "", snr_tenths / 10,
         snr_tenths < 0 ? -(snr_tenths % 10) : snr_tenths % 10);
  printf("  packets: %lu received (%lu flood, %lu direct), %lu sent (%lu flood, %lu direct)\n",
         static_cast<unsigned long>(s.packets_recv), static_cast<unsigned long>(s.recv_flood),
         static_cast<unsigned long>(s.recv_direct), static_cast<unsigned long>(s.packets_sent),
         static_cast<unsigned long>(s.sent_flood), static_cast<unsigned long>(s.sent_direct));
  printf("  airtime %lu s transmitting; duplicates %u flood, %u direct\n",
         static_cast<unsigned long>(s.airtime_s), static_cast<unsigned>(s.flood_dups),
         static_cast<unsigned>(s.direct_dups));
  if (s.has_rx_airtime) {
    printf("  airtime %lu s receiving\n", static_cast<unsigned long>(s.rx_airtime_s));
  } else {
    print("  airtime receiving: not said (an older RepeaterStats)\n");
  }
  printf("  error events %u, receive errors ", static_cast<unsigned>(s.err_events));
  if (s.has_recv_errors) {
    printf("%lu\n", static_cast<unsigned long>(s.recv_errors));
  } else {
    print("not said (an older RepeaterStats)\n");
  }
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

// ---------------------------------------------------------------------- the radio's base (p5-03)
//
// rak provision check, and rak provision <radio> confirm: spec/SYSTEM.md S-5a on
// core/provision/provision.h. The console makes the grant from the typed word - this file is
// the one place a parsed verb becomes a grant (rail R3b) - and prints each step as the sequence
// ends it, then one line for the outcome. docs/provisioning.md is the session, replayed by a
// host test.

class Console::ProvisionPrinter final : public provision::StepListener {
 public:
  explicit ProvisionPrinter(Console& console) : console_(console) {}
  void onStep(provision::Step step, const provision::Result& r) override {
    console_.printProvisionStep(step, r);
  }

 private:
  Console& console_;
};

namespace {

namespace prov = core::provision;

// Defined below, with the image store's commands.
void hashHex(const std::uint8_t* hash, std::size_t bytes, char* out);

const char* portWord(bool open) { return open ? "open" : "CLOSED"; }

// The base's one line: its hash, the bootloader's address, which SoftDevice.
void baseSummaryLine(char* buf, std::size_t size, const prov::Result& r) {
  char hash[17];
  hashHex(r.base_sha256, 8, hash);
  const unsigned long v = r.base.softdevice_version;
  std::snprintf(buf, size, "sha256 %s..., bootloader 0x%08lx, S%lu %lu.%lu.%lu, %lu bytes of flash, %u UICR words",
                hash, static_cast<unsigned long>(r.base.bootloader),
                static_cast<unsigned long>(r.base.softdevice_id), v / 1000000u, (v / 1000u) % 1000u,
                v % 1000u, static_cast<unsigned long>(r.base.flash_bytes),
                static_cast<unsigned>(r.base.uicr_word_count));
}

}  // namespace

void Console::rakProvisionCheck() {
  const flasher::BoardProfile* profile = flasher::profileForSlot(slot_);
  printf("rak provision check: slot %u, %s; nothing is written\n", static_cast<unsigned>(slot_.index),
         profile != nullptr ? profile->name : "no board profile");
  const prov::Result r = provision_.check(slot_);
  // The store.
  if (r.base_present && r.check_status != prov::Status::kBaseRefused && r.check_status != prov::Status::kBaseUnreadable) {
    char line[160];
    baseSummaryLine(line, sizeof line, r);
    printf("  base: %s; check 5p again over the stored bytes: ok (%lu ms)\n", line,
           static_cast<unsigned long>(r.base_check_ms));
  } else if (r.check_status == prov::Status::kBaseRefused) {
    printf("  base: refused at check 5p: %s\n", r.why);
  } else if (r.check_status == prov::Status::kBaseUnreadable) {
    printf("  base: %s\n", prov::describe(r.check_status));
  } else {
    printf("  base: %s\n", prov::describe(prov::Status::kNoBase));
  }
  // The battery.
  if (!r.battery_known) {
    print("  battery: unknown (no reading over the link); accepted, as a first install accepts it\n");
  } else if (r.check_status == prov::Status::kBatteryLow || r.check_status == prov::Status::kBatteryFalling) {
    printf("  battery: %lu mV: %s\n", static_cast<unsigned long>(r.battery_mv), prov::describe(r.check_status));
  } else {
    printf("  battery: %lu mV\n", static_cast<unsigned long>(r.battery_mv));
  }
  // The radio.
  if (!r.attached) {
    char line[160];
    if (r.status == prov::Status::kPins) {
      printf("error: rak provision check: %s: %s\n", prov::describe(r.status), board::describe(r.pins));
    } else {
      describeFailure(line, sizeof line, "rak provision check", r.nrf);
      printf("%s\n", line);
    }
    return;
  }
  printf("  swd: DPIDR 0x%08lx; control access port IDR 0x%08lx; protection: %s%s\n",
         static_cast<unsigned long>(r.dpidr), static_cast<unsigned long>(r.ctrl_ap_idr),
         portWord(r.port_open_at_attach),
         r.port_open_at_attach ? "" : " (the identity is readable only after the erase)");
  if (r.port_open_at_attach) {
    printf("  chip: nRF52840, variant %s: %s\n", r.variant.text, prov::describe(r.variant.kind));
    printf("  device id %08lx%08lx\n", static_cast<unsigned long>(r.identity.device_id[1]),
           static_cast<unsigned long>(r.identity.device_id[0]));
  } else if (r.application_answered) {
    printf("  link: an application answers HELLO: public key %s... ('rak id'); it is named before it is erased\n",
           r.key16);
  } else {
    print("  link: no application answers HELLO\n");
  }
  if (r.check_status != prov::Status::kOk) {
    printf("  the command would be refused at step 1: %s\n", prov::describe(r.check_status));
  }
  if (r.status == prov::Status::kVariantUnknown) {
    print("  the command would be refused at step 3: INFO.VARIANT is not in the table; nothing would be erased\n");
  }
  if (r.next[0] != 0) printf("  next: %s\n", r.next);
}

void Console::printProvisionStep(prov::Step step, const prov::Result& r) {
  const unsigned long ms = r.step_ms[static_cast<std::size_t>(step)];
  char line[160];
  switch (step) {
    case prov::Step::kCheck:
      baseSummaryLine(line, sizeof line, r);
      printf("  [1/7] checked: the base %s;\n", line);
      if (r.battery_known) {
        printf("        battery %lu mV; the slot is free (%lu ms)\n", static_cast<unsigned long>(r.battery_mv), ms);
      } else {
        printf("        battery unknown (no reading over the link: accepted); the slot is free (%lu ms)\n", ms);
      }
      return;
    case prov::Step::kAttach:
      if (r.port_open_at_attach) {
        printf("  [2/7] attached: DPIDR 0x%08lx, protection open; nRF52840 %s, device id %08lx%08lx, as typed (%lu ms)\n",
               static_cast<unsigned long>(r.dpidr), r.variant.text,
               static_cast<unsigned long>(r.identity.device_id[1]),
               static_cast<unsigned long>(r.identity.device_id[0]), ms);
      } else {
        printf("  [2/7] attached: DPIDR 0x%08lx, protection CLOSED; the link: %s (%lu ms)\n",
               static_cast<unsigned long>(r.dpidr),
               r.application_answered ? "an application answers HELLO, and it is the one named"
                                      : "no application answers HELLO, as 'locked' says",
               ms);
      }
      return;
    case prov::Step::kVariant:
      printf("  [3/7] variant %s: %s (%lu ms)\n", r.variant.text, prov::describe(r.variant.kind), ms);
      return;
    case prov::Step::kErase:
      printf("  [4/7] erased: CTRL-AP ERASEALL; the port %s; flash, NRFFW[0] and APPROTECT blank; the core halted (%lu ms)\n",
             r.soft_reset_done ? "open after one CTRL-AP soft reset (the only reset until step 7)"
                               : "open after it (no soft reset needed)",
             ms);
      return;
    case prov::Step::kWrite:
      printf("  [5/7] written: %lu flash words in %lu runs, %lu UICR words, UICR.APPROTECT %s (%lu ms)\n",
             static_cast<unsigned long>(r.flash_words), static_cast<unsigned long>(r.flash_runs),
             static_cast<unsigned long>(r.uicr_words),
             r.approtect_written == prov::kErasedWord ? "left erased (legacy)" : "= 0x0000005a", ms);
      return;
    case prov::Step::kVerify:
      printf("  [6/7] verified: %lu words read back and equal; UICR.APPROTECT reads 0x%08lx (%lu ms)\n",
             static_cast<unsigned long>(r.verified_words), static_cast<unsigned long>(r.approtect_read), ms);
      return;
    case prov::Step::kReset:
      if (r.port_open_after_reset) {
        printf("  [7/7] reset: one pin reset (waited %lu ms for the spacing), the bootloader given %lu ms; the port after it open, NRFFW[0] 0x%08lx (%lu ms)\n",
               static_cast<unsigned long>(r.reset_wait_ms), static_cast<unsigned long>(prov::kBootloaderSettleMs),
               static_cast<unsigned long>(r.nrffw0_after), ms);
      } else {
        printf("  [7/7] reset: one pin reset (waited %lu ms for the spacing), the bootloader given %lu ms; the port after it CLOSED (%lu ms)\n",
               static_cast<unsigned long>(r.reset_wait_ms), static_cast<unsigned long>(prov::kBootloaderSettleMs), ms);
      }
      return;
    case prov::Step::kNone:
    case prov::Step::kDone:
      return;
  }
}

void Console::printProvisionRefusal(const prov::Result& r) {
  char detail[192] = "";
  switch (r.status) {
    case prov::Status::kBaseRefused:
      std::snprintf(detail, sizeof detail, ": %.150s", r.why);  // §4a's longest sentence is 119
      break;
    case prov::Status::kPins:
    case prov::Status::kResetFailed:
    case prov::Status::kLookFailed:
      if (r.pins != board::Outcome::kOk) std::snprintf(detail, sizeof detail, ": %s", board::describe(r.pins));
      break;
    case prov::Status::kSwd:
    case prov::Status::kHaltFailed:
    case prov::Status::kVerifyFailed:
      std::snprintf(detail, sizeof detail, ": %s", nrf52::describe(r.nrf.status));
      break;
    case prov::Status::kDoor:
    case prov::Status::kEraseFailed:
    case prov::Status::kWriteFailed:
      std::snprintf(detail, sizeof detail, ": %s", nrf52::describe(r.door.status));
      break;
    case prov::Status::kVerifyMismatch:
      if (r.mismatch_address != 0) {
        std::snprintf(detail, sizeof detail, ": at 0x%08lx the base says 0x%08lx and the radio reads 0x%08lx",
                      static_cast<unsigned long>(r.mismatch_address), static_cast<unsigned long>(r.mismatch_expected),
                      static_cast<unsigned long>(r.mismatch_actual));
      } else {
        std::snprintf(detail, sizeof detail, ": UICR.APPROTECT should read 0x%08lx and reads 0x%08lx",
                      static_cast<unsigned long>(r.mismatch_expected), static_cast<unsigned long>(r.mismatch_actual));
      }
      break;
    case prov::Status::kNotBlank:
    case prov::Status::kApplicationAnswers:
      if (r.application_answered) std::snprintf(detail, sizeof detail, " (public key %s...)", r.key16);
      break;
    default:
      break;
  }
  printf("error: rak provision: refused at step %u (%s): %s%s\n", static_cast<unsigned>(r.ended),
         prov::describe(r.ended), prov::describe(r.status), detail);
  if (r.erased) {
    print("  the radio is erased: run the command again from the top ('rak provision check' says what to type)\n");
  } else {
    print("  nothing was erased\n");
  }
  if (r.next[0] != 0 && !r.erased) printf("  next: %s\n", r.next);
}

void Console::rakProvision(const char* radio, bool confirmed) {
  if (!confirmed) {
    printf("rak provision %s: would erase the radio whole - RUI3, any identity, UICR - through the CTRL-AP\n", radio);
    printf("  and write the base from the store; to do it, type: rak provision %s %s\n", radio, kConfirmWord);
    return;
  }
  nrf52::ProvisionGrant grant = nrf52::grantFromTypedCommand(radio);
  if (!grant.valid()) {
    printf("error: rak provision: %s; 'rak provision check' says what to type\n",
           prov::describe(prov::Status::kGrantMalformed));
    return;
  }
  printf("rak provision: slot %u, %s: S-5a's seven steps; nothing is written before step 4, each printed as it ends\n",
         static_cast<unsigned>(slot_.index), radio);
  ProvisionPrinter printer(*this);
  const prov::Result r = provision_.run(slot_, static_cast<nrf52::ProvisionGrant&&>(grant), &printer);
  if (!r.ok()) {
    printProvisionRefusal(r);
    return;
  }
  char hash[17];
  hashHex(r.base_sha256, 8, hash);
  printf("rak provision: PROVISIONED slot %u: device id %08lx%08lx, variant %s, base sha256 %s..., port after the reset %s; %lu s in all\n",
         static_cast<unsigned>(slot_.index), static_cast<unsigned long>(r.identity.device_id[1]),
         static_cast<unsigned long>(r.identity.device_id[0]), r.variant.text, hash,
         portWord(r.port_open_after_reset), static_cast<unsigned long>((r.total_ms + 999u) / 1000u));
  if (!r.port_open_after_reset) {
    print("  the access port is CLOSED after the reset: this bootloader does not copy UICR.APPROTECT into APPROTECT.DISABLE on a\n"
          "  revision 3 part (spec/SYSTEM.md 10.2). The radio works - a UF2 drive on its USB - and the flasher cannot reach it until\n"
          "  that is solved; the base was verified word for word before the reset\n");
  }
  print("  the slot awaits its first install: the supervisor leaves the radio alone until 'rak update --first-install' commits\n");
  print("  next: python tools/walterctl.py push --port <port> --as candidate <release-dir>\n"
        "        rak update --first-install\n");
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

// The base's lines: what §4a found in the file, out of the record (spec/FIRMWARE-MANIFEST.md
// §4a; the record keeps the summary where an image keeps its descriptor).
void Console::baseLines(const image::Record& record) {
  char hash[17];
  hashHex(record.sha256, 8, hash);
  const unsigned long v = static_cast<unsigned long>(record.base.softdevice_version);
  printf("the board's base, %lu bytes, sha256 %s..., bootloader 0x%08lx\n",
         static_cast<unsigned long>(record.size), hash,
         static_cast<unsigned long>(record.base.bootloader));
  printf("             S%lu %lu.%lu.%lu, %lu bytes of flash, %u UICR words",
         static_cast<unsigned long>(record.base.softdevice_id), v / 1000000, (v / 1000) % 1000,
         v % 1000, static_cast<unsigned long>(record.base.flash_bytes),
         static_cast<unsigned>(record.base.uicr_word_count));
  if (record.base.has_mbr_params) {
    printf(", the MBR's parameter page at 0x%08lx\n",
           static_cast<unsigned long>(record.base.mbr_params));
  } else {
    print("\n");
  }
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
  if (record.use == image::Use::kProvision) {
    baseLines(record);
  } else {
    const char* role = image::role_name(record.descriptor.role);
    char hash[17];
    hashHex(record.sha256, 8, hash);
    printf("%s %s %s %s, %lu bytes at 0x%08lx, sha256 %s..., area %u\n",
           role != nullptr ? role : "?", record.descriptor.board, record.descriptor.band,
           record.descriptor.version, static_cast<unsigned long>(record.size),
           static_cast<unsigned long>(record.load_address), hash,
           static_cast<unsigned>(images_->areaOf(record.which, record.slot)));
  }
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
  printf("image store: %u slots x %u images of %lu bytes (%lu sectors) and a header sector each,\n",
         static_cast<unsigned>(image::kRadioSlots),
         static_cast<unsigned>(image::kPromotableCount),
         static_cast<unsigned long>(image::kImageBytesMax),
         static_cast<unsigned long>(image::kDataSectors));
  printf("  plus one base of %lu bytes (%lu sectors) a slot, and %lu index sectors:\n",
         static_cast<unsigned long>(image::kBaseBytesMax),
         static_cast<unsigned long>(image::kBaseSectors),
         static_cast<unsigned long>(image::kIndexCopies));
  printf("  %lu bytes of a %lu-byte partition, %lu left; index sequence %lu\n",
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

// `image verify base`: the hash out of flash, and then §4a's seven rules over the same bytes
// against **this build's** region for the board - check 5p again, on the bytes as they lie in the
// Walter's own flash, because that is what task p5-03 will write to a radio.
void Console::verifyBase(const image::Record& record) {
  const flasher::BoardProfile* profile = flasher::profileForSlot(slot_);
  if (profile == nullptr) {
    print("base: this adapter has no radio in that slot, so there is no region to judge it "
          "against\n");
    return;
  }
  image::StoredImage stored;
  const image::StoreError opened = images_->open(image::Which::kBase, slot_, &stored);
  if (opened != image::StoreError::kNone) {
    printf("error: %s\n", image::describe(opened));
    return;
  }
  const image::BaseResult result = base_reader_.read(stored, profile->app.begin, profile->app.end);
  if (!result.ok()) {
    printf("base: check 5p over the stored bytes: %s: %s\n", image::describe(result.reason),
           result.why);
    return;
  }
  printf("base: check 5p over the stored bytes: a base for %s (0x%08lx to 0x%08lx), bootloader "
         "0x%08lx,\n", profile->name, static_cast<unsigned long>(profile->app.begin),
         static_cast<unsigned long>(profile->app.end),
         static_cast<unsigned long>(result.summary.bootloader));
  printf("  %lu bytes of flash in %u runs, %u UICR words - the same as its record says\n",
         static_cast<unsigned long>(result.summary.flash_bytes),
         static_cast<unsigned>(result.runs.count),
         static_cast<unsigned>(result.summary.uicr_word_count));
  if (result.summary.bootloader != record.base.bootloader ||
      result.summary.flash_bytes != record.base.flash_bytes ||
      result.summary.uicr_word_count != record.base.uicr_word_count) {
    print("  WRONG: the record and the bytes disagree about what this base is\n");
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
    printf("error: %s %s: %s\n", image::name(which),
           which == image::Which::kBase ? "" : record.descriptor.version, image::describe(e));
    return;
  }
  printf("%s: %lu bytes read back and hashed in %lu ms: the SHA-256 is the one in its record\n",
         image::name(which), static_cast<unsigned long>(record.size), ms);
  if (which == image::Which::kBase) verifyBase(record);
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
  const bool base = which == image::Which::kBase;
  // The protocol's version in the ready line, in text, before a frame has been sent: it is how
  // walterctl decides whether it is talking to a Walter at 0.1.0 - whose answers came through
  // this very conversion and could be corrupted, the hello's own among them - without having to
  // guess from a frame it may not be able to read (task p8-02, tools/walterctl.py).
  printf("image receive: slot %u's %s; this port is now a framed binary transfer (protocol %u)\n",
         static_cast<unsigned>(slot_.index), base ? "base" : image::name(which),
         static_cast<unsigned>(image::transfer::kVersion));
  print("  (COBS and CRC-16, the box link's own framing). Send manifest.json, then its .sig,\n");
  printf("  then the file: tools/walterctl.py push --as %s\n", image::name(which));
  printf("  what will be accepted: role %s, board %s, band %s, and %s of\n", needs.role,
         needs.board, needs.band, base ? "checks 1 to 4 and 5p" : "all six checks");
  print("  FIRMWARE-MANIFEST section 5 run here - the laptop is not trusted\n");
  if (base) {
    print("  (a base is the board's MBR, SoftDevice and bootloader as Intel HEX, section 4a: it\n"
          "  is written to a radio only by `rak provision`, and by nothing automatic)\n");
  }
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
  // An answer the port would not take whole (task p8-02). Said only when it happened, because it
  // never should: to the laptop such an answer is a lost one, and this line is what tells a
  // person at the bench that the port, and not the cable or the tool, is where it went.
  if (receiver_->cutShort() != 0) {
    printf("  %lu answers the port would not take whole\n",
           static_cast<unsigned long>(receiver_->cutShort()));
  }
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
  // A first install that committed clears the slot's "awaiting its first install" (task p5-03)
  // now, not at the next heartbeat.
  if (supervisor_ != nullptr) supervisor_->noticeFirstInstall();
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
    case update::State::kNodeFault:
      // The image is fine and the node is not (task p10-03). Three printf calls: together the
      // lines are longer than printf's 200-byte buffer.
      printf("rak update: the image is fine, the node is not: %s\n", update::describe(st.reason));
      printf("  %s is on the radio and proved everything an image can; no rollback was spent and\n",
             st.version);
      print("  it is on no bad list. 'rak cli erase' then 'rak cli reboot' (docs/flashing.md 3a,\n"
            "  the old key cannot be recovered), then this update can be run again.\n");
      break;
    default:
      printf("rak update: %s\n", update::describe(st.state));
      break;
  }
  if (st.time_skipped) {
    // Why it was really skipped (task p9-04). *Until phase 3's modem* has been wrong since
    // phase 3: the modem is in, and the thing that was missing on 7 November was a time **in
    // this boot** - `interfaces::Clock` restarts at zero at every wake, so the network's time is
    // taken again each time and nothing had taken it before the `rak update` was typed
    // (`core/cycle/network_clock.h`). `lte up` now gives it to the Walter's own clock, and so do
    // a wake's step 6 (`cycle now`) and the agent's server time (`agent probe`).
    print("  (the clock check was skipped: this Walter had no time of its own in this boot, so\n");
    print("   there was none to give the radio. 'lte up' before 'rak update' lets it run)\n");
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
  // The protocol's version, as `image receive` says it (above).
  printf("self receive: this Walter's other application slot (%u); this port is now a framed "
         "binary transfer (protocol %u)\n",
         static_cast<unsigned>(other.index), static_cast<unsigned>(image::transfer::kVersion));
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
  if (w.awaiting_first_install) {
    printf("the slot awaits its first install (rak provision, journal entry %lu): the ladder is not climbed for a silent radio\n"
           "  until 'rak update --first-install' commits\n",
           static_cast<unsigned long>(w.awaiting_since_seq));
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
  // The sockets the HTTPS connections of this boot were made on (task p9-02): `lte` is where a
  // person looks after a request that stalled, and the rotation's accounting is the half of that
  // story the console could not tell.
  printSockets(0, false);
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
    // **And the Walter's own clock takes it** (task p9-04). The wake cycle's step 6 does exactly
    // this with the same `interfaces::NetworkTime` through the same window
    // (`core/cycle/network_clock.h`: a time is believed only when it came from the network), and
    // until this line a console session had no clock of its own however long the uplink had been
    // up - so the link had no time to give the radio (§5.4) and `rak update`'s clock check was
    // skipped in every boot a person drove by hand. One place still decides what to believe, and
    // it is the network.
    // A time whose origin is the network has already been through that window on the way in, so
    // `give` only answers false here when there is no clock in this build.
    const bool took = wall_ != nullptr && wall_->give(r.time);
    if (r.time.origin == interfaces::TimeOrigin::kNetwork) {
      printf("  time: %lu unix seconds, from the network%s\n",
             static_cast<unsigned long>(r.time.epoch),
             took ? "; this Walter's clock is set from it, so the radio can be given it too" : "");
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

// The rotation's counters (task p9-02, core/net/socket_rotation.h). Two stalls on the first unit
// on 7 November could not be read because nothing printed which socket id a connection had taken
// or whether a close was owed on it; this is that, on the two commands a person uses when the
// uplink misbehaves. It prints the ids of the connections made since `from` - the request's own
// for `http get`, the last few of the boot for `lte` - and then the whole boot's accounting.
void Console::printSockets(std::uint32_t from, bool one_request) {
  if (sockets_ == nullptr) return;
  const net::SocketRotation& s = *sockets_;
  const std::uint32_t total = s.connections();
  if (s.idCount() == 2) {
    printf("  sockets: ids %d and %d, turn and turn about; %lu connection%s in this boot\n",
           s.firstId(), s.secondId(), static_cast<unsigned long>(total), total == 1 ? "" : "s");
  } else {
    printf("  sockets: id %d alone (the library gave out no second one); %lu connection%s in this "
           "boot\n",
           s.firstId(), static_cast<unsigned long>(total), total == 1 ? "" : "s");
  }
  // Which id each connection used, as far back as the rotation keeps them.
  const std::uint32_t kept = static_cast<std::uint32_t>(s.recentCount());
  const std::uint32_t oldest = total - kept;
  const std::uint32_t at = from > oldest ? from : oldest;
  if (at < total) {
    printf("    %s", one_request ? "this request: " : "the last few: ");
    for (std::uint32_t n = at; n < total; ++n) {
      printf("%s%d", n == at ? "" : " ", s.recentId(static_cast<std::size_t>(n - oldest)));
    }
    print("\n");
  }
  printf("    owed closes: %lu waited for, %lu timed out, %lu written off; %lu discounted; "
         "%lu event%s for an id not in the rotation\n",
         static_cast<unsigned long>(s.owedWaits()), static_cast<unsigned long>(s.owedTimeouts()),
         static_cast<unsigned long>(s.writeOffs()),
         static_cast<unsigned long>(s.discountedCloses()),
         static_cast<unsigned long>(s.otherIdEvents()), s.otherIdEvents() == 1 ? "" : "s");
  for (int which = 0; which < 2; ++which) {
    const int id = which == 0 ? s.firstId() : s.secondId();
    if (id == 0) continue;
    if (s.isQuiet(id)) {
      printf("    id %d expects no close: one was owed and written off, and none has arrived "
             "since\n", id);
    } else if (s.owes(id)) {
      printf("    id %d owes a close: the next connection on it waits %lu ms for it\n", id,
             static_cast<unsigned long>(net::kOwedCloseWaitMs));
    }
  }
}

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
  // The rotation's count before this request, so that the lines below name this request's own
  // connections (a redirect makes two) and not the boot's.
  const std::uint32_t sockets_before = sockets_ != nullptr ? sockets_->connections() : 0;
  http_->setTrace(&out_);
  const net::Result r = http_->get(url, options, body);
  http_->setTrace(nullptr);
  if (r.verdict == net::Verdict::kRefusedUrl) {
    printf("error: http get: %s\n", r.why);
    return;
  }
  if (r.status == 0) {
    printf("error: http get: no response: %s\n", r.why);
    // Above all here: a request with no response is the one whose socket id and owed close are
    // worth knowing (task p9-02).
    printSockets(sockets_before, true);
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
  printSockets(sockets_before, true);
}

// ---------------------------------------------------------------------------- the agent's uplink

bool Console::agentOk() {
  if (agent_ != nullptr) return true;
  print("error: this build has no agent uplink: nothing was given to the console (main/main.cpp)\n");
  return false;
}

void Console::agentUrl() {
  if (!agentOk()) return;
  const char* url = agent_->url();
  // The URL and the version header, and whether a token is set - never the token (S-12).
  printf("agent url: %s%s\n", url[0] != 0 ? url : "server.url unset", url[0] != 0 ? "" : " (the agent makes no call)");
  printf("  X-Agent-Version: %s\n", agent_->version());
  printf("  server.token %s\n", agent_->tokenSet() ? "set" : "unset");
  if (!agent_->configured()) print("  not configured: both are needed before a call is made\n");
}

void Console::agentProbe() {
  if (!agentOk() || !uplinkUp()) return;
  if (!agent_->configured()) {
    print("error: agent probe: not configured: server.url and server.token are needed ('set server.url "
          "https://...', 'set server.token ...')\n");
    return;
  }
  printf("agent probe: GET %s/agent/v1/config with the token (the chain is verified against this build's "
         "roots)\n", agent_->url());
  UplinkProbeResult r;
  agent_->probe(&r);
  if (!r.reachable) {
    printf("error: agent probe: %s (%lu ms)\n", r.error, static_cast<unsigned long>(r.elapsed_ms));
    return;
  }
  printf("agent probe: %s -> http %d %s, %lu bytes, %lu ms, via %s\n", r.endpoint, r.status,
         r.ok ? "ok" : "not ok", static_cast<unsigned long>(r.reply_bytes),
         static_cast<unsigned long>(r.elapsed_ms), r.via);
  if (!r.ok) printf("  %s\n", r.error);
  if (r.retry_after_s != 0) printf("  Retry-After: %lu s\n", static_cast<unsigned long>(r.retry_after_s));
  if (r.rate_limit_remaining >= 0) printf("  X-RateLimit-Remaining: %d\n", r.rate_limit_remaining);
  if (r.ok) {
    printf("  agent %lld \"%s\", transport %s, budget %d/min gap %d s, poll %d s, serverTime %s\n",
           r.agent_id, r.label, r.transport, r.max_transactions_per_minute, r.min_gap_seconds,
           r.poll_interval_s, r.server_time);
  }
}

// ---------------------------------------------------------------------------- the agent's identity

bool Console::identityOk() {
  if (identity_ != nullptr) return true;
  print("error: this build has no agent identity: nothing was given to the console "
        "(main/main.cpp)\n");
  return false;
}

void Console::agentId() {
  if (!identityOk()) return;
  AgentIdentityView& v = identity_view_;
  const bool known = identity_->identity(&v);
  if (!v.session) {
    print("agent id: the radio has answered nothing in this boot, so there is no identity to read "
          "out of it ('rak hello')\n");
  } else if (v.identity == link::IdentityState::kLost) {
    // **A radio that refuses** (task p10-03): from repeater v0.1.2 a node whose file system
    // would not mount makes no new key, and says so in §5.6. It is not a radio awaiting its
    // first install and no image can mend it - the way out is a person's two words. In pieces,
    // because printf's line buffer is 200 bytes and this is longer (task p9-04).
    print("agent id: the radio refuses: its identity is lost. No key was made, so the heartbeat\n");
    print("  carries no key, no radio block and no job types - and a counter that says which of\n");
    print("  the two this is (radioIdentityLost).\n");
    printf("  the evidence: IDENTITY answered ERR_STATE%s.\n",
           v.caps_identity_lost ? ", and HELLO advertises caps 0x21 (CLI, ATTENTION) and nothing"
                                  " else"
                                : "");
    print("  what a person does: read docs/flashing.md 3a first - the old key cannot be\n");
    print("  recovered - then 'rak cli erase' and 'rak cli reboot' make this radio a new node,\n");
    print("  whose key MeshMaster has to be told and whose settings have to be set again.\n");
  } else if (!known) {
    // A unit registered before its repeater: it checks in all the same, with no key and no job
    // types (tasks/phase-4/FOLLOWUPS.md's [04], meshmaster.dk's p4.16-132).
    print("agent id: the radio has no identity yet, so the heartbeat carries no key, no radio "
          "block and no job types - the unit is alive and its repeater is not identified\n");
  }
  printf("agent: version %s, board %s, transport %s\n", v.agent_version, v.board, v.transport);
  if (known) {
    printf("repeater: %s\n", v.public_key);
    printf("  name \"%s\", firmware %s, board %s, built on %s, boot id 0x%08lx\n",
           v.companion_name,
           v.companion_firmware[0] != 0 ? v.companion_firmware : "(none reported)",
           v.companion_board[0] != 0 ? v.companion_board : "?",
           v.companion_upstream[0] != 0 ? v.companion_upstream : "?",
           static_cast<unsigned long>(v.boot_id));
  }
  if (v.radio_known) {
    // The four as the repeater's own console prints them, so that a person can hold this line
    // against `rak cli get radio` with their eyes.
    printf("radio: %.3f MHz, bw %.1f kHz, sf %d, cr %d\n", v.freq_mhz, v.bw_khz, v.sf, v.cr);
  } else {
    printf("radio: not read%s\n",
           known ? " (the repeater's console did not answer 'get radio')" : "");
  }
  // In pieces, through print(): seventeen job types are over three hundred characters and
  // printf's line buffer is two hundred, so this line was cut at `twinoak.` and lost its
  // newline - the line after it then ran on from the middle of a word (task p9-04).
  printf("job types (%d): ", v.job_type_count);
  print(v.job_type_count > 0 ? v.job_types : "none");
  print("\n");
  // The local door (task p9-04): a job for the key above is answered by this unit and nothing
  // goes on the air. `agent jobs` says which of the last ten went that way.
  printf("local door: %s; %lu job(s), %lu request(s), %lu answered with silence, %lu with no request to make\n",
         v.door_open ? "open: a job naming this radio is answered here"
                     : "shut: no key for this radio, so no mesh job is carried at all",
         static_cast<unsigned long>(v.local_jobs), static_cast<unsigned long>(v.local_requests),
         static_cast<unsigned long>(v.local_silent),
         static_cast<unsigned long>(v.local_unserved));
  // Which of the four requests of §5.9 the door answers out of the radio's own handler (link 1.2,
  // task p9-06), read off the session's capability bit. A radio at 1.1 answers none of them, and
  // then a status, a neighbour list and the owner info of this unit's own repeater are *Failed*
  // and its access list silence - which is what kept repeater 41 *stale* on meshmaster.dk.
  printf("  from LOCAL_REQUEST: %s\n",
         v.door_local_request
             ? "status, access list, neighbours, owner info (telemetry stays LPP_GET)"
             : "none - this radio is older than link 1.2, so status, neighbours and owner info "
               "are failed and the access list silent");
  printf("intent: next check-in %lu s, at most %d transaction(s) this wake (band %s)\n",
         static_cast<unsigned long>(v.next_check_in_s), v.max_transactions_this_wake, v.band);
  if (v.time_known) {
    printf("clock: %lu, from the %s\n", static_cast<unsigned long>(v.epoch), v.time_source);
  } else {
    print("clock: this unit does not know what time it is, so the heartbeat's `at` is 0\n");
  }
  printf("heap: %lu free, %lu the least ever\n", static_cast<unsigned long>(v.free_heap),
         static_cast<unsigned long>(v.min_free_heap));
  // The token belongs to the settings and never to the identity: *set* or *unset*, as
  // `get server.token` and `agent url` say it.
  if (settings_ != nullptr) {
    printf("server.token %s\n", settings_->serverToken()[0] != 0 ? "set" : "unset");
  }
  printf("the link said: %lu identity read(s), %lu radio read(s), %lu restart(s) of the radio",
         static_cast<unsigned long>(v.identity_reads), static_cast<unsigned long>(v.radio_reads),
         static_cast<unsigned long>(v.boot_changes));
  if (v.unreadable != 0) {
    printf(", %lu unreadable radio line(s)", static_cast<unsigned long>(v.unreadable));
  }
  if (v.no_identity != 0) {
    printf(", %lu answer(s) with no key", static_cast<unsigned long>(v.no_identity));
  }
  // Of those, the refusals (task p10-03): the number a person reads to tell "this radio has
  // never had a key" from "this radio will not make one".
  if (v.identity_lost != 0) {
    printf(", %lu of them a refusal (ERR_STATE)", static_cast<unsigned long>(v.identity_lost));
  }
  print("\n");
}

void Console::agentHeartbeat() {
  if (!identityOk()) return;
  std::size_t length = 0;
  const char* body = identity_->heartbeat(&length);
  if (body == nullptr) {
    print("error: agent heartbeat: the body did not fit the buffer it is built in\n");
    return;
  }
  printf("agent heartbeat: POST /agent/v1/heartbeat, %lu bytes, built and not sent\n",
         static_cast<unsigned long>(length));
  // The body itself. A heartbeat carries no credential - the token travels in a header (task
  // p4-03) - so it goes to the port and into the ring like any other line of ours.
  print(body);
  print("\n");
}

void Console::agentObs(bool pull) {
  if (!identityOk()) return;
  if (pull) {
    const std::size_t kept = identity_->pull();
    printf("agent obs: the radio's ring drained: %lu record(s) kept\n",
           static_cast<unsigned long>(kept));
  }
  AgentObservationsView& v = obs_view_;
  identity_->observations(&v);
  if (!v.ring) {
    print("agent obs: there is no observation ring in this build: the agent's loop owns it "
          "(task p4-05)\n");
    return;
  }
  printf("agent obs: %lu of %lu waiting, %lu heard, %lu dropped (the heartbeat's figure)\n",
         static_cast<unsigned long>(v.count), static_cast<unsigned long>(v.capacity),
         static_cast<unsigned long>(v.heard), static_cast<unsigned long>(v.dropped));
  printf("  the radio: %lu dropped since it booted, %u still waiting there, %lu pull(s) made",
         static_cast<unsigned long>(v.radio_dropped), static_cast<unsigned>(v.remaining),
         static_cast<unsigned long>(v.pulls));
  if (v.own_key != 0) {
    printf(", %lu about this unit itself (dropped)", static_cast<unsigned long>(v.own_key));
  }
  if (v.unreadable != 0) printf(", %lu unreadable", static_cast<unsigned long>(v.unreadable));
  print("\n");
  if (v.rows == 0) {
    print("  nothing in the ring ('agent obs pull')\n");
    return;
  }
  // The last few, newest first, and **no names**: §7 says neither end logs the name a stranger
  // chose, and every line of this console is kept in the ring `dump` prints.
  print("  the last few, newest first (no names):\n");
  for (std::size_t i = 0; i < v.rows; ++i) {
    const AgentObservationRow& r = v.row[i];
    printf("  %2lu. %s %lu s ago, %u hop(s)", static_cast<unsigned long>(i + 1), r.public_key,
           static_cast<unsigned long>(r.ago_s), static_cast<unsigned>(r.hops));
    if (r.snr_known) {
      // Quarter-decibels as the air carries them, printed as a decimal: -10 is -2.50 dB.
      const int whole = r.snr_q4 / 4;
      const unsigned quarter = static_cast<unsigned>((r.snr_q4 < 0 ? -r.snr_q4 : r.snr_q4) % 4);
      printf(", snr %s%d.%02u dB", r.snr_q4 < 0 && whole == 0 ? "-" : "", whole, quarter * 25u);
    }
    if (r.rssi_known) printf(", rssi %d dBm", r.rssi);
    if (r.role[0] != 0) printf(", %s", r.role);
    if (r.has_position) print(", with a position");
    print("\n");
  }
}

// ---------------------------------------------------------------------------- the agent's loop

bool Console::loopOk() {
  if (loop_ != nullptr) return true;
  print("error: this build has no agent loop: the seam is core/cycle/null_backend.h "
        "(WALTER_AGENT=0, or nothing was given to the console in main/main.cpp)\n");
  return false;
}

void Console::agentWake() {
  // One wake of the loop, as a wake from sleep runs it at step 8 - over the uplink a person
  // has brought up, which is what `agent probe` asks for too. What it prints is the
  // implementation's: the steps as they end, then the report; the token travels in a header
  // and reaches no line of it (task p4-03).
  if (!loopOk() || !uplinkUp()) return;
  print("agent wake: one wake of the loop now (a job can take minutes)\n");
  if (!loop_->wake(out_)) print("agent wake: the loop did not run\n");
}

void Console::agentJobs() {
  if (!loopOk()) return;
  loop_->jobs(out_);
}

void Console::agentState() {
  if (!loopOk()) return;
  loop_->state(out_);
}

void Console::fetch(const char* base_url, image::Which which) {
  if (!httpOk() || !uplinkUp()) return;
  if (fetch_ == nullptr) {
    print("error: fetch: this build has no image store, so nothing can be fetched into it\n");
    return;
  }
  // No base URL on the line: the `fetch.url` setting (task p3-03). The pointer is the settings
  // store's own buffer and outlives this call.
  if (base_url == nullptr || base_url[0] == 0) {
    if (settings_ == nullptr || settings_->fetchUrl()[0] == 0) {
      print("error: fetch: no base URL, and fetch.url is unset: 'fetch <base-url>' or "
            "'set fetch.url https://...'\n");
      return;
    }
    base_url = settings_->fetchUrl();
    printf("fetch: no base URL given; using fetch.url\n");
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

// -- the settings (task p3-03) --------------------------------------------------------------------

bool Console::settingsOk() {
  if (settings_ != nullptr) return true;
  print("error: this build has no settings store: the `nvs` partition is missing or would not "
        "open, and every key falls back (see the boot lines)\n");
  return false;
}

void Console::get(const settings::KeyDef& key) {
  if (!settingsOk()) return;
  char line[settings::kMaxValueLen + 64];
  settings::describeValue(key, settings_->stored(key.id), line, sizeof line);
  printf("%s\n", line);
}

void Console::getAll() {
  if (!settingsOk()) return;
  char line[settings::kMaxValueLen + 64];
  for (const settings::KeyDef& key : settings::kKeys) {
    settings::describeValue(key, settings_->stored(key.id), line, sizeof line);
    printf("%s\n", line);
  }
}

// `set <key> <value>`: the value has already been validated by the parser, so what can still go
// wrong here is the flash. **The value is never echoed back** - the reply is `ok <key>` for every
// key, which is what makes a secret's reply the same as anything else's.
void Console::set(const settings::KeyDef& key, const char* value) {
  if (!settingsOk()) return;
  char why[112] = {};
  if (!settings_->put(key, value, why, sizeof why)) {
    printf("error: %s\n", why[0] != 0 ? why : "the setting could not be stored");
    return;
  }
  printf("ok %s\n", key.name);
  settingChanged(key);
}

void Console::erase() {
  if (!settingsOk()) return;
  char why[112] = {};
  const bool ok = settings_->eraseAll(why, sizeof why);
  if (!ok) printf("error: %s\n", why);
  print("every setting forgotten; the images, the journal, the bad list and the supervisor's "
        "counts are untouched\n");
  // Everything falls back now, and the things that hold a copy of a setting have to be told.
  applySettings();
  printf("the fallbacks are in force: log.level %s, swd %ld kHz, no APN, no PIN, no server\n",
         settings::name(settings_->level()), static_cast<long>(settings_->swdKhz()));
}

// What must happen when a key changes, in one place. The table's `effect` column (keys.cpp) says
// the same thing in words and docs/console.md prints it, so a change here that is not a change
// there is a lie a reader can catch.
void Console::settingChanged(const settings::KeyDef& key) {
  if (settings_ == nullptr) return;
  switch (key.id) {
    case settings::Key::kLteApn:
      if (uplink_ != nullptr) uplink_->apnChanged(settings_->apn());
      print("the APN is used the next time the uplink is brought up ('lte up')\n");
      return;
    case settings::Key::kLtePin:
      if (uplink_ != nullptr) {
        // A different PIN: a refusal recorded against the old one is not about this one
        // (core/uplink/policy.h, *The PIN*).
        const bool was_refused = uplink_->status().pin_refused;
        uplink_->pinChanged(settings_->pin(), settings_->pinEpoch());
        if (was_refused && !uplink_->status().pin_refused) {
          print("a PIN this unit had been refused is forgotten; the next uplink sends the new one\n");
        } else {
          print("the next uplink sends it; a refused PIN is never sent a second time\n");
        }
      }
      return;
    case settings::Key::kLtePsm:
      if (uplink_ != nullptr) {
        uplink_->powerSavingChanged(settings_->psm() ? interfaces::PowerSavingMode::kPsm
                                                     : interfaces::PowerSavingMode::kOff,
                                    uplink::kPsmTauSeconds, uplink::kPsmActiveSeconds);
        printf("power saving is asked for the next time the uplink is brought up (PSM %s, "
               "TAU %lu s, active %lu s)\n", settings_->psm() ? "on" : "off",
               static_cast<unsigned long>(uplink::kPsmTauSeconds),
               static_cast<unsigned long>(uplink::kPsmActiveSeconds));
      }
      return;
    case settings::Key::kServerUrl:
      if (server_ != nullptr) {
        server_->setServer(settings_->serverHost(), settings_->serverPort());
        if (settings_->serverHost()[0] == 0) {
          print("the URL has no host this firmware can reach: no server is configured and a "
                "self-update's third condition does not apply\n");
        } else {
          printf("a self-update's trial now has to reach %s:%u, and the agent's calls go "
                 "there\n", settings_->serverHost(),
                 static_cast<unsigned>(settings_->serverPort()));
        }
      }
      return;
    case settings::Key::kSwdKhz: {
      const std::uint32_t us = swd::setHalfPeriodInForceUs(settings_->swdHalfPeriodUs());
      printf("every SWD command and every flashing from here on runs at %lu kHz (a half-period "
             "of %lu us)\n", static_cast<unsigned long>(500u / us), static_cast<unsigned long>(us));
      return;
    }
    case settings::Key::kLogLevel:
      printf("what is printed when nobody asked is now %s and below; a reply is always printed\n",
             settings::name(settings_->level()));
      return;
    case settings::Key::kWakeInterval:
    case settings::Key::kWakeRadio:
      // Read at the end of a wake, when the interval is chosen and the sources armed (task
      // p3-04). A decision already taken this wake is stale, so the next sleep takes a new one.
      if (duty_ != nullptr) duty_->settingsChanged();
      printf("%s\n", key.effect);
      return;
    case settings::Key::kServerToken:
    case settings::Key::kFetchUrl:
    case settings::Key::kUnitName:
      // Read where they are used and nowhere kept: the token by the agent's uplink,
      // `fetch.url` by the next `fetch`, `unit.name` by `status`. Nothing to tell anybody.
      printf("%s\n", key.effect);
      return;
    case settings::Key::kCount:
      return;
  }
}

// Everything a setting is in force for, at boot and after an `erase confirm`. The same calls
// settingChanged makes, without the lines: nobody typed anything.
void Console::applySettings() {
  if (settings_ == nullptr) return;
  if (!settings_->begun()) settings_->begin();
  swd::setHalfPeriodInForceUs(settings_->swdHalfPeriodUs());
  if (uplink_ != nullptr) {
    uplink_->apnChanged(settings_->apn());
    uplink_->pinChanged(settings_->pin(), settings_->pinEpoch());
    uplink_->powerSavingChanged(settings_->psm() ? interfaces::PowerSavingMode::kPsm
                                                 : interfaces::PowerSavingMode::kOff,
                                uplink::kPsmTauSeconds, uplink::kPsmActiveSeconds);
  }
  if (server_ != nullptr) server_->setServer(settings_->serverHost(), settings_->serverPort());
  if (duty_ != nullptr) duty_->settingsChanged();
}

// -- sleep and the battery (task p3-04) ----------------------------------------------------------

bool Console::dutyOk() {
  if (duty_ != nullptr) return true;
  print("error: this build has no duty cycle: nothing here can sleep, read the battery's band "
        "or say what woke it\n");
  return false;
}

// `wake`: why this Walter is awake, the last ten with their reasons and durations, the spurious
// count, and whether the radio's line is armed as a wake source.
void Console::wake() {
  if (!dutyOk()) return;
  const cycle::WakeInfo& w = duty_->wake();
  printf("this wake: %s\n", cycle::describe(w.wake));
  if (w.wake == cycle::Wake::kAttention) {
    printf("  attention: reason %u (%s), detail %u, repeat %u; heard after %lu ms of the %lu ms "
           "window\n", static_cast<unsigned>(w.attention.reason),
           link::client::attentionName(w.attention.reason),
           static_cast<unsigned>(w.attention.detail),
           static_cast<unsigned>(w.attention.repeat),
           static_cast<unsigned long>(w.listened_ms),
           static_cast<unsigned long>(cycle::kAttentionWindowMs));
  } else if (w.wake == cycle::Wake::kSpurious) {
    printf("  nothing valid in %lu ms of listening: %lu spurious wake(s) since the last timer "
           "wake, %lu in all\n", static_cast<unsigned long>(w.listened_ms),
           static_cast<unsigned long>(w.spurious_in_window),
           static_cast<unsigned long>(w.spurious_total));
  }
  if (w.radio_may_be_booting) {
    print("  our own power-up reset the radio: it is still booting and is not to be judged yet\n");
  }
  printf("the radio's line: %s%s\n",
         duty_->lineWillArm() ? "armed for the next sleep" : "not armed for the next sleep",
         w.line_disarmed ? " (disarmed by spurious wakes; the next timer wake arms it again)"
                         : "");
  if (w.line_just_disarmed) {
    printf("  %lu spurious wake(s) reached the limit of %u: the line is disarmed until the next "
           "timer wake\n", static_cast<unsigned long>(w.spurious_in_window),
           static_cast<unsigned>(cycle::kSpuriousPerWindow));
  }
  if (w.line_rearmed) print("  this timer wake armed the line again\n");
  if (w.faults_in_a_row != 0) {
    printf("faults in a row: %lu (a crash or a watchdog lengthens the next sleep, it does not "
           "shorten it)\n", static_cast<unsigned long>(w.faults_in_a_row));
  }
  // Staying awake: a USB host, or `stay`.
  const cycle::GuardStatus& g = duty_->guard().status();
  printf("awake %lu s: %s\n", static_cast<unsigned long>(g.awake_ms / 1000u),
         cycle::describe(g.state));
  if (g.state == cycle::Guard::kArmed) {
    printf("  %lu s of the %lu s bound left; after it the wake is ended by a reset\n",
           static_cast<unsigned long>(g.left_ms / 1000u),
           static_cast<unsigned long>(cycle::kWakeBoundMs / 1000u));
  }
  if (g.staying) {
    printf("  'stay' is on: %lu s left of it ('stay off' ends it)\n",
           static_cast<unsigned long>(g.stay_left_ms / 1000u));
  }
  printf("wakes counted: %lu%s\n", static_cast<unsigned long>(w.wakes),
         duty_->reason().recordSurvived()
             ? ""
             : " (the wake log starts at this wake: a power cut, or the first boot)");
  for (std::size_t i = 0; i < duty_->reason().count(); ++i) {
    const cycle::WakeEntry& e = duty_->reason().at(i);
    printf("  %2lu. %-9s", static_cast<unsigned long>(i + 1), cycle::name(e.wake));
    if (e.attention != 0) {
      printf(" attention %u (%s), detail %u", static_cast<unsigned>(e.attention),
             link::client::attentionName(e.attention), static_cast<unsigned>(e.detail));
    }
    if (i == 0) {
      print(" - this wake");
    } else if (e.lasted_s == 0) {
      print("; it never reached sleep (a reset, a crash or the bound)");
    } else {
      printf("; awake %lu s", static_cast<unsigned long>(e.lasted_s));
    }
    print("\n");
  }
}

// `power`: the band, the readings behind it, what a wake in that band may do, and the interval.
// It reads the battery over the link, which costs one POWER_GET.
void Console::power() {
  if (!dutyOk()) return;
  const bool had = duty_->readPower();
  const power::Decision d = duty_->decide();
  printf("band: %s - %s\n", power::name(d.band), power::describe(d.band));
  printf("  because: %s%s\n", d.why, d.changed ? "; the band changed this wake" : "");
  if (!had) print("  this wake's POWER_GET did not answer (the link is down, or channel 2 is not valid)\n");
  if (d.reading_known) {
    printf("reading: %lu mV (the median of %lu sample(s))", static_cast<unsigned long>(d.median_mv),
           static_cast<unsigned long>(d.samples < power::kMedianOf ? d.samples : power::kMedianOf));
    if (d.current_known) {
      printf(", %ld mA (%s, the mean of what we have)", static_cast<long>(d.mean_ma),
             d.mean_ma >= 0 ? "charging" : "discharging");
    }
    if (d.from_radio) print(" - from the radio's own attention, not from a reading of ours");
    print("\n");
  } else {
    print("reading: none\n");
  }
  for (std::size_t i = 0; i < duty_->battery().sampleCount(); ++i) {
    const power::Sample& s = duty_->battery().sampleAt(i);
    printf("  %2lu. %lu mV", static_cast<unsigned long>(i + 1),
           static_cast<unsigned long>(s.millivolts));
    if (s.current_known) printf(", %ld mA", static_cast<long>(s.current_ma));
    print(i == 0 ? " - the newest\n" : "\n");
  }
  printf("thresholds (PLACEHOLDERS, docs/power.md): upper %u mV, lower %u mV, floor %u mV, "
         "hysteresis %u mV\n", static_cast<unsigned>(power::kUpperMv),
         static_cast<unsigned>(power::kLowerMv), static_cast<unsigned>(power::kFloorMv),
         static_cast<unsigned>(power::kHysteresisMv));
  printf("this wake may: modem %s, update %s, flash %s, power-cycle the radio %s\n",
         d.modem_this_wake ? "yes" : "no", d.may.update ? "yes" : "no",
         d.may.flash ? "yes" : "no", d.may.power_cycle ? "yes" : "no");
  if (d.band == power::Band::kSaving && !d.modem_this_wake) {
    printf("  the modem is used every %lu wake(s) in this band: %lu to go\n",
           static_cast<unsigned long>(power::kSavingModemEveryNth),
           static_cast<unsigned long>(d.wakes_until_modem));
  }
  printf("next wake: %lu min (%lu s)", static_cast<unsigned long>(d.interval_minutes),
         static_cast<unsigned long>(d.interval_seconds));
  if (d.overridden) {
    printf(", from wake.interval and not the band's %lu min",
           static_cast<unsigned long>(d.band_minutes));
  }
  if (d.fault_doublings != 0) {
    printf(", doubled %lu time(s) by faults in a row",
           static_cast<unsigned long>(d.fault_doublings));
  }
  print("\n");
  const cycle::SleepRefusal would = duty_->sleeper().wouldRefuse();
  if (would != cycle::SleepRefusal::kNone) {
    printf("a sleep now would be refused: %s\n", cycle::describe(would));
  }
}

// The band a sleep goes to sleep in. `sleep now` is "for the interval the battery chose", so it
// asks the battery when this wake has not already - one POWER_GET on the link - rather than
// sleeping in the band of "no reading". `power` leaves a decision behind and this reuses it.
void Console::decideIfNeeded() {
  if (duty_->decided()) return;
  (void)duty_->readPower();
  (void)duty_->decide();
}

void Console::sleepNow() {
  if (!dutyOk()) return;
  decideIfNeeded();
  print("sleeping: the journal, SLEEP_NOTICE to the radio, the modem down, the pins safe and "
        "held, then deep sleep\n");
  printSleepResult(duty_->sleepNow(true));
}

void Console::sleepFor(std::uint32_t seconds) {
  if (!dutyOk()) return;
  decideIfNeeded();
  printf("sleeping for %lu s\n", static_cast<unsigned long>(seconds));
  printSleepResult(duty_->sleepFor(seconds));
}

// What a sleep did. **On a board nothing below is printed**: the sleep does not return. It is
// what a host test reads, and what a person sees when a sleep was refused.
void Console::printSleepResult(const cycle::SleepResult& r) {
  if (r.refusal != cycle::SleepRefusal::kNone) {
    printf("error: sleep refused: %s\n", cycle::describe(r.refusal));
    return;
  }
  printf("asleep: %lu s, band %s, the radio's line %s; SLEEP_NOTICE %s; the pads %s\n",
         static_cast<unsigned long>(r.seconds), power::name(r.band),
         r.radio_line_armed ? "armed" : "not armed",
         r.notice_sent ? "answered" : link::client::describe(r.notice_fault),
         r.pins_held ? "held in the safe state" : "NOT held (a pin driver refused)");
  if (r.epoch != 0) {
    printf("  expected back at epoch %lu\n", static_cast<unsigned long>(r.epoch));
  }
  if (r.journalled) print("  written down in the journal\n");
}

// `stay` / `stay off`: awake on purpose, without a cable, for the bench.
void Console::stay(bool on) {
  if (!dutyOk()) return;
  duty_->guard().stayAwake(on);
  if (on) {
    printf("stay: this Walter will not be put to sleep by the bound on a wake for the next "
           "%lu s; 'stay off' ends it\n",
           static_cast<unsigned long>(cycle::kStayBoundMs / 1000u));
  } else {
    print("stay off: the bound on a wake applies again (a wake that does not reach sleep in "
          "five minutes is ended by a reset)\n");
  }
}

// -- dump and journal (task p3-03) ----------------------------------------------------------------

// The ring, oldest first. Printed straight to the port and **not** through print(): a dump of
// the log is not itself a line of the log, and printing it through the ring would copy the ring
// into itself.
void Console::dump() {
  printf("dump: %lu of %lu bytes kept%s\n", static_cast<unsigned long>(log_.kept()),
         static_cast<unsigned long>(LogRing::kBytes),
         log_.wrapped() ? "; the ring has wrapped, so the log begins at the first whole line" : "");
  log_.writeTo(out_);
  printf("dump: end (%lu byte(s) dropped since this boot)\n",
         static_cast<unsigned long>(log_.dropped()));
}

// The flash journal's last entries, whatever wrote them: the radio's updates (task p2-03), this
// firmware's own (p2-04) and the supervisor's ladder (p2-05). `rak update status` tells the story
// of one run; this is the log.
void Console::journal() {
  if (journal_ == nullptr) {
    print("error: journal: this build has no `journal` partition (see the boot lines)\n");
    return;
  }
  printf("journal: %lu entries in %lu sectors, next seq %lu, head sector %lu slot %lu, %lu "
         "sector erase(s) this boot\n",
         static_cast<unsigned long>(journal_->validEntries()),
         static_cast<unsigned long>(journal_->sectors()),
         static_cast<unsigned long>(journal_->nextSeq()),
         static_cast<unsigned long>(journal_->headSector()),
         static_cast<unsigned long>(journal_->headSlot()),
         static_cast<unsigned long>(journal_->erases()));
  if (journal_->isProtected()) {
    print("journal: an operation's entries are protected from a wrap: something is in progress\n");
  }
  if (journal_->empty()) {
    print("journal: empty: nothing has been journalled on this unit\n");
    return;
  }
  // The last few, newest first. Bounded: a console line at a time, no array.
  constexpr std::size_t kShow = 12;
  print("the last entries, newest first:\n");
  update::Cursor cursor;
  update::Entry entry;
  std::size_t number = 0;
  bool more = journal_->first(&cursor, &entry);
  while (more && number < kShow) {
    printJournalEntry(++number, entry);
    more = journal_->previous(&cursor, &entry);
  }
  if (more) printf("  ... and more before these ('rak update status', 'self status')\n");
}

void Console::printJournalEntry(std::size_t number, const update::Entry& entry) {
  switch (entry.kind) {
    case update::kind::kRadioUpdate: {
      update::Payload p;
      if (!update::decode(entry, &p)) break;
      printUpdateEntry(number, entry, p);
      return;
    }
    case update::kind::kSelfUpdate: {
      update::SelfPayload p;
      if (!update::decode(entry, &p)) break;
      printSelfEntry(number, entry, p);
      return;
    }
    case update::kind::kSupervisor: {
      supervisor::Payload p;
      if (!supervisor::decode(entry, &p)) break;
      printSupervisorEntry(number, entry, p);
      return;
    }
    case update::kind::kDutyCycle: {
      cycle::Payload p;
      if (!cycle::decode(entry, &p)) break;
      printSleepEntry(number, entry, p);
      return;
    }
    case update::kind::kWakeCycle: {
      cycle::WakePayload p;
      if (!cycle::decode(entry, &p)) break;
      printWakeEntry(number, entry, p);
      return;
    }
    case update::kind::kAgentAttention: {
      cycle::AgentPayload p;
      if (!cycle::decode(entry, &p)) break;
      printf("  %2lu. seq %lu: agent, %s", static_cast<unsigned long>(number),
             static_cast<unsigned long>(entry.seq), cycle::describe(p.what));
      if (p.count != 0) printf(" (%u since the record began)", static_cast<unsigned>(p.count));
      if (p.why[0] != 0) printf(": %s", p.why);
      print("\n");
      return;
    }
    default: break;
  }
  printf("  %2lu. seq %lu: kind 0x%04x, %u byte(s) - nothing in this build reads it\n",
         static_cast<unsigned long>(number), static_cast<unsigned long>(entry.seq),
         static_cast<unsigned>(entry.kind), static_cast<unsigned>(entry.size));
}

void Console::printSupervisorEntry(std::size_t number, const update::Entry& entry,
                                   const supervisor::Payload& p) {
  printf("  %2lu. seq %lu: supervisor, %s", static_cast<unsigned long>(number),
         static_cast<unsigned long>(entry.seq), supervisor::describe(p.what));
  if (p.cause != supervisor::Cause::kNone) printf(": %s", supervisor::describe(p.cause));
  if (p.rung != supervisor::Rung::kNone) printf(" (rung: %s)", supervisor::describe(p.rung));
  printf("; today %lu resets, %lu power cycles, %lu flashes",
         static_cast<unsigned long>(p.counts.resets),
         static_cast<unsigned long>(p.counts.power_cycles),
         static_cast<unsigned long>(p.counts.flashes));
  if ((p.flags & supervisor::kFlagByHand) != 0) print("; a person's command");
  if ((p.flags & supervisor::kFlagResumed) != 0) print("; after a restart");
  print("\n");
}

// A duty-cycle entry (task p3-04): a sleep worth a line, a refusal, or the radio's line armed or
// disarmed as a wake source.
void Console::printSleepEntry(std::size_t number, const update::Entry& entry,
                              const cycle::Payload& p) {
  printf("  %2lu. seq %lu: duty cycle, %s", static_cast<unsigned long>(number),
         static_cast<unsigned long>(entry.seq), cycle::describe(p.what));
  if (p.what == cycle::Wrote::kRefused) {
    printf(": %s", cycle::describe(p.refusal));
  } else if (p.what == cycle::Wrote::kSleeping) {
    printf(" for %lu s, band %s", static_cast<unsigned long>(p.interval_s),
           power::name(p.band));
  }
  printf("; woke by %s, awake %lu s", cycle::name(p.wake), static_cast<unsigned long>(p.awake_s));
  if (p.median_mv != 0) {
    printf("; %lu mV", static_cast<unsigned long>(p.median_mv));
    if ((p.flags & cycle::kFlagCharging) != 0) printf(", charging at %ld mA",
                                                      static_cast<long>(p.mean_ma));
  }
  if ((p.flags & cycle::kFlagBandChanged) != 0) print("; the band changed");
  if ((p.flags & cycle::kFlagOverridden) != 0) print("; wake.interval");
  if ((p.flags & cycle::kFlagByHand) != 0) print("; a person's command");
  print("\n");
}

// -- one whole wake (task p3-05) ----------------------------------------------------------------

bool Console::cycleOk() {
  if (cycle_ != nullptr) return true;
  print("error: this build has no wake cycle\n");
  return false;
}

void Console::cycleNow() {
  if (!cycleOk()) return;
  print("one wake, the ten steps of docs/cycle.md in order; it ends in sleep and does not come "
        "back\n");
  printPass(cycle_->run());
}

void Console::cycleDry() {
  if (!cycleOk()) return;
  print("what a wake would do, doing none of it - nothing is sent and nothing is written\n");
  printPass(cycle_->dry());
}

void Console::printPass(const cycle::Pass& pass) {
  for (std::size_t i = 0; i < cycle::kStepCount; ++i) {
    const cycle::Step step = static_cast<cycle::Step>(i);
    const cycle::StepRecord& r = pass.step[i];
    printf("  %2u %-8s %c %-26s %s", static_cast<unsigned>(i + 1), cycle::name(step),
           cycle::letter(r.outcome), cycle::describe(r.outcome), r.why);
    if (r.spent_ms != 0) printf(" (%lu ms)", static_cast<unsigned long>(r.spent_ms));
    if (pass.dry && r.deadline_ms != 0) {
      printf(" [up to %lu ms]", static_cast<unsigned long>(r.deadline_ms));
    }
    print("\n");
  }
  char line[224] = {};
  (void)cycle::recordLine(pass, line, sizeof line);
  printf("%s\n", line);
  printf("the wake: %s; the budget %lu ms, %lu left, %lu outside it; the bound %lu ms\n",
         cycle::describe(pass.ending), static_cast<unsigned long>(pass.budget_ms),
         static_cast<unsigned long>(pass.left_ms), static_cast<unsigned long>(pass.outside_ms),
         static_cast<unsigned long>(pass.bound_ms));
  // The seam, and what it is: a person who types `cycle dry` on a unit that reports to nobody
  // should be told so, in words (spec/SYSTEM.md §8).
  printf("the backend: %s; %lu reading(s) offered%s\n", interfaces::describe(pass.exchanged),
         static_cast<unsigned long>(pass.readings_offered),
         pass.readings_taken ? " and taken" : "");
  if (pass.asks != 0) {
    print("it asked for:");
    for (std::uint8_t a = 1; a < static_cast<std::uint8_t>(interfaces::Ask::kCount); ++a) {
      const interfaces::Ask ask = static_cast<interfaces::Ask>(a);
      if ((pass.asks & interfaces::askBit(ask)) != 0) printf(" %s;", interfaces::describe(ask));
    }
    print("\n");
  }
  if (pass.carried != interfaces::Ask::kNothing) {
    printf("it carried out: %s\n", interfaces::describe(pass.carried));
  }
}

void Console::printWakeEntry(std::size_t number, const update::Entry& entry,
                             const cycle::WakePayload& p) {
  char letters[16] = {};
  for (std::size_t i = 0; i < cycle::kStepCount && i + 1 < sizeof letters; ++i) {
    letters[i] = cycle::letter(p.step[i]);
  }
  printf("  %2lu. seq %lu: wake %lu, %s: radio %s, band %s, steps %s, awake %lu s, next %lu min",
         static_cast<unsigned long>(number), static_cast<unsigned long>(entry.seq),
         static_cast<unsigned long>(p.wakes), cycle::name(p.wake),
         supervisor::describe(p.radio), power::name(p.band), letters,
         static_cast<unsigned long>(p.awake_s), static_cast<unsigned long>(p.interval_s / 60u));
  if ((p.flags & cycle::kWakeFlagBandChanged) != 0) print("; the band changed");
  if ((p.flags & cycle::kWakeFlagClimbed) != 0) print("; the ladder was climbed");
  if ((p.flags & cycle::kWakeFlagOnTrial) != 0) print("; this firmware was on trial");
  if ((p.flags & cycle::kWakeFlagClockSet) != 0) print("; the radio's clock was set");
  if ((p.flags & cycle::kWakeFlagExchanged) != 0) print("; the backend was reached");
  if ((p.flags & cycle::kWakeFlagReadingsTaken) != 0) print("; the readings were taken");
  if ((p.flags & cycle::kWakeFlagShortened) != 0) print("; the sleep was shortened");
  if (p.carried != interfaces::Ask::kNothing) printf("; %s", interfaces::describe(p.carried));
  print("\n");
}

// ------------------------------------------------------------------------ the mesh client (p4-02)
//
// One status request to a neighbour through the seam of core/console/mesh_probe.h: the job, the
// send, the wait, the reply's numbers and `client_tx` before and after - which is the bench's
// whole item (one packet on the air, one reply, client_tx up by one). The wait is what the agent's
// runner gives a flood at the claim's defaults, 4 x 4000 ms (docs/library.md §3.3), so the
// console is busy for up to sixteen seconds on silence.
constexpr std::uint32_t kMeshProbeWaitMs = 16000;

void Console::rakMeshStatus(const char* key) {
  if (mesh_ == nullptr) {
    print("error: rak mesh status: this build has no mesh client\n");
    return;
  }
  printf("mesh status %.16s...: one status request over the mesh client (up to %lu ms)\n", key,
         static_cast<unsigned long>(kMeshProbeWaitMs));
  MeshProbeResult r;
  mesh_->status(key, kMeshProbeWaitMs, &r);
  switch (r.outcome) {
    case MeshProbeResult::Outcome::kBadKey:
      print("error: rak mesh status: the key is not 64 hex characters\n");
      return;
    case MeshProbeResult::Outcome::kNotBegun:
      linkError("rak mesh status: JOB_BEGIN", r.fault, r.status);
      return;
    case MeshProbeResult::Outcome::kNotSent:
      linkError("rak mesh status: SEND_REQUEST", r.fault, r.status);
      return;
    case MeshProbeResult::Outcome::kNoReply:
      printf("  sent as a %s, tag 0x%08lx; no reply: the record says %s after %lu ms\n",
             r.flooded ? "flood" : "direct packet", static_cast<unsigned long>(r.tag),
             lc::replyStateName(r.state), static_cast<unsigned long>(r.elapsed_ms));
      if (r.fault != lc::Fault::kNone) linkError("rak mesh status: the wait", r.fault, r.status);
      break;
    case MeshProbeResult::Outcome::kUnreadable:
      printf("  sent as a %s, tag 0x%08lx; reply after %lu ms (%s, snr %d/4 dB, rssi %d dBm): "
             "not a status reply this firmware reads (%u bytes)\n",
             r.flooded ? "flood" : "direct packet", static_cast<unsigned long>(r.tag),
             static_cast<unsigned long>(r.elapsed_ms), r.via_path ? "via a returned path" : "direct",
             static_cast<int>(r.snr_q4), static_cast<int>(r.rssi), static_cast<unsigned>(r.reply_len));
      break;
    case MeshProbeResult::Outcome::kOk:
      printf("  sent as a %s, tag 0x%08lx; reply after %lu ms (%s, snr %d/4 dB, rssi %d dBm)\n",
             r.flooded ? "flood" : "direct packet", static_cast<unsigned long>(r.tag),
             static_cast<unsigned long>(r.elapsed_ms), r.via_path ? "via a returned path" : "direct",
             static_cast<int>(r.snr_q4), static_cast<int>(r.rssi));
      printf("  the repeater: battery %d mV, uptime %lu s, noise floor %d dBm, last rssi %d dBm, "
             "last snr %d/4 dB, packets rx %lu tx %lu, airtime %lu s, tx queue %d\n",
             r.battery_mv, static_cast<unsigned long>(r.uptime_s), r.noise_floor_dbm, r.last_rssi,
             r.last_snr_q4, static_cast<unsigned long>(r.packets_recv),
             static_cast<unsigned long>(r.packets_sent), static_cast<unsigned long>(r.airtime_s),
             r.tx_queue_len);
      break;
  }
  if (r.counters_known) {
    printf("  client_tx %lu -> %lu (%s%ld)\n", static_cast<unsigned long>(r.client_tx_before),
           static_cast<unsigned long>(r.client_tx_after),
           r.client_tx_after >= r.client_tx_before ? "+" : "",
           static_cast<long>(r.client_tx_after) - static_cast<long>(r.client_tx_before));
  } else {
    print("  client_tx: MESH_COUNTERS did not answer\n");
  }
}

}  // namespace console
}  // namespace core
