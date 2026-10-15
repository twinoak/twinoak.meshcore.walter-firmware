// The console (task p0-03): bytes from the module's USB port in, lines parsed (parser.h), the
// commands run against RadioPins, Health and - from task p1-02 - the radio over the box link, text
// out. Portable: the platform gives it the bytes and a TextOut; host_test/ gives it fake pins, a
// scripted radio and a string, and runs a few thousand random command sequences through it to show
// that no sequence can break a pin rule.
//
// Behaviour as the agent firmware's console: a `>` prompt, typed characters echoed (so leave
// local echo off), backspace honoured, CR, LF and CR LF all end a line, a refusal is one line
// beginning `error:` that names what was wrong. docs/console.md describes it.
//
// Task p3-03 made it one thing. One grammar (parser.h), one table of commands with one line of
// help each (commands.h), one document held to that table by a host test, one settings store
// (core/settings/settings.h) whose secrets no command prints, and one ring of the last few
// kilobytes this port said (log_ring.h) that `dump` prints. **Everything the console prints goes
// through print() or printf()** - there is no third way out - which is what makes the ring whole
// and what lets one test grep every line of every command for a secret.
#pragma once

#include <cstddef>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/commands.h"
#include "core/console/log_ring.h"
#include "core/console/parser.h"
#include "core/settings/settings.h"
#include "core/flasher/flasher.h"
#include "core/health/health.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/net/client.h"
#include "core/net/trust.h"
#include "core/supervisor/supervisor.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/fetch.h"
#include "core/update/self_update.h"
#include "core/uplink/policy.h"
#include "interfaces/clock.h"
#include "interfaces/restart.h"
#include "interfaces/text_out.h"

namespace core {
namespace console {

class Console {
 public:
  // The image store and the USB transfer (task p2-02) are pointers and not references because a
  // build can be without them: platform::FlashPartition::begin() fails when the partition table
  // has no `rakfw`, and main/ then hands over nothing rather than a store over a region that is
  // not there. The `image ...` commands say so in one line instead of pretending. The radio's
  // update and its journal (task p2-03) are the same: no `journal` partition, no update; and the
  // Walter's own update (task p2-04) needs both the store (its bad list) and the journal.
  Console(interfaces::TextOut& out, board::RadioPins& pins, health::Health& health,
          interfaces::Clock& clock, interfaces::Restart& restart, link::client::Client& radio,
          board::SlotLock& slots, image::Store* images = nullptr,
          image::Receiver* receiver = nullptr, update::RadioUpdate* update = nullptr,
          update::Journal* journal = nullptr, update::SelfUpdate* self = nullptr,
          supervisor::Supervisor* supervisor = nullptr,
          uplink::UplinkPolicy* uplink = nullptr,
          // The HTTPS client (task p3-02): the client for `http get`, the fetch of a release
          // for `fetch`, the trust loader for `trust tls`. A build without a store has no fetch.
          net::Client* http = nullptr, update::Fetch* fetch = nullptr,
          net::TrustLoader* trust = nullptr,
          // The settings (task p3-03) and the server a self-update's trial must reach, which is
          // what `server.url` sets. Both pointers: a build whose `nvs` partition would not open
          // has no store, and `get` and `set` then say so in one line instead of pretending that
          // what was typed was kept.
          settings::Settings* settings = nullptr,
          update::UplinkCheck* server = nullptr);

  // How this boot happened, and the first prompt (the banner is printed by main before this).
  // Also where an update the journal says was in progress is taken up again (task p2-03):
  // resume() here, and tick() then drives it a step at a time, printing each transition. And
  // where the Walter's own update learns what this boot is (task p2-04): a trial, which tick()
  // then runs, or a rollback to record. And where the supervisor reads its retained record and
  // the journal (task p2-05), saying so when a stop is in force or the day's counts are not
  // empty - a unit found quiet should say what it has been doing before anybody asks.
  void begin();

