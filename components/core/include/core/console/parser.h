// The console's line parser: one line in, one command or one refusal out (task p0-03). A pure
// function, after the agent firmware's src/console/Parser.h, so that every line shape and every
// refusal has a host test. What a command does is console.h.
//
// The grammar (one line per command, a `>` prompt; docs/console.md):
//
//   help                  the commands
//   status                version, uptime, why it booted, the pins and the radio's power
//   reboots               why this board last restarted, and the four before that
//   pins                  the seven pins: net, D-sub, GPIO, mode, level
//   rak reset             one reset pulse on nRESET (waits out the spacing)
//   rak kill <seconds>    the LoRa rail off for that long, then back
//   reboot                restart the Walter
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
//   rak flash-test        a two-page pattern at the top of the application region, only if those
//                         pages are blank: written, verified, erased again; the radio restarted
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
//   fetch <base-url> [candidate|scanner]
//                         a release from <base-url>/manifest.json: the three steps, each check
//                         printed as it passes, the image into the candidate (default) or scanner
//                         slot
//   trust tls             the roots this firmware trusts - subject, fingerprint, expiry - and
//                         whether the modem's certificate slots hold them
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/link/codec/wire.h"

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
  kRakEvents,
  kRakText,
  // The debug port (task p1-03) and the nRF52 behind it (task p1-04).
  kRakSwdConnect,
  kRakSwdId,
  kRakSwdMem,
  // The flasher (task p1-05).
  kRakFlashTest,
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
};

// rak swd mem reads at most this many words: eight lines of four.
constexpr std::uint32_t kMaxMemWords = 32;

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
  std::uint32_t address = 0;              // kRakSwdMem: word-aligned
  std::uint32_t words = 1;                // kRakSwdMem: 1..kMaxMemWords
  // kImageVerify, kImageErase, kImageReceive: core::image::Which as a number (0 golden,
  // 1 candidate, 2 scanner). A number and not the type so that this header stays the small thing
  // it is; parser.cpp holds it to core/image/store.h's own names.
  std::uint8_t which = 0;
  bool radio_down = false;                // kSelfSwitch: the radio is known to be down
  // kHttpGet, kFetch: the URL, verbatim (the line's bound is the URL's bound here: a GitHub
  // redirect's signed URL is for the client to follow, not for a person to type).
  char url[kMaxLineLength + 1] = {};
};

struct Parsed {
  bool ok = false;
  Command command;
  char error[96] = {};  // one line, when !ok, without the "error: " prefix
};

Parsed parseLine(const char* line);

}  // namespace console
}  // namespace core
