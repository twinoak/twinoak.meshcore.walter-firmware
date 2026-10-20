#include "core/console/commands.h"

namespace core {
namespace console {

const char* describe(Group group) {
  switch (group) {
    case Group::kUnit: return "the unit";
    case Group::kRadio: return "the radio";
    case Group::kImages: return "images, trust and this firmware's own slots";
    case Group::kUplink: return "the uplink";
    case Group::kDutyCycle: return "sleep and the battery";
    case Group::kJournal: return "the journal";
    case Group::kCount: break;
  }
  return "?";
}

const CommandDef kCommands[kCommandCount] = {
    // -- the unit ------------------------------------------------------------------------------
    {"help", "help", Group::kUnit, "the commands, one line each"},
    {"status", "status", Group::kUnit,
     "one screen: this firmware and what it is, why it booted, the radio and its clock, the "
     "battery as the radio last reported it, the images, the uplink's last attempt, the next wake"},
    {"get [key]", "get", Group::kUnit,
     "one setting, or every setting in the order of the table below; a secret prints set or "
     "unset and never its value, an unset key shows its fallback in brackets"},
    {"set <key> <value>", "set", Group::kUnit,
     "store a setting; the value is the rest of the line, trimmed, so a value with spaces is "
     "fine. It is validated before it is stored and takes effect as the table says"},
    {"erase confirm", "erase", Group::kUnit,
     "forget every setting and nothing else - the images, the journal, the bad list and the "
     "supervisor's counts stay. Without the word it refuses and says what the word is"},
    {"reboot", "reboot", Group::kUnit,
     "restart the Walter; the pins go to the safe state first and 'reboots' after it says it was "
     "asked for on the console"},
    {"reboots", "reboots", Group::kUnit,
     "why this board last restarted, and the four before that, with how long each run lasted"},
    {"dump", "dump", Group::kUnit,
     "the last few kilobytes this port printed, oldest first, whole lines only. Nothing the "
     "radio sent as text is in it, and nothing the mesh said (S-12)"},

    // -- the radio -----------------------------------------------------------------------------
    {"pins", "pins", Group::kRadio,
     "the seven radio-facing pins, one line each: net, D-sub pin, GPIO, mode, level, use"},
    {"rak reset", "rak reset", Group::kRadio,
     "one 100 ms reset pulse on nRESET, open-drain, a second after the last one"},
    {"rak kill <seconds> [confirm]", "rak kill", Group::kRadio,
     "the radio's rail off for that long, then back: 1 to 30 whole seconds, and over 5 it wants "
     "the word 'confirm' - a repeater off the air is a decision, not a typo"},
    {"rak hello", "rak hello", Group::kRadio,
     "say hello to the radio: its version, role, slot and capabilities"},
    {"rak ping", "rak ping", Group::kRadio, "ping it, with the round trip"},
    {"rak status", "rak status", Group::kRadio,
     "its uptime, why it booted, its clock, its flags and counters"},
    {"rak id", "rak id", Group::kRadio, "its public key and advert name"},
    {"rak time [set <epoch>]", "rak time", Group::kRadio,
     "read its clock; 'rak time set <epoch>' sets it from here (a person at a console is the "
     "link protocol's third source of time)"},
    {"rak power", "rak power", Group::kRadio, "the three current-sense channels"},
    {"rak env", "rak env", Group::kRadio, "temperature, humidity, pressure"},
    {"rak cli <line>", "rak cli", Group::kRadio,
     "run one line on the radio's own console and print what it answered"},
    {"rak events", "rak events", Group::kRadio, "the events the radio has sent"},
    {"rak text", "rak text", Group::kRadio,
     "the lines a scanner has sent instead of frames; they are printed and not logged (S-12)"},
    {"rak swd connect", "rak swd connect", Group::kRadio,
     "connect to the radio's debug port: DPIDR and the power-up, then the lines released"},
    {"rak swd id", "rak swd id", Group::kRadio,
     "identify the chip: part, variant, flash, protection, the core's state"},
    {"rak swd mem <addr> [words]", "rak swd mem", Group::kRadio,
     "read 1 to 32 words of the chip's memory, read-only; the address is hex 0x... or decimal "
     "and word-aligned"},
    {"rak flash-test confirm", "rak flash-test", Group::kRadio,
     "write, verify and erase a two-page pattern at the top of the radio's application region, "
     "only if those pages are blank; the radio restarts. It writes to the radio's flash outside "
     "the update machine, so it wants the word"},
    {"rak update", "rak update", Group::kRadio,
     "flash the candidate, prove it, make it golden - or roll back to the golden image and never "
     "try that candidate again (minutes; docs/update.md)"},
    {"rak update --first-install", "rak update --first-install", Group::kRadio,
     "the same with no golden image to return to: a person's command only"},
    {"rak update status", "rak update status", Group::kRadio,
     "the journal's story of the last update of the radio"},
    {"rak role scanner|repeater", "rak role", Group::kRadio,
     "flash the stored scanner image and prove a scanner is there; or the golden image back, "
     "proved as an update would be"},
    {"rak watch", "rak watch", Group::kRadio,
     "the supervisor's view: healthy since when, the last incidents, today's counts against the "
     "daily caps, a stop in force (docs/supervisor.md). It only reads"},
    {"rak watch reset-caps", "rak watch reset-caps", Group::kRadio,
     "empty the day's counts and lift a stop, journalled (the bench)"},
    {"rak ladder", "rak ladder", Group::kRadio,
     "climb the supervisor's ladder now - ask, reset, power-cycle, look at the chip, stop - each "
     "rung printed as it is tried (minutes: the waits are the real ones)"},

    // -- images, trust and this firmware's own slots --------------------------------------------
    {"image list", "image list", Group::kImages,
     "every slot's three images and its base: role, board, version, size, hash, when it was "
     "stored"},
    {"image verify <which>", "image verify", Group::kImages,
     "read one back out of flash and hold it to the SHA-256 in its record: what finds flash rot "
     "before an update does"},
    {"image erase <candidate|scanner|base>", "image erase", Group::kImages,
     "forget one; the golden image is never erased (S-5: a known-good image is always on hand)"},
    {"image bad", "image bad", Group::kImages,
     "the hashes of candidates that failed a health check and are never tried again"},
    {"image receive <which>", "image receive", Group::kImages,
     "hand this port to a framed transfer and take manifest.json, its .sig and the image - or, "
     "with `base`, the board's provision file (tools/walterctl.py push); the checks run here"},
    {"trust", "trust", Group::kImages,
     "the release keys this build accepts a manifest signed with, after a self-test of the "
     "signature arithmetic on this board"},
    {"trust tls", "trust tls", Group::kImages,
     "the TLS roots this build trusts - subject, fingerprint, expiry - and whether the modem's "
     "certificate slots hold them"},
    {"self status", "self status", Group::kImages,
     "this Walter's two application slots, the trial if one is running, and the last "
     "self-update's story from the journal"},
    {"self receive", "self receive", Group::kImages,
     "take a release of this firmware over this port into the other slot "
     "(tools/walterctl.py push --as self)"},
    {"self switch [radio-down]", "self switch", Group::kImages,
     "make the received image the next to boot, tell the radio, restart; it then has one boot to "
     "prove itself. 'radio-down' says the radio is known to be down"},
    {"self confirm", "self confirm", Group::kImages,
     "end the running image's trial early: valid (the bench)"},
    {"self reject", "self reject", Group::kImages,
     "end it early: rejected, on the bad list, restart into the old image (the bench)"},

    // -- the uplink ----------------------------------------------------------------------------
    {"lte", "lte", Group::kUplink,
     "the uplink: state, operator, band, signal, what the last attempt did and when the next one "
     "is allowed. It only reads - it does not start the modem"},
    {"lte up", "lte up", Group::kUplink,
     "bring the uplink up now, inside one wake's budget (minutes: an attach from cold is slow), "
     "whatever the backoff would have said"},
    {"lte down", "lte down", Group::kUplink, "the modem's radio off; the context goes with it"},
    {"lte time", "lte time", Group::kUplink,
     "the network's time, and whether it really is the network's"},
    {"lte id", "lte id", Group::kUplink,
     "the modem's IMEI and the SIM's ICCID and IMSI, for the label on the box: printed here, "
     "written nowhere else and in no other line"},
    {"http get <url>", "http get", Group::kUplink,
     "one GET over TLS whose chain is verified: the status, the headers worth naming, the first "
     "bytes as hex, the size, the time. https only; 'lte up' first"},
    {"fetch [base-url] [candidate|scanner]", "fetch", Group::kUplink,
     "a release from <base-url>/manifest.json into the candidate (default) or scanner image: the "
     "three steps, each of the six checks as it passes, the image in ranges (minutes). Without a "
     "base URL it uses the setting fetch.url"},

    // -- sleep and the battery (task p3-04) ----------------------------------------------------
    {"wake", "wake", Group::kDutyCycle,
     "why this Walter is awake, the last ten wakes with their reasons and how long each lasted, "
     "the spurious count and whether the radio's line is armed as a wake source"},
    {"power", "power", Group::kDutyCycle,
     "the battery's band, the readings behind it, what a wake in that band may do and how long "
     "until the next one. Every threshold is a placeholder until the bench (docs/power.md)"},
    {"sleep now", "sleep now", Group::kDutyCycle,
     "go to sleep now for the interval the battery chose: the journal, SLEEP_NOTICE to the "
     "radio, the modem down, the pins safe and held, deep sleep. It does not come back - the "
     "next boot is the next wake"},
    {"sleep <seconds>", "sleep", Group::kDutyCycle,
     "the same, for a number of seconds a person picks: 1 to 604800. What the band allows is "
     "unchanged; only the timer is"},
    {"cycle now", "cycle now", Group::kDutyCycle,
     "run one whole wake on request: the ten steps of docs/cycle.md in order, each printed as it "
     "passes, and then sleep. It does not come back - the next boot is the next wake"},
    {"cycle dry", "cycle dry", Group::kDutyCycle,
     "what a wake would do, doing none of it: every step with its bound, its preconditions as "
     "they stand now, and what the backend seam is behind it. Nothing is sent and nothing is "
     "written"},
    {"stay [off]", "stay", Group::kDutyCycle,
     "keep the Walter awake without a cable, for the bench, bounded by twelve hours; 'stay off' "
     "ends it. Without it, and without a USB host, a wake that does not reach sleep in five "
     "minutes is ended by a reset"},

    // -- the journal ---------------------------------------------------------------------------
    {"journal", "journal", Group::kJournal,
     "the last entries in the flash journal in words, newest first, whatever wrote them: the "
     "radio's updates, this firmware's own, the supervisor's ladder"},
};

static_assert(sizeof kCommands / sizeof kCommands[0] == kCommandCount,
              "kCommandCount is the table's length");

}  // namespace console
}  // namespace core