  // One byte from the port. While a transfer is running (`image receive`) the bytes go to it
  // and not to the line editor: there is no echo and no prompt until it is over.
  void feed(char c);

  // Called when no byte arrived: a transfer whose other end has gone away is ended here, so that
  // a Walter whose cable was pulled mid-transfer does not sit mute for ever. The first call is
  // the main loop turning - the trial's first condition - and every call steps a trial.
  void tick();

  // One whole line, as if typed and ended. What feed() calls; tests call it directly.
  void runLine(const char* line);

 private:
  // The slot a `rak ...` line is about (task p1-06), resolved once in runLine: an optional `@N`
  // after the word `rak`, `@0` without one. slotOk() is where a slot this adapter does not have
  // becomes one line of refusal; the selection itself is taken by whoever faces the radio - the
  // link client around each exchange, a SlotHold around a pin or an SWD command, the flasher
  // inside its own call.
  bool slotOk(board::Slot slot);

  void prompt();
  // The two ways anything of this console reaches the port, and the only two: both keep what
  // they printed in the ring that `dump` prints (log_ring.h).
  void print(const char* text);
  void printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
  // A line nobody asked for - a boot's own lines, a resumed update's transitions, a trial's
  // steps - printed only at `log.level` and below (task p3-03). A reply and a refusal are never
  // gated: a person who typed a command gets an answer whatever the level says.
  bool says(settings::Level level) const;
  void help();
  void helpLine(const char* usage, const char* text);
  void status();
  void reboots();
  void pins();
  void rakReset();
  void rakKill(std::uint32_t seconds);
  void reboot();

  // The link (task p1-02). Each prints the decoded answer in words, or why there is none.
  void linkError(const char* what, link::client::Fault fault, std::uint8_t status);
  void rakHello();
  void rakPing();
  void rakStatus();
  void rakId();
  void rakTime();
  void rakTimeSet(std::uint32_t epoch);
  void rakPower();
  void rakEnv();
  void rakCli(const char* line, std::size_t size);
  void rakEvents();
  void rakText();
  void printEvent(const link::client::QueuedEvent& event, std::size_t number);
  void printBytes(const char* label, link::Bytes bytes);
  void printText(link::Text text);

  // The debug port (task p1-03).
  void rakSwdConnect();

  // The nRF52 behind the debug port (task p1-04): a session opened and closed inside the command,
  // the pins released on every exit path.
  void rakSwdId();
  void rakSwdMem(std::uint32_t address, std::uint32_t words);

  // The flasher (task p1-05): the bench's proof of erase, write and verify on real silicon.
  void rakFlashTest();
  void printFlashResult(const char* what, const flasher::Result& r);

  // The trust table (task p2-01): which release keys this build accepts, or that it accepts none.
  void trust();

  // The image store (task p2-02).
  bool imagesOk();  // false, with one error line, when this build has no store
  void imageList();
  void imageOneLine(const image::Record& record);
  void imageVerify(image::Which which);
  void imageErase(image::Which which);
  void imageBad();
  void imageReceive(image::Which which);
  void transferEnded();

  // The radio's update (task p2-03).
  bool updateOk();  // false, with one error line, when this build has no update machine
  void rakUpdate(update::Kind kind);
  void rakUpdateStatus();
  void driveUpdate();  // step() until terminal, printing each transition
  void printUpdateStep(const update::StepResult& s);
  void printUpdateOutcome();
  void printUpdateEntry(std::size_t number, const update::Entry& entry, const update::Payload& p);

  // The Walter's own update (task p2-04).
  bool selfOk();  // false, with one error line, when this build has no self-update
  void selfStatus();
  void selfReceive();
  void selfSwitch(bool radio_down);
  void selfConfirm();
  void selfReject();
  void printSelfBoot(update::SelfBoot boot);
  void printSelfStep(const update::SelfStep& s);
  void printSelfEntry(std::size_t number, const update::Entry& entry, const update::SelfPayload& p);
  void printSlot(const char* label, const interfaces::OwnSlot& slot);

