// The console's line parser: one line in, one command or one refusal out (task p0-03). A pure
// function, after the agent firmware's src/console/Parser.h, so that every line shape and every
// refusal has a host test. What a command does is console.h; the list of commands with one line
// of help each - the one `help` prints and the one `docs/console.md` is held to - is
// core/console/commands.h.
//
// **Every line of this console is parsed here and nowhere else.** The commands came a task at a
// time from phase 0 onwards; task p3-03 made them one grammar, one table and one document, and
// host_test/core/console/test_console_grammar.cpp holds all three to each other and reads the
// sources to show that parseLine has exactly one caller.
//
// The grammar (one line per command, a `>` prompt; docs/console.md):
//
//   help                  the commands
//   status                one screen: this firmware, the radio, the battery, the images, the
//                         uplink, the next wake
//   reboots               why this board last restarted, and the four before that
//   pins                  the seven pins: net, D-sub, GPIO, mode, level
//   rak reset             one reset pulse on nRESET (waits out the spacing)
//   rak kill <seconds> [confirm]
//                         the LoRa rail off for that long, then back; over
//                         kKillSecondsWithoutWord it wants the word
//   reboot                restart the Walter
//
// and the settings (task p3-03; core/settings/keys.h is the table of keys):
//
//   get                   every setting, in the table's order
//   get <key>             one setting; a secret prints `set` or `unset`, never its value
//   set <key> <value>     store one; the value is the rest of the line, trimmed, and is
//                         validated against the table before it is stored
//   erase confirm         forget every setting and nothing else
//   dump                  the last few kilobytes the port printed, whole lines only
//   journal               the last entries of the flash journal, in words
//
// Every `rak` command takes an optional slot - `rak @1 hello` - directly after the word `rak`
// (task p1-06; spec/SYSTEM.md S-11: "The Walter addresses radios by *slot*; there is one slot
// today"). Without one it is `@0`, and on a unit as built that is the only radio there is, so
// every line below behaves exactly as it did. `@1` parses and is then refused by the console,
// which is the one that knows how many radios the adapter has.
//
// and the link to the radio (task p1-02, spec/LINK-PROTOCOL.md):
//
//   rak hello             HELLO: the version, role, slot and capabilities the radio answers with
//   rak ping              PING, with the round trip
//   rak status            STATUS: uptime, why it booted, its clock, its flags and counters
//   rak id                IDENTITY: the node's public key and advert name
//   rak time              TIME_GET
//   rak time set <epoch>  TIME_SET from a person at this console (§5.4's source 3)
//   rak power             POWER_GET: the three channels
//   rak env               ENV_GET: temperature, humidity, pressure
//   rak cli <line>        CLI: one line run by the radio's own console interpreter
//   rak request <type> [hex data]
//                         LOCAL_REQUEST (§5.9, link 1.2): one request of the radio's own handler,
//                         answered as it answers an admin over the mesh and with nothing on the
//                         air. The type is two hex digits (01 status, 05 access list, 06
//                         neighbours, 07 owner info) and the data an even number of hex digits,
//                         at most 32 bytes
//   rak events            the events the client has queued
//   rak text              the lines a scanner has sent (§9)
//
// and the debug port (task p1-03) and the chip behind it (task p1-04):
//
//   rak swd connect       acquire the SWD lines, connect, print DPIDR and the power-up, release
//   rak swd id            connect, identify the nRF52840 (FICR), its protection, its core's state, release
//   rak swd mem <addr> [words]
//                         read 1 to 32 words at a word-aligned address (hex 0x... or decimal), read-only
//
// and the flasher (task p1-05):
//
//   rak flash-test confirm
//                         a two-page pattern at the top of the application region, only if those
//                         pages are blank: written, verified, erased again; the radio restarted.
//                         It writes to a radio's flash outside the update machine, so it wants
//                         the word (task p3-03)
//
// and the radio's base (task p5-03; spec/SYSTEM.md S-5a, docs/provisioning.md):
//
//   rak provision check   no write: the radio's port, its DEVICEID or the `locked` form, what the
//                         variant table would do, the base in the store, the line to type next
//   rak provision <DEVICEID>|locked|locked:<key16> confirm
//                         S-5a's sequence: erase the radio through the CTRL-AP, write the base,
//                         read every word back, one pin reset. Without the word it says what it
//                         would do; the radio's grammar is the console's to check, not this
//                         parser's
//
// and the trust table (task p2-01):
//
//   trust                 the key ids this build accepts a release signed with, or none
//
// and the image store (task p2-02; docs/ARCHITECTURE.md §6). Here the slot comes *after* the
// image's role, because that is the shape the task gives the commands and because `image list`
// is about every slot there is:
//
//   image list            every slot's three images: role, board, version, size, hash, when
//   image verify <which> [@slot]
//                         read the image back out of flash and hold it to the hash in its record
//   image erase <candidate|scanner> [@slot]
//                         forget one; never the golden image
//   image bad             the hashes of candidates that failed a health check
//   image receive <which> [@slot]
//                         take manifest.json, its .sig and the image over a framed binary
//                         transfer on this port (tools/walterctl.py; core/image/receive.h)
//
// and the radio's update (task p2-03; docs/update.md). These are `rak` lines, so the slot goes
// in front as for every other:
//
//   rak update            flash the candidate, prove it, promote it - or roll back; each
//                         transition printed as it happens
//   rak update --first-install
//                         the same with no golden image to return to: a person's command only
//   rak update status     the journal's story of the last update, in words
//   rak role scanner      flash the stored scanner image; prove that a scanner is there
//   rak role repeater     flash the golden image back and prove it
//
// and the supervisor (task p2-05; docs/supervisor.md):
//
//   rak watch             the supervisor's view: healthy since when, the last incidents, today's
//                         counts against the caps, a stop in force
//   rak watch reset-caps  the day's counts to zero and a stop lifted (the bench)
//   rak ladder            climb the ladder now, each rung printed as it is tried
//
// and the Walter's own update (task p2-04; docs/update.md). Not `rak` lines: they are about this
// board's two application slots, and take no slot:
//
//   self status           the running slot and its state, the other slot's, the trial if one is
//                         running, the last self-update's story from the journal
//   self receive          take a release of this firmware over this port, into the other slot,
//                         through the six checks (tools/walterctl.py push --as self)
//   self switch [radio-down]
//                         make the received image the next to boot, tell the radio, restart;
//                         `radio-down` says the radio is known to be down, so the trial's second
//                         condition does not apply
//   self confirm          end a trial early: the running image is valid (the bench)
//   self reject           end a trial early: the running image is rejected and the board
//                         restarts into the old one (the bench)
//
// and the uplink (task p3-01; docs/ARCHITECTURE.md §9). Not `rak` lines either: a modem has no
// slot, and the word is `lte` because that is what a person at a bench calls it:
//
//   lte                   the state, the operator, the band, the signal, what the last attempt
//                         did and when the next one is allowed
//   lte up                bring the uplink up now, inside one wake's budget (minutes)
//   lte down              the modem's radio off
//   lte time              the network's time, and whether it is the network's
//   lte id                the IMEI, the ICCID and the IMSI - **a person asking**, and the only
//                         place this firmware prints them
//
// and the HTTPS client (task p3-02; docs/ARCHITECTURE.md §9). No slot either:
//
//   http get <url>        one GET over verified TLS: the status, the headers worth naming, the
//                         first bytes as hex, the size, the time. https only
//   fetch [base-url] [candidate|scanner]
//                         a release from <base-url>/manifest.json: the three steps, each check
//                         printed as it passes, the image into the candidate (default) or scanner
//                         slot. Without a base URL the `fetch.url` setting is used (task p3-03)
//   trust tls             the roots this firmware trusts - subject, fingerprint, expiry - and
//                         whether the modem's certificate slots hold them
//
// and the modem library's own trace (task p10-06; docs/bench.md §13). No slot: a modem has none:
//
//   trace                 whether the library's TX and RX lines are going to this port, and what
//                         the trace is for
//   trace modem on|off    switch them on, or off again. Not a setting: it ends at the next boot
//
// and the agent's uplink (task p4-03; docs/ARCHITECTURE.md §13). No slot: a server is not a radio:
//
//   agent url             the server the uplink is configured for, whether a token is set, and
//                         the X-Agent-Version header every call carries
//   agent id              who this exit node says it is: the repeater's key and version from the
//                         link, the radio block, the job types it advertises, the intent
//   agent heartbeat       the body it would post, to compare with the contract fixture by eye
//   agent obs [pull]      the observation ring; `pull` drains §7 into it first
//   agent probe           one GET /agent/v1/config now, with the token: the status and the
//                         config the server answered. The bench's first look at a server
//
// and sleep (task p3-04; docs/ARCHITECTURE.md §10, docs/power.md). No slot either: a sleep is
// the whole unit's:
//
//   wake                  why this Walter is awake, the last ten wakes with their reasons and
//                         durations, the spurious count, whether the radio's line is armed
//   power                 the battery's band, the readings behind it, what the wake may do and
//                         how long until the next one
//   sleep now             go to sleep for the interval the battery chose. It does not come back
//   sleep <seconds>       the same, for a number a person picks (1 to kMaxSleepSeconds)
//   stay [off]            keep the Walter awake without a cable, for the bench, bounded
//
// and one whole wake (task p3-05; docs/cycle.md). No slot either: a wake is the whole unit's:
//
//   cycle now             run one pass of the ten steps, each printed as it passes, and sleep
//   cycle dry             what a pass would do, doing none of it
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/console/commands.h"
#include "core/link/codec/wire.h"
#include "core/settings/keys.h"