  // The supervisor (task p2-05).
  bool watchOk();  // false, with one error line, when this build has no supervisor
  void rakWatch();
  void rakWatchResetCaps();
  void rakLadder();
  void driveLadder();  // step() until the climb is over, printing each rung
  void printLadderStep(const supervisor::Step& s);
  void printIncident(std::size_t number, const supervisor::Incident& incident);
  void printRungsTried(std::uint8_t rungs);

  // The uplink (task p3-01). `lte` reads and nothing more; the other four act.
  bool lteOk();  // false, with one error line, when this build has no uplink
  void lte();
  void lteUp();
  void lteDown();
  void lteTime();
  void lteId();
  void printSignal(const interfaces::Signal& signal);
  void printPowerSaving(const interfaces::PowerSavingGrant& grant);
  void printNextTry();

  // The HTTPS client (task p3-02).
  bool httpOk();    // false, with one error line, when this build has no client
  bool uplinkUp();  // false, with one error line, when the uplink is not up ('lte up' first)
  void httpGet(const char* url);
  void fetch(const char* base_url, image::Which which);
  void trustTls();

  // The settings (task p3-03).
  bool settingsOk();  // false, with one error line, when this build has no settings store
  void get(const settings::KeyDef& key);
  void getAll();
  void set(const settings::KeyDef& key, const char* value);
  void erase();
  // What must happen when a key changes, and the one place it happens: the console is what holds
  // the uplink policy, the server seam and the SWD clock, so it is what tells them. The table's
  // `effect` column says the same thing in words, for `help` and docs/console.md.
  void settingChanged(const settings::KeyDef& key);
  // Everything the settings are in force for at boot, before a person changes anything.
  void applySettings();
  void dump();
  void journal();
  void printJournalEntry(std::size_t number, const update::Entry& entry);
  void printSupervisorEntry(std::size_t number, const update::Entry& entry,
                            const supervisor::Payload& p);

  interfaces::TextOut& out_;
  board::RadioPins& pins_;
  health::Health& health_;
  interfaces::Clock& clock_;
  interfaces::Restart& restart_;
  link::client::Client& radio_;
  board::SlotLock& slots_;
  image::Store* images_ = nullptr;
  image::Receiver* receiver_ = nullptr;
  update::RadioUpdate* update_ = nullptr;
  update::Journal* journal_ = nullptr;
  update::SelfUpdate* self_ = nullptr;
  supervisor::Supervisor* supervisor_ = nullptr;
  uplink::UplinkPolicy* uplink_ = nullptr;
  net::Client* http_ = nullptr;
  update::Fetch* fetch_ = nullptr;
  net::TrustLoader* trust_ = nullptr;
  settings::Settings* settings_ = nullptr;
  update::UplinkCheck* server_ = nullptr;
  bool driving_update_ = false;  // `rak update` is running the machine to its end
  bool ticked_ = false;          // the main loop has turned once: the trial's first condition
  board::Slot slot_{};  // the slot the line being run is about
  // A member, not a local: it carries three page buffers (12 KB), more than the main task's stack
  // (CONFIG_ESP_MAIN_TASK_STACK_SIZE 3584) would hold; the console itself has static storage.
  flasher::Flasher flasher_;
  // The last few kilobytes this port printed, for `dump`. A member for the same reason the
  // flasher is one: 4 KB does not belong on the main task's 3584-byte stack.
  LogRing log_;
  char line_[kMaxLineLength + 8] = {};
  std::size_t length_ = 0;
  bool last_was_cr_ = false;
  // While a secret's value is being typed the echo is asterisks (the agent firmware's
  // `secretValueIsBeingTyped`): the line so far begins `set <a secret key> `.
  bool secretBeingTyped() const;
};

}  // namespace console
}  // namespace core