namespace core {
namespace console {

enum class Verb : std::uint8_t {
  kEmpty,  // a blank line: nothing to do, no error
  kHelp,
  kStatus,
  kReboots,
  kPins,
  kRakReset,
  kRakKill,
  kReboot,
  // The link (task p1-02).
  kRakHello,
  kRakPing,
  kRakStatus,
  kRakId,
  kRakTime,
  kRakTimeSet,
  kRakPower,
  kRakEnv,
  kRakCli,
  // §5.9's LOCAL_REQUEST (link 1.2, task p9-06): `rak request <type> [hex data]` - the type in
  // `req_type`, the data in `bytes`/`bytes_size`.
  kRakRequest,
  kRakEvents,
  kRakText,
  // The debug port (task p1-03) and the nRF52 behind it (task p1-04).
  kRakSwdConnect,
  kRakSwdId,
  kRakSwdMem,
  // The flasher (task p1-05).
  kRakFlashTest,
  // The radio's base (task p5-03, S-5a): `rak provision check` and `rak provision <radio>
  // [confirm]` - the radio in `text`, the word in `on`.
  kRakProvisionCheck,
  kRakProvision,
  // The trust table (task p2-01).
  kTrust,
  // The image store (task p2-02).
  kImageList,
  kImageVerify,
  kImageErase,
  kImageBad,
  kImageReceive,
  // The radio's update (task p2-03).
  kRakUpdate,
  kRakUpdateFirstInstall,
  kRakUpdateStatus,
  kRakRoleScanner,
  kRakRoleRepeater,
  // The supervisor (task p2-05).
  kRakWatch,
  kRakWatchResetCaps,
  kRakLadder,
  // The mesh client (task p4-02): `rak mesh status <key>`, the key in `text`.
  kRakMeshStatus,
  // The agent's uplink (task p4-03): the configured server and the version header; one
  // GET /agent/v1/config now. Not `rak` lines: a server is not a radio and takes no slot.
  kAgentUrl,
  kAgentProbe,
  // The agent's identity (task p4-04): who this exit node says it is, the heartbeat body it
  // would post, and the observation ring - with `agent obs pull` the one §7 drain a person may
  // ask for by hand.
  kAgentId,
  kAgentHeartbeat,
  kAgentObs,
  kAgentObsPull,
  // The agent's loop (task p4-05): one wake of it now, the last ten jobs as the library
  // remembers them, and the state a sleep keeps.
  kAgentWake,
  kAgentJobs,
  kAgentState,
  // The Walter's own update (task p2-04).
  kSelfStatus,
  kSelfReceive,
  kSelfSwitch,
  kSelfConfirm,
  kSelfReject,
  // The uplink (task p3-01).
  kLte,
  kLteUp,
  kLteDown,
  kLteTime,
  kLteId,
  // The HTTPS client (task p3-02).
  kHttpGet,
  kFetch,
  kTrustTls,
  // The modem library's own TX/RX trace (task p10-06): `trace` reads, `trace modem on|off` acts
  // (the word in `on`).
  kTrace,
  kTraceModem,
  // Sleep and the battery (task p3-04).
  kWake,       // why this wake happened, and the last ten
  kPower,      // the band, the readings behind it, the next interval
  kSleepNow,   // sleep for the interval the battery chose
  kSleepFor,   // `sleep <seconds>`: a person's number
  kStay,       // `stay` / `stay off`: awake on purpose, for the bench
  // One whole wake (task p3-05).
  kCycleNow,   // run a pass now: the ten steps, then sleep
  kCycleDry,   // what a pass would do, doing none of it
  // The settings and the rest of the console (task p3-03).
  kGet,      // one key
  kGetAll,   // every key
  kSet,
  kErase,    // `erase confirm`: the settings, and only the settings
  kDump,
  kJournal,
};

// rak swd mem reads at most this many words: eight lines of four.
constexpr std::uint32_t kMaxMemWords = 32;

// `sleep <seconds>`: a week, which is core/settings/keys.h's own ceiling for `wake.interval`
// said in seconds. A person who wants longer wants a different unit.
constexpr std::uint32_t kMaxSleepSeconds = 7u * 24 * 60 * 60;

// §5.7: a CLI line is 1 to 160 bytes of UTF-8. The parser refuses a longer one here rather than
// letting the encoder refuse it later, so that the console can say which limit was passed.
constexpr std::size_t kMaxCliLine = link::kMaxCliLine;

// The longest line the console accepts, without its line ending (the agent firmware's figure).
constexpr std::size_t kMaxLineLength = 200;

struct Command {
  Verb verb = Verb::kEmpty;
  std::uint8_t slot = 0;                  // every `rak ...`: the optional `@N`, default 0
  std::uint32_t seconds = 0;              // kRakKill
  std::uint32_t epoch = 0;                // kRakTimeSet
  char text[kMaxCliLine + 1] = {};        // kRakCli: the rest of the line, verbatim
  std::size_t text_size = 0;
  // kRakRequest: §5.9's `req_type` and its `data`, 0 to 32 bytes, parsed from a hex word here so
  // that a typo is one line from the parser rather than a frame the radio refuses.
  std::uint8_t req_type = 0;
  std::uint8_t bytes[link::kMaxLocalRequestData] = {};
  std::size_t bytes_size = 0;
  std::uint32_t address = 0;              // kRakSwdMem: word-aligned
  std::uint32_t words = 1;                // kRakSwdMem: 1..kMaxMemWords
  // kImageVerify, kImageErase, kImageReceive: core::image::Which as a number (0 golden,
  // 1 candidate, 2 scanner). A number and not the type so that this header stays the small thing
  // it is; parser.cpp holds it to core/image/store.h's own names.
  std::uint8_t which = 0;
  bool radio_down = false;                // kSelfSwitch: the radio is known to be down
  bool on = false;                        // kStay: `stay` is true, `stay off` false
  // kHttpGet, kFetch: the URL, verbatim (the line's bound is the URL's bound here: a GitHub
  // redirect's signed URL is for the client to follow, not for a person to type). Empty for a
  // `fetch` with no base URL, which means the `fetch.url` setting (task p3-03).
  char url[kMaxLineLength + 1] = {};
  // kGet and kSet: the row of core/settings/keys.h's table. A pointer into that table, which is
  // static storage and outlives everything.
  const settings::KeyDef* key = nullptr;
};

// kSet's value is `text`, which kRakCli already sized: a setting's longest value and a CLI line
// are both 160 bytes, so one buffer does for both and Command does not grow.
static_assert(settings::kMaxValueLen <= kMaxCliLine, "a setting's value has to fit Command::text");

struct Parsed {
  bool ok = false;
  Command command;
  char error[96] = {};  // one line, when !ok, without the "error: " prefix
};

Parsed parseLine(const char* line);

}  // namespace console
}  // namespace core
